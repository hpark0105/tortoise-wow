"""TW-BOTS-002: repeated unavailable escapes defer in a disposable world.

A synthetic citizen starts a normal hunt against one slow-hitting hostile. Another 32
hostiles outside aggro range overflow the bounded escape scan. This tests
the real adapter's unavailable -> retry -> defer path and continued
self-defense. It does not prove safe path selection or successful escape.
Only the uniquely named, port-free lab database receives fixture changes.
The attacker uses the same Snufflesnout corridor as the existing defend
fixture, with bounded damage and enough health to keep the encounter open.
"""
import math
import os
import re
import time
import unittest
import uuid
from datetime import datetime, timezone

import test_bot_provision as p
import test_bot_follow as f


GUID = 640901
RETRY = re.compile(
    rf"retreat-retry guid:{GUID} reason:crowded failures:(\d+) "
    r"retry-ms:(\d+) deferred:(\d+)")


def seed_sql():
    spawns = []
    for index in range(33):
        angle = index * 2 * math.pi / 32
        x = -8949.95 + (0 if index == 0 else 40 * math.cos(angle))
        y = -130.493 + (10 if index == 0 else 40 * math.sin(angle))
        spawns.append(
            f"({2500900 + index},51600,0,{x},{y},86.0,0,600,600,0,100,100,0,1)")
    return f"""
INSERT INTO tw_char.characters
 (guid,account,name,race,class,gender,level,money,position_x,position_y,
  position_z,map,orientation,zone,health,power1,power2,power3,power4,power5,xp,xp_gain)
VALUES ({GUID},1000640901,'Retrycitizen',1,1,0,10,0,-8949.95,-130.493,
        86.0,0,0,12,231,0,0,0,0,0,0,1);
INSERT INTO tw_char.playerbot (char_guid,chance,ai)
 VALUES ({GUID},100,'ZoneCitizenAI');
INSERT INTO tw_char.bot_ownership
 (char_guid,account_id,bot_type,provision_version,owner_account_id)
 VALUES ({GUID},1000640901,1,2,NULL);
DELETE FROM tw_world.creature WHERE map=0
 AND position_x BETWEEN -9100 AND -8800 AND position_y BETWEEN -285 AND 20;
UPDATE tw_world.creature_template SET faction=14, level_min=10, level_max=10,
 health_min=100000, health_max=100000, regeneration=0,
 dmg_min=10, dmg_max=10, attack_power=0, dmg_multiplier=1,
 base_attack_time=6000, ranged_dmg_min=0, ranged_dmg_max=0,
 detection_range=20, call_for_help_range=0, flags_extra=flags_extra & ~2 WHERE entry=51600;
INSERT INTO tw_world.creature
 (guid,id,map,position_x,position_y,position_z,orientation,spawntimesecsmin,
  spawntimesecsmax,wander_distance,health_percent,mana_percent,movement_type,spawn_flags)
VALUES {','.join(spawns)};
"""


class CitizenRecoveryWorldTest(unittest.TestCase):
    def test_unavailable_escape_escalates_and_keeps_self_defense(self):
        p.IMAGE = os.environ.get("CITIZEN_RECOVERY_LAB_IMAGE", "tortoise-local:dev")
        try:
            image_id = p.command(["docker", "image", "inspect", "--format", "{{.Id}}", p.IMAGE])
        except (OSError, RuntimeError):
            self.skipTest("Build the candidate image and provide validated local game data first")
        before = f.BotFollowTests._personal_state()
        project = "tortoise-citizen-recovery-" + uuid.uuid4().hex[:12]
        stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        evidence = p.ROOT / "local" / (project + "-" + stamp)
        evidence.mkdir(parents=True)
        (evidence / "image-id.txt").write_text(image_id, encoding="utf-8")
        world = p.world_env_for("")
        world.update(PLAYERBOT_MIN_BOTS="0", PLAYERBOT_MAX_BOTS="0",
                     PLAYERBOT_REFRESH="600000", PLAYERBOT_UPDATE_MS="1000",
                     PLAYERBOT_TEST_LOGIN=str(GUID), PLAYERBOT_AMBIENT_ACQUIRE="1",
                     PLAYERBOT_WORLD_INTENT_ENABLE="0", PLAYER_SAVE_INTERVAL="5000")
        base = env = None
        try:
            base, env = p.boot_lab(project, evidence, world, seed_sql())
            p.command(["docker", "compose"] + base + ["up", "-d", "--no-deps", "world"], env=env)
            p.wait_for(base, env, lambda text: f"[PlayerBot][Login]  'Retrycitizen' GUID:{GUID}" in text,
                       deadline=180)
            p.wait_for(base, env, lambda text: f"retreat-unavailable guid:{GUID}" in text,
                       deadline=240)
            logs = p.wait_for(base, env, lambda text: ("3", "30000", "1") in RETRY.findall(text),
                              deadline=180)
            events = RETRY.findall(logs)
            self.assertEqual(events[:3], [("1", "5000", "0"), ("2", "5000", "0"),
                                         ("3", "30000", "1")])
            # With a 3s polling interval this samples well inside the 30s defer.
            time.sleep(10)
            during = p.command(["docker", "compose"] + base + ["logs", "--no-color", "world"], env=env)
            self.assertEqual(len(RETRY.findall(during)), 3)
            tail = during[during.index(f"retreat-retry guid:{GUID} reason:crowded failures:3"):]
            self.assertIn(f"[PlayerBot] fighting GUID:{GUID}", tail)
            logs = p.wait_for(base, env, lambda text: ("4", "30000", "1") in RETRY.findall(text),
                              deadline=60)
            self.assertNotIn(f"retreat-start guid:{GUID}", logs)
            self.assertNotIn(f"retreat-result guid:{GUID}", logs)
            self.assertNotIn(f"bot dead GUID:{GUID}", logs)
            self.assertNotIn("[CRASH]", logs)
        finally:
            if base is not None:
                try:
                    logs = p.command(["docker", "compose"] + base + ["logs", "--no-color", "world"], env=env)
                    (evidence / "world.log").write_text(logs, encoding="utf-8")
                finally:
                    p.teardown_lab(base, env, project, evidence)
        self.assertEqual(before, f.BotFollowTests._personal_state())


if __name__ == "__main__":
    unittest.main()
