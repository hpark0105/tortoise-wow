"""PORT-035 (KAP-558): .botinit - one-shot companion setup.

One disposable, port-free lab. Five characters are seeded in the emptied
spawn box: the owner (500210, account 1000500210), three owned companions
(500211, 500212, 500213; bot_ownership.owner_account_id = 1000500210) and
an intruder (500220, account 1000500220, no owned bots).

Two companions (500211, 500212) are test-logged-in and therefore online
when the script fires; the third (500213) is deliberately left offline so
the full deferred path is exercised: BotRecall queues its login, and the
botInitLeaderGuid setup (defend + follow) lands in OnPlayerInWorld when
that login completes.

The lab-only PlayerBot.FollowScript replays, through the real chat path
(WorldSession::ProcessChatMessageAfterSecurityCheck -> command parse ->
SEC_PLAYER .botinit):

  t=+5s   owner: .botinit
          (2 online companions: party recruit accepted + defend enabled +
           follow accepted; 1 offline companion: party recall queued +
           botinit deferred -> after login: recruit + deferred setup)
  t=+45s  intruder: .botinit   (reports ready:0 deferred:0 skipped:0)

Acceptance evidence:
  AC1: owner summary `botinit complete issuer:500210 acc:1000500210
       ready:2 deferred:1 skipped:0` exactly once; per-bot recruit, defend
       and follow lines for the two ready companions.
  AC2: the offline companion queues (party recall queued), then after its
       login completes: recruit accepted with the same seq, defend
       enabled, follow accepted, and the `botinit deferred setup` line;
       the queued line precedes the deferred-setup line.
  AC3: the intruder's .botinit reports ready:0 deferred:0 skipped:0 and
       never recruits, defends, or follows anything.
"""
import os
import time
import unittest
import uuid
from datetime import datetime, timezone

import test_bot_provision as p

OWNER_GUID = 500210
FAST_A = 500211
FAST_B = 500212
LATE = 500213
INTRUDER_GUID = 500220
OWNER_ACC = 1000500210
INTRUDER_ACC = 1000500220
PERSONAL_CONTAINERS = ("tortoise-local-db-1", "tortoise-local-realmd-1",
                       "tortoise-local-world-1")

FOLLOW_SCRIPT = ";".join([
    "5000:%d:botinit" % OWNER_GUID,
    "45000:%d:botinit" % INTRUDER_GUID,
])


def _seed_sql():
    return """
INSERT INTO tw_char.characters
 (guid,account,name,race,class,gender,level,money,position_x,position_y,position_z,map,
  orientation,zone,health,power1,power2,power3,power4,power5,xp,xp_gain)
VALUES (500210,1000500210,'Initowner',1,1,0,10,100000,-8949.95,-120.493,83.5312,0,
 0,12,100,0,0,0,0,0,0,1),
 (500211,1000500211,'Initfasta',1,1,0,10,100000,-8949.95,-128.493,83.5312,0,
 0,12,100,0,0,0,0,0,0,1),
 (500212,1000500212,'Initfastb',1,1,0,10,100000,-8949.95,-136.493,83.5312,0,
 0,12,100,0,0,0,0,0,0,1),
 (500213,1000500213,'Initlate',1,1,0,10,100000,-8949.95,-144.493,83.5312,0,
 0,12,100,0,0,0,0,0,0,1),
 (500220,1000500220,'Intruderx',1,1,0,10,100000,-8962.95,-136.493,83.5312,0,
 0,12,100,0,0,0,0,0,0,1);
INSERT INTO tw_char.playerbot (char_guid,chance,ai)
 VALUES (500210,100,'Default'),(500211,100,'Default'),(500212,100,'Default'),
        (500213,100,'Default'),(500220,100,'Default');
INSERT INTO tw_char.bot_ownership (char_guid,account_id,bot_type,provision_version,owner_account_id)
 VALUES (500210,1000500210,1,2,NULL),
        (500211,1000500211,1,2,1000500210),
        (500212,1000500212,1,2,1000500210),
        (500213,1000500213,1,2,1000500210),
        (500220,1000500220,1,2,NULL);
-- Lab isolation: remove natural creatures in the spawn box so the five
-- bots idle deterministically (the box is empty for the whole run).
DELETE FROM tw_world.creature WHERE map = 0
  AND position_x BETWEEN -8990 AND -8910 AND position_y BETWEEN -170 AND -110;
"""


class BotInitTests(unittest.TestCase):
    base = env = project = evidence = None
    logs = ""
    ownership = ""
    personal_before = {}
    personal_after = {}

    @staticmethod
    def _personal_state():
        try:
            out = p.command(["docker", "inspect",
                             "--format", "{{.Name}}|{{.State.StartedAt}}|{{.State.Running}}",
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
        p.IMAGE = os.environ.get("TW014_LAB_IMAGE", "tortoise-local:dev")
        try:
            p.command(["docker", "image", "inspect", p.IMAGE])
        except RuntimeError:
            raise unittest.SkipTest("%s image not present; build it first" % p.IMAGE)
        cls.personal_before = cls._personal_state()
        world = p.world_env_for("Initowner")
        world.update(PLAYERBOT_MIN_BOTS="0", PLAYERBOT_MAX_BOTS="0",
                     PLAYERBOT_REFRESH="600000", PLAYERBOT_UPDATE_MS="1000",
                     PLAYERBOT_PROVISION="",
                     PLAYERBOT_TEST_LOGIN="%d,%d,%d,%d"
                     % (OWNER_GUID, FAST_A, FAST_B, INTRUDER_GUID),
                     PLAYERBOT_QUEST_ID="0",
                     PLAYERBOT_FOLLOW_SCRIPT=FOLLOW_SCRIPT,
                     PLAYER_SAVE_INTERVAL="5000")
        try:
            cls.project = "tortoise-bot-init-" + uuid.uuid4().hex[:12]
            stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
            cls.evidence = p.ROOT / "local" / (cls.project + "-" + stamp)
            cls.evidence.mkdir(parents=True)
            cls.base, cls.env = p.boot_lab(cls.project, cls.evidence, world, _seed_sql())
            p.command(["docker", "compose"] + cls.base + ["up", "-d", "--no-deps", "world"],
                      env=cls.env)
            # The four test-login bots must log in through the roster path.
            cls.logs = p.wait_for(cls.base, cls.env, lambda text:
                                  all("test-login %d first=1 second=0" % g in text
                                      for g in (OWNER_GUID, FAST_A, FAST_B, INTRUDER_GUID)),
                                  deadline=420)
            # Owner .botinit at t=+5s: two ready, one deferred, none skipped.
            cls.logs = p.wait_for(cls.base, cls.env, lambda text:
                                  "botinit complete issuer:%d acc:%d ready:2 deferred:1 skipped:0"
                                  % (OWNER_GUID, OWNER_ACC) in text, deadline=120)
            # The offline companion's recall-queued login must reach the
            # deferred setup (recruit + defend + follow after in-world).
            cls.logs = p.wait_for(cls.base, cls.env, lambda text:
                                  "botinit deferred setup bot:Initlate guid:%d leader:%d"
                                  % (LATE, OWNER_GUID) in text, deadline=120)
            # Intruder .botinit at t=+45s: owns nothing, changes nothing.
            cls.logs = p.wait_for(cls.base, cls.env, lambda text:
                                  "botinit complete issuer:%d acc:%d ready:0 deferred:0 skipped:0"
                                  % (INTRUDER_GUID, INTRUDER_ACC) in text, deadline=120)
            # Quiet window so periodic saves settle, then a clean stop with
            # all five players online.
            end = time.monotonic() + 60
            while time.monotonic() < end:
                time.sleep(3)
            p.command(["docker", "compose"] + cls.base + ["stop", "world"], env=cls.env, timeout=180)
            (cls.evidence / "world.log").write_text(
                p.command(["docker", "compose"] + cls.base + ["logs", "--no-color", "world"],
                          env=cls.env, timeout=60),
                encoding="utf-8")
            cls.ownership = p.db_exec(cls.base, cls.env,
                                      "SELECT char_guid, account_id, owner_account_id, "
                                      "provision_version FROM bot_ownership ORDER BY char_guid")
            (cls.evidence / "ownership.txt").write_text(cls.ownership, encoding="utf-8")
            p.teardown_lab(cls.base, cls.env, cls.project, cls.evidence)
            cls.base = None
        except BaseException:
            if cls.base is not None and cls.evidence is not None:
                try:
                    failure_logs = p.command(["docker", "compose"] + cls.base +
                                             ["logs", "--no-color", "world"],
                                             env=cls.env, timeout=60)
                    (cls.evidence / "world-failure.log").write_text(failure_logs, encoding="utf-8")
                except Exception:
                    pass
                p.force_down(cls.base, cls.env)
            raise
        cls.personal_after = cls._personal_state()

    def test_test_login_bots_logged_in(self):
        for g, name in ((OWNER_GUID, "Initowner"), (FAST_A, "Initfasta"),
                        (FAST_B, "Initfastb"), (INTRUDER_GUID, "Intruderx")):
            self.assertIn("test-login %d first=1 second=0" % g, self.logs)
            self.assertIn("[PlayerBot][Login]  '%s' GUID:%d" % (name, g), self.logs)
        self.assertNotIn("[CRASH]", self.logs)

    def test_offline_companion_logs_in_via_recall(self):
        self.assertIn("[PlayerBot][Login]  'Initlate' GUID:%d" % LATE, self.logs)

    def test_owner_botinit_summary_counts(self):
        line = ("botinit complete issuer:%d acc:%d ready:2 deferred:1 skipped:0"
                % (OWNER_GUID, OWNER_ACC))
        self.assertEqual(self.logs.count(line), 1)

    def test_ready_companions_set_up_immediately(self):
        for guid, name in ((FAST_A, "Initfasta"), (FAST_B, "Initfastb")):
            self.assertIn(
                "party recruit accepted bot:%s guid:%d leader:%d seq:1" % (name, guid, OWNER_GUID),
                self.logs)
            self.assertIn(
                "defend enabled bot:%s guid:%d issuer:%d acc:%d" % (name, guid, OWNER_GUID, OWNER_ACC),
                self.logs)
            self.assertIn(
                "follow accepted bot:%s guid:%d leader:%d seq:1" % (name, guid, OWNER_GUID),
                self.logs)

    def test_deferred_companion_queued_then_set_up_after_login(self):
        queued = "party recall queued bot:Initlate guid:%d leader:%d seq:1" % (LATE, OWNER_GUID)
        deferred = "botinit deferred setup bot:Initlate guid:%d leader:%d" % (LATE, OWNER_GUID)
        self.assertIn(queued, self.logs)
        self.assertIn(deferred, self.logs)
        self.assertLess(self.logs.find(queued), self.logs.find(deferred),
                        "the recall queue must precede the deferred setup")
        # After the queued login, the pending recruit settles and the full
        # owner setup lands: recruit (same seq), defend, follow.
        self.assertIn(
            "party recruit accepted bot:Initlate guid:%d leader:%d seq:1" % (LATE, OWNER_GUID),
            self.logs)
        self.assertIn(
            "defend enabled bot:Initlate guid:%d issuer:%d acc:%d" % (LATE, OWNER_GUID, OWNER_ACC),
            self.logs)
        self.assertIn(
            "follow accepted bot:Initlate guid:%d leader:%d seq:1" % (LATE, OWNER_GUID),
            self.logs)

    def test_intruder_botinit_owns_nothing(self):
        line = ("botinit complete issuer:%d acc:%d ready:0 deferred:0 skipped:0"
                % (INTRUDER_GUID, INTRUDER_ACC))
        self.assertEqual(self.logs.count(line), 1)
        self.assertNotIn("leader:%d seq:" % INTRUDER_GUID, self.logs)

    def test_ownership_rows_intact(self):
        self.assertIn("%d\t%d\tNULL\t2" % (OWNER_GUID, OWNER_ACC), self.ownership)
        for guid, acc in ((FAST_A, 1000500211), (FAST_B, 1000500212), (LATE, 1000500213)):
            self.assertIn("%d\t%d\t%d\t2" % (guid, acc, OWNER_ACC), self.ownership)
        self.assertIn("%d\t%d\tNULL\t2" % (INTRUDER_GUID, INTRUDER_ACC), self.ownership)

    def test_clean_shutdown_with_bots_online(self):
        world_log = (self.evidence / "world.log").read_text(encoding="utf-8")
        self.assertNotIn("Received SIGSEGV", world_log,
                         "clean stop with bots online crashed the world")
        self.assertNotIn("[CRASH]", world_log)

    def test_personal_volume_untouched(self):
        self.assertNotEqual(self.project, "tortoise-local")
        if self.personal_before:
            self.assertEqual(self.personal_after, self.personal_before,
                             "personal server containers must not be restarted or touched")


if __name__ == "__main__":
    unittest.main()
