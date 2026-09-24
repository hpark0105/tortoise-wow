"""Disposable proof that a citizen walks toward a suitable hunting ground."""
import os
import unittest
import uuid
from datetime import datetime, timezone

import test_bot_provision as p
import test_bot_companion_defend as d


class ZoneCitizenDestinationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        p.IMAGE = os.environ.get("ZONE_DESTINATION_LAB_IMAGE", "tortoise-local:dev")
        p.command(["docker", "image", "inspect", p.IMAGE])
        cls.project = "tortoise-zone-destination-" + uuid.uuid4().hex[:12]
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
UPDATE tw_world.creature SET position_y=-115.493 WHERE guid={d.ATTACKER_GUID};
"""
        try:
            cls.base, cls.env = p.boot_lab(cls.project, cls.evidence, world, seed)
            p.command(["docker", "compose"] + cls.base +
                      ["up", "-d", "--no-deps", "world"], env=cls.env)
            cls.logs = p.wait_for(
                cls.base, cls.env,
                lambda text: (f"[ZoneCitizen] travel guid:{d.COMP_GUID} zone:12 "
                              f"purpose:hunt target:{d.ATTACKER_GUID}") in text,
                deadline=240)
            cls.logs = p.wait_for(
                cls.base, cls.env,
                lambda text: f"[ZoneCitizen] hunt guid:{d.COMP_GUID} target:{d.ATTACKER_GUID}" in text,
                deadline=180)
            (cls.evidence / "world.log").write_text(cls.logs, encoding="utf-8")
            p.teardown_lab(cls.base, cls.env, cls.project, cls.evidence)
            cls.base = None
        except BaseException:
            if cls.base is not None:
                p.force_down(cls.base, cls.env)
            raise

    def test_citizen_picks_and_reaches_hunting_ground_without_ambient_acquire(self):
        travel = (f"[ZoneCitizen] travel guid:{d.COMP_GUID} zone:12 "
                  f"purpose:hunt target:{d.ATTACKER_GUID}")
        hunt = f"[ZoneCitizen] hunt guid:{d.COMP_GUID} target:{d.ATTACKER_GUID}"
        self.assertIn(travel, self.logs)
        self.assertGreater(self.logs.find(hunt), self.logs.find(travel))


if __name__ == "__main__":
    unittest.main()
