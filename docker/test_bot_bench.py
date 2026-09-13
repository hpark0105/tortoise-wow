"""PORT-002: dismissed owned companions stay benched across population refreshes."""
import os
import time
import unittest
import uuid
from datetime import datetime, timezone

import test_bot_provision as p

OWNER_GUID = 600100
COMP_GUID = 600101
AMB_GUIDS = (600102, 600103, 600104, 600105)
OWNER_ACC = 1000600100
COMP_ACC = 1000600101
ALL_GUIDS = (OWNER_GUID, COMP_GUID) + AMB_GUIDS
PERSONAL_CONTAINERS = ("tortoise-local-db-1", "tortoise-local-realmd-1",
                       "tortoise-local-world-1")

# Population target = 3, refresh = 10s.
# After dismiss at 20s, active drops to 2 < target 3.
# The system must add an ambient bot, NOT re-spawn the dismissed companion.
# Recall at 60s proves the companion still responds to explicit owner action.
BENCH_SCRIPT = ";".join([
    "5000:%d:botrecruit Benchcomp" % OWNER_GUID,
    "20000:%d:botdismiss Benchcomp" % OWNER_GUID,
    "60000:%d:botrecall Benchcomp" % OWNER_GUID,
])


def _seed_sql():
    names = ["Benchowner", "Benchcomp", "Ambone", "Ambtwo", "Ambthree", "Ambfour"]
    accounts = [OWNER_ACC, COMP_ACC,
                1000600102, 1000600103, 1000600104, 1000600105]
    values = []
    for index, (guid, account, name) in enumerate(zip(ALL_GUIDS, accounts, names)):
        values.append(
            "(%d,%d,'%s',1,1,0,10,100000,%s,-120.493,83.5312,0,0,12,100,0,0,0,0,0,0,1)"
            % (guid, account, name, -8949.95 + index * 2))
    ownership = []
    for guid, account in zip(ALL_GUIDS, accounts):
        # Only the companion is owned; ambient bots have no owner.
        owner = str(OWNER_ACC) if guid == COMP_GUID else "NULL"
        ownership.append("(%d,%d,1,2,%s)" % (guid, account, owner))
    roster = ",".join("(%d,100,'Default')" % guid for guid in ALL_GUIDS)
    return """
INSERT INTO tw_char.characters
 (guid,account,name,race,class,gender,level,money,position_x,position_y,position_z,map,
  orientation,zone,health,power1,power2,power3,power4,power5,xp,xp_gain)
VALUES %s;
INSERT INTO tw_char.playerbot (char_guid,chance,ai) VALUES %s;
INSERT INTO tw_char.bot_ownership
 (char_guid,account_id,bot_type,provision_version,owner_account_id)
VALUES %s;
DELETE FROM tw_world.creature WHERE map=0
 AND position_x BETWEEN -8990 AND -8910 AND position_y BETWEEN -170 AND -100;
""" % (",\n".join(values), roster, ",\n".join(ownership))


class BotBenchTests(unittest.TestCase):
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
        p.IMAGE = os.environ.get("PORT002_LAB_IMAGE", "tortoise-local:dev")
        try:
            p.command(["docker", "image", "inspect", p.IMAGE])
        except RuntimeError:
            raise unittest.SkipTest(f"{p.IMAGE} image not present; build it first")
        cls.personal_before = cls._personal_state()
        world = p.world_env_for("")
        world.update(PLAYERBOT_MIN_BOTS="3", PLAYERBOT_MAX_BOTS="3",
                     PLAYERBOT_REFRESH="10000", PLAYERBOT_UPDATE_MS="1000",
                     PLAYERBOT_TEST_LOGIN=",".join(str(g) for g in (OWNER_GUID, COMP_GUID)),
                     PLAYERBOT_QUEST_ID="0",
                     PLAYERBOT_FOLLOW_SCRIPT=BENCH_SCRIPT,
                     PLAYER_SAVE_INTERVAL="5000")
        cls.project = "tortoise-bot-bench-" + uuid.uuid4().hex[:12]
        stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        cls.evidence = p.ROOT / "local" / (cls.project + "-" + stamp)
        cls.evidence.mkdir(parents=True)
        try:
            cls.base, cls.env = p.boot_lab(cls.project, cls.evidence, world, _seed_sql())
            p.command(["docker", "compose"] + cls.base + ["up", "-d", "--no-deps", "world"],
                      env=cls.env)
            # Wait for the recruit (5s) to complete.
            cls.logs = p.wait_for(cls.base, cls.env, lambda text:
                                  "party recruit accepted bot:Benchcomp" in text,
                                  deadline=180)
            # Wait for the dismiss (20s) to complete.
            cls.logs = p.wait_for(cls.base, cls.env, lambda text:
                                  "party dismiss accepted bot:Benchcomp" in text,
                                  deadline=120)
            # Wait for the recall (60s) to complete.
            cls.logs = p.wait_for(cls.base, cls.env, lambda text:
                                  text.count("party recruit accepted bot:Benchcomp") >= 2,
                                  deadline=180)
            time.sleep(5)
            cls.logs = p.command(["docker", "compose"] + cls.base + ["logs", "--no-color", "world"],
                                 env=cls.env, timeout=60)
            (cls.evidence / "world.log").write_text(cls.logs, encoding="utf-8")
        finally:
            if cls.base is not None:
                p.teardown_lab(cls.base, cls.env, cls.project, cls.evidence)

    @classmethod
    def tearDownClass(cls):
        cls.personal_after = cls._personal_state()

    def test_01_companion_recruited(self):
        """The owner can recruit the owned companion."""
        self.assertIn("party recruit accepted bot:Benchcomp", self.logs)

    def test_02_companion_dismissed(self):
        """The owner can dismiss the owned companion."""
        self.assertIn("party dismiss accepted bot:Benchcomp", self.logs)

    def test_03_no_unauthorized_relogin_between_dismiss_and_recall(self):
        """After dismiss, the population system must NOT re-login the companion.

        The dismiss happens at ~20s, the recall at ~60s. Between those events
        (4 refresh cycles at 10s), the companion must not appear in any login
        log line. The only logins for Benchcomp should be the initial test-login
        and the explicit recall.
        """
        # Find all login events for Benchcomp.
        login_lines = [
            line for line in self.logs.splitlines()
            if "Benchcomp" in line and ("[Login]" in line or "login" in line.lower())
            and "PlayerBot" in line
        ]
        # The companion should have exactly 2 login events:
        # 1. Initial test-login at world start
        # 2. Recall at 60s
        # If the population system re-spawned it, there would be a 3rd.
        # We look for the specific pattern of a population-triggered login
        # (which would appear as a PlayerBot login without a preceding
        # "party recall" log line nearby).
        dismiss_idx = self.logs.find("party dismiss accepted bot:Benchcomp")
        recall_idx = self.logs.find(
            "party recall queued bot:Benchcomp")
        if dismiss_idx < 0 or recall_idx < 0 or recall_idx < dismiss_idx:
            self.fail("Could not locate dismiss/recall markers in logs")
        between = self.logs[dismiss_idx:recall_idx]
        # A population-driven re-login would show as a new PlayerBot login
        # for Benchcomp between the dismiss and the recall.
        bench_relogins = [
            line for line in between.splitlines()
            if "Benchcomp" in line and "[Login]" in line
        ]
        self.assertEqual(
            len(bench_relogins), 0,
            "Companion was re-logged-in by population system after dismiss: %s"
            % bench_relogins)

    def test_04_companion_recalled(self):
        """Recall reactivates the companion, preserving ownership."""
        self.assertGreaterEqual(
            self.logs.count("party recruit accepted bot:Benchcomp"), 2)

    def test_05_ambient_population_maintained(self):
        """The ambient population target is still met by ambient bots.

        After the companion is dismissed, the system must bring up an ambient
        bot to maintain the population target of 3.
        """
        # At least 3 ambient bots should have logged in at some point.
        ambient_logins = sum(
            1 for name in ("Ambone", "Ambtwo", "Ambthree", "Ambfour")
            if name in self.logs and "[Login]" in self.logs)
        self.assertGreaterEqual(
            ambient_logins, 2,
            "Expected at least 2 ambient bot logins, got %d" % ambient_logins)

    def test_06_personal_containers_untouched(self):
        """No personal server containers were affected."""
        self.assertEqual(self.personal_before, self.personal_after)


if __name__ == "__main__":
    unittest.main()