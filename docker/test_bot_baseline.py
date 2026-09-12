import copy
from pathlib import Path
import unittest
from unittest.mock import patch
import subprocess

from bot_baseline import command, lab_config, percentile, verify_lab_containers


class BotLabSafetyTests(unittest.TestCase):
    def setUp(self):
        self.project = "tortoise-bot-lab-0123456789ab"
        self.container = {"Config": {"Labels": {"com.docker.compose.project": self.project}},
                          "HostConfig": {"PortBindings": {}},
                          "Mounts": [{"Type": "volume", "Name": self.project + "_database"},
                                     {"Type": "bind", "RW": False}]}

    def test_only_generated_project_can_be_cleaned(self):
        verify_lab_containers(self.project, [self.container])
        for project in ("tortoise-local", "tortoise-bot-lab-", self.project + "-extra"):
            with self.assertRaises(ValueError):
                verify_lab_containers(project, [self.container])

    def test_reject_personal_mount_ports_and_wrong_ownership(self):
        cases = []
        value = copy.deepcopy(self.container)
        value["Mounts"][0]["Name"] = "tortoise-local_database"
        cases.append(value)
        value = copy.deepcopy(self.container)
        value["HostConfig"]["PortBindings"] = {"8085/tcp": [{"HostPort": "8085"}]}
        cases.append(value)
        value = copy.deepcopy(self.container)
        value["Config"]["Labels"]["com.docker.compose.project"] = "tortoise-local"
        cases.append(value)
        value = copy.deepcopy(self.container)
        value["Mounts"][1]["RW"] = True
        cases.append(value)
        for value in cases:
            with self.subTest(value=value), self.assertRaises(ValueError):
                verify_lab_containers(self.project, [value])

    def test_config_has_no_personal_volume_backup_mount_or_published_port(self):
        config = lab_config(Path("C:/test"), "test:image")
        self.assertEqual(config["volumes"], {"database": {}, "world-state": {}})
        self.assertEqual(set(config["services"]), {"db", "world"})
        for service in config["services"].values():
            self.assertNotIn("ports", service)
            self.assertNotIn("env_file", service)
            for mount in service["volumes"]:
                if isinstance(mount, dict):
                    self.assertTrue(mount["read_only"])
                    self.assertNotIn("backup", mount["source"])

    def test_nearest_rank_percentile(self):
        self.assertEqual(percentile(list(range(1, 101)), .95), 95)
        self.assertEqual(percentile([8], .95), 8)

    def test_bash_stdin_preserves_lf_on_windows(self):
        with patch("bot_baseline.subprocess.run", return_value=subprocess.CompletedProcess(
                ["docker"], 0, b"ok\n", b"")) as run:
            self.assertEqual(command(["docker"], script="set -e\ntrue\n"), "ok")
        self.assertEqual(run.call_args.kwargs["input"], b"set -e\ntrue\n")


if __name__ == "__main__":
    unittest.main()
