"""PORT-007: bounded corpse loot + regroup for one owned companion.

Two disposable, port-free labs in the combat-proven corridor.

Lab A (loot + regroup): the owner (610100) and the companion (610101, bound to the
owner), which sits 70 yd south. One Kobold Vermin
(2500010, entry 6, pinned 150 HP / regen 0) sits 33 yd south of the owner
(37 yd from the companion's spawn) and drops item 117 (100% chance). The owner pins itself first (so it never
auto-aggresses the vermin), holds the companion, recruits it into the party,
and orders it to follow. The companion follows the owner by default from
spawn (Phase 1.1: the default owner-follow catch-up starts at login and a
hold does not stop it; the explicit .botfollow re-issues the same goal); the
assist lands while it is still mid-walk, about 25 yd from the vermin (the
assist lookup is 30 yd, and the vermin's 12 yd detection range keeps the
pinned owner at 33 yd clear, so the vermin only ever fights the companion). The companion is the sole party damager, so it has loot
rights. The idle-wander radius is clamped to 0.5 yd (PlayerBot.WanderRadius)
so the owner cannot drift into the vermin's aggro radius before its hold. When the vermin dies the dead corpse becomes a first-class Loot intent
(PORT-007): the companion taps it, auto-stores item 117, and the follow goal
resumes so it regroups to the owner. Acceptance: the stored line, the saved
inventory delta (DB), and the post-loot [Follow] reached.

Lab B (no-trap): the same flow but the vermin is lootless (loot_id 0). The
Loot intent still fires, the bounded attempt stores nothing (or gives up on a
silent denial), and the companion must regroup to the owner with zero
inventory delta and no stuck state. Acceptance: no stored line, the post-loot
[Follow] reached, and a zero saved-inventory delta.

In-game verification of the actual corpse/loot is PORT-010; these are log +
DB assertions. Full-bag, missing-rights, inaccessible-corpse and
owner-moves-away remain residual gaps (not fixture-satisfiable
deterministically this wave).
"""
import os
import time
import unittest
import uuid
from datetime import datetime, timezone

import test_bot_provision as p
import test_bot_follow as f

PERSONAL_CONTAINERS = ("tortoise-local-db-1", "tortoise-local-realmd-1",
                       "tortoise-local-world-1")


def _seed_sql(o, c, v, hp, loot_id):
    if loot_id:
        loot = (
            "UPDATE tw_world.creature_template SET loot_id=%d WHERE entry=6;\n"
            "DELETE FROM tw_world.creature_loot_template WHERE entry=%d;\n"
            "INSERT INTO tw_world.creature_loot_template\n"
            " (entry,item,ChanceOrQuestChance,groupid,mincountOrRef,maxcount,condition_id)\n"
            " VALUES (%d,117,100,0,1,1,0);\n" % (loot_id, loot_id, loot_id))
    else:
        loot = "UPDATE tw_world.creature_template SET loot_id=0 WHERE entry=6;\n"
    return (
        "INSERT INTO tw_char.characters\n"
        " (guid,account,name,race,class,gender,level,money,position_x,position_y,position_z,map,\n"
        "  orientation,zone,health,power1,power2,power3,power4,power5,xp,xp_gain)\n"
        "VALUES ({o},1000{o},'Rgowner',1,1,0,10,100000,-8949.95,-130.493,83.5312,0,\n"
        " 0,12,100,0,0,0,0,0,0,1),\n"
        " ({c},1000{c},'Rgcomp',1,1,0,10,100000,-8949.95,-200.493,83.5312,0,\n"
        " 0,12,100,0,0,0,0,0,0,1);\n"
        "INSERT INTO tw_char.playerbot (char_guid,chance,ai)\n"
        " VALUES ({o},100,'Default'),({c},100,'Default');\n"
        "INSERT INTO tw_char.bot_ownership (char_guid,account_id,bot_type,provision_version,owner_account_id)\n"
        " VALUES ({o},1000{o},1,2,1000{o}),\n"
        "        ({c},1000{c},1,2,1000{o});\n"
        "{loot}"
        "UPDATE tw_world.creature_template\n"
        " SET health_min={hp}, health_max={hp}, regeneration=0 WHERE entry=6;\n"
        "INSERT INTO tw_world.creature\n"
        " (guid,id,map,position_x,position_y,position_z,orientation,spawntimesecsmin,\n"
        "  spawntimesecsmax,wander_distance,health_percent,mana_percent,movement_type,spawn_flags)\n"
        "VALUES ({v},6,0,-8949.95,-163.493,83.5312,0,600,600,0,100,100,0,1);\n"
        "DELETE FROM tw_world.creature WHERE map=0 AND position_x BETWEEN -8990 AND -8910\n"
        " AND position_y BETWEEN -230 AND -100 AND guid NOT IN ({v});\n"
    ).format(o=o, c=c, v=v, hp=hp, loot=loot)


class RegroupMixin:
    """Shared lab driver + assertions; mixed into the concrete TestCase classes."""
    o = c = v = None
    comp_name = "Rgcomp"
    item = 117
    loot_id = 0
    vermin_hp = 150
    base = env = project = evidence = None
    logs = ""
    saved_item_count = None
    personal_before = {}
    personal_after = {}

    @classmethod
    def _script(cls):
        o = cls.o
        return ";".join([
            "2000:%d:bothold Rgowner" % o,
            "4000:%d:bothold Rgcomp" % o,
            "8000:%d:botrecruit Rgcomp" % o,
            "8500:%d:botfollow Rgcomp" % o,
            "9000:%d:botassist Rgcomp Kobold Vermin" % o,
            "90000:%d:bothold Rgcomp" % o,
            "100000:%d:botstop Rgcomp" % o,
        ])

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

    def assert_no_real_crashes(self):
        # The worldspawn loader emits a benign "[CRASH] Spawning already
        # spawned Gobj" warning for duplicate gameobject spawns at boot;
        # any other [CRASH] line is a real crash and fails the lab.
        crashes = [l for l in self.logs.splitlines()
                   if "[CRASH]" in l and "already spawned Gobj" not in l]
        self.assertFalse(crashes, "world crashed: %r" % crashes[:3])

    @classmethod
    def _inventory_count(cls, guid, item):
        q = ("SELECT COALESCE(SUM(ii.count),0) FROM character_inventory ci "
             "JOIN item_instance ii ON ii.guid=ci.item "
             "WHERE ci.guid=%d AND ci.item_template=%d" % (guid, item))
        return p.db_int(cls.base, cls.env, q)

    @classmethod
    def setUpClass(cls):
        p.IMAGE = os.environ.get("PORT007_LAB_IMAGE", "tortoise-local:dev")
        try:
            p.command(["docker", "image", "inspect", p.IMAGE])
        except RuntimeError:
            raise unittest.SkipTest("%s image not present; build it first" % p.IMAGE)
        cls.personal_before = cls._personal_state()
        world = p.world_env_for("Rgowner")
        world.update(PLAYERBOT_MIN_BOTS="0", PLAYERBOT_MAX_BOTS="0",
                     PLAYERBOT_REFRESH="600000", PLAYERBOT_UPDATE_MS="1000",
                     PLAYERBOT_PROVISION="",
                     PLAYERBOT_TEST_LOGIN="%d,%d" % (cls.o, cls.c),
                     PLAYERBOT_QUEST_ID="0",
                     PLAYERBOT_WANDER_RADIUS="0.5",
                     PLAYERBOT_FOLLOW_SCRIPT=cls._script(),
                     PLAYER_SAVE_INTERVAL="5000")
        try:
            cls.project = "tortoise-bot-rg-" + uuid.uuid4().hex[:12]
            stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
            cls.evidence = p.ROOT / "local" / (cls.project + "-" + stamp)
            cls.evidence.mkdir(parents=True)
            (cls.evidence / "image-id.txt").write_text(
                p.command(["docker", "image", "inspect", "--format", "{{.Id}}", p.IMAGE]),
                encoding="utf-8")
            seed = _seed_sql(cls.o, cls.c, cls.v, cls.vermin_hp, cls.loot_id)
            cls.base, cls.env = p.boot_lab(cls.project, cls.evidence, world, seed)
            p.command(["docker", "compose"] + cls.base + ["up", "-d", "--no-deps", "world"],
                      env=cls.env)
            cls.logs = p.wait_for(cls.base, cls.env, lambda text:
                                  all("test-login %d first=1 second=0" % g in text
                                      for g in (cls.o, cls.c)), deadline=420)
            cls.logs = cls._drive()
            end = time.monotonic() + 45
            while time.monotonic() < end:
                time.sleep(3)
            p.command(["docker", "compose"] + cls.base + ["stop", "world"],
                      env=cls.env, timeout=180)
            cls.saved_item_count = cls._inventory_count(cls.c, cls.item)
            (cls.evidence / "world.log").write_text(
                p.command(["docker", "compose"] + cls.base + ["logs", "--no-color", "world"],
                          env=cls.env, timeout=60),
                encoding="utf-8")
            p.teardown_lab(cls.base, cls.env, cls.project, cls.evidence)
            cls.base = None
        except BaseException:
            if cls.base is not None and cls.evidence is not None:
                try:
                    failure_logs = p.command(["docker", "compose"] + cls.base +
                                             ["logs", "--no-color", "world"], env=cls.env,
                                             timeout=60)
                    (cls.evidence / "world-failure.log").write_text(failure_logs,
                                                                    encoding="utf-8")
                except Exception:
                    pass
                p.force_down(cls.base, cls.env)
            raise
        cls.personal_after = cls._personal_state()

    @classmethod
    def _drive(cls):
        raise NotImplementedError

    def test_all_seeded_bots_logged_in(self):
        for g, n in ((self.o, "Rgowner"), (self.c, self.comp_name)):
            self.assertIn("test-login %d first=1 second=0" % g, self.logs)
            self.assertIn("[PlayerBot][Login]  '%s' GUID:%d" % (n, g), self.logs)
        self.assert_no_real_crashes()

    def test_party_formed(self):
        self.assertIn("party recruit accepted bot:%s guid:%d leader:%d"
                      % (self.comp_name, self.c, self.o), self.logs)

    def test_regroup_follow_reaches_after_loot(self):
        idx = self.logs.find(self._reached_anchor())
        self.assertGreater(idx, -1, "no anchor for the regroup found")
        reached = [m for m in f.REACHED.findall(self.logs[idx:])
                   if m[0] == str(self.c)]
        self.assertTrue(reached, "the post-loot follow never reached the owner")
        self.assertLessEqual(float(reached[0][2]), 2.0,
                             "companion must reach within the follow range of the owner")

    def test_personal_volume_untouched(self):
        self.assertNotEqual(self.project, "tortoise-local")
        if self.personal_before:
            self.assertEqual(self.personal_after, self.personal_before,
                             "personal server containers must not be restarted or touched")

    def _reached_anchor(self):
        raise NotImplementedError


class BotCompanionRegroupLootTests(RegroupMixin, unittest.TestCase):
    """Lab A: assist-kill -> first-class Loot intent -> stored +1 -> regroup."""
    o, c, v = 610100, 610101, 2500010
    loot_id = 990101
    vermin_hp = 150

    @classmethod
    def _drive(cls):
        p.wait_for(cls.base, cls.env, lambda text:
                   "corpse loot stored GUID:%d item:%d before:0 after:1"
                   % (cls.c, cls.item) in text, deadline=240)
        return p.wait_for(cls.base, cls.env, lambda text:
                          ("corpse loot stored GUID:%d item:%d before:0 after:1"
                           % (cls.c, cls.item) in text
                           and "[PlayerBot][Follow] reached GUID:%d" % cls.c in text),
                          deadline=150)

    def _reached_anchor(self):
        return "corpse loot stored GUID:%d item:%d before:0 after:1" % (self.c, self.item)

    def test_loot_intent_engaged(self):
        self.assertIn("[PlayerBot] loot intent GUID:%d target:%d" % (self.c, self.v),
                      self.logs)

    def test_stored_line(self):
        self.assertIn("corpse loot stored GUID:%d item:%d before:0 after:1"
                      % (self.c, self.item), self.logs)

    def test_saved_inventory_delta(self):
        self.assertEqual(self.saved_item_count, 1,
                         "the looted item 117 must be saved once (got %r)"
                         % self.saved_item_count)


class BotCompanionRegroupNoLootTests(RegroupMixin, unittest.TestCase):
    """Lab B: assist-kill of a lootless corpse -> no delta -> regroup, no trap."""
    o, c, v = 610400, 610401, 2500040
    loot_id = 0
    vermin_hp = 150

    @classmethod
    def _drive(cls):
        return p.wait_for(cls.base, cls.env, lambda text:
                          ("[PlayerBot] loot intent GUID:%d target:%d" % (cls.c, cls.v) in text
                           and "[PlayerBot][Follow] reached GUID:%d" % cls.c in text),
                          deadline=240)

    def _reached_anchor(self):
        return "[PlayerBot] loot intent GUID:%d target:%d" % (self.c, self.v)

    def test_loot_intent_engaged(self):
        self.assertIn("[PlayerBot] loot intent GUID:%d target:%d" % (self.c, self.v),
                      self.logs)

    def test_nothing_stored(self):
        self.assertNotIn("corpse loot stored GUID:%d item:%d" % (self.c, self.item),
                         self.logs, "a lootless corpse must not store item 117")

    def test_no_trap_and_regrouped(self):
        idx = self.logs.find(self._reached_anchor())
        self.assertGreater(idx, -1)
        tail = self.logs[idx:]
        self.assertTrue(("[PlayerBot] corpse loot processed GUID:%d" % self.c) in tail
                        or ("[PlayerBot] corpse loot giving up GUID:%d" % self.c) in tail
                        or ("[PlayerBot] corpse loot timeout GUID:%d" % self.c) in tail,
                        "the lootless corpse attempt must terminate")
        self.assertTrue("[PlayerBot][Follow] reached GUID:%d" % self.c in tail,
                        "the companion must regroup after the lootless corpse")

    def test_saved_inventory_unchanged(self):
        self.assertEqual(self.saved_item_count, 0,
                         "nothing may be saved from a lootless corpse (got %r)"
                         % self.saved_item_count)


if __name__ == "__main__":
    unittest.main()
