"""PORT-009: one normal companion death and recovery path (corpse reclaim).

One disposable, port-free lab in the combat-proven corridor.

The owner (610300) and the companion (610301, bound to the owner, 70 yd
south) are pinned and recruited; the owner self-holds at tau0 (the
FollowScript runs ahead of the bot AI updates on the same manager
tick, so the 30 yd legacy idle auto-engage can never fire first), the
companion is held at tau4 and followed at tau12. At tau30 the owner
orders an assist on a Minion of Sethir (2500300, entry 6911, pinned
2000 HP / regen 0 / no loot, base flags kept) that stands 20 yd east
and 14 yd south of the owner: off the follow path and outside the
minion's 18 yd detection, so the assist is the only engagement
trigger. The companion walks the ~23 yd to it and fights: the minion
(dmg 28.6-31.9 @1890 ms) out-damages the level-10 companion, which
dies. While dead the companion runs UpdateRecovery: the first tick
builds the corpse via BuildPlayerRepop (a socketless session never
sends the CMSG_MOVE_DEADACK that normally does it), it waits out the
30 s corpse-reclaim delay (copseReclaimDelay[0] - the expiry ladder
reads count 0 once the first second has passed), then issues the
normal CMSG_RECLAIM_CORPSE handler path and resurrects at 50% (no
free resurrection, no teleport). The tau140
backstop hold is the deterministic arc terminator: it lands while the
companion is dead (reclaim pending) or still in the first fight, and
the 2000 HP minion is unkillable by the companion's output, so no
second death cycle can start. The resurrected companion stands held at
the corpse; tau280 re-issues follow to regroup it to the owner and
tau320 holds it in a safe non-offensive end state.

Fixture deviations from the handoff design (both for determinism):

- The minion stands 20 yd east and 14 yd south of the owner (~24 yd
  straight-line) instead of on the follow path: on the path it aggroed
  the companion during the follow walk, and its 18 yd detection / the
  owner's auto-engage (observed at 18.75 yd) put both bots in combat
  before the tau8 recruit, rejecting it (combat) and the tau30 assist
  (not-in-party). Off-path the follow walk stays outside the minion's
  18 yd detection, the owner's 30 yd idle auto-engage is suppressed
  by the tau0 hold (it cannot be outranged: the assist lookup needs
  the minion within ~28 yd of the owner, inside the 30 yd engage),
  and the assist lookup (30 yd radius anchored at the companion,
  which stops up to 2 yd on either side of the owner) reads 23-26 yd
  in all cases.
- The minion keeps its base flags (no NO_AGGRO): in this engine
  NO_AGGRO also blocks retaliation (Creature::CanInitiateAttack
  returns false), so a NO_AGGRO minion would take damage forever
  without ever hitting back and the death path could never execute.
  Its 18 yd detection reaches neither the follow path (20 yd) nor the
  owner (~24 yd); it engages when the companion closes on the assist
  walk and hits it, the normal death rule the fixture exists to
  exercise.
- The minion HP is pinned to 2000: the companion's realistic output
  (~10-16 DPS over at most ~150 s of fighting) cannot kill it, so the
  tau140 backstop hold is the deterministic arc terminator and a kill
  (with its XP and a possible level-up) can never happen; the saved
  level-10 / xp-5000 assertions stay exact.
- The final script event is bothold (not botstop): a botstop leaves the
  companion order-less, and the legacy auto-acquire could re-engage the
  still-alive minion (backstop case) and kill the companion before the
  DB snapshot, flaking the saved-health assertion.

Acceptance: assist accepted; the companion dies at least once; the
recovery entered -> reclaim issued -> bot alive ordering; the
dead-to-alive window matches the 30 s reclaim delay (10 s cadence state
lines, band 3-8); no offensive lines while dead; the resurrect reports
hp <= 0.55 x max (the 50% restore, no free resurrection); the owner
never dies and the party stays intact; the companion reaches the owner
after the last follow; the saved character preserves level 10,
xp >= 5000, money 100000 and is alive (health > 0); no [CRASH];
personal containers untouched.

Unexecuted checks (code exists, this fixture cannot force them): the
natural corpse path (client dead-ack or the 6 min repop timer; the
fixture's death-ack runs first), the no-corpse hold branch beyond the
first tick, and the out-of-range walking branch (the companion always
dies on top of its corpse, in range); owner death and map change
(shared dead gate, already noted in the PORT-008 docs).
"""
import os
import re
import time
import unittest
import uuid
from datetime import datetime, timezone

import test_bot_provision as p
import test_bot_follow as f

PERSONAL_CONTAINERS = ("tortoise-local-db-1", "tortoise-local-realmd-1",
                       "tortoise-local-world-1")

O = 610300
C = 610301
K = 2500300
COMP_NAME = "Lvcomp"
KILLER_NAME = "Minion of Sethir"

BOT_DEAD = re.compile(r"\[PlayerBot\] bot dead GUID:(\d+) hp:\d+/\d+")
BOT_ALIVE = re.compile(r"\[PlayerBot\] bot alive GUID:(\d+) hp:(\d+)/(\d+)")
STATE = re.compile(
    r"\[PlayerBot\] state GUID:(\d+) map:(\d+) pos:([-0-9.]+)/([-0-9.]+)/([-0-9.]+)"
    r" combat:(\d+) victim:(\d+)")

# 10 s cadence state lines bound the dead-to-alive window: 3-8 lines maps
# to roughly 30-80 s, which covers the 30 s corpse-reclaim delay
# (copseReclaimDelay[0]) plus death-ack and tick lag (the companion
# dies on top of its corpse, so the walk is nominal).
RECOVERY_BAND = (3, 8)

OFFENSIVE_KEYWORDS = ("fighting", "engage", "loot", "Assist", "Defend", "attack")


def _seed_sql():
    return (
        "INSERT INTO tw_char.characters\n"
        " (guid,account,name,race,class,gender,level,money,position_x,position_y,position_z,map,\n"
        "  orientation,zone,health,power1,power2,power3,power4,power5,xp,xp_gain)\n"
        "VALUES (%d,1000%d,'Lsowner',1,1,0,10,100000,-8949.95,-130.493,83.5312,0,\n"
        " 0,12,100,0,0,0,0,0,0,1),\n"
        " (%d,1000%d,'%s',1,1,0,10,100000,-8949.95,-200.493,83.5312,0,\n"
        " 0,12,100,0,0,0,0,0,5000,1);\n"
        "INSERT INTO tw_char.playerbot (char_guid,chance,ai)\n"
        " VALUES (%d,100,'Default'),(%d,100,'Default');\n"
        "INSERT INTO tw_char.bot_ownership (char_guid,account_id,bot_type,provision_version,owner_account_id)\n"
        " VALUES (%d,1000%d,1,2,1000%d),\n"
        "        (%d,1000%d,1,2,1000%d);\n"
        "UPDATE tw_world.creature_template SET loot_id=0 WHERE entry=6911;\n"
        "UPDATE tw_world.creature_template\n"
        " SET health_min=2000, health_max=2000, regeneration=0 WHERE entry=6911;\n"
        # Base flags kept: the minion must retaliate when hit (NO_AGGRO
        # would block retaliation in this engine and the companion
        # would never die); its 18 yd detection reaches neither the
        # follow path (20 yd) nor the owner (~24 yd away).
        "INSERT INTO tw_world.creature\n"
        " (guid,id,map,position_x,position_y,position_z,orientation,spawntimesecsmin,\n"
        "  spawntimesecsmax,wander_distance,health_percent,mana_percent,movement_type,spawn_flags)\n"
        "VALUES (%d,6911,0,-8929.95,-144.493,83.5312,0,600,600,0,100,100,0,1);\n"
        "DELETE FROM tw_world.creature WHERE map=0 AND position_x BETWEEN -8990 AND -8910\n"
        " AND position_y BETWEEN -230 AND -100 AND guid NOT IN (%d);\n"
    ) % (O, O, C, C, COMP_NAME, O, C, O, O, O, C, C, O, K, K)


def _script():
    return ";".join([
        "0:%d:bothold Lsowner" % O,
        "4000:%d:bothold %s" % (O, COMP_NAME),
        "8000:%d:botrecruit %s" % (O, COMP_NAME),
        "12000:%d:botfollow %s" % (O, COMP_NAME),
        "30000:%d:botassist %s %s" % (O, COMP_NAME, KILLER_NAME),
        "140000:%d:bothold %s" % (O, COMP_NAME),
        "280000:%d:botfollow %s" % (O, COMP_NAME),
        "320000:%d:bothold %s" % (O, COMP_NAME),
    ])


def _reached_after_last_follow(text):
    idx = text.rfind("follow accepted bot:%s" % COMP_NAME)
    if idx == -1:
        return False
    return any(m[0] == str(C) for m in f.REACHED.findall(text[idx:]))


def _done(text):
    if not all("test-login %d first=1 second=0" % g in text for g in (O, C)):
        return False
    if ("[PlayerBot][Recovery] entered GUID:%d" % C) not in text:
        return False
    if ("[PlayerBot][Recovery] reclaim issued GUID:%d" % C) not in text:
        return False
    if text.count("follow accepted bot:%s" % COMP_NAME) < 2:
        return False
    return _reached_after_last_follow(text)


class BotCompanionRecoveryTests(unittest.TestCase):
    """Recovery lab: one owned companion, one pinned killer minion."""
    logs = ""
    db_row = ""
    personal_before = {}
    personal_after = {}
    base = env = project = evidence = None

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
        p.IMAGE = os.environ.get("PORT009_LAB_IMAGE", "tortoise-local:dev")
        try:
            p.command(["docker", "image", "inspect", p.IMAGE])
        except RuntimeError:
            raise unittest.SkipTest("%s image not present; build it first" % p.IMAGE)
        cls.personal_before = cls._personal_state()
        world = p.world_env_for("Lsowner")
        world.update(PLAYERBOT_MIN_BOTS="0", PLAYERBOT_MAX_BOTS="0",
                     PLAYERBOT_REFRESH="600000", PLAYERBOT_UPDATE_MS="1000",
                     PLAYERBOT_PROVISION="",
                     PLAYERBOT_TEST_LOGIN="%d,%d" % (O, C),
                     PLAYERBOT_QUEST_ID="0",
                     PLAYERBOT_WANDER_RADIUS="0.5",
                     PLAYERBOT_FOLLOW_SCRIPT=_script(),
                     PLAYER_SAVE_INTERVAL="5000")
        try:
            cls.project = "tortoise-bot-lv-" + uuid.uuid4().hex[:12]
            stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
            cls.evidence = p.ROOT / "local" / (cls.project + "-" + stamp)
            cls.evidence.mkdir(parents=True)
            (cls.evidence / "image-id.txt").write_text(
                p.command(["docker", "image", "inspect", "--format", "{{.Id}}", p.IMAGE]),
                encoding="utf-8")
            cls.base, cls.env = p.boot_lab(cls.project, cls.evidence, world, _seed_sql())
            p.command(["docker", "compose"] + cls.base + ["up", "-d", "--no-deps", "world"],
                      env=cls.env)
            cls.logs = p.wait_for(cls.base, cls.env, _done, deadline=420)
            (cls.evidence / "drive.log").write_text(cls.logs, encoding="utf-8")
            end = time.monotonic() + 15
            while time.monotonic() < end:
                time.sleep(3)
            p.command(["docker", "compose"] + cls.base + ["stop", "world"],
                      env=cls.env, timeout=180)
            cls.db_row = p.db_exec(cls.base, cls.env,
                "SELECT guid,account,level,xp,money,health FROM characters WHERE guid=%d" % C)
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
                                             ["logs", "--no-color", "world"], env=cls.env,
                                             timeout=60)
                    (cls.evidence / "world-failure.log").write_text(failure_logs,
                                                                    encoding="utf-8")
                except Exception:
                    pass
                p.force_down(cls.base, cls.env)
            raise
        cls.personal_after = cls._personal_state()

    # ------------------------------------------------------------------ basic

    def test_all_seeded_bots_logged_in(self):
        for g, n in ((O, "Lsowner"), (C, COMP_NAME)):
            self.assertIn("test-login %d first=1 second=0" % g, self.logs)
            self.assertIn("[PlayerBot][Login]  '%s' GUID:%d" % (n, g), self.logs)
        self.assertNotIn("[CRASH]", self.logs)

    def test_party_formed_and_intact(self):
        self.assertIn("party recruit accepted bot:%s guid:%d leader:%d"
                      % (COMP_NAME, C, O), self.logs)
        self.assertNotIn("party recruit rejected", self.logs)
        self.assertNotIn("party dismiss", self.logs)

    def test_assist_accepted(self):
        self.assertIn("assist accepted bot:%s target:%s guid:%d"
                      % (COMP_NAME, KILLER_NAME, K), self.logs)
        self.assertNotIn("assist rejected", self.logs)

    # --------------------------------------------------------------- recovery

    def _death_and_alive_indices(self):
        i_dead = self.logs.find("bot dead GUID:%d" % C)
        self.assertGreater(i_dead, -1, "the companion never died")
        i_entered = self.logs.find("[PlayerBot][Recovery] entered GUID:%d" % C)
        i_issued = self.logs.find("[PlayerBot][Recovery] reclaim issued GUID:%d" % C)
        m = BOT_ALIVE.search(self.logs, i_dead)
        self.assertIsNotNone(m, "the companion never came back alive")
        return i_dead, i_entered, i_issued, m.start()

    def test_died_and_recovered_in_order(self):
        i_dead, i_entered, i_issued, i_alive = self._death_and_alive_indices()
        self.assertGreater(i_entered, -1, "recovery never entered")
        self.assertGreater(i_entered, i_dead, "recovery entered before death")
        self.assertGreater(i_issued, i_entered, "reclaim issued before entered")
        self.assertGreater(i_alive, i_issued, "came back alive before reclaim issued")

    def test_recovery_window_matches_reclaim_delay(self):
        i_dead, _, _, i_alive = self._death_and_alive_indices()
        window = self.logs[i_dead:i_alive]
        n = sum(1 for m in STATE.findall(window) if m[0] == str(C))
        lo, hi = RECOVERY_BAND
        print("recovery window: %d x 10 s state lines dead-to-alive "
              "(~%d-%d s; nominal 30 s reclaim delay)" % (n, lo * 10, hi * 10))
        self.assertGreaterEqual(n, lo, "the companion resurrected far too fast (%d lines)" % n)
        self.assertLessEqual(n, hi, "the companion stayed dead far too long (%d lines)" % n)

    def test_no_offense_while_dead(self):
        i_dead, _, _, i_alive = self._death_and_alive_indices()
        window = self.logs[i_dead + 1:i_alive]
        for line in window.splitlines():
            if ("GUID:%d" % C) not in line:
                continue
            self.assertFalse(
                any(k in line for k in OFFENSIVE_KEYWORDS),
                "companion acted offensively while dead: %s" % line.strip())

    def test_resurrect_not_a_full_restore(self):
        i_dead, _, _, _ = self._death_and_alive_indices()
        m = BOT_ALIVE.search(self.logs, i_dead)
        hp, mx = int(m.group(2)), int(m.group(3))
        print("resurrect hp: %d/%d (%.2f)" % (hp, mx, hp / float(mx)))
        self.assertGreater(hp, 0)
        self.assertLessEqual(hp, 0.55 * mx,
                             "resurrect above the 50%% restore: %d/%d" % (hp, mx))

    def test_owner_never_died(self):
        self.assertNotIn("bot dead GUID:%d" % O, self.logs)

    # ------------------------------------------------------------- regrouping

    def test_regroup_reached_after_last_follow(self):
        idx = self.logs.rfind("follow accepted bot:%s" % COMP_NAME)
        self.assertGreater(idx, -1)
        reached = [m for m in f.REACHED.findall(self.logs[idx:]) if m[0] == str(C)]
        self.assertTrue(reached, "the companion never regrouped after the last follow")
        self.assertLessEqual(float(reached[0][2]), 2.0)

    # ------------------------------------------------------------------ state

    def test_saved_state_preserved(self):
        self.assertTrue(self.db_row.strip(), "no saved row for the companion")
        fields = self.db_row.strip().splitlines()[0].split("\t")
        self.assertEqual(len(fields), 6, "unexpected DB row: %r" % self.db_row)
        guid, account, level, xp, money, health = (int(v) for v in fields)
        self.assertEqual(guid, C)
        self.assertEqual(account, int("1000" + str(C)),
                         "account must match the seeded 1000<guid>: %r" % self.db_row)
        self.assertEqual(level, 10, "level must be preserved: %r" % self.db_row)
        self.assertGreaterEqual(xp, 5000, "earned xp must be preserved: %r" % self.db_row)
        self.assertEqual(money, 100000, "money must be preserved: %r" % self.db_row)
        self.assertGreater(health, 0, "the saved companion must be alive: %r" % self.db_row)

    def test_personal_volume_untouched(self):
        self.assertNotEqual(self.project, "tortoise-local")
        if self.personal_before:
            self.assertEqual(self.personal_after, self.personal_before,
                             "personal server containers must not be restarted or touched")


if __name__ == "__main__":
    unittest.main()
