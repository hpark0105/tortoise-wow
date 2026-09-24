"""Source-contract checks for bounded outdoor route memory and recovery."""
import os
import re
import unittest


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class OutdoorRouteValueTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with open(os.path.join(ROOT, "src/game/PlayerBots/PlayerBotAI.cpp"), encoding="utf-8") as stream:
            cls.ai = stream.read()
        with open(os.path.join(ROOT, "src/game/PlayerBots/PlayerBotAI.h"), encoding="utf-8") as stream:
            cls.header = stream.read()

    def test_route_memory_is_bounded_and_session_scoped(self):
        self.assertIn("ActivityNode _activityNodes[12]", self.header)
        self.assertIn("_activityNodeCount < 12", self.ai)
        self.assertIn("_activityNodeCount = 0", self.ai)
        self.assertIn("[WorldRoute] learned", self.ai)
        self.assertIn("[WorldRoute] replay", self.ai)

    def test_recovery_is_repath_then_walkable_nudge_then_abandon(self):
        body = re.search(r"if \(!_travelProgressMs \|\| \(MotionIdle\(\) && _travelRecoveryStage == 0\)\)(.*?)return true;\n        }\n    }",
                         self.ai, re.S)
        self.assertIsNotNone(body)
        self.assertLess(body.group(1).find("_travelRecoveryStage == 0"),
                        body.group(1).find("_travelRecoveryStage == 1"))
        self.assertIn("GetWalkRandomPosition", body.group(1))
        self.assertIn("AbandonActivityTravel()", body.group(1))
        self.assertNotIn("Teleport", body.group(1))

    def test_every_candidate_and_nudge_remains_same_zone(self):
        self.assertIn("GetZoneId(x, y, z) == _activityZone", self.ai)
        self.assertIn("GetZoneId(x, y, z) != _activityZone", self.ai)
        self.assertIn("240.0f * 240.0f", self.ai)


if __name__ == "__main__":
    unittest.main()
