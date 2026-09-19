"""PORT-019 (KAP-558): bounded personality preferences and expression,
runtime proof.

Three disposable Compose runs (port-free, unique projects, no personal
world, no real model). One host-side fake planner
(docker/personality-service/fake_planner_server.py) supplies the same
Preference proposal in every scenario; the declared profile decides the
concrete bounded outcome:

Run A (profile reckless, service enabled): the identical chase proposal
applies dist 20.0 (the reckless table) and the identical expression
proposal speaks the reckless slot-0 line, rate-limited to one per 15 s.
Run B (profile cautious, service enabled): the same proposals apply
dist 30.0 and the cautious slot-0 line: observably different, both
inside the declared band and allowlist.
Run C (profile reckless, no service): model absence is the deterministic
baseline - follow and party work, zero [Planner] lines and no chase or
expression effect (only the one-time PORT-020 config seed may log).
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

SCRIPT = ";".join([
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
    for port in range(18371, 18400):
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            try:
                s.bind(("127.0.0.1", port))
                return port
            except OSError:
                continue
    raise RuntimeError("no free fake-planner port in 18371..18399")


def _http(port, path, data=None):
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path),
                                 data=data, method="POST" if data else "GET")
    with urllib.request.urlopen(req, timeout=10) as r:
        return r.status, r.read()


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


class BotCompanionPersonalityTests(unittest.TestCase):
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

    def _base_env(self, profile, url):
        world = p.world_env_for("")
        world.update(PLAYERBOT_MIN_BOTS="0", PLAYERBOT_MAX_BOTS="0",
                     PLAYERBOT_REFRESH="600000", PLAYERBOT_UPDATE_MS="1000",
                     PLAYERBOT_TEST_LOGIN=str(OWNER_GUID) + "," + str(COMP_GUID),
                     PLAYERBOT_QUEST_ID="0", PLAYERBOT_FOLLOW_SCRIPT=SCRIPT,
                     PLAYERBOT_PERSONALITY_PROFILE=profile,
                     PLAYERBOT_PLANNER_SERVICE_URL=url,
                     PLAYER_SAVE_INTERVAL="5000")
        return world

    def _check_image(self):
        p.IMAGE = os.environ.get("PORT019_LAB_IMAGE", "tortoise-local:dev")
        try:
            p.command(["docker", "image", "inspect", "--format", "{{.Id}}", p.IMAGE])
        except RuntimeError:
            self.skipTest("Build the candidate image first")

    def _run_profile(self, run_idx, profile, dist, line, port):
        """One service-enabled run: identical proposals, profile-bound outcomes."""
        before = self._personal_state()
        project = "tortoise-bot-persona%d-%s" % (run_idx, uuid.uuid4().hex[:12])
        evidence = p.ROOT / "local" / (project + "-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ"))
        evidence.mkdir(parents=True)
        server_log = evidence / "fake-planner.log"
        server_proc = None
        server_out = None
        base = env = None
        try:
            world = self._base_env(profile, "http://hostbridge:%d" % port)
            base, env = p.boot_lab(project, evidence, world, _seed_sql())
            # host-side service reachability for the world container.
            compose_path = evidence / "compose.json"
            cfg = json.loads(compose_path.read_text(encoding="utf-8"))
            cfg["services"]["world"]["extra_hosts"] = ["hostbridge:host-gateway"]
            compose_path.write_text(json.dumps(cfg, indent=2), encoding="utf-8")
            server_out = open(server_log, "w", encoding="utf-8")
            server_proc = subprocess.Popen(
                [sys.executable, SERVER, str(port)],
                stdout=server_out, stderr=subprocess.STDOUT)
            _wait_server(port)
            _http(port, "/scenario", b"chase")
            logs = None
            try:
                p.command(["docker", "compose"] + base + ["up", "-d", "--no-deps", "world"], env=env)
                logs = p.wait_for(base, env, lambda text:
                    "[Planner] transport started url:http://hostbridge:%d" % port in text,
                    deadline=180)
                # The profile config was loaded with a declared profile.
                self.assertIn("[PlayerBotMgr] personality profile:%s" % profile, logs)
                # Same chase proposal (index 1) -> the profile's table value.
                logs = p.wait_for(base, env, lambda text:
                    "[Personality] chase GUID:%d profile:%s dist:%s" % (COMP_GUID, profile, dist) in text,
                    deadline=300)
                # Same expression proposal (slot 0) -> the profile's line.
                _http(port, "/scenario", b"express")
                logs = p.wait_for(base, env, lambda text:
                    "[Personality] expr GUID:%d profile:%s slot:0" % (COMP_GUID, profile) in text,
                    deadline=300)
                exprs = logs.count("[Personality] expr GUID:%d" % COMP_GUID)
                self.assertEqual(exprs, 1)
                # Rate bound: inside the 15 s interval no second line.
                time.sleep(8)
                logs2 = p.wait_for(base, env, lambda text:
                    "[Personality] expr GUID:%d" % COMP_GUID in text, deadline=15)
                self.assertEqual(logs2.count("[Personality] expr GUID:%d" % COMP_GUID), 1)
                p.command(["docker", "compose"] + base + ["stop", "world"], env=env)
                logs = p.wait_for(base, env, lambda text:
                    "[Planner] transport stopped" in text, deadline=60)
                (evidence / "world.log").write_text(logs, encoding="utf-8")
                self.assertNotIn("[CRASH]", logs)
            finally:
                if server_proc is not None and server_proc.poll() is None:
                    server_proc.kill()
                if server_out is not None:
                    server_out.close()
                # Failure evidence: the last captured log, even when a
                # wait timed out before the success-path write.
                if logs is not None and not (evidence / "world.log").exists():
                    (evidence / "world.log").write_text(logs, encoding="utf-8")
        finally:
            if base is not None:
                try:
                    p.teardown_lab(base, env, project, evidence)
                except Exception:
                    p.force_down(base, env)
            if server_proc is not None and server_proc.poll() is None:
                server_proc.kill()
            if server_out is not None:
                server_out.close()
        self.assertEqual(before, self._personal_state())

    def test_runA_reckless_profile_bounded_outcomes(self):
        self._check_image()
        self._run_profile(0, "reckless", "20.0",
                          "Let's go, stay behind me!", _free_port())

    def test_runB_cautious_profile_bounded_outcomes(self):
        self._check_image()
        self._run_profile(1, "cautious", "30.0",
                          "I'll keep my distance.", _free_port())

    def test_runC_model_absence_is_the_baseline(self):
        self._check_image()
        before = self._personal_state()
        project = "tortoise-bot-persona2-" + uuid.uuid4().hex[:12]
        evidence = p.ROOT / "local" / (project + "-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ"))
        evidence.mkdir(parents=True)
        base = env = None
        try:
            # Profile declared but no service: no proposals can arrive,
            # so the deterministic baseline must hold exactly.
            world = self._base_env("reckless", "")
            base, env = p.boot_lab(project, evidence, world, _seed_sql())
            p.command(["docker", "compose"] + base + ["up", "-d", "--no-deps", "world"], env=env)
            logs = p.wait_for(base, env, lambda text:
                "[PlayerBot][Follow] active GUID:%d leader:%d seq:1" % (COMP_GUID, OWNER_GUID)
                in text and "party recruit accepted bot:Plancomp" in text, deadline=300)
            (evidence / "world.log").write_text(logs, encoding="utf-8")
            self.assertEqual(logs.count("[Planner]"), 0)
            # No service: no proposals can arrive, so no chase or
            # expression effect. The only allowed personality line is
            # the one-time PORT-020 config seed for the declared profile.
            self.assertEqual(logs.count("[Personality] chase"), 0)
            self.assertEqual(logs.count("[Personality] expr"), 0)
            self.assertLessEqual(logs.count("[Personality] assigned"), 1)
            self.assertIn("[PlayerBotMgr] personality profile:reckless", logs)
            self.assertNotIn("[CRASH]", logs)
        finally:
            if base is not None:
                p.teardown_lab(base, env, project, evidence)
        self.assertEqual(before, self._personal_state())


if __name__ == "__main__":
    unittest.main()
