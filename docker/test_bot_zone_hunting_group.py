"""Disposable proof that nearby, similar-level citizens form a hunting party."""
import os
import re
import unittest
import uuid
from datetime import datetime, timezone

import test_bot_provision as p
import test_bot_companion_defend as d


class ZoneCitizenHuntingGroupTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        p.IMAGE = os.environ.get("ZONE_GROUP_LAB_IMAGE", "tortoise-local:dev")
        try:
            p.command(["docker", "image", "inspect", p.IMAGE])
        except RuntimeError:
            raise unittest.SkipTest(f"{p.IMAGE} image not present; build it first")
        cls.project = "tortoise-zone-group-" + uuid.uuid4().hex[:12]
        stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        cls.evidence = p.ROOT / "local" / (cls.project + "-" + stamp)
        cls.evidence.mkdir(parents=True)
        cls.base = cls.env = None
        world = p.world_env_for("")
        world.update(PLAYERBOT_MIN_BOTS="0", PLAYERBOT_MAX_BOTS="0",
                     PLAYERBOT_REFRESH="600000", PLAYERBOT_UPDATE_MS="1000",
                     PLAYERBOT_TEST_LOGIN=f"{d.COMP_GUID},{d.STRANGER_GUID}",
                     PLAYERBOT_AMBIENT_ACQUIRE="0", PLAYERBOT_DEBUG="1")
        seed = d._seed_sql() + f"""
UPDATE tw_char.playerbot SET ai='ZoneCitizenAI'
 WHERE char_guid IN ({d.COMP_GUID},{d.STRANGER_GUID});
UPDATE tw_char.bot_ownership SET owner_account_id=NULL
 WHERE char_guid IN ({d.COMP_GUID},{d.STRANGER_GUID});
UPDATE tw_char.characters SET level=1,health=160,position_x=-8949.95,
 position_y=-175.493,position_z=86.0 WHERE guid={d.COMP_GUID};
UPDATE tw_char.characters SET level=1,health=160,position_x=-8960.95,
 position_y=-175.493,position_z=86.0 WHERE guid={d.STRANGER_GUID};
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
                lambda text: "[ZoneCitizen][HuntingGroup] formed group:" in text
                and "members:2" in text,
                deadline=120)
            formed = re.search(
                r"\[ZoneCitizen\]\[HuntingGroup\] formed group:(\d+) leader:(\d+) "
                r"target:(\d+) members:2",
                cls.logs)
            if not formed:
                raise AssertionError("two-member hunting group formation line was malformed")
            group_id, leader_guid, first_target = formed.groups()
            cls.logs = p.wait_for(
                cls.base, cls.env,
                lambda text: f"[PlayerBot] corpse loot processed GUID:" in text
                and f"target:{first_target}" in text,
                deadline=180)
            cls.group_rows_after_combat = p.db_exec(
                cls.base, cls.env,
                f"SELECT COUNT(*) FROM group_member WHERE groupId={group_id}")
            (cls.evidence / "world.log").write_text(cls.logs, encoding="utf-8")
            p.teardown_lab(cls.base, cls.env, cls.project, cls.evidence)
            cls.base = None
        except BaseException:
            if cls.base is not None:
                p.force_down(cls.base, cls.env)
            raise

    def test_nearby_citizens_form_a_two_member_party(self):
        joined = re.search(
            r"\[ZoneCitizen\]\[HuntingGroup\] member-joined group:(\d+) guid:(\d+)",
            self.logs)
        formed = re.search(
            r"\[ZoneCitizen\]\[HuntingGroup\] formed group:(\d+) leader:(\d+) "
            r"target:(\d+) members:2",
            self.logs)
        self.assertIsNotNone(joined)
        self.assertIsNotNone(formed)
        self.assertEqual(joined.group(1), formed.group(1))
        self.assertEqual(
            {int(joined.group(2)), int(formed.group(2))},
            {d.COMP_GUID, d.STRANGER_GUID})
        self.assertIn(f"target:{formed.group(3)}", self.logs)
        self.assertEqual(self.group_rows_after_combat.strip(), "2")


if __name__ == "__main__":
    unittest.main()
