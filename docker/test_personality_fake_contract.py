"""PORT-017 (KAP-558): cross-language contract test for the deterministic
fake planner service (docker/personality-service/fake_planner.py) and the
C++ validator anchor (docker/personality-service/planner_validator_cli.cpp).

No Docker, no database, no game server: the golden vectors are byte-pinned
here, in the C++ value test and in the Python reference encoder, and every
scenario must produce the same verdict in both languages. Skips when no
C++ compiler is available.
"""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

DOCKER = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(DOCKER)
SERVICE = os.path.join(DOCKER, "personality-service")
sys.path.insert(0, SERVICE)

import fake_planner as fp  # noqa: E402

GOLDEN_REQUEST_HEX = "434c5031010000008877665544332211d14e09000300000040420f00000000000000000054000000070000000200000002000000d24e090001000000d34e09000400000000000000000000000000000000000000"
GOLDEN_RESPONSE_HEX = "434c5031010000008877665544332211d14e090003000000104a0f00000000000200000068000000d24e090001000000070000000100000000000000f84d0f000000000000000000d34e0900010000000700000003000000c8252600f84d0f000000000000000000"
GOLDEN_NOW_MS = 1002000

EXPECTED = {
    "success": "ok",
    "delay": "ok",
    "timeout": None,
    "malformed": "size-mismatch",
    "oversized": "oversized",
    "unsupported": "unknown-version",
    "stale": "stale",
    "chase": "ok",
    "express": "ok",
}


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


_CLI = {"exe": None, "kind": None, "error": None}


def _compile_cli():
    found = _find_compiler()
    if not found:
        _CLI["error"] = "no C++ compiler available (native or WSL)"
        return None
    kind, compiler = found
    tmp = tempfile.mkdtemp(prefix="p017_cli_")
    src = os.path.join(SERVICE, "planner_validator_cli.cpp")
    include = os.path.join(REPO, "src", "game", "PlayerBots")
    if kind == "wsl":
        exe = os.path.join(tmp, "p017_cli")
        cmd = ["wsl", "-e", "bash", "-lc",
               '%s -std=c++17 -Wall -Wextra -I "%s" -o "%s" "%s"'
               % (compiler, _to_wsl(include), _to_wsl(exe), _to_wsl(src))]
    else:
        exe = os.path.join(tmp, "p017_cli.exe")
        cmd = [compiler, "-std=c++17", "-Wall", "-Wextra",
               "-I", include, "-o", exe, src]
    build = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    if build.returncode != 0:
        _CLI["error"] = "cli compile failed:\n" + build.stdout + build.stderr
        return None
    _CLI["exe"] = exe
    _CLI["kind"] = kind
    return exe


def _cli(mode, now_ms, hex_payload):
    if _CLI["exe"] is None and not _compile_cli():
        raise unittest.SkipTest(_CLI["error"])
    if _CLI["kind"] == "wsl":
        args = ["wsl", _to_wsl(_CLI["exe"]), mode, str(now_ms), hex_payload]
    else:
        args = [_CLI["exe"], mode, str(now_ms), hex_payload]
    proc = subprocess.run(args, capture_output=True, text=True, timeout=60)
    if proc.returncode != 0:
        raise AssertionError("cli failed: " + proc.stdout + proc.stderr)
    return proc.stdout.strip()


class FakePlannerContractTest(unittest.TestCase):
    def _golden(self):
        return fp.make_request(fp.golden_request()), fp.golden_response()

    def test_golden_vectors_pin(self):
        req, res = self._golden()
        self.assertEqual(len(req), 84)
        self.assertEqual(len(res), 104)
        self.assertEqual(req.hex(), GOLDEN_REQUEST_HEX)
        self.assertEqual(res.hex(), GOLDEN_RESPONSE_HEX)
        self.assertEqual(fp.GOLDEN_NOW_MS, GOLDEN_NOW_MS)
        # The embedded vectors validate in both languages.
        self.assertEqual(_cli("request", GOLDEN_NOW_MS, req.hex()), "ok")
        self.assertEqual(_cli("response", GOLDEN_NOW_MS, res.hex()), "ok")

    def test_scenarios_both_languages(self):
        req, _ = self._golden()
        for scenario in fp.SCENARIOS:
            with self.subTest(scenario=scenario):
                payload, meta = fp.respond(req, scenario)
                if payload is None:
                    # timeout: the deadline passes first; the caller keeps
                    # the deterministic behavior (no model action at all).
                    self.assertIsNone(payload)
                    self.assertEqual(meta["deadline_ms"], 5000)
                    self.assertGreater(meta["delay_ms"], 5000)
                    continue
                expected = EXPECTED[scenario]
                self.assertEqual(
                    fp.validate_response(req, payload, fp.GOLDEN_NOW_MS), expected,
                    "python verdict for %s" % scenario)
                self.assertEqual(
                    _cli("response", fp.GOLDEN_NOW_MS, payload.hex()), expected,
                    "cpp verdict for %s" % scenario)

    def test_determinism(self):
        req, _ = self._golden()
        for scenario in ("success", "delay"):
            p1, m1 = fp.respond(req, scenario)
            p2, m2 = fp.respond(req, scenario)
            self.assertEqual(p1, p2)
            self.assertEqual(m1, m2)

    def test_vocabulary_closure(self):
        req, res = self._golden()
        step1_action = fp.ENVELOPE + fp.STEP * 0 + 12  # first step action byte
        for action in range(8, 256):
            with self.subTest(action=action):
                mut = bytearray(res)
                mut[step1_action] = action
                self.assertEqual(
                    fp.validate_response(req, bytes(mut), fp.GOLDEN_NOW_MS),
                    "unknown-action")
        # spot-check the C++ anchor at the edges of the same range
        for action in (8, 127, 255):
            mut = bytearray(res)
            mut[step1_action] = action
            self.assertEqual(
                _cli("response", fp.GOLDEN_NOW_MS, mut.hex()), "unknown-action")

    def test_payload_sizes(self):
        req, res = self._golden()
        self.assertEqual(len(req), 84)
        self.assertEqual(len(res), 104)
        self.assertLessEqual(len(req), fp.MAX_PAYLOAD_BYTES)
        self.assertLessEqual(len(res), fp.MAX_PAYLOAD_BYTES)
        hold = {"bot_guid": 610002, "login_generation": 1,
                "order_generation": 7, "action": fp.ACTION_HOLD,
                "target_guid": 0, "preference": 0}
        one = fp.build_response(req, [hold])
        eight = fp.build_response(req, [
            dict(hold, bot_guid=610002 + i) for i in range(fp.MAX_STEPS)])
        self.assertEqual(len(one), 72)
        self.assertEqual(len(eight), 296)
        # 296 is the fixed maximum response size (envelope + 8 steps)


if __name__ == "__main__":
    unittest.main()
