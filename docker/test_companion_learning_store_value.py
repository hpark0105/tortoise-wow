"""BL-003: value-level runner for Companion/LearningStore.h.

Compiles docker/test_companion_learning_store_value.cpp with any available
C++ compiler (native or WSL fallback) and checks the module's pure FIFO
logic. No Docker, no database, no game server needed. Skips when no
compiler is available.
"""
import os
import shutil
import subprocess
import tempfile
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(REPO, "docker", "test_companion_learning_store_value.cpp")
INCLUDE = os.path.join(REPO, "src", "game", "PlayerBots")
MGR_HEADER = os.path.join(INCLUDE, "PlayerBotMgr.h")
MGR_SOURCE = os.path.join(INCLUDE, "PlayerBotMgr.cpp")


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


class CompanionLearningStoreValueTest(unittest.TestCase):
    def test_async_status_query_wiring(self):
        with open(MGR_HEADER, encoding="utf-8") as stream:
            header = stream.read()
        with open(MGR_SOURCE, encoding="utf-8") as stream:
            source = stream.read()

        self.assertIn("bool QueueLearningStatus(uint32 charGuid, uint32 issuerGuid = 0, bool explain = false);", header)
        self.assertIn("void OnLearningStatusResult(QueryResult* result, uint32 charGuid,", header)
        self.assertIn("BeginExternalControl(charGuid, false)", source)
        self.assertIn("CharacterDatabase.AsyncPQuery", source)
        self.assertIn("kControlStatusSqlTemplate", source)
        self.assertIn("result->GetFieldCount() != 3", source)
        self.assertIn("result->GetRowCount() != 1", source)
        self.assertIn("status.insufficientEvidence = true", source)
        self.assertIn("CompleteExternalControl(true, &status)", source)

    def test_owner_command_wiring(self):
        paths = {
            "chat": os.path.join(REPO, "src", "game", "Chat", "Chat.cpp"),
            "commands": os.path.join(REPO, "src", "game", "Commands", "Commands.cpp"),
        }
        with open(paths["chat"], encoding="utf-8") as stream:
            chat = stream.read()
        with open(paths["commands"], encoding="utf-8") as stream:
            commands = stream.read()
        with open(MGR_SOURCE, encoding="utf-8") as stream:
            manager = stream.read()

        self.assertIn('{ "botlearn",       SEC_PLAYER', chat)
        self.assertIn("HandleBotLearnCommand", commands)
        self.assertIn("ValidatePartyOwner(issuer, entry, \"learn\")", manager)
        for action in ("start", "status", "pause", "resume", "explain", "rollback"):
            self.assertIn('action == "%s"' % action, manager + commands)
        self.assertIn("QueueLearningStatus(charGuid, issuer->GetGUIDLow()", manager)
        self.assertIn("QueueLearningRollback(charGuid)", manager)

    def test_learning_store_fifo(self):
        found = _find_compiler()
        if not found:
            self.skipTest("no C++ compiler available (native or WSL)")
        kind, compiler = found
        with tempfile.TemporaryDirectory() as tmp:
            if kind == "wsl":
                exe = os.path.join(tmp, "bl003_value")
                cmd = ["wsl", "-e", "bash", "-lc",
                       '%s -std=c++17 -Wall -Wextra -I "%s" -o "%s" "%s"'
                       % (compiler, _to_wsl(INCLUDE), _to_wsl(exe), _to_wsl(SRC))]
            else:
                exe = os.path.join(tmp, "bl003_value.exe")
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
            self.assertIn("learning store value tests: ALL OK", run.stdout)


if __name__ == "__main__":
    unittest.main()
