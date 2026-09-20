"""PORT-023 (KAP-558): one owner-driven cooperative quest vertical slice.

Declared quest (verified from the current tw_world data, 2026-09-16):

  456 "The Balance of Nature"
  - Method 2 (turn-in required), MinLevel 1, QuestLevel 2, race mask 589
    (Human bit set), no prerequisite (PrevQuestId 0), not repeatable.
  - Objectives (kill only, no item/cast/talk): 7x Young Nightsaber (2031,
    level 1) + 4x Young Thistle Boar (1984, level 1-2).
  - Giver and finisher: Conservator Ilthalaine (2079, level 4) via the one
    creature_questrelation row (2079, 456).
  - Rewards: 170 quest XP (XPValue, full at levels <= 27), 35 copper
    (RewOrReqMoney, scaled by Rate.Drop.Money), choice item index 0 ->
    5394 Archery Training Gloves (index 1 -> 11187 Stemleaf Bracers; the
    socketless path deterministically takes choice 0).

Group-credit rule (Group::RewardGroupAtKill): when a party member taps a
creature and it dies, every party member at group reward distance
(MaxGroupXPDistance, default 74 yd) who is alive or dead with an
unreleased corpse receives KilledMonsterCredit on its own quest log - the
vanilla tap/group credit. In Lab A the owner never attacks; its full 7/4
kill credit is pure group credit from the companion's kills.

Labs (disposable Compose projects, unique, port-free):
  A (eligible): owner accepts (lab quest script, the same authoritative
    helpers the packet handlers use); the companion mirrors: hold-
    suppressed accept, party-loss suppression, 11-kill credit to both logs,
    companion turn-in through the normal reward path, owner turn-in, clean
    stop + world restart persistence (quest rows, XP, money, inventory).
  B (ineligible): the owner never accepts; standing at the giver with the
    quest declared, the companion must do nothing (no accept, no credit,
    no reward, no rows).
  C (death/recovery): the companion accepts, is killed by a pinned
    unkillable Minion of Sethir (6911, 2000 HP, base damage), runs the one
    normal corpse-reclaim recovery, and its INCOMPLETE quest row persists
    through death and resurrection (no erasure, no turn-in, no crash).
  D (PORT-033, mirror mode): giver/finisher split - the owner accepts
    456 from the giver (2079), the companion mirrors the accept at the
    giver and turns in at the finisher (6911, a different creature with
    the QUESTGIVER npcflag) where the kill pack stands; the logged anchor
    entry proves the split; restart persistence with the owner
    cross-check in force.
  E (PORT-033, negative): a seeded COMPLETE + unrewarded quest row (457)
    the owner does not hold stays untouched while the companion stands at
    a legitimate 457 finisher - the mirror turn-in owner cross-check.
  F (KAP-558 review): persisted mirror provenance - both logs hold a
    seeded 457 row and the owner self-binds, so the owner-holds
    cross-check alone would have rewarded; with no marker row the marker
    gate keeps phase 1 quiet, and after a marker insert + restart the
    companion turns in (the reward consumes the marker) while the
    owner's unmarked row stays untouched.

No human play data, no personal-world restart, no personal containers.
"""
import os
import re
import threading
import time
import unittest
import uuid
from datetime import datetime, timezone

import test_bot_provision as p
import test_bot_follow as f

QUEST_ID = "456"
GIVER_ENTRY = 2079
REWARD_ITEM = 5394


def _quest_row(base, env, guid, quest=QUEST_ID):
    out = p.db_exec(base, env,
                    "SELECT status, rewarded, mobcount1, mobcount2 "
                    "FROM character_queststatus WHERE guid=%d AND quest=%s"
                    % (guid, quest))
    out = out.strip()
    if not out:
        return "-"
    parts = out.split("\t")
    if len(parts) != 4:
        return "bad"
    return ":".join(parts)


def _char_state(base, env, guid):
    xp = p.db_int(base, env, "SELECT xp FROM characters WHERE guid=%d" % guid)
    money = p.db_int(base, env, "SELECT money FROM characters WHERE guid=%d" % guid)
    level = p.db_int(base, env, "SELECT level FROM characters WHERE guid=%d" % guid)
    return (xp, money, level)


def _char_pos(base, env, guid):
    # The periodic save (PLAYER_SAVE_INTERVAL=5000) keeps this fresh enough
    # for the 5 s poller: it traces the bot's search center so any assist
    # target-not-found can be judged against real geometry.
    out = p.db_exec(base, env,
                    "SELECT position_x, position_y FROM characters "
                    "WHERE guid=%d" % guid)
    out = out.strip()
    if not out:
        return "-"
    parts = out.split("\t")
    if len(parts) != 2:
        return "bad"
    return "%s,%s" % (parts[0], parts[1])


def _inv_reward(base, env, guid):
    out = p.db_exec(base, env,
                    "SELECT COALESCE(COUNT(*),0) FROM character_inventory "
                    "WHERE guid=%d AND item_template=%d" % (guid, REWARD_ITEM))
    return int(out.strip() or "0")


def _full_state(base, env, guids):
    snap = {}
    for g in guids:
        snap[g] = {
            "quest": _quest_row(base, env, g),
            "state": _char_state(base, env, g),
            "inv": _inv_reward(base, env, g),
        }
    return snap


# PORT-034 (KAP-558): post-seed fixture self-assert. Lab D flakiness was
# silent fixture drift: the zero-damage UPDATE covered only the melee
# columns, but ranged damage lives in separate columns
# (ranged_dmg_min/max). The boar kept shooting 3.85-5.3 per 2 s at 30 yd,
# and the Conservator (2079, faction 80) resolved its NPC->player reaction
# through the Darnassus reputation list (faction_template row 80 ->
# reputation_list_id 21) and came back Hated, chipping the test
# characters. This helper queries, right after boot, exactly the
# tw_world.creature_template rows the fixture touched and asserts the
# neutralization each lab's assertions depend on, so future drift fails
# in seconds instead of at the 15-minute mark of a lab run.
GIVER_NEUTRAL = {"dmg_min": 0, "dmg_max": 0, "ranged_dmg_min": 0,
                 "ranged_dmg_max": 0, "detection_range": 0, "faction": 1}
PACK_ZERO = {"dmg_min": 0, "dmg_max": 0, "ranged_dmg_min": 0,
             "ranged_dmg_max": 0, "health_min": 8, "health_max": 8}


def _assert_fixture_templates(base, env, expected):
    """expected: entry -> {column: exact expected value}."""
    for entry in sorted(expected):
        cols = sorted(expected[entry])
        out = p.db_exec(
            base, env,
            "SELECT %s FROM tw_world.creature_template WHERE entry=%d"
            % (", ".join(cols), entry))
        got = out.strip().split("\t")
        if len(got) != len(cols):
            raise AssertionError(
                "fixture self-check: bad row for entry %d: %r" % (entry, out))
        for col, val in zip(cols, got):
            want = float(expected[entry][col])
            have = float(val)
            if have != want:
                raise AssertionError(
                    "fixture self-check: entry %d %s is %s, want %s"
                    % (entry, col, val, expected[entry][col]))


class _Poller:
    """5s quest/state sampler into questtimeline.txt (reconstructs the
    pre-reward progress and the persisted rows for the evidence)."""

    def __init__(self, base, env, guids, path):
        self.base, self.env, self.guids, self.path = base, env, guids, path
        self._stop = threading.Event()
        self._thread = None

    def start(self):
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def stop(self):
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=10)

    @staticmethod
    def _safe(fn, default="err"):
        try:
            return fn()
        except Exception:
            return default

    def _sample(self):
        # Per-part fault tolerance: one failing query must never kill the
        # whole sample (the old blanket try/except silently produced an
        # empty timeline when _inv_reward raised).
        rows, states = [], []
        for g in self.guids:
            q = self._safe(lambda g=g: _quest_row(self.base, self.env, g))
            s = self._safe(lambda g=g: _char_state(self.base, self.env, g),
                           (0, 0, 0))
            i = self._safe(lambda g=g: _inv_reward(self.base, self.env, g), 0)
            pos = self._safe(lambda g=g: _char_pos(self.base, self.env, g),
                             "-")
            rows.append("%d=%s" % (g, q))
            states.append("%d=xp:%d,money:%d,inv:%d,pos:%s"
                          % (g, s[0], s[1], i, pos))
        return ",".join(rows), ",".join(states)

    def _run(self):
        t0 = time.monotonic()
        with open(self.path, "a", encoding="utf-8") as fh:
            while not self._stop.is_set():
                try:
                    rows, states = self._sample()
                    fh.write("t=%.1f %s | %s\n"
                             % (time.monotonic() - t0, rows, states))
                    fh.flush()
                except Exception:
                    pass
                self._stop.wait(5)


# ---------------------------------------------------------------------------
# Lab A: the eligible pair. Full cooperative vertical slice + restart.
# ---------------------------------------------------------------------------
class BotQuestCoopEligibleTests(unittest.TestCase):
    O = 610600
    C = 610601
    ONAME = "Cqowner"
    CNAME = "Cqcomp"
    GIVER_GUID = 2500020
    SABER_GUIDS = [2500021, 2500022, 2500023, 2500024, 2500025, 2500026, 2500027]
    BOAR_GUIDS = [2500028, 2500029, 2500030, 2500031]
    FIXTURE_EXPECT = {2079: GIVER_NEUTRAL, 2031: PACK_ZERO, 1984: PACK_ZERO}

    base = env = project = evidence = None
    logs = logs_restart = ""
    snap_before_stop = snap_after_restart = None
    before = None

    @classmethod
    def _seed(cls):
        # The whole pack sits inside the owner's own grid cell (10.5-15.6 yd
        # away): the owner keeps that grid active for the entire run, and
        # every member stays well inside the 30 yd assist radius even if
        # the companion rests a few yd off the owner. The previous layout
        # put the boars at 27-31 yd, straddling the radius.
        sabers = [(-8958.5, -129.0), (-8959.5, -129.0), (-8958.5, -132.0),
                  (-8959.5, -132.0), (-8958.5, -135.0), (-8959.5, -135.0),
                  (-8958.5, -138.0)]
        boars = [(-8959.5, -138.0), (-8958.5, -141.0), (-8959.5, -141.0),
                 (-8958.5, -144.0)]
        rows = []
        for i, (x, y) in enumerate(sabers):
            rows.append("(%d,2031,0,%.2f,%.2f,83.5312,0,600,600,0,100,100,0,1)"
                        % (cls.SABER_GUIDS[i], x, y))
        for i, (x, y) in enumerate(boars):
            rows.append("(%d,1984,0,%.2f,%.2f,83.5312,0,600,600,0,100,100,0,1)"
                        % (cls.BOAR_GUIDS[i], x, y))
        return (
            "INSERT INTO tw_char.characters\n"
            " (guid,account,name,race,class,gender,level,money,position_x,position_y,position_z,map,\n"
            "  orientation,zone,health,power1,power2,power3,power4,power5,xp,xp_gain)\n"
            "VALUES\n"
            " (%d,1000%d,'%s',1,1,0,3,100000,-8947.95,-132.493,83.5312,0,\n"
            "  0,12,100,0,0,0,0,0,0,1),\n"
            " (%d,1000%d,'%s',1,1,0,3,100000,-8949.95,-200.493,83.5312,0,\n"
            "  0,12,100,0,0,0,0,0,0,1);\n"
            "INSERT INTO tw_char.playerbot (char_guid,chance,ai)\n"
            " VALUES (%d,100,'Default'),(%d,100,'Default');\n"
            "INSERT INTO tw_char.bot_ownership (char_guid,account_id,bot_type,provision_version,owner_account_id)\n"
            " VALUES (%d,1000%d,1,2,NULL),\n"
            "        (%d,1000%d,1,2,1000%d);\n"
            # Lab-only: the declared quest's creatures deal no damage, so
            # the level-3 companion cannot die in the kill sequence; the
            # death path is Lab C's job. Kills are one-shot (HP 8). Ranged
            # damage lives in separate columns (ranged_dmg_min/max): the
            # boar shoots 3.85-5.3 per 2 s at 30 yd in base data, so the
            # ranged columns are zeroed too (Lab D flakiness root cause).
            "UPDATE tw_world.creature_template SET dmg_min=0, dmg_max=0, ranged_dmg_min=0, "
            "ranged_dmg_max=0, health_min=8, health_max=8 WHERE entry IN (2031,1984);\n"
            # The Conservator (faction 80) resolves NPC->player reaction
            # through its faction_template row's Darnassus reputation list
            # (reputation_list_id 21); the lab bot came back Hated and was
            # chipped at the giver (Lab D flakiness root cause #2).
            # Neutralize: neutral faction, zero melee + ranged damage,
            # zero detection. questrelation and npc_flags are untouched.
            "UPDATE tw_world.creature_template SET faction=1, dmg_min=0, dmg_max=0, "
            "ranged_dmg_min=0, ranged_dmg_max=0, detection_range=0 WHERE entry=2079;\n"
            "INSERT INTO tw_world.creature\n"
            " (guid,id,map,position_x,position_y,position_z,orientation,spawntimesecsmin,\n"
            "  spawntimesecsmax,wander_distance,health_percent,mana_percent,movement_type,spawn_flags)\n"
            "VALUES\n"
            " (%d,%d,0,-8945.95,-132.493,83.5312,0,600,600,0,100,100,0,1),\n"
            % (cls.O, cls.O, cls.ONAME, cls.C, cls.C, cls.CNAME, cls.O, cls.C, cls.O, cls.O, cls.C, cls.C, cls.O, cls.GIVER_GUID, GIVER_ENTRY)
            + ",\n".join(rows) + ";\n"
            # Clear the ambient corridor (walk path + pack + giver area) so
            # only the seeded creatures are present; the restart observation
            # window stays deterministic.
            "DELETE FROM tw_world.creature WHERE map=0 AND position_x BETWEEN -8990 AND -8910\n"
            " AND position_y BETWEEN -230 AND -100 AND guid NOT IN (%s);\n"
            % ",".join(str(g) for g in
                       [cls.GIVER_GUID] + cls.SABER_GUIDS + cls.BOAR_GUIDS)
        )

    @classmethod
    def _script(cls):
        ev = [
            "0:%d:bothold %s" % (cls.O, cls.ONAME),
            "4000:%d:bothold %s" % (cls.O, cls.CNAME),
            "8000:%d:botrecruit %s" % (cls.O, cls.CNAME),
            "12000:%d:botfollow %s" % (cls.O, cls.CNAME),
            # Companion arrived at the owner (giver area); HOLD: the owner
            # accepts next while the companion is held - no accept may fire.
            "60000:%d:bothold %s" % (cls.O, cls.CNAME),
            # Follow releases the hold (PORT-004); the cooperative accept
            # fires on the next tick (owner INCOMPLETE, giver within 5 yd).
            "85000:%d:botfollow %s" % (cls.O, cls.CNAME),
            # Party loss: the companion (INCOMPLETE, at the giver) leaves
            # the party; its persisted quest row must survive, and no
            # cooperative action may fire while it is out.
            "95000:%d:botdismiss %s" % (cls.O, cls.CNAME),
            # Re-join; the kill phase steers the companion through the
            # zero-damage pack (name lookup picks any live match).
            "115000:%d:botrecruit %s" % (cls.O, cls.CNAME),
        ]
        t = 130000
        for i in range(7):
            ev.append("%d:%d:botassist %s Young Nightsaber" % (t, cls.O, cls.CNAME))
            t += 20000
        for i in range(4):
            ev.append("%d:%d:botassist %s Young Thistle Boar" % (t, cls.O, cls.CNAME))
            t += 20000
        # Companion returns to the owner (finisher area) -> cooperative
        # turn-in fires; the owner turns in via the lab quest script.
        ev.append("%d:%d:botfollow %s" % (t, cls.O, cls.CNAME))
        ev.append("%d:%d:bothold %s" % (t + 20000, cls.O, cls.ONAME))
        ev.append("%d:%d:bothold %s" % (t + 21000, cls.O, cls.CNAME))
        return ";".join(ev)

    @classmethod
    def setUpClass(cls):
        p.IMAGE = os.environ.get("PORT023_LAB_IMAGE", "tortoise-local:dev")
        try:
            p.command(["docker", "image", "inspect", p.IMAGE])
        except RuntimeError:
            raise unittest.SkipTest("%s image not present; build it first" % p.IMAGE)
        cls.before = f.BotFollowTests._personal_state()
        cls.project = "tortoise-bot-questcoop-" + uuid.uuid4().hex[:12]
        cls.evidence = p.ROOT / "local" / (cls.project + "-"
                                           + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ"))
        cls.evidence.mkdir(parents=True)
        world = p.world_env_for("")
        world.update(PLAYERBOT_ENABLE="1", PLAYERBOT_MIN_BOTS="0", PLAYERBOT_MAX_BOTS="0",
                     PLAYERBOT_REFRESH="600000", PLAYERBOT_UPDATE_MS="1000", PLAYERBOT_DEBUG="1",
                     PLAYERBOT_PROVISION="", PLAYERBOT_TEST_LOGIN="%d,%d" % (cls.O, cls.C),
                     PLAYERBOT_AMBIENT_ACQUIRE="0", PLAYERBOT_WANDER_RADIUS="1",
                     PLAYERBOT_QUEST_ID="0",
                     PLAYERBOT_COOPERATIVE_QUEST_ID=QUEST_ID,
                     PLAYER_SAVE_INTERVAL="5000",
                     PLAYERBOT_FOLLOW_SCRIPT=cls._script(),
                     PLAYERBOT_QUEST_SCRIPT="65000:%d:%s:accept;400000:%d:%s:turnin"
                                           % (cls.O, QUEST_ID, cls.O, QUEST_ID))
        cls.base = cls.env = None
        poller = None
        try:
            cls.base, cls.env = p.boot_lab(cls.project, cls.evidence, world, cls._seed())
            p.command(["docker", "compose"] + cls.base + ["up", "-d", "--no-deps", "world"], env=cls.env)
            _assert_fixture_templates(cls.base, cls.env, cls.FIXTURE_EXPECT)
            poller = _Poller(cls.base, cls.env, (cls.O, cls.C),
                             str(cls.evidence / "questtimeline.txt"))
            poller.start()
            cls.logs = p.wait_for(cls.base, cls.env,
                                  lambda text: "[PlayerBot][QuestScript] turnin issuer:%d quest:%s"
                                               % (cls.O, QUEST_ID) in text,
                                  deadline=900)
        except BaseException:
            if poller is not None:
                poller.stop()
            if cls.base is not None:
                try:
                    failure_logs = p.command(["docker", "compose"] + cls.base
                                             + ["logs", "--no-color", "world"],
                                             env=cls.env, timeout=60)
                    (cls.evidence / "world-failure.log").write_text(failure_logs, encoding="utf-8")
                except Exception:
                    pass
                p.force_down(cls.base, cls.env)
            raise
        # Let the post-turn-in state reach the periodic save, then snapshot.
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            if (_quest_row(cls.base, cls.env, cls.O) == "1:1:7:4"
                    and _quest_row(cls.base, cls.env, cls.C) == "1:1:7:4"):
                break
            time.sleep(5)
        if poller is not None:
            poller.stop()
        cls.snap_before_stop = _full_state(cls.base, cls.env, (cls.O, cls.C))
        (cls.evidence / "state_before_stop.txt").write_text(
            repr(cls.snap_before_stop), encoding="utf-8")
        # Clean stop (logout save), then the isolated world restart.
        # Compose logs accumulate across container generations of one
        # service, so scope the restart evidence to the tail after the
        # new generation's ready marker: the stale phase-1 tail already
        # contains the legitimate phase-1 login and turn-in lines.
        boot_marker = "World server is up and running!"
        boot_count = cls.logs.count(boot_marker)
        p.command(["docker", "compose"] + cls.base + ["stop", "world"], env=cls.env, timeout=180)
        p.command(["docker", "compose"] + cls.base + ["up", "-d", "--no-deps", "world"], env=cls.env)
        login_marker = "[PlayerBot][Login]  '%s' GUID:%d" % (cls.CNAME, cls.C)
        full = p.wait_for(cls.base, cls.env,
                          lambda text: (text.count(boot_marker) > boot_count
                                        and login_marker in
                                        text.rsplit(boot_marker, 1)[-1]),
                          deadline=480)
        cls.logs_restart = full.rsplit(boot_marker, 1)[-1]
        time.sleep(10)
        cls.snap_after_restart = _full_state(cls.base, cls.env, (cls.O, cls.C))
        (cls.evidence / "state_after_restart.txt").write_text(
            repr(cls.snap_after_restart), encoding="utf-8")
        (cls.evidence / "world.log").write_text(cls.logs, encoding="utf-8")
        (cls.evidence / "world-restart.log").write_text(cls.logs_restart, encoding="utf-8")

    @classmethod
    def tearDownClass(cls):
        if cls.base is not None:
            p.teardown_lab(cls.base, cls.env, cls.project, cls.evidence)

    def test_scripts_armed(self):
        self.assertIn("[PlayerBot][FollowScript] started events:", self.logs)
        self.assertIn("[PlayerBot][QuestScript] started events:", self.logs)

    def test_owner_accepted_via_authoritative_path(self):
        self.assertIn("[PlayerBot][QuestScript] accept issuer:%d quest:%s giver:%d ok:1"
                      % (self.O, QUEST_ID, GIVER_ENTRY), self.logs)

    def test_hold_suppressed_the_accept(self):
        # The owner's accept (tau65) lands while the companion is held
        # (tau60); the follow release (tau85, seq:4) must precede the
        # companion's accept.
        i_owner = self.logs.find("[PlayerBot][QuestScript] accept issuer:%d" % self.O)
        i_release = self.logs.find("[PlayerBot][Follow] active GUID:%d leader:%d seq:4"
                                   % (self.C, self.O))
        i_accept = self.logs.find("[CoopQuest] accepted GUID:%d quest:%s"
                                  % (self.C, QUEST_ID))
        self.assertNotEqual(i_owner, -1)
        self.assertNotEqual(i_release, -1)
        self.assertNotEqual(i_accept, -1)
        self.assertLess(i_owner, i_release)
        self.assertLess(i_release, i_accept)
        self.assertEqual(self.logs.count("[CoopQuest] accepted GUID:%d" % self.C), 1)

    def test_party_loss_suppressed_and_row_survived(self):
        i_accept = self.logs.find("[CoopQuest] accepted GUID:%d" % self.C)
        i_dismiss = self.logs.find("party dismiss accepted bot:%s guid:%d"
                                   % (self.CNAME, self.C))
        i_rejoin = self.logs.find("party recruit accepted bot:%s guid:%d leader:%d seq:3"
                                  % (self.CNAME, self.C, self.O))
        i_turnin = self.logs.find("[CoopQuest] turnin GUID:%d quest:%s"
                                  % (self.C, QUEST_ID))
        for label, idx in (("accept", i_accept), ("dismiss", i_dismiss),
                           ("rejoin", i_rejoin), ("turnin", i_turnin)):
            self.assertNotEqual(idx, -1, label + " marker missing")
        self.assertLess(i_accept, i_dismiss)
        self.assertLess(i_dismiss, i_rejoin)
        # The turn-in fired only after the re-join (party membership gate).
        self.assertLess(i_rejoin, i_turnin)
        # The persisted row survived the party loss (INCOMPLETE -> complete
        # -> rewarded, never erased): the timeline must contain the
        # companion INCOMPLETE before the turn-in sample.
        timeline = (self.evidence / "questtimeline.txt").read_text(encoding="utf-8")
        self.assertIn("%d=3:0:0:0" % self.C, timeline)
        self.assertIn("%d=1:1:7:4" % self.C, timeline)

    def test_kill_credit_on_both_logs(self):
        # The companion earned its personal 7/4 (tapped/participated kills);
        # the owner never attacked - its full 7/4 is pure tap/group credit.
        snap = self.snap_before_stop
        self.assertEqual(snap[self.C]["quest"], "1:1:7:4")
        self.assertEqual(snap[self.O]["quest"], "1:1:7:4")

    def test_companion_turnin_rewarded(self):
        m = re.search(r"\[CoopQuest\] turnin GUID:%d quest:%s anchor:\d+ xpBefore:(\d+) xpAfter:(\d+)"
                      % (self.C, QUEST_ID), self.logs)
        self.assertIsNotNone(m, "companion turn-in marker with xp delta missing")
        self.assertGreater(int(m.group(2)), int(m.group(1)))
        snap = self.snap_before_stop
        self.assertTrue(snap[self.C]["state"][0] > 0)
        self.assertGreaterEqual(snap[self.C]["state"][1], 100000)

    def test_owner_turnin_rewarded(self):
        m = re.search(r"\[PlayerBot\]\[QuestScript\] turnin issuer:%d quest:%s xpBefore:(\d+) xpAfter:(\d+)"
                      % (self.O, QUEST_ID), self.logs)
        self.assertIsNotNone(m, "owner turn-in marker with xp delta missing")
        self.assertGreater(int(m.group(2)), int(m.group(1)))
        self.assertTrue(self.snap_before_stop[self.O]["state"][0] > 0)

    def test_reward_item_in_inventory(self):
        snap = self.snap_before_stop
        self.assertEqual(snap[self.C]["inv"], 1, "companion choice item 5394")
        self.assertEqual(snap[self.O]["inv"], 1, "owner choice item 5394")

    def test_restart_persistence(self):
        self.assertIsNotNone(self.snap_before_stop)
        self.assertIsNotNone(self.snap_after_restart)
        for g in (self.O, self.C):
            self.assertEqual(self.snap_before_stop[g], self.snap_after_restart[g],
                             "state for guid %d changed across the restart" % g)
        # The companion re-logged in with the rewarded row: no new
        # cooperative action fires (rewarded suppresses the policy).
        self.assertNotIn("[CoopQuest] turnin GUID:%d" % self.C, self.logs_restart)
        self.assertNotIn("[CoopQuest] accepted GUID:%d" % self.C, self.logs_restart)

    def test_world_healthy(self):
        self.assertNotIn("[CRASH]", self.logs)
        self.assertNotIn("[CRASH]", self.logs_restart)

    def test_personal_state_untouched(self):
        self.assertEqual(self.before, f.BotFollowTests._personal_state())

    def test_declared_path_leaves_no_mirror_marker(self):
        # KAP-558 review (finding 2): the declared single-quest path is
        # not a mirror; its accept/turn-in must not write provenance
        # rows (only MirrorOwnerQuestStep does), so the declared lab
        # leaves bot_mirror_quest empty.
        for guid in (self.O, self.C):
            self.assertNotIn("[CoopQuest] mirror-marker GUID:%d" % guid,
                             self.logs)
        out = p.db_exec(self.base, self.env,
                        "SELECT COUNT(*) FROM bot_mirror_quest")
        self.assertEqual(int(out.strip() or "0"), 0,
                         "declared path must not write mirror markers")


# ---------------------------------------------------------------------------
# Lab B: the ineligible pair - the owner never accepts the declared quest.
# The companion stands at the giver for a quiet window and must do nothing.
# ---------------------------------------------------------------------------
class BotQuestCoopIneligibleTests(unittest.TestCase):
    O = 610700
    C = 610701
    ONAME = "Cqownrb"
    CNAME = "Cqcompb"
    GIVER_GUID = 2500040
    FIXTURE_EXPECT = {2079: GIVER_NEUTRAL}

    base = env = project = evidence = None
    logs = ""
    before = None

    @classmethod
    def _seed(cls):
        return (
            "INSERT INTO tw_char.characters\n"
            " (guid,account,name,race,class,gender,level,money,position_x,position_y,position_z,map,\n"
            "  orientation,zone,health,power1,power2,power3,power4,power5,xp,xp_gain)\n"
            "VALUES\n"
            " (%d,1000%d,'%s',1,1,0,3,100000,-8947.95,-132.493,83.5312,0,\n"
            "  0,12,100,0,0,0,0,0,0,1),\n"
            " (%d,1000%d,'%s',1,1,0,3,100000,-8949.95,-200.493,83.5312,0,\n"
            "  0,12,100,0,0,0,0,0,0,1);\n"
            "INSERT INTO tw_char.playerbot (char_guid,chance,ai)\n"
            " VALUES (%d,100,'Default'),(%d,100,'Default');\n"
            "INSERT INTO tw_char.bot_ownership (char_guid,account_id,bot_type,provision_version,owner_account_id)\n"
            " VALUES (%d,1000%d,1,2,NULL),\n"
            "        (%d,1000%d,1,2,1000%d);\n"
            # Giver neutralization (see Lab A): the faction-80 reputation
            # reaction made the Conservator hostile to the lab bots.
            "UPDATE tw_world.creature_template SET faction=1, dmg_min=0, dmg_max=0, "
            "ranged_dmg_min=0, ranged_dmg_max=0, detection_range=0 WHERE entry=2079;\n"
            "INSERT INTO tw_world.creature\n"
            " (guid,id,map,position_x,position_y,position_z,orientation,spawntimesecsmin,\n"
            "  spawntimesecsmax,wander_distance,health_percent,mana_percent,movement_type,spawn_flags)\n"
            "VALUES\n"
            " (%d,%d,0,-8945.95,-132.493,83.5312,0,600,600,0,100,100,0,1);\n"
            "DELETE FROM tw_world.creature WHERE map=0 AND position_x BETWEEN -8990 AND -8910\n"
            " AND position_y BETWEEN -230 AND -100 AND guid NOT IN (%d);\n"
            % (cls.O, cls.O, cls.ONAME, cls.C, cls.C, cls.CNAME, cls.O, cls.C, cls.O, cls.O, cls.C, cls.C, cls.O, cls.GIVER_GUID, GIVER_ENTRY, cls.GIVER_GUID)
        )

    @classmethod
    def setUpClass(cls):
        p.IMAGE = os.environ.get("PORT023_LAB_IMAGE", "tortoise-local:dev")
        try:
            p.command(["docker", "image", "inspect", p.IMAGE])
        except RuntimeError:
            raise unittest.SkipTest("%s image not present; build it first" % p.IMAGE)
        cls.before = f.BotFollowTests._personal_state()
        cls.project = "tortoise-bot-questcoobj-" + uuid.uuid4().hex[:12]
        cls.evidence = p.ROOT / "local" / (cls.project + "-"
                                           + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ"))
        cls.evidence.mkdir(parents=True)
        world = p.world_env_for("")
        world.update(PLAYERBOT_ENABLE="1", PLAYERBOT_MIN_BOTS="0", PLAYERBOT_MAX_BOTS="0",
                     PLAYERBOT_REFRESH="600000", PLAYERBOT_UPDATE_MS="1000", PLAYERBOT_DEBUG="1",
                     PLAYERBOT_PROVISION="", PLAYERBOT_TEST_LOGIN="%d,%d" % (cls.O, cls.C),
                     PLAYERBOT_AMBIENT_ACQUIRE="0", PLAYERBOT_WANDER_RADIUS="1",
                     PLAYERBOT_QUEST_ID="0",
                     PLAYERBOT_COOPERATIVE_QUEST_ID=QUEST_ID,
                     PLAYER_SAVE_INTERVAL="5000",
                     PLAYERBOT_FOLLOW_SCRIPT=";".join([
                         "0:%d:bothold %s" % (cls.O, cls.ONAME),
                         "4000:%d:bothold %s" % (cls.O, cls.CNAME),
                         "8000:%d:botrecruit %s" % (cls.O, cls.CNAME),
                         "12000:%d:botfollow %s" % (cls.O, cls.CNAME),
                         "60000:%d:bothold %s" % (cls.O, cls.CNAME)]))
        cls.base = cls.env = None
        try:
            cls.base, cls.env = p.boot_lab(cls.project, cls.evidence, world, cls._seed())
            p.command(["docker", "compose"] + cls.base + ["up", "-d", "--no-deps", "world"], env=cls.env)
            _assert_fixture_templates(cls.base, cls.env, cls.FIXTURE_EXPECT)
            cls.logs = p.wait_for(cls.base, cls.env,
                                  lambda text: "hold accepted bot:%s guid:%d"
                                               % (cls.CNAME, cls.C) in text,
                                  deadline=600)
            # Quiet observation window at the giver (the owner never accepts).
            time.sleep(90)
            cls.logs = p.command(["docker", "compose"] + cls.base
                                 + ["logs", "--no-color", "world"], env=cls.env, timeout=60)
            (cls.evidence / "world.log").write_text(cls.logs, encoding="utf-8")
        except BaseException:
            if cls.base is not None:
                try:
                    failure_logs = p.command(["docker", "compose"] + cls.base
                                             + ["logs", "--no-color", "world"],
                                             env=cls.env, timeout=60)
                    (cls.evidence / "world-failure.log").write_text(failure_logs, encoding="utf-8")
                except Exception:
                    pass
                p.force_down(cls.base, cls.env)
            raise

    @classmethod
    def tearDownClass(cls):
        if cls.base is not None:
            p.teardown_lab(cls.base, cls.env, cls.project, cls.evidence)

    def test_companion_recruited_and_arrived(self):
        self.assertIn("party recruit accepted bot:%s guid:%d" % (self.CNAME, self.C), self.logs)
        self.assertIn("[PlayerBot][Follow] active GUID:%d leader:%d" % (self.C, self.O), self.logs)

    def test_no_cooperative_action(self):
        self.assertNotIn("[CoopQuest]", self.logs)
        self.assertNotIn("quest accepted GUID:%d" % self.C, self.logs)

    def test_no_quest_state_anywhere(self):
        for g in (self.O, self.C):
            self.assertEqual(_quest_row(self.base, self.env, g), "-")

    def test_no_rewards(self):
        for g in (self.O, self.C):
            self.assertEqual(_inv_reward(self.base, self.env, g), 0)

    def test_world_healthy(self):
        self.assertNotIn("[CRASH]", self.logs)

    def test_personal_state_untouched(self):
        self.assertEqual(self.before, f.BotFollowTests._personal_state())


# ---------------------------------------------------------------------------
# Lab C: death/recovery with an active cooperative quest. The companion
# accepts, is killed by the pinned unkillable Minion of Sethir (6911),
# reclaims its corpse through the one normal recovery path, and its
# INCOMPLETE quest row persists through the whole arc.
# ---------------------------------------------------------------------------
class BotQuestCoopDeathTests(unittest.TestCase):
    O = 610800
    C = 610801
    ONAME = "Cqownrc"
    CNAME = "Cqcompc"
    GIVER_GUID = 2500060
    KILLER = 2500300
    KILLER_NAME = "Minion of Sethir"
    FIXTURE_EXPECT = {
        2079: GIVER_NEUTRAL,
        # Designated killer: the fixture pins 2000 HP; the base melee
        # damage is load-bearing for the death assertion (it must still
        # retaliate).
        6911: {"dmg_min": 28.6, "dmg_max": 31.9, "health_min": 2000,
               "health_max": 2000},
    }

    base = env = project = evidence = None
    logs = ""
    before = None

    @classmethod
    def _seed(cls):
        return (
            "INSERT INTO tw_char.characters\n"
            " (guid,account,name,race,class,gender,level,money,position_x,position_y,position_z,map,\n"
            "  orientation,zone,health,power1,power2,power3,power4,power5,xp,xp_gain)\n"
            "VALUES\n"
            " (%d,1000%d,'%s',1,1,0,3,100000,-8947.5,-131.5,83.5312,0,\n"
            "  0,12,100,0,0,0,0,0,0,1),\n"
            " (%d,1000%d,'%s',1,1,0,3,100000,-8949.95,-200.493,83.5312,0,\n"
            "  0,12,100,0,0,0,0,0,0,1);\n"
            "INSERT INTO tw_char.playerbot (char_guid,chance,ai)\n"
            " VALUES (%d,100,'Default'),(%d,100,'Default');\n"
            "INSERT INTO tw_char.bot_ownership (char_guid,account_id,bot_type,provision_version,owner_account_id)\n"
            " VALUES (%d,1000%d,1,2,NULL),\n"
            "        (%d,1000%d,1,2,1000%d);\n"
            # Pinned unkillable minion (2000 HP, regen 0, base damage
            # 28.6-31.9 @ 1890 ms, base flags: it must retaliate; its 18 yd
            # detection reaches neither the follow path (~21 yd) nor the
            # giver area (~20 yd)). 20 yd east and 14 yd south of the owner.
            "UPDATE tw_world.creature_template SET loot_id=0 WHERE entry=6911;\n"
            "UPDATE tw_world.creature_template SET health_min=2000, health_max=2000, regeneration=0 "
            "WHERE entry=6911;\n"
            # Giver neutralization (see Lab A); the killer 6911 keeps its
            # base damage on purpose.
            "UPDATE tw_world.creature_template SET faction=1, dmg_min=0, dmg_max=0, "
            "ranged_dmg_min=0, ranged_dmg_max=0, detection_range=0 WHERE entry=2079;\n"
            "INSERT INTO tw_world.creature\n"
            " (guid,id,map,position_x,position_y,position_z,orientation,spawntimesecsmin,\n"
            "  spawntimesecsmax,wander_distance,health_percent,mana_percent,movement_type,spawn_flags)\n"
            "VALUES\n"
            " (%d,%d,0,-8945.95,-132.493,83.5312,0,600,600,0,100,100,0,1),\n"
            " (%d,6911,0,-8927.5,-145.5,83.5312,0,600,600,0,100,100,0,1);\n"
            "DELETE FROM tw_world.creature WHERE map=0 AND position_x BETWEEN -8990 AND -8910\n"
            " AND position_y BETWEEN -230 AND -100 AND guid NOT IN (%d,%d);\n"
            % (cls.O, cls.O, cls.ONAME, cls.C, cls.C, cls.CNAME, cls.O, cls.C, cls.O, cls.O, cls.C, cls.C, cls.O, cls.GIVER_GUID, GIVER_ENTRY, cls.KILLER, cls.GIVER_GUID, cls.KILLER)
        )

    @classmethod
    def _script(cls):
        return ";".join([
            "0:%d:bothold %s" % (cls.O, cls.ONAME),
            "4000:%d:bothold %s" % (cls.O, cls.CNAME),
            "8000:%d:botrecruit %s" % (cls.O, cls.CNAME),
            "12000:%d:botfollow %s" % (cls.O, cls.CNAME),
            "30000:%d:bothold %s" % (cls.O, cls.CNAME),
            # Follow releases the hold; the cooperative accept fires at the
            # giver (the owner accepted at tau35 via the quest script).
            "50000:%d:botfollow %s" % (cls.O, cls.CNAME),
            # The companion (INCOMPLETE) walks to the pinned minion and dies.
            "75000:%d:botassist %s %s" % (cls.O, cls.CNAME, cls.KILLER_NAME),
            # Backstop: lands while dead/reclaiming or still in the fight;
            # the minion is unkillable so no second death cycle starts.
            "140000:%d:bothold %s" % (cls.O, cls.CNAME),
            "280000:%d:botfollow %s" % (cls.O, cls.CNAME),
            "320000:%d:bothold %s" % (cls.O, cls.CNAME),
        ])

    @classmethod
    def setUpClass(cls):
        p.IMAGE = os.environ.get("PORT023_LAB_IMAGE", "tortoise-local:dev")
        try:
            p.command(["docker", "image", "inspect", p.IMAGE])
        except RuntimeError:
            raise unittest.SkipTest("%s image not present; build it first" % p.IMAGE)
        cls.before = f.BotFollowTests._personal_state()
        cls.project = "tortoise-bot-questcoopd-" + uuid.uuid4().hex[:12]
        cls.evidence = p.ROOT / "local" / (cls.project + "-"
                                           + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ"))
        cls.evidence.mkdir(parents=True)
        world = p.world_env_for("")
        world.update(PLAYERBOT_ENABLE="1", PLAYERBOT_MIN_BOTS="0", PLAYERBOT_MAX_BOTS="0",
                     PLAYERBOT_REFRESH="600000", PLAYERBOT_UPDATE_MS="1000", PLAYERBOT_DEBUG="1",
                     PLAYERBOT_PROVISION="", PLAYERBOT_TEST_LOGIN="%d,%d" % (cls.O, cls.C),
                     PLAYERBOT_AMBIENT_ACQUIRE="0", PLAYERBOT_WANDER_RADIUS="1",
                     PLAYERBOT_QUEST_ID="0",
                     PLAYERBOT_COOPERATIVE_QUEST_ID=QUEST_ID,
                     PLAYER_SAVE_INTERVAL="5000",
                     PLAYERBOT_FOLLOW_SCRIPT=cls._script(),
                     PLAYERBOT_QUEST_SCRIPT="35000:%d:%s:accept" % (cls.O, QUEST_ID))
        cls.base = cls.env = None
        try:
            cls.base, cls.env = p.boot_lab(cls.project, cls.evidence, world, cls._seed())
            p.command(["docker", "compose"] + cls.base + ["up", "-d", "--no-deps", "world"], env=cls.env)
            _assert_fixture_templates(cls.base, cls.env, cls.FIXTURE_EXPECT)
            cls.logs = p.wait_for(cls.base, cls.env,
                                  lambda text: "[PlayerBot][Hold] active GUID:%d seq:8" % cls.C in text,
                                  deadline=720)
            (cls.evidence / "world.log").write_text(cls.logs, encoding="utf-8")
        except BaseException:
            if cls.base is not None:
                try:
                    failure_logs = p.command(["docker", "compose"] + cls.base
                                             + ["logs", "--no-color", "world"],
                                             env=cls.env, timeout=60)
                    (cls.evidence / "world-failure.log").write_text(failure_logs, encoding="utf-8")
                except Exception:
                    pass
                p.force_down(cls.base, cls.env)
            raise

    @classmethod
    def tearDownClass(cls):
        if cls.base is not None:
            p.teardown_lab(cls.base, cls.env, cls.project, cls.evidence)

    def test_companion_accepted_the_quest(self):
        self.assertEqual(
            self.logs.count("[CoopQuest] accepted GUID:%d quest:%s" % (self.C, QUEST_ID)), 1)

    def test_owner_accepted(self):
        self.assertIn("[PlayerBot][QuestScript] accept issuer:%d quest:%s giver:%d ok:1"
                      % (self.O, QUEST_ID, GIVER_ENTRY), self.logs)

    def test_companion_died_and_recovered(self):
        i_dead = self.logs.find("bot dead GUID:%d" % self.C)
        i_entered = self.logs.find("[PlayerBot][Recovery] entered GUID:%d" % self.C)
        i_issued = self.logs.find("[PlayerBot][Recovery] reclaim issued GUID:%d" % self.C)
        i_alive = self.logs.rfind("bot alive GUID:%d" % self.C)
        for label, idx in (("dead", i_dead), ("entered", i_entered),
                           ("issued", i_issued), ("alive", i_alive)):
            self.assertNotEqual(idx, -1, label + " marker missing")
        self.assertLess(i_dead, i_entered)
        self.assertLess(i_entered, i_issued)
        self.assertLess(i_issued, i_alive)
        # The owner never died.
        self.assertNotIn("bot dead GUID:%d" % self.O, self.logs)

    def test_quest_state_survived_death(self):
        # Both rows persist INCOMPLETE with zero kill credit (no objective
        # was ever performed); the death erased nothing and the recovery
        # did not fabricate credit.
        self.assertEqual(_quest_row(self.base, self.env, self.C), "3:0:0:0")
        self.assertEqual(_quest_row(self.base, self.env, self.O), "3:0:0:0")

    def test_no_turnin_no_reward(self):
        self.assertNotIn("[CoopQuest] turnin", self.logs)
        self.assertNotIn("[PlayerBot][QuestScript] turnin", self.logs)
        self.assertEqual(_inv_reward(self.base, self.env, self.C), 0)
        self.assertEqual(_inv_reward(self.base, self.env, self.O), 0)

    def test_world_healthy(self):
        self.assertNotIn("[CRASH]", self.logs)

    def test_personal_state_untouched(self):
        self.assertEqual(self.before, f.BotFollowTests._personal_state())



# ---------------------------------------------------------------------------
# Lab D: PORT-033 (KAP-558) - mirror mode with the giver/finisher split.
# The owner accepts 456 from the giver (2079); the companion mirrors the
# accept at the giver, completes the 11-kill pack standing at the finisher
# (6911, a different creature with the QUESTGIVER npcflag), and turns in
# there. The giver is 24+ yd from the pack, so only a finisher anchor can
# fire the turn-in; the logged anchor entry is the direct proof of the
# split. The restart phase checks that the owner cross-check plus the
# rewarded row suppress any re-action on re-login.
# ---------------------------------------------------------------------------
class BotQuestCoopFinisherTests(unittest.TestCase):
    O = 610620
    C = 610621
    ONAME = "Fcowner"
    CNAME = "Fccomp"
    GIVER_GUID = 2500080
    FINISHER_GUID = 2500081
    SABER_GUIDS = [2500082, 2500083, 2500084, 2500085, 2500086, 2500087, 2500088]
    BOAR_GUIDS = [2500089, 2500090, 2500091, 2500092]
    FINISHER_ENTRY = 6911
    FIXTURE_EXPECT = {
        2079: GIVER_NEUTRAL,
        2031: PACK_ZERO,
        1984: PACK_ZERO,
        6911: {"dmg_min": 0, "dmg_max": 0, "ranged_dmg_min": 0,
               "ranged_dmg_max": 0, "detection_range": 0,
               "health_min": 99999, "health_max": 99999},
    }

    base = env = project = evidence = None
    logs = logs_restart = ""
    snap_before_stop = snap_after_restart = None
    before = None

    @classmethod
    def _seed(cls):
        # Every mob sits within 4 yd of the finisher and within 27 yd of
        # the owner (30 yd assist radius). The finisher is 23.2 yd from
        # the owner and 24.5 yd from the giver, so when the last credit
        # lands the companion stands at the pack and only the finisher is
        # inside the 5 yd interaction distance.
        sabers = [(-8959.0, -148.5), (-8959.5, -151.0), (-8960.0, -153.5),
                  (-8961.5, -148.0), (-8962.5, -149.0), (-8963.5, -152.5),
                  (-8961.0, -150.0)]
        boars = [(-8964.0, -149.0), (-8964.5, -151.5), (-8963.5, -154.0),
                 (-8962.0, -155.0)]
        rows = []
        for i, (x, y) in enumerate(sabers):
            rows.append("(%d,2031,0,%.2f,%.2f,83.5312,0,600,600,0,100,100,0,1)"
                        % (cls.SABER_GUIDS[i], x, y))
        for i, (x, y) in enumerate(boars):
            rows.append("(%d,1984,0,%.2f,%.2f,83.5312,0,600,600,0,100,100,0,1)"
                        % (cls.BOAR_GUIDS[i], x, y))
        return (
            "INSERT INTO tw_char.characters\n"
            " (guid,account,name,race,class,gender,level,money,position_x,position_y,position_z,map,\n"
            "  orientation,zone,health,power1,power2,power3,power4,power5,xp,xp_gain)\n"
            "VALUES\n"
            " (%d,1000%d,'%s',1,1,0,3,100000,-8947.95,-132.493,83.5312,0,\n"
            "  0,12,100,0,0,0,0,0,0,1),\n"
            " (%d,1000%d,'%s',1,1,0,3,100000,-8949.95,-200.493,83.5312,0,\n"
            "  0,12,100,0,0,0,0,0,0,1);\n"
            "INSERT INTO tw_char.playerbot (char_guid,chance,ai)\n"
            " VALUES (%d,100,'Default'),(%d,100,'Default');\n"
            "INSERT INTO tw_char.bot_ownership (char_guid,account_id,bot_type,provision_version,owner_account_id)\n"
            # Owner self-binds (owner_account_id = its own lab
            # account): a NULL owner rejects the bothold below
            # ("hold rejected unowned bot") and lets the owner's
            # own bot AI random-walk away from spawn, pulling the
            # companion past the 30 yd assist range (run #3 boar
            # stall root cause).
            " VALUES (%d,1000%d,1,2,1000%d),\n"
            "        (%d,1000%d,1,2,1000%d);\n"
            # Ranged damage lives in separate columns (ranged_dmg_min/max):
            # the boar shoots 3.85-5.3 per 2 s at 30 yd in base data, so
            # the ranged columns are zeroed too (Lab D flakiness root
            # cause - the finisher-position HP drain was boar ranged fire).
            "UPDATE tw_world.creature_template SET dmg_min=0, dmg_max=0, ranged_dmg_min=0, "
            "ranged_dmg_max=0, health_min=8, health_max=8 WHERE entry IN (2031,1984);\n"
            # Giver neutralization (see Lab A). The finisher also takes
            # faction 1: base faction 14 (Monster) is hostile to players
            # via the faction_template mask fallback (hostile_mask 1 &
            # player our_mask 3), and CanInteractWithNPC rejects hostile
            # targets even at 0 yd - a real quest finisher is friendly.
            "UPDATE tw_world.creature_template SET faction=1, dmg_min=0, dmg_max=0, "
            "ranged_dmg_min=0, ranged_dmg_max=0, detection_range=0 WHERE entry=2079;\n"
            # PORT-033 fixture: split the one 456 relation - the giver
            # keeps the questrelation, the finisher takes the
            # involvedrelation and the QUESTGIVER npcflag (fork: 0x2) so
            # CanInteractWithQuestGiver passes for it.
            "DELETE FROM tw_world.creature_questrelation WHERE quest=456;\n"
            "DELETE FROM tw_world.creature_involvedrelation WHERE quest=456;\n"
            "INSERT INTO tw_world.creature_questrelation (id, quest) VALUES (2079, 456);\n"
            "INSERT INTO tw_world.creature_involvedrelation (id, quest) VALUES (6911, 456);\n"
            "UPDATE tw_world.creature_template SET npc_flags = npc_flags | 2 WHERE entry=6911;\n"
            # The finisher must be a non-threat during the kill
            # phase: it stands 3-4 yd inside the pack and would
            # otherwise aggro the companion (Minion of Sethir deals
            # real melee damage and shoots 18.3-25.2 per ~2 s at
            # range). Zero melee + ranged damage, zero detection and
            # tanky HP keep it alive and harmless; nothing targets
            # it, so it never dies.
            "UPDATE tw_world.creature_template SET faction=1, dmg_min=0, dmg_max=0, "
            "ranged_dmg_min=0, ranged_dmg_max=0, detection_range=0, "
            "health_min=99999, health_max=99999 WHERE entry=6911;\n"
            "INSERT INTO tw_world.creature\n"
            " (guid,id,map,position_x,position_y,position_z,orientation,spawntimesecsmin,\n"
            "  spawntimesecsmax,wander_distance,health_percent,mana_percent,movement_type,spawn_flags)\n"
            "VALUES\n"
            " (%d,%d,0,-8945.95,-132.493,83.5312,0,600,600,0,100,100,0,1),\n"
            " (%d,%d,0,-8962.0,-151.0,83.5312,0,600,600,0,100,100,0,1),\n"
            % (cls.O, cls.O, cls.ONAME, cls.C, cls.C, cls.CNAME,
               cls.O, cls.C, cls.O, cls.O, cls.O, cls.C, cls.C, cls.O,
               cls.GIVER_GUID, GIVER_ENTRY, cls.FINISHER_GUID, cls.FINISHER_ENTRY)
            + ",\n".join(rows) + ";\n"
            "DELETE FROM tw_world.creature WHERE map=0 AND position_x BETWEEN -8990 AND -8910\n"
            " AND position_y BETWEEN -230 AND -100 AND guid NOT IN (%s);\n"
            % ",".join(str(g) for g in
                       [cls.GIVER_GUID, cls.FINISHER_GUID] + cls.SABER_GUIDS + cls.BOAR_GUIDS)
        )

    @classmethod
    def _script(cls):
        ev = [
            "0:%d:bothold %s" % (cls.O, cls.ONAME),
            "4000:%d:bothold %s" % (cls.O, cls.CNAME),
            "8000:%d:botrecruit %s" % (cls.O, cls.CNAME),
            "12000:%d:botfollow %s" % (cls.O, cls.CNAME),
            # Companion arrived at the owner (giver area); HOLD re-pins.
            # The mirror path has no hold gate (PORT-030): the mirror
            # accept fires on the tick after the owner's accept below.
            "60000:%d:bothold %s" % (cls.O, cls.CNAME),
            "85000:%d:botfollow %s" % (cls.O, cls.CNAME),
            # Party loss: the companion (INCOMPLETE, at the giver) leaves
            # the party; its persisted quest row must survive, and no
            # cooperative action may fire while it is out.
            "95000:%d:botdismiss %s" % (cls.O, cls.CNAME),
            # Re-join; the kill phase steers the companion through the
            # zero-damage pack (name lookup picks any live match).
            "115000:%d:botrecruit %s" % (cls.O, cls.CNAME),
        ]
        t = 130000
        for i in range(7):
            ev.append("%d:%d:botassist %s Young Nightsaber" % (t, cls.O, cls.CNAME))
            t += 20000
        for i in range(4):
            ev.append("%d:%d:botassist %s Young Thistle Boar" % (t, cls.O, cls.CNAME))
            t += 20000
        # Companion returns to the owner (giver area); the turn-in has
        # already fired at the finisher. The owner turns in via the lab
        # quest script.
        ev.append("%d:%d:botfollow %s" % (t, cls.O, cls.CNAME))
        ev.append("%d:%d:bothold %s" % (t + 20000, cls.O, cls.ONAME))
        ev.append("%d:%d:bothold %s" % (t + 21000, cls.O, cls.CNAME))
        return ";".join(ev)

    @classmethod
    def setUpClass(cls):
        p.IMAGE = os.environ.get("PORT023_LAB_IMAGE", "tortoise-local:dev")
        try:
            p.command(["docker", "image", "inspect", p.IMAGE])
        except RuntimeError:
            raise unittest.SkipTest("%s image not present; build it first" % p.IMAGE)
        cls.before = f.BotFollowTests._personal_state()
        cls.project = "tortoise-bot-questcoope-" + uuid.uuid4().hex[:12]
        cls.evidence = p.ROOT / "local" / (cls.project + "-"
                                           + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ"))
        cls.evidence.mkdir(parents=True)
        world = p.world_env_for("")
        world.update(PLAYERBOT_ENABLE="1", PLAYERBOT_MIN_BOTS="0", PLAYERBOT_MAX_BOTS="0",
                     PLAYERBOT_REFRESH="600000", PLAYERBOT_UPDATE_MS="1000", PLAYERBOT_DEBUG="1",
                     PLAYERBOT_PROVISION="", PLAYERBOT_TEST_LOGIN="%d,%d" % (cls.O, cls.C),
                     PLAYERBOT_AMBIENT_ACQUIRE="0", PLAYERBOT_WANDER_RADIUS="1",
                     PLAYERBOT_QUEST_ID="0",
                     PLAYERBOT_COOPERATIVE_QUEST_ID="0",
                     PLAYERBOT_MIRROR_OWNER_QUESTS="1",
                     PLAYER_SAVE_INTERVAL="5000",
                     PLAYERBOT_FOLLOW_SCRIPT=cls._script(),
                     PLAYERBOT_QUEST_SCRIPT="65000:%d:%s:accept;400000:%d:%s:turnin;480000:%d:%s:turnin;560000:%d:%s:turnin;640000:%d:%s:turnin"
                                           % (cls.O, QUEST_ID, cls.O, QUEST_ID, cls.O, QUEST_ID, cls.O, QUEST_ID, cls.O, QUEST_ID))
        cls.base = cls.env = None
        poller = None
        try:
            cls.base, cls.env = p.boot_lab(cls.project, cls.evidence, world, cls._seed())
            p.command(["docker", "compose"] + cls.base + ["up", "-d", "--no-deps", "world"], env=cls.env)
            _assert_fixture_templates(cls.base, cls.env, cls.FIXTURE_EXPECT)
            poller = _Poller(cls.base, cls.env, (cls.O, cls.C),
                             str(cls.evidence / "questtimeline.txt"))
            poller.start()
            cls.logs = p.wait_for(cls.base, cls.env,
                                  lambda text: "[PlayerBot][QuestScript] turnin issuer:%d quest:%s"
                                               % (cls.O, QUEST_ID) in text,
                                  deadline=1200)
        except BaseException:
            if poller is not None:
                poller.stop()
            if cls.base is not None:
                try:
                    failure_logs = p.command(["docker", "compose"] + cls.base
                                             + ["logs", "--no-color", "world"],
                                             env=cls.env, timeout=60)
                    (cls.evidence / "world-failure.log").write_text(failure_logs, encoding="utf-8")
                except Exception:
                    pass
                p.force_down(cls.base, cls.env)
            raise
        # Let the post-turn-in state reach the periodic save, then snapshot.
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            if (_quest_row(cls.base, cls.env, cls.O) == "1:1:7:4"
                    and _quest_row(cls.base, cls.env, cls.C) == "1:1:7:4"):
                break
            time.sleep(5)
        if poller is not None:
            poller.stop()
        cls.snap_before_stop = _full_state(cls.base, cls.env, (cls.O, cls.C))
        (cls.evidence / "state_before_stop.txt").write_text(
            repr(cls.snap_before_stop), encoding="utf-8")
        # Clean stop (logout save), then the isolated world restart.
        # Compose logs accumulate across container generations of one
        # service, so scope the restart evidence to the tail after the
        # new generation's ready marker: the stale phase-1 tail already
        # contains the legitimate phase-1 login and turn-in lines.
        boot_marker = "World server is up and running!"
        boot_count = cls.logs.count(boot_marker)
        p.command(["docker", "compose"] + cls.base + ["stop", "world"], env=cls.env, timeout=180)
        p.command(["docker", "compose"] + cls.base + ["up", "-d", "--no-deps", "world"], env=cls.env)
        login_marker = "[PlayerBot][Login]  '%s' GUID:%d" % (cls.CNAME, cls.C)
        full = p.wait_for(cls.base, cls.env,
                          lambda text: (text.count(boot_marker) > boot_count
                                        and login_marker in
                                        text.rsplit(boot_marker, 1)[-1]),
                          deadline=480)
        cls.logs_restart = full.rsplit(boot_marker, 1)[-1]
        time.sleep(10)
        cls.snap_after_restart = _full_state(cls.base, cls.env, (cls.O, cls.C))
        (cls.evidence / "state_after_restart.txt").write_text(
            repr(cls.snap_after_restart), encoding="utf-8")
        (cls.evidence / "world.log").write_text(cls.logs, encoding="utf-8")
        (cls.evidence / "world-restart.log").write_text(cls.logs_restart, encoding="utf-8")

    @classmethod
    def tearDownClass(cls):
        if cls.base is not None:
            p.teardown_lab(cls.base, cls.env, cls.project, cls.evidence)

    def test_scripts_armed(self):
        self.assertIn("[PlayerBot][FollowScript] started events:", self.logs)
        self.assertIn("[PlayerBot][QuestScript] started events:", self.logs)

    def test_owner_accepted_via_authoritative_path(self):
        self.assertIn("[PlayerBot][QuestScript] accept issuer:%d quest:%s giver:%d ok:1"
                      % (self.O, QUEST_ID, GIVER_ENTRY), self.logs)

    def test_mirror_accepted_at_giver(self):
        # The mirror path has no hold gate (the declared policy does):
        # the accept follows the owner's accept directly, and the logged
        # anchor is the giver entry.
        i_owner = self.logs.find("[PlayerBot][QuestScript] accept issuer:%d" % self.O)
        i_accept = self.logs.find("[CoopQuest] mirror-accepted GUID:%d quest:%s anchor:%d"
                                  % (self.C, QUEST_ID, GIVER_ENTRY))
        self.assertNotEqual(i_owner, -1)
        self.assertNotEqual(i_accept, -1)
        self.assertLess(i_owner, i_accept)
        self.assertEqual(self.logs.count("[CoopQuest] mirror-accepted GUID:%d" % self.C), 1)

    def test_party_loss_suppressed_and_row_survived(self):
        i_accept = self.logs.find("[CoopQuest] mirror-accepted GUID:%d" % self.C)
        i_dismiss = self.logs.find("party dismiss accepted bot:%s guid:%d"
                                   % (self.CNAME, self.C))
        i_rejoin = self.logs.find("party recruit accepted bot:%s guid:%d leader:%d seq:3"
                                  % (self.CNAME, self.C, self.O))
        i_turnin = self.logs.find("[CoopQuest] mirror-turnin GUID:%d quest:%s"
                                  % (self.C, QUEST_ID))
        for label, idx in (("accept", i_accept), ("dismiss", i_dismiss),
                           ("rejoin", i_rejoin), ("turnin", i_turnin)):
            self.assertNotEqual(idx, -1, label + " marker missing")
        self.assertLess(i_accept, i_dismiss)
        self.assertLess(i_dismiss, i_rejoin)
        # The turn-in fired only after the re-join (party membership gate).
        self.assertLess(i_rejoin, i_turnin)
        # The persisted row survived the party loss (INCOMPLETE ->
        # complete -> rewarded, never erased): the timeline must contain
        # the companion INCOMPLETE before the turn-in sample.
        timeline = (self.evidence / "questtimeline.txt").read_text(encoding="utf-8")
        self.assertIn("%d=3:0:0:0" % self.C, timeline)
        self.assertIn("%d=1:1:7:4" % self.C, timeline)

    def test_kill_credit_on_both_logs(self):
        # The companion earned its personal 7/4 (tapped/participated
        # kills); the owner never attacked - its full 7/4 is pure
        # tap/group credit.
        snap = self.snap_before_stop
        self.assertEqual(snap[self.C]["quest"], "1:1:7:4")
        self.assertEqual(snap[self.O]["quest"], "1:1:7:4")

    def test_mirror_turnin_at_finisher(self):
        # The turn-in fired with the finisher entry as the anchor, not
        # the giver: at completion the companion stood at the pack,
        # 20+ yd from the giver, so the finisher lookup is what let it
        # reward (the PORT-033 split).
        m = re.search(r"\[CoopQuest\] mirror-turnin GUID:%d quest:%s anchor:(\d+) "
                      r"xpBefore:(\d+) xpAfter:(\d+)" % (self.C, QUEST_ID), self.logs)
        self.assertIsNotNone(m, "mirror turn-in marker with anchor missing")
        self.assertEqual(int(m.group(1)), self.FINISHER_ENTRY)
        self.assertGreater(int(m.group(3)), int(m.group(2)))
        self.assertNotIn("[CoopQuest] mirror-turnin GUID:%d quest:%s anchor:%d"
                         % (self.C, QUEST_ID, GIVER_ENTRY), self.logs)

    def test_companion_turnin_rewarded(self):
        snap = self.snap_before_stop
        self.assertTrue(snap[self.C]["state"][0] > 0)
        self.assertGreaterEqual(snap[self.C]["state"][1], 100000)
        self.assertEqual(snap[self.C]["inv"], 1, "companion choice item 5394")

    def test_owner_turnin_rewarded(self):
        m = re.search(r"\[PlayerBot\]\[QuestScript\] turnin issuer:%d quest:%s xpBefore:(\d+) xpAfter:(\d+)"
                      % (self.O, QUEST_ID), self.logs)
        self.assertIsNotNone(m, "owner turn-in marker with xp delta missing")
        self.assertGreater(int(m.group(2)), int(m.group(1)))
        self.assertTrue(self.snap_before_stop[self.O]["state"][0] > 0)
        self.assertEqual(self.snap_before_stop[self.O]["inv"], 1)

    def test_restart_persistence(self):
        self.assertIsNotNone(self.snap_before_stop)
        self.assertIsNotNone(self.snap_after_restart)
        for g in (self.O, self.C):
            self.assertEqual(self.snap_before_stop[g], self.snap_after_restart[g],
                             "state for guid %d changed across the restart" % g)
        # The companion re-logs in with the rewarded row while the owner
        # still holds (rewarded) 456: the cross-check plus the rewarded
        # flag suppress any re-accept/re-turn-in on the restart tail.
        for marker in ("[CoopQuest] mirror-accepted GUID:%d" % self.C,
                       "[CoopQuest] mirror-turnin GUID:%d" % self.C,
                       "[CoopQuest] accepted GUID:%d" % self.C,
                       "[CoopQuest] turnin GUID:%d" % self.C):
            self.assertNotIn(marker, self.logs_restart)

    def test_world_healthy(self):
        self.assertNotIn("[CRASH]", self.logs)
        self.assertNotIn("[CRASH]", self.logs_restart)

    def test_personal_state_untouched(self):
        self.assertEqual(self.before, f.BotFollowTests._personal_state())

    def test_mirror_marker_set_and_cleared(self):
        # KAP-558 review (finding 2): the mirror accept persists the
        # provenance row and the reward consumes it; the owner
        # (QuestScript-declared) never gets one.
        set_line = ("[CoopQuest] mirror-marker GUID:%d quest:%s set"
                    % (self.C, QUEST_ID))
        clear_line = ("[CoopQuest] mirror-marker GUID:%d quest:%s cleared"
                      % (self.C, QUEST_ID))
        self.assertIn(set_line, self.logs)
        self.assertIn(clear_line, self.logs)
        self.assertEqual(self.logs.count(set_line), 1)
        self.assertEqual(self.logs.count(clear_line), 1)
        self.assertNotIn("[CoopQuest] mirror-marker GUID:%d" % self.O, self.logs)
        out = p.db_exec(self.base, self.env,
                        "SELECT COUNT(*) FROM bot_mirror_quest")
        self.assertEqual(int(out.strip() or "0"), 0,
                         "the marker row must be consumed by the reward")


# ---------------------------------------------------------------------------
# Lab E: PORT-033 (KAP-558) - the mirror turn-in owner cross-check,
# negative case. The companion's log carries a seeded COMPLETE +
# unrewarded row for quest 457 that the owner does not hold, and the
# companion stands at a legitimate 457 finisher (6911, QUESTGIVER
# flagged, inside the interaction distance). Without the cross-check the
# turn-in loop would find this anchor and reward; with it, the row stays
# untouched for the whole quiet window.
# ---------------------------------------------------------------------------
class BotQuestCoopUnrelatedTests(unittest.TestCase):
    O = 610630
    C = 610631
    ONAME = "Ucowner"
    CNAME = "Uccomp"
    FINISHER_GUID = 2500095
    UNRELATED_QUEST = "457"
    FIXTURE_EXPECT = {
        6911: {"dmg_min": 0, "dmg_max": 0, "ranged_dmg_min": 0,
               "ranged_dmg_max": 0, "detection_range": 0},
    }

    base = env = project = evidence = None
    logs = ""
    before = None

    @classmethod
    def _seed(cls):
        return (
            "INSERT INTO tw_char.characters\n"
            " (guid,account,name,race,class,gender,level,money,position_x,position_y,position_z,map,\n"
            "  orientation,zone,health,power1,power2,power3,power4,power5,xp,xp_gain)\n"
            "VALUES\n"
            " (%d,1000%d,'%s',1,1,0,3,100000,-8947.95,-132.493,83.5312,0,\n"
            "  0,12,100,0,0,0,0,0,0,1),\n"
            " (%d,1000%d,'%s',1,1,0,3,100000,-8949.95,-200.493,83.5312,0,\n"
            "  0,12,100,0,0,0,0,0,0,1);\n"
            "INSERT INTO tw_char.playerbot (char_guid,chance,ai)\n"
            " VALUES (%d,100,'Default'),(%d,100,'Default');\n"
            "INSERT INTO tw_char.bot_ownership (char_guid,account_id,bot_type,provision_version,owner_account_id)\n"
            " VALUES (%d,1000%d,1,2,NULL),\n"
            "        (%d,1000%d,1,2,1000%d);\n"
            # PORT-033 fixture: 457's finisher becomes the pinned 6911
            # standing 2 yd from the owner, so the companion (following
            # the owner) ends up inside its interaction distance; the
            # owner holds no 457 row, so only the cross-check can keep
            # the seeded companion row from turning in.
            "DELETE FROM tw_world.creature_involvedrelation WHERE quest=457;\n"
            "INSERT INTO tw_world.creature_involvedrelation (id, quest) VALUES (6911, 457);\n"
            "UPDATE tw_world.creature_template SET npc_flags = npc_flags | 2 WHERE entry=6911;\n"
            # The base 6911 deals 28.6-31.9 melee plus 18.3-25.2 ranged
            # per ~2 s; standing 2 yd from the owner it killed both test
            # bots in Lab E runs. Zero all damage + detection; only the
            # QUESTGIVER flag and the 457 involvedrelation are
            # load-bearing here.
            "UPDATE tw_world.creature_template SET dmg_min=0, dmg_max=0, ranged_dmg_min=0, "
            "ranged_dmg_max=0, detection_range=0 WHERE entry=6911;\n"
            # The companion's log carries 457 COMPLETE + unrewarded (fork
            # enum: status 1) with zero objective progress: a row the
            # mirror path never accepted.
            "INSERT INTO tw_char.character_queststatus (guid, quest, status, rewarded)\n"
            " VALUES (%d, 457, 1, 0);\n"
            "INSERT INTO tw_world.creature\n"
            " (guid,id,map,position_x,position_y,position_z,orientation,spawntimesecsmin,\n"
            "  spawntimesecsmax,wander_distance,health_percent,mana_percent,movement_type,spawn_flags)\n"
            "VALUES\n"
            " (%d,6911,0,-8945.95,-132.493,83.5312,0,600,600,0,100,100,0,1);\n"
            "DELETE FROM tw_world.creature WHERE map=0 AND position_x BETWEEN -8990 AND -8910\n"
            " AND position_y BETWEEN -230 AND -100 AND guid NOT IN (%d);\n"
            % (cls.O, cls.O, cls.ONAME, cls.C, cls.C, cls.CNAME,
               cls.O, cls.C, cls.O, cls.O, cls.C, cls.C, cls.O,
               cls.C, cls.FINISHER_GUID, cls.FINISHER_GUID)
        )

    @classmethod
    def setUpClass(cls):
        p.IMAGE = os.environ.get("PORT023_LAB_IMAGE", "tortoise-local:dev")
        try:
            p.command(["docker", "image", "inspect", p.IMAGE])
        except RuntimeError:
            raise unittest.SkipTest("%s image not present; build it first" % p.IMAGE)
        cls.before = f.BotFollowTests._personal_state()
        cls.project = "tortoise-bot-questcoopu-" + uuid.uuid4().hex[:12]
        cls.evidence = p.ROOT / "local" / (cls.project + "-"
                                           + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ"))
        cls.evidence.mkdir(parents=True)
        world = p.world_env_for("")
        world.update(PLAYERBOT_ENABLE="1", PLAYERBOT_MIN_BOTS="0", PLAYERBOT_MAX_BOTS="0",
                     PLAYERBOT_REFRESH="600000", PLAYERBOT_UPDATE_MS="1000", PLAYERBOT_DEBUG="1",
                     PLAYERBOT_PROVISION="", PLAYERBOT_TEST_LOGIN="%d,%d" % (cls.O, cls.C),
                     PLAYERBOT_AMBIENT_ACQUIRE="0", PLAYERBOT_WANDER_RADIUS="1",
                     PLAYERBOT_QUEST_ID="0",
                     PLAYERBOT_COOPERATIVE_QUEST_ID="0",
                     PLAYERBOT_MIRROR_OWNER_QUESTS="1",
                     PLAYER_SAVE_INTERVAL="5000",
                     PLAYERBOT_FOLLOW_SCRIPT=";".join([
                         "0:%d:bothold %s" % (cls.O, cls.ONAME),
                         "4000:%d:bothold %s" % (cls.O, cls.CNAME),
                         "8000:%d:botrecruit %s" % (cls.O, cls.CNAME),
                         "12000:%d:botfollow %s" % (cls.O, cls.CNAME),
                         "60000:%d:bothold %s" % (cls.O, cls.CNAME)]))
        cls.base = cls.env = None
        try:
            cls.base, cls.env = p.boot_lab(cls.project, cls.evidence, world, cls._seed())
            p.command(["docker", "compose"] + cls.base + ["up", "-d", "--no-deps", "world"], env=cls.env)
            _assert_fixture_templates(cls.base, cls.env, cls.FIXTURE_EXPECT)
            cls.logs = p.wait_for(cls.base, cls.env,
                                  lambda text: "hold accepted bot:%s guid:%d"
                                               % (cls.CNAME, cls.C) in text,
                                  deadline=600)
            # Quiet observation window at the finisher (the owner never
            # holds the companion's seeded quest).
            time.sleep(90)
            cls.logs = p.command(["docker", "compose"] + cls.base
                                 + ["logs", "--no-color", "world"], env=cls.env, timeout=60)
            (cls.evidence / "world.log").write_text(cls.logs, encoding="utf-8")
        except BaseException:
            if cls.base is not None:
                try:
                    failure_logs = p.command(["docker", "compose"] + cls.base
                                             + ["logs", "--no-color", "world"],
                                             env=cls.env, timeout=60)
                    (cls.evidence / "world-failure.log").write_text(failure_logs, encoding="utf-8")
                except Exception:
                    pass
                p.force_down(cls.base, cls.env)
            raise

    @classmethod
    def tearDownClass(cls):
        if cls.base is not None:
            p.teardown_lab(cls.base, cls.env, cls.project, cls.evidence)

    def test_companion_recruited_and_arrived(self):
        self.assertIn("party recruit accepted bot:%s guid:%d" % (self.CNAME, self.C), self.logs)
        self.assertIn("[PlayerBot][Follow] active GUID:%d leader:%d" % (self.C, self.O), self.logs)

    def test_no_mirror_action(self):
        # No accept line, no turn-in attempt, no denial, no announce for
        # the quest: the cross-check skipped it before any other step.
        self.assertNotIn("[CoopQuest]", self.logs)
        self.assertNotIn("quest:%s" % self.UNRELATED_QUEST, self.logs)

    def test_unrelated_row_untouched(self):
        self.assertEqual(_quest_row(self.base, self.env, self.C, self.UNRELATED_QUEST),
                         "1:0:0:0")
        self.assertEqual(_quest_row(self.base, self.env, self.O, self.UNRELATED_QUEST), "-")

    def test_no_reward(self):
        self.assertEqual(_inv_reward(self.base, self.env, self.C), 0)

    def test_world_healthy(self):
        self.assertNotIn("[CRASH]", self.logs)

    def test_personal_state_untouched(self):
        self.assertEqual(self.before, f.BotFollowTests._personal_state())


# ---------------------------------------------------------------------------
# Lab F: KAP-558 review (finding 2) - persisted mirror provenance.
# Both characters hold a seeded 457 row COMPLETE + unrewarded, and the
# owner self-binds so its own mirror step runs too: the owner-holds
# cross-check passes for both bots, so only the persisted marker gate
# can keep the turn-in loop quiet. Phase 1 proves that (no turn-in, no
# reward, no marker row for the whole quiet window). Phase 2 inserts
# the marker row (the record MirrorOwnerQuestStep would have written on
# accept), restarts the world, and the companion turns in at the
# finisher while the owner's unmarked row stays untouched.
# ---------------------------------------------------------------------------
class BotQuestCoopMarkerTests(unittest.TestCase):
    O = 610641
    C = 610642
    ONAME = "Mkowner"
    CNAME = "Mkcomp"
    FINISHER_GUID = 2500096
    FINISHER_ENTRY = 6911
    MARKER_QUEST = "457"
    REWARD_ITEM_457 = 5405
    FIXTURE_EXPECT = {
        6911: {"dmg_min": 0, "dmg_max": 0, "ranged_dmg_min": 0,
               "ranged_dmg_max": 0, "detection_range": 0},
    }

    base = env = project = evidence = None
    row1_c = row1_o = None
    markers1 = inv1_c = None
    logs1 = logs2 = ""
    before = None

    @classmethod
    def _seed(cls):
        return (
            "INSERT INTO tw_char.characters\n"
            " (guid,account,name,race,class,gender,level,money,position_x,position_y,position_z,map,\n"
            "  orientation,zone,health,power1,power2,power3,power4,power5,xp,xp_gain)\n"
            "VALUES\n"
            " (%d,1000%d,'%s',1,1,0,3,100000,-8947.95,-132.493,83.5312,0,\n"
            "  0,12,100,0,0,0,0,0,0,1),\n"
            " (%d,1000%d,'%s',1,1,0,3,100000,-8949.95,-200.493,83.5312,0,\n"
            "  0,12,100,0,0,0,0,0,0,1);\n"
            "INSERT INTO tw_char.playerbot (char_guid,chance,ai)\n"
            " VALUES (%d,100,'Default'),(%d,100,'Default');\n"
            "INSERT INTO tw_char.bot_ownership (char_guid,account_id,bot_type,provision_version,owner_account_id)\n"
            # The owner self-binds (Lab D pattern): the owner's own
            # mirror step runs, so this lab also proves the marker gate
            # keeps an unmarked owner row from self-rewarding.
            " VALUES (%d,1000%d,1,2,1000%d),\n"
            "        (%d,1000%d,1,2,1000%d);\n"
            # PORT-033 fixture: 457's finisher is the pinned 6911 two
            # yd from the owner (owner orientation 0: +X is the side
            # the solo follower rests at), inside the companion's
            # interaction distance.
            "DELETE FROM tw_world.creature_involvedrelation WHERE quest=457;\n"
            "INSERT INTO tw_world.creature_involvedrelation (id, quest) VALUES (6911, 457);\n"
            "UPDATE tw_world.creature_template SET npc_flags = npc_flags | 2 WHERE entry=6911;\n"
            # The base 6911 is a hostile damage dealer; zero all damage
            # + detection and neutralize the faction. Only the
            # QUESTGIVER flag and the 457 involvedrelation are
            # load-bearing here.
            "UPDATE tw_world.creature_template SET faction=1, dmg_min=0, dmg_max=0, "
            "ranged_dmg_min=0, ranged_dmg_max=0, detection_range=0 WHERE entry=6911;\n"
            # Both logs carry 457 COMPLETE + unrewarded (fork enum:
            # status 1) with zero objective progress: rows the mirror
            # path never accepted.
            "INSERT INTO tw_char.character_queststatus (guid, quest, status, rewarded)\n"
            " VALUES (%d, 457, 1, 0), (%d, 457, 1, 0);\n"
            "INSERT INTO tw_world.creature\n"
            " (guid,id,map,position_x,position_y,position_z,orientation,spawntimesecsmin,\n"
            "  spawntimesecsmax,wander_distance,health_percent,mana_percent,movement_type,spawn_flags)\n"
            "VALUES\n"
            " (%d,6911,0,-8945.95,-132.493,83.5312,0,600,600,0,100,100,0,1);\n"
            "DELETE FROM tw_world.creature WHERE map=0 AND position_x BETWEEN -8990 AND -8910\n"
            " AND position_y BETWEEN -230 AND -100 AND guid NOT IN (%d);\n"
            % (cls.O, cls.O, cls.ONAME, cls.C, cls.C, cls.CNAME,
               cls.O, cls.C,
               cls.O, cls.O, cls.O, cls.C, cls.C, cls.O,
               cls.C, cls.O,
               cls.FINISHER_GUID, cls.FINISHER_GUID)
        )

    @classmethod
    def setUpClass(cls):
        p.IMAGE = os.environ.get("PORT023_LAB_IMAGE", "tortoise-local:dev")
        try:
            p.command(["docker", "image", "inspect", p.IMAGE])
        except RuntimeError:
            raise unittest.SkipTest("%s image not present; build it first" % p.IMAGE)
        cls.before = f.BotFollowTests._personal_state()
        cls.project = "tortoise-bot-questcoopm-" + uuid.uuid4().hex[:12]
        cls.evidence = p.ROOT / "local" / (cls.project + "-"
                                           + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ"))
        cls.evidence.mkdir(parents=True)
        world = p.world_env_for("")
        world.update(PLAYERBOT_ENABLE="1", PLAYERBOT_MIN_BOTS="0", PLAYERBOT_MAX_BOTS="0",
                     PLAYERBOT_REFRESH="600000", PLAYERBOT_UPDATE_MS="1000", PLAYERBOT_DEBUG="1",
                     PLAYERBOT_PROVISION="", PLAYERBOT_TEST_LOGIN="%d,%d" % (cls.O, cls.C),
                     PLAYERBOT_AMBIENT_ACQUIRE="0", PLAYERBOT_WANDER_RADIUS="1",
                     PLAYERBOT_QUEST_ID="0",
                     PLAYERBOT_COOPERATIVE_QUEST_ID="0",
                     PLAYERBOT_MIRROR_OWNER_QUESTS="1",
                     # The seeded rows are not mirror accepts; the
                     # one-shot login backfill must not mark them as
                     # legacy mirrors (that would defeat phase 1).
                     PLAYERBOT_MIRROR_MARKER_BACKFILL="0",
                     PLAYER_SAVE_INTERVAL="5000",
                     # The script re-arms on the phase-2 boot and
                     # re-forms the party (parties do not persist across
                     # logout); the follow brings the companion back
                     # inside the finisher's interaction distance.
                     PLAYERBOT_FOLLOW_SCRIPT=";".join([
                         "0:%d:bothold %s" % (cls.O, cls.ONAME),
                         "4000:%d:bothold %s" % (cls.O, cls.CNAME),
                         "8000:%d:botrecruit %s" % (cls.O, cls.CNAME),
                         "12000:%d:botfollow %s" % (cls.O, cls.CNAME),
                         "60000:%d:bothold %s" % (cls.O, cls.CNAME)]))
        cls.base = cls.env = None
        try:
            cls.base, cls.env = p.boot_lab(cls.project, cls.evidence, world, cls._seed())
            p.command(["docker", "compose"] + cls.base + ["up", "-d", "--no-deps", "world"], env=cls.env)
            _assert_fixture_templates(cls.base, cls.env, cls.FIXTURE_EXPECT)
            cls.logs1 = p.wait_for(cls.base, cls.env,
                                   lambda text: "hold accepted bot:%s guid:%d"
                                                % (cls.CNAME, cls.C) in text,
                                   deadline=600)
            # Phase 1 quiet window: both logs hold 457 COMPLETE +
            # unrewarded, the finisher is in range, no marker row.
            time.sleep(90)
            cls.logs1 = p.command(["docker", "compose"] + cls.base
                                  + ["logs", "--no-color", "world"], env=cls.env, timeout=60)
            (cls.evidence / "world-phase1.log").write_text(cls.logs1, encoding="utf-8")
            # Phase 1 DB snapshot: the test methods run after
            # phase 2 has rewarded the marked companion, so a
            # live query at assert time would read the
            # post-reward row; the phase 1 claim holds against
            # the captured pre-stop values.
            cls.row1_c = _quest_row(cls.base, cls.env, cls.C, cls.MARKER_QUEST)
            cls.row1_o = _quest_row(cls.base, cls.env, cls.O, cls.MARKER_QUEST)
            cls.markers1 = cls._marker_count()
            cls.inv1_c = cls._inv_reward_457(cls.C)
            # Phase 2: stop the world (clean stop = logout save), record
            # the provenance a mirror accept would have written while
            # the world is down (inserting before the stop let the
            # companion consume the marker on its next quest tick,
            # before the restart), then boot it again; the companion
            # turns in at the finisher.
            boot_marker = "World server is up and running!"
            boot_count = cls.logs1.count(boot_marker)
            p.command(["docker", "compose"] + cls.base + ["stop", "world"], env=cls.env, timeout=180)
            p.db_exec(cls.base, cls.env,
                      "INSERT INTO bot_mirror_quest (char_guid, quest_id, mirrored_at) "
                      "VALUES (%d, %s, 0)" % (cls.C, cls.MARKER_QUEST))
            p.command(["docker", "compose"] + cls.base + ["up", "-d", "--no-deps", "world"], env=cls.env)
            login_marker = "[PlayerBot][Login]  '%s' GUID:%d" % (cls.CNAME, cls.C)
            p.wait_for(cls.base, cls.env,
                       lambda text: (text.count(boot_marker) > boot_count
                                     and login_marker in
                                     text.rsplit(boot_marker, 1)[-1]),
                       deadline=480)
            cls.logs2 = p.wait_for(cls.base, cls.env,
                                   lambda text: ("[CoopQuest] mirror-turnin GUID:%d quest:%s"
                                                 % (cls.C, cls.MARKER_QUEST)
                                                 in text.rsplit(boot_marker, 1)[-1]),
                                   deadline=300)
            cls.logs2 = cls.logs2.rsplit(boot_marker, 1)[-1]
        except BaseException:
            if cls.base is not None:
                try:
                    failure_logs = p.command(["docker", "compose"] + cls.base
                                             + ["logs", "--no-color", "world"],
                                             env=cls.env, timeout=60)
                    (cls.evidence / "world-failure.log").write_text(failure_logs, encoding="utf-8")
                except Exception:
                    pass
                p.force_down(cls.base, cls.env)
            raise
        # Let the post-turn-in state reach the periodic save.
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            if _quest_row(cls.base, cls.env, cls.C, cls.MARKER_QUEST) == "1:1:0:0":
                break
            time.sleep(5)
        (cls.evidence / "world-phase2.log").write_text(cls.logs2, encoding="utf-8")

    @classmethod
    def tearDownClass(cls):
        if cls.base is not None:
            p.teardown_lab(cls.base, cls.env, cls.project, cls.evidence)

    # --- helpers ---------------------------------------------------------
    @classmethod
    def _marker_count(cls):
        out = p.db_exec(cls.base, cls.env,
                        "SELECT COUNT(*) FROM bot_mirror_quest")
        return int(out.strip() or "0")

    @classmethod
    def _inv_reward_457(cls, guid):
        out = p.db_exec(cls.base, cls.env,
                        "SELECT COALESCE(COUNT(*),0) FROM character_inventory "
                        "WHERE guid=%d AND item_template=%d"
                        % (guid, cls.REWARD_ITEM_457))
        return int(out.strip() or "0")

    # --- assertions ------------------------------------------------------
    def test_companion_recruited_and_arrived(self):
        self.assertIn("party recruit accepted bot:%s guid:%d" % (self.CNAME, self.C), self.logs1)
        self.assertIn("[PlayerBot][Follow] active GUID:%d leader:%d" % (self.C, self.O), self.logs1)

    def test_phase1_no_marker_no_turnin(self):
        # The owner-holds cross-check alone would have rewarded both
        # bots here; the persisted marker gate is what keeps phase 1
        # quiet. The throttled no-marker skip line is the positive
        # evidence the gate evaluated the seeded row.
        self.assertIn("[CoopQuest] mirror turnin skip GUID:%d quest:%s no-marker"
                      % (self.C, self.MARKER_QUEST), self.logs1)
        for guid in (self.O, self.C):
            self.assertNotIn("[CoopQuest] mirror-turnin GUID:%d" % guid, self.logs1)
            self.assertNotIn("[CoopQuest] mirror-marker GUID:%d" % guid, self.logs1)
        self.assertEqual(self.row1_c, "1:0:0:0",
                         "phase 1 snapshot: companion row untouched")
        self.assertEqual(self.row1_o, "1:0:0:0",
                         "phase 1 snapshot: owner row untouched")
        self.assertEqual(self.markers1, 0, "no marker row before phase 2")
        self.assertEqual(self.inv1_c, 0,
                         "phase 1 snapshot: no reward item granted")

    def test_phase2_marker_turnin(self):
        # With the marker row in place the companion turns in at the
        # finisher and the reward consumes the provenance row.
        self.assertIn("[CoopQuest] mirror-turnin GUID:%d quest:%s anchor:%d"
                      % (self.C, self.MARKER_QUEST, self.FINISHER_ENTRY), self.logs2)
        self.assertIn("[CoopQuest] mirror-marker GUID:%d quest:%s cleared"
                      % (self.C, self.MARKER_QUEST), self.logs2)
        self.assertEqual(_quest_row(self.base, self.env, self.C, self.MARKER_QUEST),
                         "1:1:0:0")
        xp = p.db_int(self.base, self.env,
                      "SELECT xp FROM characters WHERE guid=%d" % self.C)
        money = p.db_int(self.base, self.env,
                         "SELECT money FROM characters WHERE guid=%d" % self.C)
        self.assertEqual(xp, 250, "457 RewXP")
        self.assertEqual(money, 100050, "457 RewOrReqMoney 50c at Rate.Drop.Money=1")
        self.assertEqual(self._inv_reward_457(self.C), 1, "457 choice item 5405")
        self.assertEqual(self._marker_count(), 0, "marker consumed by the reward")

    def test_phase2_owner_row_untouched(self):
        # The owner's own 457 row is unmarked: the mirror gate leaves
        # it to its own (absent) handler.
        self.assertNotIn("[CoopQuest] mirror-turnin GUID:%d" % self.O, self.logs2)
        self.assertNotIn("[CoopQuest] mirror-marker GUID:%d" % self.O, self.logs2)
        self.assertEqual(_quest_row(self.base, self.env, self.O, self.MARKER_QUEST),
                         "1:0:0:0")
        money = p.db_int(self.base, self.env,
                         "SELECT money FROM characters WHERE guid=%d" % self.O)
        self.assertEqual(money, 100000)
        self.assertEqual(self._inv_reward_457(self.O), 0)

    def test_world_healthy(self):
        self.assertNotIn("[CRASH]", self.logs1)
        self.assertNotIn("[CRASH]", self.logs2)

    def test_personal_state_untouched(self):
        self.assertEqual(self.before, f.BotFollowTests._personal_state())


if __name__ == "__main__":
    unittest.main()
