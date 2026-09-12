"""MVP-003: hostile player targets are skipped during idle acquisition.

The lab uses two persistent playerbot rows with opposing factions at the same
Stormwind coordinates.  Both are loaded through the normal login path, so the
debug-only target-skip diagnostic is the observable contract.  The personal
Docker project is never used.
"""
import json
import os
import sys
import time
import unittest
import uuid
from datetime import datetime, timezone

import test_bot_provision as p


class BotTargetPolicyTests(unittest.TestCase):
    base = None
    env = None
    project = None
    evidence = None
    logs = ""

    @classmethod
    def setUpClass(cls):
        p.IMAGE = os.environ.get("MVP003_LAB_IMAGE", "tortoise-local:mvp003-review")
        try:
            p.command(["docker", "image", "inspect", p.IMAGE])
        except RuntimeError:
            raise unittest.SkipTest(f"{p.IMAGE} image not present; build it first")
        cls.project = "tortoise-bot-target-" + uuid.uuid4().hex[:12]
        cls.evidence = p.ROOT / "local" / (cls.project + "-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ"))
        cls.evidence.mkdir(parents=True)
        cls.base = None
        cls.addClassCleanup(cls.cleanup_lab)
        seed = """
INSERT INTO tw_char.characters
  (guid, account, name, race, class, gender, level, money,
   position_x, position_y, position_z, map, orientation, zone, playerFlags, health,
   power1, power2, power3, power4, power5)
VALUES
  (500001, 1000500001, 'Targetally', 1, 1, 0, 10, 100000,
   -8949.95, -132.493, 83.5312, 0, 0, 12, 512, 26, 0, 0, 0, 0, 0),
  (500002, 1000500002, 'Targethorde', 2, 1, 0, 10, 100000,
   -8949.95, -132.493, 83.5312, 0, 0, 12, 512, 26, 0, 0, 0, 0, 0);
INSERT INTO tw_char.playerbot (char_guid, chance, ai) VALUES
  (500001, 100, 'Default'), (500002, 100, 'Default');
INSERT INTO tw_char.bot_ownership (char_guid, account_id, bot_type, provision_version) VALUES
  (500001, 1000500001, 1, 2), (500002, 1000500002, 1, 2);
"""
        world = p.world_env_for("Targetally")
        world.update(PLAYERBOT_ENABLE="1", PLAYERBOT_MIN_BOTS="0", PLAYERBOT_MAX_BOTS="0",
                     PLAYERBOT_REFRESH="600000", PLAYERBOT_UPDATE_MS="1000", PLAYERBOT_DEBUG="1",
                     PLAYERBOT_PROVISION="", PLAYERBOT_TEST_LOGIN="500001,500002")
        try:
            cls.base, cls.env = p.boot_lab(cls.project, cls.evidence, world, seed)
            p.command(["docker", "compose"] + cls.base + ["up", "-d", "--no-deps", "world"], env=cls.env)
            cls.logs = p.wait_for(
                cls.base,
                cls.env,
                lambda text: (
                    "World server is up and running!" in text
                    and "[PlayerBot][Login]  'Targetally' GUID:500001" in text
                    and "[PlayerBot][Login]  'Targethorde' GUID:500002" in text
                ),
                deadline=600,
            )
            time.sleep(5)
            cls.logs = p.command(["docker", "compose"] + cls.base + ["logs", "--no-color", "world"],
                                 env=cls.env, timeout=60)
            (cls.evidence / "world.log").write_text(cls.logs, encoding="utf-8")
        except BaseException:
            if cls.base is not None:
                p.force_down(cls.base, cls.env)
            raise

    @classmethod
    def cleanup_lab(cls):
        if cls.base is None:
            return
        try:
            ids = p.command(["docker", "compose"] + cls.base + ["ps", "-a", "-q"], env=cls.env).splitlines()
            for row in (json.loads(p.command(["docker", "inspect", *ids])) if ids else []):
                labels = row["Config"]["Labels"]
                assert labels.get("com.docker.compose.project") == cls.project
                assert not row["HostConfig"].get("PortBindings")
        finally:
            p.teardown_lab(cls.base, cls.env, cls.project, cls.evidence)

    def test_world_stays_ready_and_players_are_skipped(self):
        self.assertIn("World server is up and running!", self.logs)
        self.assertNotIn("[CRASH]", self.logs)

    def test_both_factions_logged_in_for_target_fixture(self):
        self.assertIn("[PlayerBot][Login]  'Targetally' GUID:500001", self.logs)
        self.assertIn("[PlayerBot][Login]  'Targethorde' GUID:500002", self.logs)


if __name__ == "__main__":
    unittest.main()
