"""PORT-024: owned-companion equipment progression from earned loot.

Three disposable, port-free labs in the combat-proven corridor.

Lab A (upgrade/downgrade/illegal + restart persistence): the owner
(610300) and the companion (610301, bound to the owner, seeded with
the baseline sword 1008 in the mainhand equipment slot). One pinned
Kobold Vermin (2500300, entry 6, 150 HP / regen 0) drops loot
template 990301 with three items at 100%: 15335 (a strict upgrade),
3267 (a downgrade) and 7298 (rogue-only, illegal for a warrior). The
companion follows the owner, assists the kill, taps the corpse,
stores all three items and the deterministic evaluator equips 15335
(the baseline 1008 returns to the bag the upgrade came from), keeps
3267 (not a strict upgrade) and 7298 (fails the authoritative class
check) in the bag. The world is then restarted in place and the saved
equipment/inventory state must round-trip unchanged: the same
instance GUIDs, exactly one equipped slot (no free level-based
refresh for the owned companion), and no equipment log lines in the
new boot generation.

Lab B (full-bag pressure): the same assist-kill flow, but the
companion's main bag is pre-filled (16 stacks of 20 Earthroot, item
2449) and the vermin drops only the upgrade 15335. AutoStoreLoot
cannot store it; the evaluator raises the bounded inventory-pressure
state without deleting anything, equipment stays at the seeded
baseline, and the companion still regroups.

Lab C (ambient policy preserved): a single ambient bot (610500, no
owner account) in a cleared corridor. Its automatic level-based
equipment mints on login (several equipped slots, mainhand included),
proving the AutoEquipForLevel gate leaves the ambient policy
untouched.

In-game verification of the visual swap is PORT-026; these are log +
DB assertions.
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

BOOT_MARKER = "World server is up and running!"
MAINHAND = 15            # EquipmentSlots::EQUIPMENT_SLOT_MAINHAND
EQUIP_END = 19           # EquipmentSlots::EQUIPMENT_SLOT_END
BAG_START, BAG_END = 23, 39  # InventoryPackSlots (bag0 item slots)

ITEM_BASELINE = 1008     # Well-used Sword (ilvl 10, common): score 6020
ITEM_UPGRADE = 15335     # Briarsteel Shortsword (ilvl 14, fine): score 9034
ITEM_DOWNGRADE = 3267    # Forsaken Shortsword (ilvl 5, common): score 3015
ITEM_ILLEGAL = 7298      # Blade of Cunning (rogue only)
ITEM_FILLER = 2449       # Earthroot (trade goods, common, stack 20)

BASELINE_INSTANCE = 9000301


def _characters(o, c):
    return (
        "INSERT INTO tw_char.characters\n"
        " (guid,account,name,race,class,gender,level,money,position_x,position_y,position_z,map,\n"
        "  orientation,zone,health,power1,power2,power3,power4,power5,xp,xp_gain)\n"
        "VALUES (%d,1000%d,'Eqowner',1,1,0,10,100000,-8949.95,-130.493,83.5312,0,\n"
        "  0,12,100,0,0,0,0,0,0,1),\n"
        "       (%d,1000%d,'Eqcomp',1,1,0,10,100000,-8949.95,-200.493,83.5312,0,\n"
        "  0,12,100,0,0,0,0,0,0,1);\n" % (o, o, c, c))


def _ownership(o, c):
    return (
        "INSERT INTO tw_char.playerbot (char_guid,chance,ai)\n"
        " VALUES (%d,100,'Default'),(%d,100,'Default');\n"
        "INSERT INTO tw_char.bot_ownership (char_guid,account_id,bot_type,provision_version,owner_account_id)\n"
        " VALUES (%d,1000%d,1,2,1000%d),\n"
        "        (%d,1000%d,1,2,1000%d);\n" % (o, c, o, o, o, c, c, o))


def _instance_rows(rows):
    return (
        "INSERT INTO tw_char.item_instance\n"
        " (guid,itemEntry,owner_guid,creatorGuid,giftCreatorGuid,count,duration,charges,flags,enchantments,\n"
        "  randomPropertyId,transmogrifyId,durability,text,generated_loot)\n"
        "VALUES" + ",".join(rows) + ";\n")


def _inventory_rows(rows):
    return ("INSERT INTO tw_char.character_inventory (guid,bag,slot,item,item_template)\n"
            "VALUES" + ",".join(rows) + ";\n")


def _baseline_weapon(c):
    inst = "(%d,%d,%d,0,0,1,0,'0 0 0 0 0',0,'',0,0,65535,0,0)" % (BASELINE_INSTANCE, ITEM_BASELINE, c)
    inv = "(%d,0,%d,%d,%d)" % (c, MAINHAND, BASELINE_INSTANCE, ITEM_BASELINE)
    return _instance_rows([inst]) + _inventory_rows([inv])


def _filler_stacks(c):
    inst, inv = [], []
    for i in range(16):
        g = 9000410 + i
        inst.append("(%d,%d,%d,0,0,20,0,'0 0 0 0 0',0,'',0,0,0,0,0)" % (g, ITEM_FILLER, c))
        inv.append("(%d,0,%d,%d,%d)" % (c, BAG_START + i, g, ITEM_FILLER))
    return _instance_rows(inst) + _inventory_rows(inv)


def _loot(loot_id, items):
    rows = ",".join(" (%d,%d,100,0,1,1,0)" % (loot_id, i) for i in items)
    return (
        "UPDATE tw_world.creature_template SET loot_id=%d WHERE entry=6;\n"
        "DELETE FROM tw_world.creature_loot_template WHERE entry=%d;\n"
        "INSERT INTO tw_world.creature_loot_template\n"
        " (entry,item,ChanceOrQuestChance,groupid,mincountOrRef,maxcount,condition_id)\n"
        "VALUES%s;\n" % (loot_id, loot_id, rows))


def _vermin(v, hp=150):
    return (
        "UPDATE tw_world.creature_template\n"
        " SET health_min=%d, health_max=%d, regeneration=0 WHERE entry=6;\n"
        "INSERT INTO tw_world.creature\n"
        " (guid,id,map,position_x,position_y,position_z,orientation,spawntimesecsmin,\n"
        "  spawntimesecsmax,wander_distance,health_percent,mana_percent,movement_type,spawn_flags)\n"
        "VALUES (%d,6,0,-8949.95,-163.493,83.5312,0,600,600,0,100,100,0,1);\n"
        "DELETE FROM tw_world.creature WHERE map=0 AND position_x BETWEEN -8990 AND -8910\n"
        " AND position_y BETWEEN -230 AND -100 AND guid NOT IN (%d);\n" % (hp, hp, v, v))


class LabMixin:
    """Shared owner+companion lab driver; mixed into the concrete labs."""
    o = c = v = None
    loot_id = 0
    loot_items = ()
    baseline = False
    fill_bags = False
    restart = False
    comp_name = "Eqcomp"
    base = env = project = evidence = None
    logs = ""
    logs2 = ""
    boots = 0
    state1 = None
    state2 = None
    personal_before = {}
    personal_after = {}

    @classmethod
    def _script(cls):
        o = cls.o
        return ";".join([
            "2000:%d:bothold Eqowner" % o,
            "4000:%d:bothold Eqcomp" % o,
            "8000:%d:botrecruit Eqcomp" % o,
            "8500:%d:botfollow Eqcomp" % o,
            "9000:%d:botassist Eqcomp Kobold Vermin" % o,
            "90000:%d:bothold Eqcomp" % o,
            "100000:%d:botstop Eqcomp" % o,
        ])

    @classmethod
    def _seed(cls):
        sql = _characters(cls.o, cls.c) + _ownership(cls.o, cls.c)
        if cls.baseline:
            sql += _baseline_weapon(cls.c)
        if cls.fill_bags:
            sql += _filler_stacks(cls.c)
        if cls.loot_id:
            sql += _loot(cls.loot_id, cls.loot_items)
        sql += _vermin(cls.v)
        return sql

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
        crashes = [l for l in self.logs.splitlines()
                   if "[CRASH]" in l and "already spawned Gobj" not in l]
        self.assertFalse(crashes, "world crashed: %r" % crashes[:3])

    @classmethod
    def _inventory(cls):
        q = ("SELECT ci.slot, ci.item, ci.item_template FROM tw_char.character_inventory "
             "ci WHERE ci.guid=%d AND ci.bag=0 ORDER BY slot" % cls.c)
        rows = []
        for line in p.db_exec(cls.base, cls.env, q).splitlines():
            parts = line.split("\t")
            if len(parts) == 3:
                rows.append((int(parts[0]), int(parts[1]), int(parts[2])))
        return rows

    @classmethod
    def setUpClass(cls):
        p.IMAGE = os.environ.get("PORT024_LAB_IMAGE", "tortoise-local:dev")
        try:
            p.command(["docker", "image", "inspect", p.IMAGE])
        except RuntimeError:
            raise unittest.SkipTest("%s image not present; build it first" % p.IMAGE)
        cls.personal_before = cls._personal_state()
        world = p.world_env_for("Eqowner")
        world.update(PLAYERBOT_MIN_BOTS="0", PLAYERBOT_MAX_BOTS="0",
                     PLAYERBOT_REFRESH="600000", PLAYERBOT_UPDATE_MS="1000",
                     PLAYERBOT_PROVISION="",
                     PLAYERBOT_TEST_LOGIN="%d,%d" % (cls.o, cls.c),
                     PLAYERBOT_QUEST_ID="0",
                     PLAYERBOT_WANDER_RADIUS="0.5",
                     PLAYERBOT_FOLLOW_SCRIPT=cls._script(),
                     PLAYER_SAVE_INTERVAL="5000")
        try:
            cls.project = "tortoise-bot-eq-" + uuid.uuid4().hex[:12]
            stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
            cls.evidence = p.ROOT / "local" / (cls.project + "-" + stamp)
            cls.evidence.mkdir(parents=True)
            (cls.evidence / "image-id.txt").write_text(
                p.command(["docker", "image", "inspect", "--format", "{{.Id}}", p.IMAGE]),
                encoding="utf-8")
            try:
                cls.base, cls.env = p.boot_lab(cls.project, cls.evidence, world, cls._seed())
            except BaseException:
                p.force_down(["-f", str(cls.evidence / "compose.json"), "-p", cls.project],
                             dict(os.environ, BOT_LAB_ROOT_PASSWORD="heal",
                                  BOT_LAB_DB_PASSWORD="heal"))
                raise
            p.command(["docker", "compose"] + cls.base + ["up", "-d", "--no-deps", "world"],
                      env=cls.env)
            cls.logs = p.wait_for(cls.base, cls.env, lambda text:
                                  all("test-login %d first=1 second=0" % g in text
                                      for g in (cls.o, cls.c)), deadline=420)
            cls.logs = cls._drive()
            end = time.monotonic() + 45
            while time.monotonic() < end:
                time.sleep(3)
            cls.state1 = cls._inventory()
            # Capture DB-visible counts while the lab database is still up;
            # the lab is torn down before the test methods run.
            cls.filler_count = p.db_int(cls.base, cls.env,
                "SELECT COALESCE(SUM(ii.count),0) FROM character_inventory ci "
                "JOIN item_instance ii ON ii.guid=ci.item "
                "WHERE ci.guid=%d AND ci.item_template=%d" % (cls.c, ITEM_FILLER))
            cls.upgrade_count = p.db_int(cls.base, cls.env,
                "SELECT COUNT(*) FROM character_inventory "
                "WHERE guid=%d AND item_template=%d" % (cls.c, ITEM_UPGRADE))
            p.command(["docker", "compose"] + cls.base + ["stop", "world"],
                      env=cls.env, timeout=180)
            if cls.restart:
                cls.state2 = cls._restart_phase()
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

    @classmethod
    def _restart_phase(cls):
        # A single project may restart the world container in place,
        # so docker logs accumulate across generations; scope every
        # restart assertion to the text after the new boot marker.
        cls.boots = cls.logs.count(BOOT_MARKER)
        p.command(["docker", "compose"] + cls.base + ["up", "-d", "--no-deps", "world"],
                  env=cls.env)
        cls.logs2 = p.wait_for(cls.base, cls.env, lambda text:
                               text.count(BOOT_MARKER) > cls.boots
                               and ("[PlayerBot][Login]  '%s' GUID:%d" % (cls.comp_name, cls.c))
                               in text.rsplit(BOOT_MARKER, cls.boots + 1)[-1],
                               deadline=420)
        end = time.monotonic() + 20
        while time.monotonic() < end:
            time.sleep(3)
        return cls._inventory()

    @property
    def restart_tail(self):
        return self.logs2.rsplit(BOOT_MARKER, self.boots + 1)[-1]

    def test_all_seeded_bots_logged_in(self):
        for g, n in ((self.o, "Eqowner"), (self.c, self.comp_name)):
            self.assertIn("test-login %d first=1 second=0" % g, self.logs)
            self.assertIn("[PlayerBot][Login]  '%s' GUID:%d" % (n, g), self.logs)
        self.assert_no_real_crashes()

    def test_party_formed(self):
        self.assertIn("party recruit accepted bot:%s guid:%d leader:%d"
                      % (self.comp_name, self.c, self.o), self.logs)

    def test_equipment_no_free_refresh(self):
        equipped = [r for r in self.state1 if r[0] < EQUIP_END]
        self.assertEqual(len(equipped), 1,
                         "the owned companion must keep exactly its earned"
                         " equipment (no free level-based refresh): %r" % self.state1)

    def test_personal_volume_untouched(self):
        self.assertNotEqual(self.project, "tortoise-local")
        if self.personal_before:
            self.assertEqual(self.personal_after, self.personal_before,
                             "personal server containers must not be restarted or touched")


class BotCompanionEquipmentUpgradeTests(LabMixin, unittest.TestCase):
    """Lab A: upgrade equipped, downgrade + illegal kept, restart round-trip."""
    o, c, v = 610300, 610301, 2500300
    loot_id = 990301
    loot_items = (ITEM_UPGRADE, ITEM_DOWNGRADE, ITEM_ILLEGAL)
    baseline = True
    restart = True

    @classmethod
    def _drive(cls):
        return p.wait_for(cls.base, cls.env, lambda text:
                          ("equipment upgraded GUID:%d slot:%d item:%d"
                           % (cls.c, MAINHAND, ITEM_UPGRADE) in text
                           and ("equipment keep GUID:%d item:%d"
                                % (cls.c, ITEM_DOWNGRADE)) in text
                           and ("equipment skip illegal GUID:%d item:%d"
                                % (cls.c, ITEM_ILLEGAL)) in text
                           and "[PlayerBot][Follow] reached GUID:%d" % cls.c in text),
                          deadline=420)

    def test_upgrade_equipped_with_replacement_retained(self):
        equipped = [r for r in self.state1 if r[0] < EQUIP_END]
        self.assertEqual([r[0] for r in equipped], [MAINHAND])
        self.assertEqual(equipped[0][2], ITEM_UPGRADE,
                         "the strict upgrade must be equipped: %r" % self.state1)
        self.assertIn((MAINHAND, equipped[0][1], ITEM_UPGRADE), self.state1)
        retained = [r for r in self.state1 if r[1] == BASELINE_INSTANCE]
        self.assertEqual(len(retained), 1, "the baseline instance must be retained")
        self.assertEqual(retained[0][2], ITEM_BASELINE)
        self.assertGreaterEqual(retained[0][0], BAG_START)
        for tpl in (ITEM_DOWNGRADE, ITEM_ILLEGAL):
            kept = [r for r in self.state1 if r[2] == tpl]
            self.assertEqual(len(kept), 1,
                             "item %d must be kept in the bag: %r" % (tpl, self.state1))
            self.assertGreaterEqual(kept[0][0], BAG_START)

    def test_restart_roundtrip(self):
        self.assertEqual(sorted(self.state2), sorted(self.state1),
                         "saved equipment/inventory must round-trip the restart unchanged")
        equipped = [r for r in self.state2 if r[0] < EQUIP_END]
        self.assertEqual(len(equipped), 1)
        self.assertEqual(equipped[0][2], ITEM_UPGRADE)
        # No re-evaluation or re-mint in the new boot generation.
        for marker in ("equipment upgraded", "equipment keep",
                       "equipment skip illegal", "equipment pressure"):
            self.assertNotIn(marker, self.restart_tail)


class BotCompanionEquipmentPressureTests(LabMixin, unittest.TestCase):
    """Lab B: full bags raise bounded pressure without deletion or equip."""
    o, c, v = 610400, 610401, 2500400
    loot_id = 990401
    loot_items = (ITEM_UPGRADE,)
    baseline = True
    fill_bags = True

    @classmethod
    def _drive(cls):
        return p.wait_for(cls.base, cls.env, lambda text:
                          ("equipment pressure GUID:%d item:%d stored:0"
                           % (cls.c, ITEM_UPGRADE) in text
                           and "[PlayerBot][Follow] reached GUID:%d" % cls.c in text),
                          deadline=420)

    def test_pressure_raised_no_upgrade(self):
        self.assertIn("equipment pressure GUID:%d item:%d stored:0"
                      % (self.c, ITEM_UPGRADE), self.logs)
        self.assertNotIn("equipment upgraded", self.logs)
        self.assertNotIn("equipment swap denied", self.logs)

    def test_nothing_stored_nothing_deleted(self):
        # The lab database is disposed before the test methods run, so the
        # counts are captured in setUpClass while it is still up.
        self.assertEqual(self.filler_count, 16 * 20,
                         "the filler stacks must be untouched")
        self.assertEqual(self.upgrade_count, 0,
                         "an unstoreable item must not be minted or kept")

    def test_equipment_unchanged(self):
        equipped = [r for r in self.state1 if r[0] < EQUIP_END]
        self.assertEqual(equipped, [(MAINHAND, BASELINE_INSTANCE, ITEM_BASELINE)],
                         "the seeded baseline must stay equipped: %r" % self.state1)

    def test_regrouped_after_pressure(self):
        idx = self.logs.find("equipment pressure GUID:%d item:%d stored:0"
                             % (self.c, ITEM_UPGRADE))
        self.assertGreater(idx, -1)
        reached = [m for m in f.REACHED.findall(self.logs[idx:])
                   if m[0] == str(self.c)]
        self.assertTrue(reached, "the companion must regroup after a pressure event")
        self.assertLessEqual(float(reached[0][2]), 2.0)


class BotCompanionEquipmentAmbientTests(unittest.TestCase):
    """Lab C: the ambient bot keeps its automatic level-based equipment."""
    a = 610500
    base = env = project = evidence = None
    logs = ""
    equip_rows = None
    personal_before = {}
    personal_after = {}

    @classmethod
    def _seed(cls):
        return (
            "INSERT INTO tw_char.characters\n"
            " (guid,account,name,race,class,gender,level,money,position_x,position_y,position_z,map,\n"
            "  orientation,zone,health,power1,power2,power3,power4,power5,xp,xp_gain)\n"
            "VALUES (%d,1000%d,'Eqamb',1,1,0,10,100000,-8949.95,-130.493,83.5312,0,\n"
            " 0,12,100,0,0,0,0,0,0,1);\n"
            "INSERT INTO tw_char.playerbot (char_guid,chance,ai) VALUES (%d,100,'Default');\n"
            "INSERT INTO tw_char.bot_ownership (char_guid,account_id,bot_type,provision_version,owner_account_id)\n"
            " VALUES (%d,1000%d,1,2,NULL);\n"
            "DELETE FROM tw_world.creature WHERE map=0 AND position_x BETWEEN -8990 AND -8910\n"
            " AND position_y BETWEEN -230 AND -100;\n"
            % (cls.a, cls.a, cls.a, cls.a, cls.a))

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
        p.IMAGE = os.environ.get("PORT024_LAB_IMAGE", "tortoise-local:dev")
        try:
            p.command(["docker", "image", "inspect", p.IMAGE])
        except RuntimeError:
            raise unittest.SkipTest("%s image not present; build it first" % p.IMAGE)
        cls.personal_before = cls._personal_state()
        world = p.world_env_for("Eqamb")
        world.update(PLAYERBOT_MIN_BOTS="0", PLAYERBOT_MAX_BOTS="0",
                     PLAYERBOT_REFRESH="600000", PLAYERBOT_UPDATE_MS="1000",
                     PLAYERBOT_PROVISION="",
                     PLAYERBOT_TEST_LOGIN=str(cls.a),
                     PLAYERBOT_QUEST_ID="0",
                     PLAYERBOT_WANDER_RADIUS="0.5",
                     PLAYER_SAVE_INTERVAL="5000")
        try:
            cls.project = "tortoise-bot-eqamb-" + uuid.uuid4().hex[:12]
            stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
            cls.evidence = p.ROOT / "local" / (cls.project + "-" + stamp)
            cls.evidence.mkdir(parents=True)
            (cls.evidence / "image-id.txt").write_text(
                p.command(["docker", "image", "inspect", "--format", "{{.Id}}", p.IMAGE]),
                encoding="utf-8")
            try:
                cls.base, cls.env = p.boot_lab(cls.project, cls.evidence, world, cls._seed())
            except BaseException:
                p.force_down(["-f", str(cls.evidence / "compose.json"), "-p", cls.project],
                             dict(os.environ, BOT_LAB_ROOT_PASSWORD="heal",
                                  BOT_LAB_DB_PASSWORD="heal"))
                raise
            p.command(["docker", "compose"] + cls.base + ["up", "-d", "--no-deps", "world"],
                      env=cls.env)
            cls.logs = p.wait_for(cls.base, cls.env, lambda text:
                                  "test-login %d first=1 second=0" % cls.a in text,
                                  deadline=420)
            end = time.monotonic() + 20
            while time.monotonic() < end:
                time.sleep(3)
            rows = []
            for line in p.db_exec(cls.base, cls.env,
                                  "SELECT ci.slot, ci.item, ci.item_template "
                                  "FROM character_inventory ci "
                                  "WHERE ci.guid=%d AND ci.bag=0 ORDER BY slot" % cls.a
                                  ).splitlines():
                parts = line.split("\t")
                if len(parts) == 3:
                    rows.append((int(parts[0]), int(parts[1]), int(parts[2])))
            cls.equip_rows = rows
            p.command(["docker", "compose"] + cls.base + ["stop", "world"],
                      env=cls.env, timeout=180)
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

    def test_ambient_autoequip_preserved(self):
        equipped = [r for r in self.equip_rows if r[0] < EQUIP_END]
        self.assertGreaterEqual(len(equipped), 3,
                                "the ambient bot must keep its automatic"
                                " equipment: %r" % self.equip_rows)
        self.assertTrue(any(r[0] == MAINHAND for r in equipped),
                        "the ambient bot must be auto-equipped a mainhand weapon")

    def test_login_and_no_crashes(self):
        self.assertIn("test-login %d first=1 second=0" % self.a, self.logs)
        crashes = [l for l in self.logs.splitlines()
                   if "[CRASH]" in l and "already spawned Gobj" not in l]
        self.assertFalse(crashes, "world crashed: %r" % crashes[:3])

    def test_personal_volume_untouched(self):
        self.assertNotEqual(self.project, "tortoise-local")
        if self.personal_before:
            self.assertEqual(self.personal_after, self.personal_before,
                             "personal server containers must not be restarted or touched")


if __name__ == "__main__":
    unittest.main()
