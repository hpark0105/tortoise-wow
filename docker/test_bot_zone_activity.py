"""Disposable solo hunt and travel proof for an independent zone citizen."""
import os
import unittest
import uuid
from datetime import datetime, timezone

import test_bot_provision as p
import test_bot_companion_defend as d


class ZoneCitizenActivityTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        p.IMAGE = os.environ.get("ZONE_ACTIVITY_LAB_IMAGE", "tortoise-local:dev")
        p.command(["docker", "image", "inspect", p.IMAGE])
        cls.project = "tortoise-zone-activity-" + uuid.uuid4().hex[:12]
        stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        cls.evidence = p.ROOT / "local" / (cls.project + "-" + stamp)
        cls.evidence.mkdir(parents=True)
        cls.base = cls.env = None
        world = p.world_env_for("")
        world.update(PLAYERBOT_MIN_BOTS="0", PLAYERBOT_MAX_BOTS="0",
                     PLAYERBOT_REFRESH="600000", PLAYERBOT_UPDATE_MS="1000",
                     PLAYERBOT_TEST_LOGIN=str(d.COMP_GUID),
                     PLAYERBOT_AMBIENT_ACQUIRE="0", PLAYERBOT_DEBUG="1")
        seed = d._seed_sql() + f"""
UPDATE tw_char.playerbot SET ai='ZoneCitizenAI' WHERE char_guid={d.COMP_GUID};
UPDATE tw_char.bot_ownership SET owner_account_id=NULL WHERE char_guid={d.COMP_GUID};
UPDATE tw_char.characters SET level=1,health=160 WHERE guid={d.COMP_GUID};
UPDATE tw_world.creature_template SET level_min=1,level_max=1,
 health_min=50,health_max=50,regeneration=0 WHERE entry=51600;
UPDATE tw_world.creature SET position_x=-8946.95,position_y=-175.493,position_z=86.0
 WHERE guid={d.ATTACKER_GUID};
"""
        try:
            cls.base, cls.env = p.boot_lab(cls.project, cls.evidence, world, seed)
            p.command(["docker", "compose"] + cls.base +
                      ["up", "-d", "--no-deps", "world"], env=cls.env)
            cls.logs = p.wait_for(
                cls.base, cls.env,
                lambda text: f"[ZoneCitizen] hunt guid:{d.COMP_GUID} target:{d.ATTACKER_GUID}" in text,
                deadline=420)
            cls.logs = p.wait_for(
                cls.base, cls.env,
                lambda text: f"[ZoneCitizen] travel guid:{d.COMP_GUID} zone:12" in text,
                deadline=180)
            cls.logs = p.wait_for(
                cls.base, cls.env,
                lambda text: f"[PlayerBot] fighting GUID:{d.COMP_GUID} victim:{d.ATTACKER_GUID}" in text,
                deadline=120)
            cls.owner_binding = p.db_exec(
                cls.base, cls.env,
                f"SELECT COALESCE(owner_account_id,0) FROM bot_ownership WHERE char_guid={d.COMP_GUID}")
            (cls.evidence / "world.log").write_text(cls.logs, encoding="utf-8")
            p.teardown_lab(cls.base, cls.env, cls.project, cls.evidence)
            cls.base = None
        except BaseException:
            if cls.base is not None:
                p.force_down(cls.base, cls.env)
            raise

    def test_unowned_citizen_hunts_and_travels_without_ambient_acquire(self):
        self.assertEqual(self.owner_binding.strip(), "0")
        hunt = self.logs.find(f"[ZoneCitizen] hunt guid:{d.COMP_GUID} target:{d.ATTACKER_GUID}")
        travel = self.logs.find(f"[ZoneCitizen] travel guid:{d.COMP_GUID} zone:12")
        self.assertGreaterEqual(hunt, 0)
        self.assertGreaterEqual(travel, 0)
        self.assertIn(f"[PlayerBot] fighting GUID:{d.COMP_GUID} victim:{d.ATTACKER_GUID}",
                      self.logs)


if __name__ == "__main__":
    unittest.main()
