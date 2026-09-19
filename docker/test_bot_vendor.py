"""PORT-025: bag-pressure reporting and bounded vendor junk cleanup.

Four disposable, port-free labs in the combat-proven corridor.

Lab A (sale + restart persistence): the owner (610600) and the
companion (610601) in a cleared corridor. The companion's bag is
seeded to the declared pressure threshold (14 items, 2 free
slots): 11 stacks of Tough Jerky 117 (sellable), one q0 sword
1896 (protected: a strict upgrade over the empty mainhand), one
quest item 182 (protected) and one q2 sword 15335 (protected:
quality outside the junk matrix). A pinned vendor (entry 21002)
sits 10 yd along the corridor between the companion spawn and the
owner. Bag pressure preempts the follow, the companion approaches
the vendor, sells exactly the declared junk (220c) and keeps the
protected items, then regroups with the owner. The world is
restarted in place and the saved inventory/money must round-trip
unchanged with no vendor activity in the new boot generation.

Lab B (hold preempts cleanup): the same fixture. A hold issued
before the first possible sale tick must freeze the whole cleanup
path (no scan, no sale) until a newer follow order releases it;
the sale then completes and the companion regroups.

Lab C (no-vendor pacing): the same fixture without a vendor, plus
a pinned Kobold decoy (entry 6, loot disabled, 150 HP / regen 0)
so the walk to the owner is not trivially empty. The companion
reports "no vendor" at the declared pace, sells nothing, deletes
nothing, and still regroups.

Lab D (owned-only gate): an ambient bot (610640, no owner
account) with a full bag while a vendor is seeded inside the
declared search radius. The cleanup path must stay completely
silent: zero [Inventory] activity.

In-game verification of the vendor window is PORT-026; these are
log + DB assertions.
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
BAG_START, BAG_END = 23, 39  # InventoryPackSlots (bag0 item slots)

ITEM_FILLER = 117        # Tough Jerky (q1 consumable, stack 20, sell 1c)
ITEM_Q0_SWORD = 1896     # q0 weapon (protected: strict upgrade over the empty mainhand)
ITEM_QUEST = 182         # quest item (class 12, bonded)
ITEM_UPGRADE = 15335     # q2 sword (protected: quality outside the junk matrix)
VENDOR_ENTRY = 21002     # Squire Boltfling (faction 35, level 1, npc_flags 6)

MONEY_START = 100000
MONEY_END = MONEY_START + 11 * 20      # 100220: 11 jerky stacks (the q0 sword is protected)

CORR_X = -8949.95
CORR_Z = 83.5312
# Walkable span proven by test_bot_follow: -152.493 (companion, 32 yd
# walk) to -120.493 (owner). Every spawn and the pinned vendor stay
# inside it; the widened clear corridor removes the base vendor
# (79953) that sits outside the old box but inside the 75 yd scan.
OWNER_Y = -120.493
COMP_Y = -152.493
VENDOR_Y = -142.493
VERMIN_Y = -142.493
AMB_Y = -130.493


def _characters(o, c):
    return (
        "INSERT INTO tw_char.characters\n"
        " (guid,account,name,race,class,gender,level,money,position_x,position_y,position_z,map,\n"
        "  orientation,zone,health,power1,power2,power3,power4,power5,xp,xp_gain)\n"
        "VALUES (%d,1000%d,'Vendowner',1,1,0,10,%d,%.2f,%.3f,%.4f,0,\n"
        "  0,12,100,0,0,0,0,0,0,1),\n"
        "       (%d,1000%d,'Vendcomp',1,1,0,10,%d,%.2f,%.3f,%.4f,0,\n"
        "  0,12,100,0,0,0,0,0,0,1);\n"
        % (o, o, MONEY_START, CORR_X, OWNER_Y, CORR_Z,
           c, c, MONEY_START, CORR_X, COMP_Y, CORR_Z))


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


def _full_bags(c):
    # 14 items in slots 23..36 = 2 free slots, the declared pressure
    # threshold. Sellable: 11 jerky stacks (q1 consumable).
    # Protected: the q0 sword (strict upgrade over the empty
    # mainhand), the quest item and the q2 sword (quality outside
    # the junk matrix).
    #
    # The jerky charges mirror Item::New(), which initializes
    # instance charges from the template (spellcharges_1 = -1).
    # The string must carry all MAX_ITEM_PROTO_SPELLS (5) tokens:
    # Item::LoadFromDB applies charges only when the token count
    # matches exactly, and Item::SaveToDB always writes 5. A
    # 4-token seed leaves the instance charges at 0 and the
    # vendor handler's charge-price multiplier computes 0/-1 =
    # -0.0, zeroing the sale price of every stack.
    inst, inv = [], []
    for i in range(11):
        g = 9000510 + i
        inst.append("(%d,%d,%d,0,0,20,0,'-1 0 0 0 0',0,'',0,0,65535,0,0)"
                    % (g, ITEM_FILLER, c))
        inv.append("(%d,0,%d,%d,%d)" % (c, BAG_START + i, g, ITEM_FILLER))
    for g, tpl, slot in ((9000530, ITEM_Q0_SWORD, 34),
                         (9000540, ITEM_QUEST, 35),
                         (9000550, ITEM_UPGRADE, 36)):
        inst.append("(%d,%d,%d,0,0,1,0,'0 0 0 0 0',0,'',0,0,65535,0,0)"
                    % (g, tpl, c))
        inv.append("(%d,0,%d,%d,%d)" % (c, slot, g, tpl))
    return _instance_rows(inst) + _inventory_rows(inv)


def _clear_corridor():
    return ("DELETE FROM tw_world.creature WHERE map=0 "
            "AND position_x BETWEEN -9035 AND -8865 "
            "AND position_y BETWEEN -285 AND -50;\n")


def _vendor(g):
    return ("INSERT INTO tw_world.creature\n"
            " (guid,id,map,position_x,position_y,position_z,orientation,spawntimesecsmin,\n"
            "  spawntimesecsmax,wander_distance,health_percent,mana_percent,movement_type,spawn_flags)\n"
            "VALUES (%d,%d,0,%.2f,%.3f,%.4f,0,600,600,0,100,100,0,1);\n"
            % (g, VENDOR_ENTRY, CORR_X, VENDOR_Y, CORR_Z))


def _vermin(g):
    return ("UPDATE tw_world.creature_template\n"
            " SET loot_id=0, health_min=150, health_max=150, regeneration=0 WHERE entry=6;\n"
            "INSERT INTO tw_world.creature\n"
            " (guid,id,map,position_x,position_y,position_z,orientation,spawntimesecsmin,\n"
            "  spawntimesecsmax,wander_distance,health_percent,mana_percent,movement_type,spawn_flags)\n"
            "VALUES (%d,6,0,%.2f,%.3f,%.4f,0,600,600,0,100,100,0,1);\n"
            % (g, CORR_X, VERMIN_Y, CORR_Z))


class LabMixin:
    """Shared owner+companion lab driver; mixed into labs A-C."""
    o = c = None
    vendor_guid = None
    vermin_guid = None
    restart = False
    comp_name = "Vendcomp"
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
            "1000:%d:bothold Vendowner" % o,
            "2000:%d:bothold Vendcomp" % o,
            "3000:%d:botrecruit Vendcomp" % o,
            "4000:%d:botfollow Vendcomp" % o,
            "120000:%d:botstop Vendcomp" % o,
        ])

    @classmethod
    def _seed(cls):
        sql = _characters(cls.o, cls.c) + _ownership(cls.o, cls.c)
        sql += _full_bags(cls.c)
        sql += _clear_corridor()
        if cls.vendor_guid:
            sql += _vendor(cls.vendor_guid)
        if cls.vermin_guid:
            sql += _vermin(cls.vermin_guid)
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
        # bag0 slot < BUYBACK_SLOT_START (69 in this fork): sold items
        # persist as buyback rows in the same table and must not
        # count as bag content.
        q = ("SELECT ci.slot, ci.item, ci.item_template FROM tw_char.character_inventory "
             "ci WHERE ci.guid=%d AND ci.bag=0 AND ci.slot < 69 ORDER BY slot" % cls.c)
        rows = []
        for line in p.db_exec(cls.base, cls.env, q).splitlines():
            parts = line.split("\t")
            if len(parts) == 3:
                rows.append((int(parts[0]), int(parts[1]), int(parts[2])))
        return rows

    @classmethod
    def _state(cls):
        money = p.db_int(cls.base, cls.env,
                         "SELECT money FROM characters WHERE guid=%d" % cls.c)
        return (cls._inventory(), money)

    @classmethod
    def setUpClass(cls):
        p.IMAGE = os.environ.get("PORT025_LAB_IMAGE", "tortoise-local:dev")
        try:
            p.command(["docker", "image", "inspect", p.IMAGE])
        except RuntimeError:
            raise unittest.SkipTest("%s image not present; build it first" % p.IMAGE)
        cls.personal_before = cls._personal_state()
        world = p.world_env_for("Vendowner")
        world.update(PLAYERBOT_MIN_BOTS="0", PLAYERBOT_MAX_BOTS="0",
                     PLAYERBOT_REFRESH="600000", PLAYERBOT_UPDATE_MS="1000",
                     PLAYERBOT_PROVISION="",
                     PLAYERBOT_TEST_LOGIN="%d,%d" % (cls.o, cls.c),
                     PLAYERBOT_QUEST_ID="0",
                     PLAYERBOT_WANDER_RADIUS="0.5",
                     PLAYERBOT_PERSONALITY_PROFILE="reckless",
                     PLAYERBOT_FOLLOW_SCRIPT=cls._script(),
                     PLAYER_SAVE_INTERVAL="5000")
        try:
            cls.project = "tortoise-bot-vend-" + uuid.uuid4().hex[:12]
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
            cls.state1 = cls._state()
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
                               and ("[PlayerBot][Login]  '%s' GUID:%d"
                                    % (cls.comp_name, cls.c))
                               in text.rsplit(BOOT_MARKER, cls.boots + 1)[-1],
                               deadline=420)
        end = time.monotonic() + 20
        while time.monotonic() < end:
            time.sleep(3)
        return cls._state()

    @property
    def restart_tail(self):
        return self.logs2.rsplit(BOOT_MARKER, self.boots + 1)[-1]

    def test_all_seeded_bots_logged_in(self):
        for g, n in ((self.o, "Vendowner"), (self.c, self.comp_name)):
            self.assertIn("test-login %d first=1 second=0" % g, self.logs)
            self.assertIn("[PlayerBot][Login]  '%s' GUID:%d" % (n, g), self.logs)
        self.assert_no_real_crashes()

    def test_party_formed(self):
        self.assertIn("party recruit accepted bot:%s guid:%d leader:%d"
                      % (self.comp_name, self.c, self.o), self.logs)

    def test_personal_volume_untouched(self):
        self.assertNotEqual(self.project, "tortoise-local")
        if self.personal_before:
            self.assertEqual(self.personal_after, self.personal_before,
                             "personal server containers must not be restarted or touched")

    def test_regrouped(self):
        reached = [m for m in f.REACHED.findall(self.logs) if m[0] == str(self.c)]
        self.assertTrue(reached, "the companion must regroup with the owner")
        self.assertLessEqual(float(reached[-1][2]), 2.0)


class BotVendorSaleTests(LabMixin, unittest.TestCase):
    """Lab A: declared junk sold to the vendor; restart round-trip."""
    o, c = 610600, 610601
    vendor_guid = 2506001
    restart = True

    @classmethod
    def _drive(cls):
        return p.wait_for(cls.base, cls.env, lambda text:
                          ("[Inventory] pressure cleared GUID:%d" % cls.c in text
                           and any(m[0] == str(cls.c) for m in f.REACHED.findall(text))),
                          deadline=420)

    def test_pressure_reported_once_with_profile(self):
        self.assertEqual(self.logs.count("[Inventory] pressure GUID:%d" % self.c), 1)
        self.assertIn("[Inventory] pressure GUID:%d free:2 stored:0 profile:reckless"
                      % self.c, self.logs)

    def test_vendor_found_and_junk_sold(self):
        self.assertIn("[Inventory] vendor found GUID:%d vendor:%d"
                      % (self.c, self.vendor_guid), self.logs)
        self.assertEqual(self.logs.count(
            "[Inventory] sold GUID:%d item:%d count:20 money:20 vendor:%d"
            % (self.c, ITEM_FILLER, self.vendor_guid)), 11,
            "exactly the 11 jerky stacks must be sold")
        self.assertNotIn("[Inventory] sold GUID:%d item:%d"
                         % (self.c, ITEM_Q0_SWORD), self.logs)
        self.assertNotIn("[Inventory] sold GUID:%d item:%d"
                         % (self.c, ITEM_QUEST), self.logs)
        self.assertNotIn("[Inventory] sold GUID:%d item:%d"
                         % (self.c, ITEM_UPGRADE), self.logs)
        self.assertNotIn("[Inventory] exhausted GUID:%d" % self.c, self.logs)

    def test_protected_items_kept(self):
        inv, money = self.state1
        kept = [r for r in inv if r[2] in (ITEM_Q0_SWORD, ITEM_QUEST, ITEM_UPGRADE)]
        self.assertEqual(len(kept), 3, "protected items must remain: %r" % inv)
        for r in kept:
            self.assertGreaterEqual(r[0], BAG_START)
        self.assertEqual([r for r in inv if r[2] == ITEM_FILLER], [],
                         "all sellable junk must be gone: %r" % inv)

    def test_money_gained(self):
        self.assertEqual(self.state1[1], MONEY_END,
                         "11x20c of declared junk sales: %r" % (self.state1[1],))

    def test_restart_roundtrip(self):
        self.assertEqual(sorted(self.state2[0]), sorted(self.state1[0]),
                         "saved inventory must round-trip the restart unchanged")
        self.assertEqual(self.state2[1], MONEY_END,
                         "saved money must round-trip the restart unchanged")
        self.assertNotIn("[Inventory]", self.restart_tail,
                         "no vendor activity in the new boot generation")


class BotVendorHoldTests(LabMixin, unittest.TestCase):
    """Lab B: a hold freezes the cleanup path until it is released."""
    o, c = 610620, 610621
    vendor_guid = 2506101

    @classmethod
    def _script(cls):
        o = cls.o
        return ";".join([
            "1000:%d:bothold Vendowner" % o,
            "2000:%d:bothold Vendcomp" % o,
            "3000:%d:botrecruit Vendcomp" % o,
            "5000:%d:botfollow Vendcomp" % o,
            # The hold lands before the first possible sale tick (the
            # sale needs the 5 s discovery pace + the approach): it must
            # freeze scan and sale alike until the newer follow releases
            # it.
            "8000:%d:bothold Vendcomp" % o,
            "50000:%d:botfollow Vendcomp" % o,
            "120000:%d:botstop Vendcomp" % o,
        ])

    @classmethod
    def _drive(cls):
        return p.wait_for(cls.base, cls.env, lambda text:
                          ("[Inventory] pressure cleared GUID:%d" % cls.c in text
                           and any(m[0] == str(cls.c) for m in f.REACHED.findall(text))),
                          deadline=420)

    def test_hold_active(self):
        self.assertIn("[PlayerBot][Hold] active GUID:%d" % self.c, self.logs)

    def test_hold_freezes_cleanup(self):
        idx_hold = self.logs.find("[PlayerBot][Hold] active GUID:%d" % self.c)
        self.assertGreater(idx_hold, -1)
        idx_release = self.logs.find("[PlayerBot][Follow] active", idx_hold)
        self.assertGreater(idx_release, -1,
                           "a newer follow order must release the hold")
        segment = self.logs[idx_hold:idx_release]
        for marker in ("[Inventory] sold", "[Inventory] vendor found",
                       "[Inventory] no vendor", "[Inventory] exhausted"):
            self.assertNotIn(marker, segment,
                             "no cleanup activity while held")

    def test_sale_completes_after_release(self):
        idx_hold = self.logs.find("[PlayerBot][Hold] active GUID:%d" % self.c)
        idx_release = self.logs.find("[PlayerBot][Follow] active", idx_hold)
        tail = self.logs[idx_release:]
        self.assertEqual(tail.count(
            "[Inventory] sold GUID:%d item:%d count:20 money:20 vendor:%d"
            % (self.c, ITEM_FILLER, self.vendor_guid)), 11)
        self.assertNotIn("[Inventory] sold GUID:%d item:%d"
                         % (self.c, ITEM_Q0_SWORD), tail)
        self.assertIn("[Inventory] pressure cleared GUID:%d" % self.c, tail)
        inv, money = self.state1
        self.assertEqual(money, MONEY_END)
        self.assertEqual(len([r for r in inv
                              if r[2] in (ITEM_Q0_SWORD, ITEM_QUEST, ITEM_UPGRADE)]), 3)
        self.assertEqual([r for r in inv if r[2] == ITEM_FILLER], [])


class BotVendorNoVendorTests(LabMixin, unittest.TestCase):
    """Lab C: no vendor in radius -> paced reports, nothing sold."""
    o, c = 610630, 610631
    vermin_guid = 2506201

    @classmethod
    def _drive(cls):
        return p.wait_for(cls.base, cls.env, lambda text:
                          ("[Inventory] no vendor GUID:%d" % cls.c in text
                           and any(m[0] == str(cls.c) for m in f.REACHED.findall(text))),
                          deadline=420)

    def test_no_vendor_reported_paced(self):
        n = self.logs.count("[Inventory] no vendor GUID:%d radius:75" % self.c)
        self.assertGreaterEqual(n, 1, "at least the first failed scan reports")
        self.assertLessEqual(n, 3, "reports are paced to the 30 s window")
        self.assertEqual(self.logs.count("[Inventory] pressure GUID:%d" % self.c), 1)

    def test_nothing_sold_nothing_deleted(self):
        for marker in ("[Inventory] vendor found", "[Inventory] sold",
                       "[Inventory] exhausted", "[Inventory] pressure cleared"):
            self.assertNotIn(marker, self.logs)
        inv, money = self.state1
        self.assertEqual(money, MONEY_START)
        self.assertEqual(len(inv), 14, "the bag must be untouched: %r" % inv)
        self.assertEqual(len([r for r in inv if r[2] == ITEM_FILLER]), 11)
        self.assertEqual(len([r for r in inv if r[2] == ITEM_Q0_SWORD]), 1)


class BotVendorAmbientGateTests(unittest.TestCase):
    """Lab D: the owned-only gate keeps the cleanup path silent."""
    a = 610640
    vendor_guid = 2506301
    base = env = project = evidence = None
    logs = ""
    personal_before = {}
    personal_after = {}

    @classmethod
    def _seed(cls):
        return (
            "INSERT INTO tw_char.characters\n"
            " (guid,account,name,race,class,gender,level,money,position_x,position_y,position_z,map,\n"
            "  orientation,zone,health,power1,power2,power3,power4,power5,xp,xp_gain)\n"
            "VALUES (%d,1000%d,'Vendamb',1,1,0,10,%d,%.2f,%.3f,%.4f,0,\n"
            " 0,12,100,0,0,0,0,0,0,1);\n"
            % (cls.a, cls.a, MONEY_START, CORR_X, AMB_Y, CORR_Z)
            + "INSERT INTO tw_char.playerbot (char_guid,chance,ai) VALUES (%d,100,'Default');\n"
            "INSERT INTO tw_char.bot_ownership (char_guid,account_id,bot_type,provision_version,owner_account_id)\n"
            " VALUES (%d,1000%d,1,2,NULL);\n" % (cls.a, cls.a, cls.a)
            + _full_bags(cls.a) + _clear_corridor() + _vendor(cls.vendor_guid))

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
        p.IMAGE = os.environ.get("PORT025_LAB_IMAGE", "tortoise-local:dev")
        try:
            p.command(["docker", "image", "inspect", p.IMAGE])
        except RuntimeError:
            raise unittest.SkipTest("%s image not present; build it first" % p.IMAGE)
        cls.personal_before = cls._personal_state()
        world = p.world_env_for("Vendamb")
        world.update(PLAYERBOT_MIN_BOTS="0", PLAYERBOT_MAX_BOTS="0",
                     PLAYERBOT_REFRESH="600000", PLAYERBOT_UPDATE_MS="1000",
                     PLAYERBOT_PROVISION="",
                     PLAYERBOT_TEST_LOGIN=str(cls.a),
                     PLAYERBOT_QUEST_ID="0",
                     PLAYERBOT_WANDER_RADIUS="0.5",
                     PLAYER_SAVE_INTERVAL="5000")
        try:
            cls.project = "tortoise-bot-vendamb-" + uuid.uuid4().hex[:12]
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
            end = time.monotonic() + 25
            while time.monotonic() < end:
                time.sleep(3)
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

    def test_login_and_no_crashes(self):
        self.assertIn("test-login %d first=1 second=0" % self.a, self.logs)
        crashes = [l for l in self.logs.splitlines()
                   if "[CRASH]" in l and "already spawned Gobj" not in l]
        self.assertFalse(crashes, "world crashed: %r" % crashes[:3])

    def test_cleanup_path_silent_for_ambient(self):
        self.assertNotIn("[Inventory]", self.logs,
                         "the owned-only gate must keep the cleanup path "
                         "silent for ambient bots")

    def test_personal_volume_untouched(self):
        self.assertNotEqual(self.project, "tortoise-local")
        if self.personal_before:
            self.assertEqual(self.personal_after, self.personal_before,
                             "personal server containers must not be restarted or touched")


if __name__ == "__main__":
    unittest.main()
