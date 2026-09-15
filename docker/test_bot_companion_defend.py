"""PORT-006: reactive owner defend for one owned companion.

One disposable, port-free lab. Three roster bots are seeded in the
combat-proven corridor (the assist fixture area, X -8949.95, ground
Z ~79-85): the owner (610300, its own reserved account; unowned
on purpose - its legacy idle acquisition is the combat starter), the
companion (610301, bound to the owner) 45 yd south of the owner, and
an unowned stranger (610302) 31 yd west / 46 yd south of the owner.
One creature is seeded: the attacker (2500040, entry 51600,
Snufflesnout) 10 yd north of the owner. The entity is seeded at
Z 86.0, above the local ground, so it drops onto the surface.

The attacker choice is evidence-driven (defend-run1/2):
  * run1 used the Y -90..-110 area at Z 83.5312; that area sits on a
    ~1.7 yd terrain step that buries the seeds - nobody could path to
    the attacker, no combat ever started, and the run proved nothing.
  * run2 (this corridor) seeded a Kobold Vermin (entry 6) as the
    attacker: its victim state only flapped in sub-second windows
    (three [Defend] active/cleared pairs at t9-t16) and the defend
    path never locked (no fighting line ever). The same run seeded a
    Snufflesnout (entry 51600) as a bystander 26 yd east: the owner
    bot's legacy 30 yd acquisition picked it up after the vermin
    died, it fought back (owner combat:1), and the defend scan locked
    on it three flaps later ([Defend] fighting target:2500041) -
    proof the Snufflesnout sustains victim state long enough to lock.
The Snufflesnout is therefore the attacker. Its template is pinned
(1200/1200 HP, regeneration = 0): its natural 182 HP plus ~60-HP-per-
4-s regeneration outlasted the owner alone in run2 (net damage ~0
over 26 s), and 1200 keeps the kill inside the t24-t50 window even
when the companion adds its DPS.

There is deliberately no hostile bystander. Any hostile creature
inside the companion's 30 yd defend scan is also inside the owner
bot's 30 yd legacy acquisition radius (the companion rests within 2
yd of the owner), and the owner's post-fight idle wander drifted up
to ~30 yd in 27 s in run2 - so no in-box placement can stay outside
the owner's pickup for the whole window, and a picked-up bystander
fights back and becomes a legal defend target, making the old
"bystander never pulled" assertion unsatisfiable. The negative
guarantee is asserted instead as: every [Defend] target line in the
whole run references the attacker, and the distant stranger is never
referenced alongside the companion.

Mechanics: the owner bot's idle target acquisition (30 yd) engages
the attacker within seconds and the attacker fights back (its victim
becomes the owner - victim equality is the only defend trigger;
neutrals and unengaged creatures are never pulled). The companion
starts 45 yd south so its first idle step (default owner-follow
stops 25 yd from the owner; the timer starts at 0 and fires on the
first AI tick) can never reach the attacker's 30 yd scan radius; the
follow order binds it to the owner before any later step, and an owned
companion never autonomously acquires - so the defend path, not idle
acquisition, is what engages the attacker:
defend fires once the companion is inside the 30 yd scan radius
while the attacker's victim is the owner.

The lab-only PlayerBot.FollowScript replays, through the real chat
path:

  t=+2s    owner: .botdefend Defendcomp on    (defend enabled)
  t=+3s    owner: .botfollow Defendcomp       (follow seq 1; the
           companion walks 45 yd north, defend engages mid-walk)
  t=+5s    owner: .botdefend Defendcomp off   (toggle proof)
  t=+7s    owner: .botdefend Defendcomp on    (defend enabled)
  t=+18s   owner: .bothold Defendcomp         (hold seq 2; the
           companion is mid-defend by now - hold cancels the pursuit)
  t=+24s   owner: .botfollow Defendcomp       (follow seq 3; the
           companion re-engages the still-attacking creature)
  t=+50s   owner: .botfollow Defendcomp       (follow seq 4; the
           defender is dead by now - proof the follow resumes after
           the fight)
  t=+60s   owner: .botstop Defendcomp         (the goal is withdrawn)
  t=+65s   stranger: .botdefend ...           (rejected: not owner)

Acceptance evidence (log assertions; in-game verification is
PORT-010):
  * defend toggle accepted exactly on/off/on by the owner;
  * the companion engages the owner's attacker ([Defend] active)
    only after the owner is actually being attacked;
  * every [Defend] target line in the run references the attacker
    (defend never pulls the companion to an unrelated entity);
  * the distant stranger is never referenced alongside the
    companion;
  * hold seq 2 wins over the active defend (no pursuit between the
    hold and the release) and the defender outlives the hold window
    (the companion re-engages after the release);
  * the defend state clears with "owner safe" once the defender is
    dead and the owner is still alive;
  * follow seq 4 reaches the owner (return to follow);
  * .botstop withdraws the goal (FollowStop logs inactive just
    before the acceptance line; no follow/defend activity after);
  * the stranger's .botdefend is rejected not-owner;
  * no crash; personal containers untouched.

Known fixture limitation: no entity is inside the 30 yd scan radius
while un-attacking (the attacker is attacked from t~2.5 and is the
only creature), so the "nearby neutral ignored" case is covered by
code review of the victim-equality gate, not by the run.

"""
import os
import re
import time
import unittest
import uuid
from datetime import datetime, timezone

import test_bot_provision as p
import test_bot_follow as f

OWNER_GUID = 610300
COMP_GUID = 610301
STRANGER_GUID = 610302
OWNER_ACC = 1000610300
COMP_ACC = 1000610301
STRANGER_ACC = 1000610302
ATTACKER_GUID = 2500040
PERSONAL_CONTAINERS = ("tortoise-local-db-1", "tortoise-local-realmd-1",
                       "tortoise-local-world-1")

DEFEND_SCRIPT = ";".join([
    "2000:%d:botdefend Defendcomp on" % OWNER_GUID,
    "3000:%d:botfollow Defendcomp" % OWNER_GUID,
    "5000:%d:botdefend Defendcomp off" % OWNER_GUID,
    "7000:%d:botdefend Defendcomp on" % OWNER_GUID,
    "18000:%d:bothold Defendcomp" % OWNER_GUID,
    "24000:%d:botfollow Defendcomp" % OWNER_GUID,
    "50000:%d:botfollow Defendcomp" % OWNER_GUID,
    "60000:%d:botstop Defendcomp" % OWNER_GUID,
    "65000:%d:botdefend Defendcomp on" % STRANGER_GUID,
])


def _seed_sql():
    return """
INSERT INTO tw_char.characters
 (guid,account,name,race,class,gender,level,money,position_x,position_y,position_z,map,
  orientation,zone,health,power1,power2,power3,power4,power5,xp,xp_gain)
VALUES (610300,1000610300,'Defendowner',1,1,0,10,100000,-8949.95,-130.493,86.0,0,
 0,12,100,0,0,0,0,0,0,1),
 (610301,1000610301,'Defendcomp',1,1,0,10,100000,-8949.95,-175.493,86.0,0,
 0,12,100,0,0,0,0,0,0,1),
 (610302,1000610302,'Defstranger',1,1,0,10,100000,-8980.95,-176.493,86.0,0,
 0,12,100,0,0,0,0,0,0,1);
INSERT INTO tw_char.playerbot (char_guid,chance,ai)
 VALUES (610300,100,'Default'),(610301,100,'Default'),(610302,100,'Default');
INSERT INTO tw_char.bot_ownership (char_guid,account_id,bot_type,provision_version,owner_account_id)
 VALUES (610300,1000610300,1,2,NULL),
        (610301,1000610301,1,2,1000610300),
        (610302,1000610302,1,2,NULL);
INSERT INTO tw_world.creature
 (guid,id,map,position_x,position_y,position_z,orientation,spawntimesecsmin,
  spawntimesecsmax,wander_distance,health_percent,mana_percent,movement_type,spawn_flags)
VALUES (2500040,51600,0,-8949.95,-120.493,86.0,0,600,600,0,100,100,0,1);
UPDATE tw_world.creature_template
 SET health_min = 1200, health_max = 1200, regeneration = 0 WHERE entry = 51600;
DELETE FROM tw_world.creature WHERE map=0 AND position_x BETWEEN -8990 AND -8910
 AND position_y BETWEEN -230 AND -100 AND guid NOT IN (2500040);

"""


class BotCompanionDefendTests(unittest.TestCase):
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
        p.IMAGE = os.environ.get("PORT006_LAB_IMAGE", "tortoise-local:dev")
        try:
            p.command(["docker", "image", "inspect", p.IMAGE])
        except RuntimeError:
            raise unittest.SkipTest("%s image not present; build it first" % p.IMAGE)
        cls.personal_before = cls._personal_state()
        world = p.world_env_for("Defendowner")
        world.update(PLAYERBOT_MIN_BOTS="0", PLAYERBOT_MAX_BOTS="0",
                     PLAYERBOT_REFRESH="600000", PLAYERBOT_UPDATE_MS="1000",
                     PLAYERBOT_PROVISION="",
                     PLAYERBOT_TEST_LOGIN="%d,%d,%d" % (OWNER_GUID, COMP_GUID, STRANGER_GUID),
                     PLAYERBOT_QUEST_ID="0",
                     PLAYERBOT_FOLLOW_SCRIPT=DEFEND_SCRIPT,
                     PLAYER_SAVE_INTERVAL="5000")
        try:
            cls.project = "tortoise-bot-defend-" + uuid.uuid4().hex[:12]
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
            # The last script event (the stranger's not-owner defend) settles.
            cls.logs = p.wait_for(cls.base, cls.env, lambda text:
                                  "defend rejected not-owner bot:Defendcomp issuer:%d acc:%d owner:%d"
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

    def _owner_fought_attacker_at(self):
        for pat in ("[PlayerBot] engage GUID:%d target:%d",
                    "[PlayerBot] fighting GUID:%d victim:%d"):
            at = self.logs.find(pat % (OWNER_GUID, ATTACKER_GUID))
            if at != -1:
                return at
        return -1

    def test_all_seeded_bots_logged_in(self):
        for g, n in ((OWNER_GUID, "Defendowner"), (COMP_GUID, "Defendcomp"),
                     (STRANGER_GUID, "Defstranger")):
            self.assertIn("test-login %d first=1 second=0" % g, self.logs)
            self.assertIn("[PlayerBot][Login]  '%s' GUID:%d" % (n, g), self.logs)
        self.assertNotIn("[CRASH]", self.logs)

    def test_defend_toggled_by_owner(self):
        on = "defend enabled bot:Defendcomp guid:%d issuer:%d acc:%d" % (COMP_GUID, OWNER_GUID, OWNER_ACC)
        off = "defend disabled bot:Defendcomp guid:%d issuer:%d acc:%d" % (COMP_GUID, OWNER_GUID, OWNER_ACC)
        self.assertEqual(self.logs.count(on), 2, "on, off, on = two enables")
        self.assertEqual(self.logs.count(off), 1)
        first_on = self.logs.find(on)
        off_at = self.logs.find(off)
        second_on = self.logs.find(on, first_on + 1)
        self.assertTrue(0 <= first_on < off_at < second_on, "toggle order must be on, off, on")

    def test_defend_engages_only_an_attacker(self):
        owner_fought_at = self._owner_fought_attacker_at()
        self.assertGreater(owner_fought_at, -1,
                           "the owner never engaged the attacker; the fixture timeline broke")
        active = "[PlayerBot][Defend] active GUID:%d target:%d" % (COMP_GUID, ATTACKER_GUID)
        active_at = self.logs.find(active)
        self.assertGreater(active_at, -1, "defend never engaged the owner's attacker")
        self.assertGreater(active_at, owner_fought_at,
                           "defend fired before the owner was actually being attacked")

    def test_defend_only_targets_the_attacker(self):
        # Every defend target line in the whole run must reference the
        # attacker: defend never pulls the companion to an unrelated
        # entity. See the docstring for why no hostile bystander is
        # seeded.
        targets = re.findall(
            r"\[PlayerBot\]\[Defend\] [a-z]+ GUID:%d target:(\d+)" % COMP_GUID,
            self.logs)
        self.assertTrue(targets, "the defend path never produced a target")
        for t in targets:
            self.assertEqual(t, str(ATTACKER_GUID),
                             "defend targeted an unrelated entity: %s" % t)

    def test_stranger_never_referenced(self):
        # The stranger sits outside every acquisition/scan radius for
        # the whole run; no companion line may ever reference it.
        for line in self.logs.splitlines():
            if "[PlayerBot]" in line and str(COMP_GUID) in line \
                    and str(STRANGER_GUID) in line:
                self.fail("the companion referenced the stranger: %r" % line)

    def test_hold_wins_over_active_defend(self):
        hold = "[PlayerBot][Hold] active GUID:%d seq:2" % COMP_GUID
        fighting = "[PlayerBot][Defend] fighting GUID:%d target:%d" % (COMP_GUID, ATTACKER_GUID)
        hold_at = self.logs.find(hold)
        self.assertGreater(hold_at, -1, "the mid-defend hold never activated")
        first_fight = self.logs.find(fighting)
        self.assertGreaterEqual(first_fight, 0, "no defend pursuit before the hold")
        self.assertLess(first_fight, hold_at,
                        "the companion must have been pursuing before the hold")
        release = "[PlayerBot][Follow] active GUID:%d leader:%d seq:3" % (COMP_GUID, OWNER_GUID)
        release_at = self.logs.find(release)
        self.assertGreater(release_at, hold_at)
        self.assertNotIn(fighting, self.logs[hold_at:release_at],
                         "hold must cancel the defend pursuit until the next order")
        self.assertGreater(self.logs.find(fighting, release_at), -1,
                           "the defender outlived the hold window; the companion "
                           "re-engages after the release")

    def test_defender_dies_and_owner_stays_safe(self):
        clear = "[PlayerBot][Defend] cleared GUID:%d target:%d reason:owner safe" % (COMP_GUID, ATTACKER_GUID)
        release = "[PlayerBot][Follow] active GUID:%d leader:%d seq:3" % (COMP_GUID, OWNER_GUID)
        clear_at = self.logs.find(clear)
        self.assertGreater(clear_at, -1,
                           "the defend state never cleared with the owner safe")
        self.assertGreater(clear_at, self.logs.find(release),
                           "the defender must die after the release, with the owner safe")
        self.assertNotIn("[PlayerBot] bot dead GUID:%d" % OWNER_GUID, self.logs,
                         "the owner must survive the defend fight")

    def test_follow_resumes_after_fight(self):
        reached = [m for m in f.REACHED.findall(self.logs)
                   if m[0] == str(COMP_GUID) and m[3] == "4"]
        self.assertTrue(reached, "the post-fight follow (seq 4) never reached the owner")
        self.assertLessEqual(float(reached[0][2]), 2.0,
                             "companion must reach within the follow range of the owner")

    def test_stop_withdraws_the_goal(self):
        # FollowStop() logs the inactive line before BotStop() logs the
        # acceptance, so search backwards from the acceptance line.
        stop = "stop accepted bot:Defendcomp guid:%d issuer:%d seq:4" % (COMP_GUID, OWNER_GUID)
        stop_at = self.logs.find(stop)
        self.assertGreater(stop_at, -1)
        inactive = "[PlayerBot][Follow] inactive GUID:%d" % COMP_GUID
        self.assertGreater(self.logs.rfind(inactive, 0, stop_at + 1), -1,
                           "stop must emit the follow inactive line")
        tail = self.logs[stop_at:]
        self.assertNotIn("[PlayerBot][Follow] active GUID:%d" % COMP_GUID, tail)
        self.assertNotIn("[PlayerBot][Defend] active GUID:%d" % COMP_GUID, tail)

    def test_not_owner_rejected(self):
        self.assertIn("defend rejected not-owner bot:Defendcomp issuer:%d acc:%d owner:%d"
                      % (STRANGER_GUID, STRANGER_ACC, OWNER_ACC), self.logs)

    def test_personal_volume_untouched(self):
        self.assertNotEqual(self.project, "tortoise-local")
        if self.personal_before:
            self.assertEqual(self.personal_after, self.personal_before,
                             "personal server containers must not be restarted or touched")


if __name__ == "__main__":
    unittest.main()
