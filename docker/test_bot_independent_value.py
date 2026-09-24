"""Source-contract checks for selective setup and off-duty companions.

The world build verifies C++ compilation; in-game movement remains a pilot
check because no source-level test can prove pathfinding in a live map.
"""
import os
import re
import unittest


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def source(path):
    with open(os.path.join(ROOT, path), encoding="utf-8") as stream:
        return stream.read()


class IndependentCompanionValueTest(unittest.TestCase):
    def test_named_init_filters_roster_after_ownership_check(self):
        mgr = source("src/game/PlayerBots/PlayerBotMgr.cpp")
        body = re.search(r"bool PlayerBotMgr::BotInit\(.*?\n\{(.*?)\n\}", mgr, re.S)
        self.assertIsNotNone(body)
        self.assertIn("FindBotByName(botName)", body.group(1))
        self.assertIn("selected->ownerAccountId != issuer->GetSession()->GetAccountId()", body.group(1))
        self.assertIn("if (selected && e != selected)", body.group(1))
        self.assertIn("if (!e->persistent || e->ownerAccountId != issuerAcc)", body.group(1))
        self.assertIn("occupied + pendingSlots >= MAX_GROUP_SIZE", body.group(1))

    def test_dismiss_releases_orders_without_benching(self):
        mgr = source("src/game/PlayerBots/PlayerBotMgr.cpp")
        body = re.search(r"bool PlayerBotMgr::BotDismiss\(.*?\n\{(.*?)\n\}", mgr, re.S)
        self.assertIsNotNone(body)
        self.assertIn("e->ai->ReleaseToWorld()", body.group(1))
        self.assertIn("e->defendEnabled = false", body.group(1))
        self.assertNotIn("DeleteBot(", body.group(1))
        ai = source("src/game/PlayerBots/PlayerBotAI.cpp")
        release = re.search(r"void PlayerBotAI::ReleaseToWorld\(\).*?\n\{(.*?)\n\}", ai, re.S)
        self.assertIsNotNone(release)
        for fragment in ("FollowStop()", "_held = false", "_assistTargetGuid = 0"):
            self.assertIn(fragment, release.group(1))

    def test_client_uninvite_releases_group_bound_orders(self):
        ai = source("src/game/PlayerBots/PlayerBotAI.cpp")
        body = re.search(r"bool PlayerBotAI::UpdateCompanion\(uint32 diff\)\s*\{(.*?)\n\}", ai, re.S)
        self.assertIsNotNone(body)
        for fragment in ("_followGroupId", "!me->GetGroup()", "ReleaseToWorld()",
                         "botEntry->defendEnabled = false"):
            self.assertIn(fragment, body.group(1))
        release = re.search(r"void PlayerBotAI::ReleaseToWorld\(\)\s*\{(.*?)\n\}", ai, re.S)
        self.assertIn("_followGroupId = 0", release.group(1))

    def test_ungrouped_owned_bot_uses_shared_activity_while_owner_online(self):
        ai = source("src/game/PlayerBots/PlayerBotAI.cpp")
        update = re.search(r"void PlayerBotAI::UpdateAI\(const uint32 diff\).*?\n\{(.*?)\n\}", ai, re.S)
        self.assertIsNotNone(update)
        for fragment in ("FindOwnerByAccount()", "!me->GetGroup()",
                         "!_following && !_held && !_assistTargetGuid",
                         "(citizenOffDuty || ownedOffDuty) && UpdateIndependentActivity(diff)"):
            self.assertIn(fragment, update.group(1))
        self.assertIn("bool PlayerBotAI::UpdateIndependentActivity(uint32 diff)", ai)
        self.assertIn("else if (IsOwnedCompanion())", ai)  # owner-offline fallback

    def test_all_persistent_bots_can_acquire_after_party_release(self):
        ai = source("src/game/PlayerBots/PlayerBotAI.cpp")
        body = re.search(r"else\n\s*\{\n\s*// All persistent bots use the same citizen acquisition policy.*?\n\s*\}\n\s*else\n\s*_combatCheckTimer", ai, re.S)
        self.assertIsNotNone(body)
        self.assertIn("SelectNearestTarget(30.0f)", body.group(0))
        self.assertNotIn("no-order idle", body.group(0))

    def test_shared_kills_create_persistent_rate_limited_acquaintances(self):
        ai = source("src/game/PlayerBots/PlayerBotAI.cpp")
        self.assertIn("void PlayerBotAI::RecordSocialEncounter", ai)
        for fragment in ("bot_social_acquaintance", "shared_events = shared_events + 1",
                         "_socialCooldownMs = 30000",
                         "event:shared-kill", "Good work, friend."):
            self.assertIn(fragment, ai)
        self.assertIn("RecordSocialEncounter(target, true)", ai)

    def test_owned_world_activation_is_opt_in_and_separate(self):
        mgr = source("src/game/PlayerBots/PlayerBotMgr.cpp")
        server = source("docker/server.py")
        self.assertIn('"PlayerBot.OwnedWorldTarget", 0', mgr)
        self.assertIn('"PLAYERBOT_OWNED_WORLD_TARGET", "0"', server)
        body = re.search(r"void PlayerBotMgr::UpdateOwnedWorldPopulation\(\).*?\n\{(.*?)\n\}", mgr, re.S)
        self.assertIsNotNone(body)
        for fragment in ("PB_STATE_LOADING", "PB_STATE_ONLINE", "sWorld.FindSession(e->ownerAccountId)",
                         "e->state != PB_STATE_OFFLINE", "AddBot((uint32)e->playerGUID, false)",
                         "ownedWorldRetryAfterMs"):
            self.assertIn(fragment, body.group(1))
        self.assertNotIn("CompletePartyRecruit", body.group(1))

    def test_off_duty_owned_bot_enters_corpse_recovery(self):
        ai = source("src/game/PlayerBots/PlayerBotAI.cpp")
        body = re.search(
            r"bool PlayerBotAI::UpdateRecovery\(uint32 diff\)\s*\{(.*?)\n\}",
            ai, re.S)
        self.assertIsNotNone(body)
        # Dead owned bots - off-duty or under an active order - enter the
        # normal corpse-reclaim path.
        self.assertIn(
            "if (!IsOwnedCompanion() && !IsZoneCitizen() &&",
            body.group(1))
        # The pre-fix order-only gate no longer gates recovery.
        self.assertNotIn(
            "if (!_following && !_held && !_assistTargetGuid)",
            body.group(1))
        # The existing normal reclaim mechanism is unchanged.
        self.assertIn("CMSG_RECLAIM_CORPSE", body.group(1))

    def test_citizen_party_and_evidence_paths_share_guarded_capabilities(self):
        ai = source("src/game/PlayerBots/PlayerBotAI.cpp")
        mgr = source("src/game/PlayerBots/PlayerBotMgr.cpp")
        self.assertIn("(botEntry->ownerAccountId || IsZoneCitizen())", ai)
        self.assertIn("be->recruiterAccountId == leaderAccount", ai)
        self.assertIn("be->recruiterGuid == leaderLow", ai)
        self.assertIn("member->GetGroup() == group", ai)
        self.assertIn("botEntry->recruiterGuid == owner->GetGUIDLow()", ai)
        self.assertIn("(e->ai && e->ai->IsZoneCitizen() && e->recruiterAccountId)", mgr)
        self.assertIn("(!IsZoneCitizen() && !FindOwnerByAccount())", ai)


if __name__ == "__main__":
    unittest.main()
