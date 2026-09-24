"""PORT-035 (KAP-558): .botinit - one-shot companion setup + stale-group recovery.

One disposable, port-free lab, two phases. Five characters are seeded in
the emptied spawn box: the owner (500210, account 1000500210), three owned
companions (500211, 500212, 500213; bot_ownership.owner_account_id =
1000500210) and an intruder (500220, account 1000500220, no owned bots).

Phase 1: two companions (500211, 500212) are test-logged-in and therefore
online when the script fires; the third (500213) is deliberately left
offline so the full deferred path is exercised: BotRecall queues its
login, and the botInitLeaderGuid setup (defend + follow) lands in
OnPlayerInWorld when that login completes.

The lab-only PlayerBot.FollowScript replays, through the real chat path
(WorldSession::ProcessChatMessageAfterSecurityCheck -> command parse ->
SEC_PLAYER .botinit):

  t=+5s   owner: .botinit
          (2 online companions: party recruit accepted + defend enabled +
           follow accepted; 1 offline companion: party recall queued +
           botinit deferred -> after login: recruit + deferred setup)
  t=+45s  intruder: .botinit   (reports ready:0 deferred:0 skipped:0)

Phase 2 (stale-group recovery): after the world stops, the fixture
reconstructs a bot-only stale party from the phase-1 group snapshot. Normal
owner logout now disbands companion-only parties, so this is deliberately
synthetic legacy-state recovery, not a claim about current logout behavior.
The world is started again and the same follow script fires: the bots online
in the stale group must be made to
leave it (disbanding it when it is the last online member), and the
deferred bot must be recruited cleanly whether its stale membership was
still attached at login or already deleted by that disband.

Acceptance evidence:
  AC1 (phase 1): owner summary `ready:2 deferred:1 skipped:0`; per-bot
       recruit, defend and follow lines for the ready companions; the
       offline companion queued, then recruit + deferred setup after
       login; the intruder reports ready:0 and never acts.
  AC2 (phase 2): the two bots that were online in the stale group at
       recruit time leave it exactly once each (`bot left stale group`);
       the deferred bot is recruited with no `party recruit rejected
       already-grouped` anywhere (its stale membership is removed by the
       guard if the group still had online members when its recall login
       attached, or by the group disband deleting the remaining rows
       when the last online member left); the owner summary
       (ready:2 deferred:1) and the intruder summary (ready:0) each
       appear exactly twice (once per phase); the only group left after
       phase 2 is the owner's fresh party with exactly four members.
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

OWNER_SUMMARY = ("botinit complete issuer:%d acc:%d ready:2 deferred:1 skipped:0"
                 % (OWNER_GUID, OWNER_ACC))
INTRUDER_SUMMARY = ("botinit complete issuer:%d acc:%d ready:0 deferred:0 skipped:0"
                    % (INTRUDER_GUID, INTRUDER_ACC))

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
            # ---- phase 1: the four test-login bots must log in.
            cls.logs = p.wait_for(cls.base, cls.env, lambda text:
                                  all("test-login %d first=1 second=0" % g in text
                                      for g in (OWNER_GUID, FAST_A, FAST_B, INTRUDER_GUID)),
                                  deadline=420)
            # Owner .botinit at t=+5s: two ready, one deferred, none skipped.
            cls.logs = p.wait_for(cls.base, cls.env, lambda text:
                                  OWNER_SUMMARY in text, deadline=120)
            # The offline companion's recall-queued login must reach the
            # deferred setup (recruit + defend + follow after in-world).
            cls.logs = p.wait_for(cls.base, cls.env, lambda text:
                                  "botinit deferred setup bot:Initlate guid:%d leader:%d"
                                  % (LATE, OWNER_GUID) in text, deadline=120)
            # Intruder .botinit at t=+45s: owns nothing, changes nothing.
            cls.logs = p.wait_for(cls.base, cls.env, lambda text:
                                  INTRUDER_SUMMARY in text, deadline=120)
            # ---- phase 2: stop with the party persisted, rewrite the
            # group rows into the real owner-logout end state, restart.
            end = time.monotonic() + 30
            while time.monotonic() < end:
                time.sleep(3)
            # Capture the disposable phase-1 party before clean shutdown
            # disbands it, then reconstruct a bot-led stale roster below.
            group_row = p.db_exec(cls.base, cls.env,
                                  "SELECT * FROM tw_char.groups WHERE leaderGuid = %d"
                                  % OWNER_GUID).strip().splitlines()
            if len(group_row) != 1:
                raise AssertionError("expected one disposable phase-1 party")
            group_fields = [int(value) for value in group_row[0].split("\t")]
            if len(group_fields) != 16:
                raise AssertionError("unexpected group schema in disposable lab")
            group_fields[1] = FAST_A
            p.command(["docker", "compose"] + cls.base + ["stop", "world"], env=cls.env, timeout=180)
            stale_group = group_fields[0]
            p.db_exec(cls.base, cls.env,
                      "REPLACE INTO tw_char.groups VALUES (%s);"
                      " DELETE FROM tw_char.group_member WHERE groupId = %d;"
                      " INSERT INTO tw_char.group_member (groupId,memberGuid,assistant,subgroup)"
                      " VALUES (%d,%d,0,0),(%d,%d,0,0),(%d,%d,0,0);"
                      % (",".join(str(value) for value in group_fields), stale_group,
                         stale_group, FAST_A, stale_group, FAST_B, stale_group, LATE))
            p.command(["docker", "compose"] + cls.base + ["up", "-d", "--no-deps", "world"],
                      env=cls.env)
            # Phase-2 script: the owner summary and the intruder summary each
            # re-appear (count >= 2) and the deferred bot's recruit settles
            # after its recall login (count >= 2).
            cls.logs = p.wait_for(cls.base, cls.env, lambda text:
                                  text.count(OWNER_SUMMARY) >= 2 and
                                  text.count(INTRUDER_SUMMARY) >= 2 and
                                  text.count("party recruit accepted bot:Initlate guid:%d"
                                             % LATE) >= 2,
                                  deadline=300)
            # Quiet window so periodic saves settle, then a clean stop with
            # all players online.
            end = time.monotonic() + 60
            while time.monotonic() < end:
                time.sleep(3)
            # Inspect the live party before clean shutdown: owner logout
            # now correctly disbands companion-only groups on stop.
            cls.group_state = p.db_exec(cls.base, cls.env,
                                        "SELECT g.groupId, g.leaderGuid, "
                                        "COUNT(m.memberGuid) AS members "
                                        "FROM tw_char.groups g "
                                        "LEFT JOIN tw_char.group_member m "
                                        "ON m.groupId = g.groupId "
                                        "GROUP BY g.groupId, g.leaderGuid "
                                        "ORDER BY g.groupId")
            (cls.evidence / "groups.txt").write_text(cls.group_state, encoding="utf-8")
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
        # Once per phase: both ready companions online, the offline one
        # deferred, none skipped.
        self.assertEqual(self.logs.count(OWNER_SUMMARY), 2)

    def test_ready_companions_set_up_immediately(self):
        for guid, name in ((FAST_A, "Initfasta"), (FAST_B, "Initfastb")):
            # Phase 1 and phase 2 each recruit, defend, and follow.
            self.assertEqual(
                self.logs.count(
                    "party recruit accepted bot:%s guid:%d leader:%d seq:1" % (name, guid, OWNER_GUID)),
                2)
            self.assertEqual(
                self.logs.count(
                    "defend enabled bot:%s guid:%d issuer:%d acc:%d" % (name, guid, OWNER_GUID, OWNER_ACC)),
                2)
            self.assertEqual(
                self.logs.count(
                    "follow accepted bot:%s guid:%d leader:%d seq:1" % (name, guid, OWNER_GUID)),
                2)

    def test_deferred_companion_queued_then_set_up_after_login(self):
        queued = "party recall queued bot:Initlate guid:%d leader:%d seq:1" % (LATE, OWNER_GUID)
        deferred = "botinit deferred setup bot:Initlate guid:%d leader:%d" % (LATE, OWNER_GUID)
        self.assertEqual(self.logs.count(queued), 2)
        self.assertEqual(self.logs.count(deferred), 2)
        self.assertLess(self.logs.find(queued), self.logs.find(deferred),
                        "the recall queue must precede the deferred setup")
        # After each queued login, the pending recruit settles and the full
        # owner setup lands: recruit (same seq), defend, follow.
        self.assertEqual(
            self.logs.count(
                "party recruit accepted bot:Initlate guid:%d leader:%d seq:1" % (LATE, OWNER_GUID)),
            2)
        self.assertEqual(
            self.logs.count(
                "defend enabled bot:Initlate guid:%d issuer:%d acc:%d" % (LATE, OWNER_GUID, OWNER_ACC)),
            2)
        self.assertEqual(
            self.logs.count(
                "follow accepted bot:Initlate guid:%d leader:%d seq:1" % (LATE, OWNER_GUID)),
            2)

    def test_recruited_companions_have_live_group_links(self):
        # A roster row alone is insufficient: bot-first taps require the
        # registered bot Player to point at the same live group as the owner.
        for name in ("Initfasta", "Initfastb", "Initlate"):
            recruit_lines = [line for line in self.logs.splitlines()
                             if "party recruit accepted bot:%s " % name in line]
            self.assertEqual(len(recruit_lines), 2)
            for line in recruit_lines:
                self.assertIn(" live:1", line)

    def test_stale_group_abandoned_in_phase2(self):
        # The two bots that were online in the stale group at recruit time
        # must each leave it exactly once through the guard. The deferred
        # bot's stale membership is cleaned either way: through the guard
        # if the group still had online members when its recall login
        # attached, or by the group disband deleting the remaining rows
        # when the last online member left - so only its recruit is
        # asserted here.
        for guid, name in ((FAST_A, "Initfasta"), (FAST_B, "Initfastb")):
            self.assertEqual(
                self.logs.count("bot left stale group bot:%s guid:%d group:" % (name, guid)), 1,
                "an online stale-group bot must leave exactly once via the guard")
        self.assertNotIn("party recruit rejected already-grouped", self.logs,
                         "a stale group must be abandoned, never rejected")
        # End state: the stale persisted group (id 1) is gone; the only
        # group left is the owner's fresh party (id 2) led by the owner
        # with exactly four members.
        rows = [r.split("\t") for r in self.group_state.splitlines() if r.strip()]
        self.assertEqual(len(rows), 1, "only the owner's fresh party may remain")
        self.assertEqual(rows[0][0], "2", "the fresh party takes the next group id")
        self.assertEqual(rows[0][1], str(OWNER_GUID), "the owner leads the fresh party")
        self.assertEqual(rows[0][2], "4", "owner plus three companions")

    def test_intruder_botinit_owns_nothing(self):
        self.assertEqual(self.logs.count(INTRUDER_SUMMARY), 2)
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
