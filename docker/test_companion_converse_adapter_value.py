"""PORT-022 (KAP-558): value-level coverage for the conversation adapter
(converse.py). No I/O, no sockets, no model call: persona mapping, strict
{"reply": "..."} parsing, and bounded sanitization (length, leading dot,
control characters, non-ASCII). Runs as part of the normal docker test
discovery.
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "personality-service"))
import converse as cv


class PersonaTest(unittest.TestCase):
    def test_declared_profiles(self):
        self.assertEqual(set(cv.PERSONAS.keys()),
                         {"none", "reckless", "cautious"})
        for name in ("none", "reckless", "cautious"):
            self.assertTrue(cv.persona_for(name))

    def test_unknown_profile_is_neutral(self):
        self.assertEqual(cv.persona_for("bogus"), cv.persona_for("none"))
        self.assertEqual(cv.persona_for(None), cv.persona_for("none"))
        self.assertEqual(cv.persona_for(""), cv.persona_for("none"))

    def test_persona_is_profile_distinct(self):
        # The A/B contract: one persona, observably different text.
        self.assertNotEqual(cv.persona_for("reckless"),
                            cv.persona_for("cautious"))
        self.assertNotEqual(cv.persona_for("reckless"), cv.persona_for("none"))

    def test_messages_carry_persona_and_text_only(self):
        msgs = cv.build_converse_messages("reckless", "Companion how are you")
        self.assertEqual([m["role"] for m in msgs], ["system", "user"])
        self.assertIn("reckless", msgs[1]["content"])
        self.assertIn("Companion how are you", msgs[1]["content"])
        # Never any live world identifiers in the prompt.
        for m in msgs:
            self.assertNotIn("GUID", m["content"])
            self.assertNotIn("position", m["content"])


class SanitizeTest(unittest.TestCase):
    def test_drops_control_chars_and_newlines(self):
        self.assertEqual(cv.sanitize_reply("a\nb\tc"), "abc")
        self.assertEqual(cv.sanitize_reply("x\ry\nz"), "xyz")

    def test_trims_and_strips_leading_dot(self):
        self.assertEqual(cv.sanitize_reply("  .defend on  "), "defend on")
        self.assertEqual(cv.sanitize_reply("..hi"), "hi")

    def test_truncates_to_max(self):
        self.assertEqual(len(cv.sanitize_reply("a" * 300)), cv.MAX_REPLY)

    def test_drops_non_ascii(self):
        self.assertEqual(cv.sanitize_reply("caf\u00e9"), "caf")

    def test_empty_and_non_string(self):
        self.assertEqual(cv.sanitize_reply(""), "")
        self.assertEqual(cv.sanitize_reply(None), "")
        self.assertEqual(cv.sanitize_reply(123), "")


class ParseTest(unittest.TestCase):
    def test_valid(self):
        self.assertEqual(cv.parse_converse_text('{"reply": "On me!"}'), "On me!")

    def test_tolerates_surrounding_prose(self):
        self.assertEqual(
            cv.parse_converse_text('sure {"reply": "I am not sure."} hope it helps'),
            "I am not sure.")

    def test_rejects_wrong_keys(self):
        self.assertEqual(cv.parse_converse_text('{"reply": "x", "extra": 1}'), "")
        self.assertEqual(cv.parse_converse_text('{"other": "x"}'), "")

    def test_rejects_non_string_reply(self):
        self.assertEqual(cv.parse_converse_text('{"reply": 5}'), "")

    def test_rejects_prose_only(self):
        self.assertEqual(
            cv.parse_converse_text("I would follow the leader."), "")

    def test_rejects_empty_reply(self):
        self.assertEqual(cv.parse_converse_text('{"reply": ""}'), "")

    def test_sanitizes_the_parsed_reply(self):
        # A model that leaks a leading dot or an overlong line is bounded.
        self.assertEqual(cv.parse_converse_text('{"reply": ".hold on"}'), "hold on")
        self.assertEqual(len(cv.parse_converse_text(
            '{"reply": "' + "a" * 500 + '"}')), cv.MAX_REPLY)


if __name__ == "__main__":
    unittest.main()