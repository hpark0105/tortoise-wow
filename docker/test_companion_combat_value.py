"""PORT-012 (KAP-558): value-level runner for Companion/Combat.h.

Compiles docker/test_companion_combat_value.cpp with any available C++
compiler (native or WSL fallback) and checks the module's pure logic.
No Docker, no database, no game server needed. Skips when no compiler is
available.
"""
import os
import shutil
import subprocess
import tempfile
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(REPO, "docker", "test_companion_combat_value.cpp")
INCLUDE = os.path.join(REPO, "src", "game", "PlayerBots")


def _to_wsl(path):
    p = path.replace("\\", "/")
    if p.startswith("C:"):
        p = "/mnt/c" + p[2:]
    return p


def _find_compiler():
    for name in ("g++", "clang++", "c++"):
        found = shutil.which(name)
        if found:
            return "native", found
    try:
        proc = subprocess.run(
            ["wsl", "-e", "bash", "-lc", "command -v g++ || command -v clang++"],
            capture_output=True, text=True, timeout=60)
        out = proc.stdout.strip()
        if proc.returncode == 0 and out:
            return "wsl", out.splitlines()[-1].strip()
    except (OSError, subprocess.SubprocessError):
        pass
    return None


class CompanionCombatValueTest(unittest.TestCase):
    def test_combat_value_module(self):
        found = _find_compiler()
        if not found:
            self.skipTest("no C++ compiler available (native or WSL)")
        kind, compiler = found
        with tempfile.TemporaryDirectory() as tmp:
            if kind == "wsl":
                exe = os.path.join(tmp, "p012_value")
                cmd = ["wsl", "-e", "bash", "-lc",
                       '%s -std=c++17 -Wall -Wextra -I "%s" -o "%s" "%s"'
                       % (compiler, _to_wsl(INCLUDE), _to_wsl(exe), _to_wsl(SRC))]
            else:
                exe = os.path.join(tmp, "p012_value.exe")
                cmd = [compiler, "-std=c++17", "-Wall", "-Wextra",
                       "-I", INCLUDE, "-o", exe, SRC]
            build = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
            self.assertEqual(build.returncode, 0,
                             "compile failed:\n" + build.stdout + build.stderr)
            if kind == "wsl":
                run = subprocess.run(["wsl", _to_wsl(exe)],
                                     capture_output=True, text=True, timeout=120)
            else:
                run = subprocess.run([exe], capture_output=True, text=True, timeout=120)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn("value tests: ALL OK", run.stdout)


if __name__ == "__main__":
    unittest.main()
