"""Disposable proof that an ungrouped owned companion shares citizen activity."""
import os
import unittest
import uuid
from datetime import datetime, timezone

import test_bot_provision as p
import test_bot_companion_defend as d


class OwnedActivityTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        p.IMAGE = os.environ.get("OWNED_ACTIVITY_LAB_IMAGE", "tortoise-local:dev")
        try:
            p.command(["docker", "image", "inspect", p.IMAGE])
        except RuntimeError:
            raise unittest.SkipTest(f"{p.IMAGE} image not present; build it first")
        cls.project = "tortoise-owned-activity-" + uuid.uuid4().hex[:12]
        stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        cls.evidence = p.ROOT / "local" / (cls.project + "-" + stamp)
        cls.evidence.mkdir(parents=True)
        cls.base = cls.env = None
        world = p.world_env_for("")
        world.update(PLAYERBOT_MIN_BOTS="0", PLAYERBOT_MAX_BOTS="0",
                     PLAYERBOT_REFRESH="600000", PLAYERBOT_UPDATE_MS="1000",
                     PLAYERBOT_TEST_LOGIN=f"{d.OWNER_GUID},{d.COMP_GUID}",
                     PLAYERBOT_AMBIENT_ACQUIRE="0", PLAYERBOT_DEBUG="1")
        seed = d._seed_sql() + f"""
UPDATE tw_char.characters SET level=1,health=160 WHERE guid IN ({d.OWNER_GUID},{d.COMP_GUID});
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
                lambda output: f"[ZoneCitizen] hunt guid:{d.COMP_GUID} target:{d.ATTACKER_GUID}" in output,
                deadline=420)
            cls.logs = p.wait_for(
                cls.base, cls.env,
                lambda output: f"[PlayerBot] fighting GUID:{d.COMP_GUID} victim:{d.ATTACKER_GUID}" in output,
                deadline=120)
            cls.binding = p.db_exec(
                cls.base, cls.env,
                f"SELECT owner_account_id FROM bot_ownership WHERE char_guid={d.COMP_GUID}")
            (cls.evidence / "world.log").write_text(cls.logs, encoding="utf-8")
            p.teardown_lab(cls.base, cls.env, cls.project, cls.evidence)
            cls.base = None
        except BaseException:
            if cls.base is not None:
                p.force_down(cls.base, cls.env)
            raise

    def test_owned_companion_hunts_without_ambient_acquire(self):
        self.assertEqual(self.binding.strip(), str(d.OWNER_ACC))
        self.assertIn(f"[ZoneCitizen] hunt guid:{d.COMP_GUID} target:{d.ATTACKER_GUID}",
                      self.logs)
        self.assertIn(f"[PlayerBot] fighting GUID:{d.COMP_GUID} victim:{d.ATTACKER_GUID}",
                      self.logs)


if __name__ == "__main__":
    unittest.main()
