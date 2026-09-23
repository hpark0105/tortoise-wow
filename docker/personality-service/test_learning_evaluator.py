import os, sys, unittest
sys.path.insert(0, os.path.dirname(__file__))
from learning_evaluator import Outcome, evaluate, assign_arm

class EvaluatorTests(unittest.TestCase):
    def test_improvement_and_regression(self):
        b = [Outcome(True, .5, 1000) for _ in range(3)]
        c = [Outcome(True, .4, 900) for _ in range(3)]
        self.assertEqual(evaluate(b, c), "improvement")
        self.assertEqual(evaluate(b, [Outcome(True, .3, 900, 1) for _ in range(3)]), "regression")
    def test_sparse_and_incomplete_fail_closed(self):
        x = [Outcome(True, .1, 1)]
        self.assertEqual(evaluate(x, x), "inconclusive")
        self.assertEqual(evaluate([Outcome(False, 0, 0)] * 3, [Outcome(True, 0, 0)] * 3), "inconclusive")
    def test_deterministic_assignment(self):
        self.assertEqual([assign_arm(i, "c") for i in range(4)], ["candidate", "baseline", "candidate", "baseline"])
        self.assertEqual(assign_arm(0, "c", False), "baseline")

if __name__ == "__main__": unittest.main()
