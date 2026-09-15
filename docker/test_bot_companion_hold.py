"""PORT-004: real owner commands, hold cancellation and stale-goal rejection.

Uses only synthetic follow fixtures in a unique port-free Compose project.
No human play data or personal-world restart. This does not prove combat roles.
"""
import os
import unittest
import uuid
from datetime import datetime, timezone

import test_bot_provision as p
import test_bot_follow as f


class BotCompanionHoldTests(unittest.TestCase):
    def test_hold_invalidates_prior_orders_and_allows_new_follow(self):
        p.IMAGE = os.environ.get("PORT004_LAB_IMAGE", "tortoise-local:dev")
        try:
            image_id = p.command(["docker", "image", "inspect", "--format", "{{.Id}}", p.IMAGE])
        except RuntimeError:
            self.skipTest("Build the candidate image first")
        before = f.BotFollowTests._personal_state()
        project = "tortoise-bot-hold-" + uuid.uuid4().hex[:12]
        evidence = p.ROOT / "local" / (project + "-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ"))
        evidence.mkdir(parents=True)
        (evidence / "image-id.txt").write_text(image_id, encoding="utf-8")
        world = p.world_env_for("")
        world.update(PLAYERBOT_MIN_BOTS="0", PLAYERBOT_MAX_BOTS="0",
                     PLAYERBOT_REFRESH="600000", PLAYERBOT_UPDATE_MS="1000",
                     PLAYERBOT_TEST_LOGIN="500110,500111,500120",
                     PLAYERBOT_QUEST_ID="0", PLAYER_SAVE_INTERVAL="5000",
                     PLAYERBOT_FOLLOW_SCRIPT=";".join([
                         "5000:500110:botfollow Followcomp",
                         "15000:500110:bothold Followcomp",
                         "20000:stale:Followcomp:500110:2",
                         "25000:500120:bothold Followcomp",
                         "30000:500110:botfollow Followcomp",
                         "45000:500110:bothyld Followcomp",
                         "50000:stale:Followcomp:500110:3",
                         "60000:500110:botfollow Followcomp"]))
        base = env = None
        try:
            base, env = p.boot_lab(project, evidence, world, f._seed_sql())
            p.command(["docker", "compose"] + base + ["up", "-d", "--no-deps", "world"], env=env)
            logs = p.wait_for(base, env, lambda text:
                "[PlayerBot][Follow] active GUID:500111 leader:500110 seq:5" in text, deadline=480)
            (evidence / "world.log").write_text(logs, encoding="utf-8")
            self.assertIn("[PlayerBot][Hold] active GUID:500111 seq:2", logs)
            self.assertIn("[PlayerBot][Hold] active GUID:500111 seq:4", logs)
            self.assertIn("goal rejected stale seq:2 current:2", logs)
            self.assertIn("goal rejected stale seq:3 current:4", logs)
            self.assertIn("hold rejected not-owner", logs)
            self.assertEqual(logs.count("[PlayerBot][Follow] active GUID:500111"), 3)
            self.assertNotIn("[CRASH]", logs)
        finally:
            if base is not None:
                p.teardown_lab(base, env, project, evidence)
        self.assertEqual(before, f.BotFollowTests._personal_state())


if __name__ == "__main__":
    unittest.main()
