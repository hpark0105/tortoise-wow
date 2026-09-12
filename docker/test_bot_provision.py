"""Isolated lab validation for idempotent, resumable bot provisioning (TW-010).

Boots a fresh, port-free two-service lab (disposable MariaDB + the current
tortoise-local:dev world image) with PlayerBot.Enable=1 and a PlayerBot.Provision
identity. The world provisions that bot at load: a reserved-range synthetic
account (>= 1e9, no account row per contract C5), a core-allocated character guid,
a native Human Warrior created from the configured starting state, the roster row, and the
bot_ownership binding with provision_version=2 (contract C6 / section 5a).

Two scenarios are validated:

- AC1 (idempotent, from empty, run twice): an empty lab is provisioned on the
  first world start and the world is restarted so provisioning runs a second
  time; exactly one account/character/binding/roster row must exist and the
  second run must be a no-op.
- AC2 (resumable, no unrelated deletion): a pre-seeded orphan character
  (character present, no roster row and no binding - the state C6 allows after a
  crash) plus an unrelated fully-provisioned bot; provisioning completes the
  orphan in place (same guid/account, no duplicate) and leaves the unrelated
  bot's records intact.
- R1 (review 2026-09-11): a fresh provision must not reuse a reserved account
  already owned by an unbound orphan character; resuming the orphan keeps its
  identity and leaves the fresh bot untouched.
- R2 (review 2026-09-11): provisioning a character whose binding is mismatched
  or on an unsupported version is rejected without changing character,
  binding, or roster data, whether or not a roster row exists.
- R3 (review 2026-09-11): the temporary login overload logs in an existing
  character, rejects a duplicate request, and fails cleanly for a guid with no
  character (config-gated runtime probe, lab only).
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
RESERVED = 1000000000
IMAGE = "tortoise-local:dev"
MIGRATION = ROOT / "sql" / "database_updates" / "character" / "20260911174500_character.sql"
PROVISION_MIGRATION = ROOT / "sql" / "database_updates" / "character" / "20260912120000_character.sql"
PROVISION_NAME = "ProvisionBot"
UNRELATED_NAME = "UnrelatedBot"
UNRELATED_GUID = 500001
ORPHAN_GUID = 500005
REV_ORPHAN_NAME = "Revorphan"
REV_ORPHAN_GUID = 500010
REV_FRESH_NAME = "Revfresh"
MISSING_PROBE_GUID = 509999
MISFIT1_NAME = "Revmisfit"
MISFIT1_GUID = 500020
MISFIT2_NAME = "Revmisfitb"
MISFIT2_GUID = 500021
BADVER_NAME = "Revbadver"
BADVER_GUID = 500022
MISFIT1_BOUND = RESERVED + 500031
MISFIT2_BOUND = RESERVED + 500032


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


def db_int(base, env, query):
    out = db_exec(base, env, query).strip()
    return int(out) if out else 0


def world_env_for(name):
    return {"DB_PASSWORD": "${BOT_LAB_DB_PASSWORD:?}",
            "PLAYERBOT_ENABLE": "1", "PLAYERBOT_MIN_BOTS": "1", "PLAYERBOT_MAX_BOTS": "2",
            "PLAYERBOT_REFRESH": "10000", "PLAYERBOT_UPDATE_MS": "5000", "PLAYERBOT_DEBUG": "1",
            "PLAYERBOT_PROVISION": name}


def write_compose(evidence, world_env):
    bind = lambda source, target: {"type": "bind", "source": str(ROOT / source),
                                   "target": target, "read_only": True}
    (evidence / "compose.json").write_text(json.dumps({
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


def wait_for(base, env, pred, deadline=600):
    end = time.monotonic() + deadline
    logs = ""
    while time.monotonic() < end:
        logs = command(["docker", "compose"] + base + ["logs", "--no-color", "world"], env=env, timeout=60)
        if pred(logs):
            return logs
        time.sleep(3)
    raise RuntimeError("world did not reach the expected state")


def boot_lab(project, evidence, world_env, seed_sql=None):
    write_compose(evidence, world_env)
    env = dict(os.environ, BOT_LAB_ROOT_PASSWORD=secrets.token_hex(24),
               BOT_LAB_DB_PASSWORD=secrets.token_hex(24))
    base = ["-f", str(evidence / "compose.json"), "-p", project]
    command(["docker", "compose"] + base + ["config", "--quiet"], env=env)
    command(["docker", "compose"] + base + ["up", "-d", "--wait", "--wait-timeout", "600", "db"],
            env=env, timeout=660)
    db_exec(base, env, MIGRATION.read_text(encoding="utf-8"))
    db_exec(base, env, PROVISION_MIGRATION.read_text(encoding="utf-8"))
    if seed_sql:
        db_exec(base, env, seed_sql)
    return base, env


def teardown_lab(base, env, project, evidence):
    try:
        ids = command(["docker", "compose"] + base + ["ps", "-a", "-q"], env=env).splitlines()
        for row in (json.loads(command(["docker", "inspect", *ids])) if ids else []):
            labels = row["Config"]["Labels"]
            if labels.get("com.docker.compose.project") != project:
                raise AssertionError("refusing to remove container from another project")
            if row["HostConfig"].get("PortBindings"):
                raise AssertionError("provision lab unexpectedly published ports")
    except AssertionError:
        raise
    except Exception as exc:
        print(f"prov-lab: teardown verification degraded: {exc}", flush=True)
    try:
        command(["docker", "compose"] + base + ["down", "--volumes"], env=env, timeout=300)
    finally:
        (evidence / "compose.json").write_text(
            "removed: " + datetime.now(timezone.utc).isoformat() + "\n", encoding="utf-8")


def force_down(base, env):
    try:
        command(["docker", "compose"] + base + ["down", "--volumes"], env=env, timeout=300)
    except Exception as exc:
        print(f"prov-lab: self-heal degraded: {exc}", flush=True)


class BotProvisionIdempotentTests(unittest.TestCase):
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
        cls.project = "tortoise-bot-prov-" + uuid.uuid4().hex[:12]
        cls.evidence = ROOT / "local" / (cls.project + "-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ"))
        cls.evidence.mkdir(parents=True)
        try:
            cls.base, cls.env = boot_lab(cls.project, cls.evidence, world_env_for(PROVISION_NAME))
            # Run 1: provision from an empty lab.
            command(["docker", "compose"] + cls.base + ["up", "-d", "--no-deps", "world"], env=cls.env)
            logs = wait_for(cls.base, cls.env, lambda l: "World server is up and running!" in l)
            # Run 2: restart the same world so provisioning runs again.
            command(["docker", "compose"] + cls.base + ["stop", "world"], env=cls.env, timeout=180)
            command(["docker", "compose"] + cls.base + ["start", "world"], env=cls.env, timeout=180)
            cls.logs = wait_for(cls.base, cls.env,
                                lambda l: l.count("World server is up and running!") >= 2)
            (cls.evidence / "world.log").write_text(cls.logs, encoding="utf-8")
            cls.guid = db_exec(cls.base, cls.env,
                               f"SELECT guid FROM characters WHERE name = '{PROVISION_NAME}'").strip()
        except BaseException:
            if cls.base is not None:
                force_down(cls.base, cls.env)
            raise

    @classmethod
    def tearDownClass(cls):
        if cls.base is None:
            return
        teardown_lab(cls.base, cls.env, cls.project, cls.evidence)

    def test_world_ready(self):
        self.assertIn("World server is up and running!", self.logs,
                      "world never became ready; log: " + str(self.evidence / "world.log"))

    def test_ac1_exactly_one_after_two_runs(self):
        self.assertEqual(db_int(self.base, self.env,
                                f"SELECT COUNT(*) FROM characters WHERE name = '{PROVISION_NAME}'"), 1,
                         "exactly one character for the provisioned identity; log: " + str(self.evidence / "world.log"))
        guid = self.guid
        account = int(db_exec(self.base, self.env,
                              f"SELECT account FROM characters WHERE name = '{PROVISION_NAME}'").strip())
        self.assertGreaterEqual(account, RESERVED, "account must be in the reserved range")
        self.assertEqual(db_int(self.base, self.env, f"SELECT COUNT(*) FROM bot_ownership WHERE char_guid = {guid}"), 1)
        self.assertEqual(db_int(self.base, self.env, f"SELECT COUNT(*) FROM playerbot WHERE char_guid = {guid}"), 1)
        self.assertEqual(db_int(self.base, self.env,
                                f"SELECT MAX(provision_version) FROM bot_ownership WHERE char_guid = {guid}"), 2)
        bound = int(db_exec(self.base, self.env,
                            f"SELECT account_id FROM bot_ownership WHERE char_guid = {guid}").strip())
        self.assertEqual(bound, account, "binding must match the character owner")

    def test_ac1_second_run_is_idempotent_noop(self):
        self.assertEqual(self.logs.count(f"created native character '{PROVISION_NAME}'"), 1,
                         "the bot must be created exactly once across both runs")
        self.assertGreaterEqual(self.logs.count("idempotent no-op"), 1,
                                "the second run must be an idempotent no-op")

    def test_native_character_matches_core_starting_state(self):
        actual = db_exec(
            self.base, self.env,
            f"SELECT race,class,gender,level,money,map,zone,ROUND(position_x,3),"
            f"ROUND(position_y,3),ROUND(position_z,3) FROM characters WHERE guid={self.guid}")
        expected = db_exec(
            self.base, self.env,
            # Core creation persists zone=0; normal login derives/caches zone.
            "SELECT 1,1,0,1,0,map,0,ROUND(position_x,3),ROUND(position_y,3),"
            "ROUND(position_z,3) FROM playercreateinfo WHERE race=1 AND class=1",
            database="tw_world")
        self.assertEqual(actual, expected, "native bot must use the core Human Warrior starting state")
        self.assertGreater(db_int(self.base, self.env,
                                  f"SELECT health FROM characters WHERE guid={self.guid}"), 0)

    def test_native_spells_actions_items_and_homebind_match_create_info(self):
        spells = db_exec(self.base, self.env,
                         f"SELECT spell FROM character_spell WHERE guid={self.guid} ORDER BY spell")
        # Core default race/class spells are derived by LearnDefaultSpells and
        # intentionally marked dependent, so native creation does not duplicate
        # them in character_spell.
        self.assertEqual(spells, "")
        self.assertGreater(int(db_exec(self.base, self.env,
                                       "SELECT COUNT(*) FROM playercreateinfo_spell WHERE race=1 AND class=1",
                                       database="tw_world")), 0)

        actions = db_exec(self.base, self.env,
                          f"SELECT button,action,type FROM character_action WHERE guid={self.guid} ORDER BY button")
        expected_actions = db_exec(self.base, self.env,
                                   "SELECT button,action,type FROM playercreateinfo_action WHERE race=1 AND class=1 ORDER BY button",
                                   database="tw_world")
        self.assertEqual(actions, expected_actions)

        items = db_exec(self.base, self.env,
                        f"SELECT ci.item_template,SUM(ii.count) FROM character_inventory ci "
                        f"JOIN item_instance ii ON ii.guid=ci.item WHERE ci.guid={self.guid} "
                        "GROUP BY ci.item_template ORDER BY ci.item_template")
        expected_items = db_exec(self.base, self.env,
                                 "SELECT itemid,SUM(amount) FROM playercreateinfo_item "
                                 "WHERE race=1 AND class=1 GROUP BY itemid ORDER BY itemid",
                                 database="tw_world")
        self.assertEqual(items, expected_items)

        home = db_exec(self.base, self.env,
                       f"SELECT map,zone,ROUND(position_x,3),ROUND(position_y,3),ROUND(position_z,3) "
                       f"FROM character_homebind WHERE guid={self.guid}")
        expected_home = db_exec(self.base, self.env,
                                "SELECT map,zone,ROUND(position_x,3),ROUND(position_y,3),ROUND(position_z,3) "
                                "FROM playercreateinfo WHERE race=1 AND class=1",
                                database="tw_world")
        self.assertEqual(home, expected_home)
        self.assertEqual(db_int(self.base, self.env,
                                f"SELECT phase FROM bot_provision_state WHERE char_guid={self.guid}"), 2)


class BotProvisionResumeTests(unittest.TestCase):
    base = None
    env = None
    project = None
    evidence = None
    logs = ""

    @staticmethod
    def _seed_sql():
        return (
            "INSERT INTO tw_char.characters "
            "(guid, account, name, race, class, gender, level, money, "
            "position_x, position_y, position_z, map, orientation, zone, health, "
            "power1, power2, power3, power4, power5) VALUES\n"
            f"  ({UNRELATED_GUID}, {RESERVED + UNRELATED_GUID}, '{UNRELATED_NAME}', 1, 1, 0, 10, 100000, "
            "-8949.95, -132.493, 83.5312, 0, 0, 12, 26, 0, 0, 0, 0, 0),\n"
            f"  ({ORPHAN_GUID}, {RESERVED + ORPHAN_GUID}, '{PROVISION_NAME}', 1, 1, 0, 10, 100000, "
            "-8947.95, -134.493, 83.5312, 0, 0, 12, 26, 0, 0, 0, 0, 0);\n"
            "INSERT INTO tw_char.playerbot (char_guid, chance, ai) VALUES\n"
            f"  ({UNRELATED_GUID}, 100, 'Default');\n"
            "INSERT INTO tw_char.bot_ownership (char_guid, account_id, bot_type, provision_version) VALUES\n"
            f"  ({UNRELATED_GUID}, {RESERVED + UNRELATED_GUID}, 1, 2);\n"
            "INSERT INTO tw_char.bot_provision_state (char_guid, account_id, character_name, phase) VALUES\n"
            f"  ({ORPHAN_GUID}, {RESERVED + ORPHAN_GUID}, '{PROVISION_NAME}', 2);\n"
        )

    @classmethod
    def setUpClass(cls):
        try:
            command(["docker", "image", "inspect", IMAGE])
        except RuntimeError:
            raise unittest.SkipTest(f"{IMAGE} image not present; build it first")
        cls.project = "tortoise-bot-prov-" + uuid.uuid4().hex[:12]
        cls.evidence = ROOT / "local" / (cls.project + "-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ"))
        cls.evidence.mkdir(parents=True)
        try:
            cls.base, cls.env = boot_lab(cls.project, cls.evidence, world_env_for(PROVISION_NAME),
                                         seed_sql=cls._seed_sql())
            command(["docker", "compose"] + cls.base + ["up", "-d", "--no-deps", "world"], env=cls.env)
            cls.logs = wait_for(cls.base, cls.env, lambda l: "World server is up and running!" in l)
            (cls.evidence / "world.log").write_text(cls.logs, encoding="utf-8")
        except BaseException:
            if cls.base is not None:
                force_down(cls.base, cls.env)
            raise

    @classmethod
    def tearDownClass(cls):
        if cls.base is None:
            return
        teardown_lab(cls.base, cls.env, cls.project, cls.evidence)

    def test_world_ready(self):
        self.assertIn("World server is up and running!", self.logs,
                      "world never became ready; log: " + str(self.evidence / "world.log"))

    def test_ac2_orphan_completed_no_duplicate(self):
        self.assertEqual(db_int(self.base, self.env,
                                f"SELECT COUNT(*) FROM characters WHERE name = '{PROVISION_NAME}'"), 1,
                         "the orphan must be completed in place, not duplicated")
        account = int(db_exec(self.base, self.env,
                              f"SELECT account FROM characters WHERE guid = {ORPHAN_GUID}").strip())
        self.assertEqual(account, RESERVED + ORPHAN_GUID, "orphan identity (guid/account) must be preserved")
        self.assertEqual(db_int(self.base, self.env, f"SELECT COUNT(*) FROM bot_ownership WHERE char_guid = {ORPHAN_GUID}"), 1)
        self.assertEqual(db_int(self.base, self.env, f"SELECT COUNT(*) FROM playerbot WHERE char_guid = {ORPHAN_GUID}"), 1)
        self.assertEqual(db_int(self.base, self.env,
                                f"SELECT MAX(provision_version) FROM bot_ownership WHERE char_guid = {ORPHAN_GUID}"), 2)
        self.assertIn("completed (native-ready character)", self.logs)
        self.assertEqual(self.logs.count("created character"), 0,
                         "an existing orphan must be reused, never re-created")

    def test_ac2_unrelated_bot_untouched(self):
        self.assertEqual(db_int(self.base, self.env,
                                f"SELECT COUNT(*) FROM characters WHERE name = '{UNRELATED_NAME}'"), 1)
        account = int(db_exec(self.base, self.env,
                              f"SELECT account FROM characters WHERE guid = {UNRELATED_GUID}").strip())
        self.assertEqual(account, RESERVED + UNRELATED_GUID, "unrelated bot owner must be unchanged")
        self.assertEqual(db_int(self.base, self.env, f"SELECT COUNT(*) FROM bot_ownership WHERE char_guid = {UNRELATED_GUID}"), 1)
        self.assertEqual(db_int(self.base, self.env, f"SELECT COUNT(*) FROM playerbot WHERE char_guid = {UNRELATED_GUID}"), 1)


def snapshot(base, env, name):
    """Bounded state snapshot for one bot identity (review R1/R2 evidence)."""
    guid = db_exec(base, env, f"SELECT guid FROM characters WHERE name = '{name}'").strip()
    if not guid:
        return None
    account = int(db_exec(base, env, f"SELECT account FROM characters WHERE guid = {guid}").strip() or 0)
    bound = db_exec(base, env,
                    f"SELECT account_id, provision_version FROM bot_ownership WHERE char_guid = {guid}").strip().splitlines()
    return {"guid": int(guid), "account": account,
            "bindings": [(int(r.split("\t")[0]), int(r.split("\t")[1])) for r in bound if r],
            "roster": db_int(base, env, f"SELECT COUNT(*) FROM playerbot WHERE char_guid = {guid}")}


def start_world_again(base, env, evidence, world_env, label, marker):
    """Recreate the world service with a new env (provision name) and wait until the
    provisioning marker and readiness both appear in the fresh container log."""
    write_compose(evidence, world_env)
    command(["docker", "compose"] + base + ["up", "-d", "--no-deps", "--force-recreate", "world"],
            env=env, timeout=300)
    logs = wait_for(base, env, lambda l: marker in l and "World server is up and running!" in l)
    (evidence / (label + ".log")).write_text(logs, encoding="utf-8")
    return logs


class BotProvisionReviewRepairTests(unittest.TestCase):
    """R1/R3 (review 2026-09-11): orphan account reservation + temp-login probe.

    Run 1 provisions a fresh name while an unbound orphan owns a reserved
    account; the fresh bot must not reuse that account. The config-gated
    test-login probe exercises the temporary login overload (existing
    character succeeds once, duplicate rejected, missing character fails
    cleanly). Run 2 resumes the orphan; its identity is preserved and the
    fresh bot is untouched.
    """
    base = None
    env = None
    project = None
    evidence = None
    logs_run1 = ""
    logs_run2 = ""
    snap1 = None
    snap2 = None

    @staticmethod
    def _seed_sql():
        return (
            "INSERT INTO tw_char.characters "
            "(guid, account, name, race, class, gender, level, money, "
            "position_x, position_y, position_z, map, orientation, zone, health, "
            "power1, power2, power3, power4, power5) VALUES\n"
            f"  ({REV_ORPHAN_GUID}, {RESERVED + REV_ORPHAN_GUID}, '{REV_ORPHAN_NAME}', 1, 1, 0, 10, 100000, "
            "-8946.95, -136.493, 83.5312, 0, 0, 12, 26, 0, 0, 0, 0, 0);\n"
            "INSERT INTO tw_char.bot_provision_state (char_guid, account_id, character_name, phase) VALUES\n"
            f"  ({REV_ORPHAN_GUID}, {RESERVED + REV_ORPHAN_GUID}, '{REV_ORPHAN_NAME}', 2);\n"
        )

    @classmethod
    def setUpClass(cls):
        try:
            command(["docker", "image", "inspect", IMAGE])
        except RuntimeError:
            raise unittest.SkipTest(f"{IMAGE} image not present; build it first")
        cls.project = "tortoise-bot-prov-" + uuid.uuid4().hex[:12]
        cls.evidence = ROOT / "local" / (cls.project + "-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ"))
        cls.evidence.mkdir(parents=True)
        try:
            world_env = world_env_for(REV_FRESH_NAME)
            world_env["PLAYERBOT_TEST_LOGIN"] = f"{REV_ORPHAN_GUID},{MISSING_PROBE_GUID}"
            cls.base, cls.env = boot_lab(cls.project, cls.evidence, world_env, seed_sql=cls._seed_sql())
            command(["docker", "compose"] + cls.base + ["up", "-d", "--no-deps", "world"], env=cls.env)
            cls.logs_run1 = wait_for(
                cls.base, cls.env,
                lambda l: "World server is up and running!" in l
                          and f"login queued for {REV_ORPHAN_GUID}" in l)
            (cls.evidence / "world-run1.log").write_text(cls.logs_run1, encoding="utf-8")
            cls.snap1 = {n: snapshot(cls.base, cls.env, n) for n in (REV_FRESH_NAME, REV_ORPHAN_NAME)}
            cls.logs_run2 = start_world_again(
                cls.base, cls.env, cls.evidence, world_env_for(REV_ORPHAN_NAME),
                "world-run2", "completed (native-ready character)")
            cls.snap2 = {n: snapshot(cls.base, cls.env, n) for n in (REV_FRESH_NAME, REV_ORPHAN_NAME)}
        except BaseException:
            if cls.base is not None:
                force_down(cls.base, cls.env)
            raise

    @classmethod
    def tearDownClass(cls):
        if cls.base is None:
            return
        teardown_lab(cls.base, cls.env, cls.project, cls.evidence)

    def test_r1_fresh_bot_reserved_account_not_reused(self):
        fresh = self.snap1[REV_FRESH_NAME]
        self.assertIsNotNone(fresh, "fresh bot must be created; log: " + str(self.evidence / "world-run1.log"))
        self.assertGreaterEqual(fresh["account"], RESERVED, "fresh account must be in the reserved range")
        self.assertNotEqual(fresh["account"], RESERVED + REV_ORPHAN_GUID,
                            "fresh provision must not reuse the unbound orphan's reserved account")
        self.assertEqual(fresh["bindings"], [(fresh["account"], 2)], "exactly one valid binding for the fresh bot")
        self.assertEqual(fresh["roster"], 1)

    def test_r1_orphan_resumed_with_own_identity(self):
        before, after = self.snap1[REV_ORPHAN_NAME], self.snap2[REV_ORPHAN_NAME]
        self.assertIsNotNone(before)
        self.assertEqual(after["guid"], before["guid"], "orphan must be completed in place")
        self.assertEqual(after["account"], before["account"], "orphan account identity must be preserved")
        self.assertEqual(after["bindings"], [(after["account"], 2)],
                         "exactly one valid binding for the orphan after resume")
        self.assertEqual(after["roster"], 1)
        self.assertIn("completed (native-ready character)", self.logs_run2)
        self.assertEqual(self.logs_run2.count(f"created character '{REV_ORPHAN_NAME}'"), 0)

    def test_r1_fresh_bot_untouched_after_orphan_resume(self):
        self.assertEqual(self.snap2[REV_FRESH_NAME], self.snap1[REV_FRESH_NAME],
                         "resuming the orphan must not change the fresh bot")
        self.assertEqual(self.logs_run2.count(f"created character '{REV_FRESH_NAME}'"), 0)

    def test_r3_probe_temp_login_paths(self):
        self.assertIn(f"test-login {REV_ORPHAN_GUID} first=1 second=0", self.logs_run1,
                      "temp login for an existing character must succeed once and reject the duplicate")
        self.assertIn(f"login queued for {REV_ORPHAN_GUID}", self.logs_run1,
                      "the temporary bot's login must actually be queued")
        self.assertIn(f"test-login {MISSING_PROBE_GUID} first=0 second=0", self.logs_run1,
                      "a guid without a character must fail cleanly")


class BotProvisionFailClosedTests(unittest.TestCase):
    """R2 (review 2026-09-11): every inconsistent partial binding is rejected.

    Three world starts on one lab, each provisioning a different character:
    a mismatched binding without a roster row, a mismatched binding with a
    roster row, and a matched binding on an unsupported version. None may
    change character, binding, or roster data, and each must report
    rejection.
    """
    base = None
    env = None
    project = None
    evidence = None
    logs1 = ""
    logs2 = ""
    logs3 = ""
    snap1 = None
    snap2 = None
    snap3 = None

    NAMES = (MISFIT1_NAME, MISFIT2_NAME, BADVER_NAME)

    @staticmethod
    def _seed_sql():
        rows = []
        for guid, name, posx in ((MISFIT1_GUID, MISFIT1_NAME, -8944.95),
                                 (MISFIT2_GUID, MISFIT2_NAME, -8943.95),
                                 (BADVER_GUID, BADVER_NAME, -8942.95)):
            rows.append(
                f"  ({guid}, {RESERVED + guid}, '{name}', 1, 1, 0, 10, 100000, "
                f"{posx}, -138.493, 83.5312, 0, 0, 12, 26, 0, 0, 0, 0, 0)")
        return (
            "INSERT INTO tw_char.characters "
            "(guid, account, name, race, class, gender, level, money, "
            "position_x, position_y, position_z, map, orientation, zone, health, "
            "power1, power2, power3, power4, power5) VALUES\n"
            + ",\n".join(rows) + ";\n"
            f"INSERT INTO tw_char.bot_ownership (char_guid, account_id, bot_type, provision_version) VALUES\n"
            f"  ({MISFIT1_GUID}, {MISFIT1_BOUND}, 1, 2),\n"
            f"  ({MISFIT2_GUID}, {MISFIT2_BOUND}, 1, 2),\n"
            f"  ({BADVER_GUID}, {RESERVED + BADVER_GUID}, 1, 1);\n"
            f"INSERT INTO tw_char.playerbot (char_guid, chance, ai) VALUES\n"
            f"  ({MISFIT2_GUID}, 100, 'Default');\n"
        )

    @staticmethod
    def _expect(name, guid, bound, version, roster):
        return {"guid": guid, "account": RESERVED + guid,
                "bindings": [(bound, version)], "roster": roster}

    @classmethod
    def _snapshots(cls):
        return {n: snapshot(cls.base, cls.env, n) for n in cls.NAMES}

    @classmethod
    def setUpClass(cls):
        try:
            command(["docker", "image", "inspect", IMAGE])
        except RuntimeError:
            raise unittest.SkipTest(f"{IMAGE} image not present; build it first")
        cls.project = "tortoise-bot-prov-" + uuid.uuid4().hex[:12]
        cls.evidence = ROOT / "local" / (cls.project + "-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ"))
        cls.evidence.mkdir(parents=True)
        marker = "rejected, nothing modified (fail closed)"
        try:
            cls.base, cls.env = boot_lab(cls.project, cls.evidence, world_env_for(MISFIT1_NAME),
                                         seed_sql=cls._seed_sql())
            command(["docker", "compose"] + cls.base + ["up", "-d", "--no-deps", "world"], env=cls.env)
            cls.logs1 = wait_for(cls.base, cls.env, lambda l: marker in l and "World server is up and running!" in l)
            (cls.evidence / "world-run1.log").write_text(cls.logs1, encoding="utf-8")
            cls.snap1 = cls._snapshots()
            cls.logs2 = start_world_again(cls.base, cls.env, cls.evidence, world_env_for(MISFIT2_NAME),
                                          "world-run2", marker)
            cls.snap2 = cls._snapshots()
            cls.logs3 = start_world_again(cls.base, cls.env, cls.evidence, world_env_for(BADVER_NAME),
                                          "world-run3", marker)
            cls.snap3 = cls._snapshots()
        except BaseException:
            if cls.base is not None:
                force_down(cls.base, cls.env)
            raise

    @classmethod
    def tearDownClass(cls):
        if cls.base is None:
            return
        teardown_lab(cls.base, cls.env, cls.project, cls.evidence)

    def test_r2_mismatched_binding_without_roster_rejected(self):
        expected = self._expect(MISFIT1_NAME, MISFIT1_GUID, MISFIT1_BOUND, 2, 0)
        self.assertEqual(self.snap1[MISFIT1_NAME], expected,
                         "mismatched binding (no roster) must not change any data; log: " + str(self.evidence / "world-run1.log"))
        self.assertIn("inconsistent binding", self.logs1)
        self.assertIn("rejected, nothing modified (fail closed)", self.logs1)
        self.assertNotIn("created character", self.logs1)
        self.assertNotIn("completed (native-ready character)", self.logs1)
        self.assertNotIn("roster row added to existing binding", self.logs1)

    def test_r2_mismatched_binding_with_roster_rejected(self):
        expected = self._expect(MISFIT2_NAME, MISFIT2_GUID, MISFIT2_BOUND, 2, 1)
        self.assertEqual(self.snap2[MISFIT2_NAME], expected,
                         "mismatched binding (roster present) must not change any data; log: " + str(self.evidence / "world-run2.log"))
        self.assertIn("rejected, nothing modified (fail closed)", self.logs2)
        self.assertNotIn("completed (native-ready character)", self.logs2)
        self.assertNotIn("roster row added to existing binding", self.logs2)

    def test_r2_unsupported_version_rejected(self):
        expected = self._expect(BADVER_NAME, BADVER_GUID, RESERVED + BADVER_GUID, 1, 0)
        self.assertEqual(self.snap3[BADVER_NAME], expected,
                         "unsupported binding version must not change any data; log: " + str(self.evidence / "world-run3.log"))
        self.assertIn("rejected, nothing modified (fail closed)", self.logs3)

    def test_r2_no_cross_changes_between_runs(self):
        for name in self.NAMES:
            self.assertEqual(self.snap3[name], self.snap1[name],
                             f"{name} must be unchanged across all three runs")

if __name__ == "__main__":
    unittest.main()
