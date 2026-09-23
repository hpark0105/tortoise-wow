import importlib.util
import os
import unittest

PATH = os.path.join(os.path.dirname(__file__), "real_planner.py")
spec = importlib.util.spec_from_file_location("real_planner", PATH)
rp = importlib.util.module_from_spec(spec); spec.loader.exec_module(rp)

class LearningPromptTests(unittest.TestCase):
    def test_bounded_request_and_metadata(self):
        doc = rp.build_learning_request("warrior:abc", 2,
            [{"id": "l1", "text": "prefer the legal candidate", "evidence_count": 3}],
            [{"id": "e1", "outcome": "complete", "duration_ms": 1200}], "r1")
        self.assertEqual(doc["schema_version"], 2)
        self.assertEqual(rp.inference_metadata(10, 35, True), {"latency_ms": 25, "contended": True})

    def test_rejects_fabricated_or_sensitive_evidence(self):
        with self.assertRaises(ValueError):
            rp.build_learning_request("warrior:abc", 2, [{"id": "x", "text": "x", "evidence_count": 0}])
        with self.assertRaises(ValueError):
            rp.build_learning_request("warrior:abc", 2, [{"id": "x", "text": "target_guid=7", "evidence_count": 1}])

    def test_limits(self):
        with self.assertRaises(ValueError):
            rp.build_learning_request("warrior:abc", 2, [{"id": str(i), "text": "ok", "evidence_count": 1} for i in range(4)])

if __name__ == "__main__":
    unittest.main()
