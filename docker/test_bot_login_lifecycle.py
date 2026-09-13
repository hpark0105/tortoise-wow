"""Isolated lab validation for lifecycle-safe bot logins (TW-009).

Boots a fresh, port-free two-service lab with PlayerBot.Enable=1 and a roster
of two valid reserved-account persistent bots. The manager-driven refresh
lottery cycles the bots through repeated login -> logout -> re-login
sequences. With the TW-009 state machine (single ONLINE transition on actual
in-world entry, one queued login per session, generation-stamped async login
completions) the test asserts:

- AC1 (observable form): for every bot, login/logout events never overlap
  (no two consecutive logins for the same GUID - a repeated add request can
  never produce a second session on top of an active one) and no duplicate
  login was rejected by the gates in normal operation.
- AC2 (no false positives): the generation guards never reject a valid login
  in normal operation (no stale-login-completion or stale-in-world-entry
  rejections in the log); the guards themselves are code-level invariants
  because a forced stale completion needs an in-flight load at the exact
  moment of timeout + retry, which the lab cannot deterministically stage.
- TW-007 regression: every bot whose last observed event was a logout was
  saved under its stored owner.
MVP-002 (KAP-552) adds BotStaleCompletionTests: a single-bot lab arms the
config-gated stale probe (PLAYERBOT_TEST_STALE_LOGIN), which logs the bot
out (generation 1), re-logs it in (generation 2), and delivers a synthetic
generation-1 login completion through the normal async callback path; the
stale-completion guard must reject it and preserve the generation-2
session. The default-config lifecycle lab above additionally proves the
probe hook stays inert without PLAYERBOT_TEST_STALE_LOGIN.
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


class BotLoginLifecycleTests(unittest.TestCase):
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
        cls.project = "tortoise-bot-lc-" + uuid.uuid4().hex[:12]
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
  (400001, {r1}, 'LcBotOne', 1, 1, 0, 10, 100000,
   -8949.95, -132.493, 83.5312, 0, 0, 12, 26, 0, 0, 0, 0, 0),
  (400005, {r5}, 'LcBotTwo', 1, 1, 0, 10, 100000,
   -8947.95, -134.493, 83.5312, 0, 0, 12, 26, 0, 0, 0, 0, 0);
INSERT INTO tw_char.playerbot (char_guid, chance, ai) VALUES
  (400001, 100, 'Default'),
  (400005, 100, 'Default');
INSERT INTO tw_char.bot_ownership (char_guid, account_id, bot_type, provision_version) VALUES
  (400001, {r1}, 1, 1),
  (400005, {r5}, 1, 1);
""".format(r1=RESERVED + 400001, r5=RESERVED + 400005)
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
            # Cycle window: need at least two manager logouts so each bot's
            # session lifecycle (create -> entry -> logout -> re-create) runs.
            cycle_deadline = time.monotonic() + 600
            while time.monotonic() < cycle_deadline:
                cls.logs = command(["docker", "compose"] + cls.base + ["logs", "--no-color", "world"],
                                   env=cls.env, timeout=60)
                if cls.logout_count(cls.logs) >= 2:
                    break
                time.sleep(5)
            ps_out = command(["docker", "compose"] + cls.base + ["ps", "--format", "json"],
                             env=cls.env, timeout=60)
            for row in (json.loads(line) for line in ps_out.splitlines() if line.strip()):
                if row.get("Service") == "world":
                    cls.world_state = row.get("State", "")
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
            print(f"lc-lab: teardown degraded: {exc}", flush=True)

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
                    raise AssertionError("lifecycle lab unexpectedly published ports")
        except AssertionError:
            raise
        except Exception as exc:
            print(f"lc-lab: teardown verification degraded: {exc}", flush=True)
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

    def lifecycle_events(self):
        return re.findall(r"\[PlayerBot\]\[(Login|Logout)\]\s*'[^']*'\s+GUID:(\d+)", self.logs)

    def test_world_ready_and_survives_cycles(self):
        self.assertIn("World server is up and running!", self.logs,
                      "world never became ready; log: " + str(self.evidence / "world.log"))
        self.assertEqual(self.world_state, "running",
                         "world container not running after bot cycles (crash?); log: "
                         + str(self.evidence / "world.log"))

    def test_ac1_no_overlapping_sessions_per_bot(self):
        events = self.lifecycle_events()
        self.assertGreaterEqual(len([e for e in events if e[0] == "Logout"]), 2,
                                "expected >=2 manager logouts across the cycles; log: "
                                + str(self.evidence / "world.log"))
        last = {}
        for kind, guid in events:
            if kind == "Login" and last.get(guid) == "Login":
                self.fail(f"overlapping sessions for bot {guid} (login before previous logout); log: "
                          + str(self.evidence / "world.log"))
            last[guid] = kind
        self.assertNotIn("duplicate login rejected", self.logs,
                         "duplicate login gate fired in normal operation; log: "
                         + str(self.evidence / "world.log"))

    def test_ac2_no_false_stale_rejections(self):
        self.assertNotIn("stale login completion", self.logs,
                         "stale-completion guard rejected a valid login; log: "
                         + str(self.evidence / "world.log"))
        self.assertNotIn("stale in-world entry", self.logs,
                         "in-world-entry guard rejected a valid entry; log: "
                         + str(self.evidence / "world.log"))
        self.assertNotIn("unexpected in-world entry", self.logs,
                         "in-world entry seen in an unexpected state; log: "
                         + str(self.evidence / "world.log"))

    def test_logged_out_bots_saved_under_owner(self):
        last = {}
        for kind, guid in self.lifecycle_events():
            last[guid] = kind
        for guid, kind in last.items():
            if kind != "Logout":
                continue
            row = self.row(int(guid))
            self.assertIsNotNone(row, f"character {guid} missing")
            account, logout_time = int(row[0]), int(row[1])
            self.assertEqual(account, RESERVED + int(guid), f"owner rewritten for {guid}")
            self.assertGreater(logout_time, 0,
                               f"bot {guid} last event was logout but was never saved; log: "
                               + str(self.evidence / "world.log"))

    def test_mvp002_hook_disabled_by_default(self):
        self.assertNotIn("stale-probe", self.logs,
                         "stale-completion probe ran although PLAYERBOT_TEST_STALE_LOGIN "
                         "was not set; log: " + str(self.evidence / "world.log"))

    @staticmethod
    def logged_in_guids(logs):
        return re.findall(r"\[PlayerBot\]\[Login\]\s*'[^']*'\s+GUID:(\d+)", logs)

    @staticmethod
    def logged_out_guids(logs):
        return re.findall(r"\[PlayerBot\]\[Logout\]\s*'[^']*'\s+GUID:(\d+)", logs)

    @classmethod
    def logout_count(cls, logs):
        return len(cls.logged_out_guids(logs))



class BotStaleCompletionTests(unittest.TestCase):
    """MVP-002 (KAP-552 / TW-009): deterministic stale login-completion rejection.

    One persistent bot (guid 500001) is armed with the lab-only stale probe
    (PLAYERBOT_TEST_STALE_LOGIN). The probe logs the generation-1 bot out,
    re-logs it in as generation 2, then delivers a synthetic generation-1
    login completion through the normal async callback path. The guard in
    CharacterHandler::HandlePlayerLoginCallback must reject the stale
    completion ("stale login completion ... current session preserved") and
    the generation-2 session must remain authoritative: no crash, no
    re-login, no in-world re-entry, world still running. The manager lottery
    is kept out of the window (REFRESH=600000) so only the probe drives the
    bot lifecycle.
    """
    base = None
    env = None
    project = None
    evidence = None
    logs = ""
    world_state = ""
    GUID = "500001"

    @classmethod
    def setUpClass(cls):
        try:
            command(["docker", "image", "inspect", IMAGE])
        except RuntimeError:
            raise unittest.SkipTest(f"{IMAGE} image not present; build it first")
        cls.project = "tortoise-bot-stale-" + uuid.uuid4().hex[:12]
        cls.evidence = ROOT / "local" / (cls.project + "-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ"))
        cls.evidence.mkdir(parents=True)
        bind = lambda source, target: {"type": "bind", "source": str(ROOT / source),
                                       "target": target, "read_only": True}
        world_env = {"DB_PASSWORD": "${BOT_LAB_DB_PASSWORD:?}",
                     "PLAYERBOT_ENABLE": "1", "PLAYERBOT_MIN_BOTS": "1", "PLAYERBOT_MAX_BOTS": "1",
                     "PLAYERBOT_REFRESH": "600000", "PLAYERBOT_UPDATE_MS": "5000", "PLAYERBOT_DEBUG": "1",
                     "PLAYERBOT_TEST_LOGIN": cls.GUID, "PLAYERBOT_TEST_STALE_LOGIN": cls.GUID}
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
  (500001, {acc}, 'Stalebot', 1, 1, 0, 10, 100000,
   -8949.95, -132.493, 83.5312, 0, 0, 12, 26, 0, 0, 0, 0, 0);
INSERT INTO tw_char.playerbot (char_guid, chance, ai) VALUES
  (500001, 100, 'Default');
INSERT INTO tw_char.bot_ownership (char_guid, account_id, bot_type, provision_version) VALUES
  (500001, {acc}, 1, 1);
""".format(acc=RESERVED + 500001)
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
            reject_deadline = time.monotonic() + 600
            while time.monotonic() < reject_deadline:
                cls.logs = command(["docker", "compose"] + cls.base + ["logs", "--no-color", "world"],
                                   env=cls.env, timeout=60)
                if "stale login completion for " + cls.GUID in cls.logs:
                    break
                time.sleep(3)
            else:
                (cls.evidence / "world.log").write_text(cls.logs, encoding="utf-8")
                raise RuntimeError("stale-completion rejection never observed; inspect local world.log")
            # Settle so any (incorrect) follow-on handling lands in the log.
            time.sleep(10)
            ps_out = command(["docker", "compose"] + cls.base + ["ps", "--format", "json"],
                             env=cls.env, timeout=60)
            for row in (json.loads(line) for line in ps_out.splitlines() if line.strip()):
                if row.get("Service") == "world":
                    cls.world_state = row.get("State", "")
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
            print(f"stale-lab: teardown degraded: {exc}", flush=True)

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
                    raise AssertionError("stale lab unexpectedly published ports")
        except AssertionError:
            raise
        except Exception as exc:
            print(f"stale-lab: teardown verification degraded: {exc}", flush=True)
        try:
            command(["docker", "compose"] + cls.base + ["down", "--volumes"], env=cls.env, timeout=300)
        finally:
            (cls.evidence / "compose.json").write_text(
                "removed: " + datetime.now(timezone.utc).isoformat() + "\n", encoding="utf-8")

    def bot_events(self):
        return [kind for kind, guid in
                re.findall(r"\[PlayerBot\]\[(Login|Logout)\]\s*'[^']*'\s+GUID:(\d+)", self.logs)
                if guid == self.GUID]

    def test_lab_mechanics(self):
        self.assertIn("World server is up and running!", self.logs,
                      "world never became ready; log: " + str(self.evidence / "world.log"))
        self.assertIn("stale-probe armed for " + self.GUID, self.logs,
                      "probe never armed (config not rendered?); log: "
                      + str(self.evidence / "world.log"))
        self.assertEqual(self.bot_events(), ["Login", "Logout", "Login"],
                         "expected gen-1 login, probe logout, gen-2 login; log: "
                         + str(self.evidence / "world.log"))

    def test_s1_stale_completion_rejected_gen2_authoritative(self):
        m = re.search(r"stale login completion for " + self.GUID
                      + r" \(gen (\d+) != (\d+)\); current session preserved", self.logs)
        self.assertIsNotNone(m,
                             "stale completion was not rejected with the preservation log; log: "
                             + str(self.evidence / "world.log"))
        old_gen, cur_gen = int(m.group(1)), int(m.group(2))
        self.assertEqual(cur_gen, old_gen + 1,
                         "injected completion must be exactly one generation old; log: "
                         + str(self.evidence / "world.log"))
        after = self.logs[m.end():]
        for needle in ("[CRASH] HandlePlayerLogin", "stale in-world entry",
                       "unexpected in-world entry", "stale login completion for " + self.GUID,
                       "[PlayerBot][Login]"):
            self.assertNotIn(needle, after,
                             "unexpected %r after the stale rejection; log: " % needle
                             + str(self.evidence / "world.log"))
        self.assertEqual(self.world_state, "running",
                         "world not running after the stale-completion probe (crash?); log: "
                         + str(self.evidence / "world.log"))


if __name__ == "__main__":
    unittest.main()
