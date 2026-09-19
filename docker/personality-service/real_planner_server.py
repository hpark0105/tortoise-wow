"""PORT-021 (KAP-558): host-side real-model planner service.

Same wire contract as fake_planner_server.py (POST /plan; plus
GET /health and GET /primer for readiness and payload inspection).
Every round: decode the request, build the bounded prompt, call the
local model ONCE (hard timeout, single flight), parse the JSON
strictly, and answer with protocol bytes. Provider failure (offline,
busy, timeout, malformed) answers the deterministic Hold fallback, so
the world never waits and always receives a valid, non-empty
response that passes the same protocol checks as the fake service.

Environment:
  REAL_PLANNER_PORT        default 18341
  REAL_PLANNER_MODEL_URL   default http://127.0.0.1:8090/v1
  REAL_PLANNER_KEY_FILE    default park-llama key file
  REAL_PLANNER_TIMEOUT_MS  default 4000 (inside the 5000 ms transport
                           round deadline; the fallback leaves room)
  REAL_PLANNER_TICK_MS     default 10000 (world tick for the capture
                           policy; lab fixtures set 1000)
  REAL_PLANNER_MAX_TOKENS  default 64
"""
import json
import os
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fake_planner as fp
import model_client as mc
import real_planner as rp
import converse as cv

def _chat_template_kwargs():
    """Provider-specific template options. The default disables Qwen3
    thinking mode (its reasoning otherwise consumes the whole max-token
    budget and content stays empty). Unknown kwargs are ignored by
    other models' templates."""
    raw = os.environ.get(
        "REAL_PLANNER_CHAT_TEMPLATE_KWARGS",
        '{"enable_thinking": false}')
    try:
        doc = json.loads(raw)
        if not isinstance(doc, dict):
            raise ValueError
        return doc
    except ValueError:
        return {}


CFG = {
    "port": int(os.environ.get("REAL_PLANNER_PORT", "18341")),
    "model_url": os.environ.get("REAL_PLANNER_MODEL_URL", mc.DEFAULT_BASE_URL),
    "key_file": os.environ.get("REAL_PLANNER_KEY_FILE", ""),
    "timeout_ms": int(os.environ.get("REAL_PLANNER_TIMEOUT_MS", "4000")),
    # PORT-031 (KAP-558): per-path model budgets. The world's converse
    # round has a hard 4000 ms I/O deadline, so converse worst case
    # (lock wait + model call) must stay under it. The plan path only
    # enriches (Hold is the deterministic fallback) and is rate-gated to
    # one model call per tick window so it stops pinning the shared
    # model and starving /converse.
    "plan_timeout_ms": int(os.environ.get("REAL_PLANNER_PLAN_TIMEOUT_MS", "1500")),
    "converse_lock_wait_ms": int(
        os.environ.get("REAL_PLANNER_CONVERSE_LOCK_WAIT_MS", "1000")),
    "converse_timeout_ms": int(os.environ.get(
        "REAL_PLANNER_CONVERSE_TIMEOUT_MS", "2500")),
    "tick_ms": int(os.environ.get("REAL_PLANNER_TICK_MS", "10000")),
    "max_tokens": int(os.environ.get("REAL_PLANNER_MAX_TOKENS", "64")),
    "chat_template_kwargs": _chat_template_kwargs(),
}

# Single flight: one model request at a time (shared-model policy); a
# concurrent round gets the Hold fallback instead of a queue.
MODEL_LOCK = threading.Lock()

# PORT-031 (KAP-558): plan model-call rate gate (monotonic seconds of the
# last started plan model call); gated rounds answer Hold immediately.
LAST_PLAN_CALL = {"t": 0.0}
MODEL_ID = {"id": None}
PRIMER = None  # (text, sha256) loaded at startup


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        print("[real-planner] %s " % self.address_string() + fmt % args,
              flush=True)

    def _send(self, status, payload):
        self.send_response(status)
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        try:
            self.wfile.write(payload)
        except (BrokenPipeError, ConnectionResetError):
            pass  # transport deadline passed: expected on slow rounds

    def do_GET(self):
        if self.path == "/health":
            self._send(200, b"ok")
        elif self.path == "/primer":
            text, _digest = PRIMER
            self._send(200, text.encode("utf-8"))
        else:
            self._send(404, b"")

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(length) if length else b""
        if self.path.startswith("/converse"):
            self._handle_converse(raw)
            return
        if self.path != "/plan":
            self._send(404, b"")
            return
        if len(raw) != fp.REQUEST_BYTES:
            print("[real-planner] plan bad size %d" % len(raw), flush=True)
            self._send(400, b"bad request size")
            return
        env = fp.decode_envelope(raw)
        body = fp.decode_request_body(raw[fp.ENVELOPE:])
        print("[real-planner] plan reqid:%x owner:%u bots:%u gen:%u" % (
            env["request_id"], env["owner_guid"], body["bot_count"],
            body["generation"]), flush=True)
        t0 = time.monotonic()
        steps = None
        reason = None
        meta = None
        if time.monotonic() - LAST_PLAN_CALL["t"] < CFG["tick_ms"] / 1000.0:
            # PORT-031: rate-gated - the shared model answers the
            # player-facing converse rounds; plan enrichment re-runs in
            # the next window. Hold is the deterministic fallback.
            gate_ms = int((time.monotonic() - t0) * 1000)
            payload = rp.build_wire_response(
                raw, rp.fallback_steps(body), gate_ms, CFG["tick_ms"])
            print("[real-planner] plan gated reqid:%x ms:%d" % (
                env["request_id"], gate_ms), flush=True)
            self._send(200, payload)
            return
        if not MODEL_LOCK.acquire(blocking=False):
            reason = "busy"
        else:
            try:
                LAST_PLAN_CALL["t"] = time.monotonic()
                key = mc.read_api_key(CFG["key_file"] or None)
                if MODEL_ID["id"] is None:
                    MODEL_ID["id"] = mc.fetch_model_id(CFG["model_url"], key)
                text, meta = mc.chat(
                    CFG["model_url"], key, MODEL_ID["id"],
                    rp.build_messages(raw, PRIMER[0]),
                    CFG["plan_timeout_ms"] / 1000.0,
                    max_tokens=CFG["max_tokens"],
                    extra={"chat_template_kwargs": CFG["chat_template_kwargs"]}
                    if CFG["chat_template_kwargs"] else None)
                steps = rp.parse_model_text(text, body["bot_count"])
                if steps is None:
                    reason = "bad-schema"
            except mc.ModelError as e:
                reason = e.reason
                if reason == "no-key-file" or reason == "no-model":
                    MODEL_ID["id"] = None  # re-probe next round
            finally:
                MODEL_LOCK.release()
        round_ms = int((time.monotonic() - t0) * 1000)
        if steps is None:
            steps = rp.fallback_steps(body)
            print("[real-planner] fallback reqid:%x reason:%s ms:%d" % (
                env["request_id"], reason, round_ms), flush=True)
        else:
            print("[real-planner] model ok reqid:%x ms:%d tokens:%d/%d "
                  "steps:%u" % (env["request_id"], meta["latency_ms"],
                                meta["prompt_tokens"],
                                meta["completion_tokens"], len(steps)),
                  flush=True)
        try:
            payload = rp.build_wire_response(raw, steps, round_ms,
                                             CFG["tick_ms"])
        except Exception as exc:
            print("[real-planner] encode failed reqid:%x: %s" % (
                env["request_id"], exc), flush=True)
            # Last-resort deterministic bytes, never an empty reply.
            payload = fp.build_response(
                raw, [
                    {"bot_guid": b["bot_guid"], "login_generation": 1,
                     "order_generation": body["generation"],
                     "action": fp.ACTION_HOLD, "target_guid": 0,
                     "preference": 0}
                    for b in body["bots"][:body["bot_count"]]])
        self._send(200, payload)

    def _handle_converse(self, raw):
        # PORT-022 (KAP-558): one bounded, personality-consistent, text-only
        # reply. The profile is a safe enum string from the URL; the body is
        # the world-sanitized message text. Strict model schema
        # {"reply": "..."}; every failure is no reply (HTTP 500, empty).
        profile = "none"
        if "?profile=" in self.path:
            prof = self.path.split("?profile=", 1)[1].split("&", 1)[0].strip()
            if prof in ("none", "reckless", "cautious"):
                profile = prof
        try:
            text = raw.decode("utf-8", "replace")
        except Exception:
            text = ""
        text = cv.sanitize_reply(text, cv.MAX_TEXT)
        t0 = time.monotonic()
        reason = None
        reply = None
        meta = None
        # PORT-031: converse is the player-facing path - wait briefly for
        # a plan call in flight (plan calls are capped at
        # plan_timeout_ms), then take the model. Worst case stays under
        # the world's 4000 ms round deadline.
        if not MODEL_LOCK.acquire(
                blocking=True, timeout=CFG["converse_lock_wait_ms"] / 1000.0):
            reason = "busy"
        else:
            try:
                key = mc.read_api_key(CFG["key_file"] or None)
                if MODEL_ID["id"] is None:
                    MODEL_ID["id"] = mc.fetch_model_id(CFG["model_url"], key)
                text2, meta = mc.chat(
                    CFG["model_url"], key, MODEL_ID["id"],
                    cv.build_converse_messages(profile, text),
                    CFG["converse_timeout_ms"] / 1000.0,
                    max_tokens=CFG["max_tokens"],
                    extra={"chat_template_kwargs": CFG["chat_template_kwargs"]}
                    if CFG["chat_template_kwargs"] else None)
                reply = cv.parse_converse_text(text2)
                if not reply:
                    reason = "bad-schema"
            except mc.ModelError as e:
                reason = e.reason
                if reason == "no-key-file" or reason == "no-model":
                    MODEL_ID["id"] = None  # re-probe next round
            finally:
                MODEL_LOCK.release()
        round_ms = int((time.monotonic() - t0) * 1000)
        if reply:
            print("[real-planner] converse profile:%s model ok ms:%d "
                  "tokens:%d/%d len:%d" % (profile, meta["latency_ms"],
                                           meta["prompt_tokens"],
                                           meta["completion_tokens"],
                                           len(reply)), flush=True)
            self._send(200, reply.encode("utf-8"))
        else:
            print("[real-planner] converse profile:%s fallback reason:%s "
                  "ms:%d" % (profile, reason, round_ms), flush=True)
            self._send(500, b"")


def main():
    global PRIMER
    PRIMER = rp.load_primer()
    server = ThreadingHTTPServer(("0.0.0.0", CFG["port"]), Handler)
    server.daemon_threads = True
    print("[real-planner] listening port:%d model_url:%s primer:%s "
          "sha256:%s timeout_ms:%d tick_ms:%d plan_timeout_ms:%d "
          "converse_lock_wait_ms:%d converse_timeout_ms:%d" % (
              CFG["port"], CFG["model_url"], rp.PRIMER_NAME, PRIMER[1],
              CFG["timeout_ms"], CFG["tick_ms"], CFG["plan_timeout_ms"],
              CFG["converse_lock_wait_ms"], CFG["converse_timeout_ms"]),
          flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
