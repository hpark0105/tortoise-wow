"""MVP-004: prove an owned bot earns XP through normal creature combat."""
import os
import time
import unittest
import uuid
from datetime import datetime, timezone

import test_bot_provision as p


class BotCombatXpTests(unittest.TestCase):
    base = env = project = evidence = None
    logs = ""
    before_xp = after_xp = 0

    @classmethod
    def setUpClass(cls):
        p.IMAGE = os.environ.get("MVP004_LAB_IMAGE", "tortoise-local:mvp003-review")
        try:
            p.command(["docker", "image", "inspect", p.IMAGE])
        except RuntimeError:
            raise unittest.SkipTest(f"{p.IMAGE} image not present; build it first")
        cls.project = "tortoise-bot-xp-" + uuid.uuid4().hex[:12]
        cls.evidence = p.ROOT / "local" / (cls.project + "-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ"))
        cls.evidence.mkdir(parents=True)
        seed = """
INSERT INTO tw_char.characters
 (guid,account,name,race,class,gender,level,money,position_x,position_y,position_z,map,
  orientation,zone,health,power1,power2,power3,power4,power5,xp,xp_gain)
VALUES
 (500101,1000500101,'Combatxp',1,1,0,3,100000,-8949.95,-132.493,83.5312,0,
  0,12,100,0,0,0,0,0,0,1);
INSERT INTO tw_char.playerbot (char_guid,chance,ai) VALUES (500101,100,'Default');
INSERT INTO tw_char.bot_ownership (char_guid,account_id,bot_type,provision_version)
 VALUES (500101,1000500101,1,2);
INSERT INTO tw_world.creature
 (guid,id,map,position_x,position_y,position_z,orientation,spawntimesecsmin,
  spawntimesecsmax,wander_distance,health_percent,mana_percent,movement_type,spawn_flags)
VALUES (2500001,6,0,-8945.95,-132.493,83.5312,0,600,600,0,25,100,0,1);
"""
        world = p.world_env_for("Combatxp")
        world.update(PLAYERBOT_ENABLE="1", PLAYERBOT_MIN_BOTS="0", PLAYERBOT_MAX_BOTS="0",
                     PLAYERBOT_REFRESH="600000", PLAYERBOT_UPDATE_MS="1000", PLAYERBOT_DEBUG="1",
                     PLAYERBOT_PROVISION="", PLAYERBOT_TEST_LOGIN="500101")
        try:
            cls.base, cls.env = p.boot_lab(cls.project, cls.evidence, world, seed)
            cls.before_xp = p.db_int(cls.base, cls.env, "SELECT xp FROM characters WHERE guid=500101")
            p.command(["docker", "compose"] + cls.base + ["up", "-d", "--no-deps", "world"], env=cls.env)
            cls.logs = p.wait_for(cls.base, cls.env,
                                  lambda text: "[PlayerBot][Login]  'Combatxp' GUID:500101" in text,
                                  deadline=600)
            deadline = time.monotonic() + 180
            while time.monotonic() < deadline:
                cls.after_xp = p.db_int(cls.base, cls.env, "SELECT xp FROM characters WHERE guid=500101")
                if cls.after_xp > cls.before_xp:
                    break
                time.sleep(3)
            cls.logs = p.command(["docker", "compose"] + cls.base + ["logs", "--no-color", "world"], env=cls.env, timeout=60)
            (cls.evidence / "world.log").write_text(cls.logs, encoding="utf-8")
            (cls.evidence / "xp.txt").write_text(
                f"bot=Combatxp guid=500101 creature=6 before={cls.before_xp} after={cls.after_xp}\n",
                encoding="utf-8")
        except BaseException:
            if cls.base is not None:
                p.force_down(cls.base, cls.env)
            raise

    @classmethod
    def tearDownClass(cls):
        if cls.base is not None:
            p.teardown_lab(cls.base, cls.env, cls.project, cls.evidence)

    def test_normal_combat_awards_xp(self):
        self.assertIn("[PlayerBot][Login]  'Combatxp' GUID:500101", self.logs)
        self.assertGreater(self.after_xp, self.before_xp)
        self.assertNotIn("[CRASH]", self.logs)


if __name__ == "__main__":
    unittest.main()
