"""PORT-021 (KAP-558): real-adapter runtime proof.

One disposable Compose project, one world generation, one host-side
real_planner_server, one in-process stub model endpoint. Phases:

A offline   model url -> dead port: Hold fallback flows end to end
            (world offer action:1; adapter reason:connect-fail)
B timeout   model url -> 6 s stub: adapter 1.5 s budget passes first
            (reason:timeout; world still gets a valid Hold)
C schema    stub answers the strict JSON contract: the preference is
            applied (reckless chase dist:20.0) through the same
            identity/generation/age/allowlist checks as the fake
D malformed stub answers prose: reason:bad-schema, Hold, no effect
E busy      two concurrent /plan calls: one is served, one is
            reason:busy (single flight)
F live      only when PORT021_MODEL_URL is set and reachable: one
            real model round; the measured response is recorded

Every phase restarts the adapter with a different model url; each
phase produces at least one HTTP-200 round so the transport timeout
counter resets and no 60 s cooldown can fire between phases.
"""
import json
import os
import socket
import subprocess
import sys
import threading
import time
import unittest
import urllib.request
import uuid
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import test_bot_provision as p

sys.path.insert(0, os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "personality-service"))

ROOT = os.path.dirname(os.path.abspath(__file__))
SERVER = os.path.join(ROOT, "personality-service", "real_planner_server.py")

OWNER_GUID = 500150
COMP_GUID = 500151
OWNER_ACC = 1000500150
COMP_ACC = 1000500151
PERSONAL_CONTAINERS = ("tortoise-local-db-1", "tortoise-local-realmd-1",
                       "tortoise-local-world-1")
STUB_VALID_CONTENT = (
    '{"steps": [{"bot": 1, "action": "preference", '
    '"preference": {"id": "follow_chase", "value": 1}}]}')

COMP_NAME = "Rplcomp"

SCRIPT = ";".join([
    "5000:%d:botfollow %s" % (OWNER_GUID, COMP_NAME),
    "10000:%d:botrecruit %s" % (OWNER_GUID, COMP_NAME),
])

STUB = {"scenario": "slow"}
STUB_LOCK = threading.Lock()


def _seed_sql():
    return """
INSERT INTO tw_char.characters
 (guid,account,name,race,class,gender,level,money,position_x,position_y,position_z,map,
  orientation,zone,health,power1,power2,power3,power4,power5,xp,xp_gain)
VALUES (%d,%d,'Rplowner',1,1,0,10,100000,-8949.95,-120.493,83.5312,0,
 0,12,100,0,0,0,0,0,0,1),
 (%d,%d,'Rplcomp',1,1,0,10,100000,-8949.95,-152.493,83.5312,0,
 0,12,100,0,0,0,0,0,0,1);
INSERT INTO tw_char.playerbot (char_guid,chance,ai)
 VALUES (%d,100,'Default'),(%d,100,'Default');
INSERT INTO tw_char.bot_ownership
 (char_guid,account_id,bot_type,provision_version,owner_account_id)
 VALUES (%d,%d,1,2,NULL),
        (%d,%d,1,2,%d);
DELETE FROM tw_world.creature WHERE map=0
 AND position_x BETWEEN -8990 AND -8910 AND position_y BETWEEN -170 AND -110;
""" % (OWNER_GUID, OWNER_ACC, COMP_GUID, COMP_ACC,
       OWNER_GUID, COMP_GUID, OWNER_GUID, OWNER_ACC,
       COMP_GUID, COMP_ACC, OWNER_ACC)


def _free_port(lo, hi):
    for port in range(lo, hi + 1):
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            try:
                s.bind(("127.0.0.1", port))
                return port
            except OSError:
                continue
    raise RuntimeError("no free port in %d..%d" % (lo, hi))


class _StubHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def _reply(self, status, payload):
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        try:
            self.wfile.write(payload)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def _drain(self):
        length = int(self.headers.get("Content-Length", "0"))
        if length:
            self.rfile.read(length)

    def do_GET(self):
        # llama-compatible paths carry the /v1 prefix.
        if self.path == "/v1/models":
            self._reply(200, json.dumps(
                {"data": [{"id": "stub-model"}]}).encode())
        else:
            self._reply(404, b"{}")

    def do_POST(self):
        self._drain()
        if self.path != "/v1/chat/completions":
            self._reply(404, b"{}")
            return
        with STUB_LOCK:
            scenario = STUB["scenario"]
        if scenario == "slow":
            time.sleep(6.0)
            content = STUB_VALID_CONTENT
        elif scenario == "garbage":
            content = "I would follow the leader and keep a safe distance."
        else:  # valid
            content = STUB_VALID_CONTENT
        payload = json.dumps({"choices": [{
            "message": {"role": "assistant", "content": content}}],
            "usage": {"prompt_tokens": 123, "completion_tokens": 12}}).encode()
        self._reply(200, payload)


class BotCompanionRealPlannerTests(unittest.TestCase):
    @staticmethod
    def _personal_state():
        try:
            out = p.command(["docker", "inspect", "--format",
                             "{{.Name}}|{{.State.StartedAt}}|{{.State.Running}}",
                             *PERSONAL_CONTAINERS], timeout=60)
        except RuntimeError:
            return {}
        state = {}
        for line in out.splitlines():
            parts = line.split("|")
            if len(parts) == 3:
                state[parts[0].lstrip("/")] = (parts[1], parts[2])
        return state

    def test_real_adapter_rounds_and_fallbacks(self):
        p.IMAGE = os.environ.get("PORT021_LAB_IMAGE", "tortoise-local:dev")
        try:
            p.command(["docker", "image", "inspect", "--format", "{{.Id}}", p.IMAGE])
        except RuntimeError:
            self.skipTest("Build the candidate image first")
        before = self._personal_state()
        adapter_port = _free_port(18341, 18369)
        stub_port = _free_port(18401, 18420)
        key_file = None
        project = "tortoise-bot-rplanner-" + uuid.uuid4().hex[:12]
        evidence = p.ROOT / "local" / (project + "-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ"))
        evidence.mkdir(parents=True)
        adapter_log = evidence / "real-planner.log"
        stub_srv = None
        stub_proc = None
        adapter_proc = None
        adapter_out = None
        base = env = None
        all_logs = []
        try:
            # Hermetic key file: the stub ignores auth, but the adapter
            # must read a key before calling.
            key_file = evidence / "model.key"
            key_file.write_text("fixture-key-not-secret\n", encoding="utf-8")

            stub_srv = ThreadingHTTPServer(("127.0.0.1", stub_port),
                                           _StubHandler)
            stub_srv.daemon_threads = True
            stub_proc = threading.Thread(target=stub_srv.serve_forever,
                                         daemon=True)
            stub_proc.start()

            world = p.world_env_for("")
            world.update(PLAYERBOT_MIN_BOTS="0", PLAYERBOT_MAX_BOTS="0",
                         PLAYERBOT_REFRESH="600000", PLAYERBOT_UPDATE_MS="1000",
                         PLAYERBOT_TEST_LOGIN=str(OWNER_GUID) + "," + str(COMP_GUID),
                         PLAYERBOT_QUEST_ID="0", PLAYERBOT_FOLLOW_SCRIPT=SCRIPT,
                         PLAYERBOT_PERSONALITY_PROFILE="reckless",
                         PLAYERBOT_PLANNER_SERVICE_URL="http://hostbridge:%d" % adapter_port,
                         PLAYER_SAVE_INTERVAL="5000")
            base, env = p.boot_lab(project, evidence, world, _seed_sql())
            compose_path = evidence / "compose.json"
            cfg = json.loads(compose_path.read_text(encoding="utf-8"))
            cfg["services"]["world"]["extra_hosts"] = ["hostbridge:host-gateway"]
            compose_path.write_text(json.dumps(cfg, indent=2), encoding="utf-8")

            def start_adapter(model_url, timeout_ms=1500, key_file_path=None):
                nonlocal adapter_proc, adapter_out
                if adapter_proc is not None and adapter_proc.poll() is None:
                    adapter_proc.kill()
                    adapter_proc.wait(timeout=10)
                if adapter_out is not None:
                    adapter_out.close()
                if key_file_path is None:
                    key_file_path = str(key_file)
                adapter_env = dict(os.environ)
                adapter_env.update(
                    REAL_PLANNER_PORT=str(adapter_port),
                    REAL_PLANNER_MODEL_URL=model_url,
                    REAL_PLANNER_KEY_FILE=key_file_path,
                    REAL_PLANNER_TIMEOUT_MS=str(timeout_ms),
                    REAL_PLANNER_TICK_MS="1000",
                    REAL_PLANNER_MAX_TOKENS="64")
                adapter_out = open(adapter_log, "a", encoding="utf-8")
                adapter_proc = subprocess.Popen(
                    [sys.executable, SERVER],
                    stdout=adapter_out, stderr=subprocess.STDOUT,
                    env=adapter_env)
                end = time.monotonic() + 20
                while time.monotonic() < end:
                    try:
                        with urllib.request.urlopen(
                                "http://127.0.0.1:%d/health" % adapter_port,
                                timeout=2) as r:
                            if r.status == 200:
                                return
                    except Exception:
                        time.sleep(0.3)
                raise RuntimeError("real planner server did not come up")

            def adapter_text():
                # The process holds the file open; read from disk.
                return adapter_log.read_text(encoding="utf-8", errors="replace")

            def expect(pred, deadline, label=""):
                """wait_for that dumps the world log on timeout."""
                try:
                    logs = p.wait_for(base, env, pred, deadline=deadline)
                except RuntimeError:
                    try:
                        fail_logs = p.command(
                            ["docker", "compose"] + base + ["logs", "world"],
                            env=env, timeout=60)
                    except RuntimeError:
                        fail_logs = ""
                    all_logs.append(fail_logs)
                    (evidence / "world.log").write_text(
                        "\n".join(all_logs), encoding="utf-8")
                    raise RuntimeError(
                        "world did not reach: %s" % label)
                all_logs.append(logs)
                return logs

            try:
                # World up with the offline adapter (phase A).
                start_adapter("http://127.0.0.1:1/v1")
                p.command(["docker", "compose"] + base + ["up", "-d", "--no-deps", "world"], env=env)
                expect(lambda text:
                    "[Planner] offer owner:%d bot:%d action:1" % (OWNER_GUID, COMP_GUID) in text,
                    deadline=300, label="phase A offer")
                self.assertIn("fallback reqid:", adapter_text())
                self.assertIn("reason:connect-fail", adapter_text())

                # Phase B: timeout (6 s stub vs 1.5 s adapter budget).
                with STUB_LOCK:
                    STUB["scenario"] = "slow"
                start_adapter("http://127.0.0.1:%d/v1" % stub_port)
                end = time.monotonic() + 60
                while time.monotonic() < end and "reason:timeout" not in adapter_text():
                    time.sleep(1)
                self.assertIn("reason:timeout", adapter_text(),
                              "adapter must time out the slow model and fall back")

                # Phase C: strict-schema model answer applies a preference.
                with STUB_LOCK:
                    STUB["scenario"] = "valid"
                start_adapter("http://127.0.0.1:%d/v1" % stub_port)
                logs = expect(lambda text:
                    "[Personality] chase GUID:%d profile:reckless dist:20.0" % COMP_GUID in text,
                    deadline=180, label="phase C chase")
                self.assertIn("model ok reqid:", adapter_text())
                self.assertEqual(logs.count("[Personality] chase GUID:%d" % COMP_GUID), 1)

                # Phase D: prose answer is rejected, Hold fallback applies.
                with STUB_LOCK:
                    STUB["scenario"] = "garbage"
                start_adapter("http://127.0.0.1:%d/v1" % stub_port)
                end = time.monotonic() + 60
                while time.monotonic() < end and "reason:bad-schema" not in adapter_text():
                    time.sleep(1)
                self.assertIn("reason:bad-schema", adapter_text())
                # No personality effect from the malformed round.
                time.sleep(5)
                logs = expect(lambda text:
                    "[Personality] chase GUID:%d" % COMP_GUID in text, deadline=10,
                    label="phase D chase re-capture")
                self.assertEqual(logs.count("[Personality] chase GUID:%d" % COMP_GUID), 1)

                # Phase E: single flight under concurrency.
                with STUB_LOCK:
                    STUB["scenario"] = "slow"

                import fake_planner as fp
                probe_req = fp.make_request({
                    "protocol_version": fp.PROTOCOL_VERSION,
                    "request_id": 0xF00D000000000001,
                    "owner_guid": 610001,
                    "observation_version": fp.OBSERVATION_VERSION,
                    "capture_time_ms": 1000000,
                    "step_count": 0,
                    "total_size": fp.REQUEST_BYTES,
                    "generation": 7,
                    "flags": 0b010,
                    "bots": [(610002, 1)],
                })

                def plan_once():
                    req = urllib.request.Request(
                        "http://127.0.0.1:%d/plan" % adapter_port,
                        data=probe_req, method="POST")
                    with urllib.request.urlopen(req, timeout=10) as r:
                        return r.status

                results = []

                def worker():
                    results.append(plan_once())

                t1 = threading.Thread(target=worker)
                t2 = threading.Thread(target=worker)
                t1.start(); t2.start()
                t1.join(timeout=12); t2.join(timeout=12)
                self.assertEqual(results, [200, 200],
                                 "both concurrent rounds must be answered")
                self.assertIn("reason:busy", adapter_text())

                # Phase F: optional live model round.
                live_url = os.environ.get("PORT021_MODEL_URL", "").strip()
                live = None
                if live_url:
                    live = {"url": live_url, "reached": False,
                            "outcome": None, "model": None}
                    try:
                        import model_client as mc
                        key = mc.read_api_key(os.environ.get("PARK_LLAMA_API_KEY_FILE") or None)
                        live["model"] = mc.fetch_model_id(live_url, key)
                        # Long budget: the transport round has its own
                        # 5 s deadline and falls back; the adapter still
                        # records the measured model response for the
                        # evidence card.
                        # The live service validates its key: use the
                        # operator-local park-llama key file (host path,
                        # never logged), not the hermetic fixture key.
                        live_key = os.environ.get("PARK_LLAMA_API_KEY_FILE")
                        if not live_key:
                            live_key = os.path.join(
                                os.environ["LOCALAPPDATA"], "park-llama",
                                "llama-api.key")
                        start_adapter(live_url, timeout_ms=25000,
                                      key_file_path=live_key)
                        mark = len(adapter_text())
                        end = time.monotonic() + 150
                        while time.monotonic() < end:
                            tail = adapter_text()[mark:]
                            if "plan reqid" not in tail:
                                time.sleep(1)
                                continue
                            live["reached"] = True
                            if "model ok reqid" in tail:
                                line = tail.split("model ok reqid", 1)[1].split("\n", 1)[0]
                                live["outcome"] = "measured response: " + line
                                live["measured"] = [
                                    l for l in tail.splitlines()
                                    if "model ok reqid" in l or
                                    "fallback reqid" in l][:5]
                                break
                            if "fallback reqid" in tail:
                                live["outcome"] = "fallback: " + tail.split(
                                    "fallback reqid", 1)[1].split("\n", 1)[0]
                                break
                        if live["outcome"] is None:
                            live["outcome"] = "no model result within window"
                    except Exception as exc:
                        live["outcome"] = "probe failed: %s" % exc
                (evidence / "world.log").write_text("\n".join(all_logs), encoding="utf-8")
                self.assertNotIn("[CRASH]", "\n".join(all_logs))
                if live is not None:
                    (evidence / "live-model-measurement.json").write_text(
                        json.dumps(live, indent=2), encoding="utf-8")
            finally:
                if adapter_proc is not None and adapter_proc.poll() is None:
                    adapter_proc.kill()
                if adapter_out is not None:
                    adapter_out.close()
                if stub_srv is not None:
                    stub_srv.shutdown()
                if not (evidence / "world.log").exists() and all_logs:
                    (evidence / "world.log").write_text("\n".join(all_logs), encoding="utf-8")
        finally:
            if base is not None:
                try:
                    p.teardown_lab(base, env, project, evidence)
                except Exception:
                    p.force_down(base, env)
            if adapter_proc is not None and adapter_proc.poll() is None:
                adapter_proc.kill()
            if adapter_out is not None:
                adapter_out.close()
            if stub_srv is not None:
                stub_srv.shutdown()
        self.assertEqual(before, self._personal_state())


if __name__ == "__main__":
    unittest.main()
