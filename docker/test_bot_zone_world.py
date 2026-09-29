"""Disposable mixed-faction citizen placement smoke test.

The socketless anchor is admitted only by the lab-only test-anchor setting.
No personal character or world volume is touched.
"""
import os
import re
import math
import time
import unittest
import uuid
from datetime import datetime, timezone

import test_bot_provision as p


ANCHOR = 630100
NAMES = ("Zonebota", "Zonebotb", "Zonebotc", "Zonebotd", "Morgath")
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
                         "Zonebotc,1,5,0;Zonebotd,1,4,1"),
                     PLAYERBOT_ZONE_HORDE_PROVISION_COUNT="1",
                     PLAYERBOT_ZONE_PROVISION_LEVEL="10",
                     PLAYERBOT_ZONE_WORLD_TEST_ANCHOR_GUID=str(ANCHOR),
                     PLAYERBOT_ZONE_WORLD_TARGET="5",
                     PLAYERBOT_ZONE_WORLD_ZONE_TARGETS="0:12:4;0:85:1",
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
                "('Zonebota','Zonebotb','Zonebotc','Zonebotd','Morgath') AND online=1")
            cls.citizens = p.db_exec(
                cls.base, cls.env,
                "SELECT c.name,c.guid,c.race,c.class,b.owner_account_id,p.ai,c.level FROM tw_char.characters c "
                "JOIN tw_char.bot_ownership b ON b.char_guid=c.guid "
                "JOIN tw_char.playerbot p ON p.char_guid=c.guid WHERE c.name IN "
                "('Zonebota','Zonebotb','Zonebotc','Zonebotd','Morgath') ORDER BY c.name")
            cls.appearances = p.db_exec(
                cls.base, cls.env,
                "SELECT c.name,c.playerBytes,c.playerBytes2,m.skin_id,m.face_id,"
                "m.hair_style_id,m.hair_color_id,m.facial_hair_id FROM tw_char.characters c "
                "JOIN tw_char.bot_provision_state m ON m.char_guid=c.guid "
                "WHERE c.name IN ('Zonebota','Zonebotb','Zonebotc','Zonebotd','Morgath') ORDER BY c.name")
            cls.gear = p.db_exec(
                cls.base, cls.env,
                "SELECT c.name,COUNT(i.item) AS equipped,COALESCE(MAX(t.quality),0),"
                "COALESCE(MAX(t.required_level),0) FROM tw_char.characters c "
                "LEFT JOIN tw_char.character_inventory i ON i.guid=c.guid AND i.bag=0 AND i.slot<19 "
                "LEFT JOIN tw_char.item_instance s ON s.guid=i.item "
                "LEFT JOIN tw_world.item_template t ON t.entry=s.itemEntry "
                "WHERE c.name IN ('Zonebota','Zonebotb','Zonebotc','Zonebotd','Morgath') "
                "GROUP BY c.name ORDER BY c.name")
            (cls.evidence / "citizens.tsv").write_text(cls.citizens, encoding="utf-8")
            (cls.evidence / "appearances.tsv").write_text(cls.appearances, encoding="utf-8")
            (cls.evidence / "gear.tsv").write_text(cls.gear, encoding="utf-8")
            cls.horde = p.db_int(cls.base, cls.env,
                                 "SELECT COUNT(*) FROM tw_char.characters WHERE name='Morgath'")
            cls.logs = p.wait_for(
                cls.base, cls.env,
                lambda text: "[ZoneCitizen] recruited bot:Zonebota" in text,
                deadline=180)
            citizen_guid = int(next(row.split('\t')[1] for row in cls.citizens.splitlines()
                                    if row.split('\t')[0] == "Zonebota"))
            cls.logs = p.wait_for(
                cls.base, cls.env,
                lambda text: "[PlayerBot][Follow] active GUID:%d leader:%d" %
                (citizen_guid, ANCHOR) in text,
                deadline=30)
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
                try:
                    logs = p.command(["docker", "compose"] + cls.base +
                                     ["logs", "--no-color", "world"],
                                     env=cls.env, timeout=60)
                    (cls.evidence / "world-failure.log").write_text(logs, encoding="utf-8")
                except BaseException:
                    pass
                p.force_down(cls.base, cls.env)
            raise

    def test_generated_horde_and_alliance_citizens_reach_configured_zones(self):
        citizens = [line.split('\t') for line in self.citizens.splitlines()]
        self.assertEqual(self.online, 5)
        self.assertEqual(self.horde, 1)
        self.assertEqual(self.recruited_groups, 1)
        alliance_leader_follower = next(int(row[1]) for row in citizens if row[0] == "Zonebota")
        self.assertIn("[PlayerBot][Follow] active GUID:%d leader:%d" %
                      (alliance_leader_follower, ANCHOR), self.logs)
        self.assertEqual(self.groups, 0)
        self.assertEqual(self.still_online, 1)
        self.assertEqual(len(citizens), 5)
        for row in citizens:
            if row[0] == "Morgath":
                self.assertEqual(row[2], "5")
                self.assertIn(row[3], {"1", "4", "5", "8", "9"})
                self.assertEqual(row[4:], ["NULL", "ZoneCitizenAI", "10"])
            else:
                self.assertEqual(row[2], "1")
                self.assertEqual(row[4:], ["NULL", "ZoneCitizenAI", "10"])
        placed = PLACED.findall(self.logs)
        self.assertEqual({int(row[0]) for row in placed}, {int(row[1]) for row in citizens})
        names_by_guid = {int(row[1]): row[0] for row in citizens}
        expected_zones = {"Morgath": (0, 85)}
        for guid, map_id, zone, x, y, _z in placed:
            name = names_by_guid[int(guid)]
            expected_map, expected_zone = expected_zones.get(name, (0, 12))
            self.assertEqual((int(map_id), int(zone)), (expected_map, expected_zone), name)
            self.assertTrue(all(math.isfinite(float(value)) for value in (x, y, _z)), name)

    def test_appearance_and_gear_are_varied_but_modest(self):
        appearances = [row.split('\t') for row in self.appearances.splitlines()]
        self.assertEqual(len(appearances), 5)
        self.assertGreater(len({tuple(row[3:]) for row in appearances}), 1)
        for name, player_bytes, player_bytes2, skin, face, hair, color, facial in appearances:
            packed = int(skin) | int(face) << 8 | int(hair) << 16 | int(color) << 24
            self.assertEqual(int(player_bytes), packed, name)
            self.assertEqual(int(player_bytes2) & 255, int(facial), name)
            if name == "Morgath":
                self.assertLessEqual(int(skin), 5, name)
                self.assertLessEqual(int(face), 9, name)
                self.assertLessEqual(int(hair), 16, name)
                self.assertLessEqual(int(color), 10, name)
                self.assertLessEqual(int(facial), 16, name)
        gear = [row.split('\t') for row in self.gear.splitlines()]
        self.assertEqual(len(gear), 5)
        self.assertTrue(all(int(row[1]) > 0 for row in gear))
        # Citizen gear rolls can choose poor, uncommon, or rare loot by design;
        # they must remain below the ambient best-kit path and level-appropriate.
        self.assertTrue(all(int(row[2]) <= 3 and int(row[3]) <= 8 for row in gear),
                        repr(gear))


if __name__ == "__main__":
    unittest.main()
