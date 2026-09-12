"""Isolated lab validation for the per-bot save guard (TW-007, contract C4).

Boots a fresh, port-free two-service lab with PlayerBot.Enable=1 and a roster
of two valid reserved-account persistent bots plus a human-owned character
with a mismatched binding and an orphan roster row. The manager-driven
refresh lottery then logs an online bot out; that logout runs the normal
LogoutPlayer -> SaveToDB path under the new saveability guard, after which
the world is shut down gracefully (SIGTERM). Asserts the persistent bots'
earned state was written under their stored owners while the quarantined
human-owned character was never saved and its record is unchanged.
"""
import json
from datetime import datetime, timezone
import os
import re
import secrets
import subprocess
import time
import unittest
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PROJECT_PATTERN = r"tortoise-bot-save-[a-f0-9]{12}"
RESERVED = 1000000000
IMAGE = "tortoise-local:dev"
MIGRATION = ROOT / "sql" / "database_updates" / "character" / "20260911174500_character.sql"


def command(args, *, env=None, timeout=120, input_text=None):
    result = subprocess.run(args, cwd=ROOT, env=env, capture_output=True, timeout=timeout,
                            input=input_text.encode("utf-8") if input_text is not None else None)
    if result.returncode:
        tail = result.stderr.decode("utf-8", errors="replace").strip()[-400:]
        raise RuntimeError(f"command failed ({result.returncode}): {args[0]}: {tail}")
    return result.stdout.decode("utf-8", errors="replace").strip()


def db_exec(base, env, query, flags="--batch --skip-column-names", database="tw_char"):
    script = 'export MYSQL_PWD="$MARIADB_ROOT_PASSWORD"\nmariadb --user=root ' + flags + (" " + database if database else "")
    return command(["docker", "compose"] + base + ["exec", "-T", "db", "bash"],
                   env=env, input_text=script + "\n" + query, timeout=300)


class BotSaveGuardTests(unittest.TestCase):
    base = None
    env = None
    project = None
    evidence = None
    logs = ""

    @classmethod
    def setUpClass(cls):
        try:
            command(["docker", "image", "inspect", IMAGE])
        except RuntimeError:
            raise unittest.SkipTest(f"{IMAGE} image not present; build it first")
        cls.project = "tortoise-bot-save-" + uuid.uuid4().hex[:12]
        cls.evidence = ROOT / "local" / (cls.project + "-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ"))
        cls.evidence.mkdir(parents=True)
        bind = lambda source, target: {"type": "bind", "source": str(ROOT / source),
                                       "target": target, "read_only": True}
        world_env = {"DB_PASSWORD": "${BOT_LAB_DB_PASSWORD:?}",
                     "PLAYERBOT_ENABLE": "1", "PLAYERBOT_MIN_BOTS": "1", "PLAYERBOT_MAX_BOTS": "2",
                     "PLAYERBOT_REFRESH": "10000", "PLAYERBOT_UPDATE_MS": "5000", "PLAYERBOT_DEBUG": "1"}
        (cls.evidence / "compose.json").write_text(json.dumps({
            "services": {
                "db": {
                    "image": "mariadb:10.11",
                    "environment": {"MARIADB_ROOT_PASSWORD": "${BOT_LAB_ROOT_PASSWORD:?}",
                                    "DB_PASSWORD": "${BOT_LAB_DB_PASSWORD:?}"},
                    "volumes": ["database:/var/lib/mysql", bind("sql", "/bootstrap/sql"),
                                bind("docker/init-db.sh", "/docker-entrypoint-initdb.d/10-tortoise.sh")],
                    "healthcheck": {"test": ["CMD", "healthcheck.sh", "--connect", "--innodb_initialized"],
                                    "interval": "5s", "timeout": "5s", "retries": 120,
                                    "start_period": "5m"},
                    "stop_grace_period": "2m",
                },
                "world": {
                    "image": IMAGE, "command": ["world"],
                    "environment": world_env,
                    "volumes": ["world-state:/state", bind("data", "/data")],
                    "stdin_open": True, "tty": True, "stop_grace_period": "2m",
                },
            },
            "volumes": {"database": {}, "world-state": {}},
        }, indent=2), encoding="utf-8")
        cls.env = dict(os.environ, BOT_LAB_ROOT_PASSWORD=secrets.token_hex(24),
                       BOT_LAB_DB_PASSWORD=secrets.token_hex(24))
        cls.base = ["-f", str(cls.evidence / "compose.json"), "-p", cls.project]
        command(["docker", "compose"] + cls.base + ["config", "--quiet"], env=cls.env)
        command(["docker", "compose"] + cls.base + ["up", "-d", "--wait", "--wait-timeout", "600", "db"],
                env=cls.env, timeout=660)
        db_exec(cls.base, cls.env, MIGRATION.read_text(encoding="utf-8"))
        fixtures = """
INSERT INTO tw_char.characters
  (guid, account, name, race, class, gender, level, money,
   position_x, position_y, position_z, map, orientation, zone,
   health, power1, power2, power3, power4, power5)
VALUES
  (200001, {r1}, 'SaveBotOne', 1, 1, 0, 10, 100000,
   -8949.95, -132.493, 83.5312, 0, 0, 12, 26, 0, 0, 0, 0, 0),
  (200005, {r5}, 'SaveBotTwo', 1, 1, 0, 10, 100000,
   -8947.95, -134.493, 83.5312, 0, 0, 12, 26, 0, 0, 0, 0, 0),
  (200002, 5, 'HumanOwned', 1, 1, 0, 10, 100000,
   -8949.95, -132.493, 83.5312, 0, 0, 12, 26, 0, 0, 0, 0, 0);
INSERT INTO tw_char.playerbot (char_guid, chance, ai) VALUES
  (200001, 100, 'Default'),
  (200005, 100, 'Default'),
  (200002, 10, 'Default'),
  (200099, 10, 'Default');
INSERT INTO tw_char.bot_ownership (char_guid, account_id, bot_type, provision_version) VALUES
  (200001, {r1}, 1, 1),
  (200005, {r5}, 1, 1),
  (200002, {r2}, 1, 1);
""".format(r1=RESERVED + 200001, r5=RESERVED + 200005, r2=RESERVED + 200002)
        db_exec(cls.base, cls.env, fixtures)
        command(["docker", "compose"] + cls.base + ["up", "-d", "--no-deps", "world"], env=cls.env)
        deadline = time.monotonic() + 600
        logs = ""
        while time.monotonic() < deadline:
            logs = command(["docker", "compose"] + cls.base + ["logs", "--no-color", "world"],
                           env=cls.env, timeout=60)
            (cls.evidence / "startup.log").write_text(logs, encoding="utf-8")
            if "World server is up and running!" in logs:
                break
            time.sleep(2)
        else:
            raise RuntimeError("Lab world failed to become ready; inspect local startup.log")
        login_deadline = time.monotonic() + 60
        while time.monotonic() < login_deadline:
            cls.logs = command(["docker", "compose"] + cls.base + ["logs", "--no-color", "world"],
                               env=cls.env, timeout=60)
            if cls.logged_in_guids(cls.logs) & {"200001", "200005"}:
                break
            time.sleep(3)
        # The manager-driven refresh lottery logs an online roster bot out
        # (OnBotLogout -> PB_STATE_OFFLINE); the next session update then runs
        # LogoutPlayer -> SaveToDB. Refresh is shortened to 10s so a delete is
        # rolled every 10s (2/3 chance per tick with two bots online); at the
        # default 60s a whole 600s window could elapse with no logout under
        # suite CPU contention, which made this test flaky. Wait for the
        # observed logout so the save assertion stays deterministic.
        logout_deadline = time.monotonic() + 600
        while time.monotonic() < logout_deadline:
            cls.logs = command(["docker", "compose"] + cls.base + ["logs", "--no-color", "world"],
                               env=cls.env, timeout=60)
            if cls.logged_out_guids(cls.logs) & {"200001", "200005"}:
                break
            time.sleep(5)
        # Graceful world shutdown (SIGTERM) after the observed logout.
        command(["docker", "compose"] + cls.base + ["stop", "--timeout", "90", "world"], env=cls.env, timeout=180)
        cls.logs = command(["docker", "compose"] + cls.base + ["logs", "--no-color", "world"],
                           env=cls.env, timeout=60)
        (cls.evidence / "world.log").write_text(cls.logs, encoding="utf-8")

    @classmethod
    def tearDownClass(cls):
        if cls.base is None:
            return
        try:
            ids = command(["docker", "compose"] + cls.base + ["ps", "-a", "-q"], env=cls.env).splitlines()
            for row in (json.loads(command(["docker", "inspect", *ids])) if ids else []):
                labels = row["Config"]["Labels"]
                if labels.get("com.docker.compose.project") != cls.project:
                    raise AssertionError("refusing to remove container from another project")
                if row["HostConfig"].get("PortBindings"):
                    raise AssertionError("save lab unexpectedly published ports")
        except AssertionError:
            raise
        except Exception as exc:
            print(f"save-lab: teardown verification degraded: {exc}", flush=True)
        try:
            command(["docker", "compose"] + cls.base + ["down", "--volumes"], env=cls.env, timeout=300)
        finally:
            (cls.evidence / "compose.json").write_text(
                "removed: " + datetime.now(timezone.utc).isoformat() + "\n", encoding="utf-8")

    def row(self, guid):
        out = db_exec(self.base, self.env,
                      f"SELECT account, logout_time, totaltime FROM characters WHERE guid = {guid}")
        if not out:
            return None
        return out.split("\t")

    def test_valid_bot_full_lifecycle_observed(self):
        logged_in = self.logged_in_guids(self.logs)
        logged_out = self.logged_out_guids(self.logs)
        self.assertTrue(logged_in & {"200001", "200005"},
                        "no valid persistent bot logged in; log: " + str(self.evidence / "world.log"))
        self.assertTrue(logged_out & {"200001", "200005"},
                        "no valid persistent bot logged out before shutdown; log: " + str(self.evidence / "world.log"))
        self.assertIn("roster entry 200002 has no valid ownership binding", self.logs)

    @staticmethod
    def logged_in_guids(logs):
        return set(re.findall(r"\[PlayerBot\]\[Login\]\s*'[^']*'\s+GUID:(\d+)", logs))

    @staticmethod
    def logged_out_guids(logs):
        return set(re.findall(r"\[PlayerBot\]\[Logout\]\s*'[^']*'\s+GUID:(\d+)", logs))

    def test_ac1_persistent_bots_saved_under_stored_owner(self):
        logged_out = self.logged_out_guids(self.logs)
        self.assertTrue(logged_out & {"200001", "200005"},
                        "no logout observed; cannot verify save; log: " + str(self.evidence / "world.log"))
        for guid, owner in ((200001, RESERVED + 200001), (200005, RESERVED + 200005)):
            row = self.row(guid)
            self.assertIsNotNone(row, f"character {guid} missing")
            account, logout_time, totaltime = int(row[0]), int(row[1]), int(row[2])
            self.assertEqual(account, owner, f"owner rewritten for {guid}")
            if str(guid) in logged_out:
                self.assertGreater(logout_time, 0,
                                   f"persistent bot {guid} logged out but never saved; log: {self.evidence / 'world.log'}")

    def test_ac2_quarantined_human_character_never_saved(self):
        row = self.row(200002)
        self.assertIsNotNone(row, "character 200002 missing")
        account, logout_time, _ = int(row[0]), int(row[1]), int(row[2])
        self.assertEqual(account, 5, "human-owned character was reassigned")
        self.assertEqual(logout_time, 0, "quarantined human-owned character was saved")


if __name__ == "__main__":
    unittest.main()