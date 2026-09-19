"""PORT-020 (KAP-558): persisted versioned personality state, runtime
proof.

One disposable Compose project, one database, three world generations:

Gen 1 (config reckless, empty personality table): the companion's first
login seeds exactly one bot_personality row (schema 1, profile
reckless) from the declared config, and the fake service's chase
proposal applies the reckless distance (behavior proof).
Gen 2 (config changed to cautious, same DB): the re-invited companion
keeps the persisted reckless profile - no reassignment, the config
never overrides a persisted identity, and character state is unchanged
across the restart.
Gen 3 (row tampered to an unknown schema version): the load fails
closed to the deterministic baseline - the transport still rounds, but
no personality effect appears, and the tampered row is never
overwritten.
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

OWNER_GUID = 500150
COMP_GUID = 500151
OWNER_ACC = 1000500150
COMP_ACC = 1000500151
PERSONAL_CONTAINERS = ("tortoise-local-db-1", "tortoise-local-realmd-1",
                       "tortoise-local-world-1")

SCRIPT = ";".join([
    "5000:%d:botfollow Perscomp" % OWNER_GUID,
    "10000:%d:botrecruit Perscomp" % OWNER_GUID,
])


def _seed_sql():
    return """
INSERT INTO tw_char.characters
 (guid,account,name,race,class,gender,level,money,position_x,position_y,position_z,map,
  orientation,zone,health,power1,power2,power3,power4,power5,xp,xp_gain)
VALUES (%d,%d,'Persowner',1,1,0,10,100000,-8949.95,-120.493,83.5312,0,
 0,12,100,0,0,0,0,0,0,1),
 (%d,%d,'Perscomp',1,1,0,10,100000,-8949.95,-152.493,83.5312,0,
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


def _char_state(base, env):
    # Stable identity fields only: positions move with normal
    # follow/wander gameplay and are not personality state.
    out = p.db_exec(base, env,
                    "SELECT guid, money, level "
                    "FROM characters WHERE guid IN (%d, %d) ORDER BY guid"
                    % (OWNER_GUID, COMP_GUID))
    return out.strip()


class BotCompanionPersonalityPersistTests(unittest.TestCase):
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

    def test_persisted_identity_survives_restart_and_fails_closed(self):
        p.IMAGE = os.environ.get("PORT020_LAB_IMAGE", "tortoise-local:dev")
        try:
            p.command(["docker", "image", "inspect", "--format", "{{.Id}}", p.IMAGE])
        except RuntimeError:
            self.skipTest("Build the candidate image first")
        before = self._personal_state()
        port = _free_port()
        project = "tortoise-bot-persist-" + uuid.uuid4().hex[:12]
        evidence = p.ROOT / "local" / (project + "-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ"))
        evidence.mkdir(parents=True)
        server_log = evidence / "fake-planner.log"
        server_proc = None
        server_out = None
        base = env = None
        all_logs = []
        try:
            world = p.world_env_for("")
            world.update(PLAYERBOT_MIN_BOTS="0", PLAYERBOT_MAX_BOTS="0",
                         PLAYERBOT_REFRESH="600000", PLAYERBOT_UPDATE_MS="1000",
                         PLAYERBOT_TEST_LOGIN=str(OWNER_GUID) + "," + str(COMP_GUID),
                         PLAYERBOT_QUEST_ID="0", PLAYERBOT_FOLLOW_SCRIPT=SCRIPT,
                         PLAYERBOT_PERSONALITY_PROFILE="reckless",
                         PLAYERBOT_PLANNER_SERVICE_URL="http://hostbridge:%d" % port,
                         PLAYER_SAVE_INTERVAL="5000")
            base, env = p.boot_lab(project, evidence, world, _seed_sql())
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
            try:
                # ---- Gen 1: config seed --------------------------------
                p.command(["docker", "compose"] + base + ["up", "-d", "--no-deps", "world"], env=env)
                logs = p.wait_for(base, env, lambda text:
                    "[Personality] assigned GUID:%d profile:reckless (config seed)" % COMP_GUID in text,
                    deadline=300)
                all_logs.append(logs)
                row = p.db_exec(base, env,
                                "SELECT schema_version, profile_id, assigned_at "
                                "FROM bot_personality WHERE char_guid=%d" % COMP_GUID).strip()
                self.assertTrue(row.startswith("1\t1\t"), "unexpected row: %r" % row)
                assigned_at = row.split("\t")[2]
                logs = p.wait_for(base, env, lambda text:
                    "[Personality] chase GUID:%d profile:reckless dist:20.0" % COMP_GUID in text,
                    deadline=120)
                all_logs.append(logs)
                state1 = _char_state(base, env)
                self.assertNotIn("[CRASH]", logs)
                p.command(["docker", "compose"] + base + ["stop", "world"], env=env)
                logs = p.wait_for(base, env, lambda text:
                    "[Planner] transport stopped" in text, deadline=60)
                all_logs.append(logs)
                gen1_final = logs  # full gen1 container log: one seed

                # ---- Gen 2: config changed, persisted wins -------------
                # The world environment is baked into compose.json at
                # boot; changing only the subprocess env would restart
                # the same config. Rewrite the file so compose recreates
                # the world with the new declared profile.
                cfg = json.loads(compose_path.read_text(encoding="utf-8"))
                cfg["services"]["world"]["environment"]["PLAYERBOT_PERSONALITY_PROFILE"] = "cautious"
                compose_path.write_text(json.dumps(cfg, indent=2), encoding="utf-8")
                env2 = dict(env)
                p.command(["docker", "compose"] + base + ["up", "-d", "--no-deps", "world"], env=env2)
                logs = p.wait_for(base, env2, lambda text:
                    "[PlayerBot][Login]  'Perscomp'" in text and
                    "[Planner] transport started" in text, deadline=300)
                all_logs.append(logs)
                self.assertIn("[PlayerBotMgr] personality profile:cautious", logs)
                logs = p.wait_for(base, env2, lambda text:
                    "[Personality] chase GUID:%d profile:reckless dist:20.0" % COMP_GUID in text,
                    deadline=120)
                all_logs.append(logs)
                gen2_final = logs  # fresh gen2 container log: no re-seed
                # The persisted identity is unchanged: same row, same seed time.
                row2 = p.db_exec(base, env2,
                                 "SELECT schema_version, profile_id, assigned_at "
                                 "FROM bot_personality WHERE char_guid=%d" % COMP_GUID).strip()
                self.assertEqual(row2, row)
                self.assertEqual(_char_state(base, env2), state1)
                # Each wait_for returns the full cumulative container log,
                # so count within one generation final log, not across
                # overlapping dumps. Gen-1 seeds exactly once; Gen-2 (fresh
                # container, persisted row present) must not re-seed.
                seed = "[Personality] assigned GUID:%d" % COMP_GUID
                self.assertEqual(gen1_final.count(seed), 1,
                                 "gen1 must seed exactly once, saw %d" % gen1_final.count(seed))
                self.assertEqual(gen2_final.count(seed), 0,
                                 "gen2 must not re-seed a persisted identity")
                p.command(["docker", "compose"] + base + ["stop", "world"], env=env2)
                p.wait_for(base, env2, lambda text: "[Planner] transport stopped" in text, deadline=60)

                # ---- Gen 3: unknown schema fails closed ----------------
                p.db_exec(base, env2,
                          "UPDATE bot_personality SET schema_version=99 "
                          "WHERE char_guid=%d" % COMP_GUID)
                # No world environment changed since gen 2, so compose
                # would restart the same container and its log would keep
                # the gen-2 chase line. A marker-only env var (ignored by
                # the server) forces a fresh container so each generation
                # is counted in isolation, as in gens 1-2.
                cfg = json.loads(compose_path.read_text(encoding="utf-8"))
                cfg["services"]["world"]["environment"]["PORT020_GEN"] = "3"
                compose_path.write_text(json.dumps(cfg, indent=2), encoding="utf-8")
                p.command(["docker", "compose"] + base + ["up", "-d", "--no-deps", "world"], env=env2)
                logs = p.wait_for(base, env2, lambda text:
                    "personality row for %d rejected (schema=99 profile=1); deterministic baseline" % COMP_GUID in text,
                    deadline=300)
                all_logs.append(logs)
                # The transport still plans; the personality must stay dead.
                p.wait_for(base, env2, lambda text:
                    "[Planner] submit owner:%d gen:" % OWNER_GUID in text, deadline=120)
                time.sleep(15)
                # Re-capture so the count covers the post-submit planner
                # activity too (fresh gen-3 container: all lines are
                # generation 3).
                logs = p.wait_for(base, env2, lambda text:
                    "[Planner] submit owner:%d gen:" % OWNER_GUID in text, deadline=30)
                self.assertEqual(logs.count("[Personality] chase GUID:%d" % COMP_GUID), 0)
                self.assertEqual(logs.count("[Personality] expr GUID:%d" % COMP_GUID), 0)
                row3 = p.db_exec(base, env2,
                                 "SELECT schema_version, profile_id, assigned_at "
                                 "FROM bot_personality WHERE char_guid=%d" % COMP_GUID).strip()
                self.assertTrue(row3.startswith("99\t1\t"), "tampered row must not be rewritten: %r" % row3)
                (evidence / "world.log").write_text("\n".join(all_logs), encoding="utf-8")
                self.assertNotIn("[CRASH]", "\n".join(all_logs))
            finally:
                if server_proc is not None and server_proc.poll() is None:
                    server_proc.kill()
                if server_out is not None:
                    server_out.close()
                # Failure evidence: the last captured log, even when a
                # wait timed out before the success-path write.
                if not (evidence / "world.log").exists() and all_logs:
                    (evidence / "world.log").write_text("\n".join(all_logs), encoding="utf-8")
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


if __name__ == "__main__":
    unittest.main()
