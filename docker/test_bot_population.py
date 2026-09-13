"""TW-012: real roster capacity and exact population boundaries."""
import os
import time
import unittest
import uuid
from datetime import datetime, timezone

import test_bot_provision as p


def seed_roster(count):
    chars = []
    roster = []
    owners = []
    for i in range(count):
        guid = 501000 + i
        account = 1000501000 + i
        # Classic character names contain letters only.
        name = "Popbot" + chr(ord("a") + i)
        chars.append(
            "(%d,%d,'%s',1,1,0,3,100000,-8949.95,%s,83.5312,0,0,12,100,0,0,0,0,0,0,1)"
            % (guid, account, name, -132.493 + i * 2.0))
        roster.append("(%d,100,'Default')" % guid)
        owners.append("(%d,%d,1,2)" % (guid, account))
    return """
INSERT INTO tw_char.characters
 (guid,account,name,race,class,gender,level,money,position_x,position_y,position_z,map,
  orientation,zone,health,power1,power2,power3,power4,power5,xp,xp_gain)
VALUES %s;
INSERT INTO tw_char.playerbot (char_guid,chance,ai) VALUES %s;
INSERT INTO tw_char.bot_ownership (char_guid,account_id,bot_type,provision_version)
 VALUES %s;
DELETE FROM tw_world.creature WHERE map=0 AND position_x BETWEEN -8990 AND -8910
 AND position_y BETWEEN -170 AND -100;
""" % (",".join(chars), ",".join(roster), ",".join(owners))


def run_scenario(label, roster_count, minimum, maximum, expected):
    project = "tortoise-bot-pop-%s-%s" % (label, uuid.uuid4().hex[:8])
    evidence = p.ROOT / "local" / (
        project + "-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ"))
    evidence.mkdir(parents=True)
    world = p.world_env_for("")
    world.update(PLAYERBOT_ENABLE="1", PLAYERBOT_MIN_BOTS=str(minimum),
                 PLAYERBOT_MAX_BOTS=str(maximum), PLAYERBOT_REFRESH="15000",
                 PLAYERBOT_UPDATE_MS="500", PLAYERBOT_DEBUG="1",
                 PLAYERBOT_PROVISION="", PLAYERBOT_QUEST_ID="0")
    base = env = None
    try:
        base, env = p.boot_lab(project, evidence, world, seed_roster(roster_count))
        p.command(["docker", "compose"] + base + ["up", "-d", "--no-deps", "world"], env=env)
        marker = "[PlayerBotMgr] Between %d and %d bots online" % (expected, expected)
        logs = p.wait_for(base, env, lambda text: marker in text, deadline=180)
        if expected:
            expected_guids = [501000 + i for i in range(expected)]
            logs = p.wait_for(
                base, env,
                lambda text: all("[PlayerBot][Login]  'Popbot%s' GUID:%d" % (chr(ord("a") + i), guid) in text
                                 for i, guid in enumerate(expected_guids)),
                deadline=180)
        else:
            time.sleep(8)
        # Cross a reconciliation interval: an exact target must remain stable.
        # Refresh also defines the login timeout, so keep it long enough for
        # normal asynchronous character loading on a cold disposable world.
        time.sleep(20)
        logs = p.command(["docker", "compose"] + base + ["logs", "--no-color", "world"],
                         env=env, timeout=60)
        (evidence / "world.log").write_text(logs, encoding="utf-8")
        return logs
    finally:
        if base is not None:
            p.teardown_lab(base, env, project, evidence)


class BotPopulationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        p.IMAGE = os.environ.get("TW012_LAB_IMAGE", "tortoise-local:dev")
        try:
            p.command(["docker", "image", "inspect", p.IMAGE])
        except RuntimeError:
            raise unittest.SkipTest("%s image not present; build it first" % p.IMAGE)
        cls.zero = run_scenario("zero", 10, 0, 0, 0)
        cls.one = run_scenario("clamp", 1, 10, 10, 1)
        cls.ten = run_scenario("ten", 10, 10, 10, 10)

    def test_zero_target_stays_empty(self):
        self.assertNotIn("[PlayerBot][Login]", self.zero)

    def test_request_above_capacity_clamps_to_one(self):
        self.assertIn("[PlayerBotMgr] Between 1 and 1 bots online", self.one)
        self.assertEqual(self.one.count("[PlayerBot][Login]"), 1)
        self.assertNotIn("[PlayerBot][Logout]", self.one)

    def test_exact_ten_logs_each_bot_once_without_churn(self):
        self.assertIn("[PlayerBotMgr] Between 10 and 10 bots online", self.ten)
        self.assertEqual(self.ten.count("[PlayerBot][Login]"), 10)
        self.assertNotIn("[PlayerBot][Logout]", self.ten)

    def test_worlds_stayed_healthy(self):
        for logs in (self.zero, self.one, self.ten):
            self.assertIn("World server is up and running!", logs)
            self.assertNotIn("[CRASH]", logs)


if __name__ == "__main__":
    unittest.main()
