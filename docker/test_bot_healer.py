"""PORT-015 (KAP-558): one legal healer triage policy for the declared
matrix (Druid, level 12+, Regrowth spell 8936 in the learned-spell
book; see Companion/Healer.h).

One disposable, port-free lab. Two roster bots are seeded: the owner
(610240, level 40 warrior, its own reserved account, spawned at 600
health - below the declared 70% threshold on its roughly 1430 max)
10 yd north of the attacker, and the declared healer (610241, level 17
druid, bound to the owner, Regrowth in its learned-spell book:
character_spell 8936, the IsDeclaredHealer gate, full health and 60
mana) 5 yd north of the owner.

The attacker:

  * 2500140 "Healb" (entry 6 renamed, level pinned to 14, 20000/20000
    HP, regeneration 0, its reactive EventAI kept): 10 yd south of the
    owner. It does not aggro a walk-by; it retaliates only when the
    owner's scripted self-assist swings it, so the owner is the sole
    victim for the whole run and the healer (which never attacks and
    carries no swing threat) is never drawn into combat. The pinned 20000
    HP far outstrips the owner's white damage inside the run window, so
    the retaliation - and the owner's in-combat state - survives until
    the final hold.

PlayerBot.WanderRadius is clamped to 2 yd for the lab so idle bots stay
near their spawn until their first scripted order.

Owned companions never autonomously acquire targets (hardening), so the
owner attacks only through a self-assist (.botassist Healowner Healb):
the issuer and the bot are the same player in the same party, which the
authorization ladder accepts.

The lab-only PlayerBot.FollowScript replays, through the real chat path:

  t=+0.2s  owner: .bothold Healowner             (pins the anchor)
  t=+0.4s  owner: .bothold Healheal              (pins the healer)
  t=+1.5s  owner: .bothold Healowner             (safety net)
  t=+1.7s  owner: .bothold Healheal              (safety net)
  t=+8s    owner: .botrecruit Healheal           (party formed)
  t=+8.5s  owner: .botfollow Healheal            (binds the follow
           leader: the declared priority's protected member is the
           follow leader - a recruited companion is not followed
           implicitly)
  t=+8.5s  the follow bind makes the owner the protected follow
           leader, and the injured owner (600 of roughly 1430, below the
           declared 70% threshold) is inside the declared 40 yd heal
           radius (the follow range is 2 yd). The declared triage
           (Companion/Healer.h) casts Regrowth 8936 (2 s cast, 84-98
           heal, 96 mana, 0 cooldown). The in-flight cast occupies the
           generic spell container for the whole cast window, which the
           withDelayed busy form observes: an unguarded re-accept would
           replace the in-flight cast (the storm the earlier build
           produced: 16 accepted casts in the same window, every one at
           the identical pre-cast mana). The accepted cast count stays
           bounded (at most a handful: the 2 s cast window plus the
           declared mana floor plus the 70% thresholds). The declared
           heal is movement-interruptible (interruptFlags 15), so the
           accepted cast stops the walk and the follow goal holds
           position for the cast window; the approach resumes after it
           ends. Every accepted cast lands at pre-cast mana at or above
           cost + reserve (128). The accepted heals never cancel the
           follow behavior: the bot keeps the follow goal running and
           never engages the attacker (its state lines keep victim:0).
           The druid starts below its own threshold (the login clamp
           leaves it around 316 of 576) but the owner has the declared
           priority, so a self-heal is legal only after the owner has
           left the threshold window.
  t=+10s   owner: .botassist Healowner Healb
           (accepted self-assist; the owner walks 10 yd to the attacker
           and starts white swings; the attacker retaliates and the
           owner fights until the final owner hold)
  t=+58s   owner: .bothold Healowner             (the owner stops the
           fight so the quiet window ends out of combat)
  t=+60s   owner: .bothold Healheal              (hold: every [Healer]
           line must stop)

Acceptance evidence (log assertions; in-game verification is later):
  * a bounded number (1-5) of "[Healer] heal cast-accepted ... spell:8936"
    lines, every one at pre-cast mana at or above cost + reserve (the
    declared floor held on every acceptance); the unguarded re-accept
    storm produced 16 accepted casts in the same window, so a count above
    5 is the storm regressing;
  * no cast targets the owner while its last state sample is at or above
    the 70% threshold (the threshold boundary is pinned at the value
    level by test_companion_healer_value.cpp, which also pins the exact
    mana-floor boundary at 128/127);
  * a self-heal (if any) lands only after the owner's last state sample
    is at or above the threshold (the declared priority held while the
    owner was still injured); no self cast-rejected line at all;
  * every attempted heal (accepted or rejected) names the declared
    spell and target through the PORT-012 cast outcome vocabulary;
  * the healer keeps running the behavior path after the cast (state
    heartbeats with victim:0 keep flowing) and the held bot emits no
    [Healer] lines after the final hold;
  * the owner survives the run and recovers health at some point (heal
    and/or the normal out-of-combat regeneration after the hold);
  * no crash; personal containers untouched.
"""
import os
import re
import time
import unittest
import uuid
from datetime import datetime, timezone

import test_bot_provision as p

OWNER_GUID = 610240
HEALER_GUID = 610241
OWNER_ACC = 1000610240
HEALER_ACC = 1000610241
ATTACKER_GUID = 2500140
PERSONAL_CONTAINERS = ("tortoise-local-db-1", "tortoise-local-realmd-1",
                       "tortoise-local-world-1")

HEAL_SCRIPT = ";".join([
    "200:%d:bothold Healowner" % OWNER_GUID,
    "400:%d:bothold Healheal" % OWNER_GUID,
    "1500:%d:bothold Healowner" % OWNER_GUID,
    "1700:%d:bothold Healheal" % OWNER_GUID,
    "8000:%d:botrecruit Healheal" % OWNER_GUID,
    "8500:%d:botfollow Healheal" % OWNER_GUID,
    "10000:%d:botassist Healowner Healb" % OWNER_GUID,
    "58000:%d:bothold Healowner" % OWNER_GUID,
    "60000:%d:bothold Healheal" % OWNER_GUID,
])

HEAL_CAST = re.compile(
    r"\[Healer\] heal cast-accepted GUID:(\d+) t:(\d+) spell:(\d+) "
    r"mana-before:(\d+) mana:(\d+)/(\d+)")
OWNER_STATE = re.compile(r"\[PlayerBot\] state GUID:%d .*mhp:(\d+)/(\d+)" % OWNER_GUID)
HEALER_STATE = re.compile(r"\[PlayerBot\] state GUID:%d .*victim:(\d+) vhp:\d+/\d+ mhp:(\d+)/(\d+)" % HEALER_GUID)


def _seed_sql():
    return """
INSERT INTO tw_char.characters
 (guid,account,name,race,class,gender,level,money,position_x,position_y,position_z,map,
  orientation,zone,health,power1,power2,power3,power4,power5,xp,xp_gain)
VALUES (610240,1000610240,'Healowner',1,1,0,40,100000,-8949.95,-200.493,83.5312,0,
 0,12,600,0,0,0,0,0,0,1),
 (610241,1000610241,'Healheal',4,11,0,17,100000,-8949.95,-205.493,83.5312,0,
 0,12,576,60,0,0,0,0,0,1);
INSERT INTO tw_char.playerbot (char_guid,chance,ai)
 VALUES (610240,100,'Default'),(610241,100,'Default');
INSERT INTO tw_char.bot_ownership (char_guid,account_id,bot_type,provision_version,owner_account_id)
 VALUES (610240,1000610240,1,2,1000610240),
        (610241,1000610241,1,2,1000610240);
-- The gate is the learned-spell book: Player::HasSpell reads
-- character_spell (spell ids) at login. 8936 Regrowth is the declared
-- heal of the pinned matrix.
INSERT INTO tw_char.character_spell (guid,spell,active,disabled)
 VALUES (610241,8936,1,0);
INSERT INTO tw_world.creature
 (guid,id,map,position_x,position_y,position_z,orientation,spawntimesecsmin,
  spawntimesecsmax,wander_distance,health_percent,mana_percent,movement_type,spawn_flags)
VALUES (2500140,6,0,-8949.95,-210.493,83.5312,0,600,600,0,100,100,0,1);
-- Pin the attacker: level 14 (its retaliation must cross the owner's
-- 70% threshold quickly enough and stay slow enough for the owner to
-- survive the run), fixed 20000 HP (the measured level-40 white damage
-- far outstrips 2000 inside the run window), no regeneration. Its
-- reactive EventAI is kept: it retaliates when attacked and does not
-- aggro a walk-by.
UPDATE tw_world.creature_template
 SET health_min = 20000, health_max = 20000, regeneration = 0,
     level_min = 14, level_max = 14, name = 'Healb' WHERE entry = 6;
DELETE FROM tw_world.creature WHERE map=0 AND position_x BETWEEN -8990 AND -8910
 AND position_y BETWEEN -230 AND -100
 AND guid NOT IN (2500140);
"""


class BotHealerTests(unittest.TestCase):
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
        p.IMAGE = os.environ.get("PORT015_LAB_IMAGE", "tortoise-local:dev")
        try:
            p.command(["docker", "image", "inspect", p.IMAGE])
        except RuntimeError:
            raise unittest.SkipTest("%s image not present; build it first" % p.IMAGE)
        cls.personal_before = cls._personal_state()
        world = p.world_env_for("Healowner")
        world.update(PLAYERBOT_MIN_BOTS="0", PLAYERBOT_MAX_BOTS="0",
                     PLAYERBOT_REFRESH="600000", PLAYERBOT_UPDATE_MS="1000",
                     PLAYERBOT_PROVISION="",
                     PLAYERBOT_TEST_LOGIN="%d,%d" % (OWNER_GUID, HEALER_GUID),
                     PLAYERBOT_QUEST_ID="0",
                     PLAYERBOT_WANDER_RADIUS="2",
                     PLAYERBOT_FOLLOW_SCRIPT=HEAL_SCRIPT,
                     PLAYER_SAVE_INTERVAL="5000")
        try:
            cls.project = "tortoise-bot-healer-" + uuid.uuid4().hex[:12]
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
                                      for g in (OWNER_GUID, HEALER_GUID)),
                                  deadline=420)
            # The final scripted hold is the last event to settle (the
            # first two healer holds land at t=0.4s and t=1.7s).
            cls.logs = p.wait_for(cls.base, cls.env, lambda text:
                                  text.count("hold accepted bot:Healheal") >= 3,
                                  deadline=420)
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
        # The lab is fully torn down here; record the personal server's
        # state after the whole run (the test asserting on it runs first).
        cls.personal_after = cls._personal_state()

    @staticmethod
    def _full_log():
        return BotHealerTests.evidence.joinpath("world.log").read_text(encoding="utf-8")

    def _cast_matches(self, text=None):
        text = self.logs if text is None else text
        return [m for m in HEAL_CAST.finditer(text)
                if m.group(1) == str(HEALER_GUID)]

    def test_both_bots_logged_in(self):
        for g, n in ((OWNER_GUID, "Healowner"), (HEALER_GUID, "Healheal")):
            self.assertIn("test-login %d first=1 second=0" % g, self.logs)
            self.assertIn("[PlayerBot][Login]  '%s' GUID:%d" % (n, g), self.logs)
        self.assertNotIn("[CRASH]", self.logs)

    def test_party_formed(self):
        self.assertIn("party recruit accepted bot:Healheal guid:%d leader:%d"
                      % (HEALER_GUID, OWNER_GUID), self.logs)
        self.assertIn("hold accepted bot:Healheal guid:%d issuer:%d"
                      % (HEALER_GUID, OWNER_GUID), self.logs)
        self.assertIn("[PlayerBot][Follow] active GUID:%d leader:%d" % (HEALER_GUID, OWNER_GUID), self.logs)

    def test_owner_self_assist_accepted(self):
        # The protected member fights only through an explicit order: owned
        # companions never autonomously acquire targets.
        self.assertIn("assist accepted bot:Healowner target:Healb guid:%d"
                      % ATTACKER_GUID, self.logs)

    def test_heal_casts_bounded_and_floor_safe(self):
        # The one-flight gate plus the declared mana floor and the 70%
        # thresholds keep the accepted casts bounded: the unguarded
        # re-accept storm produced 16 accepted casts in the same window,
        # every one at the identical pre-cast mana (the in-flight cast's
        # slot freed early and the triage re-accepted it). Legitimate
        # triage accepts at most a handful: each accepted cast occupies
        # the 2 s cast window, spends 96 of the seeded 60 mana plus
        # out-of-combat regeneration, and the 70% thresholds stop the
        # owner and self lines once they recover.
        matches = self._cast_matches()
        self.assertGreaterEqual(len(matches), 1,
                                "no heal cast accepted at all")
        self.assertLessEqual(len(matches), 5,
                             "the unguarded re-accept storm regressed "
                             "(it produced 16 accepted casts in the same "
                             "window): %r" % [m.group(0) for m in matches])
        for m in matches:
            self.assertEqual(m.group(3), "8936",
                             "only the declared heal is cast")
            self.assertGreaterEqual(int(m.group(4)), 96 + 32,
                                    "the declared mana floor (cost + "
                                    "reserve) held on every accepted cast")

    def test_self_heal_only_after_owner_protected(self):
        # A self-heal is legal only after the declared priority holds:
        # the owner (the follow leader) must already be at or above the
        # 70% threshold when the first self-cast lands. The druid starts
        # below its own threshold (the login clamp leaves it around 316
        # of 576), so a self-cast while the owner is still injured would
        # be the declared priority inverting. A self cast-rejected line
        # is not legal at all in the fixture (the declared heal carries
        # no cooldown; a rejection would have to name the declared spell
        # and outcome and none is expected).
        full = self._full_log()
        self.assertNotIn("[Healer] heal cast-rejected GUID:%d t:%d"
                         % (HEALER_GUID, HEALER_GUID), full)
        self_casts = [m for m in HEAL_CAST.finditer(full)
                      if m.group(1) == str(HEALER_GUID)
                      and m.group(2) == str(HEALER_GUID)]
        for sc in self_casts:
            prior = [m for m in OWNER_STATE.finditer(full)
                     if m.start() < sc.start()]
            self.assertTrue(prior,
                            "no owner state sample before the self-cast")
            hp, maxhp = int(prior[-1].group(1)), int(prior[-1].group(2))
            self.assertGreaterEqual((hp * 1000) // maxhp, 700,
                                    "self-heal while the owner was still "
                                    "below the 70%% threshold: owner %d/%d "
                                    "at the last sample before it"
                                    % (hp, maxhp))

    def test_every_attempt_uses_the_cast_vocabulary(self):
        # Every attempted heal (accepted or rejected) names the declared
        # spell and target and reports the PORT-012 cast outcome.
        accepted = self.logs.count("[Healer] heal cast-accepted")
        rejected_raw = self.logs.count("[Healer] heal cast-rejected")
        rejected_named = len(re.findall(
            r"\[Healer\] heal cast-rejected GUID:\d+ t:\d+ spell:8936 res:\d+ category:\S+",
            self.logs))
        self.assertEqual(accepted, len(self._cast_matches()),
                         "every accepted heal must name the declared spell and target")
        self.assertEqual(rejected_raw, rejected_named,
                         "every rejected heal must carry the cast outcome diagnostics")

    def test_no_cast_above_threshold(self):
        # The declared 70% threshold at runtime: no heal may target the
        # owner while its last state sample is at or above the threshold
        # (a later sample showing it injured again would re-open the
        # selection, and the cast would then be legal again). The exact
        # threshold and mana-floor boundaries are pinned at the value
        # level by test_companion_healer_value.cpp.
        full = self._full_log()
        samples = [(m.start(), int(m.group(1)), int(m.group(2)))
                   for m in OWNER_STATE.finditer(full)]
        self.assertTrue(samples, "no owner state heartbeat at all")
        for c in HEAL_CAST.finditer(full):
            if c.group(1) != str(HEALER_GUID) or c.group(2) != str(OWNER_GUID):
                continue
            prior = [s for s in samples if s[0] < c.start()]
            self.assertTrue(prior,
                            "no owner state sample before an owner cast")
            hp, maxhp = prior[-1][1], prior[-1][2]
            self.assertLess((hp * 1000) // maxhp, 700,
                            "a heal cast the owner while it was already at "
                            "or above the 70%% threshold: %d/%d" % (hp, maxhp))

    def test_behavior_path_continues_after_heal(self):
        # The accepted heal does not cancel the selected behavior: the
        # healer keeps running the companion path after the cast and never
        # engages the attacker (its victim stays 0 on every state line).
        matches = self._cast_matches()
        self.assertTrue(matches, "no cast to anchor on")
        cast_off = matches[0].start()
        after = self.logs[cast_off:]
        states = list(HEALER_STATE.finditer(after))
        self.assertGreaterEqual(len(states), 2,
                                "the healer kept running the behavior path "
                                "after the cast")
        for m in states:
            self.assertEqual(m.group(1), "0",
                             "the healer engaged a target: victim:%s" % m.group(1))

    def test_hold_stops_healer_actions(self):
        full = self._full_log()
        holds = [m.start() for m in re.finditer(
            r"\[PlayerBot\]\[Hold\] active GUID:%d" % HEALER_GUID, full)]
        tail = full[holds[-1]:]
        self.assertNotIn("[Healer]", tail,
                         "the held healer keeps emitting healer policy lines")

    def test_owner_survived_and_recovered(self):
        full = self._full_log()
        self.assertNotIn("[PlayerBot] bot dead GUID:%d" % OWNER_GUID, full)
        samples = [(int(m.group(1)), int(m.group(2)))
                   for m in OWNER_STATE.finditer(full)]
        self.assertTrue(samples, "no owner state heartbeat at all")
        self.assertGreater(samples[-1][0], 0, "the owner is dead at the end")
        # The owner's health must rise at some point after the cast: the
        # accepted heal and/or the normal out-of-combat regeneration after
        # the hold (in combat the health only falls).
        cast_off = self._cast_matches()[0].start() if self._cast_matches() else 0
        seq = [(hp, mx, m.start()) for m in OWNER_STATE.finditer(full)
               for hp, mx in [(int(m.group(1)), int(m.group(2)))]
               if m.start() >= cast_off]
        rises = [(b[0] - a[0]) for a, b in zip(seq, seq[1:]) if b[0] > a[0]]
        self.assertTrue(rises,
                        "the owner's health never rose after the cast "
                        "(heal effect + post-combat regeneration missing)")

    def test_attacker_stayed_alive_and_engaged(self):
        # The pinned 20000 HP attacker survives the whole run: the
        # owner's assist never saw it die and the retaliation window is
        # real.
        self.assertNotIn("[PlayerBot][Assist] target gone GUID:%d target:%d"
                         % (OWNER_GUID, ATTACKER_GUID), self.logs)

    def test_personal_volume_untouched(self):
        self.assertNotEqual(self.project, "tortoise-local")
        if self.personal_before:
            self.assertEqual(self.personal_after, self.personal_before,
                             "personal server containers must not be restarted or touched")


if __name__ == "__main__":
    unittest.main()
