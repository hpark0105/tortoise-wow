"""Disposable normal-invite follow/defend proof for an unowned zone citizen."""
import os
import unittest
import uuid
from datetime import datetime, timezone

import test_bot_provision as p
import test_bot_companion_defend as d


class ZoneCitizenRecruitDefendTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        p.IMAGE = os.environ.get("ZONE_RECRUIT_LAB_IMAGE", "tortoise-local:dev")
        p.command(["docker", "image", "inspect", p.IMAGE])
        cls.project = "tortoise-zone-recruit-" + uuid.uuid4().hex[:12]
        stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        cls.evidence = p.ROOT / "local" / (cls.project + "-" + stamp)
        cls.evidence.mkdir(parents=True)
        cls.base = cls.env = None
        world = p.world_env_for("")
        world.update(PLAYERBOT_MIN_BOTS="0", PLAYERBOT_MAX_BOTS="0",
                     PLAYERBOT_REFRESH="600000", PLAYERBOT_UPDATE_MS="1000",
                     PLAYERBOT_TEST_LOGIN=f"{d.OWNER_GUID},{d.COMP_GUID}",
                     PLAYERBOT_ZONE_WORLD_TEST_ANCHOR_GUID=str(d.OWNER_GUID),
                     PLAYERBOT_PARTY_INVITE_SCRIPT=(
                         f"2000:{d.OWNER_GUID}:Defendcomp"),
                     PLAYERBOT_AMBIENT_ACQUIRE="1", PLAYERBOT_DEBUG="1")
        seed = d._seed_sql() + f"""
UPDATE tw_char.playerbot SET ai='ZoneCitizenAI' WHERE char_guid={d.COMP_GUID};
UPDATE tw_char.bot_ownership SET owner_account_id=NULL WHERE char_guid={d.COMP_GUID};
"""
        try:
            cls.base, cls.env = p.boot_lab(cls.project, cls.evidence, world, seed)
            p.command(["docker", "compose"] + cls.base +
                      ["up", "-d", "--no-deps", "world"], env=cls.env)
            cls.logs = p.wait_for(
                cls.base, cls.env,
                lambda text: f"[ZoneCitizen] recruited bot:Defendcomp guid:{d.COMP_GUID}" in text,
                deadline=420)
            cls.logs = p.wait_for(
                cls.base, cls.env,
                lambda text: f"[PlayerBot][Follow] path GUID:{d.COMP_GUID} leader:{d.OWNER_GUID}" in text,
                deadline=90)
            cls.logs = p.wait_for(
                cls.base, cls.env,
                lambda text: f"[PlayerBot][Defend] fighting GUID:{d.COMP_GUID} target:{d.ATTACKER_GUID}" in text,
                deadline=120)
            cls.ownership = p.db_exec(
                cls.base, cls.env,
                f"SELECT COALESCE(owner_account_id,0) FROM bot_ownership WHERE char_guid={d.COMP_GUID}")
            (cls.evidence / "world.log").write_text(cls.logs, encoding="utf-8")
            p.teardown_lab(cls.base, cls.env, cls.project, cls.evidence)
            cls.base = None
        except BaseException:
            if cls.base is not None:
                p.force_down(cls.base, cls.env)
            raise

    def test_unowned_citizen_follows_and_defends_only_during_invite(self):
        self.assertEqual(self.ownership.strip(), "0")
        self.assertIn(f"[ZoneCitizen] recruited bot:Defendcomp guid:{d.COMP_GUID}", self.logs)
        self.assertIn(f"[PlayerBot][Follow] path GUID:{d.COMP_GUID} leader:{d.OWNER_GUID}", self.logs)
        self.assertIn(f"[PlayerBot][Defend] fighting GUID:{d.COMP_GUID} target:{d.ATTACKER_GUID}", self.logs)


if __name__ == "__main__":
    unittest.main()
