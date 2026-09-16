"""PORT-018 (KAP-558): live fake planner service for the transport fixture.

A stdlib-only HTTP server that speaks the planner protocol v1 on the wire.
It reuses fake_planner.py (same directory) as the reference encoder so the
byte layout cannot drift. Unlike the value-level fake (no I/O), this one
answers real sockets so the C++ transport's connect, deadline, oversize and
fail-closed paths run for real.

Scenarios (switched at runtime via POST /scenario):
  success     echo the request, one Hold step per bot, future capture
  delay       success plus a 400 ms transport delay
  timeout     sleeps past the transport's 5000 ms round deadline
  malformed   response truncated (4 bytes dropped)
  oversized   response padded beyond the 4096 byte ceiling
  unsupported protocol_version + 1
  stale       capture 2000 ms in the past (1000 ms age budget)
  chase       PORT-019 Preference step: FollowChase index 1 (medium)
  express     PORT-019 Preference step: Expression slot 0

Every request and scenario change is logged to stdout; the fixture owns
this process and reads its log as evidence.
"""
import os
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fake_planner as fp

SCENARIO_LOCK = threading.Lock()
SCENARIO = {"name": "success"}


def set_scenario(name):
    if name not in fp.SCENARIOS:
        return False
    with SCENARIO_LOCK:
        SCENARIO["name"] = name
    print("[fake-planner] scenario -> %s" % name, flush=True)
    return True


def get_scenario():
    with SCENARIO_LOCK:
        return SCENARIO["name"]


def build_live_response(request, scenario):
    """(payload, delay_ms, status). payload None = send nothing usable."""
    env = fp.decode_envelope(request)
    body = fp.decode_request_body(request[fp.ENVELOPE:])
    bots = [(b["bot_guid"], b["cls"]) for b in body["bots"][:body["bot_count"]]]
    if scenario == "stale":
        # The service claims to have planned a capture 2000 ms in the past:
        # past the 1000 ms response age budget at any later fetch.
        capture = max(1, env["capture_time_ms"] - 2000)
        steps = [
            {"bot_guid": g, "login_generation": 1,
             "order_generation": body["generation"],
             "action": fp.ACTION_HOLD, "target_guid": 0, "preference": 0}
            for (g, _c) in bots]
        payload = fp.build_response(request, steps, capture_offset_ms=0)
        # build_response offsets from the request capture; shift the
        # envelope to the claimed past capture explicitly.
        import struct
        payload = bytearray(payload)
        fp_encode = fp.encode_envelope(
            fp.MAGIC, fp.PROTOCOL_VERSION, env["request_id"], env["owner_guid"],
            env["observation_version"], capture, len(steps),
            fp.ENVELOPE + fp.STEP * len(steps))
        payload[:fp.ENVELOPE] = fp_encode
        return bytes(payload), 0, 200
    if scenario == "unsupported":
        steps = [
            {"bot_guid": g, "login_generation": 1,
             "order_generation": body["generation"],
             "action": fp.ACTION_HOLD, "target_guid": 0, "preference": 0}
            for (g, _c) in bots]
        payload = fp.build_response(request, steps, capture_offset_ms=2000)
        import struct
        payload = bytearray(payload)
        env2 = fp.decode_envelope(bytes(payload))
        payload[:fp.ENVELOPE] = fp.encode_envelope(
            fp.MAGIC, fp.PROTOCOL_VERSION + 1, env2["request_id"],
            env2["owner_guid"], env2["observation_version"],
            env2["capture_time_ms"], env2["step_count"], env2["total_size"])
        return bytes(payload), 0, 200
    if scenario in ("chase", "express"):
        # PORT-019: one Preference step per bot. The packed field
        # is (id << 8) | value; the world maps it per profile.
        packed = (1 << 8) | 1 if scenario == "chase" else (2 << 8) | 0
        payload = fp.build_response(
            request, [
                {"bot_guid": g, "login_generation": 1,
                 "order_generation": body["generation"],
                 "action": fp.ACTION_PREFERENCE, "target_guid": 0,
                 "preference": packed}
                for (g, _c) in bots],
            capture_offset_ms=2000)
        return payload, 0, 200
    payload = fp.build_response(
        request, [
            {"bot_guid": g, "login_generation": 1,
             "order_generation": body["generation"],
             "action": fp.ACTION_HOLD, "target_guid": 0, "preference": 0}
            for (g, _c) in bots],
        capture_offset_ms=2000)
    if scenario == "success":
        return payload, 0, 200
    if scenario == "delay":
        return payload, 400, 200
    if scenario == "timeout":
        return payload, 6000, 200
    if scenario == "malformed":
        return bytes(payload[:-4]), 0, 200
    if scenario == "oversized":
        return payload + b"\x00" * (fp.MAX_PAYLOAD_BYTES + 128), 0, 200
    raise ValueError("unknown scenario: %s" % scenario)


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        print("[fake-planner] %s " % self.address_string() + fmt % args, flush=True)

    def _send(self, status, payload):
        self.send_response(status)
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        try:
            self.wfile.write(payload)
        except (BrokenPipeError, ConnectionResetError):
            # The transport hit its deadline and closed: expected for the
            # timeout scenario.
            pass

    def do_GET(self):
        if self.path == "/scenario":
            self._send(200, get_scenario().encode("ascii"))
        else:
            self._send(404, b"")

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(length) if length else b""
        if self.path == "/scenario":
            name = raw.decode("ascii", "replace").strip()
            if set_scenario(name):
                self._send(200, b"ok")
            else:
                self._send(400, b"unknown scenario")
            return
        if self.path != "/plan":
            self._send(404, b"")
            return
        env = fp.decode_envelope(raw)
        body = fp.decode_request_body(raw[fp.ENVELOPE:])
        print("[fake-planner] POST /plan len:%d scenario:%s reqid:%x owner:%u "
              "bots:%u gen:%u" % (len(raw), get_scenario(), env["request_id"],
                                  env["owner_guid"], body["bot_count"],
                                  body["generation"]), flush=True)
        if len(raw) != fp.REQUEST_BYTES:
            self._send(400, b"bad request size")
            return
        try:
            payload, delay_ms, status = build_live_response(raw, get_scenario())
        except Exception as exc:  # fail closed, keep the server alive
            print("[fake-planner] build failed: %s" % exc, flush=True)
            self._send(500, b"")
            return
        if delay_ms:
            time.sleep(delay_ms / 1000.0)
        self._send(status, payload)


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 18311
    server = ThreadingHTTPServer(("0.0.0.0", port), Handler)
    server.daemon_threads = True
    print("[fake-planner] listening port:%d" % port, flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
