"""Value-level safety checks for the party-only combat debrief prompt."""
import os
import sys
import threading
import unittest
import urllib.error
import urllib.request
from http.server import ThreadingHTTPServer
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "personality-service"))
import combat_review as cr
import real_planner_server as service


class CombatReviewTests(unittest.TestCase):
    def test_bounded_counters_and_persona(self):
        messages = cr.build_messages(
            "cautious",
            "duration_ms=12000 damage=80 taken=14 casts_ok=2 casts_rejected=1 decisions=5")
        self.assertEqual(len(messages), 2)
        self.assertIn("cautious", messages[1]["content"])
        self.assertIn("counters", messages[0]["content"])
        self.assertNotIn("target", messages[1]["content"])

    def test_rejects_extra_facts_and_unbounded_values(self):
        base = "duration_ms=12000 damage=80 taken=14 casts_ok=2 casts_rejected=1 decisions=5"
        self.assertIsNone(cr.build_messages("none", base + " target=wolf"))
        self.assertIsNone(cr.build_messages("none", base.replace("12000", "9999999")))
        self.assertIsNone(cr.build_messages("none", "damage=-1"))
        self.assertIsNone(cr.build_messages("none", "x" * 201))

    def test_accepts_bounded_party_memory(self):
        messages = cr.build_messages(
            "none",
            "duration_ms=12000 damage=80 taken=220 casts_ok=2 "
            "casts_rejected=1 decisions=6 memory=caution")
        self.assertIn("Recent party memory: caution", messages[1]["content"])


class CombatReviewAdapterTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = ThreadingHTTPServer(("127.0.0.1", 0), service.Handler)
        cls.worker = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.worker.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.worker.join(timeout=5)

    def post(self, body):
        request = urllib.request.Request(
            "http://127.0.0.1:%d/combat-review?profile=cautious" %
            self.server.server_port, data=body, method="POST")
        with urllib.request.urlopen(request, timeout=5) as response:
            return response.status, response.read()

    def test_model_reply_is_personal_and_text_only(self):
        with mock.patch.object(service.mc, "read_api_key", return_value="test-only"), \
             mock.patch.object(service.mc, "fetch_model_id", return_value="test-model"), \
             mock.patch.object(service.mc, "chat", return_value=(
                 '{"reply":"I should mind my timing next time."}', {})) as chat:
            service.MODEL_ID["id"] = None
            self.assertEqual(self.post(
                b"duration_ms=12000 damage=80 taken=14 casts_ok=2 "
                b"casts_rejected=1 decisions=5"),
                (200, b"I should mind my timing next time."))
            self.assertIn("cautious", chat.call_args.args[3][1]["content"])

    def test_invalid_payload_and_busy_model_fail_closed(self):
        with mock.patch.object(service.mc, "chat") as chat:
            with self.assertRaises(urllib.error.HTTPError) as caught:
                self.post(b"target=wolf damage=1")
            self.assertEqual(caught.exception.code, 400)
            chat.assert_not_called()
        service.MODEL_LOCK.acquire()
        try:
            with self.assertRaises(urllib.error.HTTPError) as caught:
                self.post(b"duration_ms=1 damage=1 taken=0 casts_ok=0 "
                          b"casts_rejected=0 decisions=1")
            self.assertEqual(caught.exception.code, 503)
        finally:
            service.MODEL_LOCK.release()


if __name__ == "__main__":
    unittest.main()
