import unittest
from owner_controls import authorize
class OwnerControlTests(unittest.TestCase):
    def test_owner_transitions(self):
        self.assertEqual(authorize("pause", 1, 1)["mode"], "paused")
        self.assertEqual(authorize("resume", 1, 1, "paused")["mode"], "observe")
        self.assertEqual(authorize("rollback", 1, 1)["mode"], "observe")
    def test_intruder_rejected(self):
        self.assertFalse(authorize("pause", 2, 1)["ok"])
        self.assertFalse(authorize("drop-db", 1, 1)["ok"])
if __name__ == "__main__": unittest.main()
