"""Disposable-database validation for the bot ownership metadata migration (TW-005).

Boots one fresh MariaDB in a separate, port-free Compose project that never
mounts or touches personal server volumes, loads the repository base schema,
applies the character migration twice (idempotency), and checks:
  AC1 stable bot identity/ownership/type/version with no duplicate records,
  AC2 validation reports unowned or incompatible metadata rows while human
      character records stay byte-identical.
"""
import json
from datetime import datetime, timezone
import os
import secrets
import subprocess
import unittest
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PROJECT_PATTERN = r"tortoise-bot-mig-[a-f0-9]{12}"
RESERVED = 1000000000
MIGRATION = ROOT / "sql" / "database_updates" / "character" / "20260911174500_character.sql"
BASE_SCHEMA = ROOT / "sql" / "create_databases.sql"


def command(args, *, env=None, timeout=120, input_text=None):
    result = subprocess.run(args, cwd=ROOT, env=env, capture_output=True, timeout=timeout,
                            input=input_text.encode("utf-8") if input_text is not None else None)
    if result.returncode:
        tail = result.stderr.decode("utf-8", errors="replace").strip()[-400:]
        raise RuntimeError(f"command failed ({result.returncode}): {args[0]}: {tail}")
    return result.stdout.decode("utf-8", errors="replace").strip()


def db_exec(base, env, query, database="tw_char", flags="--batch --skip-column-names"):
    """Run mariadb as root inside the lab container; the rest of stdin is SQL."""
    if database:
        script = 'export MYSQL_PWD="$MARIADB_ROOT_PASSWORD"\nmariadb --user=root ' + flags + " " + database
    else:
        script = 'export MYSQL_PWD="$MARIADB_ROOT_PASSWORD"\nmariadb --user=root ' + flags
    return command(["docker", "compose"] + base + ["exec", "-T", "db", "bash"],
                   env=env, input_text=script + "\n" + query, timeout=300)


class BotOwnershipMigrationTests(unittest.TestCase):
    project = None
    base = None
    env = None
    evidence = None

    @classmethod
    def setUpClass(cls):
        cls.project = "tortoise-bot-mig-" + uuid.uuid4().hex[:12]
        cls.evidence = ROOT / "local" / (cls.project + "-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ"))
        cls.evidence.mkdir(parents=True)
        (cls.evidence / "compose.json").write_text(json.dumps({
            "services": {
                "db": {
                    "image": "mariadb:10.11",
                    "environment": {"MARIADB_ROOT_PASSWORD": "${MIG_ROOT_PASSWORD:?}"},
                    "volumes": ["database:/var/lib/mysql",
                                {"type": "bind", "source": str(ROOT / "sql"),
                                 "target": "/bootstrap/sql", "read_only": True}],
                    "healthcheck": {"test": ["CMD", "healthcheck.sh", "--connect", "--innodb_initialized"],
                                    "interval": "5s", "timeout": "5s", "retries": 120,
                                    "start_period": "5m"},
                    "stop_grace_period": "2m",
                }
            },
            "volumes": {"database": {}},
        }, indent=2), encoding="utf-8")
        cls.env = dict(os.environ, MIG_ROOT_PASSWORD=secrets.token_hex(24))
        cls.base = ["-f", str(cls.evidence / "compose.json"), "-p", cls.project]
        command(["docker", "compose"] + cls.base + ["config", "--quiet"], env=cls.env)
        command(["docker", "compose"] + cls.base + ["up", "-d", "--wait", "--wait-timeout", "600", "db"],
                env=cls.env, timeout=660)
        # Real repository base schema: creates tw_char with characters and playerbot.
        db_exec(cls.base, cls.env, BASE_SCHEMA.read_text(encoding="utf-8"), database="", flags="--batch")
        fixtures = """
INSERT INTO tw_char.characters (guid, account, name) VALUES
  (100001, 1, 'HumanOne'),
  (100002, {r1}, 'BotAlpha'),
  (100003, 42, 'LegacyBot');
INSERT INTO tw_char.playerbot (char_guid, chance, ai) VALUES
  (100002, 10, 'Default'),
  (100003, 10, 'Default'),
  (100099, 10, 'Default');
""".format(r1=RESERVED + 100002)
        db_exec(cls.base, cls.env, fixtures)
        migration_sql = MIGRATION.read_text(encoding="utf-8")
        # Apply twice: updater rerun plus direct rerun must be duplicate-free.
        for _ in range(2):
            db_exec(cls.base, cls.env, migration_sql)

    @classmethod
    def tearDownClass(cls):
        if cls.base is None:
            return
        ids = command(["docker", "compose"] + cls.base + ["ps", "-a", "-q"], env=cls.env).splitlines()
        if ids:
            for row in json.loads(command(["docker", "inspect", *ids])):
                labels = row["Config"]["Labels"]
                if labels.get("com.docker.compose.project") != cls.project:
                    raise AssertionError("refusing to remove container from another project")
                if row["HostConfig"].get("PortBindings"):
                    raise AssertionError("migration lab unexpectedly published ports")
                for mount in row["Mounts"]:
                    if mount["Type"] == "volume" and mount["Name"] != cls.project + "_database":
                        raise AssertionError("unexpected volume in migration lab")
        command(["docker", "compose"] + cls.base + ["down", "--volumes", "db"], env=cls.env, timeout=300)
        (cls.evidence / "compose.json").write_text(
            "removed: " + datetime.now(timezone.utc).isoformat() + "\n", encoding="utf-8")

    def query(self, text):
        return db_exec(self.base, self.env, text)

    def human_snapshot(self):
        return self.query("SELECT guid, account, name FROM characters WHERE account < 1000000000 ORDER BY guid")

    def test_ac1_stable_identity_no_duplicates(self):
        rows = self.query("SELECT char_guid, account_id, bot_type, provision_version "
                          "FROM bot_ownership ORDER BY char_guid").splitlines()
        # Only the pre-marked roster bot (100002) gets a binding; legacy (100003)
        # and the orphan roster row (100099) must not.
        self.assertEqual(rows, [f"100002\t{RESERVED + 100002}\t1\t1"])
        self.assertEqual(self.query("SELECT COUNT(*) FROM bot_ownership"), "1")

    def test_ac2_validation_reports_mismatches(self):
        # Corrupt metadata: unowned character and a human-owned character.
        self.query("INSERT INTO bot_ownership (char_guid, account_id, bot_type, provision_version) VALUES "
                   f"(100077, {RESERVED + 100077}, 1, 1), (100001, {RESERVED + 100001}, 1, 1)")
        validation = """
SELECT b.char_guid, 'unowned_character' AS issue
FROM bot_ownership b LEFT JOIN characters c ON c.guid = b.char_guid
WHERE c.guid IS NULL
UNION ALL
SELECT b.char_guid, 'character_owned_by_non_bot_account' AS issue
FROM bot_ownership b JOIN characters c ON c.guid = b.char_guid
WHERE c.account < 1000000000
UNION ALL
SELECT c.guid, 'roster_without_binding' AS issue
FROM playerbot p JOIN characters c ON c.guid = p.char_guid
LEFT JOIN bot_ownership b ON b.char_guid = c.guid
WHERE b.char_guid IS NULL
ORDER BY 1, 2
"""
        reported = self.query(validation).splitlines()
        self.assertIn("100001\tcharacter_owned_by_non_bot_account", reported)
        self.assertIn("100003\troster_without_binding", reported)
        self.assertIn("100077\tunowned_character", reported)

    def test_human_records_untouched(self):
        before = self.human_snapshot()
        db_exec(self.base, self.env, MIGRATION.read_text(encoding="utf-8"))
        after = self.human_snapshot()
        self.assertEqual(before, after)
        self.assertEqual(self.query("SELECT account FROM characters WHERE guid = 100001"), "1")


if __name__ == "__main__":
    unittest.main()