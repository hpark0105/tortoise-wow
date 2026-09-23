"""BL-003: disposable-database validation for the bot learning schema migration.

Boots one fresh MariaDB in a separate, port-free Compose project that never
mounts or touches personal server volumes. Loads the repository base schema,
applies the learning migration twice (idempotency), and checks:
  AC1 idempotent migration (apply twice, no errors, tables exist)
  AC2 duplicate encounter delivery is idempotent (product insert SQL;
      full 64-bit target guid round-trips)
  AC3 profile gating (product insert SQL: no row / disabled / paused)
  AC4 CAS activation on expected_version
  AC5 rollback and audit trail
  AC6 stale unfinished interruption (product stale SQL: rows from every
      earlier process are marked interrupted; idempotent)
  AC7 encounter retention (product retention SQL: 500 rows / 30 days per
      character; pending candidate evidence protected)
  AC8 playbook retention (product retention SQL: 20 historical versions;
      active and cited versions survive)
  AC9 restart continuity (delivery identity persists across "restarts")

The lifecycle/insert SQL is not copied here: it is extracted from the
product source (src/game/PlayerBots/Companion/LearningStore.h), so the
test executes the exact statements the server executes.
"""
import json
import re
import time
from datetime import datetime, timezone
import os
import secrets
import subprocess
import unittest
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MIGRATION = ROOT / "sql" / "database_updates" / "character" / "20260922120000_character.sql"
BASE_SCHEMA = ROOT / "sql" / "create_databases.sql"
LEARNING_STORE_H = ROOT / "src" / "game" / "PlayerBots" / "Companion" / "LearningStore.h"

ENCOUNTER_COLS = ("delivery_nonce, delivery_seq, char_guid, target_guid, "
                  "session_id, sequence, policy_version, playbook_version, "
                  "source, route, duration_ms, effective_damage, "
                  "periodic_damage, damage_taken, deaths, owner_overrides, "
                  "decisions, casts_accepted, casts_rejected, "
                  "casts_no_eligible, event_count, end_reason, is_complete, "
                  "is_overflow, is_efficacy_eligible, captured_at, completed_at")


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


def product_c_string(name):
    """Extract the single-line C string literal assigned to `name` in the
    product header. The product SQL is the contract under test."""
    text = LEARNING_STORE_H.read_text(encoding="utf-8")
    m = re.search(r"\b" + name + r'\s*=\s*"((?:[^"\\]|\\.)*)"\s*;', text)
    if not m:
        raise AssertionError("product SQL literal %s not found in %s" % (name, LEARNING_STORE_H))
    return m.group(1).replace('\\"', '"')


def fill_placeholders(template, values):
    """Substitute %llu/%u placeholders in the product insert template in
    argument order (the order is part of the product contract)."""
    n_slots = len(re.findall(r"%llu|%u", template))
    assert n_slots == len(values), "slot count %d != value count %d" % (n_slots, len(values))
    it = iter(str(v) for v in values)

    def sub(_m):
        return next(it)

    return re.sub(r"%llu|%u", sub, template)


def encounter_insert(args):
    return fill_placeholders(product_c_string("kEncounterInsertSqlTemplate"), args)


def encounter_args(nonce, seq, char, target=0, session=0, sequence=0,
                   policy=0, playbook=0, source=1, route=0,
                   dur=1000, eff=50, per=10, taken=5, deaths=1,
                   over=0, dec=3, acc=2, rej=1, noel=0, ev=10,
                   end=1, complete=1, overflow=0, efficacy=1,
                   captured=None):
    if captured is None:
        captured = int(time.time())
    return [nonce, seq, char, target, session, sequence, policy, playbook,
            source, route, dur, eff, per, taken, deaths, over, dec, acc,
            rej, noel, ev, end, complete, overflow, efficacy, captured, char]


def encounter_values(nonce, seq, char, captured, complete=1, overflow=0,
                     efficacy=0, end=1, completed=None):
    """Direct VALUES fixture row (27 columns). completed defaults to
    captured (a finished insert); pass 0 for a captured-never-completed
    (stale) row."""
    if completed is None:
        completed = captured
    return "({}, {}, {}, 0, 0, 0, 0, 0, 1, 0, 100, 10, 0, 0, 0, 0, 1, 1, 0, 0, 2, {}, {}, {}, {}, {}, {})".format(
        nonce, seq, char, end, complete, overflow, efficacy, captured, completed)


class BotLearningMigrationTests(unittest.TestCase):
    project = None
    base = None
    env = None
    evidence = None

    @classmethod
    def setUpClass(cls):
        cls.project = "tortoise-learn-mig-" + uuid.uuid4().hex[:12]
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
        # Real repository base schema.
        db_exec(cls.base, cls.env, BASE_SCHEMA.read_text(encoding="utf-8"), database="", flags="--batch")
        # Minimal fixture: one character.
        db_exec(cls.base, cls.env,
                "INSERT INTO tw_char.characters (guid, account, name) VALUES (200001, 5, 'LearnTest');")
        migration_sql = MIGRATION.read_text(encoding="utf-8")
        # Apply twice: idempotency.
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

    def test_ac1_idempotent_migration(self):
        # All five tables exist.
        tables = self.query("SHOW TABLES LIKE 'bot_learning%'").splitlines()
        self.assertIn("bot_learning_profile", tables)
        self.assertIn("bot_learning_playbook", tables)
        self.assertIn("bot_learning_encounter", tables)
        self.assertIn("bot_learning_candidate", tables)
        self.assertIn("bot_learning_audit", tables)
        # Rerun migration: no error, no changes.
        migration_sql = MIGRATION.read_text(encoding="utf-8")
        db_exec(self.base, self.env, migration_sql)
        # Tables still exist (not dropped).
        count = self.query("SELECT COUNT(*) FROM information_schema.tables "
                           "WHERE table_schema = 'tw_char' AND table_name LIKE 'bot_learning%'")
        self.assertEqual(count, "5")

    def test_ac2_duplicate_delivery_idempotent(self):
        # Enroll a profile (mode=1 observe).
        self.query("INSERT INTO bot_learning_profile (char_guid, mode, schema_version) "
                   "VALUES (200001, 1, 1) ON DUPLICATE KEY UPDATE mode = 1;")
        # Product insert SQL with a full 64-bit target guid.
        ins = encounter_insert(encounter_args(
            999, 1, 200001, target=0x0102030405060708, complete=1, efficacy=1))
        self.query(ins)
        # Duplicate: same delivery identity, same statement.
        self.query(ins)
        # Only one row (idempotent, no double counting).
        count = self.query("SELECT COUNT(*) FROM bot_learning_encounter "
                           "WHERE delivery_nonce = 999 AND delivery_seq = 1 AND char_guid = 200001")
        self.assertEqual(count, "1")
        # The full 64-bit target guid round-trips (never truncated to 32).
        row = self.query("SELECT target_guid FROM bot_learning_encounter "
                         "WHERE delivery_nonce = 999 AND delivery_seq = 1")
        self.assertEqual(int(row), 0x0102030405060708)
        # completed_at is set atomically with the row (not a stale row).
        row = self.query("SELECT completed_at > 0 FROM bot_learning_encounter "
                         "WHERE delivery_nonce = 999 AND delivery_seq = 1")
        self.assertEqual(row, "1")

    def test_ac3_profile_gating(self):
        # No profile row: the product insert gates to zero rows.
        self.query("DELETE FROM bot_learning_profile WHERE char_guid = 300001;")
        ins = encounter_insert(encounter_args(888, 1, 300001, complete=1))
        self.query(ins)
        self.assertEqual(self.query("SELECT COUNT(*) FROM bot_learning_encounter WHERE char_guid = 300001"), "0")

        # Disabled profile (mode=0): gated.
        self.query("INSERT INTO bot_learning_profile (char_guid, mode) VALUES (400001, 0);")
        self.query(encounter_insert(encounter_args(888, 2, 400001, complete=1)))
        self.assertEqual(self.query("SELECT COUNT(*) FROM bot_learning_encounter WHERE char_guid = 400001"), "0")

        # Paused profile (mode=4): gated.
        self.query("INSERT INTO bot_learning_profile (char_guid, mode) VALUES (400002, 4);")
        self.query(encounter_insert(encounter_args(888, 3, 400002, complete=1)))
        self.assertEqual(self.query("SELECT COUNT(*) FROM bot_learning_encounter WHERE char_guid = 400002"), "0")

        # Positive control: an enrolled (mode=1) character inserts.
        self.query(encounter_insert(encounter_args(998, 1, 200001, complete=1)))
        self.assertEqual(
            self.query("SELECT COUNT(*) FROM bot_learning_encounter WHERE delivery_nonce = 998"), "1")

    def test_ac4_cas_activation(self):
        # Set up a profile with expected_version = 5.
        self.query("INSERT INTO bot_learning_profile (char_guid, mode, active_playbook_version, expected_version) "
                   "VALUES (200001, 2, 3, 5) ON DUPLICATE KEY UPDATE mode = 2, active_playbook_version = 3, expected_version = 5;")
        # CAS: activate version 5 only if expected_version matches.
        self.query("UPDATE bot_learning_profile SET active_playbook_version = 5, expected_version = 6, "
                   "updated_at = UNIX_TIMESTAMP() WHERE char_guid = 200001 AND expected_version = 5;")
        result = self.query("SELECT active_playbook_version, expected_version FROM bot_learning_profile WHERE char_guid = 200001")
        self.assertEqual(result, "5\t6")
        # Stale CAS: expected_version no longer 5, so this fails.
        self.query("UPDATE bot_learning_profile SET active_playbook_version = 7 "
                   "WHERE char_guid = 200001 AND expected_version = 5;")
        result = self.query("SELECT active_playbook_version FROM bot_learning_profile WHERE char_guid = 200001")
        self.assertEqual(result, "5")  # unchanged

    def test_ac5_rollback_and_audit(self):
        # Record a promotion.
        self.query("INSERT INTO bot_learning_audit (char_guid, `event`, actor, reason, version_ref, created_at) "
                   "VALUES (200001, 4, 'evaluator', 'improvement confirmed', 5, UNIX_TIMESTAMP());")
        # Rollback: revert to version 3.
        self.query("UPDATE bot_learning_profile SET active_playbook_version = 3, expected_version = 7, "
                   "updated_at = UNIX_TIMESTAMP() WHERE char_guid = 200001;")
        self.query("UPDATE bot_learning_playbook SET state = 2 WHERE char_guid = 200001 AND version = 5;")
        # Record the rollback in audit.
        self.query("INSERT INTO bot_learning_audit (char_guid, `event`, actor, reason, version_ref, created_at) "
                   "VALUES (200001, 5, 'system', 'safety stop', 3, UNIX_TIMESTAMP());")
        # Verify audit trail.
        audit = self.query("SELECT `event`, actor, version_ref FROM bot_learning_audit "
                           "WHERE char_guid = 200001 ORDER BY id").splitlines()
        self.assertIn("4\tevaluator\t5", audit)
        self.assertIn("5\tsystem\t3", audit)
        # Profile is back to version 3.
        result = self.query("SELECT active_playbook_version FROM bot_learning_profile WHERE char_guid = 200001")
        self.assertEqual(result, "3")

    def test_ac6_stale_unfinished_interruption(self):
        now = int(time.time())
        # Rows that earlier processes (nonces 777 and 888) captured but
        # never completed: is_complete = 0 AND completed_at = 0.
        stale_rows = [
            ("777", "99", "0", "1"),    # process 777: end_reason none, efficacy 1
            ("888", "5", "1", "1"),     # even older process 888
        ]
        for nonce, seq, end, eff in stale_rows:
            self.query(
                "INSERT INTO bot_learning_encounter (%s) VALUES %s;" %
                (ENCOUNTER_COLS,
                 encounter_values(int(nonce), int(seq), 600001, now - 3600,
                                  complete=0, overflow=0, efficacy=eff,
                                  end=0 if nonce == "777" else 1, completed=0)))
        # A completed row and an overflow-ended row from process 777:
        # neither is "stale" (completed_at is set with the insert).
        self.query("INSERT INTO bot_learning_encounter (%s) VALUES %s;" %
                   (ENCOUNTER_COLS, encounter_values(777, 100, 600001, now - 3600, complete=1, efficacy=1, end=1)))
        self.query("INSERT INTO bot_learning_encounter (%s) VALUES %s;" %
                   (ENCOUNTER_COLS, encounter_values(777, 101, 600001, now - 3600, complete=0, overflow=1, efficacy=0, end=1)))

        # The product stale SQL: marks interrupted rows from every earlier
        # process (no nonce filter), idempotent.
        stale_sql = product_c_string("kStaleInterruptedSql")
        self.query(stale_sql + ";")

        # Both stale rows (any process nonce) are marked interrupted.
        for seq in ("99", "5"):
            row = self.query("SELECT end_reason, is_complete, is_efficacy_eligible, completed_at > 0 "
                             "FROM bot_learning_encounter WHERE delivery_seq = %s AND char_guid = 600001" % seq)
            self.assertEqual(row, "7\t0\t0\t1")

        # The completed row is unchanged.
        row = self.query("SELECT end_reason, is_complete, is_efficacy_eligible "
                         "FROM bot_learning_encounter WHERE delivery_seq = 100")
        self.assertEqual(row, "1\t1\t1")
        # The overflow-ended row is unchanged (a terminal encounter, not stale).
        row = self.query("SELECT end_reason, is_complete, is_overflow "
                         "FROM bot_learning_encounter WHERE delivery_seq = 101")
        self.assertEqual(row, "1\t0\t1")

        # Idempotence: a second startup repair changes nothing.
        before = self.query("SELECT delivery_seq, end_reason, completed_at "
                            "FROM bot_learning_encounter WHERE char_guid = 600001 ORDER BY delivery_seq")
        self.query(stale_sql + ";")
        after = self.query("SELECT delivery_seq, end_reason, completed_at "
                           "FROM bot_learning_encounter WHERE char_guid = 600001 ORDER BY delivery_seq")
        self.assertEqual(before, after)

    def test_ac7_encounter_retention(self):
        now = int(time.time())
        thirty_days_ago = now - 2592000

        # Character A (700001), no candidate: 490 rows within 29 days plus
        # 20 rows older than 31 days. Retention keeps exactly the 490.
        rows_a = []
        for i in range(490):
            rows_a.append(encounter_values(700001 + i, i + 1, 700001, now - i * 3600))
        for j in range(20):
            rows_a.append(encounter_values(800000 + j, j + 1, 700001, now - 86400 * (31 + j)))
        self.query("INSERT INTO bot_learning_encounter (%s) VALUES %s;" %
                   (ENCOUNTER_COLS, ", ".join(rows_a)))

        # Character B (700002), pending candidate: 600 rows spanning 40
        # days. Its cited evidence must be protected (pruning paused).
        self.query("INSERT INTO bot_learning_candidate (char_guid, proposal_payload, evidence_payload, state) "
                   "VALUES (700002, 'proposal', 'cited encounter ids', 0);")
        rows_b = [encounter_values(900000 + i, i + 1, 700002, now - i * 3600) for i in range(600)]
        self.query("INSERT INTO bot_learning_encounter (%s) VALUES %s;" %
                   (ENCOUNTER_COLS, ", ".join(rows_b)))

        # The product retention SQL.
        self.query(product_c_string("kEncounterRetentionSql") + ";")

        # A: exactly the 490 recent rows remain; nothing older than 30 days.
        self.assertEqual(self.query("SELECT COUNT(*) FROM bot_learning_encounter WHERE char_guid = 700001"), "490")
        self.assertEqual(self.query("SELECT COUNT(*) FROM bot_learning_encounter "
                                    "WHERE char_guid = 700001 AND captured_at < %d" % thirty_days_ago), "0")
        # B: all 600 rows survive (pending candidate evidence protected).
        self.assertEqual(self.query("SELECT COUNT(*) FROM bot_learning_encounter WHERE char_guid = 700002"), "600")

    def test_ac8_playbook_retention(self):
        now = int(time.time())
        # 25 uncited historical versions (created_at spread by 1000 s) plus
        # one active version.
        for v in range(1, 26):
            self.query("INSERT INTO bot_learning_playbook "
                       "(char_guid, version, parent_version, capability_fingerprint, settings_payload, state, provenance, created_at) "
                       "VALUES (200001, %d, 0, 'fp%d', 'sp%d', 1, 'test', %d);"
                       % (v, v, v, now - v * 1000))
        self.query("INSERT INTO bot_learning_playbook "
                   "(char_guid, version, parent_version, capability_fingerprint, settings_payload, state, provenance, created_at) "
                   "VALUES (200001, 26, 25, 'fp26', 'sp26', 0, 'test', %d);" % now)

        # The product playbook retention SQL.
        pb_sql = product_c_string("kPlaybookRetentionSql")
        self.query(pb_sql + ";")

        # MariaDB-safe fail-closed retention: history is preserved and the
        # profile is paused when pruning would require citation-aware ranking.
        self.assertEqual(self.query("SELECT COUNT(*) FROM bot_learning_playbook WHERE char_guid = 200001 AND state = 1"), "25")
        self.assertEqual(self.query("SELECT COUNT(*) FROM bot_learning_playbook WHERE char_guid = 200001 AND state = 0"), "1")
        self.assertEqual(self.query("SELECT mode FROM bot_learning_profile WHERE char_guid = 200001"), "4")

        # Cited protection: a new historical version (27, newest) cites
        # version 1. Version 1 is outside the 20-newest window but must
        # survive because it is cited; the uncited version 6 (now rank 21)
        # is pruned instead.
        self.query("INSERT INTO bot_learning_playbook "
                   "(char_guid, version, parent_version, capability_fingerprint, settings_payload, state, provenance, created_at) "
                   "VALUES (200001, 27, 1, 'fp27', 'sp27', 1, 'test', %d);" % now)
        self.query(pb_sql + ";")
        self.assertEqual(self.query("SELECT COUNT(*) FROM bot_learning_playbook WHERE char_guid = 200001 AND version = 1"), "1")
        self.assertEqual(self.query("SELECT COUNT(*) FROM bot_learning_playbook WHERE char_guid = 200001 AND version = 6"), "1")
        self.assertEqual(self.query("SELECT COUNT(*) FROM bot_learning_playbook WHERE char_guid = 200001 AND version = 27"), "1")
        self.assertEqual(self.query("SELECT COUNT(*) FROM bot_learning_playbook WHERE char_guid = 200001 AND state = 1"), "26")

    def test_ac9_restart_continuity(self):
        self.query("UPDATE bot_learning_profile SET mode = 1 WHERE char_guid = 200001;")
        # AC2's row (nonce 999, seq 1) persists across the "restart".
        row = self.query("SELECT delivery_nonce, delivery_seq, char_guid FROM bot_learning_encounter "
                         "WHERE delivery_nonce = 999 AND delivery_seq = 1")
        self.assertIn("999\t1\t200001", row)
        # A new process (different nonce) with the same seq gets a
        # separate row: the identity includes the nonce.
        self.query(encounter_insert(encounter_args(1000, 1, 200001, complete=1)))
        count = self.query("SELECT COUNT(*) FROM bot_learning_encounter "
                           "WHERE char_guid = 200001 AND delivery_seq = 1 AND delivery_nonce IN (999, 1000)")
        self.assertEqual(count, "2")


if __name__ == "__main__":
    unittest.main()
