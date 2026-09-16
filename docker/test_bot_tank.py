"""PORT-014 (KAP-558): one legal tank threat policy for the declared
matrix (Warrior, level 10, Taunt spell 355 in the learned-spell
book; see Companion/Tank.h).

One disposable, port-free lab. Two roster bots are seeded: the owner
(610230, level 40, its own reserved account) 10 yd north of the vermin,
and the declared tank (610231, level 10, bound to the owner, Taunt in
its learned-spell book: character_spell 355, the IsDeclaredTank gate)
4.4 yd south of it.

The cluster:

  * 2500030 "Kobold Vermin" (entry 6, pinned 20000/20000, regeneration 0):
    the first pull, 10 yd south of the owner; the reactive EventAI
    vermin retaliates when attacked (it does not aggro a walk-by, so
    neither bot is engaged before its first scripted assist), and a
    leash_range of 0 means it never leashes, so its threat list - and
    the tank's stored threat in it - survives the whole run.
  * 2500031 "Tankb" (entry 7 renamed, pinned 20000/20000, regeneration 0,
    NullAI): the second pull, 15 yd east of A; passive, so it outlives
    the tank's white damage and stays engaged for the pull-cap proof.
  * 2500032 "Tankc" (entry 48 renamed, pinned 20000/20000, regeneration 0,
    level 1-2, NullAI): the refused third pull, 8 yd east of B (the proven
    assist-fixture gap); it is
    never engaged - the manager refuses the assist at the pull cap
    before the tank moves.

Both creature templates are pinned by the seed: regeneration = 0 (wild
creatures otherwise regen maxHP/3 per 4 s tick, which makes survival
windows nondeterministic) and fixed 20000 HP (the measured level-40/level-10 white damage
far outstrips 2000 inside the run window, which killed A on the
first run before the pull-cap evidence could land).

PlayerBot.WanderRadius is clamped to 2 yd for the lab so idle bots stay
near their spawn until their first scripted order.

Owned companions never autonomously acquire targets (hardening), so the
owner attacks A only through a self-assist (.botassist Tankowner Kobold
Vermin): the issuer and the bot are the same player in the same party,
which the authorization ladder accepts. That self-assist is what gives
the protected member its stored threat.

The lab-only PlayerBot.FollowScript replays, through the real chat path
(the owner is the only issuer, so the script clock starts the instant
the owner is online):

  t=+0.2s  owner: .bothold Tankowner             (pins the anchor)
  t=+0.4s  owner: .bothold Tanktank              (pins the tank; a held
           tank never acquires a target before its first order)
  t=+1.5s  owner: .bothold Tankowner             (safety net)
  t=+1.7s  owner: .bothold Tanktank              (safety net)
  t=+8s    owner: .botrecruit Tanktank           (party formed)
  t=+8.5s  owner: .botfollow Tanktank            (binds the follow
           leader: the tank policy's protected member is the follow
           leader - a recruited companion is not followed
           implicitly)
  t=+10s   owner: .botassist Tanktank Kobold Vermin
           (accepted; the hold is cancelled by the new order; the tank
           walks 4.4 yd to A and starts white swings - measured normal
           threat; A retaliates and the tank becomes the victim)
  t=+18s   owner: .botassist Tankowner Kobold Vermin
           (accepted self-assist; the owner walks in and its level-40
           white damage accumulates stored threat on A)
  ...      the tank policy (Companion/Tank.h) reads the threat list each
           2 s combat step: while the tank is the victim with the owner
           below the 1.1x switch margin it keeps white swings; when the
           owner's stored threat reaches the margin - or the owner
           becomes the victim - the tank casts Taunt 355 (instant, 0
           rage, category 82 with a 10 s recovery): the core catches the
           tank's threat up to the victim's and switches the victim back.
           A cooled-down taunt reads as not-usable and degrades to the
           ordinary attack in the same evaluation (no false delay).
  t=+44s   owner: .botassist Tanktank Tankb      (accepted second pull;
           the tank walks 15 yd to B and engages it while it still
           holds stored threat on A - a live two-target pull)
  t=+54s   owner: .botassist Tanktank Tankc      (rejected pull-cap:
           engaged = B (the tank's live victim) + A (stored threat in
           its threat list) = 2; C is never engaged)
  t=+58s   owner: .bothold Tanktank              (hold; the tank stops
           all tank actions - no further [Tank] lines)

Acceptance evidence (log assertions; in-game verification is PORT-010):
  * exactly two tank assists accepted (A then B) plus the owner's
    self-assist on A;
  * the first [Tank] threat line lands only after the first tank assist
    (the held tank acquired nothing before its order) and a
    victim-is-tank line with measured normal threat (me > 0) exists
    before the first taunt cast;
  * at least one "[Tank] taunt cast-accepted ... spell:355" on A, the
    observation line preceding it shows the owner's threat > 0 (the
    protected member held threat), and a victim-is-tank line follows
    the first cast (the victim switched back);
  * the pull-cap rejection on Tankc with engaged:2 cap:2, and no active
    or fighting line for 2500032 (the unrelated creature is never
    pulled);
  * the final hold: no [Tank] or tank-assist fighting lines after it;
  * A, B and C all survive the run; the tank stays online throughout;
  * no crash; personal containers untouched.
"""
import os
import re
import time
import unittest
import uuid
from datetime import datetime, timezone

import test_bot_provision as p

OWNER_GUID = 610230
TANK_GUID = 610231
OWNER_ACC = 1000610230
TANK_ACC = 1000610231
A_GUID = 2500030
B_GUID = 2500031
C_GUID = 2500032
PERSONAL_CONTAINERS = ("tortoise-local-db-1", "tortoise-local-realmd-1",
                       "tortoise-local-world-1")

TANK_SCRIPT = ";".join([
    "200:%d:bothold Tankowner" % OWNER_GUID,
    "400:%d:bothold Tanktank" % OWNER_GUID,
    "1500:%d:bothold Tankowner" % OWNER_GUID,
    "1700:%d:bothold Tanktank" % OWNER_GUID,
    "8000:%d:botrecruit Tanktank" % OWNER_GUID,
    "8500:%d:botfollow Tanktank" % OWNER_GUID,
    "10000:%d:botassist Tanktank Kobold Vermin" % OWNER_GUID,
    "18000:%d:botassist Tankowner Kobold Vermin" % OWNER_GUID,
    "44000:%d:botassist Tanktank Tankb" % OWNER_GUID,
    "54000:%d:botassist Tanktank Tankc" % OWNER_GUID,
    "58000:%d:bothold Tanktank" % OWNER_GUID,
])

TANK_THREAT = re.compile(
    r"\[Tank\] threat GUID:(\d+) me:(\d+) owner:(\d+) t:(\d+) victim:(\d+)")


def _seed_sql():
    return """
INSERT INTO tw_char.characters
 (guid,account,name,race,class,gender,level,money,position_x,position_y,position_z,map,
  orientation,zone,health,power1,power2,power3,power4,power5,xp,xp_gain)
VALUES (610230,1000610230,'Tankowner',1,1,0,40,100000,-8949.95,-200.493,83.5312,0,
 0,12,100,0,0,0,0,0,0,1),
 (610231,1000610231,'Tanktank',1,1,0,10,100000,-8949.95,-214.893,83.5312,0,
 0,12,100,0,0,0,0,0,0,1);
INSERT INTO tw_char.playerbot (char_guid,chance,ai)
 VALUES (610230,100,'Default'),(610231,100,'Default');
INSERT INTO tw_char.bot_ownership (char_guid,account_id,bot_type,provision_version,owner_account_id)
 VALUES (610230,1000610230,1,2,1000610230),
        (610231,1000610231,1,2,1000610230);
-- The gate is the learned-spell book: Player::HasSpell reads
-- character_spell (spell ids) at login. The skill line seed (257
-- "Protection") is matrix fidelity only: in the pinned renumbered
-- DBC skill line 355 is the warlock "Affliction" line, so a
-- character_skills row for 355 would be rejected for a warrior.
INSERT INTO tw_char.character_spell (guid,spell,active,disabled)
 VALUES (610231,355,1,0);
INSERT INTO tw_char.character_skills (guid,skill,value,max)
 VALUES (610231,257,1,1);
INSERT INTO tw_world.creature
 (guid,id,map,position_x,position_y,position_z,orientation,spawntimesecsmin,
  spawntimesecsmax,wander_distance,health_percent,mana_percent,movement_type,spawn_flags)
VALUES (2500030,6,0,-8949.95,-210.493,83.5312,0,600,600,0,100,100,0,1),
 (2500031,7,0,-8934.95,-210.493,83.5312,0,600,600,0,100,100,0,1),
 (2500032,48,0,-8926.95,-210.493,83.5312,0,600,600,0,100,100,0,1);
UPDATE tw_world.creature_template
 SET health_min = 20000, health_max = 20000, regeneration = 0 WHERE entry = 6;
UPDATE tw_world.creature_template
 SET health_min = 20000, health_max = 20000, regeneration = 0,
     name = 'Tankb', ai_name = 'NullAI' WHERE entry = 7;
UPDATE tw_world.creature_template
 SET health_min = 20000, health_max = 20000, regeneration = 0,
     level_min = 1, level_max = 2, name = 'Tankc', ai_name = 'NullAI'
 WHERE entry = 48;
DELETE FROM tw_world.creature WHERE map=0 AND position_x BETWEEN -8990 AND -8910
 AND position_y BETWEEN -230 AND -100
 AND guid NOT IN (2500030,2500031,2500032);
"""


class BotTankTests(unittest.TestCase):
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
        p.IMAGE = os.environ.get("PORT014_LAB_IMAGE", "tortoise-local:dev")
        try:
            p.command(["docker", "image", "inspect", p.IMAGE])
        except RuntimeError:
            raise unittest.SkipTest("%s image not present; build it first" % p.IMAGE)
        cls.personal_before = cls._personal_state()
        world = p.world_env_for("Tankowner")
        world.update(PLAYERBOT_MIN_BOTS="0", PLAYERBOT_MAX_BOTS="0",
                     PLAYERBOT_REFRESH="600000", PLAYERBOT_UPDATE_MS="1000",
                     PLAYERBOT_PROVISION="",
                     PLAYERBOT_TEST_LOGIN="%d,%d" % (OWNER_GUID, TANK_GUID),
                     PLAYERBOT_QUEST_ID="0",
                     PLAYERBOT_WANDER_RADIUS="2",
                     PLAYERBOT_FOLLOW_SCRIPT=TANK_SCRIPT,
                     PLAYER_SAVE_INTERVAL="5000")
        try:
            cls.project = "tortoise-bot-tank-" + uuid.uuid4().hex[:12]
            stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
            cls.evidence = p.ROOT / "local" / (cls.project + "-" + stamp)
            cls.evidence.mkdir(parents=True)
            (cls.evidence / "image-id.txt").write_text(
                p.command(["docker", "image", "inspect", "--format", "{{.Id}}", p.IMAGE]),
                encoding="utf-8")
            cls.base, cls.env = p.boot_lab(cls.project, cls.evidence, world, _seed_sql())
            p.command(["docker", "compose"] + cls.base + ["up", "-d", "--no-deps", "world"],
                      env=cls.env)
            # Both seeded bots must log in through the roster path (second
            # probe login rejected as a duplicate).
            cls.logs = p.wait_for(cls.base, cls.env, lambda text:
                                  all("test-login %d first=1 second=0" % g in text
                                      for g in (OWNER_GUID, TANK_GUID)),
                                  deadline=420)
            # The pull-cap rejection is the last scripted event to settle.
            cls.logs = p.wait_for(cls.base, cls.env, lambda text:
                                  "assist rejected pull-cap bot:Tanktank target:Tankc"
                                  in text, deadline=420)
            # Quiet window so periodic saves settle, then a clean stop with
            # both bots online (exercises the shutdown path).
            end = time.monotonic() + 30
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

    def test_both_bots_logged_in(self):
        for g, n in ((OWNER_GUID, "Tankowner"), (TANK_GUID, "Tanktank")):
            self.assertIn("test-login %d first=1 second=0" % g, self.logs)
            self.assertIn("[PlayerBot][Login]  '%s' GUID:%d" % (n, g), self.logs)
        self.assertNotIn("[CRASH]", self.logs)

    def test_party_formed(self):
        self.assertIn("party recruit accepted bot:Tanktank guid:%d leader:%d"
                      % (TANK_GUID, OWNER_GUID), self.logs)
        self.assertIn("hold accepted bot:Tanktank guid:%d issuer:%d"
                      % (TANK_GUID, OWNER_GUID), self.logs)

    def test_exactly_two_tank_pulls_accepted(self):
        accepted = self.logs.count("assist accepted bot:Tanktank")
        self.assertEqual(accepted, 2, "exactly two tank assists may be accepted")
        self.assertIn("assist accepted bot:Tanktank target:Kobold Vermin guid:%d"
                      % A_GUID, self.logs)
        self.assertIn("assist accepted bot:Tanktank target:Tankb guid:%d"
                      % B_GUID, self.logs)

    def test_owner_self_assist_accepted(self):
        # The protected member fights only through an explicit order: owned
        # companions never autonomously acquire targets.
        self.assertIn("assist accepted bot:Tankowner target:Kobold Vermin guid:%d"
                      % A_GUID, self.logs)

    def _tank_threat_lines(self):
        return [(int(m.group(2)), int(m.group(3)), int(m.group(5)), m.start())
                for m in TANK_THREAT.finditer(self.logs)
                if m.group(1) == str(TANK_GUID) and m.group(4) == str(A_GUID)]

    def test_no_tank_action_before_first_order(self):
        first_threat = self.logs.find("[Tank] threat GUID:%d" % TANK_GUID)
        first_assist = self.logs.find("assist accepted bot:Tanktank")
        self.assertGreater(first_threat, -1)
        self.assertGreater(first_assist, -1)
        self.assertGreater(first_threat, first_assist,
                           "the held tank must not act before its first order")

    def test_normal_threat_established_before_first_taunt(self):
        lines = self._tank_threat_lines()
        measured = [ln for ln in lines if ln[2] == TANK_GUID and ln[0] > 0]
        self.assertTrue(measured,
                        "no victim-is-tank line with measured normal threat (me > 0)")
        first_taunt = self.logs.find("[Tank] taunt cast-accepted")
        self.assertGreater(first_taunt, -1, "the tank never cast Taunt")
        self.assertLess(min(ln[3] for ln in measured), first_taunt,
                        "normal threat must be measured before the first taunt")

    def test_taunt_cast_when_owner_holds_threat_and_victim_switches_back(self):
        lines = self._tank_threat_lines()
        first_taunt = self.logs.find("[Tank] taunt cast-accepted GUID:%d t:%d spell:355"
                                     % (TANK_GUID, A_GUID))
        self.assertGreater(first_taunt, -1)
        # The observation sampled in the same step that cast the taunt must
        # show the protected member holding threat.
        before = [ln for ln in lines if ln[3] < first_taunt]
        self.assertTrue(before, "no [Tank] threat line precedes the first taunt")
        self.assertGreater(before[-1][1], 0,
                           "the owner held no threat when the taunt fired")
        # After the first accepted cast the victim is the tank again.
        after = [ln for ln in lines if ln[3] > first_taunt and ln[2] == TANK_GUID]
        self.assertTrue(after,
                        "the victim did not switch back to the tank after the taunt")
        # Every attempted taunt is the declared ability on the declared
        # target, reported through the PORT-012 cast outcome vocabulary
        # (accepted or rejected with a category).
        self.assertEqual(
            self.logs.count("[Tank] taunt cast-accepted") +
            self.logs.count("[Tank] taunt cast-rejected"),
            self.logs.count("[Tank] taunt cast-accepted GUID:%d t:%d spell:355" % (TANK_GUID, A_GUID)) +
            len(re.findall(r"\[Tank\] taunt cast-rejected GUID:%d t:%d spell:355 res:\d+ category:\S+" % (TANK_GUID, A_GUID), self.logs)),
            "every taunt attempt must name the declared spell and target")

    def test_pull_cap_refuses_third_target(self):
        self.assertIn("assist rejected pull-cap bot:Tanktank target:Tankc engaged:2 cap:2 issuer:%d"
                      % OWNER_GUID, self.logs)
        self.assertNotIn("assist accepted bot:Tanktank target:Tankc", self.logs)
        self.assertNotIn("[PlayerBot][Assist] active GUID:%d target:%d"
                         % (TANK_GUID, C_GUID), self.logs)
        self.assertNotIn("[PlayerBot][Assist] fighting GUID:%d target:%d"
                         % (TANK_GUID, C_GUID), self.logs)

    def test_hold_stops_tank_actions(self):
        # The final scripted hold lands after the pull-cap log
        # snapshot the other assertions run on; use the complete
        # evidence log, written after the quiet window.
        full = self.__class__.evidence.joinpath("world.log").read_text(
            encoding="utf-8")
        holds = [m.start() for m in re.finditer(
            r"\[PlayerBot\]\[Hold\] active GUID:%d" % TANK_GUID, full)]
        self.assertEqual(len(holds), 3, "the three scripted tank holds")
        tail = full[holds[-1]:]
        self.assertNotIn("[Tank] threat", tail,
                         "the held tank keeps emitting tank policy lines")
        self.assertNotIn("[PlayerBot][Assist] fighting GUID:%d" % TANK_GUID, tail,
                         "the held tank keeps fighting")

    def test_all_targets_survive(self):
        for guid in (A_GUID, B_GUID, C_GUID):
            self.assertNotIn("[PlayerBot][Assist] target gone GUID:%d target:%d"
                             % (TANK_GUID, guid), self.logs)
            for label in ("slots", "processed"):
                self.assertNotIn("corpse loot %s GUID:%d target:%d" % (label, TANK_GUID, guid),
                                 self.logs)
        # The owner's self-assist on A must also stay engaged (A survives).
        self.assertNotIn("[PlayerBot][Assist] target gone GUID:%d target:%d"
                         % (OWNER_GUID, A_GUID), self.logs)

    def test_tank_stayed_online(self):
        self.assertNotIn("assist rejected offline bot:Tanktank", self.logs)

    def test_personal_volume_untouched(self):
        self.assertNotEqual(self.project, "tortoise-local")
        if self.personal_before:
            self.assertEqual(self.personal_after, self.personal_before,
                             "personal server containers must not be restarted or touched")


if __name__ == "__main__":
    unittest.main()
