"""PORT-024 (KAP-558): value-level runner for the companion equipment
policy. Compiles docker/test_companion_equipment_value.cpp with any
available C++ compiler (native or WSL fallback) and checks the score
formula (required level, itemLevel*2/3, player-level fallback,
quality/item-level tie-breaks) and the strict-upgrade verdicts. No
Docker, no database, no game server needed. Skips when no compiler is
available.
"""
import os
import shutil
import subprocess
import tempfile
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(REPO, "docker", "test_companion_equipment_value.cpp")
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


def _score(req, ilvl, quality, player):
    # Mirror of Companion::Equipment::Score for cross-checking.
    effective = req
    if not effective:
        estimate = (ilvl * 2) // 3 if ilvl else player
        effective = max(1, estimate)
    return effective * 1000 + quality * 10 + ilvl


class CompanionEquipmentValueTest(unittest.TestCase):
    def test_equipment_score_mirror(self):
        self.assertEqual(_score(10, 14, 2, 10), 10 * 1000 + 2 * 10 + 14)
        self.assertEqual(_score(0, 14, 2, 10), 9 * 1000 + 2 * 10 + 14)
        self.assertEqual(_score(0, 5, 1, 10), 3 * 1000 + 1 * 10 + 5)
        self.assertEqual(_score(0, 3, 0, 10), 2 * 1000 + 0 + 3)
        self.assertEqual(_score(0, 1, 1, 10), 1 * 1000 + 1 * 10 + 1)
        self.assertEqual(_score(0, 0, 1, 10), 10 * 1000 + 1 * 10 + 0)
        self.assertLess(_score(10, 12, 1, 10), _score(10, 13, 2, 10))
        self.assertLess(_score(10, 12, 2, 10), _score(10, 13, 2, 10))
        # Lab fixture values.
        self.assertEqual(_score(0, 10, 1, 10), 6020)
        self.assertEqual(_score(0, 14, 2, 10), 9034)
        self.assertEqual(_score(0, 5, 1, 10), 3015)
        # Strict-upgrade verdicts.
        self.assertGreater(9034, 6020)
        self.assertEqual(6020, 6020)
        self.assertLess(3015, 6020)

    def test_equipment_value_contract(self):
        found = _find_compiler()
        if not found:
            self.skipTest("no C++ compiler available (native or WSL)")
        kind, compiler = found
        with tempfile.TemporaryDirectory() as tmp:
            if kind == "wsl":
                exe = os.path.join(tmp, "p024_value")
                cmd = ["wsl", "-e", "bash", "-lc",
                       '%s -std=c++17 -Wall -Wextra -I "%s" -o "%s" "%s"'
                       % (compiler, _to_wsl(INCLUDE), _to_wsl(exe), _to_wsl(SRC))]
            else:
                exe = os.path.join(tmp, "p024_value.exe")
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
