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

# Timeline (the lab-only PlayerBot.FollowScript replays owner chat events;
# the clock starts the instant the owner is online):
#   t=+8s    owner: .botrecruit Prioritycomp      (party formed; the
#            assist order requires issuer and bot in the same party; the
#            owner is an owned companion of its own account, so it has no
#            ambient auto-hunt and never joins the fight on its own)
#   t=+12s   owner: .botassist Prioritycomp
#            Kobold Laborer                        (the companion engages
#            the creature: the pre-follow combat)
#   t=+18s   owner: .botfollow Prioritycomp       (follow arrives
#            mid-combat: the assist order is cancelled, but the live fight
#            must continue as ContinueCombat - the PORT-003 property)
#
# After the kill the companion loots the corpse, then follows the owner to
# "reached". The setup no longer relies on autonomous target acquisition:
# the KAP-558 hardening (0cbedd1) removed the owned companion's auto-hunt
# default (owned companions idle near their owner and fight only via
# self-defense or explicit orders), so engagement is driven by the
# explicit owner assist order. The creature is a Kobold Laborer (entry 80,
# level 3-4, armor 52) pinned to 300 HP (see _seed_sql): the kill takes
# ~10-20 s from the t=+12 s assist, so combat outlives the 18 s follow
# goal and ends well inside the 240 s window.
# (creature.health_percent is clamped to 100 at world load; HP must come
# from the template itself.)
# After the fix: companion keeps fighting, then follows after combat ends.
# Before the fix: companion stops fighting immediately and follows.
PRIORITY_SCRIPT = ";".join((
    "8000:%d:botrecruit Prioritycomp" % OWNER_GUID,
    "12000:%d:botassist Prioritycomp Kobold Laborer" % OWNER_GUID,
    "18000:%d:botfollow Prioritycomp" % OWNER_GUID,
))


def _seed_sql():
    # Owner sits 20 yd from the companion: inside the 25 yd
    # kOwnerFollowChaseDist, so the companion holds position near its
    # spawn instead of walking to the owner before the assist lands. The
    # owner is owned by its own account (not ambient), so it has no
    # auto-hunt and never engages the creature on its own - only the
    # companion fights, via the explicit assist order.
    # Companion starts 1 yd from the creature (the assist target).
    # Creature 80 = Kobold Laborer (level 3-4, ~78-86 HP, armor 52),
    # attackable by the test bots. The lab-only override below pins BOTH
    # health_min and health_max to 300 so the kill takes ~10-20 s from the
    # t=+12 s assist: past the 18 s follow goal and well inside the 240 s
    # follow wait window.
    # Both columns must be pinned: SelectLevel rolls the level (3 or 4)
    # and interpolates spawn HP between health_min and health_max, so an
    # override of only health_max left a 50/50 coin flip (level-3 spawns
    # came up at 78 HP and died before the follow goal fired).
    # The override is a fixture constant, applied only to the disposable
    # lab database.
    # regeneration is cleared (0): an unowned creature regenerates
    # maxHealth/3 per 4 s tick while out of combat, and this passive
    # NPC never enters combat (its AI takes no victim), so with the
    # stock regeneration=3 it would never die (run 10: HP oscillated
    # 185-300 for the whole 240 s window).
    return """
INSERT INTO tw_char.characters
 (guid,account,name,race,class,gender,level,money,position_x,position_y,position_z,map,
  orientation,zone,health,power1,power2,power3,power4,power5,xp,xp_gain)
VALUES
 (610100,1000610100,'Priowner',1,1,0,10,100000,-8949.95,-154.493,83.5312,0,
  0,12,100,0,0,0,0,0,0,1),
 (610101,1000610101,'Prioritycomp',1,1,0,10,100000,-8949.95,-134.493,83.5312,0,
  0,12,100,0,0,0,0,0,0,1);
INSERT INTO tw_char.playerbot (char_guid,chance,ai)
 VALUES (610100,100,'Default'),(610101,100,'Default');
INSERT INTO tw_char.bot_ownership (char_guid,account_id,bot_type,provision_version,owner_account_id)
 VALUES (610100,1000610100,1,2,1000610100),(610101,1000610101,1,2,1000610100);
UPDATE tw_world.creature_template SET health_min = 300, health_max = 300, regeneration = 0 WHERE entry = 80;
INSERT INTO tw_world.creature
 (guid,id,map,position_x,position_y,position_z,orientation,spawntimesecsmin,
  spawntimesecsmax,wander_distance,health_percent,mana_percent,movement_type,spawn_flags)
VALUES (2500010,80,0,-8949.95,-135.493,83.5312,0,600,600,0,100,100,0,1);
DELETE FROM tw_world.creature WHERE map=0 AND position_x BETWEEN -8990 AND -8910
 AND position_y BETWEEN -230 AND -100 AND guid <> 2500010;
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
            # Wait for the follow goal to be issued (18 s script event).
            cls.logs = p.wait_for(cls.base, cls.env, lambda text:
                                  "[PlayerBot][Follow] active" in text,
                                  deadline=180)
            # Wait for the creature to die (combat completes).
            # The bot fights the chicken; once it dies, the bot resumes follow.
            # We look for a "reached" or "leader unavailable" log AFTER the
            # follow goal was active, proving the bot transitioned from
            # combat to follow.
            deadline = time.monotonic() + 240
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

    def test_01_companion_engaged_combat(self):
        """The companion engaged the creature before the follow goal arrived.

        Engagement is driven by the owner's t=+12 s .botassist order (the
        KAP-558 hardening removed the owned companion's auto-hunt default):
        the assist executor logs "[Assist] fighting" lines, and the
        post-follow continuation logs "fighting" through the shared
        executor. Either proves the companion engaged the creature.
        """
        self.assertTrue(
            "engage GUID:610101" in self.logs or
            "fighting GUID:610101" in self.logs,
            "No combat activity found for the companion")

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
        self.assertEqual(self.personal_before, self._personal_state())


if __name__ == "__main__":
    unittest.main()
