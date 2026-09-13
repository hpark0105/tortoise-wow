"""NEXT-001: configured native bot identity and fail-closed conflicts."""
import os
import unittest
import uuid
from datetime import datetime, timezone

import test_bot_provision as p

VALID = "Specbot,3,3,1,0,0,0,0,0"  # female Dwarf Hunter
CONFLICT = "Specbot,3,3,1,1,0,0,0,0"
INVALID = "Badbot,1,7,0,0,0,0,0,0"  # Human Shaman has no playercreateinfo


class BotIdentitySpecTests(unittest.TestCase):
    base = env = project = evidence = None

    @classmethod
    def setUpClass(cls):
        p.IMAGE = os.environ.get("NEXT001_LAB_IMAGE", "tortoise-local:dev")
        try:
            p.command(["docker", "image", "inspect", p.IMAGE])
        except RuntimeError:
            raise unittest.SkipTest("%s image not present; build it first" % p.IMAGE)
        cls.project = "tortoise-bot-spec-" + uuid.uuid4().hex[:12]
        cls.evidence = p.ROOT / "local" / (
            cls.project + "-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ"))
        cls.evidence.mkdir(parents=True)
        world = p.world_env_for(VALID)
        world.update(PLAYERBOT_ENABLE="0", PLAYERBOT_MIN_BOTS="0", PLAYERBOT_MAX_BOTS="0")
        try:
            cls.base, cls.env = p.boot_lab(cls.project, cls.evidence, world)
            p.command(["docker", "compose"] + cls.base + ["up", "-d", "--no-deps", "world"], env=cls.env)
            cls.valid_logs = p.wait_for(cls.base, cls.env, lambda text:
                                        "created native character 'Specbot'" in text, deadline=300)
            cls.s1 = cls.state()
            changed = p.world_env_for(CONFLICT)
            changed.update(PLAYERBOT_ENABLE="0", PLAYERBOT_MIN_BOTS="0", PLAYERBOT_MAX_BOTS="0")
            cls.conflict_logs = p.start_world_again(
                cls.base, cls.env, cls.evidence, changed, "conflict",
                "identity spec for 'Specbot' conflicts with its existing marker")
            cls.s2 = cls.state()
            invalid = p.world_env_for(INVALID)
            invalid.update(PLAYERBOT_ENABLE="0", PLAYERBOT_MIN_BOTS="0", PLAYERBOT_MAX_BOTS="0")
            cls.invalid_logs = p.start_world_again(
                cls.base, cls.env, cls.evidence, invalid, "invalid",
                "identity spec for 'Badbot' has an invalid race/class/gender combination")
            cls.bad_rows = p.db_int(cls.base, cls.env,
                                    "SELECT COUNT(*) FROM characters WHERE name='Badbot'")
        except BaseException:
            if cls.base is not None:
                p.force_down(cls.base, cls.env)
            raise

    @classmethod
    def tearDownClass(cls):
        if cls.base is not None:
            p.teardown_lab(cls.base, cls.env, cls.project, cls.evidence)

    @classmethod
    def state(cls):
        return p.db_exec(cls.base, cls.env, """
SELECT c.guid,c.race,c.class,c.gender,c.playerBytes,c.playerBytes2,
 b.account_id,b.provision_version,
 m.race_id,m.class_id,m.gender_id,m.skin_id,m.face_id,m.hair_style_id,m.hair_color_id,m.facial_hair_id,
 h.map,h.zone,h.position_x,h.position_y,h.position_z,
 i.map,i.zone,i.position_x,i.position_y,i.position_z
FROM characters c JOIN bot_ownership b ON b.char_guid=c.guid
JOIN bot_provision_state m ON m.char_guid=c.guid
JOIN character_homebind h ON h.guid=c.guid
JOIN tw_world.playercreateinfo i ON i.race=c.race AND i.class=c.class
WHERE c.name='Specbot';""").strip()

    def test_configured_identity_and_homebind(self):
        fields = self.s1.split("\t")
        self.assertEqual(fields[1:5], ["3", "3", "1", "0"])
        self.assertEqual(int(fields[5]) & 0xFF, 0)
        self.assertEqual(fields[8:16], ["3", "3", "1", "0", "0", "0", "0", "0"])
        self.assertEqual(fields[16:18], fields[21:23])
        for actual, expected in zip(fields[18:21], fields[23:26]):
            self.assertAlmostEqual(float(actual), float(expected), places=3)
        self.assertEqual(fields[16:21], fields[21:26])

    def test_changed_spec_is_rejected_without_mutation(self):
        self.assertIn("conflicts with its existing marker", self.conflict_logs)
        self.assertEqual(self.s2, self.s1)

    def test_illegal_race_class_is_rejected(self):
        self.assertIn("invalid race/class/gender combination", self.invalid_logs)
        self.assertEqual(self.bad_rows, 0)

    def test_worlds_stayed_healthy(self):
        for logs in (self.valid_logs, self.conflict_logs, self.invalid_logs):
            self.assertNotIn("[CRASH]", logs)


if __name__ == "__main__":
    unittest.main()
