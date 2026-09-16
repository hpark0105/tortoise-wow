"""PORT-016 (KAP-558): one deterministic damage policy for the
declared melee matrix (Rogue; see Companion/Damage.h).

One disposable, port-free lab. Four roster bots are seeded: the
owner (610240, priest, level 40, its own reserved account) 10 yd
north of the vermin; the declared tank (610241, warrior, level 10,
bound to the owner, Taunt 355 in its learned-spell book) 4.4 yd
south of it; the declared damage companion (610242, rogue, level
14, bound to the owner) beside the owner; and an undeclared
mage (class 8) (610243, bound to the owner) beside the owner, which
must stay behaviorally inert with respect to the damage policy.

The owner is deliberately not a warrior: AutoLearnSpellsForLevel
fills the whole class book at login, so a warrior owner could
pass the declared-tank gate (class 1 + learned 355) and be
resolved by FindDeclaredTank before the actual tank, corrupting
the pull-gate evidence. A priest passes no companion gate.

The cluster:

  * 2500040 "Dmga" (entry 6 renamed, pinned 20000/20000,
    regeneration 0): the second pull, 10 yd south of the owner
    and 6 yd north of C. The reactive EventAI vermin retaliates
    when attacked and never leashes (leash_range 0), so its
    threat list survives the whole run; its death inside the
    window is harmless (no assertion depends on it outliving).
  * 2500041 "Dmgb" (entry 7 renamed, pinned 20000/20000,
    regeneration 0, NullAI): the unrelated bystander 15 yd east
    of A. No bot may ever name it - the damage policy resolves
    only the declared tank's established target, never the
    nearest hostile.
  * 2500042 "Dmgc" (entry 48 renamed, pinned 20000/20000,
    regeneration 0, level 1-2, NullAI, auras "17743" Peon
    Sleeping = a 120 s root): the controlled first pull 1.6 yd
    south of the tank. The self-cast root aura survives the
    whole run, so the damage companion must see it as the
    tank's established target and hold (crowd-control
    preservation), never deal damage to it; the 20 s second
    pull on A then exercises the live two-target cap on the
    tank (C keeps stored threat) exactly like the tank fixture.

PlayerBot.WanderRadius is clamped to 2 yd for the lab so idle
bots stay near their spawn until their first scripted order.

The lab-only PlayerBot.FollowScript replays, through the real
chat path (the owner is the only issuer, so the script clock
starts the instant the owner is online):

  t=+0.2..0.8s  owner: .bothold each of the four (pins anchors)
  t=+1.5..2.1s  owner: .bothold each again (safety net)
  t=+3..4.2s    owner: .botrecruit tank / rogue / class-8 bot
  t=+4.8..6s    owner: .botfollow tank / rogue / class-8 bot
  t=+8s        owner: .botassist Dmgtank Dmgc (pull 1; the tank
               swings the rooted C in range; the tank holds the
               stored threat and C's victim - the established
               target under crowd control: the rogue must hold)
  t=+20s       owner: .botassist Dmgtank Dmga (pull 2; the tank
               re-targets A 6 yd away; A retaliates and the tank
               becomes the victim - the established target frees
               the pull gate and the rogue engages)
  t=+68s       owner: .bothold Dmgrogue (owner override; every
               damage action stops - stale follow/directive
               cannot cancel the hold)

Acceptance evidence (log assertions; in-game verification is the
card's later phase):
  * all four bots log in through the roster path, no crash;
  * three party recruits accepted;
  * threat gate: the rogue's first [Damage] pull/fighting line
    naming A lands strictly after A's pull is accepted, and a
    pre-pull [Damage] wait line with no established target
    (target:0 established:0) precedes it;
  * the unrelated bystander is never named by any bot;
  * the controlled target: at least one [Damage] wait line
    naming C with cc:1 (the crowd-control hold) and zero
    [Damage] pull/fighting lines naming C;
  * owner override: after the final rogue hold no [Damage]
    pull/fighting or fighting line for the rogue;
  * the undeclared class-8 bot emits no [Damage] lines and
    never fights;
  * B and C survive the run; the tank stays online;
  * personal containers untouched.
"""
import os
import re
import time
import unittest
import uuid
from datetime import datetime, timezone

import test_bot_provision as p

OWNER_GUID = 610240
TANK_GUID = 610241
DAMAGE_GUID = 610242
MAGE_GUID = 610243
A_GUID = 2500040
B_GUID = 2500041
C_GUID = 2500042
PERSONAL_CONTAINERS = ("tortoise-local-db-1", "tortoise-local-realmd-1",
                       "tortoise-local-world-1")

DAMAGE_SCRIPT = ";".join([
    "200:%d:bothold Dmgowner" % OWNER_GUID,
    "400:%d:bothold Dmgtank" % OWNER_GUID,
    "600:%d:bothold Dmgrogue" % OWNER_GUID,
    "800:%d:bothold Dmgmage" % OWNER_GUID,
    "1500:%d:bothold Dmgowner" % OWNER_GUID,
    "1700:%d:bothold Dmgtank" % OWNER_GUID,
    "1900:%d:bothold Dmgrogue" % OWNER_GUID,
    "2100:%d:bothold Dmgmage" % OWNER_GUID,
    "3000:%d:botrecruit Dmgtank" % OWNER_GUID,
    "3600:%d:botrecruit Dmgrogue" % OWNER_GUID,
    "4200:%d:botrecruit Dmgmage" % OWNER_GUID,
    "4800:%d:botfollow Dmgtank" % OWNER_GUID,
    "5400:%d:botfollow Dmgrogue" % OWNER_GUID,
    "6000:%d:botfollow Dmgmage" % OWNER_GUID,
    "8000:%d:botassist Dmgtank Dmgc" % OWNER_GUID,
        "20000:%d:botassist Dmgtank Dmga" % OWNER_GUID,
    "68000:%d:bothold Dmgrogue" % OWNER_GUID,
])


def _seed_sql():
    return """
INSERT INTO tw_char.characters
 (guid,account,name,race,class,gender,level,money,position_x,position_y,position_z,map,
  orientation,zone,health,power1,power2,power3,power4,power5,xp,xp_gain)
VALUES (610240,1000610240,\'Dmgowner\',1,5,0,40,100000,-8949.95,-200.493,83.5312,0,
 0,12,100,0,0,0,0,0,0,1),
 (610241,1000610241,\'Dmgtank\',1,1,0,10,100000,-8949.95,-214.893,83.5312,0,
 0,12,100,0,0,0,0,0,0,1),
 (610242,1000610242,\'Dmgrogue\',1,4,0,14,100000,-8949.95,-203.493,83.5312,0,
 0,12,100,0,0,0,0,0,0,1),
 (610243,1000610243,\'Dmgmage\',1,8,0,10,100000,-8947.95,-202.493,83.5312,0,
 0,12,100,0,0,0,0,0,0,1);
INSERT INTO tw_char.playerbot (char_guid,chance,ai)
 VALUES (610240,100,\'Default\'),(610241,100,\'Default\'),
        (610242,100,\'Default\'),(610243,100,\'Default\');
INSERT INTO tw_char.bot_ownership (char_guid,account_id,bot_type,provision_version,owner_account_id)
 VALUES (610240,1000610240,1,2,1000610240),
        (610241,1000610241,1,2,1000610240),
        (610242,1000610242,1,2,1000610240),
        (610243,1000610243,1,2,1000610240);
-- The declared-tank gate is the learned-spell book: Player::HasSpell
-- reads character_spell (spell ids) at login. The skill line seed
-- (257) is matrix fidelity only.
INSERT INTO tw_char.character_spell (guid,spell,active,disabled)
 VALUES (610241,355,1,0);
INSERT INTO tw_char.character_skills (guid,skill,value,max)
 VALUES (610241,257,1,1);
-- Declared damage matrix fidelity (1752 Sinister Strike, 2098
-- Eviscerate, 703 Garrote); auto-learn covers the book anyway.
INSERT INTO tw_char.character_spell (guid,spell,active,disabled)
 VALUES (610242,1752,1,0),(610242,2098,1,0),(610242,703,1,0);
INSERT INTO tw_world.creature
 (guid,id,map,position_x,position_y,position_z,orientation,spawntimesecsmin,
  spawntimesecsmax,wander_distance,health_percent,mana_percent,movement_type,spawn_flags)
VALUES (2500040,6,0,-8949.95,-210.493,83.5312,0,600,600,0,100,100,0,1),
 (2500041,7,0,-8934.95,-210.493,83.5312,0,600,600,0,100,100,0,1),
 (2500042,48,0,-8949.95,-216.493,83.5312,0,600,600,0,100,100,0,1);
UPDATE tw_world.creature_template
 SET health_min = 20000, health_max = 20000, regeneration = 0,
     name = \'Dmga\' WHERE entry = 6;
UPDATE tw_world.creature_template
 SET health_min = 20000, health_max = 20000, regeneration = 0,
     name = \'Dmgb\', ai_name = \'NullAI\' WHERE entry = 7;
UPDATE tw_world.creature_template
 SET health_min = 20000, health_max = 20000, regeneration = 0,
     level_min = 1, level_max = 2, name = \'Dmgc\', ai_name = \'NullAI\',
     auras = \'17743\' WHERE entry = 48;
DELETE FROM tw_world.creature WHERE map=0 AND position_x BETWEEN -8990 AND -8910
 AND position_y BETWEEN -230 AND -100
 AND guid NOT IN (2500040,2500041,2500042);
"""


class BotDamageTests(unittest.TestCase):
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
        p.IMAGE = os.environ.get("PORT016_LAB_IMAGE", "tortoise-local:dev")
        try:
            p.command(["docker", "image", "inspect", p.IMAGE])
        except RuntimeError:
            raise unittest.SkipTest("%s image not present; build it first" % p.IMAGE)
        cls.personal_before = cls._personal_state()
        world = p.world_env_for("Dmgowner")
        world.update(PLAYERBOT_MIN_BOTS="0", PLAYERBOT_MAX_BOTS="0",
                     PLAYERBOT_REFRESH="600000", PLAYERBOT_UPDATE_MS="1000",
                     PLAYERBOT_PROVISION="",
                     PLAYERBOT_TEST_LOGIN="%d,%d,%d,%d" % (OWNER_GUID, TANK_GUID,
                                                           DAMAGE_GUID, MAGE_GUID),
                     PLAYERBOT_QUEST_ID="0",
                     PLAYERBOT_WANDER_RADIUS="2",
                     PLAYERBOT_FOLLOW_SCRIPT=DAMAGE_SCRIPT,
                     PLAYER_SAVE_INTERVAL="5000")
        try:
            cls.project = "tortoise-bot-damage-" + uuid.uuid4().hex[:12]
            stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
            cls.evidence = p.ROOT / "local" / (cls.project + "-" + stamp)
            cls.evidence.mkdir(parents=True)
            (cls.evidence / "image-id.txt").write_text(
                p.command(["docker", "image", "inspect", "--format", "{{.Id}}", p.IMAGE]),
                encoding="utf-8")
            cls.base, cls.env = p.boot_lab(cls.project, cls.evidence, world, _seed_sql())
            p.command(["docker", "compose"] + cls.base + ["up", "-d", "--no-deps", "world"],
                      env=cls.env)
            # All four seeded bots must log in through the roster path.
            cls.logs = p.wait_for(cls.base, cls.env, lambda text:
                                  all("test-login %d first=1 second=0" % g in text
                                      for g in (OWNER_GUID, TANK_GUID, DAMAGE_GUID, MAGE_GUID)),
                                  deadline=420)
            # The final rogue hold is the last scripted event; three rogue
            # holds total (pin, safety net, owner override) must have landed.
            cls.logs = p.wait_for(cls.base, cls.env,
                                  lambda text: text.count(
                                      "[PlayerBot][Hold] active GUID:%d" % DAMAGE_GUID) >= 3,
                                  deadline=420)
            # Quiet window so periodic saves settle, then a clean stop with
            # all bots online (exercises the shutdown path).
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

    def test_all_four_logged_in(self):
        for g, n in ((OWNER_GUID, "Dmgowner"), (TANK_GUID, "Dmgtank"),
                     (DAMAGE_GUID, "Dmgrogue"), (MAGE_GUID, "Dmgmage")):
            self.assertIn("test-login %d first=1 second=0" % g, self.logs)
            self.assertIn("[PlayerBot][Login]  \'%s\' GUID:%d" % (n, g), self.logs)
        self.assertNotIn("[CRASH]", self.logs)

    def test_party_formed(self):
        for name, g in (("Dmgtank", TANK_GUID), ("Dmgrogue", DAMAGE_GUID),
                        ("Dmgmage", MAGE_GUID)):
            self.assertIn("party recruit accepted bot:%s guid:%d leader:%d" % (name, g, OWNER_GUID),
                          self.logs)
            self.assertIn("hold accepted bot:%s guid:%d issuer:%d" % (name, g, OWNER_GUID),
                          self.logs)

    def _rogue_engage_positions(self, target):
        return [m.start() for m in re.finditer(
            r"\[Damage\] (?:pull|fighting) GUID:%d (?:t|target):%d" % (DAMAGE_GUID, target),
            self.logs)]

    def test_threat_gate_delays_engagement(self):
        assist = self.logs.find("assist accepted bot:Dmgtank target:Dmga guid:%d" % A_GUID)
        self.assertGreater(assist, -1, "the tank pull was never accepted")
        engages = self._rogue_engage_positions(A_GUID)
        self.assertTrue(engages, "the rogue never engaged the tank\'s established target")
        self.assertGreater(engages[0], assist,
                           "the rogue engaged before the tank pull was accepted")
        waits = [m.start() for m in re.finditer(
            r"\[Damage\] wait GUID:%d tank:%d target:0 established:0" % (DAMAGE_GUID, TANK_GUID),
            self.logs)]
        self.assertTrue(waits,
                        "no pre-pull [Damage] wait line with no established target")
        self.assertLess(max(waits), engages[0],
                        "a pre-pull wait line must precede the first engagement")

    def test_unrelated_hostile_untouched(self):
        for g in (OWNER_GUID, TANK_GUID, DAMAGE_GUID, MAGE_GUID):
            self.assertNotIn("[PlayerBot][Assist] active GUID:%d target:%d" % (g, B_GUID),
                             self.logs)
            self.assertNotIn("[PlayerBot][Assist] fighting GUID:%d target:%d" % (g, B_GUID),
                             self.logs)
        self.assertEqual(self._rogue_engage_positions(B_GUID), [],
                         "the damage companion targeted the unrelated bystander")

    def test_controlled_target_held(self):
        self.assertIn("assist accepted bot:Dmgtank target:Dmgc guid:%d" % C_GUID, self.logs)
        self.assertTrue(re.search(
            r"\[Damage\] wait GUID:%d tank:%d target:%d established:\d+ cc:1"
            % (DAMAGE_GUID, TANK_GUID, C_GUID), self.logs),
            "no crowd-control hold line naming the rooted target")
        self.assertEqual(self._rogue_engage_positions(C_GUID), [],
                         "the damage companion dealt damage to the controlled target")

    def test_owner_hold_override(self):
        # The hold evidence can land after the log snapshot the other
        # assertions run on; use the complete evidence log.
        full = self.__class__.evidence.joinpath("world.log").read_text(encoding="utf-8")
        holds = [m.start() for m in re.finditer(
            r"\[PlayerBot\]\[Hold\] active GUID:%d" % DAMAGE_GUID, full)]
        self.assertEqual(len(holds), 3, "the three scripted rogue holds")
        tail = full[holds[-1]:]
        self.assertIsNone(re.search(r"\[Damage\] (?:pull|fighting) GUID:%d" % DAMAGE_GUID, tail),
                          "the damage companion kept acting after the owner hold")
        self.assertNotIn("[PlayerBot] fighting GUID:%d" % DAMAGE_GUID, tail,
                         "the held rogue keeps fighting")

    def test_undeclared_class_inert(self):
        self.assertIsNone(re.search(r"\[Damage\] \w+ GUID:%d" % MAGE_GUID, self.logs),
                          "the undeclared class emitted damage policy lines")
        self.assertNotIn("[PlayerBot][Assist] active GUID:%d" % MAGE_GUID, self.logs)
        self.assertNotIn("[PlayerBot] fighting GUID:%d" % MAGE_GUID, self.logs)

    def test_b_and_c_survive(self):
        for guid in (B_GUID, C_GUID):
            self.assertNotIn("[PlayerBot][Assist] target gone GUID:%d target:%d"
                             % (TANK_GUID, guid), self.logs)
            for label in ("slots", "processed"):
                self.assertNotIn("corpse loot %s GUID:%d target:%d" % (label, TANK_GUID, guid),
                                 self.logs)

    def test_tank_stayed_online(self):
        self.assertNotIn("assist rejected offline bot:Dmgtank", self.logs)

    def test_personal_volume_untouched(self):
        self.assertNotEqual(self.project, "tortoise-local")
        if self.personal_before:
            self.assertEqual(self.personal_after, self.personal_before,
                             "personal server containers must not be restarted or touched")


if __name__ == "__main__":
    unittest.main()
