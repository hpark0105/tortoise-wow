"""MVP-007: prove exact earned state is preserved across a clean world restart.

One disposable lab run has the owned bot (500105) legitimately earn:
  * one item 117 from a deterministic corpse (loot template 990006), and
  * quest 456 "The Balance of Nature" (7x 2031 + 4x 1984, giver/finisher
    2079) through the normal accept / objective / turn-in path.

The world then performs a clean stop (logout save), all seeded creatures
and creatures around the spawn are removed so the restart observation
window is deterministic, and the world restarts with the declared quest
disabled (PLAYERBOT_QUEST_ID=0). After re-login the persisted state must
match the post-clean-save snapshot: identity, level, XP, money, the
looted item instance, ownership and roster exactly, position within 50
yards.
"""
import os
import time
import unittest
import uuid
from datetime import datetime, timezone

import test_bot_provision as p

BOT_GUID = 500105
BOT_NAME = "Restbot"
QUEST_ID = "456"
ITEM_ID = 117
LOOT_GUID = 2500061
GIVER_GUID = 2500040
KILL_GUIDS = [2500041, 2500042, 2500043, 2500044, 2500045, 2500046, 2500047]
BOAR_GUIDS = [2500051, 2500052, 2500053, 2500054]
SEED_GUIDS = [LOOT_GUID, GIVER_GUID] + KILL_GUIDS + BOAR_GUIDS

def _creature_rows():
    rows = ["({},6,0,-8945.95,-132.493,83.5312,0,600,600,0,25,100,0,1)".format(LOOT_GUID),
            "({},2079,0,-8945.95,-132.493,83.5312,0,600,600,0,100,100,0,1)".format(GIVER_GUID)]
    # Spread the pack along the y axis (4 yd spacing) so the bot faces a
    # few attackers at a time instead of the whole stacked group.
    rows += ["({},2031,0,-8962.95,{y},83.5312,0,600,600,0,25,100,0,1)".format(g, y=-128.493 + 4.0 * i)
             for i, g in enumerate(KILL_GUIDS)]
    rows += ["({},1984,0,-8962.95,{y},83.5312,0,600,600,0,25,100,0,1)".format(g, y=-156.493 + 4.0 * i)
             for i, g in enumerate(BOAR_GUIDS)]
    return ",".join(rows)


def _seed_sql():
    return """
INSERT INTO tw_char.characters
 (guid,account,name,race,class,gender,level,money,position_x,position_y,position_z,map,
  orientation,zone,health,power1,power2,power3,power4,power5,xp,xp_gain)
VALUES (500105,1000500105,'Restbot',1,1,0,3,100000,-8949.95,-132.493,83.5312,0,
 0,12,100,0,0,0,0,0,0,1);
INSERT INTO tw_char.playerbot (char_guid,chance,ai) VALUES (500105,100,'Default');
INSERT INTO tw_char.bot_ownership (char_guid,account_id,bot_type,provision_version)
 VALUES (500105,1000500105,1,2);
-- Lab-only fixture: the declared-quest creatures deal no damage and have
-- reduced health so the L3 bot survives and finishes the kill sequence;
-- the quest flow is what is under test.
UPDATE tw_world.creature_template SET dmg_min=0, dmg_max=0, health_min=8, health_max=8 WHERE entry IN (2031,1984);
UPDATE tw_world.creature_template SET loot_id=990006 WHERE entry=6;
DELETE FROM tw_world.creature_loot_template WHERE entry=990006;
INSERT INTO tw_world.creature_loot_template
 (entry,item,ChanceOrQuestChance,groupid,mincountOrRef,maxcount,condition_id)
 VALUES (990006,117,100,0,1,1,0);
INSERT INTO tw_world.creature
 (guid,id,map,position_x,position_y,position_z,orientation,spawntimesecsmin,
  spawntimesecsmax,wander_distance,health_percent,mana_percent,movement_type,spawn_flags)
VALUES
""" + _creature_rows() + ";"


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


def _state(base, env):
    out = p.db_exec(base, env,
                    "SELECT level, xp, money, map, zone, position_x, position_y, position_z "
                    "FROM characters WHERE guid = %d" % BOT_GUID).strip()
    if not out:
        return None
    lvl, xp, money, mapid, zone, x, y, z = out.split("\t")
    items = []
    for ig in p.db_exec(base, env,
                        "SELECT item_guid FROM character_inventory "
                        "WHERE owner_guid = %d AND item = %d ORDER BY item_guid"
                        % (BOT_GUID, ITEM_ID)).strip().splitlines():
        items.append(p.db_exec(base, env,
                               "SELECT entry, count FROM item_instance WHERE guid = %s" % ig).strip())
    return {
        "level": int(lvl), "xp": int(xp), "money": int(money),
        "map": int(mapid), "zone": int(zone),
        "pos": (float(x), float(y), float(z)),
        "items": items,
        "ownership": p.db_exec(base, env,
                               "SELECT account_id, provision_version FROM bot_ownership "
                               "WHERE char_guid = %d" % BOT_GUID).strip(),
        "roster": p.db_int(base, env,
                           "SELECT COUNT(*) FROM playerbot WHERE char_guid = %d" % BOT_GUID),
    }


class BotRestartTests(unittest.TestCase):
    base = env = project = evidence = None
    logs = logs_restart = ""
    s1 = s2 = None

    @classmethod
    def setUpClass(cls):
        p.IMAGE = os.environ.get("MVP007_LAB_IMAGE", "tortoise-local:dev")
        try:
            p.command(["docker", "image", "inspect", p.IMAGE])
        except RuntimeError:
            raise unittest.SkipTest("%s image not present; build it first" % p.IMAGE)
        cls.project = "tortoise-bot-restart-" + uuid.uuid4().hex[:12]
        cls.evidence = p.ROOT / "local" / (cls.project + "-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ"))
        cls.evidence.mkdir(parents=True)
        world = p.world_env_for(BOT_NAME)
        world.update(PLAYERBOT_MIN_BOTS="0", PLAYERBOT_MAX_BOTS="0",
                     PLAYERBOT_REFRESH="600000", PLAYERBOT_UPDATE_MS="1000",
                     PLAYERBOT_PROVISION="", PLAYERBOT_TEST_LOGIN=str(BOT_GUID),
                     PLAYERBOT_QUEST_ID=QUEST_ID, PLAYER_SAVE_INTERVAL="5000")
        try:
            cls.base, cls.env = p.boot_lab(cls.project, cls.evidence, world, _seed_sql())
            p.command(["docker", "compose"] + cls.base + ["up", "-d", "--no-deps", "world"], env=cls.env)
            cls.logs = p.wait_for(cls.base, cls.env,
                                  lambda text: "quest rewarded GUID:%d quest:%s" % (BOT_GUID, QUEST_ID) in text,
                                  deadline=420)
            # Let the rewarded state land in a periodic save before the
            # clean stop; the row persists afterwards with rewarded=1.
            end = time.monotonic() + 60
            while time.monotonic() < end and not _quest_rewarded_saved(cls.base, cls.env):
                time.sleep(3)
            # Clean stop performs the logout save; the DB state right after
            # the stop is the pre-restart snapshot.
            p.command(["docker", "compose"] + cls.base + ["stop", "world"], env=cls.env, timeout=180)
            cls.s1 = _state(cls.base, cls.env)
            self_assert = _quest_in_log(cls.base, cls.env)
            (cls.evidence / "questlog_after_stop.txt").write_text(
                "quest_in_log_after_stop=%s\n" % ("yes" if self_assert else "no"), encoding="utf-8")
            # Deterministic observation window: remove the seeded creatures
            # and everything around the spawn so the bot cannot earn or
            # lose anything across the restart.
            p.db_exec(cls.base, cls.env,
                      "DELETE FROM tw_world.creature WHERE guid IN (%s)"
                      % ",".join(str(g) for g in SEED_GUIDS), database="tw_world")
            p.db_exec(cls.base, cls.env,
                      "DELETE FROM tw_world.creature WHERE map = 0 "
                      "AND position_x BETWEEN -8990 AND -8910 AND position_y BETWEEN -170 AND -110",
                      database="tw_world")
            world_restart = dict(world)
            world_restart["PLAYERBOT_QUEST_ID"] = "0"
            cls.logs_restart = p.start_world_again(cls.base, cls.env, cls.evidence, world_restart,
                                                   "world-restart",
                                                   "[PlayerBot][Login]  '%s' GUID:%d" % (BOT_NAME, BOT_GUID))
            cls.s2 = _state(cls.base, cls.env)
            (cls.evidence / "restart.txt").write_text(
                "bot=%s guid=%d quest=%s item=%d\ns1=%r\ns2=%r\n"
                % (BOT_NAME, BOT_GUID, QUEST_ID, ITEM_ID, cls.s1, cls.s2), encoding="utf-8")
        except BaseException:
            if cls.base is not None:
                try:
                    failure_logs = p.command(["docker", "compose"] + cls.base + ["logs", "--no-color", "world"],
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

    def test_earned_loot_and_quest_markers(self):
        self.assertIn("corpse loot stored GUID:%d item:%d before:0 after:1" % (BOT_GUID, ITEM_ID), self.logs)
        self.assertIn("quest accepted GUID:%d quest:%s" % (BOT_GUID, QUEST_ID), self.logs)
        self.assertIn("quest objective complete GUID:%d quest:%s" % (BOT_GUID, QUEST_ID), self.logs)
        self.assertIn("quest rewarded GUID:%d quest:%s" % (BOT_GUID, QUEST_ID), self.logs)

    def test_relogin_after_restart(self):
        self.assertIn("[PlayerBot][Login]  '%s' GUID:%d" % (BOT_NAME, BOT_GUID), self.logs_restart)

    def test_state_matches_across_restart(self):
        self.assertIsNotNone(self.s1)
        self.assertIsNotNone(self.s2)
        for key in ("level", "xp", "money", "map", "zone", "items", "ownership", "roster"):
            self.assertEqual(self.s2[key], self.s1[key], "%s changed across restart" % key)
        x1, y1, z1 = self.s1["pos"]
        x2, y2, z2 = self.s2["pos"]
        dist = ((x1 - x2) ** 2 + (y1 - y2) ** 2 + (z1 - z2) ** 2) ** 0.5
        self.assertLessEqual(dist, 50.0)

    def test_world_stayed_healthy(self):
        self.assertNotIn("[CRASH]", self.logs)
        self.assertNotIn("[CRASH]", self.logs_restart)


if __name__ == "__main__":
    unittest.main()
