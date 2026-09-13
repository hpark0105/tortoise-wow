"""MVP-008: prove exact earned state survives an isolated backup restore.

Lab A (disposable, port-free project): the owned bot 500106 earns the same
deterministic state as MVP-007:
  * one item 117 from a deterministic corpse (loot template 990006), and
  * quest 456 "The Balance of Nature" (7x 2031 + 4x 1984, giver/finisher
    2079) through the normal accept / objective / turn-in path.
The world performs a clean stop (logout save), the snapshot S1 is taken,
and the lab's own db runs /ops/backup.sh (the same script the personal
server's backup.ps1 invokes), dumping tw_logon/tw_char/tw_world/tw_logs
into the lab's evidence directory with a sha256 sidecar.

Lab B (a separate disposable project sharing nothing with Lab A or the
personal server): a fresh db boots and the backup is imported through
/ops/restore-check.sh (the same script the personal test-restore.ps1
invokes), which validates the filename pattern and sha256 first, then
imports and runs the bootstrap check. A row comparison must show identity,
ownership, XP, money, inventory and quest state matching S1 exactly. Lab B
then boots the world with the declared quest enabled: the restored
rewarded quest must resume at phase 3 and fail closed on re-reward (no
double XP), and the state must still match S1 exactly.

Personal-volume protection: both labs are separate compose projects (uuid
names, port-free, project-scoped volumes); the backup/restore scripts only
operate inside the lab db container and the lab's evidence directory;
restore-check.sh rejects any filename backup.sh never produced; and the
test records the personal server containers' StartedAt before and after
the run and requires it unchanged.
"""
import os
import shutil
import time
import unittest
import uuid
from datetime import datetime, timezone

import test_bot_provision as p

BOT_GUID = 500106
BOT_NAME = "Restorebot"
QUEST_ID = "456"
ITEM_ID = 117
LOOT_GUID = 2500062
GIVER_GUID = 2500041
KILL_GUIDS = [2500042, 2500043, 2500044, 2500045, 2500046, 2500047, 2500048]
BOAR_GUIDS = [2500052, 2500053, 2500054, 2500055]
PERSONAL_CONTAINERS = ("tortoise-local-db-1", "tortoise-local-realmd-1",
                       "tortoise-local-world-1")
EXPECTED_QUEST_ROW = "1\t1\t7\t4"  # COMPLETE, rewarded, 7x2031 + 4x1984


def _creature_rows():
    rows = ["({},6,0,-8945.95,-132.493,83.5312,0,600,600,0,25,100,0,1)".format(LOOT_GUID),
            "({},2079,0,-8945.95,-132.493,83.5312,0,600,600,0,100,100,0,1)".format(GIVER_GUID)]
    # Spread the pack along the y axis (4 yd spacing) so the bot faces a
    # few attackers at a time instead of the whole stacked group.
    rows += ["({},2031,0,-8962.95,{y},83.5312,0,600,600,0,25,100,0,1)".format(g, y=-128.493 + 4.0 * i)
             for i, g in enumerate(KILL_GUIDS)]
    rows += ["({},1984,0,-8962.95,{y},83.5312,0,600,600,0,25,100,0,1)".format(g, y=-156.493 + 4.0 * i)
             for i, g in enumerate(BOAR_GUIDS)]
    return ",".join(rows)


def _seed_sql():
    return """
INSERT INTO tw_char.characters
 (guid,account,name,race,class,gender,level,money,position_x,position_y,position_z,map,
  orientation,zone,health,power1,power2,power3,power4,power5,xp,xp_gain)
VALUES (500106,1000500106,'Restorebot',1,1,0,3,100000,-8949.95,-132.493,83.5312,0,
 0,12,100,0,0,0,0,0,0,1);
INSERT INTO tw_char.playerbot (char_guid,chance,ai) VALUES (500106,100,'Default');
INSERT INTO tw_char.bot_ownership (char_guid,account_id,bot_type,provision_version)
 VALUES (500106,1000500106,1,2);
-- Lab-only fixture: the declared-quest creatures deal no damage and have
-- reduced health so the L3 bot survives and finishes the kill sequence;
-- the quest flow is what is under test.
UPDATE tw_world.creature_template SET dmg_min=0, dmg_max=0, health_min=8, health_max=8 WHERE entry IN (2031,1984);
UPDATE tw_world.creature_template SET loot_id=990006 WHERE entry=6;
DELETE FROM tw_world.creature_loot_template WHERE entry=990006;
INSERT INTO tw_world.creature_loot_template
 (entry,item,ChanceOrQuestChance,groupid,mincountOrRef,maxcount,condition_id)
 VALUES (990006,117,100,0,1,1,0);
-- Lab isolation: remove natural creatures around the spawn so the seeded
-- creatures are the only ones in the box during the earn phase.
DELETE FROM tw_world.creature WHERE map = 0
  AND position_x BETWEEN -8990 AND -8910 AND position_y BETWEEN -170 AND -110;
INSERT INTO tw_world.creature
 (guid,id,map,position_x,position_y,position_z,orientation,spawntimesecsmin,
  spawntimesecsmax,wander_distance,health_percent,mana_percent,movement_type,spawn_flags)
VALUES
""" + _creature_rows() + ";"


def _quest_rewarded_saved(base, env):
    out = p.db_exec(base, env,
                    "SELECT 1 FROM character_queststatus WHERE guid=%d AND quest=%s AND rewarded=1"
                    % (BOT_GUID, QUEST_ID))
    return out.strip() == "1"


def _state(base, env):
    out = p.db_exec(base, env,
                    "SELECT level, xp, money, map, zone, position_x, position_y, position_z "
                    "FROM characters WHERE guid = %d" % BOT_GUID).strip()
    if not out:
        return None
    lvl, xp, money, mapid, zone, x, y, z = out.split("\t")
    # item_instance owns the ownership link (owner_guid); character_inventory
    # only maps instance guid -> bag/slot in this fork.
    items = []
    for row in p.db_exec(base, env,
                         "SELECT guid, itemEntry, count FROM item_instance "
                         "WHERE owner_guid = %d AND itemEntry = %d ORDER BY guid"
                         % (BOT_GUID, ITEM_ID)).strip().splitlines():
        items.append(row)
    # The rewarded row persists as COMPLETE (fork enum 1) with rewarded=1
    # and the per-objective mob counts (7x 2031, 4x 1984).
    quest = p.db_exec(base, env,
                      "SELECT status, rewarded, mobcount1, mobcount2 "
                      "FROM character_queststatus WHERE guid = %d AND quest = %s"
                      % (BOT_GUID, QUEST_ID)).strip()
    return {
        "level": int(lvl), "xp": int(xp), "money": int(money),
        "map": int(mapid), "zone": int(zone),
        "pos": (float(x), float(y), float(z)),
        "items": items,
        "quest": quest,
        "ownership": p.db_exec(base, env,
                               "SELECT account_id, provision_version FROM bot_ownership "
                               "WHERE char_guid = %d" % BOT_GUID).strip(),
        "roster": p.db_int(base, env,
                           "SELECT COUNT(*) FROM playerbot WHERE char_guid = %d" % BOT_GUID),
    }


class BotRestoreTests(unittest.TestCase):
    base_a = env_a = project_a = evidence_a = None
    base_b = env_b = project_b = evidence_b = None
    logs_a = logs_b = ""
    s1 = s_b = s2b = None
    backup_name = None
    guard_rejected = False
    guard_stderr = ""
    personal_before = {}
    personal_after = {}

    @staticmethod
    def _personal_state():
        try:
            out = p.command(["docker", "inspect",
                             "--format", "{{.Name}}|{{.State.StartedAt}}|{{.State.Running}}",
                             *PERSONAL_CONTAINERS], timeout=60)
        except RuntimeError:
            return {}
        state = {}
        for line in out.splitlines():
            parts = line.split("|")
            if len(parts) == 3:
                state[parts[0].lstrip("/")] = (parts[1], parts[2])
        return state

    @classmethod
    def setUpClass(cls):
        p.IMAGE = os.environ.get("MVP008_LAB_IMAGE", "tortoise-local:dev")
        try:
            p.command(["docker", "image", "inspect", p.IMAGE])
        except RuntimeError:
            raise unittest.SkipTest("%s image not present; build it first" % p.IMAGE)
        cls.personal_before = cls._personal_state()
        world = p.world_env_for(BOT_NAME)
        world.update(PLAYERBOT_MIN_BOTS="0", PLAYERBOT_MAX_BOTS="0",
                     PLAYERBOT_REFRESH="600000", PLAYERBOT_UPDATE_MS="1000",
                     PLAYERBOT_PROVISION="", PLAYERBOT_TEST_LOGIN=str(BOT_GUID),
                     PLAYERBOT_QUEST_ID=QUEST_ID, PLAYER_SAVE_INTERVAL="5000")
        try:
            # --- Lab A: earn, clean stop, snapshot S1, backup ---
            cls.project_a = "tortoise-bot-restore-a-" + uuid.uuid4().hex[:12]
            stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
            cls.evidence_a = p.ROOT / "local" / (cls.project_a + "-" + stamp)
            cls.evidence_a.mkdir(parents=True)
            (cls.evidence_a / "backups").mkdir()
            extra_a = [p.bind("docker", "/ops"),
                       p.bind_abs(cls.evidence_a / "backups", "/backups", read_only=False)]
            cls.base_a, cls.env_a = p.boot_lab(cls.project_a, cls.evidence_a, world,
                                               _seed_sql(), extra_db_volumes=extra_a)
            p.command(["docker", "compose"] + cls.base_a + ["up", "-d", "--no-deps", "world"],
                      env=cls.env_a)
            cls.logs_a = p.wait_for(cls.base_a, cls.env_a,
                                    lambda text: "quest rewarded GUID:%d quest:%s" % (BOT_GUID, QUEST_ID) in text,
                                    deadline=420)
            # The quest phase machine preempts the normal combat loop, so
            # gate the clean stop on both earnings being in the log.
            cls.logs_a = p.wait_for(cls.base_a, cls.env_a,
                                    lambda text: "corpse loot stored GUID:%d item:%d before:0 after:1" % (BOT_GUID, ITEM_ID) in text,
                                    deadline=240)
            # Let the rewarded state land in a periodic save before the
            # clean stop; the row persists afterwards with rewarded=1.
            end = time.monotonic() + 60
            while time.monotonic() < end and not _quest_rewarded_saved(cls.base_a, cls.env_a):
                time.sleep(3)
            p.command(["docker", "compose"] + cls.base_a + ["stop", "world"], env=cls.env_a, timeout=180)
            (cls.evidence_a / "world.log").write_text(
                p.command(["docker", "compose"] + cls.base_a + ["logs", "--no-color", "world"],
                          env=cls.env_a, timeout=60),
                encoding="utf-8")
            cls.s1 = _state(cls.base_a, cls.env_a)
            (cls.evidence_a / "s1.txt").write_text(repr(cls.s1) + "\n", encoding="utf-8")
            # Back up the lab's own db through the same script the personal
            # server uses; the dump and sha256 sidecar land in Lab A's
            # evidence directory.
            out = p.command(["docker", "compose"] + cls.base_a +
                            ["exec", "-T", "db", "bash", "/ops/backup.sh"],
                            env=cls.env_a, timeout=900)
            for line in out.splitlines():
                if line.startswith("Backup saved: "):
                    cls.backup_name = line[len("Backup saved: "):].rsplit("/", 1)[-1].strip()
            if not cls.backup_name:
                raise AssertionError("backup.sh did not report a saved file: %r" % out)
            sql_file = cls.evidence_a / "backups" / cls.backup_name
            sha_file = cls.evidence_a / "backups" / (cls.backup_name + ".sha256")
            if not (sql_file.is_file() and sha_file.is_file()):
                raise AssertionError("backup artifacts missing: %s" % sql_file)
            (cls.evidence_a / "backup.txt").write_text(out + "\n", encoding="utf-8")
            p.teardown_lab(cls.base_a, cls.env_a, cls.project_a, cls.evidence_a)
            cls.base_a = None

            # --- Lab B: fresh project, restore, compare, world run ---
            cls.project_b = "tortoise-bot-restore-b-" + uuid.uuid4().hex[:12]
            stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
            cls.evidence_b = p.ROOT / "local" / (cls.project_b + "-" + stamp)
            cls.evidence_b.mkdir(parents=True)
            (cls.evidence_b / "backups").mkdir()
            shutil.copy2(sql_file, cls.evidence_b / "backups" / cls.backup_name)
            shutil.copy2(sha_file, cls.evidence_b / "backups" / (cls.backup_name + ".sha256"))
            extra_b = [p.bind("docker", "/ops"),
                       p.bind_abs(cls.evidence_b / "backups", "/backups", read_only=True)]
            cls.base_b, cls.env_b = p.boot_lab(cls.project_b, cls.evidence_b, world,
                                               None, extra_db_volumes=extra_b)
            # Fail-closed guard: a filename backup.sh never produced must be
            # rejected before the db is touched.
            try:
                p.command(["docker", "compose"] + cls.base_b + ["exec", "-T", "db",
                         "bash", "/ops/restore-check.sh", "not-a-backup.sql"],
                          env=cls.env_b, timeout=120)
                cls.guard_rejected = False
            except RuntimeError as exc:
                cls.guard_rejected = True
                cls.guard_stderr = str(exc)
            out = p.command(["docker", "compose"] + cls.base_b + ["exec", "-T", "db",
                          "bash", "/ops/restore-check.sh", cls.backup_name],
                          env=cls.env_b, timeout=1800)
            (cls.evidence_b / "restore-check.txt").write_text(out + "\n", encoding="utf-8")
            # Pure row-level comparison before Lab B's world runs: the
            # acceptance criterion (identity, ownership, XP, inventory and
            # quest state must match the source snapshot exactly).
            cls.s_b = _state(cls.base_b, cls.env_b)
            (cls.evidence_b / "sb.txt").write_text(repr(cls.s_b) + "\n", encoding="utf-8")
            p.command(["docker", "compose"] + cls.base_b + ["up", "-d", "--no-deps", "world"],
                      env=cls.env_b)
            cls.logs_b = p.wait_for(cls.base_b, cls.env_b,
                                    lambda text: "quest init GUID:%d quest:%s phase:3" % (BOT_GUID, QUEST_ID) in text,
                                    deadline=420)
            cls.logs_b = p.wait_for(cls.base_b, cls.env_b,
                                    lambda text: "quest reward denied GUID:%d quest:%s" % (BOT_GUID, QUEST_ID) in text,
                                    deadline=240)
            # Quiet window so periodic saves settle, as in MVP-007.
            end = time.monotonic() + 60
            while time.monotonic() < end:
                time.sleep(3)
            p.command(["docker", "compose"] + cls.base_b + ["stop", "world"], env=cls.env_b, timeout=180)
            (cls.evidence_b / "world.log").write_text(
                p.command(["docker", "compose"] + cls.base_b + ["logs", "--no-color", "world"],
                          env=cls.env_b, timeout=60),
                encoding="utf-8")
            cls.s2b = _state(cls.base_b, cls.env_b)
            (cls.evidence_b / "s2b.txt").write_text(repr(cls.s2b) + "\n", encoding="utf-8")
            p.teardown_lab(cls.base_b, cls.env_b, cls.project_b, cls.evidence_b)
            cls.base_b = None
        except BaseException:
            for base, env, ev in ((cls.base_a, cls.env_a, cls.evidence_a),
                                  (cls.base_b, cls.env_b, cls.evidence_b)):
                if base is not None and ev is not None:
                    try:
                        failure_logs = p.command(["docker", "compose"] + base +
                                                 ["logs", "--no-color", "world"],
                                                 env=env, timeout=60)
                        (ev / "world-failure.log").write_text(failure_logs, encoding="utf-8")
                    except Exception:
                        pass
                    p.force_down(base, env)
            raise
        # Both labs are fully torn down here; record the personal server's
        # state after the entire run (the test asserting on it runs before
        # tearDownClass, so the capture must happen here).
        cls.personal_after = cls._personal_state()

    def test_lab_a_earned_markers(self):
        self.assertIn("quest accepted GUID:%d quest:%s" % (BOT_GUID, QUEST_ID), self.logs_a)
        self.assertIn("quest objective complete GUID:%d quest:%s" % (BOT_GUID, QUEST_ID), self.logs_a)
        self.assertIn("quest rewarded GUID:%d quest:%s" % (BOT_GUID, QUEST_ID), self.logs_a)
        self.assertIn("corpse loot stored GUID:%d item:%d before:0 after:1" % (BOT_GUID, ITEM_ID), self.logs_a)

    def test_source_snapshot_is_exact_earned_state(self):
        s = self.s1
        self.assertIsNotNone(s, "source snapshot missing; evidence: %s" % self.evidence_a)
        self.assertEqual(len(s["items"]), 1, "earned item missing from source snapshot")
        self.assertEqual(s["quest"], EXPECTED_QUEST_ROW,
                         "rewarded quest row must be COMPLETE, rewarded, with persisted 7x2031 + 4x1984")
        self.assertEqual(s["ownership"], "1000500106\t2")
        self.assertEqual(s["roster"], 1)

    def test_restore_guard_rejects_foreign_filename(self):
        self.assertTrue(self.guard_rejected,
                        "restore-check.sh must reject a filename backup.sh never produced")
        self.assertIn("Expected a backup filename", self.guard_stderr)

    def test_restore_matches_source_snapshot(self):
        self.assertIsNotNone(self.s_b, "restored snapshot missing; evidence: %s" % self.evidence_b)
        self.assertEqual(self.s_b, self.s1,
                         "restored rows must match the source snapshot exactly")

    def test_restored_world_resumes_and_fails_closed(self):
        self.assertIn("[PlayerBot][Login]  '%s' GUID:%d" % (BOT_NAME, BOT_GUID), self.logs_b)
        self.assertIn("quest init GUID:%d quest:%s phase:3" % (BOT_GUID, QUEST_ID), self.logs_b)
        self.assertIn("quest reward denied GUID:%d quest:%s" % (BOT_GUID, QUEST_ID), self.logs_b)
        self.assertNotIn("quest rewarded", self.logs_b,
                         "a restored rewarded quest must never be rewarded twice")

    def test_state_survives_restored_world_run(self):
        self.assertIsNotNone(self.s2b)
        for key in ("level", "xp", "money", "map", "zone", "items", "quest", "ownership", "roster"):
            self.assertEqual(self.s2b[key], self.s1[key], "%s changed in the restored lab" % key)
        x1, y1, z1 = self.s1["pos"]
        x2, y2, z2 = self.s2b["pos"]
        dist = ((x1 - x2) ** 2 + (y1 - y2) ** 2 + (z1 - z2) ** 2) ** 0.5
        self.assertLessEqual(dist, 50.0)

    def test_labs_stayed_healthy(self):
        self.assertNotIn("[CRASH]", self.logs_a)
        self.assertNotIn("[CRASH]", self.logs_b)
        for evidence, label in ((self.evidence_a, "lab-a"), (self.evidence_b, "lab-b")):
            world_log = (evidence / "world.log").read_text(encoding="utf-8")
            self.assertNotIn("Received SIGSEGV", world_log,
                             "%s clean stop crashed the world" % label)

    def test_personal_volume_untouched(self):
        self.assertNotEqual(self.project_a, "tortoise-local")
        self.assertNotEqual(self.project_b, "tortoise-local")
        if self.personal_before:
            self.assertEqual(self.personal_after, self.personal_before,
                             "personal server containers must not be restarted or touched")


if __name__ == "__main__":
    unittest.main()
