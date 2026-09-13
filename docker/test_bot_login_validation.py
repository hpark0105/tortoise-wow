"""Isolated lab validation for bot login ownership checks (TW-006, contract C2/C6/C7).

Boots a fresh, port-free two-service lab (disposable database + the current
tortoise-local:dev world image) with PlayerBot.Enable=1 and a fixture roster:
one valid reserved-account bot, one human-owned character listed in the
roster, and one roster row without a character. Asserts the valid bot logs in
under its approved account identity, the invalid rows are quarantined at
load, no ownership rewrite happens, and the world stays healthy.
"""
import json
from datetime import datetime, timezone
import os
import secrets
import subprocess
import time
import unittest
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PROJECT_PATTERN = r"tortoise-bot-login-[a-f0-9]{12}"
RESERVED = 1000000000
IMAGE = "tortoise-local:dev"
MIGRATION = ROOT / "sql" / "database_updates" / "character" / "20260911174500_character.sql"
BASE_SCHEMA = ROOT / "sql" / "create_databases.sql"


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


class BotLoginValidationTests(unittest.TestCase):
    base = None
    env = None
    project = None
    evidence = None

    @classmethod
    def setUpClass(cls):
        try:
            command(["docker", "image", "inspect", IMAGE])
        except RuntimeError:
            raise unittest.SkipTest(f"{IMAGE} image not present; build it first")
        cls.project = "tortoise-bot-login-" + uuid.uuid4().hex[:12]
        cls.evidence = ROOT / "local" / (cls.project + "-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ"))
        cls.evidence.mkdir(parents=True)
        bind = lambda source, target: {"type": "bind", "source": str(ROOT / source),
                                       "target": target, "read_only": True}
        world_env = {"DB_PASSWORD": "${BOT_LAB_DB_PASSWORD:?}",
                     "PLAYERBOT_ENABLE": "1", "PLAYERBOT_MIN_BOTS": "1", "PLAYERBOT_MAX_BOTS": "2",
                     "PLAYERBOT_REFRESH": "15000", "PLAYERBOT_UPDATE_MS": "5000", "PLAYERBOT_DEBUG": "1"}
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
        # The entrypoint init already created the schema and tw_world base data;
        # rerunning create_databases.sql here would drop that data, so only the
        # migration and fixtures are applied on top.
        db_exec(cls.base, cls.env, MIGRATION.read_text(encoding="utf-8"))
        fixtures = """
 INSERT INTO tw_char.characters
   (guid, account, name, race, class, gender, level, money,
    position_x, position_y, position_z, map, orientation, zone,
    health, power1, power2, power3, power4, power5)
 VALUES
   (200001, {r1}, 'ValidBotOne', 1, 1, 0, 10, 100000,
    -8949.95, -132.493, 83.5312, 0, 0, 12, 26, 0, 0, 0, 0, 0),
   (200005, {r5}, 'ValidBotTwo', 1, 1, 0, 10, 100000,
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
   (200005, {r5}, 1, 1);
""".format(r1=RESERVED + 200001, r5=RESERVED + 200005)
        db_exec(cls.base, cls.env, fixtures)
        command(["docker", "compose"] + cls.base + ["up", "-d", "--no-deps", "world"], env=cls.env)
        deadline = time.monotonic() + 600
        last_progress = 0
        logs = ""
        while time.monotonic() < deadline:
            logs = command(["docker", "compose"] + cls.base + ["logs", "--no-color", "world"],
                           env=cls.env, timeout=60)
            (cls.evidence / "startup.log").write_text(logs, encoding="utf-8")
            if "World server is up and running!" in logs:
                break
            elapsed = int(time.monotonic() - (deadline - 600))
            if elapsed >= last_progress + 30:
                print(f"Waiting for world: {elapsed}s elapsed", flush=True)
                last_progress = elapsed
            time.sleep(2)
        else:
            raise RuntimeError("Lab world failed to become ready; inspect local startup.log")
        # With two valid bots and MinBots=1 the startup AddRandomBot logs in one
        # bot deterministically; poll briefly for the login line (session load is
        # async) and capture a final log snapshot.
        login_deadline = time.monotonic() + 60
        cls.logs = ""
        while time.monotonic() < login_deadline:
            cls.logs = command(["docker", "compose"] + cls.base + ["logs", "--no-color", "world"],
                               env=cls.env, timeout=60)
            if "[PlayerBot][Login]" in cls.logs:
                break
            time.sleep(3)
        time.sleep(3)
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
                    raise AssertionError("login lab unexpectedly published ports")
                for mount in row["Mounts"]:
                    if mount["Type"] == "volume" and mount["Name"] not in {
                            cls.project + "_database", cls.project + "_world-state"}:
                        raise AssertionError("unexpected volume in login lab")
                    if mount["Type"] == "bind" and mount["RW"]:
                        raise AssertionError("writable host mount in login lab")
        except AssertionError:
            raise
        except Exception as exc:
            print(f"login-lab: teardown verification degraded: {exc}", flush=True)
        try:
            command(["docker", "compose"] + cls.base + ["down", "--volumes"], env=cls.env, timeout=300)
        finally:
            (cls.evidence / "compose.json").write_text(
                "removed: " + datetime.now(timezone.utc).isoformat() + "\n", encoding="utf-8")

    def test_world_ready_and_healthy(self):
        self.assertIn("World server is up and running!", self.logs)
        running = command(["docker", "compose"] + self.base + ["ps", "--services", "--status", "running"],
                          env=self.env)
        self.assertIn("world", running.splitlines())

    def test_valid_bot_logs_in_with_approved_identity(self):
        login_lines = [l for l in self.logs.splitlines() if "[PlayerBot][Login]" in l]
        self.assertTrue(login_lines, f"no valid bot logged in; log: {self.evidence / 'world.log'}")
        # Every login must carry the approved account identity for its character.
        for line in login_lines:
            if "200001" in line:
                self.assertIn(f"Acc:{RESERVED + 200001}", line)
            elif "200005" in line:
                self.assertIn(f"Acc:{RESERVED + 200005}", line)
            else:
                self.fail(f"unexpected bot in login line: {line}")
        for bad in ("login rejected", "ownership mismatch", "conflicting session",
                    "duplicate login", "session load timeout"):
            self.assertNotIn(bad, self.logs, f"unexpected failure log line: {bad}")

    def test_invalid_roster_rows_quarantined(self):
        self.assertIn("roster entry 200002 has no valid ownership binding", self.logs)
        self.assertIn("roster entry 200099 references a missing character", self.logs)
        self.assertNotIn("roster entry 200001 has no valid ownership binding", self.logs)

    def test_human_record_and_ownership_untouched(self):
        account = db_exec(self.base, self.env,
                          "SELECT account FROM characters WHERE guid = 200002")
        self.assertEqual(account, "5")
        binding = db_exec(self.base, self.env,
                          "SELECT COUNT(*) FROM bot_ownership WHERE char_guid = 200002")
        self.assertEqual(binding, "0")


if __name__ == "__main__":
    unittest.main()