"""A dead citizen reclaims its corpse before cross-map safety relocation.

Runs in a uniquely named disposable world/database project. The fixture
starts a level-1 Night Elf ghost with a Teldrassil corpse but a stale saved
Tirisfal map, matching the production failure signature. It verifies that
login returns to the corpse first, the normal reclaim runs, and only then
does the allocator queue the safety relocation.
"""
import os
import re
import unittest
import uuid
from datetime import datetime, timezone

import test_bot_provision as p


ANCHOR = 640950
CITIZEN = 640951
ANCHOR_NAME = "Corpseanchor"
CITIZEN_NAME = "Corpsebot"
CORPSE_GUID = 840951
CORPSE_X, CORPSE_Y, CORPSE_Z = 10124.7, 1347.1, 1322.1
RECOVERY_FIRST = re.compile(
    rf"\[ZoneCitizen\]\[Recovery\] corpse-first-login queued guid:{CITIZEN} "
    r"map:1 zone:141 saved-map:0 saved-zone:85 relocation-after-reclaim:1")


def seed_sql():
    return f"""
INSERT INTO tw_char.characters
 (guid,account,name,race,class,gender,level,xp,money,playerFlags,
  position_x,position_y,position_z,map,orientation,online,zone,health,power1)
VALUES
 ({ANCHOR},1000640950,'{ANCHOR_NAME}',1,1,0,10,0,100000,0,
  -9466.37,21.4192,56.3401,0,0,0,12,100,0),
 ({CITIZEN},1000640951,'{CITIZEN_NAME}',4,1,0,1,0,0,16,
  10124.7,1347.1,1322.1,0,0,0,85,1,0);
INSERT INTO tw_char.playerbot (char_guid,chance,ai) VALUES
 ({ANCHOR},100,'PlayerBotAI'),({CITIZEN},100,'ZoneCitizenAI');
INSERT INTO tw_char.bot_ownership
 (char_guid,account_id,bot_type,provision_version,owner_account_id) VALUES
 ({ANCHOR},1000640950,1,2,NULL),({CITIZEN},1000640951,1,2,NULL);
INSERT INTO tw_char.corpse
 (guid,player,position_x,position_y,position_z,orientation,map,time,corpse_type,instance)
VALUES ({CORPSE_GUID},{CITIZEN},{CORPSE_X},{CORPSE_Y},{CORPSE_Z},0,1,
        UNIX_TIMESTAMP()-600,1,0);
INSERT INTO tw_char.bot_citizen_danger_memory
 (char_guid,map_id,zone_id,cell_x,cell_y,deaths,safe_until,updated_at)
VALUES ({CITIZEN},1,141,FLOOR({CORPSE_X}/40),FLOOR({CORPSE_Y}/40),3,
        UNIX_TIMESTAMP()+900,UNIX_TIMESTAMP());
"""


class ZoneCitizenCorpseRecoveryTest(unittest.TestCase):
    def test_reclaims_before_safety_relocation(self):
        p.IMAGE = os.environ.get("ZONE_CORPSE_LAB_IMAGE", "tortoise-local:dev")
        try:
            p.command(["docker", "image", "inspect", p.IMAGE])
        except RuntimeError:
            self.skipTest(f"{p.IMAGE} image not present; build it first")

        project = "tortoise-zone-corpse-" + uuid.uuid4().hex[:12]
        stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        evidence = p.ROOT / "local" / (project + "-" + stamp)
        evidence.mkdir(parents=True)
        world = p.world_env_for(ANCHOR_NAME)
        world.update(
            PLAYERBOT_MIN_BOTS="0", PLAYERBOT_MAX_BOTS="0",
            PLAYERBOT_PROVISION="", PLAYERBOT_TEST_LOGIN=str(ANCHOR),
            PLAYERBOT_ZONE_WORLD_TEST_ANCHOR_GUID=str(ANCHOR),
            PLAYERBOT_ZONE_WORLD_TARGET="1", PLAYERBOT_ZONE_WORLD_PACE_MS="1000",
            PLAYERBOT_ZONE_WORLD_LOGIN_BATCH="1", PLAYERBOT_ZONE_WORLD_RADIUS_YD="250",
            PLAYERBOT_ZONE_WORLD_ZONE_TARGETS="", PLAYERBOT_AMBIENT_ACQUIRE="0",
            PLAYERBOT_DEBUG="1", PLAYERBOT_UPDATE_MS="1000", PLAYERBOT_REFRESH="600000")

        base = env = None
        try:
            base, env = p.boot_lab(project, evidence, world, seed_sql())
            p.command(["docker", "compose"] + base + ["up", "-d", "--no-deps", "world"], env=env)
            logs = p.wait_for(
                base, env,
                lambda text: bool(RECOVERY_FIRST.search(text)),
                deadline=240)
            logs = p.wait_for(
                base, env,
                lambda text: f"[ZoneCitizen][Survival] leaving-danger guid:{CITIZEN}" in text,
                deadline=90)
            logs = p.wait_for(
                base, env,
                lambda text: re.search(
                    rf"\[ZoneCitizen\] login queued guid:{CITIZEN} .*relocation:1", text),
                deadline=180)
            corpse_count = p.db_int(
                base, env,
                f"SELECT COUNT(*) FROM tw_char.corpse WHERE player={CITIZEN} AND corpse_type<>0")
            self.assertEqual(corpse_count, 0, "normal reclaim must consume the corpse")
            recovery_index = logs.index(RECOVERY_FIRST.search(logs).group(0))
            alive_index = logs.index(
                f"[ZoneCitizen][Survival] leaving-danger guid:{CITIZEN}")
            relocation_index = logs.index(
                f"[ZoneCitizen] login queued guid:{CITIZEN}", alive_index)
            self.assertLess(recovery_index, alive_index)
            self.assertRegex(logs[relocation_index:],
                             rf"\[ZoneCitizen\] login queued guid:{CITIZEN} .*relocation:1")
            (evidence / "world.log").write_text(logs, encoding="utf-8")
        except BaseException:
            if base is not None:
                try:
                    logs = p.command(["docker", "compose"] + base + ["logs", "--no-color", "world"],
                                     env=env, timeout=60)
                    (evidence / "world-failure.log").write_text(logs, encoding="utf-8")
                except BaseException:
                    pass
            raise
        finally:
            if base is not None:
                p.teardown_lab(base, env, project, evidence)


if __name__ == "__main__":
    unittest.main()
