"""Isolated lab validation for bot controller initialization and null-entry safety (TW-008).

Boots a fresh, port-free two-service lab with PlayerBot.Enable=1 and a roster
of two valid reserved-account persistent bots. The manager-driven refresh
lottery cycles the bots through repeated login -> logout -> re-login
sequences, so the shared controller (PlayerBotAI) is attached to a player,
detached (Remove with a valid player, after which me == nullptr) and
re-attached (SetPlayer) at least twice. Asserts the world stays healthy
across the cycles, that at least two manager logouts were observed, and that
every bot whose last observed lifecycle event was a logout was saved under
its stored owner.

AC2 (teleport-ack reachability) is a code-level invariant: the teleport-ack
blocks in PlayerBotAI::UpdateAI run before the IsInWorld return, so a valid
player mid-teleport still gets the ack. This tree has no GM teleport/kick
command and the bots do not use portals, so a bot worldport cannot be
triggered deterministically from the lab.
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


class BotInitSafetyTests(unittest.TestCase):
    base = None
    env = None
    project = None
    evidence = None
    logs = ""
    world_state = ""

    @classmethod
    def setUpClass(cls):
        try:
            command(["docker", "image", "inspect", IMAGE])
        except RuntimeError:
            raise unittest.SkipTest(f"{IMAGE} image not present; build it first")
        cls.project = "tortoise-bot-init-" + uuid.uuid4().hex[:12]
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
        try:
            command(["docker", "compose"] + cls.base + ["config", "--quiet"], env=cls.env)
            command(["docker", "compose"] + cls.base + ["up", "-d", "--wait", "--wait-timeout", "600", "db"],
                    env=cls.env, timeout=660)
            fixtures = """
    INSERT INTO tw_char.characters
      (guid, account, name, race, class, gender, level, money,
       position_x, position_y, position_z, map, orientation, zone,
       health, power1, power2, power3, power4, power5)
    VALUES
      (300001, {r1}, 'InitBotOne', 1, 1, 0, 10, 100000,
       -8949.95, -132.493, 83.5312, 0, 0, 12, 26, 0, 0, 0, 0, 0),
      (300005, {r5}, 'InitBotTwo', 1, 1, 0, 10, 100000,
       -8947.95, -134.493, 83.5312, 0, 0, 12, 26, 0, 0, 0, 0, 0);
    INSERT INTO tw_char.playerbot (char_guid, chance, ai) VALUES
      (300001, 100, 'Default'),
      (300005, 100, 'Default');
    INSERT INTO tw_char.bot_ownership (char_guid, account_id, bot_type, provision_version) VALUES
      (300001, {r1}, 1, 1),
      (300005, {r5}, 1, 1);
    """.format(r1=RESERVED + 300001, r5=RESERVED + 300005)
            db_exec(cls.base, cls.env, MIGRATION.read_text(encoding="utf-8"))
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
                if cls.logged_in_guids(cls.logs):
                    break
                time.sleep(3)
            # Cycle window: need at least two manager logouts so the controller is
            # detached (Remove) and re-attached (SetPlayer) at least twice. With a
            # 10s refresh, cycles land every 10-30s; 600s leaves a wide margin.
            cycle_deadline = time.monotonic() + 600
            while time.monotonic() < cycle_deadline:
                cls.logs = command(["docker", "compose"] + cls.base + ["logs", "--no-color", "world"],
                                   env=cls.env, timeout=60)
                if cls.logout_count(cls.logs) >= 2:
                    break
                time.sleep(5)
            # The world must still be alive after the cycles; a null dereference in
            # the controller would crash it and the container would stop.
            ps_out = command(["docker", "compose"] + cls.base + ["ps", "--format", "json"],
                             env=cls.env, timeout=60)
            ps = [json.loads(line) for line in ps_out.splitlines() if line.strip()]
            for row in ps:
                if row.get("Service") == "world":
                    cls.world_state = row.get("State", "")
            # Graceful world shutdown (SIGTERM) after the observed cycles.
            command(["docker", "compose"] + cls.base + ["stop", "--timeout", "90", "world"], env=cls.env, timeout=180)
            cls.logs = command(["docker", "compose"] + cls.base + ["logs", "--no-color", "world"],
                               env=cls.env, timeout=60)
            (cls.evidence / "world.log").write_text(cls.logs, encoding="utf-8")
        except BaseException:
            # Self-heal: remove the generated lab on any boot failure so a
            # setUpClass error cannot leave orphaned containers or volumes.
            cls._teardown()
            raise

    @classmethod
    def _teardown(cls):
        if cls.base is None:
            return
        try:
            command(["docker", "compose"] + cls.base + ["down", "--volumes"], env=cls.env, timeout=300)
        except Exception as exc:
            print(f"init-lab: teardown degraded: {exc}", flush=True)

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
                    raise AssertionError("init lab unexpectedly published ports")
        except AssertionError:
            raise
        except Exception as exc:
            print(f"init-lab: teardown verification degraded: {exc}", flush=True)
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

    def test_world_ready_and_survives_cycles(self):
        self.assertIn("World server is up and running!", self.logs,
                      "world never became ready; log: " + str(self.evidence / "world.log"))
        self.assertEqual(self.world_state, "running",
                         "world container not running after bot cycles (crash?); log: "
                         + str(self.evidence / "world.log"))

    def test_ac1_controller_cycles_without_crash(self):
        self.assertGreaterEqual(self.logout_count(self.logs), 2,
                                "expected >=2 manager logouts (attach/detach/re-attach); log: "
                                + str(self.evidence / "world.log"))
        self.assertGreaterEqual(len(self.logged_in_guids(self.logs)), 2,
                                "expected >=2 login events across the cycles; log: "
                                + str(self.evidence / "world.log"))

    def test_logged_out_bots_saved_under_owner(self):
        for guid in self.last_logged_out_guids():
            row = self.row(int(guid))
            self.assertIsNotNone(row, f"character {guid} missing")
            account, logout_time = int(row[0]), int(row[1])
            self.assertEqual(account, RESERVED + int(guid), f"owner rewritten for {guid}")
            self.assertGreater(logout_time, 0,
                               f"bot {guid} last event was logout but was never saved; log: "
                               + str(self.evidence / "world.log"))

    @staticmethod
    def logged_in_guids(logs):
        return re.findall(r"\[PlayerBot\]\[Login\]\s*'[^']*'\s+GUID:(\d+)", logs)

    @staticmethod
    def logged_out_guids(logs):
        return re.findall(r"\[PlayerBot\]\[Logout\]\s*'[^']*'\s+GUID:(\d+)", logs)

    @classmethod
    def logout_count(cls, logs):
        return len(cls.logged_out_guids(logs))

    def last_logged_out_guids(self):
        events = re.findall(r"\[PlayerBot\]\[(Login|Logout)\]\s*'[^']*'\s+GUID:(\d+)", self.logs)
        last = {}
        for kind, guid in events:
            last[guid] = kind
        return [guid for guid, kind in last.items() if kind == "Logout"]


if __name__ == "__main__":
    unittest.main()
