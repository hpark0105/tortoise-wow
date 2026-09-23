import unittest
from qualification import qualify
class QualificationTests(unittest.TestCase):
    def test_no_live_claim(self):
        self.assertEqual(qualify({}, {}, {}, False)["status"], "pending-real-world-evidence")
    def test_real_evidence_gate(self):
        self.assertEqual(qualify({}, {}, {"eligible": True}, True)["status"], "qualified")
if __name__ == "__main__": unittest.main()
