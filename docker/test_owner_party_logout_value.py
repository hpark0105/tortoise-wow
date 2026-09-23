"""TW-OWNER-PARTY-LOGOUT v1: static value test for the owner-logout
companion-party disband predicate.

Verifies the source invariants without a running server:
- PlayerBotMgr declares and implements IsOwnedCompanionGroup.
- The predicate checks FindBotByGuid + ownerAccountId for each
  non-logout member.
- WorldSession::LogoutPlayer calls the predicate with the correct
  guards (not raid, not BG, m_Socket, GetAccountId) before the
  normal RemoveFromGroup path.
- Disband is called when the predicate is true.
"""
import os
import re
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def _read(path):
    full = os.path.join(REPO, path)
    with open(full, "r", encoding="utf-8") as f:
        return f.read()


class OwnerPartyLogoutStaticTest(unittest.TestCase):

    # -- PlayerBotMgr.h ------------------------------------------------------

    def test_header_declares_method(self):
        src = _read("src/game/PlayerBots/PlayerBotMgr.h")
        sig = "bool IsOwnedCompanionGroup(Group const* group, uint32 logoutGuid, uint32 accountId) const;"
        self.assertIn(sig, src,
                      "PlayerBotMgr.h missing IsOwnedCompanionGroup declaration")

    # -- PlayerBotMgr.cpp ----------------------------------------------------

    def test_implementation_exists(self):
        src = _read("src/game/PlayerBots/PlayerBotMgr.cpp")
        self.assertIn(
            "bool PlayerBotMgr::IsOwnedCompanionGroup(Group const* group, uint32 logoutGuid, uint32 accountId) const",
            src,
            "PlayerBotMgr.cpp missing IsOwnedCompanionGroup definition")

    def test_predicate_checks_findbotbyguid(self):
        src = _read("src/game/PlayerBots/PlayerBotMgr.cpp")
        # Extract the function body
        m = re.search(
            r"bool PlayerBotMgr::IsOwnedCompanionGroup.*?\n\{(.*?)\n\}",
            src, re.DOTALL)
        self.assertIsNotNone(m, "could not extract IsOwnedCompanionGroup body")
        body = m.group(1)
        self.assertIn("FindBotByGuid", body,
                      "predicate must call FindBotByGuid")
        self.assertIn("ownerAccountId", body,
                      "predicate must check ownerAccountId")
        self.assertIn("logoutGuid", body,
                      "predicate must skip the logging-out member")
        self.assertIn("ownerFound && companionFound", body,
                      "predicate must require both the owner and a companion")

    def test_predicate_returns_false_on_null_group(self):
        src = _read("src/game/PlayerBots/PlayerBotMgr.cpp")
        m = re.search(
            r"bool PlayerBotMgr::IsOwnedCompanionGroup.*?\n\{(.*?)\n\}",
            src, re.DOTALL)
        self.assertIsNotNone(m)
        body = m.group(1)
        self.assertIn("if (!group)", body,
                      "predicate must guard against null group")

    # -- WorldSession.cpp ----------------------------------------------------

    def test_worldsession_calls_predicate(self):
        src = _read("src/game/WorldSession.cpp")
        self.assertIn("sPlayerBotMgr.IsOwnedCompanionGroup", src,
                      "WorldSession.cpp must call IsOwnedCompanionGroup")

    def test_worldsession_guards(self):
        src = _read("src/game/WorldSession.cpp")
        # The disband block must check: not raid, not BG, m_Socket
        block_re = re.compile(
            r"TW-OWNER-PARTY-LOGOUT.*?_player->GetGroup\(\)->Disband\(\);",
            re.DOTALL)
        m = block_re.search(src)
        self.assertIsNotNone(m,
                             "WorldSession.cpp missing disband block after predicate")
        block = m.group(0)
        self.assertIn("isRaidGroup", block,
                      "must guard against raid groups")
        self.assertIn("isBGGroup", block,
                      "must guard against battleground groups")
        self.assertIn("m_Socket", block,
                      "must check m_Socket (normal logout)")
        self.assertIn("GetAccountId", block,
                      "must pass the session account ID")

    def test_disband_before_removefromgroup(self):
        src = _read("src/game/WorldSession.cpp")
        disband_pos = src.find("TW-OWNER-PARTY-LOGOUT: disband companion-only party")
        remove_pos = src.find("_player->RemoveFromGroup();", disband_pos)
        self.assertNotEqual(disband_pos, -1, "disband log line not found")
        self.assertNotEqual(remove_pos, -1,
                            "RemoveFromGroup not found after disband block")
        self.assertLess(disband_pos, remove_pos,
                        "Disband must come before RemoveFromGroup in the logout path")

    def test_no_raid_or_bg_disband(self):
        """The disband path must NOT apply to raid or BG groups."""
        src = _read("src/game/WorldSession.cpp")
        block_re = re.compile(
            r"TW-OWNER-PARTY-LOGOUT.*?_player->GetGroup\(\)->Disband\(\);",
            re.DOTALL)
        m = block_re.search(src)
        self.assertIsNotNone(m)
        block = m.group(0)
        # The guard must negate isRaidGroup and isBGGroup
        self.assertIn("!_player->GetGroup()->isRaidGroup()", block)
        self.assertIn("!_player->GetGroup()->isBGGroup()", block)


if __name__ == "__main__":
    unittest.main()
