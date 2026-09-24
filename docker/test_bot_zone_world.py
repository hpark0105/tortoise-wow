"""Disposable four-citizen same-zone placement smoke test.

The socketless anchor is admitted only by the lab-only test-anchor setting.
No personal character or world volume is touched.
"""
import math
import os
import re
import time
import unittest
import uuid
from datetime import datetime, timezone

import test_bot_provision as p


ANCHOR = 630100
NAMES = ("Zonebota", "Zonebotb", "Zonebotc", "Zonebotd")
ANCHOR_X, ANCHOR_Y = -9466.37, 21.4192
PLACED = re.compile(
    r"\[ZoneCitizen\] placed guid:(\d+) map:(\d+) zone:(\d+) "
    r"pos:([-\d.]+)/([-\d.]+)/([-\d.]+)")


def seed_sql():
    rows = [(ANCHOR, 1000630100, "Zoneanchor", 1, 1, ANCHOR_X, ANCHOR_Y)]
    chars = ",".join(
        "(%d,%d,'%s',%d,%d,0,10,100000,%f,%f,56.3401,0,0,12,100,0,0,0,0,0,0,1)"
        % row for row in rows)
    bindings = ",".join("(%d,%d,1,2)" % (guid, account)
                        for guid, account, *_ in rows)
    return ("INSERT INTO tw_char.characters "
            "(guid,account,name,race,class,gender,level,money,position_x,position_y,position_z,map,"
            "orientation,zone,health,power1,power2,power3,power4,power5,xp,xp_gain) VALUES %s;"
            " INSERT INTO tw_char.bot_ownership "
            "(char_guid,account_id,bot_type,provision_version) VALUES %s;"
            % (chars, bindings))


class ZoneWorldTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        p.IMAGE = os.environ.get("ZONE_WORLD_LAB_IMAGE", "tortoise-local:dev")
        p.command(["docker", "image", "inspect", p.IMAGE])
        cls.project = "tortoise-zone-world-" + uuid.uuid4().hex[:12]
        stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        cls.evidence = p.ROOT / "local" / (cls.project + "-" + stamp)
        cls.evidence.mkdir(parents=True)
        cls.base = cls.env = None
        world = p.world_env_for("Zoneanchor")
        world.update(PLAYERBOT_MIN_BOTS="0", PLAYERBOT_MAX_BOTS="0",
                     PLAYERBOT_PROVISION="", PLAYERBOT_TEST_LOGIN=str(ANCHOR),
                     PLAYERBOT_ZONE_PROVISION=(
                         "Zonebota,1,1,0;Zonebotb,1,8,1;"
                         "Zonebotc,1,5,0;Zonebotd,1,4,1;"
                         "Zonehorde,2,1,0"),
                     PLAYERBOT_ZONE_PROVISION_LEVEL="10",
                     PLAYERBOT_ZONE_WORLD_TEST_ANCHOR_GUID=str(ANCHOR),
                     PLAYERBOT_ZONE_WORLD_TARGET="4",
                     PLAYERBOT_ZONE_WORLD_PACE_MS="1000",
                     PLAYERBOT_ZONE_WORLD_RADIUS_YD="250",
                     PLAYERBOT_PARTY_INVITE_SCRIPT=f"15000:{ANCHOR}:Zonebota",
                     PLAYERBOT_FOLLOW_SCRIPT=f"28000:{ANCHOR}:botdismiss Zonebota",
                     PLAYERBOT_AMBIENT_ACQUIRE="0", PLAYERBOT_DEBUG="1",
                     PLAYERBOT_UPDATE_MS="1000", PLAYERBOT_REFRESH="600000")
        try:
            cls.base, cls.env = p.boot_lab(cls.project, cls.evidence, world,
                                           seed_sql())
            p.command(["docker", "compose"] + cls.base +
                      ["up", "-d", "--no-deps", "world"], env=cls.env)
            cls.logs = p.wait_for(
                cls.base, cls.env,
                lambda text: len(PLACED.findall(text)) >= len(NAMES),
                deadline=420)
            cls.logs = p.wait_for(
                cls.base, cls.env,
                lambda text: all("[PlayerBot][Login]  '%s'" % name in text
                                 for name in NAMES),
                deadline=180)
            time.sleep(5)
            cls.logs = p.command(["docker", "compose"] + cls.base +
                                 ["logs", "--no-color", "world"],
                                 env=cls.env, timeout=60)
            cls.online = p.db_int(
                cls.base, cls.env,
                "SELECT COUNT(*) FROM tw_char.characters WHERE name IN "
                "('Zonebota','Zonebotb','Zonebotc','Zonebotd') AND online=1")
            cls.citizens = p.db_exec(
                cls.base, cls.env,
                "SELECT c.guid,c.race,b.owner_account_id,p.ai,c.level FROM tw_char.characters c "
                "JOIN tw_char.bot_ownership b ON b.char_guid=c.guid "
                "JOIN tw_char.playerbot p ON p.char_guid=c.guid WHERE c.name IN "
                "('Zonebota','Zonebotb','Zonebotc','Zonebotd') ORDER BY c.name")
            cls.horde = p.db_int(cls.base, cls.env,
                                 "SELECT COUNT(*) FROM tw_char.characters WHERE name='Zonehorde'")
            cls.logs = p.wait_for(
                cls.base, cls.env,
                lambda text: "[ZoneCitizen] recruited bot:Zonebota" in text,
                deadline=180)
            citizen_guid = int(cls.citizens.splitlines()[0].split('\t')[0])
            cls.logs = p.wait_for(
                cls.base, cls.env,
                lambda text: "[PlayerBot][Follow] path GUID:%d leader:%d" %
                (citizen_guid, ANCHOR) in text,
                deadline=15)
            cls.recruited_groups = p.db_int(
                cls.base, cls.env,
                "SELECT COUNT(*) FROM tw_char.group_member gm JOIN tw_char.characters c "
                "ON c.guid=gm.memberGuid WHERE c.name='Zonebota'")
            cls.logs = p.wait_for(
                cls.base, cls.env,
                lambda text: "party dismiss accepted bot:Zonebota" in text,
                deadline=180)
            cls.groups = p.db_int(cls.base, cls.env,
                                  "SELECT COUNT(*) FROM tw_char.groups")
            cls.still_online = p.db_int(
                cls.base, cls.env,
                "SELECT COUNT(*) FROM tw_char.characters WHERE name='Zonebota' AND online=1")
            (cls.evidence / "world.log").write_text(cls.logs, encoding="utf-8")
            p.teardown_lab(cls.base, cls.env, cls.project, cls.evidence)
            cls.base = None
        except BaseException:
            if cls.base is not None:
                p.force_down(cls.base, cls.env)
            raise

    def test_four_alliance_citizens_placed_in_anchors_zone(self):
        citizens = [line.split('\t') for line in self.citizens.splitlines()]
        self.assertEqual(self.online, 4)
        self.assertEqual(self.horde, 0)
        self.assertEqual(self.recruited_groups, 1)
        self.assertIn("[PlayerBot][Follow] path GUID:%d leader:%d" %
                      (int(citizens[0][0]), ANCHOR), self.logs)
        self.assertEqual(self.groups, 0)
        self.assertEqual(self.still_online, 1)
        self.assertEqual(len(citizens), 4)
        self.assertTrue(all(row[1:] == ['1', 'NULL', 'ZoneCitizenAI', '10']
                            for row in citizens))
        placed = PLACED.findall(self.logs)
        self.assertEqual({int(row[0]) for row in placed},
                         {int(row[0]) for row in citizens})
        for guid, map_id, zone, x, y, _z in placed:
            self.assertEqual((int(map_id), int(zone)), (0, 12), guid)
            distance = math.hypot(float(x) - ANCHOR_X, float(y) - ANCHOR_Y)
            self.assertGreaterEqual(distance, 79.0, guid)
            self.assertLessEqual(distance, 251.0, guid)


if __name__ == "__main__":
    unittest.main()
