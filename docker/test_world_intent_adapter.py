"""Loopback-only /world-intent contract; local model calls are stubbed."""
import os
import sys
import threading
import unittest
import urllib.error
import urllib.request
from http.server import ThreadingHTTPServer
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "personality-service"))
import real_planner_server as service


class WorldIntentAdapterTest(unittest.TestCase):
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
            "http://127.0.0.1:%d/world-intent?profile=cautious" % self.server.server_port,
            data=body, method="POST")
        with urllib.request.urlopen(request, timeout=5) as response:
            return response.status, response.read()

    def test_strict_model_intent_round_trip(self):
        with mock.patch.object(service.mc, "read_api_key", return_value="test-only"), \
             mock.patch.object(service.mc, "fetch_model_id", return_value="test-model") as fetch, \
             mock.patch.object(service.mc, "chat", return_value=(
                 '{"intent":"rest"}', {})) as chat:
            service.MODEL_ID["id"] = None
            self.assertEqual(self.post(b"Worldbota|roam"), (200, b"rest"))
            self.assertEqual(chat.call_count, 1)
            self.assertIn("Worldbota", chat.call_args.args[3][1]["content"])
            self.assertEqual(chat.call_args.args[4], 1.5)
            self.assertEqual(fetch.call_args.kwargs["timeout_s"], 0.5)

    def test_invalid_request_never_reaches_model(self):
        with mock.patch.object(service.mc, "chat") as chat:
            with self.assertRaises(urllib.error.HTTPError) as caught:
                self.post(b"Worldbota|attack")
            self.assertEqual(caught.exception.code, 400)
            chat.assert_not_called()

    def test_malicious_output_fails_closed(self):
        with mock.patch.object(service.mc, "read_api_key", return_value="test-only"), \
             mock.patch.object(service.mc, "fetch_model_id", return_value="test-model"), \
             mock.patch.object(service.mc, "chat", return_value=(
                 '{"intent":"attack"}', {})):
            service.MODEL_ID["id"] = None
            with self.assertRaises(urllib.error.HTTPError) as caught:
                self.post(b"Worldbota|roam")
            self.assertEqual(caught.exception.code, 503)

    def test_busy_model_fails_closed(self):
        service.MODEL_LOCK.acquire()
        try:
            with self.assertRaises(urllib.error.HTTPError) as caught:
                self.post(b"Worldbota|roam")
            self.assertEqual(caught.exception.code, 503)
        finally:
            service.MODEL_LOCK.release()

    def test_model_http_error_invalidates_cached_id(self):
        service.MODEL_ID["id"] = "stale-model"
        with mock.patch.object(service.mc, "read_api_key", return_value="test-only"), \
             mock.patch.object(service.mc, "chat", side_effect=service.mc.ModelError("http-error")):
            with self.assertRaises(urllib.error.HTTPError) as caught:
                self.post(b"Worldbota|roam")
            self.assertEqual(caught.exception.code, 503)
            self.assertIsNone(service.MODEL_ID["id"])


if __name__ == "__main__":
    unittest.main()
