"""Configuration wiring for optional world processing-time telemetry."""
import unittest
from pathlib import Path

import bot_baseline
import server


class TelemetryConfTests(unittest.TestCase):
    def test_render_replaces_template_telemetry_line(self):
        template = "Perf.ReportInterval = 600\nPerf.ProcessingTelemetry = 0\nMotd = hi\n"
        output = server.render(template, {"Perf.ProcessingTelemetry": "15", "Motd": "hello"})
        self.assertIn("Perf.ProcessingTelemetry = 15", output)
        self.assertIn("Motd = hello", output)
        self.assertEqual(output.count("Perf.ProcessingTelemetry"), 1)

    def test_render_appends_unknown_telemetry_key(self):
        output = server.render("Motd = hi\n", {"Perf.ProcessingTelemetry": "30"})
        self.assertIn("Perf.ProcessingTelemetry = 30", output)

    def test_render_replaces_telemetry_file_line(self):
        template = 'Perf.ProcessingTelemetry = 5\nPerf.ProcessingTelemetryFile = "x.log"\n'
        output = server.render(template, {"Perf.ProcessingTelemetryFile": '"/state/t.log"'})
        self.assertIn('Perf.ProcessingTelemetryFile = "/state/t.log"', output)
        self.assertEqual(output.count("Perf.ProcessingTelemetryFile"), 1)

    def test_render_replaces_player_save_interval(self):
        output = server.render("PlayerSave.Interval = 60000\n", {"PlayerSave.Interval": "5000"})
        self.assertIn("PlayerSave.Interval = 5000", output)

    def test_lab_config_omits_telemetry_env_by_default(self):
        config = bot_baseline.lab_config(Path("."), "tortoise-local:dev")
        self.assertNotIn("PERF_PROCESSING_TELEMETRY", config["services"]["world"]["environment"])

    def test_lab_config_sets_telemetry_env(self):
        config = bot_baseline.lab_config(Path("."), "tortoise-local:dev", 15)
        self.assertEqual(config["services"]["world"]["environment"]["PERF_PROCESSING_TELEMETRY"], "15")


if __name__ == "__main__":
    unittest.main()
