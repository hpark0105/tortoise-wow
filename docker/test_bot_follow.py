"""TW-014 (KAP-557): deterministic owner-only follow/stop for one owned companion.

One disposable, port-free lab. Three roster bots are seeded in an emptied
spawn box: the owner (500110, its own reserved account 1000500110), the
companion (500111, its own reserved account 1000500111 with the human-owner
binding bot_ownership.owner_account_id = 1000500110, seeded 32 yd from the
owner) and an intruder (500120, account 1000500120). The companion keeps its
own reserved session identity (contract C2: one session per account), so it
can log in while the owner is online; ownership is expressed by the
owner_account_id binding added by the TW-014 migration.

The lab-only PlayerBot.FollowScript replays, through the real chat path
(WorldSession::ProcessChatMessageAfterSecurityCheck -> command parse ->
SEC_PLAYER .botfollow/.botstop), a deterministic sequence:

  t=+5s   owner: .botfollow Followcomp   (goal seq 1; 32 yd walk)
  t=+60s  owner: .botstop Followcomp     (goal seq 1 invalidated)
  t=+70s  intruder: .botfollow ...       (rejected: not owner)
  t=+80s  intruder: .botstop ...         (rejected: not owner)
  t=+90s  owner: .botfollow Followcomp   (goal seq 2; companion close)
  t=+125s owner: .botstop Followcomp     (goal seq 2 invalidated)
  t=+135s stale delivery: expired goal (leader 500110, seq 2)
          (must be rejected by the seq guard; must not resume following)
  t=+145s owner: .botfollow Followcomp   (goal seq 3)
  t=+155s owner: .botstop Followcomp     (goal seq 3 invalidated)

Acceptance evidence:
  AC1: follow accepted seq:1, [Follow] active seq:1, [Follow] reached with
       dist <= 2.0 (from a 32 yd seed), stop accepted seq:1, [Follow]
       inactive; DB samples during the first follow window show the
       companion within 12 yd of the owner (both saved on the same world
       save tick).
  AC2: intruder commands rejected (not-owner, acc 1000500120, owner
       1000500110); the stale seq-2 delivery after the seq-2 stop is
       rejected and produces no second [Follow] active seq:2.
"""
import os
import re
import time
import unittest
import uuid
from datetime import datetime, timezone

import test_bot_provision as p

OWNER_GUID = 500110
COMP_GUID = 500111
INTRUDER_GUID = 500120
OWNER_ACC = 1000500110
COMP_ACC = 1000500111
INTRUDER_ACC = 1000500120
PERSONAL_CONTAINERS = ("tortoise-local-db-1", "tortoise-local-realmd-1",
                       "tortoise-local-world-1")

FOLLOW_SCRIPT = ";".join([
    "5000:%d:botfollow Followcomp" % OWNER_GUID,
    "60000:%d:botstop Followcomp" % OWNER_GUID,
    "70000:%d:botfollow Followcomp" % INTRUDER_GUID,
    "80000:%d:botstop Followcomp" % INTRUDER_GUID,
    "90000:%d:botfollow Followcomp" % OWNER_GUID,
    "125000:%d:botstop Followcomp" % OWNER_GUID,
    "135000:stale:Followcomp:%d:2" % OWNER_GUID,
    "145000:%d:botfollow Followcomp" % OWNER_GUID,
    "155000:%d:botstop Followcomp" % OWNER_GUID,
])

REACHED = re.compile(
    r"\[PlayerBot\]\[Follow\] reached GUID:(\d+) leader:(\d+) dist:([\d.]+) seq:(\d+)")


def _seed_sql():
    return """
INSERT INTO tw_char.characters
 (guid,account,name,race,class,gender,level,money,position_x,position_y,position_z,map,
  orientation,zone,health,power1,power2,power3,power4,power5,xp,xp_gain)
VALUES (500110,1000500110,'Followowner',1,1,0,10,100000,-8949.95,-120.493,83.5312,0,
 0,12,100,0,0,0,0,0,0,1),
 (500111,1000500111,'Followcomp',1,1,0,10,100000,-8949.95,-152.493,83.5312,0,
 0,12,100,0,0,0,0,0,0,1),
 (500120,1000500120,'Intruder',1,1,0,10,100000,-8962.95,-136.493,83.5312,0,
 0,12,100,0,0,0,0,0,0,1);
INSERT INTO tw_char.playerbot (char_guid,chance,ai)
 VALUES (500110,100,'Default'),(500111,100,'Default'),(500120,100,'Default');
INSERT INTO tw_char.bot_ownership (char_guid,account_id,bot_type,provision_version,owner_account_id)
 VALUES (500110,1000500110,1,2,NULL),
        (500111,1000500111,1,2,1000500110),
        (500120,1000500120,1,2,NULL);
-- Lab isolation: remove natural creatures in the spawn box so the three
-- bots idle deterministically (the box is empty for the whole run).
DELETE FROM tw_world.creature WHERE map = 0
  AND position_x BETWEEN -8990 AND -8910 AND position_y BETWEEN -170 AND -110;
"""


def _dist(base, env):
    out = p.db_exec(base, env,
                    "SELECT a.position_x,a.position_y,a.position_z,"
                    "b.position_x,b.position_y,b.position_z "
                    "FROM characters a, characters b WHERE a.guid=%d AND b.guid=%d"
                    % (OWNER_GUID, COMP_GUID)).strip()
    if not out:
        return None
    try:
        x1, y1, z1, x2, y2, z2 = out.split("\t")
        return ((float(x1) - float(x2)) ** 2 +
                (float(y1) - float(y2)) ** 2 +
                (float(z1) - float(z2)) ** 2) ** 0.5
    except ValueError:
        return None


class BotFollowTests(unittest.TestCase):
    base = env = project = evidence = None
    logs = ""
    dist_samples = []
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
        world = p.world_env_for("Followowner")
        world.update(PLAYERBOT_MIN_BOTS="0", PLAYERBOT_MAX_BOTS="0",
                     PLAYERBOT_REFRESH="600000", PLAYERBOT_UPDATE_MS="1000",
                     PLAYERBOT_PROVISION="",
                     PLAYERBOT_TEST_LOGIN="%d,%d,%d" % (OWNER_GUID, COMP_GUID, INTRUDER_GUID),
                     PLAYERBOT_QUEST_ID="0",
                     PLAYERBOT_FOLLOW_SCRIPT=FOLLOW_SCRIPT,
                     PLAYER_SAVE_INTERVAL="5000")
        try:
            cls.project = "tortoise-bot-follow-" + uuid.uuid4().hex[:12]
            stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
            cls.evidence = p.ROOT / "local" / (cls.project + "-" + stamp)
            cls.evidence.mkdir(parents=True)
            cls.base, cls.env = p.boot_lab(cls.project, cls.evidence, world, _seed_sql())
            p.command(["docker", "compose"] + cls.base + ["up", "-d", "--no-deps", "world"],
                      env=cls.env)
            # All three seeded bots must log in through the roster path
            # (second probe login rejected as a duplicate).
            cls.logs = p.wait_for(cls.base, cls.env, lambda text:
                                  all("test-login %d first=1 second=0" % g in text
                                      for g in (OWNER_GUID, COMP_GUID, INTRUDER_GUID)),
                                  deadline=420)
            # First follow goal accepted (owner command via the real chat path).
            cls.logs = p.wait_for(cls.base, cls.env, lambda text:
                                  "follow accepted bot:Followcomp guid:%d leader:%d seq:1"
                                  % (COMP_GUID, OWNER_GUID) in text, deadline=120)
            # DB samples while the first follow window is open (goal seq 1
            # runs from +5s to +60s of the script clock): sustained tracking
            # means three consecutive snapshots within 12 yd of the owner
            # (both bots are saved on the same world save tick, so each
            # sample is a consistent pair snapshot).
            end = time.monotonic() + 75
            while time.monotonic() < end:
                d = _dist(cls.base, cls.env)
                if d is not None:
                    cls.dist_samples.append(d)
                    if len(cls.dist_samples) >= 3 and all(x <= 12.0 for x in cls.dist_samples[-3:]):
                        break
                time.sleep(2)
            (cls.evidence / "distances.txt").write_text(
                "\n".join("%.2f" % d for d in cls.dist_samples) + "\n", encoding="utf-8")
            # Full script horizon: the last (seq 3) stop must be accepted.
            cls.logs = p.wait_for(cls.base, cls.env, lambda text:
                                  "stop accepted bot:Followcomp guid:%d issuer:%d seq:3"
                                  % (COMP_GUID, OWNER_GUID) in text, deadline=420)
            # Quiet window so periodic saves settle, then a clean stop with
            # all three bots online (exercises the World.cpp shutdown fix).
            end = time.monotonic() + 60
            while time.monotonic() < end:
                time.sleep(3)
            p.command(["docker", "compose"] + cls.base + ["stop", "world"], env=cls.env, timeout=180)
            (cls.evidence / "world.log").write_text(
                p.command(["docker", "compose"] + cls.base + ["logs", "--no-color", "world"],
                          env=cls.env, timeout=60),
                encoding="utf-8")
            # Ownership rows before teardown (the lab db is destroyed after).
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
        # The lab is fully torn down here; record the personal server's state
        # after the whole run (the test asserting on it runs first).
        cls.personal_after = cls._personal_state()

    def test_all_seeded_bots_logged_in(self):
        self.assertIn("test-login %d first=1 second=0" % OWNER_GUID, self.logs)
        self.assertIn("test-login %d first=1 second=0" % COMP_GUID, self.logs)
        self.assertIn("test-login %d first=1 second=0" % INTRUDER_GUID, self.logs)
        self.assertIn("[PlayerBot][Login]  'Followowner' GUID:%d" % OWNER_GUID, self.logs)
        self.assertIn("[PlayerBot][Login]  'Followcomp' GUID:%d" % COMP_GUID, self.logs)
        self.assertIn("[PlayerBot][Login]  'Intruder' GUID:%d" % INTRUDER_GUID, self.logs)
        self.assertNotIn("[CRASH]", self.logs)

    def test_follow_accepted_and_reached_within_range(self):
        self.assertIn("follow accepted bot:Followcomp guid:%d leader:%d seq:1" % (COMP_GUID, OWNER_GUID), self.logs)
        self.assertIn("[PlayerBot][Follow] active GUID:%d leader:%d seq:1" % (COMP_GUID, OWNER_GUID), self.logs)
        reached = [m for m in REACHED.findall(self.logs) if m[0] == str(COMP_GUID)]
        seq1 = [m for m in reached if m[3] == "1"]
        self.assertTrue(seq1, "no reached marker for goal seq 1")
        self.assertLessEqual(float(seq1[0][2]), 2.0,
                             "companion must reach within the follow range of the owner")

    def test_db_samples_show_companion_tracking_owner(self):
        self.assertGreaterEqual(len(self.dist_samples), 3,
                                "not enough position samples collected")
        in_range = [d for d in self.dist_samples if d <= 12.0]
        self.assertGreaterEqual(len(in_range), 3,
                                "companion never sustained within 12 yd of the owner: %r"
                                % self.dist_samples)

    def test_stop_inactivates_goal(self):
        for seq in ("1", "2", "3"):
            self.assertIn("stop accepted bot:Followcomp guid:%d issuer:%d seq:%s"
                          % (COMP_GUID, OWNER_GUID, seq), self.logs)
        self.assertEqual(self.logs.count("[PlayerBot][Follow] inactive GUID:%d" % COMP_GUID), 3,
                         "each owner stop must invalidate the active goal exactly once")

    def test_foreign_player_commands_rejected(self):
        self.assertIn("follow rejected not-owner bot:Followcomp issuer:%d acc:%d owner:%d"
                      % (INTRUDER_GUID, INTRUDER_ACC, OWNER_ACC), self.logs)
        self.assertIn("stop rejected not-owner bot:Followcomp issuer:%d acc:%d owner:%d"
                      % (INTRUDER_GUID, INTRUDER_ACC, OWNER_ACC), self.logs)

    def test_stale_goal_after_stop_does_not_resume(self):
        stale = "[PlayerBot][Follow] goal rejected stale seq:2 current:2 GUID:%d" % COMP_GUID
        self.assertIn(stale, self.logs)
        # The expired goal was delivered after the seq-2 stop: the stale
        # rejection must come after that stop, and the seq-2 goal must have
        # been activated exactly once (at the t=+90s follow).
        self.assertGreater(self.logs.find(stale),
                           self.logs.find("stop accepted bot:Followcomp guid:%d issuer:%d seq:2"
                                          % (COMP_GUID, OWNER_GUID)))
        self.assertEqual(self.logs.count("[PlayerBot][Follow] active GUID:%d leader:%d seq:2"
                                         % (COMP_GUID, OWNER_GUID)), 1)

    def test_refollow_after_stop_reaches_again(self):
        reached = [m for m in REACHED.findall(self.logs) if m[0] == str(COMP_GUID)]
        self.assertTrue([m for m in reached if m[3] == "2"],
                        "re-follow (seq 2) never reached the owner")
        self.assertTrue([m for m in reached if m[3] == "3"],
                        "final follow (seq 3) never reached the owner")

    def test_ownership_rows_intact(self):
        self.assertIn("%d\t%d\tNULL\t2" % (OWNER_GUID, OWNER_ACC), self.ownership)
        self.assertIn("%d\t%d\t%d\t2" % (COMP_GUID, COMP_ACC, OWNER_ACC), self.ownership,
                      "companion must remain bound to its own session account "
                      "with the owner binding on %d" % OWNER_ACC)
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
