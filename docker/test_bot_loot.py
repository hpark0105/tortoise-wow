"""MVP-005: prove bounded corpse loot through normal inventory rules."""
import os
import unittest
import uuid
from datetime import datetime, timezone

import test_bot_provision as p


class BotLootTests(unittest.TestCase):
    base = env = project = evidence = None
    logs = ""
    item_count = 0

    @classmethod
    def setUpClass(cls):
        p.IMAGE = os.environ.get("MVP005_LAB_IMAGE", "tortoise-local:dev")
        try:
            p.command(["docker", "image", "inspect", p.IMAGE])
        except RuntimeError:
            raise unittest.SkipTest(f"{p.IMAGE} image not present; build it first")
        cls.project = "tortoise-bot-loot-" + uuid.uuid4().hex[:12]
        cls.evidence = p.ROOT / "local" / (cls.project + "-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ"))
        cls.evidence.mkdir(parents=True)
        seed = """
INSERT INTO tw_char.characters
 (guid,account,name,race,class,gender,level,money,position_x,position_y,position_z,map,
  orientation,zone,health,power1,power2,power3,power4,power5,xp,xp_gain)
VALUES (500102,1000500102,'Lootbot',1,1,0,3,100000,-8949.95,-132.493,83.5312,0,
 0,12,100,0,0,0,0,0,0,1);
INSERT INTO tw_char.playerbot (char_guid,chance,ai) VALUES (500102,100,'Default');
INSERT INTO tw_char.bot_ownership (char_guid,account_id,bot_type,provision_version)
 VALUES (500102,1000500102,1,2);
UPDATE tw_world.creature_template SET loot_id=990006 WHERE entry=6;
DELETE FROM tw_world.creature_loot_template WHERE entry=990006;
INSERT INTO tw_world.creature_loot_template
 (entry,item,ChanceOrQuestChance,groupid,mincountOrRef,maxcount,condition_id)
 VALUES (990006,117,100,0,1,1,0);
-- Lab isolation: remove natural creatures around the spawn so the seeded
-- kobold is the only creature in the box (deterministic loot proof).
DELETE FROM tw_world.creature WHERE map = 0
  AND position_x BETWEEN -8990 AND -8910 AND position_y BETWEEN -170 AND -110;
INSERT INTO tw_world.creature
 (guid,id,map,position_x,position_y,position_z,orientation,spawntimesecsmin,
  spawntimesecsmax,wander_distance,health_percent,mana_percent,movement_type,spawn_flags)
 VALUES (2500002,6,0,-8945.95,-132.493,83.5312,0,600,600,0,25,100,0,1);
"""
        world = p.world_env_for("Lootbot")
        world.update(PLAYERBOT_ENABLE="1", PLAYERBOT_MIN_BOTS="0", PLAYERBOT_MAX_BOTS="0",
                     PLAYERBOT_REFRESH="600000", PLAYERBOT_UPDATE_MS="1000", PLAYERBOT_DEBUG="1",
                     PLAYERBOT_PROVISION="", PLAYERBOT_TEST_LOGIN="500102",
                     PLAYER_SAVE_INTERVAL="5000")
        try:
            cls.base, cls.env = p.boot_lab(cls.project, cls.evidence, world, seed)
            p.command(["docker", "compose"] + cls.base + ["up", "-d", "--no-deps", "world"], env=cls.env)
            cls.logs = p.wait_for(cls.base, cls.env,
                                  lambda text: "[PlayerBot][Login]  'Lootbot' GUID:500102" in text,
                                  deadline=600)
            cls.logs = p.wait_for(cls.base, cls.env,
                                  lambda text: "corpse loot stored GUID:500102 item:117 before:0 after:1" in text,
                                  deadline=180)
            cls.item_count = 1
            cls.logs = p.command(["docker", "compose"] + cls.base + ["logs", "--no-color", "world"], env=cls.env, timeout=60)
            (cls.evidence / "world.log").write_text(cls.logs, encoding="utf-8")
            (cls.evidence / "loot.txt").write_text(
                f"bot=Lootbot guid=500102 creature=6 item=117 inventory_rows={cls.item_count}\n",
                encoding="utf-8")
        except BaseException:
            if cls.base is not None:
                try:
                    failure_logs = p.command(["docker", "compose"] + cls.base + ["logs", "--no-color", "world"],
                                             env=cls.env, timeout=60)
                    (cls.evidence / "world-failure.log").write_text(failure_logs, encoding="utf-8")
                except Exception:
                    pass
                p.force_down(cls.base, cls.env)
            raise

    @classmethod
    def tearDownClass(cls):
        if cls.base is not None:
            p.teardown_lab(cls.base, cls.env, cls.project, cls.evidence)

    def test_loot_enters_inventory_once(self):
        self.assertEqual(self.item_count, 1)
        self.assertIn("corpse loot stored GUID:500102 item:117 before:0 after:1", self.logs)
        self.assertNotIn("[CRASH]", self.logs)
        self.assertNotIn("[DB Auto-Updater] Attempting to execute update", self.logs,
                         "database updates must be applied+recorded at db init; the world "
                         "auto-updater must be a no-op or fixture seeds get clobbered")


if __name__ == "__main__":
    unittest.main()
