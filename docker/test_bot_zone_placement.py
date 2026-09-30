"""Regression checks for durable, unconstrained citizen placement."""
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class ZonePlacementConfigurationTest(unittest.TestCase):
    def test_temporary_zone_pin_is_not_exposed_or_generated(self):
        compose = (ROOT / "compose.yaml").read_text(encoding="utf-8")
        server = (ROOT / "docker" / "server.py").read_text(encoding="utf-8")
        manager = (ROOT / "src" / "game" / "PlayerBots" / "PlayerBotMgr.cpp").read_text(
            encoding="utf-8")
        header = (ROOT / "src" / "game" / "PlayerBots" / "PlayerBotMgr.h").read_text(
            encoding="utf-8")

        for source in (compose, server, manager, header):
            self.assertNotIn("ZONE_WORLD_PLACEMENT_ZONE", source)
            self.assertNotIn("ZoneWorldPlacementZone", source)

    def test_population_allocator_does_not_force_travelers_back_to_one_zone(self):
        manager = (ROOT / "src" / "game" / "PlayerBots" / "PlayerBotMgr.cpp").read_text(
            encoding="utf-8")
        self.assertNotIn("return-to-zone queued", manager)
        self.assertIn("SelectCitizenProgressionDestination", manager)
        self.assertIn("zoneWorldSafetyExcludeZone", manager)

    def test_blocked_safety_relocation_cannot_starve_normal_population_fill(self):
        manager = (ROOT / "src" / "game" / "PlayerBots" / "PlayerBotMgr.cpp").read_text(
            encoding="utf-8")
        manager = manager.replace("\r\n", "\n")
        self.assertIn("if (safetyRelocationWaiting)\n        collectCandidates(true)", manager)
        self.assertIn("collectCandidates(false)", manager)
        self.assertIn("e->zoneWorldSafetyRelocation != safetyPass", manager)
        self.assertIn("no_suitable_zone:%u", manager)

    def test_population_allocator_waits_for_old_account_session_to_drain(self):
        manager = (ROOT / "src" / "game" / "PlayerBots" / "PlayerBotMgr.cpp").read_text(
            encoding="utf-8").replace("\r\n", "\n")
        allocator = manager[
            manager.index("void PlayerBotMgr::UpdateZoneWorldPopulation"):]
        self.assertIn("sWorld.FindSession(citizen->accountId)", allocator)
        self.assertIn("sWorld.FindSession(e->accountId)", allocator)
        self.assertIn("session_busy:%u", allocator)

    def test_failed_safety_relocations_report_per_destination_rejection_reasons(self):
        manager = (ROOT / "src" / "game" / "PlayerBots" / "PlayerBotMgr.cpp").read_text(
            encoding="utf-8").replace("\r\n", "\n")
        failure = manager[manager.index("if (!targetPlan)",
                                         manager.index("void PlayerBotMgr::UpdateZoneWorldPopulation")):]
        failure = failure[:failure.index("std::map<std::pair<uint32, uint32>, std::vector<WorldLocation>>")]
        self.assertIn("if (e->zoneWorldSafetyRelocation)", failure)
        self.assertIn("no-destination guid:%u level:%u faction:%u", failure)
        self.assertIn("destination-rejected guid:%u map:%u zone:%u", failure)
        for reason in ("excluded-danger-zone", "faction-or-area-rejected",
                       "no-verified-creature-profile", "fallback-level-too-high",
                       "fallback-productive-share-too-low", "fallback-overlevel-mix"):
            self.assertIn(reason, failure)

    def test_population_fallback_keeps_a_minimum_productive_and_level_safety_gate(self):
        manager = (ROOT / "src" / "game" / "PlayerBots" / "PlayerBotMgr.cpp").read_text(
            encoding="utf-8")
        header = (ROOT / "src" / "game" / "PlayerBots" / "PlayerBotMgr.h").read_text(
            encoding="utf-8")
        self.assertIn("CitizenZoneFallbackMinimumProductiveSharePercent = 5", header)
        self.assertIn("CitizenZoneFallbackMaximumOverlevelSharePercent = 5", header)
        self.assertIn("candidateLevel > citizenLevel + 3", manager)
        self.assertIn("share <= currentProductiveShare", manager)
        self.assertIn("safeWeight * 100 < totalWeight *", manager)
        self.assertIn("e->zoneWorldRetryAfterMs = m_elapsedTime + 60000;", manager)


class CitizenProgressionTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.manager = (ROOT / "src" / "game" / "PlayerBots" / "PlayerBotMgr.cpp").read_text(
            encoding="utf-8")
        cls.ai = (ROOT / "src" / "game" / "PlayerBots" / "PlayerBotAI.cpp").read_text(
            encoding="utf-8")

    def test_route_choice_uses_observed_creature_levels_without_local_radius_gate(self):
        self.assertIn("GetCitizenZoneProductiveSharePercent", self.manager)
        self.assertIn("item.first.first != citizen->GetMapId()", self.manager)
        self.assertIn("candidateArea->Team == AREATEAM_ALLY", self.manager)
        self.assertIn("candidateArea->Team == AREATEAM_HORDE", self.manager)
        self.assertNotIn("kCitizenProgressionMaxDistanceYd", self.manager)
        destinationSelector = self.manager[
            self.manager.index("bool PlayerBotMgr::SelectCitizenProgressionDestination"):]
        self.assertNotIn("distance <=", destinationSelector)

    def test_level_up_decision_uses_productive_share_and_handles_level_cap(self):
        refresh = self.ai[self.ai.index("void PlayerBotAI::RefreshCitizenProgression"):]
        self.assertIn("GetCitizenZoneProductiveSharePercent", refresh)
        self.assertIn("level >= 60", refresh)
        self.assertIn('"level-cap"', refresh)

    def test_persisted_route_can_resume_after_long_same_continent_leg(self):
        journal = self.ai[self.ai.index("void PlayerBotAI::LoadCitizenJournal"):]
        self.assertIn("targetMap == me->GetMapId()", journal)
        self.assertIn("targetProductiveShare >= PlayerBotMgr::CitizenZoneMinimumProductiveSharePercent", journal)
        self.assertNotIn("savedTargetNearby", journal)

    def test_pilot_zone_targets_are_explicit_and_allocation_stays_faction_safe(self):
        compose = (ROOT / "compose.yaml").read_text(encoding="utf-8")
        server = (ROOT / "docker" / "server.py").read_text(encoding="utf-8")
        self.assertIn("PLAYERBOT_ZONE_WORLD_ZONE_TARGETS", compose)
        self.assertIn("PlayerBot.ZoneWorldZoneTargets", server)
        self.assertIn("zoneTargets.find(';', zoneTargetFrom)", self.manager)
        self.assertIn("confZoneWorldZoneTargets.find(plan.key)", self.manager)
        self.assertIn("factionAllowed(plan.key.second)", self.manager)
        self.assertIn("pending->state == PB_STATE_LOADING", self.manager)
        self.assertIn("GetPreparedSpawnMap()", self.manager)
        self.assertIn("GetPreparedSpawnZone()", self.manager)


if __name__ == "__main__":
    unittest.main()
