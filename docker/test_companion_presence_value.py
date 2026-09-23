"""Standalone value test for Companion/Presence.h."""
import os
import shutil
import subprocess
import tempfile
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(REPO, "docker", "test_companion_presence_value.cpp")
INCLUDE = os.path.join(REPO, "src", "game", "PlayerBots")


class CompanionPresenceValueTest(unittest.TestCase):
    def test_presence_catalog(self):
        compiler = next((shutil.which(n) for n in ("g++", "clang++", "c++") if shutil.which(n)), None)
        if not compiler:
            self.skipTest("no native C++ compiler available")
        with tempfile.TemporaryDirectory() as tmp:
            exe = os.path.join(tmp, "presence_value.exe")
            build = subprocess.run(
                [compiler, "-std=c++17", "-Wall", "-Wextra", "-I", INCLUDE, "-o", exe, SRC],
                capture_output=True, text=True, timeout=120)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([exe], capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn("presence value tests: ALL OK", run.stdout)


if __name__ == "__main__":
    unittest.main()
