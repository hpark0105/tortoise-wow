"""PORT-008: bounded pursuit (leash) + owner-loss recovery for one companion.

One disposable, port-free lab in the combat-proven corridor.

The owner (610200) and the companion (610201, bound to the owner, 70 yd
south) are pinned, recruited, and put on follow. At tau20 the owner
orders an assist on a Kobold Vermin (2500200, entry 6, pinned 150 HP /
regen 0 / no loot) that hovers 20 yd ABOVE ground level, 30 yd south of
the owner. The companion reaches it mid-walk (the assist lookup is 30 yd
raw 3D and the owner stays beyond the 31.2 yd 3D aggro distance). The
target's 20 yd vertical offset is beyond melee Z-reach forever, and the
fixture pins the creature (extra_flags NO_AGGRO|FIXED_Z|NO_TARGET) so it
can never acquire a target, descend, or move: a deterministic "valid but
unreachable" hostile. The companion chases, never lands a hit, and the
30 s pursuit leash (kPursuitLeashMs) expires; the assist is abandoned
and the follow goal regroups the companion to the owner.

At tau68 (offsets relative to the owner's own login, via
PlayerBot.TestLogoutScript) the owner logs out: the 2-person party
disbands (a group cannot survive its leader leaving) and the companion
must hold safe - no offensive lines, position frozen - until the owner
relogs at tau80. At tau105 the owner re-issues follow (BotFollow is
owner-gated only, so it succeeds on the now-ungrouped pair) and the
companion reaches immediately; tau120 stops it.

Acceptance: assist accepted, leash armed + expired with the budget
honored (the 2 s cadence "[Assist] fighting" lines between them bound
the elapsed window), the post-leash [Follow] reached, the
owner-absence window with no companion offensive lines, test-logout /
test-relogin ordering, the post-rejoin follow + reached, no [CRASH],
personal containers untouched.

Owner death and a map change share the same gate predicate
(!leader || !leader->IsAlive() in IsFollowOwnerAvailable) but are not
separately executed: they are reported as unexecuted shared-predicate
checks.
"""
import os
import re
import time
import unittest
import uuid
from datetime import datetime, timezone

import test_bot_provision as p
import test_bot_follow as f

PERSONAL_CONTAINERS = ("tortoise-local-db-1", "tortoise-local-realmd-1",
                       "tortoise-local-world-1")

O = 610200
C = 610201
V = 2500200
COMP_NAME = "Lscomp"
VERMIN_NAME = "Kobold Vermin"

LEASH_ARMED = re.compile(r"\[PlayerBot\] pursuit leash armed GUID:(\d+) target:(\d+)")
LEASH_EXPIRED = re.compile(r"\[PlayerBot\] pursuit leash expired GUID:(\d+) target:(\d+)")
ASSIST_FIGHTING = re.compile(r"\[PlayerBot\]\[Assist\] fighting GUID:(\d+)")
STATE = re.compile(
    r"\[PlayerBot\] state GUID:(\d+) map:(\d+) pos:([-0-9.]+)/([-0-9.]+)/([-0-9.]+)"
    r" combat:(\d+) victim:(\d+)")

# 2 s cadence "[Assist] fighting" lines bound the leash window: 30 s / 2 s
# is 15 nominal; the band maps to roughly 24-36 s of actual pursuit.
FIGHTING_BAND = (12, 18)

OFFENSIVE_KEYWORDS = ("fighting", "engage", "loot", "Assist", "Defend", "attack")


def _seed_sql():
    return (
        "INSERT INTO tw_char.characters\n"
        " (guid,account,name,race,class,gender,level,money,position_x,position_y,position_z,map,\n"
        "  orientation,zone,health,power1,power2,power3,power4,power5,xp,xp_gain)\n"
        "VALUES (%d,1000%d,'Lsowner',1,1,0,10,100000,-8949.95,-130.493,83.5312,0,\n"
        " 0,12,100,0,0,0,0,0,0,1),\n"
        " (%d,1000%d,'%s',1,1,0,10,100000,-8949.95,-200.493,83.5312,0,\n"
        " 0,12,100,0,0,0,0,0,0,1);\n"
        "INSERT INTO tw_char.playerbot (char_guid,chance,ai)\n"
        " VALUES (%d,100,'Default'),(%d,100,'Default');\n"
        "INSERT INTO tw_char.bot_ownership (char_guid,account_id,bot_type,provision_version,owner_account_id)\n"
        " VALUES (%d,1000%d,1,2,1000%d),\n"
        "        (%d,1000%d,1,2,1000%d);\n"
        "UPDATE tw_world.creature_template SET loot_id=0 WHERE entry=6;\n"
        "UPDATE tw_world.creature_template\n"
        " SET health_min=150, health_max=150, regeneration=0 WHERE entry=6;\n"
        # NO_AGGRO (0x2) | FIXED_Z (0x40) | NO_TARGET (0x20000): the hover
        # point never acquires a target, never falls, never moves.
        "UPDATE tw_world.creature_template SET flags_extra=flags_extra|131138 WHERE entry=6;\n"
        "INSERT INTO tw_world.creature\n"
        " (guid,id,map,position_x,position_y,position_z,orientation,spawntimesecsmin,\n"
        "  spawntimesecsmax,wander_distance,health_percent,mana_percent,movement_type,spawn_flags)\n"
        "VALUES (%d,6,0,-8949.95,-160.0,103.5312,0,600,600,0,100,100,0,1);\n"
        "DELETE FROM tw_world.creature WHERE map=0 AND position_x BETWEEN -8990 AND -8910\n"
        " AND position_y BETWEEN -230 AND -100 AND guid NOT IN (%d);\n"
    ) % (O, O, C, C, COMP_NAME, O, C, O, O, O, C, C, O, V, V)


def _script():
    return ";".join([
        "2000:%d:bothold Lsowner" % O,
        "4000:%d:bothold %s" % (O, COMP_NAME),
        "8000:%d:botrecruit %s" % (O, COMP_NAME),
        "12000:%d:botfollow %s" % (O, COMP_NAME),
        "20000:%d:botassist %s %s" % (O, COMP_NAME, VERMIN_NAME),
        "105000:%d:botfollow %s" % (O, COMP_NAME),
        "120000:%d:botstop %s" % (O, COMP_NAME),
    ])


def _reached_after_last_follow(text):
    idx = text.rfind("follow accepted bot:%s" % COMP_NAME)
    if idx == -1:
        return False
    return any(m[0] == str(C) for m in f.REACHED.findall(text[idx:]))


class BotCompanionLeashTests(unittest.TestCase):
    """Leash + owner-loss lab: one owned companion, one pinned hover hostile."""
    logs = ""
    personal_before = {}
    personal_after = {}
    base = env = project = evidence = None

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
        p.IMAGE = os.environ.get("PORT008_LAB_IMAGE", "tortoise-local:dev")
        try:
            p.command(["docker", "image", "inspect", p.IMAGE])
        except RuntimeError:
            raise unittest.SkipTest("%s image not present; build it first" % p.IMAGE)
        cls.personal_before = cls._personal_state()
        world = p.world_env_for("Lsowner")
        world.update(PLAYERBOT_MIN_BOTS="0", PLAYERBOT_MAX_BOTS="0",
                     PLAYERBOT_REFRESH="600000", PLAYERBOT_UPDATE_MS="1000",
                     PLAYERBOT_PROVISION="",
                     PLAYERBOT_TEST_LOGIN="%d,%d" % (O, C),
                     PLAYERBOT_QUEST_ID="0",
                     PLAYERBOT_WANDER_RADIUS="0.5",
                     PLAYERBOT_FOLLOW_SCRIPT=_script(),
                     # Offsets in ms relative to the owner's own login
                     # (the follow-script clock): out at tau68, back at tau80.
                     PLAYERBOT_TEST_LOGOUT_SCRIPT="%d,68000,80000" % O,
                     PLAYER_SAVE_INTERVAL="5000")
        try:
            cls.project = "tortoise-bot-ls-" + uuid.uuid4().hex[:12]
            stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
            cls.evidence = p.ROOT / "local" / (cls.project + "-" + stamp)
            cls.evidence.mkdir(parents=True)
            (cls.evidence / "image-id.txt").write_text(
                p.command(["docker", "image", "inspect", "--format", "{{.Id}}", p.IMAGE]),
                encoding="utf-8")
            cls.base, cls.env = p.boot_lab(cls.project, cls.evidence, world, _seed_sql())
            p.command(["docker", "compose"] + cls.base + ["up", "-d", "--no-deps", "world"],
                      env=cls.env)
            cls.logs = p.wait_for(cls.base, cls.env, lambda text:
                                  all("test-login %d first=1 second=0" % g in text
                                      for g in (O, C)), deadline=420)
            # Drive to the post-rejoin follow: the tau105 botfollow needs the
            # owner back online (relogin at tau80 + login latency), then the
            # companion must reach again.
            cls.logs = p.wait_for(cls.base, cls.env, lambda text:
                                  ("Playerbot: test-relogin %d at" % O in text
                                   and text.count("follow accepted bot:%s" % COMP_NAME) >= 2
                                   and _reached_after_last_follow(text)),
                                  deadline=420)
            (cls.evidence / "drive.log").write_text(cls.logs, encoding="utf-8")
            end = time.monotonic() + 15
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

    # ------------------------------------------------------------------ basic

    def test_all_seeded_bots_logged_in(self):
        for g, n in ((O, "Lsowner"), (C, COMP_NAME)):
            self.assertIn("test-login %d first=1 second=0" % g, self.logs)
            self.assertIn("[PlayerBot][Login]  '%s' GUID:%d" % (n, g), self.logs)
        self.assertNotIn("[CRASH]", self.logs)

    def test_probe_armed_and_party_formed(self):
        self.assertIn("Playerbot: test-logout armed guid:%d out:68000 re:80000" % O, self.logs)
        self.assertIn("party recruit accepted bot:%s guid:%d leader:%d"
                      % (COMP_NAME, C, O), self.logs)

    # ------------------------------------------------------------------ leash

    def test_assist_accepted(self):
        self.assertIn("assist accepted bot:%s target:%s guid:%d"
                      % (COMP_NAME, VERMIN_NAME, V), self.logs)
        self.assertNotIn("assist rejected", self.logs)

    def test_leash_armed_and_expired(self):
        armed = [m for m in LEASH_ARMED.findall(self.logs) if m[0] == str(C)]
        self.assertTrue(armed, "the pursuit leash never armed for the companion")
        self.assertEqual(armed[0][1], str(V))
        expired = [m for m in LEASH_EXPIRED.findall(self.logs) if m[0] == str(C)]
        self.assertTrue(expired, "the pursuit leash never expired for the companion")
        self.assertEqual(expired[0][1], str(V))
        # The unreachable target must never be looted.
        self.assertNotIn("[PlayerBot] loot intent GUID:%d" % C, self.logs)

    def test_leash_budget_honored(self):
        armed_idx = self.logs.find("[PlayerBot] pursuit leash armed GUID:%d" % C)
        expired_idx = self.logs.find("[PlayerBot] pursuit leash expired GUID:%d" % C)
        self.assertGreater(armed_idx, -1)
        self.assertGreater(expired_idx, armed_idx)
        window = self.logs[armed_idx:expired_idx]
        n = sum(1 for g in ASSIST_FIGHTING.findall(window) if g == str(C))
        lo, hi = FIGHTING_BAND
        print("leash budget: %d x 2 s assist-fighting lines between armed and expired "
              "(~%d-%d s window; nominal 30 s)" % (n, 2 * lo, 2 * hi))
        self.assertGreaterEqual(n, lo, "the leash expired far too early (%d lines)" % n)
        self.assertLessEqual(n, hi, "the leash expired far too late (%d lines)" % n)

    def test_regroup_after_leash(self):
        idx = self.logs.find("[PlayerBot] pursuit leash expired GUID:%d" % C)
        self.assertGreater(idx, -1)
        reached = [m for m in f.REACHED.findall(self.logs[idx:]) if m[0] == str(C)]
        self.assertTrue(reached, "the companion never regrouped after the leash")
        self.assertLessEqual(float(reached[0][2]), 2.0)

    # ------------------------------------------------------------- owner loss

    def test_owner_absence_holds_safe(self):
        ilogout = self.logs.find("Playerbot: test-logout %d at" % O)
        irelogin = self.logs.find("Playerbot: test-relogin %d at" % O)
        self.assertGreater(ilogout, -1, "the owner never logged out")
        self.assertGreater(irelogin, ilogout, "relogin must follow the logout")
        # The post-leash regroup completed before the owner left.
        reached = [m for m in f.REACHED.findall(self.logs[:ilogout]) if m[0] == str(C)]
        self.assertTrue(reached, "the companion had not regrouped before the owner left")
        self.assertLessEqual(float(reached[-1][2]), 2.0)
        # While the owner is absent: no companion offensive lines at all.
        window = self.logs[ilogout:irelogin]
        for line in window.splitlines():
            if ("GUID:%d" % C) not in line:
                continue
            self.assertFalse(
                any(k in line for k in OFFENSIVE_KEYWORDS),
                "companion acted offensively while the owner was absent: %s" % line.strip())
        # Position frozen while absent (10 s cadence state lines).
        positions = [(m[2], m[3], m[4]) for m in STATE.findall(window) if m[0] == str(C)]
        if len(positions) >= 2:
            self.assertEqual(len(set(positions)), 1,
                             "companion moved while the owner was absent: %r" % positions)

    def test_follow_resumes_after_rejoin(self):
        irelogin = self.logs.find("Playerbot: test-relogin %d at" % O)
        self.assertGreater(irelogin, -1)
        tail = self.logs[irelogin:]
        self.assertIn("follow accepted bot:%s" % COMP_NAME, tail)
        idx = tail.rfind("follow accepted bot:%s" % COMP_NAME)
        reached = [m for m in f.REACHED.findall(tail[idx:]) if m[0] == str(C)]
        self.assertTrue(reached, "the companion never reached the re-joined owner")
        self.assertLessEqual(float(reached[0][2]), 2.0)

    # ------------------------------------------------------------- isolation

    def test_personal_volume_untouched(self):
        self.assertNotEqual(self.project, "tortoise-local")
        if self.personal_before:
            self.assertEqual(self.personal_after, self.personal_before,
                             "personal server containers must not be restarted or touched")


if __name__ == "__main__":
    unittest.main()
