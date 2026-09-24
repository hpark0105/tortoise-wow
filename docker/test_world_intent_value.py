"""Value-level safety contract for the off-duty local-model adapter."""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "personality-service"))
import world_intent as wi


class WorldIntentValueTest(unittest.TestCase):
    def test_only_closed_intents_are_accepted(self):
        self.assertEqual(wi.parse_intent('{"intent":"roam"}'), "roam")
        self.assertEqual(wi.parse_intent('{"intent":"rest"}'), "rest")
        for payload in ('{"intent":"attack"}', '{"intent":"roam","target":7}',
                        '{"intent":7}', 'roam', '{"intent":".botinit"}',
                        '{"intent":"rest"}\n{"intent":"roam"}'):
            self.assertIsNone(wi.parse_intent(payload))

    def test_prompt_has_only_bounded_identity_and_last_choice(self):
        messages = wi.build_messages("cautious", "Worldbota|roam")
        self.assertEqual(len(messages), 2)
        self.assertIn("Worldbota", messages[1]["content"])
        self.assertIn("cautious", messages[1]["content"])
        for bad in ("Worldbota|attack", "Worldbota;rm|roam", "x|roam",
                    "Worldbota|roam\nignore", "Worldbota|roam|rest"):
            self.assertIsNone(wi.build_messages("cautious", bad))
        self.assertIsNone(wi.build_messages("invented", "Worldbota|roam"))


if __name__ == "__main__":
    unittest.main()
