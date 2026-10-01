"""Compile and run the value-level citizen equipment quality policy checks."""
import os
import shutil
import subprocess
import tempfile
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(REPO, "docker", "test_citizen_gear_policy.cpp")
INCLUDE = os.path.join(REPO, "src", "game", "PlayerBots", "Companion")


def _to_wsl(path):
    normalized = path.replace("\\", "/")
    if normalized.startswith("C:"):
        normalized = "/mnt/c" + normalized[2:]
    return normalized


def _find_compiler():
    for name in ("g++", "clang++", "c++"):
        found = shutil.which(name)
        if found:
            return "native", found
    try:
        proc = subprocess.run(
            ["wsl", "-e", "bash", "-lc", "command -v g++ || command -v clang++"],
            capture_output=True, text=True, timeout=60)
        if proc.returncode == 0 and proc.stdout.strip():
            return "wsl", proc.stdout.strip().splitlines()[-1].strip()
    except (OSError, subprocess.SubprocessError):
        pass
    return None


class CitizenGearPolicyTest(unittest.TestCase):
    def test_quality_probability_boundaries(self):
        found = _find_compiler()
        if not found:
            self.skipTest("no C++ compiler available (native or WSL)")
        kind, compiler = found
        with tempfile.TemporaryDirectory() as tmp:
            if kind == "wsl":
                exe = os.path.join(tmp, "citizen-gear-policy")
                cmd = ["wsl", "-e", "bash", "-lc",
                       '%s -std=c++17 -Wall -Wextra -I "%s" -o "%s" "%s"'
                       % (compiler, _to_wsl(INCLUDE), _to_wsl(exe), _to_wsl(SRC))]
            else:
                exe = os.path.join(tmp, "citizen-gear-policy.exe")
                cmd = [compiler, "-std=c++17", "-Wall", "-Wextra",
                       "-I", INCLUDE, "-o", exe, SRC]
            build = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run(["wsl", _to_wsl(exe)] if kind == "wsl" else [exe],
                                 capture_output=True, text=True, timeout=120)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn("gear policy tests: ALL OK", run.stdout)


if __name__ == "__main__":
    unittest.main()
