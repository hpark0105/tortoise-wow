"""PORT-005 (Phase 1.1 hardening item 1): assist ranks legal live
hostiles over invalid live matches and dead matches.

One disposable, port-free lab. Three roster bots are seeded: the owner
(610200, its own reserved account) near the creature cluster, the
companion (610201, bound to the owner) 45 yd north of the owner, and a
stranger (610202, owned by its own account) far to the northwest.

The cluster:

  * 2500020 "Sinkbeast" (entry 51600, pinned 2500/2500): a passive
    hostile decoy running NullCreatureAI (ai_name 'NullAI'), 6 yd south
    of the owner. It is hostile to the player (faction 25) so the
    owner's first-tick auto-aggro - which always lands before any
    FollowScript self-hold can be delivered - targets it, but
    NullCreatureAI never retaliates (empty AttackStart / AttackedBy /
    MoveInLineOfSight and a no-op base DamageTaken) and never flees, so
    the owner's combat clears at the t=+0.2 s self-hold CombatStop and
    the t=+8 s recruit succeeds. It is strictly the nearest hostile to
    the owner (6 yd vs 25 yd to the vermin line), robust to the ~8 yd
    first-tick position anomaly, and is an aggro sink, not an assist
    target: no script line ever names it.
  * 2500021 Kobold Vermin (entry 6, pinned 50/50): the first assist
    target, 25 yd south of the owner; a fast, deterministic kill.
  * 2500022 "Kobold Vermin" (entry 7 renamed, pinned 2000/2000): the
    ranking target, 15 yd east of 2500021. The 2000 HP pin guarantees
    it outlives the t=+50..+58 s assist window, so the resolver
    picking it (over a closer invalid same-name match and a dead
    same-name corpse) is what the test proves.
  * 2500025 "Kobold Vermin" (entry 65 renamed, faction 3 = Dwarf,
    friendly to the bot): the invalid live match, 4 yd north of
    2500021 - closer to the companion than 2500022, but unattackable,
    so it must never be selected when a legal same-name hostile is in
    range.
  * 2500024 "Kobold Warrior" (entry 48 renamed, pinned 20/20, level 1-2): the
    dead-only target, 8 yd east of 2500022; killed by the t=+62 s
    assist so the t=+74 s assist by the same name hits target-dead.
    Its level is pinned to 1-2 (the base entry 48 is a level 21-22
    Skeletal Warrior) so the companion's white-only damage - no
    on-next-swing spells are cast after the rotation hardening - drops
    it inside the +62..+70 s window deterministically.

Why the passive-hostile decoy. The world update loop runs the bot's
legacy AI (auto-aggro within 30 yd) before UpdateFollowScript can
deliver even a delay-0 self-hold, so the owner always engages the
nearest hostile on its first tick. If that hostile retaliates (the
EventAI vermin do), the self-hold's CombatStop clears the owner's own
attack but not the vermin's, so the owner stays IsInCombat() and the
t=+8 s recruit is rejected (party recruit rejected combat). A
NullCreatureAI decoy is hostile (auto-aggroed) but never attacks back,
so after the self-hold the owner has no victim and no attacker and is
fully out of combat. The decoy sits 6 yd south of the owner and the
vermin line 25 yd south - a 19 yd gap that keeps the decoy strictly the
nearest hostile even under the ~8 yd first-tick anomaly seen in the
earlier critter build, where the owner engaged a 21 yd vermin over a
closer 15 yd one. The assist targets 2500022 and 2500024 are likewise
made passive (NullAI): a level-21 EventAI warrior (2500024) kills the
level-15 companion before the companion kills the warrior, so neither
assist target may retaliate.

All creature templates are pinned by the seed: regeneration = 0 (wild
creatures otherwise regen maxHP/3 per 4 s tick - Creature::RegenerateAll
- which makes survival windows nondeterministic) and fixed HP.

PlayerBot.WanderRadius is clamped to 2 yd for the lab so idle bots
stay near their spawn until their first scripted order.

The lab-only PlayerBot.FollowScript replays, through the real chat
path (the owner is the only issuer, so the script clock starts the
instant the owner is online):

  t=+0.2s  owner: .bothold Assistowner             (pins the anchor
           against the first-tick aggro as soon as it can land)
  t=+1.5s  owner: .bothold Assistowner             (safety net)
  t=+6s    owner: .botassist Assistcomp Kobold Vermin
           (rejected: not in a party yet)
  t=+8s    owner: .botrecruit Assistcomp           (party formed)
  t=+10s   owner: .botfollow Assistcomp            (follow seq 1; the
           companion walks ~45 yd south to the owner)
  t=+36s   owner: .botassist Assistcomp Kobold Vermin
           (accepted seq 2; nearest legal live hostile 2500021 wins
           over the invalid 2500025; the companion chases and kills
           2500021)
  t=+46s   owner: .bothold Assistcomp              (hold seq 3 pins
           the companion at the 2500021 corpse)
  t=+50s   owner: .botassist Assistcomp Kobold Vermin
           (accepted seq 4 on 2500022: the dead 2500021 corpse (0 yd)
           and the invalid 2500025 (4 yd) are both skipped - the
           ranking proof; the companion chases ~15 yd and trades
           blows with 2500022)
  t=+58s   owner: .bothold Assistcomp              (hold seq 5
           cancels the active 2500022 pursuit; 2500022 survives)
  t=+62s   owner: .botassist Assistcomp Kobold Warrior
           (accepted seq 6; 2500024, 8 yd away, is killed)
  t=+70s   owner: .bothold Assistcomp              (hold seq 7 pins
           the companion at the 2500024 corpse)
  t=+74s   owner: .botassist Assistcomp Kobold Warrior   (rejected:
           target-dead; only the dead match remains)
  t=+78s   owner: .botassist Assistcomp NoSuchMob   (rejected: not
           found)
  t=+82s   owner: .bothold Assistcomp              (hold seq 8)
  t=+86s   owner: .botfollow Assistcomp            (follow seq 9; the
           companion walks back to the owner)
  t=+102s  owner: .botassist Assistcomp Assistowner (rejected: target
           is a player)
  t=+112s  owner: .botassist Assstranger Kobold Vermin
           (rejected: not owner - the stranger is owned by its own
           account, so the check fires before the target lookup)

Acceptance evidence (log assertions; in-game verification is
PORT-010):
  * exactly three assists accepted, with distinct targets and seqs,
    and the seq-4 accept on 2500022 proving the legal-live-hostile
    ranking (dead same-name corpse and closer invalid live match both
    skipped);
  * [Assist] fighting lines for all three accepted targets (assist
    policy drove combat);
  * no [Assist] fighting for 2500022 after the hold seq 5 (hold
    cancels the pursuit);
  * 2500020 (critter) and 2500022 survive (no target-gone or corpse
    line for either);
  * [Follow] reached for the post-kill follow seq 9 (follow resumes);
  * the five distinct rejection lines;
  * no crash; personal containers untouched.
"""
import os
import time
import unittest
import uuid
from datetime import datetime, timezone

import test_bot_provision as p
import test_bot_follow as f

OWNER_GUID = 610200
COMP_GUID = 610201
STRANGER_GUID = 610202
OWNER_ACC = 1000610200
COMP_ACC = 1000610201
STRANGER_ACC = 1000610202
DECOY_GUID = 2500020
VERMIN_A_GUID = 2500021
VERMIN_B_GUID = 2500022
FRIENDLY_GUID = 2500025
WARRIOR_GUID = 2500024
PERSONAL_CONTAINERS = ("tortoise-local-db-1", "tortoise-local-realmd-1",
                       "tortoise-local-world-1")

ASSIST_SCRIPT = ";".join([
    "200:%d:bothold Assistowner" % OWNER_GUID,
    "1500:%d:bothold Assistowner" % OWNER_GUID,
    "6000:%d:botassist Assistcomp Kobold Vermin" % OWNER_GUID,
    "8000:%d:botrecruit Assistcomp" % OWNER_GUID,
    "10000:%d:botfollow Assistcomp" % OWNER_GUID,
    "36000:%d:botassist Assistcomp Kobold Vermin" % OWNER_GUID,
    "46000:%d:bothold Assistcomp" % OWNER_GUID,
    "50000:%d:botassist Assistcomp Kobold Vermin" % OWNER_GUID,
    "58000:%d:bothold Assistcomp" % OWNER_GUID,
    "62000:%d:botassist Assistcomp Kobold Warrior" % OWNER_GUID,
    "70000:%d:bothold Assistcomp" % OWNER_GUID,
    "74000:%d:botassist Assistcomp Kobold Warrior" % OWNER_GUID,
    "78000:%d:botassist Assistcomp NoSuchMob" % OWNER_GUID,
    "82000:%d:bothold Assistcomp" % OWNER_GUID,
    "86000:%d:botfollow Assistcomp" % OWNER_GUID,
    "102000:%d:botassist Assistcomp Assistowner" % OWNER_GUID,
    "112000:%d:botassist Assstranger Kobold Vermin" % OWNER_GUID,
])


def _seed_sql():
    return """
INSERT INTO tw_char.characters
 (guid,account,name,race,class,gender,level,money,position_x,position_y,position_z,map,
  orientation,zone,health,power1,power2,power3,power4,power5,xp,xp_gain)
VALUES (610200,1000610200,'Assistowner',1,1,0,10,100000,-8949.95,-185.493,83.5312,0,
 0,12,100,0,0,0,0,0,0,1),
 (610201,1000610201,'Assistcomp',1,1,0,15,100000,-8949.95,-140.493,83.5312,0,
 0,12,100,0,0,0,0,0,0,1),
 (610202,1000610202,'Assstranger',1,1,0,10,100000,-8962.95,-136.493,83.5312,0,
 0,12,100,0,0,0,0,0,0,1);
INSERT INTO tw_char.playerbot (char_guid,chance,ai)
 VALUES (610200,100,'Default'),(610201,100,'Default'),(610202,100,'Default');
INSERT INTO tw_char.bot_ownership (char_guid,account_id,bot_type,provision_version,owner_account_id)
 VALUES (610200,1000610200,1,2,1000610200),
        (610201,1000610201,1,2,1000610200),
        (610202,1000610202,1,2,1000610202);
INSERT INTO tw_world.creature
 (guid,id,map,position_x,position_y,position_z,orientation,spawntimesecsmin,
  spawntimesecsmax,wander_distance,health_percent,mana_percent,movement_type,spawn_flags)
VALUES (2500020,51600,0,-8949.95,-191.493,83.5312,0,600,600,0,100,100,0,1),
 (2500021,6,0,-8949.95,-210.493,83.5312,0,600,600,0,100,100,0,1),
 (2500022,7,0,-8934.95,-210.493,83.5312,0,600,600,0,100,100,0,1),
 (2500024,48,0,-8926.95,-210.493,83.5312,0,600,600,0,100,100,0,1),
 (2500025,65,0,-8949.95,-206.493,83.5312,0,600,600,0,100,100,0,1);
UPDATE tw_world.creature_template
 SET ai_name = 'NullAI', name = 'Sinkbeast', health_min = 2500, health_max = 2500,
     regeneration = 0 WHERE entry = 51600;
UPDATE tw_world.creature_template
 SET health_min = 50, health_max = 50, regeneration = 0 WHERE entry = 6;
UPDATE tw_world.creature_template
 SET health_min = 2000, health_max = 2000, regeneration = 0,
     name = 'Kobold Vermin', ai_name = 'NullAI' WHERE entry = 7;
UPDATE tw_world.creature_template
 SET health_min = 20, health_max = 20, regeneration = 0,
     level_min = 1, level_max = 2,
     name = 'Kobold Warrior', ai_name = 'NullAI' WHERE entry = 48;
UPDATE tw_world.creature_template
 SET name = 'Kobold Vermin', faction = 3 WHERE entry = 65;
DELETE FROM tw_world.creature WHERE map=0 AND position_x BETWEEN -8990 AND -8910
 AND position_y BETWEEN -230 AND -100
 AND guid NOT IN (2500020,2500021,2500022,2500024,2500025);
"""


class BotCompanionAssistTests(unittest.TestCase):
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
        p.IMAGE = os.environ.get("PORT005_LAB_IMAGE", "tortoise-local:dev")
        try:
            p.command(["docker", "image", "inspect", p.IMAGE])
        except RuntimeError:
            raise unittest.SkipTest("%s image not present; build it first" % p.IMAGE)
        cls.personal_before = cls._personal_state()
        world = p.world_env_for("Assistowner")
        world.update(PLAYERBOT_MIN_BOTS="0", PLAYERBOT_MAX_BOTS="0",
                     PLAYERBOT_REFRESH="600000", PLAYERBOT_UPDATE_MS="1000",
                     PLAYERBOT_PROVISION="",
                     PLAYERBOT_TEST_LOGIN="%d,%d,%d" % (OWNER_GUID, COMP_GUID, STRANGER_GUID),
                     PLAYERBOT_QUEST_ID="0",
                     PLAYERBOT_WANDER_RADIUS="2",
                     PLAYERBOT_FOLLOW_SCRIPT=ASSIST_SCRIPT,
                     PLAYER_SAVE_INTERVAL="5000")
        try:
            cls.project = "tortoise-bot-assist-" + uuid.uuid4().hex[:12]
            stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
            cls.evidence = p.ROOT / "local" / (cls.project + "-" + stamp)
            cls.evidence.mkdir(parents=True)
            (cls.evidence / "image-id.txt").write_text(
                p.command(["docker", "image", "inspect", "--format", "{{.Id}}", p.IMAGE]),
                encoding="utf-8")
            cls.base, cls.env = p.boot_lab(cls.project, cls.evidence, world, _seed_sql())
            p.command(["docker", "compose"] + cls.base + ["up", "-d", "--no-deps", "world"],
                      env=cls.env)
            # All three seeded bots must log in through the roster path
            # (second probe login rejected as a duplicate).
            cls.logs = p.wait_for(cls.base, cls.env, lambda text:
                                  all("test-login %d first=1 second=0" % g in text
                                      for g in (OWNER_GUID, COMP_GUID, STRANGER_GUID)),
                                  deadline=420)
            # The last script event (the not-owner assist on the stranger)
            # settles.
            cls.logs = p.wait_for(cls.base, cls.env, lambda text:
                                  "assist rejected not-owner bot:Assstranger issuer:%d acc:%d owner:%d"
                                  % (OWNER_GUID, OWNER_ACC, STRANGER_ACC) in text, deadline=420)
            # Quiet window so periodic saves settle, then a clean stop with
            # all three bots online (exercises the shutdown path).
            end = time.monotonic() + 60
            while time.monotonic() < end:
                time.sleep(3)
            p.command(["docker", "compose"] + cls.base + ["stop", "world"], env=cls.env, timeout=180)
            (cls.evidence / "world.log").write_text(
                p.command(["docker", "compose"] + cls.base + ["logs", "--no-color", "world"],
                          env=cls.env, timeout=60),
                encoding="utf-8")
            p.teardown_lab(cls.base, cls.env, cls.project, cls.evidence)
            cls.base = None
        except BaseException:
            if cls.base is not None and cls.evidence is not None:
                try:
                    failure_logs = p.command(["docker", "compose"] + cls.base +
                                             ["logs", "--no-color", "world"],
                                             env=cls.env, timeout=60)
                    (cls.evidence / "world-failure.log").write_text(failure_logs, encoding="utf-8")
                except Exception:
                    pass
                p.force_down(cls.base, cls.env)
            raise
        # The lab is fully torn down here; record the personal server's state
        # after the whole run (the test asserting on it runs first).
        cls.personal_after = cls._personal_state()

    def test_all_seeded_bots_logged_in(self):
        for g, n in ((OWNER_GUID, "Assistowner"), (COMP_GUID, "Assistcomp"),
                     (STRANGER_GUID, "Assstranger")):
            self.assertIn("test-login %d first=1 second=0" % g, self.logs)
            self.assertIn("[PlayerBot][Login]  '%s' GUID:%d" % (n, g), self.logs)
        self.assertNotIn("[CRASH]", self.logs)

    def test_party_formed_and_owner_pinned(self):
        self.assertIn("party recruit accepted bot:Assistcomp guid:%d leader:%d"
                      % (COMP_GUID, OWNER_GUID), self.logs)
        self.assertIn("hold accepted bot:Assistowner guid:%d issuer:%d"
                      % (OWNER_GUID, OWNER_GUID), self.logs)

    def test_assist_accepts_ranked_targets(self):
        self.assertIn("assist accepted bot:Assistcomp target:Kobold Vermin guid:%d seq:2"
                      % VERMIN_A_GUID, self.logs)
        self.assertIn("assist accepted bot:Assistcomp target:Kobold Vermin guid:%d seq:4"
                      % VERMIN_B_GUID, self.logs)
        self.assertIn("assist accepted bot:Assistcomp target:Kobold Warrior guid:%d seq:6"
                      % WARRIOR_GUID, self.logs)
        self.assertEqual(self.logs.count("assist accepted bot:Assistcomp"), 3,
                         "exactly three assists may be accepted")
        self.assertIn("[PlayerBot][Assist] active GUID:%d target:%d seq:2"
                      % (COMP_GUID, VERMIN_A_GUID), self.logs)
        self.assertIn("[PlayerBot][Assist] active GUID:%d target:%d seq:4"
                      % (COMP_GUID, VERMIN_B_GUID), self.logs)
        self.assertIn("[PlayerBot][Assist] active GUID:%d target:%d seq:6"
                      % (COMP_GUID, WARRIOR_GUID), self.logs)

    def test_invalid_and_dead_matches_do_not_mask_legal(self):
        # At t=+50 s the resolver sees, by name "Kobold Vermin": the dead
        # 2500021 corpse (nearest match, 0 yd), the invalid 2500025
        # (nearest live match, unattackable, 4 yd), and the legal 2500022
        # (15 yd). The accept must name 2500022: a raw nearest selection
        # would have produced a target-dead or target-friendly rejection
        # instead.
        self.assertIn("assist accepted bot:Assistcomp target:Kobold Vermin guid:%d seq:4"
                      % VERMIN_B_GUID, self.logs)
        for prefix in ("target-dead", "target-friendly", "target-is-player",
                       "target-invalid", "target-not-in-sight"):
            self.assertNotIn("assist rejected %s bot:Assistcomp target:Kobold Vermin"
                             % prefix, self.logs)

    def test_assist_policy_drove_combat_for_all_targets(self):
        for guid in (VERMIN_A_GUID, VERMIN_B_GUID, WARRIOR_GUID):
            self.assertIn("[PlayerBot][Assist] fighting GUID:%d target:%d"
                          % (COMP_GUID, guid), self.logs)

    def test_hold_cancels_active_assist_pursuit(self):
        hold = "[PlayerBot][Hold] active GUID:%d seq:5" % COMP_GUID
        fighting = "[PlayerBot][Assist] fighting GUID:%d target:%d" % (COMP_GUID, VERMIN_B_GUID)
        self.assertIn(hold, self.logs)
        hold_at = self.logs.find(hold)
        self.assertGreater(hold_at, -1)
        self.assertNotIn(fighting, self.logs[hold_at:],
                         "the 2500022 pursuit must stop after the hold seq 5")

    def test_survivors_outlive_their_assist_windows(self):
        # The passive decoy (2500020, 2500 HP pin) absorbs the owner's
        # unscheduled first-tick aggro; it must not die while any assist
        # is active or before the final stop. 2500022 (2000 HP pin) took
        # the t=+50..+58 s window; it must survive too.
        for guid, label in ((DECOY_GUID, "passive decoy"), (VERMIN_B_GUID, "passive vermin")):
            gone_at = self.logs.find(
                "[PlayerBot][Assist] target gone GUID:%d target:%d" % (COMP_GUID, guid))
            self.assertEqual(gone_at, -1,
                             "%s died during its assist window" % label)
            corpse_at = self.logs.find(
                "corpse loot slots GUID:%d target:%d" % (COMP_GUID, guid))
            if corpse_at == -1:
                corpse_at = self.logs.find(
                    "corpse loot processed GUID:%d target:%d" % (COMP_GUID, guid))
            self.assertEqual(corpse_at, -1,
                             "%s was killed and looted" % label)

    def test_follow_resumes_after_kill(self):
        reached = [m for m in f.REACHED.findall(self.logs)
                   if m[0] == str(COMP_GUID) and m[3] == "9"]
        self.assertTrue(reached, "the post-kill follow (seq 9) never reached the owner")
        self.assertLessEqual(float(reached[0][2]), 2.0,
                             "companion must reach within the follow range of the owner")

    def test_rejection_ladder(self):
        self.assertIn("assist rejected not-in-party bot:Assistcomp issuer:%d" % OWNER_GUID, self.logs)
        self.assertIn("assist rejected not-owner bot:Assstranger issuer:%d acc:%d owner:%d"
                      % (OWNER_GUID, OWNER_ACC, STRANGER_ACC), self.logs)
        self.assertIn("assist rejected target-not-found bot:Assistcomp target:NoSuchMob issuer:%d"
                      % OWNER_GUID, self.logs)
        self.assertIn("assist rejected target-is-player bot:Assistcomp target:Assistowner issuer:%d"
                      % OWNER_GUID, self.logs)
        self.assertIn("assist rejected target-dead bot:Assistcomp target:Kobold Warrior issuer:%d"
                      % OWNER_GUID, self.logs)
        # Precondition for the not-in-party line: the companion was online
        # when the pre-recruit assist was delivered (a late login would
        # have produced an offline rejection instead).
        comp_login = "[PlayerBot][Login]  'Assistcomp' GUID:%d" % COMP_GUID
        not_in_party = "assist rejected not-in-party bot:Assistcomp issuer:%d" % OWNER_GUID
        self.assertGreater(self.logs.find(not_in_party), self.logs.find(comp_login),
                           "pre-recruit assist must be delivered after the companion is online")

    def test_target_dead_check_comes_after_warrior_kill(self):
        dead = "assist rejected target-dead bot:Assistcomp target:Kobold Warrior issuer:%d" % OWNER_GUID
        fighting = "[PlayerBot][Assist] fighting GUID:%d target:%d" % (COMP_GUID, WARRIOR_GUID)
        self.assertGreater(self.logs.find(dead), self.logs.find(fighting))

    def test_personal_volume_untouched(self):
        self.assertNotEqual(self.project, "tortoise-local")
        if self.personal_before:
            self.assertEqual(self.personal_after, self.personal_before,
                             "personal server containers must not be restarted or touched")


if __name__ == "__main__":
    unittest.main()