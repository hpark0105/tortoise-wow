"""PORT-003: active follow goal does not suppress existing combat."""
import os
import time
import unittest
import uuid
from datetime import datetime, timezone

import test_bot_provision as p

OWNER_GUID = 610100
COMP_GUID = 610101
OWNER_ACC = 1000610100
COMP_ACC = 1000610101
PERSONAL_CONTAINERS = ("tortoise-local-db-1", "tortoise-local-realmd-1",
                       "tortoise-local-world-1")

# At 10s the owner issues .botfollow to the companion.
# The companion should already be in combat with the seeded creature.
# After the fix: companion keeps fighting, then follows after combat ends.
# Before the fix: companion stops fighting immediately and follows.
PRIORITY_SCRIPT = "10000:%d:botfollow" % OWNER_GUID


def _seed_sql():
    # Owner is far from the combat so the companion must move to follow.
    # Companion starts near the creature (within aggro range).
    # Creature 6 = Chicken (weak, dies quickly to a level-10 bot).
    return """
INSERT INTO tw_char.characters
 (guid,account,name,race,class,gender,level,money,position_x,position_y,position_z,map,
  orientation,zone,health,power1,power2,power3,power4,power5,xp,xp_gain)
VALUES
 (610100,1000610100,'Priorityowner',1,1,0,10,100000,-8949.95,-120.493,83.5312,0,
  0,12,100,0,0,0,0,0,0,0,1),
 (610101,1000610101,'Prioritycomp',1,1,0,10,100000,-8949.95,-134.493,83.5312,0,
  0,12,100,0,0,0,0,0,0,0,1);
INSERT INTO tw_char.playerbot (char_guid,chance,ai) VALUES (610101,100,'Default');
INSERT INTO tw_char.bot_ownership (char_guid,account_id,bot_type,provision_version)
 VALUES (610101,1000610101,1,2);
INSERT INTO tw_world.creature
 (guid,id,map,position_x,position_y,position_z,orientation,spawntimesecsmin,
  spawntimesecsmax,wander_distance,health_percent,mana_percent,movement_type,spawn_flags)
VALUES (2500010,6,0,-8949.95,-135.493,83.5312,0,600,600,0,100,100,0,1);
DELETE FROM tw_world.creature WHERE map=0 AND position_x BETWEEN -8990 AND -8910
 AND position_y BETWEEN -170 AND -100 AND guid <> 2500010;
"""


class BotCompanionPriorityTests(unittest.TestCase):
    base = env = project = evidence = None
    logs = ""
    personal_before = {}
    personal_after = {}

    @staticmethod
    def _personal_state():
        try:
            out = p.command(["docker", "inspect", "--format",
                             "{{.Name}}|{{.State.StartedAt}}|{{.State.Running}}",
                             *PERSONAL_CONTAINERS], timeout=60)
        except RuntimeError:
            return {}
        state = {}
        for line in out.splitlines():
            parts = line.split("|")
            if len(parts) == 3:
                state[parts[0].lstrip("/")] = (parts[1], parts[2])
        return state

    @classmethod
    def setUpClass(cls):
        p.IMAGE = os.environ.get("PORT003_LAB_IMAGE", "tortoise-local:dev")
        try:
            p.command(["docker", "image", "inspect", p.IMAGE])
        except RuntimeError:
            raise unittest.SkipTest(f"{p.IMAGE} image not present; build it first")
        cls.personal_before = cls._personal_state()
        world = p.world_env_for("")
        world.update(PLAYERBOT_MIN_BOTS="0", PLAYERBOT_MAX_BOTS="0",
                     PLAYERBOT_REFRESH="600000", PLAYERBOT_UPDATE_MS="1000",
                     PLAYERBOT_TEST_LOGIN=",".join(str(g) for g in (OWNER_GUID, COMP_GUID)),
                     PLAYERBOT_QUEST_ID="0",
                     PLAYERBOT_FOLLOW_SCRIPT=PRIORITY_SCRIPT,
                     PLAYER_SAVE_INTERVAL="5000")
        cls.project = "tortoise-bot-priority-" + uuid.uuid4().hex[:12]
        stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        cls.evidence = p.ROOT / "local" / (cls.project + "-" + stamp)
        cls.evidence.mkdir(parents=True)
        try:
            cls.base, cls.env = p.boot_lab(cls.project, cls.evidence, world, _seed_sql())
            p.command(["docker", "compose"] + cls.base + ["up", "-d", "--no-deps", "world"],
                      env=cls.env)
            # Wait for the follow goal to be issued (10s mark).
            cls.logs = p.wait_for(cls.base, cls.env, lambda text:
                                  "[PlayerBot][Follow] active" in text,
                                  deadline=180)
            # Wait for the creature to die (combat completes).
            # The bot fights the chicken; once it dies, the bot resumes follow.
            # We look for a "reached" or "leader unavailable" log AFTER the
            # follow goal was active, proving the bot transitioned from
            # combat to follow.
            deadline = time.monotonic() + 120
            while time.monotonic() < deadline:
                cls.logs = p.command(["docker", "compose"] + cls.base +
                                     ["logs", "--no-color", "world"], env=cls.env, timeout=60)
                # After combat ends, the bot should resume following.
                # The "reached" log means it got within follow range of the owner.
                # Alternatively, "leader unavailable" means the owner is on a
                # different map (shouldn't happen here) or dead.
                follow_idx = cls.logs.find("[PlayerBot][Follow] active")
                if follow_idx >= 0:
                    after_follow = cls.logs[follow_idx:]
                    if "[PlayerBot][Follow] reached" in after_follow:
                        break
                time.sleep(5)
            else:
                cls.logs = p.command(["docker", "compose"] + cls.base +
                                     ["logs", "--no-color", "world"], env=cls.env, timeout=60)
            (cls.evidence / "world.log").write_text(cls.logs, encoding="utf-8")
        finally:
            if cls.base is not None:
                p.teardown_lab(cls.base, cls.env, cls.project, cls.evidence)

    @classmethod
    def tearDownClass(cls):
        cls.personal_after = cls._personal_state()

    def test_01_companion_engaged_combat(self):
        """The companion fought the creature before the follow goal arrived.

        The creature is within aggro range at spawn; the companion's normal
        AI should pick it up and engage before the 10s follow script fires.
        """
        self.assertIn("fighting GUID:610101", self.logs)

    def test_02_follow_goal_activated(self):
        """The follow goal was accepted and activated."""
        self.assertIn("[PlayerBot][Follow] active GUID:610101", self.logs)

    def test_03_combat_not_suppressed_by_follow(self):
        """After the follow goal activated, the companion still fought.

        This is the key PORT-003 assertion: the follow goal did NOT stop
        the active combat. We expect at least one 'fighting' log line
        AFTER the 'Follow active' line.
        """
        follow_idx = self.logs.find("[PlayerBot][Follow] active GUID:610101")
        self.assertGreater(follow_idx, 0, "Follow goal was not activated")
        after_follow = self.logs[follow_idx:]
        self.assertIn("fighting GUID:610101", after_follow,
                      "No combat activity after follow goal - combat was suppressed")

    def test_04_companion_resumed_follow_after_combat(self):
        """After the creature died, the companion moved toward its owner.

        The 'reached' log proves the bot transitioned from combat back to
        following and closed the distance to the owner.
        """
        self.assertIn("[PlayerBot][Follow] reached", self.logs)

    def test_05_personal_containers_untouched(self):
        """No personal server containers were affected."""
        self.assertEqual(self.personal_before, self.personal_after)


if __name__ == "__main__":
    unittest.main()