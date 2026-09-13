"""MVP-006: prove one declared supported quest through normal quest APIs.

Declared quest: 456 "The Balance of Nature" (MinLevel 1, QuestLevel 2,
Alliance race mask). Objectives: kill 7 Young Nightsaber (2031) and 4 Young
Thistle Boar (1984). Giver and finisher: Conservator Ilthalaine (2079).
Reward: 170 quest XP plus normal combat credit, all through the standard
quest accept / objective / turn-in path.

Fail-closed lab: same declared quest but no giver present; the bot may earn
combat XP but must not receive quest credit or rewards.
"""
import os
import re
import time
import unittest
import uuid
from datetime import datetime, timezone

import test_bot_provision as p

QUEST_ID = "456"
BOT_GUID = 500104
GIVER_GUID = 2500020
KILL_GUIDS = [2500021, 2500022, 2500023, 2500024, 2500025, 2500026, 2500027]
BOAR_GUIDS = [2500031, 2500032, 2500033, 2500034]

def _kill_rows():
    # Spread the pack along the y axis (4 yd spacing) so the bot faces a
    # few attackers at a time instead of the whole stacked group.
    rows = ["({g},2031,0,-8962.95,{y},83.5312,0,600,600,0,25,100,0,1)".format(g=g, y=-128.493 + 4.0 * i)
            for i, g in enumerate(KILL_GUIDS)]
    rows += ["({g},1984,0,-8962.95,{y},83.5312,0,600,600,0,25,100,0,1)".format(g=g, y=-156.493 + 4.0 * i)
             for i, g in enumerate(BOAR_GUIDS)]
    return ",".join(rows)


def _bot_seed():
    return """
INSERT INTO tw_char.characters
 (guid,account,name,race,class,gender,level,money,position_x,position_y,position_z,map,
  orientation,zone,health,power1,power2,power3,power4,power5,xp,xp_gain)
VALUES
 (500104,1000500104,'Questbot',1,1,0,3,100000,-8949.95,-132.493,83.5312,0,
  0,12,100,0,0,0,0,0,0,1);
INSERT INTO tw_char.playerbot (char_guid,chance,ai) VALUES (500104,100,'Default');
INSERT INTO tw_char.bot_ownership (char_guid,account_id,bot_type,provision_version)
 VALUES (500104,1000500104,1,2);
"""


def _quest_in_log(base, env):
    # Quest state persists in character_queststatus (this fork has no
    # characters.questlog). Fork QuestStatus enum: COMPLETE=1,
    # UNAVAILABLE=2, INCOMPLETE=3; the save writes m_status verbatim,
    # and a rewarded row persists with rewarded=1.
    out = p.db_exec(base, env,
                    "SELECT 1 FROM character_queststatus WHERE guid=%d AND quest=%s AND status IN (1,3)"
                    % (BOT_GUID, QUEST_ID))
    return out.strip() == "1"


def _quest_rewarded_saved(base, env):
    out = p.db_exec(base, env,
                    "SELECT 1 FROM character_queststatus WHERE guid=%d AND quest=%s AND rewarded=1"
                    % (BOT_GUID, QUEST_ID))
    return out.strip() == "1"


def _poll_quest_state(cls, seconds, label, stop_when=None):
    """Sample the quest row and bot XP every 5s into questtimeline.txt so
    periodic-save timing is reconstructable after the fact. Stops early
    when stop_when() returns true."""
    end = time.monotonic() + seconds
    with open(cls.evidence / "questtimeline.txt", "a", encoding="utf-8") as f:
        while time.monotonic() < end:
            in_log = "yes" if _quest_in_log(cls.base, cls.env) else "none"
            xp = p.db_int(cls.base, cls.env, "SELECT xp FROM characters WHERE guid=%d" % BOT_GUID)
            f.write("%s %s quest=%s xp=%d\n"
                    % (label, datetime.now(timezone.utc).strftime("%H:%M:%S"), in_log, xp))
            f.flush()
            if stop_when is not None and stop_when():
                break
            time.sleep(5)


def _run_lab(cls, with_giver, wait_marker, deadline):
    cls.base, cls.env = None, None
    cls.project = "tortoise-bot-quest-" + uuid.uuid4().hex[:12]
    cls.evidence = p.ROOT / "local" / (cls.project + "-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ"))
    cls.evidence.mkdir(parents=True)
    giver_row = ""
    if with_giver:
        giver_row = "({g},2079,0,-8945.95,-132.493,83.5312,0,600,600,0,100,100,0,1),".format(g=GIVER_GUID)
    seed = _bot_seed() + """
-- Lab-only fixture: the declared-quest creatures deal no damage so the L3
-- bot survives the full kill sequence; the quest flow is what is under test.
UPDATE tw_world.creature_template SET dmg_min=0, dmg_max=0, health_min=8, health_max=8 WHERE entry IN (2031,1984);
INSERT INTO tw_world.creature
 (guid,id,map,position_x,position_y,position_z,orientation,spawntimesecsmin,
  spawntimesecsmax,wander_distance,health_percent,mana_percent,movement_type,spawn_flags)
VALUES
""" + giver_row + _kill_rows() + ";"
    world = p.world_env_for("Questbot")
    world.update(PLAYERBOT_ENABLE="1", PLAYERBOT_MIN_BOTS="0", PLAYERBOT_MAX_BOTS="0",
                 PLAYERBOT_REFRESH="600000", PLAYERBOT_UPDATE_MS="1000", PLAYERBOT_DEBUG="1",
                 PLAYERBOT_PROVISION="", PLAYERBOT_TEST_LOGIN=str(BOT_GUID),
                 PLAYERBOT_QUEST_ID=QUEST_ID, PLAYER_SAVE_INTERVAL="5000")
    cls.base, cls.env = p.boot_lab(cls.project, cls.evidence, world, seed)
    cls.before_xp = p.db_int(cls.base, cls.env, "SELECT xp FROM characters WHERE guid=500104")
    p.command(["docker", "compose"] + cls.base + ["up", "-d", "--no-deps", "world"], env=cls.env)
    cls.logs = p.wait_for(cls.base, cls.env,
                          lambda text: "[PlayerBot][Login]  'Questbot' GUID:500104" in text,
                          deadline=600)
    try:
        cls.logs = p.wait_for(cls.base, cls.env, lambda text: wait_marker in text, deadline=deadline)
    except RuntimeError:
        _poll_quest_state(cls, 30, "timeout")
        cls.logs = p.command(["docker", "compose"] + cls.base + ["logs", "--no-color", "world"], env=cls.env, timeout=60)
        raise
    # Ensure the rewarded state has been saved before reading it back.
    _poll_quest_state(cls, 60, "post", stop_when=lambda: _quest_rewarded_saved(cls.base, cls.env))
    cls.logs = p.command(["docker", "compose"] + cls.base + ["logs", "--no-color", "world"], env=cls.env, timeout=60)
    cls.after_xp = p.db_int(cls.base, cls.env, "SELECT xp FROM characters WHERE guid=500104")
    (cls.evidence / "world.log").write_text(cls.logs, encoding="utf-8")
    (cls.evidence / "quest.txt").write_text(
        "bot=Questbot guid=500104 quest=%s giver=%s before=%d after=%d\n"
        % (QUEST_ID, GIVER_GUID if with_giver else "absent", cls.before_xp, cls.after_xp),
        encoding="utf-8")


class BotQuestFailClosedTests(unittest.TestCase):
    base = env = project = evidence = None
    logs = ""
    before_xp = after_xp = 0

    @classmethod
    def setUpClass(cls):
        p.IMAGE = os.environ.get("MVP006_LAB_IMAGE", "tortoise-local:dev")
        try:
            p.command(["docker", "image", "inspect", p.IMAGE])
        except RuntimeError:
            raise unittest.SkipTest("%s image not present; build it first" % p.IMAGE)
        try:
            # No giver in this lab: the declared quest can never be accepted,
            # so no quest credit may appear after the quiet observation window.
            _run_lab(cls, with_giver=False,
                     wait_marker="[PlayerBot] quest init GUID:500104 quest:456 phase:1",
                     deadline=120)
            _poll_quest_state(cls, 90, "quiet")
            cls.logs = p.command(["docker", "compose"] + cls.base + ["logs", "--no-color", "world"], env=cls.env, timeout=60)
        except BaseException:
            if cls.base is not None:
                try:
                    failure_logs = p.command(["docker", "compose"] + cls.base + ["logs", "--no-color", "world"], env=cls.env, timeout=60)
                    (cls.evidence / "world-failure.log").write_text(failure_logs, encoding="utf-8")
                except Exception:
                    pass
                p.force_down(cls.base, cls.env)
            raise

    @classmethod
    def tearDownClass(cls):
        if cls.base is not None:
            p.teardown_lab(cls.base, cls.env, cls.project, cls.evidence)

    def test_declared_quest_initialized(self):
        self.assertIn("[PlayerBot] quest init GUID:500104 quest:456 phase:1", self.logs)

    def test_no_quest_credit_without_giver(self):
        self.assertNotIn("quest accepted GUID:500104 quest:456", self.logs)
        self.assertNotIn("quest rewarded GUID:500104 quest:456", self.logs)
        self.assertFalse(_quest_in_log(self.base, self.env))

    def test_world_stayed_healthy(self):
        self.assertNotIn("[CRASH]", self.logs)


class BotQuestFlowTests(unittest.TestCase):
    base = env = project = evidence = None
    logs = ""
    before_xp = after_xp = 0

    @classmethod
    def setUpClass(cls):
        p.IMAGE = os.environ.get("MVP006_LAB_IMAGE", "tortoise-local:dev")
        try:
            p.command(["docker", "image", "inspect", p.IMAGE])
        except RuntimeError:
            raise unittest.SkipTest("%s image not present; build it first" % p.IMAGE)
        try:
            _run_lab(cls, with_giver=True,
                     wait_marker="quest rewarded GUID:500104 quest:456",
                     deadline=300)
        except BaseException:
            if cls.base is not None:
                try:
                    failure_logs = p.command(["docker", "compose"] + cls.base + ["logs", "--no-color", "world"], env=cls.env, timeout=60)
                    (cls.evidence / "world-failure.log").write_text(failure_logs, encoding="utf-8")
                except Exception:
                    pass
                p.force_down(cls.base, cls.env)
            raise

    @classmethod
    def tearDownClass(cls):
        if cls.base is not None:
            p.teardown_lab(cls.base, cls.env, cls.project, cls.evidence)

    def test_quest_accepted_via_giver(self):
        self.assertIn("quest accepted GUID:500104 quest:456", self.logs)

    def test_quest_objective_completed(self):
        self.assertIn("quest objective complete GUID:500104 quest:456", self.logs)

    def test_quest_rewarded_and_saved(self):
        self.assertIn("quest rewarded GUID:500104 quest:456", self.logs)
        # The rewarded log line carries the in-world XP credit
        # (xpBefore/xpAfter); the DB row must persist with
        # rewarded=1 and the XP delta must be saved.
        m = re.search(r"quest rewarded GUID:500104 quest:456 xpBefore:(\d+) xpAfter:(\d+)", self.logs)
        self.assertIsNotNone(m, "rewarded marker must carry xpBefore/xpAfter")
        self.assertGreater(int(m.group(2)), int(m.group(1)))
        self.assertTrue(_quest_rewarded_saved(self.base, self.env), "rewarded row must be saved")
        self.assertGreater(self.after_xp, self.before_xp)

    def test_world_stayed_healthy(self):
        self.assertNotIn("[CRASH]", self.logs)


if __name__ == "__main__":
    unittest.main()
