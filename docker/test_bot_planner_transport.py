"""PORT-018 (KAP-558): bounded nonblocking planner transport, runtime proof.

Two disposable Compose runs (port-free, unique projects, no personal
world, no real model):

Run 1 (disabled regression): no PlayerBot.PlannerServiceURL. Party and
follow work exactly as before and the world log contains zero
[Planner] lines: the transport is a no-op by default.

Run 2 (live transport): the world talks to a host-side fake planner
(docker/personality-service/fake_planner_server.py) over the compose
host-gateway. The fixture drives the scenario endpoint and asserts the
log contract: started/submit/offer, per-reason rejects (stale,
size-mismatch, unknown-version), timeout counting into a 60 s cooldown
with request silence, party-session invalidation on dismiss/recruit,
and a clean transport stop.
"""
import json
import os
import socket
import subprocess
import sys
import time
import unittest
import urllib.request
import uuid
from datetime import datetime, timezone

import test_bot_provision as p

ROOT = os.path.dirname(os.path.abspath(__file__))
SERVER = os.path.join(ROOT, "personality-service", "fake_planner_server.py")

OWNER_GUID = 500140
COMP_GUID = 500141
OWNER_ACC = 1000500140
COMP_ACC = 1000500141
PERSONAL_CONTAINERS = ("tortoise-local-db-1", "tortoise-local-realmd-1",
                       "tortoise-local-world-1")

FULL_SCRIPT = ";".join([
    "5000:%d:botfollow Plancomp" % OWNER_GUID,
    "10000:%d:botrecruit Plancomp" % OWNER_GUID,
    "75000:%d:botdismiss Plancomp" % OWNER_GUID,
    "85000:%d:botrecruit Plancomp" % OWNER_GUID,
])
SHORT_SCRIPT = ";".join([
    "5000:%d:botfollow Plancomp" % OWNER_GUID,
    "10000:%d:botrecruit Plancomp" % OWNER_GUID,
])


def _seed_sql():
    return """
INSERT INTO tw_char.characters
 (guid,account,name,race,class,gender,level,money,position_x,position_y,position_z,map,
  orientation,zone,health,power1,power2,power3,power4,power5,xp,xp_gain)
VALUES (%d,%d,'Planowner',1,1,0,10,100000,-8949.95,-120.493,83.5312,0,
 0,12,100,0,0,0,0,0,0,1),
 (%d,%d,'Plancomp',1,1,0,10,100000,-8949.95,-152.493,83.5312,0,
 0,12,100,0,0,0,0,0,0,1);
INSERT INTO tw_char.playerbot (char_guid,chance,ai)
 VALUES (%d,100,'Default'),(%d,100,'Default');
INSERT INTO tw_char.bot_ownership
 (char_guid,account_id,bot_type,provision_version,owner_account_id)
 VALUES (%d,%d,1,2,NULL),
        (%d,%d,1,2,%d);
-- Lab isolation: empty the spawn box so the pair idles deterministically.
DELETE FROM tw_world.creature WHERE map=0
 AND position_x BETWEEN -8990 AND -8910 AND position_y BETWEEN -170 AND -110;
""" % (OWNER_GUID, OWNER_ACC, COMP_GUID, COMP_ACC,
       OWNER_GUID, COMP_GUID, OWNER_GUID, OWNER_ACC,
       COMP_GUID, COMP_ACC, OWNER_ACC)


def _free_port():
    for port in range(18311, 18340):
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            try:
                s.bind(("127.0.0.1", port))
                return port
            except OSError:
                continue
    raise RuntimeError("no free fake-planner port in 18311..18339")


def _http(port, path, data=None):
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path),
                                 data=data, method="POST" if data else "GET")
    with urllib.request.urlopen(req, timeout=10) as r:
        return r.status, r.read()


def _server_lines(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return f.read()
    except FileNotFoundError:
        return ""


def _server_post_count(path):
    return _server_lines(path).count("POST /plan")


def _wait_server(port, deadline=30):
    end = time.monotonic() + deadline
    while time.monotonic() < end:
        try:
            status, _ = _http(port, "/scenario")
            if status == 200:
                return
        except Exception:
            pass
        time.sleep(0.5)
    raise RuntimeError("fake planner server did not come up")


class BotPlannerTransportTests(unittest.TestCase):
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

    def _base_env(self, script):
        world = p.world_env_for("")
        world.update(PLAYERBOT_MIN_BOTS="0", PLAYERBOT_MAX_BOTS="0",
                     PLAYERBOT_REFRESH="600000", PLAYERBOT_UPDATE_MS="1000",
                     PLAYERBOT_TEST_LOGIN=str(OWNER_GUID) + "," + str(COMP_GUID),
                     PLAYERBOT_QUEST_ID="0", PLAYERBOT_FOLLOW_SCRIPT=script,
                     PLAYER_SAVE_INTERVAL="5000")
        return world

    def test_run1_disabled_transport_is_a_noop(self):
        p.IMAGE = os.environ.get("PORT018_LAB_IMAGE", "tortoise-local:dev")
        try:
            p.command(["docker", "image", "inspect", "--format", "{{.Id}}", p.IMAGE])
        except RuntimeError:
            self.skipTest("Build the candidate image first")
        before = self._personal_state()
        project = "tortoise-bot-planner0-" + uuid.uuid4().hex[:12]
        evidence = p.ROOT / "local" / (project + "-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ"))
        evidence.mkdir(parents=True)
        base = env = None
        try:
            base, env = p.boot_lab(project, evidence, self._base_env(SHORT_SCRIPT), _seed_sql())
            p.command(["docker", "compose"] + base + ["up", "-d", "--no-deps", "world"], env=env)
            logs = p.wait_for(base, env, lambda text:
                "[PlayerBot][Follow] active GUID:%d leader:%d seq:1" % (COMP_GUID, OWNER_GUID)
                in text and "party recruit accepted bot:Plancomp" in text, deadline=300)
            (evidence / "world.log").write_text(logs, encoding="utf-8")
            self.assertEqual(logs.count("[Planner]"), 0)
            self.assertNotIn("[CRASH]", logs)
        finally:
            if base is not None:
                p.teardown_lab(base, env, project, evidence)
        self.assertEqual(before, self._personal_state())

    def test_run2_live_transport_contract(self):
        p.IMAGE = os.environ.get("PORT018_LAB_IMAGE", "tortoise-local:dev")
        try:
            p.command(["docker", "image", "inspect", "--format", "{{.Id}}", p.IMAGE])
        except RuntimeError:
            self.skipTest("Build the candidate image first")
        before = self._personal_state()
        port = _free_port()
        project = "tortoise-bot-planner1-" + uuid.uuid4().hex[:12]
        evidence = p.ROOT / "local" / (project + "-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ"))
        evidence.mkdir(parents=True)
        server_log = evidence / "fake-planner.log"
        server_proc = None
        base = env = None
        try:
            world = self._base_env(FULL_SCRIPT)
            world["PLAYERBOT_PLANNER_SERVICE_URL"] = "http://hostbridge:%d" % port
            base, env = p.boot_lab(project, evidence, world, _seed_sql())
            # host-side service reachability for the world container.
            compose_path = evidence / "compose.json"
            cfg = json.loads(compose_path.read_text(encoding="utf-8"))
            cfg["services"]["world"]["extra_hosts"] = ["hostbridge:host-gateway"]
            compose_path.write_text(json.dumps(cfg, indent=2), encoding="utf-8")
            server_proc = subprocess.Popen(
                [sys.executable, SERVER, str(port)],
                stdout=open(server_log, "w", encoding="utf-8"),
                stderr=subprocess.STDOUT)
            _wait_server(port)
            try:
                p.command(["docker", "compose"] + base + ["up", "-d", "--no-deps", "world"], env=env)
                logs = p.wait_for(base, env, lambda text:
                    "[Planner] transport started url:http://hostbridge:%d" % port in text,
                    deadline=180)
                # success: shared round submitted and the offer recorded.
                logs = p.wait_for(base, env, lambda text:
                    "[Planner] submit owner:%d gen:0" % OWNER_GUID in text and
                    "[Planner] offer owner:%d bot:%d action:1" % (OWNER_GUID, COMP_GUID) in text,
                    deadline=120)
                self.assertGreater(_server_post_count(str(server_log)), 0)
                offers_before = logs.count("[Planner] offer owner:%d" % OWNER_GUID)
                # stale: the service answered a capture past the age budget.
                _http(port, "/scenario", b"stale")
                logs = p.wait_for(base, env, lambda text:
                    "[Planner] reject owner:%d bot:%d reason:stale" % (OWNER_GUID, COMP_GUID) in text,
                    deadline=90)
                # malformed: truncated body, size no longer matches.
                _http(port, "/scenario", b"malformed")
                logs = p.wait_for(base, env, lambda text:
                    "[Planner] reject owner:%d bot:%d reason:size-mismatch" % (OWNER_GUID, COMP_GUID) in text,
                    deadline=90)
                # unsupported: protocol version bump rejected.
                _http(port, "/scenario", b"unsupported")
                logs = p.wait_for(base, env, lambda text:
                    "[Planner] reject owner:%d bot:%d reason:unknown-version" % (OWNER_GUID, COMP_GUID) in text,
                    deadline=90)
                # timeout: repeated round failures cool the session down.
                _http(port, "/scenario", b"timeout")
                logs = p.wait_for(base, env, lambda text:
                    "[Planner] cooldown owner:%d ms:60000" % OWNER_GUID in text,
                    deadline=180)
                self.assertGreaterEqual(logs.count("[Planner] timeout owner:%d" % OWNER_GUID), 3)
                # cooldown silence: no new round reaches the service.
                posts_at_cooldown = _server_post_count(str(server_log))
                time.sleep(10)
                self.assertEqual(_server_post_count(str(server_log)), posts_at_cooldown)
                # party-session change: dismiss kills the session, recruit
                # starts a fresh generation (invalidation also clears the
                # cooldown, so the new session submits immediately).
                _http(port, "/scenario", b"success")
                logs = p.wait_for(base, env, lambda text:
                    "party dismiss accepted bot:Plancomp" in text and
                    "[Planner] invalidate owner:%d" % OWNER_GUID in text,
                    deadline=120)
                inv_idx = logs.find("[Planner] invalidate owner:%d" % OWNER_GUID)
                self.assertGreater(inv_idx, logs.find("party dismiss accepted bot:Plancomp"))
                logs = p.wait_for(base, env, lambda text:
                    text.count("party recruit accepted bot:Plancomp") >= 2 and
                    text.count("[Planner] submit owner:%d gen:1" % OWNER_GUID) >= 1,
                    deadline=120)
                self.assertGreater(logs.count("[Planner] offer owner:%d" % OWNER_GUID),
                                   offers_before)
                # clean stop: bounded worker join, no crash.
                p.command(["docker", "compose"] + base + ["stop", "world"],
                          env=env, timeout=180)
                logs = p.command(["docker", "compose"] + base +
                                 ["logs", "--no-color", "world"], env=env, timeout=60)
                (evidence / "world.log").write_text(logs, encoding="utf-8")
                (evidence / "fake-planner.log").write_text(
                    _server_lines(str(server_log)), encoding="utf-8")
                self.assertIn("[Planner] transport stopped", logs)
                self.assertNotIn("[CRASH]", logs)
                p.teardown_lab(base, env, project, evidence)
                base = None
            finally:
                if server_proc is not None:
                    server_proc.terminate()
                    try:
                        server_proc.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        server_proc.kill()
        finally:
            if base is not None:
                try:
                    (evidence / "world-failure.log").write_text(
                        p.command(["docker", "compose"] + base +
                                  ["logs", "--no-color", "world"],
                                  env=env, timeout=60), encoding="utf-8")
                except Exception:
                    pass
                p.force_down(base, env)
            if server_proc is not None and server_proc.poll() is None:
                server_proc.kill()
        self.assertEqual(before, self._personal_state())


if __name__ == "__main__":
    unittest.main(verbosity=2)
