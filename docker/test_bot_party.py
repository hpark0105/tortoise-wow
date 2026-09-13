"""CMP-010 disposable runtime proof for owned companion party lifecycle."""
import os
import time
import unittest
import uuid
from datetime import datetime, timezone

import test_bot_provision as p

OWNER_GUID = 500130
COMP_GUID = 500131
INTRUDER_GUID = 500132
FILL_GUIDS = (500133, 500134, 500135, 500136)
OWNER_ACC = 1000500130
COMP_ACC = 1000500131
INTRUDER_ACC = 1000500132
ALL_GUIDS = (OWNER_GUID, COMP_GUID, INTRUDER_GUID) + FILL_GUIDS
PERSONAL_CONTAINERS = ("tortoise-local-db-1", "tortoise-local-realmd-1",
                       "tortoise-local-world-1")

PARTY_SCRIPT = ";".join([
    "5000:%d:botrecruit Partycomp" % INTRUDER_GUID,
    "10000:%d:botrecruit Fillone" % OWNER_GUID,
    "12000:%d:botrecruit Filltwo" % OWNER_GUID,
    "14000:%d:botrecruit Fillthree" % OWNER_GUID,
    "16000:%d:botrecruit Fillfour" % OWNER_GUID,
    "20000:%d:botrecruit Partycomp" % OWNER_GUID,
    "25000:%d:botdismiss Fillone" % OWNER_GUID,
    "27000:%d:botdismiss Filltwo" % OWNER_GUID,
    "29000:%d:botdismiss Fillthree" % OWNER_GUID,
    "31000:%d:botdismiss Fillfour" % OWNER_GUID,
    "36000:%d:botrecruit Partycomp" % OWNER_GUID,
    "45000:%d:botdismiss Partycomp" % OWNER_GUID,
    "55000:%d:botrecall Partycomp" % OWNER_GUID,
    "70000:%d:botdismiss Partycomp" % OWNER_GUID,
    "80000:%d:botrecall Partycomp" % OWNER_GUID,
    "80000:%d:botdismiss Partycomp" % OWNER_GUID,
])


def _seed_sql():
    names = ["Partyowner", "Partycomp", "Intruder", "Fillone", "Filltwo",
             "Fillthree", "Fillfour"]
    accounts = [OWNER_ACC, COMP_ACC, INTRUDER_ACC,
                1000500133, 1000500134, 1000500135, 1000500136]
    values = []
    for index, (guid, account, name) in enumerate(zip(ALL_GUIDS, accounts, names)):
        values.append(
            "(%d,%d,'%s',1,1,0,10,100000,%s,-120.493,83.5312,0,0,12,100,0,0,0,0,0,0,1)"
            % (guid, account, name, -8949.95 + index * 2))
    ownership = []
    for guid, account in zip(ALL_GUIDS, accounts):
        owner = str(OWNER_ACC) if guid == COMP_GUID or guid in FILL_GUIDS else "NULL"
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


def _group_snapshot(base, env):
    return p.db_exec(
        base, env,
        "SELECT g.leaderGuid,gm.memberGuid FROM `groups` g "
        "JOIN group_member gm ON gm.groupId=g.groupId ORDER BY gm.memberGuid").strip()


class BotPartyTests(unittest.TestCase):
    base = env = project = evidence = None
    logs = ""
    full_snapshot = recruit_snapshot = recall_snapshot = final_snapshot = ""
    ownership = ""
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
        p.IMAGE = os.environ.get("CMP010_LAB_IMAGE", "tortoise-local:dev")
        try:
            p.command(["docker", "image", "inspect", p.IMAGE])
        except RuntimeError:
            raise unittest.SkipTest(f"{p.IMAGE} image not present; build it first")
        cls.personal_before = cls._personal_state()
        world = p.world_env_for("")
        world.update(PLAYERBOT_MIN_BOTS="0", PLAYERBOT_MAX_BOTS="0",
                     PLAYERBOT_REFRESH="600000", PLAYERBOT_UPDATE_MS="1000",
                     PLAYERBOT_TEST_LOGIN=",".join(str(g) for g in ALL_GUIDS),
                     PLAYERBOT_QUEST_ID="0", PLAYERBOT_FOLLOW_SCRIPT=PARTY_SCRIPT,
                     PLAYER_SAVE_INTERVAL="5000")
        cls.project = "tortoise-bot-party-" + uuid.uuid4().hex[:12]
        stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        cls.evidence = p.ROOT / "local" / (cls.project + "-" + stamp)
        cls.evidence.mkdir(parents=True)
        try:
            cls.base, cls.env = p.boot_lab(cls.project, cls.evidence, world, _seed_sql())
            p.command(["docker", "compose"] + cls.base + ["up", "-d", "--no-deps", "world"],
                      env=cls.env)
            cls.logs = p.wait_for(cls.base, cls.env, lambda text:
                                  "party recruit rejected full bot:Partycomp" in text,
                                  deadline=420)
            cls.full_snapshot = _group_snapshot(cls.base, cls.env)
            cls.logs = p.wait_for(cls.base, cls.env, lambda text:
                                  "party recruit accepted bot:Partycomp" in text,
                                  deadline=120)
            cls.recruit_snapshot = _group_snapshot(cls.base, cls.env)
            cls.logs = p.wait_for(cls.base, cls.env, lambda text:
                                  "party recall queued bot:Partycomp" in text and
                                  text.count("party recruit accepted bot:Partycomp") >= 2,
                                  deadline=120)
            cls.recall_snapshot = _group_snapshot(cls.base, cls.env)
            # Wait for the 80000 dismiss (seq:7): second accepted dismiss.
            cls.logs = p.wait_for(cls.base, cls.env, lambda text:
                                  text.count("party dismiss accepted bot:Partycomp") >= 2,
                                  deadline=120)
            time.sleep(10)
            cls.logs = p.command(["docker", "compose"] + cls.base +
                                 ["logs", "--no-color", "world"], env=cls.env, timeout=60)
            cls.final_snapshot = _group_snapshot(cls.base, cls.env)
            cls.ownership = p.db_exec(
                cls.base, cls.env,
                "SELECT char_guid,account_id,owner_account_id FROM bot_ownership "
                "WHERE char_guid BETWEEN 500130 AND 500136 ORDER BY char_guid")
            (cls.evidence / "world.log").write_text(cls.logs, encoding="utf-8")
            (cls.evidence / "group-snapshots.txt").write_text(
                "FULL\n%s\nRECRUIT\n%s\nRECALL\n%s\nFINAL\n%s\n" %
                (cls.full_snapshot, cls.recruit_snapshot,
                 cls.recall_snapshot, cls.final_snapshot), encoding="utf-8")
            (cls.evidence / "ownership.txt").write_text(cls.ownership, encoding="utf-8")
            p.command(["docker", "compose"] + cls.base + ["stop", "world"],
                      env=cls.env, timeout=180)
            p.teardown_lab(cls.base, cls.env, cls.project, cls.evidence)
            cls.base = None
        except BaseException:
            if cls.base is not None:
                try:
                    (cls.evidence / "world-failure.log").write_text(
                        p.command(["docker", "compose"] + cls.base +
                                  ["logs", "--no-color", "world"],
                                  env=cls.env, timeout=60), encoding="utf-8")
                except Exception:
                    pass
                p.force_down(cls.base, cls.env)
            raise
        cls.personal_after = cls._personal_state()

    def test_foreign_owner_is_rejected(self):
        self.assertIn("party recruit rejected not-owner bot:Partycomp", self.logs)

    def test_full_party_is_rejected(self):
        self.assertIn("party recruit rejected full bot:Partycomp", self.logs)
        rows = self.full_snapshot.splitlines()
        self.assertEqual(len(rows), 5)
        self.assertTrue(all(row.startswith(str(OWNER_GUID) + "\t") for row in rows))

    def test_recruit_uses_normal_group_with_human_leader(self):
        self.assertIn("%d\t%d" % (OWNER_GUID, OWNER_GUID), self.recruit_snapshot)
        self.assertIn("%d\t%d" % (OWNER_GUID, COMP_GUID), self.recruit_snapshot)

    def test_dismiss_and_recall_preserve_owner_and_group_again(self):
        self.assertIn("party dismiss accepted bot:Partycomp", self.logs)
        self.assertIn("party recall queued bot:Partycomp", self.logs)
        # The 36000 recruit (seq:2) created a group with owner as leader
        self.assertIn("party recruit accepted bot:Partycomp guid:%d leader:%d seq:2" % (COMP_GUID, OWNER_GUID), self.logs)
        self.assertIn("%d\t%d\t%d" % (COMP_GUID, COMP_ACC, OWNER_ACC), self.ownership)

    def test_pending_recall_is_invalidated_by_dismiss(self):
        # The 55000 async recall is invalidated: login completion is rejected
        # (bot not yet in-world), the 70000 dismiss is rejected (bot not in
        # party), and the 80000 recall+dismiss cycle leaves the bot out.
        self.assertIn("party recall completion rejected bot:Partycomp", self.logs)
        self.assertIn("party dismiss rejected membership bot:Partycomp", self.logs)
        self.assertIn("party recruit accepted bot:Partycomp guid:%d leader:%d seq:6" % (COMP_GUID, OWNER_GUID), self.logs)
        self.assertIn("party dismiss accepted bot:Partycomp guid:%d leader:%d seq:7" % (COMP_GUID, OWNER_GUID), self.logs)
        self.assertNotIn("%d\t%d" % (OWNER_GUID, COMP_GUID), self.final_snapshot)

    def test_world_and_personal_server_stayed_healthy(self):
        self.assertNotIn("[CRASH]", self.logs)
        self.assertEqual(self.personal_before, self.personal_after)


if __name__ == "__main__":
    unittest.main(verbosity=2)
