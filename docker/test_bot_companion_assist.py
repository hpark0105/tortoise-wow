"""PORT-005: owner-selected assist target for one owned companion.

One disposable, port-free lab. Three roster bots are seeded: the owner
(610200, its own reserved account), the companion (610201, bound to the
owner) 40 yd south of the owner, and an unowned stranger (610202). One
Snufflesnout (creature 2500020, entry 51600) sits 25 yd south of the
companion's spawn and one Kobold Vermin (2500021, entry 6) 21 yd from
it.

Both creature templates are pinned by the seed: regeneration = 0 (wild
creatures otherwise regen maxHP/3 per 4 s tick - Creature::RegenerateAll
- which made the survival window nondeterministic; the same root cause
the priority fixture hit) and fixed HP. The snufflesnout is pinned high
(900/900) so it deterministically outlives the assist window; it is
passive and the follow intent never acquires targets (PORT-003), so it
may sit alive on the follow path. The vermin is pinned low (50/50) for
a fast, deterministic kill.

Timeline pressure: the companion's auto-aggro range (30 yd) covers the
snufflesnout (25 yd), so it engages on spawn; the recruit guard rejects
a bot in combat, which is why the companion is held at t=+4s so the
recruit at t=+8s can pass.

The lab-only PlayerBot.FollowScript replays, through the real chat path:

  t=+2s    owner: .botassist Assistcomp Snufflesnout
           (rejected: not in a party yet)
  t=+4s    owner: .bothold Assistcomp             (hold seq 1; stops the
           auto-engagement so the recruit passes its combat guard)
  t=+8s    owner: .botrecruit Assistcomp          (party formed)
  t=+10s   owner: .bothold Assistowner            (the owner is pinned in
           place; a stationary follow anchor)
  t=+14s   owner: .botassist Assistcomp Snufflesnout
           (accepted seq 2; the new generation cancels the hold; the
           assist policy drives the fight)
  t=+20s   owner: .bothold Assistcomp             (hold seq 3 cancels the
           active assist; the snufflesnout survives the 6 s window)
  t=+24s   owner: .botassist Assistcomp Kobold Vermin
           (accepted seq 4; the new order cancels the hold; the
           companion chases ~21 yd and kills the vermin)
  t=+60s   owner: .bothold Assistcomp             (hold seq 5 pins the
           companion at the corpses and clears legacy re-engagement)
  t=+65s   owner: .botassist Assistcomp Kobold Vermin   (rejected:
           target-dead)
  t=+70s   owner: .botassist Assistcomp NoSuchMob (rejected: not found)
  t=+75s   owner: .botfollow Assistcomp           (follow seq 6 resumes;
           the companion walks back to the owner)
  t=+100s  owner: .botassist Assistcomp Assistowner (rejected: target is
           a player)
  t=+110s  stranger: .botassist Assistcomp Snufflesnout
           (rejected: not owner)

Acceptance evidence (log assertions; in-game verification is PORT-010):
  * assist accepted exactly twice, with distinct targets and seqs;
  * [Assist] fighting lines for both creatures (assist policy drove
    combat);
  * no [Assist] fighting for the snufflesnout after the hold seq 3
    (hold cancels the pursuit) and its corpse is not looted before the
    hold (it survived the assist window);
  * [Follow] reached after the post-kill follow (follow resumes);
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
SNUFF_GUID = 2500020
VERMIN_GUID = 2500021
PERSONAL_CONTAINERS = ("tortoise-local-db-1", "tortoise-local-realmd-1",
                       "tortoise-local-world-1")

ASSIST_SCRIPT = ";".join([
    "2000:%d:botassist Assistcomp Snufflesnout" % OWNER_GUID,
    "4000:%d:bothold Assistcomp" % OWNER_GUID,
    "8000:%d:botrecruit Assistcomp" % OWNER_GUID,
    "10000:%d:bothold Assistowner" % OWNER_GUID,
    "14000:%d:botassist Assistcomp Snufflesnout" % OWNER_GUID,
    "20000:%d:bothold Assistcomp" % OWNER_GUID,
    "24000:%d:botassist Assistcomp Kobold Vermin" % OWNER_GUID,
    "60000:%d:bothold Assistcomp" % OWNER_GUID,
    "65000:%d:botassist Assistcomp Kobold Vermin" % OWNER_GUID,
    "70000:%d:botassist Assistcomp NoSuchMob" % OWNER_GUID,
    "75000:%d:botfollow Assistcomp" % OWNER_GUID,
    "100000:%d:botassist Assistcomp Assistowner" % OWNER_GUID,
    "110000:%d:botassist Assistcomp Snufflesnout" % STRANGER_GUID,
])


def _seed_sql():
    return """
INSERT INTO tw_char.characters
 (guid,account,name,race,class,gender,level,money,position_x,position_y,position_z,map,
  orientation,zone,health,power1,power2,power3,power4,power5,xp,xp_gain)
VALUES (610200,1000610200,'Assistowner',1,1,0,10,100000,-8949.95,-130.493,83.5312,0,
 0,12,100,0,0,0,0,0,0,1),
 (610201,1000610201,'Assistcomp',1,1,0,10,100000,-8949.95,-170.493,83.5312,0,
 0,12,100,0,0,0,0,0,0,1),
 (610202,1000610202,'Assstranger',1,1,0,10,100000,-8962.95,-136.493,83.5312,0,
 0,12,100,0,0,0,0,0,0,1);
INSERT INTO tw_char.playerbot (char_guid,chance,ai)
 VALUES (610200,100,'Default'),(610201,100,'Default'),(610202,100,'Default');
INSERT INTO tw_char.bot_ownership (char_guid,account_id,bot_type,provision_version,owner_account_id)
 VALUES (610200,1000610200,1,2,1000610200),
        (610201,1000610201,1,2,1000610200),
        (610202,1000610202,1,2,NULL);
INSERT INTO tw_world.creature
 (guid,id,map,position_x,position_y,position_z,orientation,spawntimesecsmin,
  spawntimesecsmax,wander_distance,health_percent,mana_percent,movement_type,spawn_flags)
VALUES (2500020,51600,0,-8949.95,-195.493,83.5312,0,600,600,0,100,100,0,1),
 (2500021,6,0,-8934.95,-210.493,83.5312,0,600,600,0,100,100,0,1);
UPDATE tw_world.creature_template
 SET health_min = 900, health_max = 900, regeneration = 0 WHERE entry = 51600;
UPDATE tw_world.creature_template
 SET health_min = 50, health_max = 50, regeneration = 0 WHERE entry = 6;
DELETE FROM tw_world.creature WHERE map=0 AND position_x BETWEEN -8990 AND -8910
 AND position_y BETWEEN -230 AND -100 AND guid NOT IN (2500020,2500021);
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
            # The last script event (the stranger's not-owner assist) settles.
            cls.logs = p.wait_for(cls.base, cls.env, lambda text:
                                  "assist rejected not-owner bot:Assistcomp issuer:%d acc:%d owner:%d"
                                  % (STRANGER_GUID, STRANGER_ACC, OWNER_ACC) in text, deadline=420)
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

    def test_assist_accepted_for_both_targets(self):
        self.assertIn("assist accepted bot:Assistcomp target:Snufflesnout guid:%d seq:2"
                      % SNUFF_GUID, self.logs)
        self.assertIn("assist accepted bot:Assistcomp target:Kobold Vermin guid:%d seq:4"
                      % VERMIN_GUID, self.logs)
        self.assertEqual(self.logs.count("assist accepted bot:Assistcomp"), 2,
                         "exactly two assists may be accepted")
        self.assertIn("[PlayerBot][Assist] active GUID:%d target:%d seq:2"
                      % (COMP_GUID, SNUFF_GUID), self.logs)
        self.assertIn("[PlayerBot][Assist] active GUID:%d target:%d seq:4"
                      % (COMP_GUID, VERMIN_GUID), self.logs)

    def test_assist_policy_drove_combat_for_both_targets(self):
        self.assertIn("[PlayerBot][Assist] fighting GUID:%d target:%d"
                      % (COMP_GUID, SNUFF_GUID), self.logs)
        self.assertIn("[PlayerBot][Assist] fighting GUID:%d target:%d"
                      % (COMP_GUID, VERMIN_GUID), self.logs)

    def test_hold_cancels_active_assist_pursuit(self):
        hold = "[PlayerBot][Hold] active GUID:%d seq:3" % COMP_GUID
        fighting = "[PlayerBot][Assist] fighting GUID:%d target:%d" % (COMP_GUID, SNUFF_GUID)
        self.assertIn(hold, self.logs)
        hold_at = self.logs.find(hold)
        self.assertGreater(hold_at, -1)
        self.assertNotIn(fighting, self.logs[hold_at:],
                         "the snufflesnout pursuit must stop after the hold seq 3")

    def test_survivor_outlives_the_assist_and_hold_window(self):
        # The snufflesnout took ~4 s of auto-aggro plus the t=+14..20 s
        # assist window before the hold. Its template is pinned (900 HP,
        # regeneration = 0), so it deterministically outlives the window;
        # it must not die while the assist is active or before the hold:
        # that is the survival proof behind "hold cancels the pursuit and
        # the target lives".
        hold_at = self.logs.find("[PlayerBot][Hold] active GUID:%d seq:3" % COMP_GUID)
        self.assertGreater(hold_at, -1)
        gone_at = self.logs.find(
            "[PlayerBot][Assist] target gone GUID:%d target:%d" % (COMP_GUID, SNUFF_GUID))
        if gone_at != -1:
            self.assertGreater(gone_at, hold_at,
                               "the assist target died while the assist was active")
        corpse_at = self.logs.find(
            "corpse loot slots GUID:%d target:%d" % (COMP_GUID, SNUFF_GUID))
        if corpse_at == -1:
            corpse_at = self.logs.find(
                "corpse loot processed GUID:%d target:%d" % (COMP_GUID, SNUFF_GUID))
        if corpse_at != -1:
            self.assertGreater(corpse_at, hold_at,
                               "the assist target was killed before the hold")

    def test_follow_resumes_after_kill(self):
        reached = [m for m in f.REACHED.findall(self.logs)
                   if m[0] == str(COMP_GUID) and m[3] == "6"]
        self.assertTrue(reached, "the post-kill follow (seq 6) never reached the owner")
        self.assertLessEqual(float(reached[0][2]), 2.0,
                             "companion must reach within the follow range of the owner")

    def test_rejection_ladder(self):
        self.assertIn("assist rejected not-in-party bot:Assistcomp issuer:%d" % OWNER_GUID, self.logs)
        self.assertIn("assist rejected not-owner bot:Assistcomp issuer:%d acc:%d owner:%d"
                      % (STRANGER_GUID, STRANGER_ACC, OWNER_ACC), self.logs)
        self.assertIn("assist rejected target-not-found bot:Assistcomp target:NoSuchMob issuer:%d"
                      % OWNER_GUID, self.logs)
        self.assertIn("assist rejected target-is-player bot:Assistcomp target:Assistowner issuer:%d"
                      % OWNER_GUID, self.logs)
        self.assertIn("assist rejected target-dead bot:Assistcomp target:Kobold Vermin issuer:%d"
                      % OWNER_GUID, self.logs)

    def test_target_dead_check_comes_after_vermin_kill(self):
        dead = "assist rejected target-dead bot:Assistcomp target:Kobold Vermin issuer:%d" % OWNER_GUID
        fighting = "[PlayerBot][Assist] fighting GUID:%d target:%d" % (COMP_GUID, VERMIN_GUID)
        self.assertGreater(self.logs.find(dead), self.logs.find(fighting))

    def test_personal_volume_untouched(self):
        self.assertNotEqual(self.project, "tortoise-local")
        if self.personal_before:
            self.assertEqual(self.personal_after, self.personal_before,
                             "personal server containers must not be restarted or touched")


if __name__ == "__main__":
    unittest.main()
