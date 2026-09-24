"""Disposable, port-free owned-world activation smoke test.

Four durable owned bots and a socketless test owner are seeded in a separate
Compose project. Target two must queue exactly two owned logins, one per pace,
without creating any party. The personal server/volume is never used.
"""
import os
import json
import socket
import subprocess
import sys
import time
import unittest
import uuid
from datetime import datetime, timezone

import test_bot_provision as p


OWNER = 620100
BOT_COUNT = int(os.environ.get("BOT_OWNED_WORLD_BOTS", "4"))
TARGET = int(os.environ.get("BOT_OWNED_WORLD_TARGET", "2"))
REAL_INTENT_LAB = os.environ.get("BOT_OWNED_WORLD_INTENT_REAL", "0") == "1"
INTENT_LAB = REAL_INTENT_LAB or os.environ.get("BOT_OWNED_WORLD_INTENT", "0") == "1"
if not (2 <= BOT_COUNT <= 60 and 1 <= TARGET <= BOT_COUNT):
    raise ValueError("disposable owned-world fixture requires 2..60 bots and a valid target")
BOT_GUIDS = tuple(620101 + index for index in range(BOT_COUNT))
BOT_SUFFIXES = tuple(
    chr(97 + index) if index < 26 else chr(97 + (index - 26) // 26) + chr(97 + (index - 26) % 26)
    for index in range(BOT_COUNT))
OWNER_ACC = 1000620100


def seed_sql():
    rows = [(OWNER, OWNER_ACC, "Worldowner")]
    rows += [(guid, 1000620100 + index, "Worldbot" + suffix)
             for index, (guid, suffix) in enumerate(zip(BOT_GUIDS, BOT_SUFFIXES), 1)]
    characters = ",".join(
        "(%d,%d,'%s',1,1,0,10,100000,-8949.95,%f,83.5312,0,0,12,100,0,0,0,0,0,0,1)"
        % (guid, account, name, -120.493 - (index % 5) * 8.0)
        for index, (guid, account, name) in enumerate(rows))
    roster = ",".join("(%d,100,'Default')" % guid for guid, _, _ in rows)
    bindings = ",".join(
        "(%d,%d,1,2,%s)" % (guid, account, "NULL" if guid == OWNER else str(OWNER_ACC))
        for guid, account, _ in rows)
    return ("INSERT INTO tw_char.characters "
            "(guid,account,name,race,class,gender,level,money,position_x,position_y,position_z,map,"
            "orientation,zone,health,power1,power2,power3,power4,power5,xp,xp_gain) VALUES %s;"
            " INSERT INTO tw_char.playerbot (char_guid,chance,ai) VALUES %s;"
            " INSERT INTO tw_char.bot_ownership "
            "(char_guid,account_id,bot_type,provision_version,owner_account_id) VALUES %s;"
            " DELETE FROM tw_world.creature WHERE map = 0 "
            "AND position_x BETWEEN -8990 AND -8910 AND position_y BETWEEN -170 AND -110;"
            % (characters, roster, bindings))


class OwnedWorldTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        p.IMAGE = os.environ.get("TW014_LAB_IMAGE", "tortoise-local:dev")
        p.command(["docker", "image", "inspect", p.IMAGE])
        cls.project = "tortoise-owned-world-" + uuid.uuid4().hex[:12]
        stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        cls.evidence = p.ROOT / "local" / (cls.project + "-" + stamp)
        cls.evidence.mkdir(parents=True)
        cls.base = cls.env = None
        cls.adapter_proc = None
        cls.adapter_out = None
        world = p.world_env_for("Worldowner")
        world.update(PLAYERBOT_MIN_BOTS="0", PLAYERBOT_MAX_BOTS="0",
                     PLAYERBOT_PROVISION="", PLAYERBOT_DEBUG="1",
                     PLAYERBOT_TEST_LOGIN=str(OWNER),
                     PLAYERBOT_OWNED_WORLD_TARGET=str(TARGET),
                     PLAYERBOT_OWNED_WORLD_PACE_MS="1000",
                     PLAYERBOT_UPDATE_MS="1000", PLAYERBOT_REFRESH="600000",
                     PLAYERBOT_QUEST_ID="0", PERF_PROCESSING_TELEMETRY="5")
        try:
            if INTENT_LAB:
                with socket.socket() as sock:
                    sock.bind(("0.0.0.0", 0))
                    port = sock.getsockname()[1]
                cls.adapter_log = cls.evidence / (
                    "real-world-intent.log" if REAL_INTENT_LAB else "fake-world-intent.log")
                cls.adapter_out = open(cls.adapter_log, "a", encoding="utf-8")
                service = p.ROOT / "docker" / "personality-service"
                if REAL_INTENT_LAB:
                    adapter_env = dict(os.environ)
                    adapter_env.update(
                        REAL_PLANNER_PORT=str(port),
                        REAL_PLANNER_MODEL_URL=os.environ.get(
                            "BOT_OWNED_WORLD_MODEL_URL", "http://127.0.0.1:8090/v1"),
                        REAL_PLANNER_KEY_FILE=os.environ.get(
                            "PARK_LLAMA_API_KEY_FILE",
                            os.path.join(os.environ["LOCALAPPDATA"], "park-llama", "llama-api.key")))
                    command = [sys.executable, str(service / "real_planner_server.py")]
                else:
                    adapter_env = dict(os.environ)
                    command = [sys.executable, str(service / "fake_planner_server.py"), str(port)]
                cls.adapter_proc = subprocess.Popen(
                    command, stdout=cls.adapter_out, stderr=subprocess.STDOUT,
                    env=adapter_env)
                world.update(PLAYERBOT_WORLD_INTENT_ENABLE="1",
                             PLAYERBOT_WORLD_INTENT_INTERVAL_MS="60000",
                             PLAYERBOT_WORLD_INTENT_GLOBAL_PACE_MS="5000",
                             PLAYERBOT_CONVERSATION_SERVICE_URL="http://hostbridge:%d" % port)
            cls.base, cls.env = p.boot_lab(cls.project, cls.evidence, world, seed_sql())
            if INTENT_LAB:
                compose_path = cls.evidence / "compose.json"
                cfg = json.loads(compose_path.read_text(encoding="utf-8"))
                cfg["services"]["world"]["extra_hosts"] = ["hostbridge:host-gateway"]
                compose_path.write_text(json.dumps(cfg, indent=2), encoding="utf-8")
            p.command(["docker", "compose"] + cls.base + ["up", "-d", "--no-deps", "world"],
                      env=cls.env)
            cls.logs = p.wait_for(
                cls.base, cls.env,
                lambda text: text.count("Playerbot: owned world login queued guid:") >= TARGET,
                deadline=420)
            cls.logs = p.wait_for(
                cls.base, cls.env,
                lambda text: all("[PlayerBot][Login]  'Worldbot%s'" % suffix in text
                                 for suffix in BOT_SUFFIXES[:TARGET]),
                deadline=300)
            if INTENT_LAB:
                cls.logs = p.wait_for(
                    cls.base, cls.env,
                    lambda text: all(
                        "[WorldIntent] accepted bot:%d intent:" % guid in text
                        for guid in (BOT_GUIDS[:TARGET] if REAL_INTENT_LAB else BOT_GUIDS[:1])),
                    deadline=180 if REAL_INTENT_LAB else 120)
            time.sleep(60 if TARGET >= 25 else 10)  # over-target and steady-state window
            cls.logs = p.command(["docker", "compose"] + cls.base + ["logs", "--no-color", "world"],
                                 env=cls.env, timeout=60)
            cls.group_rows = p.db_exec(cls.base, cls.env,
                                       "SELECT COUNT(*) FROM tw_char.groups").strip()
            cls.online_count = int(p.db_exec(
                cls.base, cls.env,
                "SELECT COUNT(*) FROM tw_char.characters WHERE guid BETWEEN %d AND %d AND online=1"
                % (BOT_GUIDS[0], BOT_GUIDS[-1])).strip())
            telemetry = p.command(["docker", "compose"] + cls.base +
                                  ["exec", "-T", "world", "sh", "-c",
                                   "cat /state/world_processing_telemetry.log 2>/dev/null || true"],
                                  env=cls.env, timeout=60)
            (cls.evidence / "telemetry.log").write_text(telemetry, encoding="utf-8")
            stats = p.command(["docker", "stats", "--no-stream", "--format",
                               "{{.Name}}|{{.CPUPerc}}|{{.MemUsage}}"], timeout=60)
            (cls.evidence / "resources.txt").write_text(stats, encoding="utf-8")
            p.teardown_lab(cls.base, cls.env, cls.project, cls.evidence)
            cls.base = None
        except BaseException:
            if cls.base is not None:
                p.force_down(cls.base, cls.env)
            raise
        finally:
            if cls.adapter_proc is not None:
                cls.adapter_proc.terminate()
                cls.adapter_proc.wait(timeout=10)
            if cls.adapter_out is not None:
                cls.adapter_out.close()

    def test_exact_target_and_pace(self):
        lines = [line for line in self.logs.splitlines()
                 if "Playerbot: owned world login queued guid:" in line]
        self.assertEqual(len(lines), TARGET)
        for index, guid in enumerate(BOT_GUIDS[:TARGET], 1):
            self.assertIn("guid:%d active:%d target:%d" % (guid, index, TARGET), lines[index - 1])
        for guid in BOT_GUIDS[TARGET:]:
            self.assertNotIn("owned world login queued guid:%d" % guid, self.logs)

    def test_no_party_created(self):
        self.assertEqual(self.group_rows, "0")
        self.assertNotIn("party recruit accepted", self.logs)

    def test_requested_bots_stay_online(self):
        self.assertEqual(self.online_count, TARGET)

    @unittest.skipUnless(INTENT_LAB, "world-intent adapter fixture disabled")
    def test_off_duty_world_intent_round(self):
        adapter = self.adapter_log.read_text(encoding="utf-8", errors="replace")
        for guid in (BOT_GUIDS[:TARGET] if REAL_INTENT_LAB else BOT_GUIDS[:1]):
            self.assertIn("[WorldIntent] accepted bot:%d intent:" % guid, self.logs)
        if REAL_INTENT_LAB:
            self.assertGreaterEqual(adapter.count("world-intent accepted intent:"), TARGET)
            self.assertNotIn("world-intent fallback reason:", adapter)
        else:
            self.assertIn("POST /world-intent valid:1", adapter)


if __name__ == "__main__":
    unittest.main()
