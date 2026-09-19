"""PORT-021 (KAP-558): value-level tests for the real-model planner
adapter. No world, no live model: prompt boundedness, strict schema
parsing, fallback determinism, wire validity under lab and production
tick cadences, and model-client failure mapping against local stubs.
"""
import http.server
import json
import os
import socket
import sys
import threading
import time
import unittest
import urllib.request

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "personality-service"))
import fake_planner as fp
import model_client as mc
import real_planner as rp


def _request(generation=7, flags=0b010, bots=((610002, 1), (610003, 8))):
    return fp.make_request({
        "protocol_version": fp.PROTOCOL_VERSION,
        "request_id": 0x1122334455667788,
        "owner_guid": 610001,
        "observation_version": fp.OBSERVATION_VERSION,
        "capture_time_ms": 1000000,
        "step_count": 0,
        "total_size": fp.REQUEST_BYTES,
        "generation": generation,
        "flags": flags,
        "bots": list(bots),
    })


class PrimerTests(unittest.TestCase):
    def test_versioned_bounded_and_pinned(self):
        text, digest = rp.load_primer()
        raw = text.encode("utf-8")
        self.assertLessEqual(len(raw), rp.PRIMER_MAX_BYTES)
        self.assertEqual(digest, rp.PRIMER_SHA256)
        first = text.splitlines()[0]
        self.assertIn("primer", first.lower())
        self.assertIn("version 1", first)
        # No live data: no 6+ digit ids, no urls, no coordinates.
        import re
        self.assertIsNone(re.search(r"\d{6,}", text))
        self.assertNotIn("http", text.lower())


class PromptTests(unittest.TestCase):
    def test_bounded_and_non_sensitive(self):
        primer, _ = rp.load_primer()
        req = _request()
        msgs = rp.build_messages(req, primer)
        self.assertEqual([m["role"] for m in msgs], ["system", "user"])
        user = msgs[1]["content"]
        self.assertIn("version 1", user)
        self.assertIn("Warrior", user)   # cls 1
        self.assertIn("Mage", user)      # cls 8
        self.assertIn("order_generation: 7", user)
        self.assertIn("held=0 following=1 owner_available=0", user)
        # Raw GUIDs and request ids never enter the prompt.
        for secret in ("610001", "610002", "610003", "1122334455667788"):
            self.assertNotIn(secret, user)
        self.assertLess(len(msgs[0]["content"] + user), 8000)
        # Closed vocabulary is declared to the model.
        for action in ("none", "hold", "follow", "defend", "regroup",
                       "preference"):
            self.assertIn(action, msgs[0]["content"])
        for forbidden in ("assist", "loot"):
            self.assertNotIn("'%s'" % forbidden, msgs[0]["content"])
            self.assertNotIn('\\"%s\\"' % forbidden, msgs[0]["content"])

    def test_held_flag_renders(self):
        primer, _ = rp.load_primer()
        user = rp.build_messages(_request(flags=0b111), primer)[1]["content"]
        self.assertIn("held=1 following=1 owner_available=1", user)


class ParseTests(unittest.TestCase):
    def _doc(self, **over):
        doc = {"steps": [{"bot": 1, "action": "hold"}]}
        doc.update(over)
        return json.dumps(doc)

    def test_valid_actions_and_bounds(self):
        steps = rp.parse_model_text(self._doc(steps=[
            {"bot": 1, "action": "none"},
            {"bot": 2, "action": "preference",
             "preference": {"id": "follow_chase", "value": 2}},
        ]), 2)
        self.assertEqual(steps, [
            {"bot": 1, "action": fp.ACTION_NONE, "preference": 0},
            {"bot": 2, "action": fp.ACTION_PREFERENCE,
             "preference": (1 << 8) | 2},
        ])
        steps = rp.parse_model_text(self._doc(steps=[
            {"bot": 1, "action": "preference",
             "preference": {"id": "expression", "value": 0}}]), 1)
        self.assertEqual(steps[0]["preference"], (2 << 8) | 0)

    def test_rejects_violations(self):
        cases = {
            "unknown action": [{"bot": 1, "action": "dance"}],
            "assist not allowed": [{"bot": 1, "action": "assist"}],
            "loot not allowed": [{"bot": 1, "action": "loot"}],
            "pref on non-pref": [
                {"bot": 1, "action": "hold",
                 "preference": {"id": "expression", "value": 0}}],
            "unknown pref id": [
                {"bot": 1, "action": "preference",
                 "preference": {"id": "aggro", "value": 0}}],
            "chase value high": [
                {"bot": 1, "action": "preference",
                 "preference": {"id": "follow_chase", "value": 3}}],
            "expr value high": [
                {"bot": 1, "action": "preference",
                 "preference": {"id": "expression", "value": 2}}],
            "bool value": [
                {"bot": 1, "action": "preference",
                 "preference": {"id": "expression", "value": True}}],
            "bot zero": [{"bot": 0, "action": "hold"}],
            "bot above count": [{"bot": 3, "action": "hold"}],
            "duplicate bot": [{"bot": 1, "action": "hold"},
                              {"bot": 1, "action": "none"}],
            "extra top key": None,
            "extra step key": None,
            "pref extra key": None,
            "non-string action": [{"bot": 1, "action": 1}],
            "null action": [{"bot": 1}],
        }
        for name, steps in cases.items():
            with self.subTest(name=name):
                if name == "extra top key":
                    text = json.dumps({"steps": [{"bot": 1, "action": "hold"}],
                                       "extra": 1})
                elif name == "extra step key":
                    text = json.dumps({"steps": [
                        {"bot": 1, "action": "hold", "target": 42}]})
                elif name == "pref extra key":
                    text = json.dumps({"steps": [
                        {"bot": 1, "action": "preference",
                         "preference": {"id": "expression", "value": 0,
                                        "note": "hi"}}]})
                else:
                    text = self._doc(steps=steps)
                self.assertIsNone(rp.parse_model_text(text, 2), name)

    def test_rejects_empty_and_bad_json(self):
        self.assertIsNone(rp.parse_model_text("", 2))
        self.assertIsNone(rp.parse_model_text("no json here", 2))
        self.assertIsNone(rp.parse_model_text(self._doc(steps=[]), 2))
        self.assertIsNone(rp.parse_model_text("[1,2]", 2))
        self.assertIsNone(rp.parse_model_text('{"steps": null}', 2))

    def test_tolerates_surrounding_prose_and_fences(self):
        text = ('Sure! Here is the plan:\n```json\n'
                '{"steps": [{"bot": 1, "action": "hold"}]}\n```\nDone.')
        steps = rp.parse_model_text(text, 1)
        self.assertEqual(steps, [{"bot": 1, "action": fp.ACTION_HOLD,
                                  "preference": 0}])


class FallbackAndWireTests(unittest.TestCase):
    def test_fallback_is_hold_for_every_bot(self):
        req = _request()
        body = fp.decode_request_body(req[fp.ENVELOPE:])
        steps = rp.fallback_steps(body)
        self.assertEqual(len(steps), 2)
        for s in steps:
            self.assertEqual(s["action"], fp.ACTION_HOLD)
            self.assertEqual(s["preference"], 0)

    def test_wire_valid_lab_tick(self):
        req = _request()
        body = fp.decode_request_body(req[fp.ENVELOPE:])
        steps = rp.parse_model_text(
            '{"steps":[{"bot":1,"action":"preference",'
            '"preference":{"id":"follow_chase","value":1}},'
            '{"bot":2,"action":"hold"}]}', 2)
        resp = rp.build_wire_response(req, steps, round_ms=0, tick_ms=1000)
        cap = fp.decode_envelope(req)["capture_time_ms"]
        # Fetch on the next 1 s lab tick after a fast round.
        self.assertEqual(fp.validate_response(req, resp, cap + 1000),
                         fp.REJECT_OK)

    def test_wire_valid_prod_tick_slow_round(self):
        req = _request()
        steps = rp.fallback_steps(
            fp.decode_request_body(req[fp.ENVELOPE:]))
        resp = rp.build_wire_response(req, steps, round_ms=2500,
                                      tick_ms=10000)
        cap = fp.decode_envelope(req)["capture_time_ms"]
        env = fp.decode_envelope(resp)
        self.assertLessEqual(env["capture_time_ms"] - cap, rp.CAPTURE_MAX_MS)
        # Fetch on the next 10 s production tick (after the PORT-021
        # consume-before-submit order): still inside the age budget.
        self.assertEqual(fp.validate_response(req, resp, cap + 10000),
                         fp.REJECT_OK)
        # And the step lifetime stays within the 5000 ms protocol bound.
        step = fp.decode_step(resp, fp.ENVELOPE)
        self.assertLessEqual(step["expires_at_ms"] - env["capture_time_ms"],
                             fp.MAX_STEP_LIFETIME_MS)

    def test_capture_offset_bounds(self):
        self.assertEqual(rp.capture_offset_ms(0, 1000), 3000)
        self.assertEqual(rp.capture_offset_ms(2500, 10000), 12500)
        self.assertEqual(rp.capture_offset_ms(60000, 10000), rp.CAPTURE_MAX_MS)


class _StubBase(http.server.BaseHTTPRequestHandler):
    kind = "json"
    delay_ms = 0

    def log_message(self, *a):
        pass

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        if length:
            self.rfile.read(length)
        if self.delay_ms:
            time.sleep(self.delay_ms / 1000.0)
        if self.kind == "json":
            payload = json.dumps({"choices": [{
                "message": {"content": '{"steps":[{"bot":1,"action":"hold"}]}'}}
            ]}).encode()
            self.send_response(200)
        elif self.kind == "garbage":
            payload = b"definitely not json"
            self.send_response(200)
        else:
            payload = b""
            self.send_response(500)
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)


def _stub_port(handler_cls):
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler_cls)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, srv.server_address[1]


class ModelClientTests(unittest.TestCase):
    def test_dead_port_is_connect_fail(self):
        with self.assertRaises(mc.ModelError) as ctx:
            mc.chat("http://127.0.0.1:1/v1", "k", "m",
                    [{"role": "user", "content": "hi"}], timeout_s=2.0)
        self.assertEqual(ctx.exception.reason, "connect-fail")

    def test_missing_key_file(self):
        with self.assertRaises(mc.ModelError) as ctx:
            mc.read_api_key(r"C:\nonexistent-dir-xyz\key.txt")
        self.assertEqual(ctx.exception.reason, "no-key-file")

    def test_timeout_and_bad_json_and_http_error(self):
        import types

        class Slow(_StubBase):
            kind = "json"
            delay_ms = 3000

        srv, port = _stub_port(Slow)
        try:
            with self.assertRaises(mc.ModelError) as ctx:
                mc.chat("http://127.0.0.1:%d/v1" % port, "k", "m",
                        [{"role": "user", "content": "hi"}], timeout_s=1.0)
            self.assertEqual(ctx.exception.reason, "timeout")
        finally:
            srv.shutdown()

        class Garbage(_StubBase):
            kind = "garbage"

        srv, port = _stub_port(Garbage)
        try:
            with self.assertRaises(mc.ModelError) as ctx:
                mc.chat("http://127.0.0.1:%d/v1" % port, "k", "m",
                        [{"role": "user", "content": "hi"}], timeout_s=2.0)
            self.assertEqual(ctx.exception.reason, "bad-json")
        finally:
            srv.shutdown()

        class Err500(_StubBase):
            kind = "error"

        srv, port = _stub_port(Err500)
        try:
            with self.assertRaises(mc.ModelError) as ctx:
                mc.chat("http://127.0.0.1:%d/v1" % port, "k", "m",
                        [{"role": "user", "content": "hi"}], timeout_s=2.0)
            self.assertEqual(ctx.exception.reason, "http-error")
        finally:
            srv.shutdown()


if __name__ == "__main__":
    unittest.main()
