"""PORT-034 (KAP-558): provision a bounded companion cohort; per-class skills.

Phase 1 (provisioning):
  One disposable world provisions four named companions from a single
  PLAYERBOT_PROVISION list (short spec Name,race,class,gender):
    Bram,1,1,0    Human warrior male
    Rowan,1,3,0   Human hunter male
    Elowen,10,8,1 High elf mage female
    Clem,3,5,0    Dwarf priest male
  Asserted: four characters (unique guids, reserved bot accounts, the
  requested race/class/gender, level 1), phase-2 provision markers,
  provision_version-2 ownership, roster rows, and the four "created
  native character" log lines. No other characters exist in the lab DB.

Phase 2 (cooperative run + idempotent re-provision):
  Same project, world restarted with the SAME provision list (must be
  four "already provisioned; idempotent no-op" lines, no new creates)
  plus the Lab D quest geometry: owner (level-3 human warrior) accepts
  456 at the giver (2079) via the lab quest script; Rowan, Elowen and
  Clem (test-login) mirror-accept at the giver, fight the 7+4 kill
  pack through distributed botassist events, and mirror turn in at the
  finisher (6911). The owner fixture self-binds and self-holds at
  0 s (the Lab D pattern) so it stays anchored at spawn: an unbound
  owner is rejected by bothold and its AI takes the legacy idle
  wander, a ~1 yd/step random walk that drifts ~5 yd off spawn
  inside the kill window and breaks the 30 yd assist/turn-in radii
  (run #5 root cause). The fixture reuses the quest-coop
  neutralization
  (ranged columns included) with the post-seed self-assert.
  Asserted: quest rows complete (1:1:7:4) or rewarded (row gone);
    each companion level >= 2 (300 XP
  pre-grant + 170 quest XP + kill credit crosses the 400 XP level-1
  threshold); one mirror-accept (anchor 2079) and one mirror turn-in
  (anchor 6911) per companion; per-class skill learning below.

Skill assertions (empirical, no hardcoded spell ids - the fork's
spell numbering is pinned and data-specific):
  - each companion persists >= 3 learned spells (character_spell)
    after login;
  - the three companions' spell sets are pairwise distinct (their
    classmask/racemask filters differ);
  - the post-run set is a superset of the pre-run set per companion
    (monotone learning).
  The learned set grows with level in this fork's data: class-masked
  rows are gated by spell_template.spellLevel (observed spread 0-60,
  e.g. Heroic Strike/Fireball 1, Charge/Frostbolt 4, Hamstring 32),
  and req_skill_value gates only require the skill line to exist
  (the learner raises it, PORT-028). The lab's 1->2 level-ups cross
  no level-4 gate, so growth is asserted as monotonicity
  (non-shrinking), not strict increase.

No human play data, no personal-world restart, no personal containers.
Retrieval sync not performed (embedding paused per operator instruction);
direct-read fallback only.
"""
import os
import time
import unittest
import uuid
from datetime import datetime, timezone

import test_bot_provision as p
import test_bot_follow as f
from test_bot_quest_coop import _quest_row, _assert_fixture_templates, GIVER_NEUTRAL, PACK_ZERO

QUEST_ID = "456"
GIVER_ENTRY = 2079
FINISHER_ENTRY = 6911

# Roster: name -> (race, class, gender). Verified against this fork's
# ChrRaces/ChrClasses DBCs: race 1 Human, 3 Dwarf, 4 Night Elf; class
# 1 Warrior, 3 Hunter, 5 Priest, 8 Mage.
BOTS = {
    "Bram": (1, 1, 0),
    "Rowan": (1, 3, 0),
    "Elowen": (10, 8, 1),
    "Clem": (3, 5, 0),
}
PROVISION = ";".join("%s,%d,%d,%d" % ((n,) + BOTS[n]) for n in BOTS)
# The three companions that fight the quest in phase 2 (Bram stays on
# the bench; he is the rename target of the existing personal bot).
COHORT = ("Rowan", "Elowen", "Clem")

O = 610640
ONAME = "Cohowner"
GIVER_GUID = 2500100
FINISHER_GUID = 2500101
SABER_GUIDS = [2500102, 2500103, 2500104, 2500105, 2500106, 2500107, 2500108]
BOAR_GUIDS = [2500109, 2500110, 2500111, 2500112]

# Lab D geometry (verified there): every mob within 4 yd of the
# finisher, within 27 yd of the anchored owner (30 yd assist
# radius; the farthest boars sit 26.5 yd out, leaving 3.5 yd for
# the follow offset - the owner must not drift: it self-binds
# and self-holds at 0 s, see the phase-2 seed).
SABER_POS = [(-8959.0, -148.5), (-8959.5, -151.0), (-8960.0, -153.5),
             (-8961.5, -148.0), (-8962.5, -149.0), (-8963.5, -152.5),
             (-8961.0, -150.0)]
BOAR_POS = [(-8964.0, -149.0), (-8964.5, -151.5), (-8963.5, -154.0),
            (-8962.0, -155.0)]
# Companion start points: ~68-70 yd south of the owner, inside the
# cleared corridor, one point each (no spawn overlap).
BOT_POS = {
    "Rowan": (-8949.95, -200.493),
    "Elowen": (-8948.95, -201.493),
    "Clem": (-8947.95, -202.493),
}
XP_PREGRANT = 300  # +170 quest XP + kill credit crosses the 400 XP L1->L2 bar


def _spells(base, env, guid):
    out = p.db_exec(base, env,
                    "SELECT spell FROM tw_char.character_spell WHERE guid=%d" % guid)
    return frozenset(int(x) for x in out.split() if x)


def _char_info(base, env, name):
    out = p.db_exec(base, env,
                    "SELECT guid, account, race, class, gender, level, xp, "
                    "position_x, position_y FROM tw_char.characters "
                    "WHERE name='%s'" % name)
    out = out.strip()
    if not out:
        return None
    parts = out.split("\t")
    if len(parts) != 9:
        return "bad"
    return {"guid": int(parts[0]), "account": int(parts[1]),
            "race": int(parts[2]), "class": int(parts[3]), "gender": int(parts[4]),
            "level": int(parts[5]), "xp": int(parts[6]),
            "pos": (parts[7], parts[8])}


class BotCompanionCohortTests(unittest.TestCase):
    base = env = project = evidence = None
    log1 = ""            # phase-1 world log (accumulated, full)
    log2 = ""            # phase-2 world log (accumulated across generations)
    guids = {}           # name -> guid (discovered in phase 1)
    chars = {}           # name -> _char_info snapshot (phase 1)
    spells_pre = {}      # name -> frozenset (after phase-2 login)
    spells_post = {}     # name -> frozenset (after the run)
    before = None

    @classmethod
    def _phase2_seed(cls, guids):
        r, e, c = guids["Rowan"], guids["Elowen"], guids["Clem"]
        sabers = ["(%d,2031,0,%.2f,%.2f,83.5312,0,600,600,0,100,100,0,1)"
                  % (g, x, y) for g, (x, y) in zip(SABER_GUIDS, SABER_POS)]
        boars = ["(%d,1984,0,%.2f,%.2f,83.5312,0,600,600,0,100,100,0,1)"
                 % (g, x, y) for g, (x, y) in zip(BOAR_GUIDS, BOAR_POS)]
        return (
            "INSERT INTO tw_char.characters\n"
            " (guid,account,name,race,class,gender,level,money,position_x,position_y,position_z,map,\n"
            "  orientation,zone,health,power1,power2,power3,power4,power5,xp,xp_gain)\n"
            "VALUES\n"
            " (%d,1000%d,'%s',1,1,0,3,100000,-8947.95,-132.493,83.5312,0,\n"
            "  0,12,100,0,0,0,0,0,0,1);\n"
            "INSERT INTO tw_char.playerbot (char_guid,chance,ai)\n"
            " VALUES (%d,100,'Default');\n"
            "INSERT INTO tw_char.bot_ownership (char_guid,account_id,bot_type,provision_version,owner_account_id)\n"
            # Owner self-binds (owner_account_id = its own lab
            # account), the Lab D pattern: a NULL owner rejects the
            # 0 s bothold ("hold rejected unowned bot") and leaves
            # the owner on the legacy idle-wander branch (it is not
            # an owned companion), a ~1 yd/step random walk that
            # drifts ~5 yd off spawn inside the kill window (run #5
            # target-dead root cause). Self-bound, the hold arms at
            # 0 s and pins the owner at spawn for the whole run.
            " VALUES (%d,1000%d,1,2,1000%d);\n"
            # Owner fixture rows only; the cohort companions already have
            # their provisioned roster/ownership rows.
            "UPDATE tw_world.creature_template SET faction=1, dmg_min=0, dmg_max=0, "
            "ranged_dmg_min=0, ranged_dmg_max=0, detection_range=0 WHERE entry=2079;\n"
            "UPDATE tw_world.creature_template SET dmg_min=0, dmg_max=0, ranged_dmg_min=0, "
            "ranged_dmg_max=0, health_min=8, health_max=8 WHERE entry IN (2031,1984);\n"
            "DELETE FROM tw_world.creature_questrelation WHERE quest=456;\n"
            "DELETE FROM tw_world.creature_involvedrelation WHERE quest=456;\n"
            "INSERT INTO tw_world.creature_questrelation (id, quest) VALUES (2079, 456);\n"
            "INSERT INTO tw_world.creature_involvedrelation (id, quest) VALUES (6911, 456);\n"
            "UPDATE tw_world.creature_template SET npc_flags = npc_flags | 2 WHERE entry=6911;\n"
            # The finisher takes faction 1 like the giver: base faction
            # 14 (Monster) fails the CanInteractWithNPC hostility gate
            # (faction_template mask fallback: 1 & 3), so the mirror
            # turn-in would be denied even at 0 yd.
            "UPDATE tw_world.creature_template SET faction=1, dmg_min=0, dmg_max=0, "
            "ranged_dmg_min=0, ranged_dmg_max=0, detection_range=0, "
            "health_min=99999, health_max=99999 WHERE entry=6911;\n"
            "INSERT INTO tw_world.creature\n"
            " (guid,id,map,position_x,position_y,position_z,orientation,spawntimesecsmin,\n"
            "  spawntimesecsmax,wander_distance,health_percent,mana_percent,movement_type,spawn_flags)\n"
            "VALUES\n"
            " (%d,2079,0,-8945.95,-132.493,83.5312,0,600,600,0,100,100,0,1),\n"
            " (%d,6911,0,-8962.0,-151.0,83.5312,0,600,600,0,100,100,0,1),\n"
            % (O, O, ONAME, O, O, O, O, GIVER_GUID, FINISHER_GUID)
            + ",\n".join(sabers + boars) + ";\n"
            "DELETE FROM tw_world.creature WHERE map=0 AND position_x BETWEEN -8990 AND -8910\n"
            " AND position_y BETWEEN -230 AND -100 AND guid NOT IN (%s);\n"
            % ",".join(str(g) for g in [GIVER_GUID, FINISHER_GUID]
                       + SABER_GUIDS + BOAR_GUIDS)
            # Party commands reject unowned bots; provisioning leaves the
            # cohort ownerless, so bind the three to the lab owner account.
            + "UPDATE tw_char.bot_ownership SET owner_account_id=1000%d "
              "WHERE char_guid IN (%d,%d,%d);\n" % (O, r, e, c)
        )

    @classmethod
    def _follow_script(cls):
        ev = [
            "0:%d:bothold %s" % (O, ONAME),
            "3000:%d:bothold Rowan" % O,
            "5000:%d:bothold Elowen" % O,
            "7000:%d:bothold Clem" % O,
            "10000:%d:botrecruit Rowan" % O,
            "12000:%d:botrecruit Elowen" % O,
            "14000:%d:botrecruit Clem" % O,
            "20000:%d:botfollow Rowan" % O,
            "22000:%d:botfollow Elowen" % O,
            "24000:%d:botfollow Clem" % O,
            # All three arrive at the owner (giver area); hold re-pins.
            "60000:%d:bothold Rowan" % O,
            "61000:%d:bothold Elowen" % O,
            "62000:%d:bothold Clem" % O,
            # Follow releases the hold; the owner accepts at 65 s (quest
            # script) and each mirror accept fires on the next tick.
            "70000:%d:botfollow Rowan" % O,
            "71000:%d:botfollow Elowen" % O,
            "72000:%d:botfollow Clem" % O,
        ]
        # Kill phase: 7 sabers + 4 boars across the three classes so each
        # companion fights and earns kill credit/XP.
        kills = [
            ("Rowan", "Young Nightsaber"),
            ("Elowen", "Young Nightsaber"),
            ("Clem", "Young Nightsaber"),
            ("Rowan", "Young Nightsaber"),
            ("Elowen", "Young Nightsaber"),
            ("Clem", "Young Nightsaber"),
            ("Rowan", "Young Nightsaber"),
            ("Rowan", "Young Thistle Boar"),
            ("Elowen", "Young Thistle Boar"),
            ("Clem", "Young Thistle Boar"),
            ("Rowan", "Young Thistle Boar"),
        ]
        t = 90000
        for bot, mob in kills:
            ev.append("%d:%d:botassist %s %s" % (t, O, bot, mob))
            t += 20000
        # Safety net (run #5): re-issue the two boar assists that can
        # miss the 30 yd companion-centered name radius; each is a
        # no-op target-dead when the original already landed.
        ev.append("%d:%d:botassist Clem Young Thistle Boar" % (t - 15000, O))
        ev.append("%d:%d:botassist Rowan Young Thistle Boar" % (t - 10000, O))
        # When the last credit lands the owner's quest is COMPLETE while
        # the non-killer companions still stand at the anchored owner
        # (~24 yd from the finisher): the v2 turn-in walk arms on the
        # next quest tick and must finish before the hold pins the pack.
        # Timeline: ~303 s credit -> ~304 s arm -> ~315 s arrival ->
        # mirror turn-ins -> 330 s hold, then walk back out well after
        # the owner's 400 s turn-in.
        ev.append("%d:%d:bothold Rowan" % (t + 20000, O))
        ev.append("%d:%d:bothold Elowen" % (t + 21000, O))
        ev.append("%d:%d:bothold Clem" % (t + 22000, O))
        ev.append("%d:%d:botfollow Rowan" % (t + 150000, O))
        ev.append("%d:%d:botfollow Elowen" % (t + 151000, O))
        ev.append("%d:%d:botfollow Clem" % (t + 152000, O))
        ev.append("%d:%d:bothold Rowan" % (t + 180000, O))
        ev.append("%d:%d:bothold Elowen" % (t + 181000, O))
        ev.append("%d:%d:bothold Clem" % (t + 182000, O))
        return ";".join(line if ":" in line else "%d:%s" % (O, line) for line in ev)

    @classmethod
    def setUpClass(cls):
        p.IMAGE = os.environ.get("PORT034_LAB_IMAGE", "tortoise-local:dev")
        try:
            p.command(["docker", "image", "inspect", p.IMAGE])
        except RuntimeError:
            raise unittest.SkipTest("%s image not present; build it first" % p.IMAGE)
        cls.before = f.BotFollowTests._personal_state()
        cls.project = "tortoise-bot-cohort-" + uuid.uuid4().hex[:12]
        cls.evidence = p.ROOT / "local" / (cls.project + "-"
                                           + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ"))
        cls.evidence.mkdir(parents=True)
        world = p.world_env_for("")
        world.update(PLAYERBOT_ENABLE="1", PLAYERBOT_MIN_BOTS="0", PLAYERBOT_MAX_BOTS="0",
                     PLAYERBOT_REFRESH="600000", PLAYERBOT_UPDATE_MS="1000", PLAYERBOT_DEBUG="1",
                     PLAYERBOT_PROVISION=PROVISION, PLAYERBOT_TEST_LOGIN="",
                     PLAYERBOT_AMBIENT_ACQUIRE="0", PLAYERBOT_WANDER_RADIUS="1",
                     PLAYERBOT_QUEST_ID="0", PLAYERBOT_COOPERATIVE_QUEST_ID="0",
                     PLAYERBOT_MIRROR_OWNER_QUESTS="0",
                     PLAYER_SAVE_INTERVAL="5000")
        cls.base = cls.env = None
        try:
            # --- Phase 1: provision the cohort (no logins, no bots online)
            cls.base, cls.env = p.boot_lab(cls.project, cls.evidence, world, None)
            p.command(["docker", "compose"] + cls.base + ["up", "-d", "--no-deps", "world"],
                      env=cls.env)
            deadline = time.monotonic() + 300
            while time.monotonic() < deadline:
                missing = [n for n in BOTS if _char_info(cls.base, cls.env, n) is None]
                if not missing:
                    break
                time.sleep(5)
            else:
                raise RuntimeError("cohort not provisioned within deadline; missing %s"
                                   % missing)
            for name in BOTS:
                cls.chars[name] = _char_info(cls.base, cls.env, name)
                cls.guids[name] = cls.chars[name]["guid"]
            cls.log1 = p.command(["docker", "compose"] + cls.base
                                 + ["logs", "--no-color", "world"], env=cls.env, timeout=60)
            (cls.evidence / "phase1.log").write_text(cls.log1, encoding="utf-8")
            # Clean stop, then the phase-2 fixture (XP + travel positions).
            p.command(["docker", "compose"] + cls.base + ["stop", "world"],
                      env=cls.env, timeout=180)
            for n in COHORT:
                x, y = BOT_POS[n]
                p.db_exec(cls.base, cls.env,
                          "UPDATE tw_char.characters SET xp=%d, position_x=%.3f, "
                          "position_y=%.3f, position_z=83.5312, map=0, zone=12 "
                          "WHERE name='%s';\n" % (XP_PREGRANT, x, y, n))
        except BaseException:
            cls._teardown_failed()
            raise

        # --- Phase 2: same provision list (idempotency) + quest run ---
        try:
            world2 = p.world_env_for("")
            world2.update(PLAYERBOT_ENABLE="1", PLAYERBOT_MIN_BOTS="0", PLAYERBOT_MAX_BOTS="0",
                          PLAYERBOT_REFRESH="600000", PLAYERBOT_UPDATE_MS="1000", PLAYERBOT_DEBUG="1",
                          PLAYERBOT_PROVISION=PROVISION,
                          PLAYERBOT_TEST_LOGIN="%d,%d,%d,%d"
                                               % (O, cls.guids["Rowan"], cls.guids["Elowen"],
                                                  cls.guids["Clem"]),
                          PLAYERBOT_AMBIENT_ACQUIRE="0", PLAYERBOT_WANDER_RADIUS="1",
                          PLAYERBOT_QUEST_ID="0", PLAYERBOT_COOPERATIVE_QUEST_ID="0",
                          PLAYERBOT_MIRROR_OWNER_QUESTS="1",
                          PLAYER_SAVE_INTERVAL="5000",
                          PLAYERBOT_FOLLOW_SCRIPT=cls._follow_script(),
                          PLAYERBOT_QUEST_SCRIPT=("65000:%d:%s:accept;"
                                                  "400000:%d:%s:turnin;"
                                                  "480000:%d:%s:turnin;"
                                                  "560000:%d:%s:turnin;"
                                                  "640000:%d:%s:turnin")
                                                % (O, QUEST_ID, O, QUEST_ID,
                                                   O, QUEST_ID, O, QUEST_ID,
                                                   O, QUEST_ID))
            p.write_compose(cls.evidence, world2)
            cls._phase2_apply_seed()
            p.command(["docker", "compose"] + cls.base
                      + ["up", "-d", "--no-deps", "--force-recreate", "world"],
                      env=cls.env, timeout=300)
            cls.log2 = p.wait_for(cls.base, cls.env,
                                  lambda text: "test-login %d first=1" % O in text
                                  and "World server is up and running!" in text,
                                  deadline=600)
            # Pre-run skill snapshot: all three cohort companions logged
            # in (learned at OnPlayerLogin, persisted by the 5 s save).
            deadline = time.monotonic() + 120
            while time.monotonic() < deadline:
                if all(len(_spells(cls.base, cls.env, cls.guids[n])) >= 3 for n in COHORT):
                    break
                time.sleep(5)
            for n in COHORT:
                cls.spells_pre[n] = _spells(cls.base, cls.env, cls.guids[n])
            # The run: owner turn-in is the last scripted act (400 s,
            # retried through 640 s by the quest script).
            cls.log2 = p.wait_for(cls.base, cls.env,
                                  lambda text: "[PlayerBot][QuestScript] turnin issuer:%d quest:%s"
                                               % (O, QUEST_ID) in text,
                                  deadline=900)
            # Let level-ups/save settle, then the post-run snapshot.
            time.sleep(15)
            for n in COHORT:
                cls.spells_post[n] = _spells(cls.base, cls.env, cls.guids[n])
            # Poll the persisted quest rows (5 s save cadence).
            deadline = time.monotonic() + 60
            while time.monotonic() < deadline:
                if all(_quest_row(cls.base, cls.env, g) == "1:1:7:4"
                       for g in [O] + [cls.guids[n] for n in COHORT]):
                    break
                time.sleep(5)
            cls.log2 = p.command(["docker", "compose"] + cls.base
                                 + ["logs", "--no-color", "world"], env=cls.env, timeout=60)
            (cls.evidence / "phase2.log").write_text(cls.log2, encoding="utf-8")
        except BaseException:
            if cls.base is not None:
                try:
                    failure_logs = p.command(["docker", "compose"] + cls.base
                                             + ["logs", "--no-color", "world"],
                                             env=cls.env, timeout=60)
                    (cls.evidence / "world-failure.log").write_text(failure_logs, encoding="utf-8")
                except Exception:
                    pass
            p.force_down(cls.base, cls.env)
            raise

    @classmethod
    def _phase2_apply_seed(cls):
        # The world is stopped; apply the phase-2 fixture directly.
        p.db_exec(cls.base, cls.env, cls._phase2_seed(cls.guids))
        _assert_fixture_templates(
            cls.base, cls.env,
            {2079: GIVER_NEUTRAL, 2031: PACK_ZERO, 1984: PACK_ZERO,
             6911: {"dmg_min": 0, "dmg_max": 0, "ranged_dmg_min": 0,
                    "ranged_dmg_max": 0, "detection_range": 0,
                    "health_min": 99999, "health_max": 99999}})

    @classmethod
    def _teardown_failed(cls):
        if cls.base is not None:
            try:
                failure_logs = p.command(["docker", "compose"] + cls.base
                                         + ["logs", "--no-color", "world"],
                                         env=cls.env, timeout=60)
                (cls.evidence / "world-failure.log").write_text(failure_logs, encoding="utf-8")
            except Exception:
                pass
            p.force_down(cls.base, cls.env)

    @classmethod
    def tearDownClass(cls):
        if cls.base is not None:
            p.teardown_lab(cls.base, cls.env, cls.project, cls.evidence)

    # --- Phase 1 assertions -------------------------------------------

    def test_phase1_four_characters(self):
        self.assertEqual(len(self.guids), 4)
        guids = set(self.guids.values())
        self.assertEqual(len(guids), 4, "guids must be unique")
        for name, (race, cls_, gender) in BOTS.items():
            info = self.chars[name]
            self.assertNotEqual(info, "bad", name)
            self.assertEqual((info["race"], info["class"], info["gender"]),
                             (race, cls_, gender), name)
            self.assertEqual(info["level"], 1, name)
            self.assertGreaterEqual(info["account"], 1000000000, name)

    def test_phase1_only_cohort_exists(self):
        # The final DB also contains the Cohowner created by phase
        # 2, so assert the exact name set instead of a raw count.
        out = p.db_exec(self.base, self.env,
                        "SELECT name FROM characters ORDER BY name")
        names = set(x.strip() for x in out.splitlines() if x.strip())
        self.assertEqual(names, set(BOTS) | {ONAME})

    def test_phase1_provision_markers(self):
        out = p.db_exec(self.base, self.env,
                        "SELECT character_name, phase, race_id, class_id, gender_id "
                        "FROM bot_provision_state ORDER BY character_name")
        rows = [tuple(x.split("\t")) for x in out.splitlines() if x]
        self.assertEqual(len(rows), 4)
        for name, phase, race, cls_, gender in rows:
            r, c, g = BOTS[name]
            self.assertEqual((phase, int(race), int(cls_), int(gender)),
                             ("2", r, c, g), name)

    def test_phase1_ownership_and_roster(self):
        out = p.db_exec(self.base, self.env,
                        "SELECT b.char_guid, b.account_id, b.provision_version, "
                        "c.name, b.owner_account_id, "
                        "(SELECT COUNT(*) FROM playerbot p WHERE p.char_guid=b.char_guid) "
                        "FROM bot_ownership b JOIN characters c ON c.guid=b.char_guid")
        rows = [tuple(x.split("\t")) for x in out.splitlines() if x]
        # Four provisioned companions plus the phase-2 owner self-bind
        # row (run #5 root-cause fix: an unbound owner rejects the 0 s
        # bothold and its legacy wander drifts it off spawn; see the
        # phase-2 seed).
        self.assertEqual(len(rows), 5)
        for guid, account, version, name, owner, roster in rows:
            if name == ONAME:
                self.assertEqual(int(guid), O, name)
                self.assertEqual(int(account), int("1000" + str(O)), name)
                self.assertEqual((version, owner, roster),
                                 ("2", str(int("1000" + str(O))), "1"), name)
                continue
            self.assertEqual(int(guid), self.guids[name])
            self.assertEqual(int(account), self.chars[name]["account"])
            self.assertEqual((version, roster), ("2", "1"), name)

    def test_phase1_created_lines(self):
        for name in BOTS:
            self.assertIn("Playerbot provisioning: created native character '%s' (guid %d account %d)"
                          % (name, self.guids[name], self.chars[name]["account"]),
                          self.log1)

    # --- Phase 2 assertions -------------------------------------------

    def test_phase2_idempotent_reprovision(self):
        self.assertEqual(self.log2.count("already provisioned; idempotent no-op"), 4)
        self.assertEqual(self.log2.count("created native character"), 0)

    def test_phase2_all_logged_in(self):
        for guid in [O] + [self.guids[n] for n in COHORT]:
            self.assertIn("test-login %d first=1 second=0" % guid, self.log2)

    def test_phase2_four_quest_rows(self):
        # A rewarded quest leaves no row ("-"); a complete-but-
        # pending row reads 1:1:7:4. Any other value means the
        # quest stuck.
        for guid in [O] + [self.guids[n] for n in COHORT]:
            row = _quest_row(self.base, self.env, guid)
            self.assertIn(row, ("1:1:7:4", "-"),
                          "quest row for guid %d: %r" % (guid, row))

    def test_phase2_mirror_accept_per_companion(self):
        for n in COHORT:
            line = "[CoopQuest] mirror-accepted GUID:%d quest:%s anchor:%d" % (
                self.guids[n], QUEST_ID, GIVER_ENTRY)
            self.assertEqual(self.log2.count(line), 1, n)

    def test_phase2_mirror_turnin_per_companion(self):
        for n in COHORT:
            prefix = "[CoopQuest] mirror-turnin GUID:%d quest:%s anchor:%d " % (
                self.guids[n], QUEST_ID, FINISHER_ENTRY)
            self.assertEqual(self.log2.count(prefix), 1, n)

    def test_phase2_owner_accept_and_turnin(self):
        self.assertEqual(
            self.log2.count("[PlayerBot][QuestScript] accept issuer:%d quest:%s "
                            "giver:%d ok:1" % (O, QUEST_ID, GIVER_ENTRY)), 1)
        self.assertEqual(
            self.log2.count("[PlayerBot][QuestScript] turnin issuer:%d quest:%s"
                            % (O, QUEST_ID)), 1)

    def test_phase2_companions_level2(self):
        for n in COHORT:
            level = p.db_int(self.base, self.env,
                             "SELECT level FROM characters WHERE guid=%d" % self.guids[n])
            self.assertGreaterEqual(level, 2, n)

    def test_phase2_skill_learning_counts(self):
        for n in COHORT:
            self.assertGreaterEqual(len(self.spells_pre[n]), 3,
                                    "%s learned %d spells at level 1"
                                    % (n, len(self.spells_pre[n])))

    def test_phase2_skill_sets_distinct(self):
        for i, a in enumerate(COHORT):
            for b in COHORT[i + 1:]:
                self.assertNotEqual(self.spells_pre[a], self.spells_pre[b],
                                    "%s and %s share an identical spell set" % (a, b))

    def _spell_rank_info(self, ids):
        # spell id -> (name, spellLevel) from the lab world DB (a copy
        # of the personal world data); used by the rank-aware
        # monotonicity check below.
        info = {}
        ordered = sorted(ids)
        for i in range(0, len(ordered), 100):
            chunk = ordered[i:i + 100]
            out = p.db_exec(self.base, self.env,
                            "SELECT entry, name, spellLevel FROM tw_world.spell_template "
                            "WHERE entry IN (%s)" % ",".join(str(x) for x in chunk))
            for line in out.splitlines():
                if not line.strip():
                    continue
                entry, name, lvl = line.split("\t")
                info[int(entry)] = (name, int(lvl))
        return info

    def test_phase2_skill_sets_monotone(self):
        # Rank-aware monotonicity. In this fork's data (Talent.dbc)
        # class/racial abilities such as High Elf Critical Mass are
        # talent families: learning a rank through AddSpell makes the
        # engine unlearn the other ranks of the same talent
        # (Player.cpp AddSpell talent block), so the learned set may
        # legitimately lose a lower rank when a rank of the same
        # ability is learned at a level-up (run #10 [SpellTrace]:
        # level-2 AutoLearn of Critical Mass rank 2, spell 11368,
        # unlearned rank 1, spell 11115 - the character keeps the
        # ability at its highest learned rank). A lost spell is
        # acceptable only when a gained spell with the same name at a
        # spellLevel >= replaces it; any other shrinkage is a
        # regression.
        allids = set()
        for n in COHORT:
            allids |= set(self.spells_pre[n]) | set(self.spells_post[n])
        info = self._spell_rank_info(allids)
        for n in COHORT:
            pre = self.spells_pre[n]
            post = self.spells_post[n]
            gained = post - pre
            for lost_id in sorted(pre - post):
                lname, llvl = info.get(lost_id, ("?", -1))
                replaced = any(
                    info.get(g, ("?", -1))[0] == lname
                    and info.get(g, ("?", -1))[1] >= llvl
                    for g in gained)
                self.assertTrue(replaced,
                                "%s lost spell %d (%s, spellLevel %d) without a "
                                "same-ability replacement at level >= %d; lost=%s gained=%s"
                                % (n, lost_id, lname, llvl, llvl,
                                   sorted(pre - post), sorted(gained)))

    def test_world_healthy(self):
        self.assertNotIn("[CRASH]", self.log1)
        self.assertNotIn("[CRASH]", self.log2)

    def test_personal_state_untouched(self):
        self.assertEqual(self.before, f.BotFollowTests._personal_state())


if __name__ == "__main__":
    unittest.main()
