"""R6 (KAP-546 / TW-003): a blocked telemetry consumer must not stall the world thread.

Boots a fresh, port-free two-service lab (disposable MariaDB + the current
world image) with world processing telemetry enabled at a 1 s interval and
its sink pointed at a named pipe (FIFO) that nothing reads. The sink's
dedicated writer thread blocks on opening the FIFO; the world thread keeps
enqueuing into the bounded queue until it is full, then drops and counts.

Validated:
- the bot's periodic player save keeps updating the character row in the
  database while the consumer is blocked (world updates continue);
- when a reader finally opens the FIFO, the writer drains the bounded queue
  and exposes the accumulated drops with a bounded in-band notice;
- graceful world shutdown stays bounded with the consumer still blocked, and
  the sink reports its drop/unsent counters.

Run with a dedicated review image: R6_LAB_IMAGE=tortoise-local:r6-review
python -m unittest docker.test_bot_telemetry_sink
"""
import json
import os
import re
import secrets
import subprocess
import time
import unittest
import uuid
from datetime import datetime, timezone
from pathlib import Path

import test_bot_provision as p

ROOT = p.ROOT
IMAGE = os.environ.get("R6_LAB_IMAGE", p.IMAGE)
FIFO = "/state/telemetry_fifo"
BOT = "Telbot"
QUEUE_CAPACITY = 64
BLOCKED_WINDOW_S = 80


def write_compose(evidence, world_env):
    bind = lambda source, target: {"type": "bind", "source": str(ROOT / source),
                                   "target": target, "read_only": True}
    # Create the FIFO before server.py starts so the sink's writer thread
    # blocks on open (no reader) instead of creating a plain file.
    entrypoint = ["/bin/sh", "-c",
                  "rm -f " + FIFO + "; mkfifo " + FIFO +
                  " && exec python3 /opt/tortoise/server.py world"]
    (evidence / "compose.json").write_text(json.dumps({
        "services": {
            "db": {
                "image": "mariadb:10.11",
                "environment": {"MARIADB_ROOT_PASSWORD": "${BOT_LAB_ROOT_PASSWORD:?}",
                                "DB_PASSWORD": "${BOT_LAB_DB_PASSWORD:?}"},
                "volumes": ["database:/var/lib/mysql", bind("sql", "/bootstrap/sql"),
                            bind("docker/init-db.sh", "/docker-entrypoint-initdb.d/10-tortoise.sh")],
                "healthcheck": {"test": ["CMD", "healthcheck.sh", "--connect", "--innodb_initialized"],
                                "interval": "5s", "timeout": "5s", "retries": 120,
                                "start_period": "5m"},
                "stop_grace_period": "2m",
            },
            "world": {
                "image": IMAGE, "command": ["world"], "entrypoint": entrypoint,
                "environment": world_env,
                "volumes": ["world-state:/state", bind("data", "/data")],
                "stdin_open": True, "tty": True, "stop_grace_period": "2m",
            },
        },
        "volumes": {"database": {}, "world-state": {}},
    }, indent=2), encoding="utf-8")


def boot(project, evidence, world_env):
    write_compose(evidence, world_env)
    env = dict(os.environ, BOT_LAB_ROOT_PASSWORD=secrets.token_hex(24),
               BOT_LAB_DB_PASSWORD=secrets.token_hex(24))
    base = ["-f", str(evidence / "compose.json"), "-p", project]
    p.command(["docker", "compose"] + base + ["config", "--quiet"], env=env)
    p.command(["docker", "compose"] + base + ["up", "-d", "--wait", "--wait-timeout", "600", "db"],
              env=env, timeout=660)
    p.db_exec(base, env, p.MIGRATION.read_text(encoding="utf-8"))
    p.db_exec(base, env, p.PROVISION_MIGRATION.read_text(encoding="utf-8"))
    return base, env


def bot_position(base, env):
    out = p.db_exec(base, env,
                    "SELECT position_x, position_y, position_z FROM characters "
                    "WHERE name = '" + BOT + "'").strip()
    return out or None


class BotTelemetrySinkTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        p.command(["docker", "image", "inspect", IMAGE])
        cls.project = "tortoise-bot-tel-" + uuid.uuid4().hex[:12]
        cls.evidence = ROOT / "local" / (cls.project + "-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ"))
        cls.evidence.mkdir(parents=True)
        cls.base = None
        cls.addClassCleanup(cls.cleanup_lab)
        world_env = {
            "DB_PASSWORD": "${BOT_LAB_DB_PASSWORD:?}",
            "PLAYERBOT_ENABLE": "1", "PLAYERBOT_MIN_BOTS": "1", "PLAYERBOT_MAX_BOTS": "1",
            "PLAYERBOT_REFRESH": "10000", "PLAYERBOT_UPDATE_MS": "5000", "PLAYERBOT_DEBUG": "1",
            "PLAYERBOT_PROVISION": BOT,
            "PLAYER_SAVE_INTERVAL": "5000",
            "PERF_PROCESSING_TELEMETRY": "1",
            "PERF_PROCESSING_TELEMETRY_FILE": json.dumps(FIFO),
        }
        cls.base, cls.env = boot(cls.project, cls.evidence, world_env)
        p.command(["docker", "compose"] + cls.base + ["up", "-d", "--no-deps", "world"], env=cls.env)
        cls.logs = p.wait_for(cls.base, cls.env, lambda l: "World server is up and running!" in l)
        (cls.evidence / "world-startup.log").write_text(cls.logs, encoding="utf-8")
        if "World processing telemetry self-test failed" in cls.logs:
            detail = "\n".join(line for line in cls.logs.splitlines()
                               if "R6 self-test" in line or "telemetry" in line.lower())
            raise AssertionError(
                "telemetry self-test failed at startup (see world-startup.log):\n" + detail)

        # World progress while the consumer is blocked: the bot wanders from
        # its first AI update and the periodic player save (5 s) rewrites the
        # character row in the database.
        cls.position0 = None
        cls.position_moved = False
        end = time.monotonic() + BLOCKED_WINDOW_S
        while time.monotonic() < end:
            pos = bot_position(cls.base, cls.env)
            if pos:
                if cls.position0 is None:
                    cls.position0 = pos
                elif pos != cls.position0:
                    cls.position_moved = True
            time.sleep(10)
        (cls.evidence / "positions.txt").write_text(
            "initial=" + str(cls.position0) + "\nmoved=" + str(cls.position_moved) + "\n",
            encoding="utf-8")

        # Unblock the consumer: the reader opening the FIFO releases the sink
        # writer's open, which then drains the bounded queue.
        cls.read_path = cls.evidence / "sink-read.txt"
        cls.reader_out = open(str(cls.read_path), "wb")
        cls.reader = subprocess.Popen(
            ["docker", "compose"] + cls.base + ["exec", "-T", "world", "sh", "-c", "cat " + FIFO],
            cwd=ROOT, env=cls.env, stdout=cls.reader_out, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 60
        lines = []
        while time.monotonic() < deadline:
            if cls.read_path.exists() and cls.read_path.stat().st_size:
                lines = cls.read_path.read_text(encoding="utf-8", errors="replace").splitlines()
                if any("dropped while the sink queue was full" in line for line in lines):
                    break
            time.sleep(2)
        cls.drain_lines = lines
        (cls.evidence / "sink-read.txt.flush").write_text(
            "\n".join(lines) + ("\n" if lines else ""), encoding="utf-8")

        # Graceful shutdown with the consumer still attached must stay bounded.
        p.command(["docker", "compose"] + cls.base + ["stop", "world"], env=cls.env, timeout=180)
        cls.reader.wait(timeout=60)
        cls.reader_out.close()
        cls.reader = None
        cls.shutdown_logs = p.command(["docker", "compose"] + cls.base +
                                      ["logs", "--no-color", "world"], env=cls.env, timeout=60)
        (cls.evidence / "world-shutdown.log").write_text(cls.shutdown_logs, encoding="utf-8")

    @classmethod
    def cleanup_lab(cls):
        if cls.base is None:
            return
        p.teardown_lab(cls.base, cls.env, cls.project, cls.evidence)

    def test_world_updates_continue_while_consumer_blocked(self):
        self.assertIsNotNone(self.position0, "bot character row must exist")
        self.assertTrue(self.position_moved,
                        "bot position must change in the database while the "
                        "telemetry consumer is blocked (see positions.txt)")

    def test_drops_exposed_and_queue_bounded(self):
        self.assertTrue(any("dropped while the sink queue was full" in line
                            for line in self.drain_lines),
                        "drop notice must be exposed when the consumer recovers")
        notice = next(line for line in self.drain_lines
                      if "dropped while the sink queue was full" in line)
        self.assertEqual(notice, self.drain_lines[0],
                         "drop notice must precede the first drained line")
        dropped = int(re.search(r"(\d+) report line", notice).group(1))
        self.assertGreaterEqual(dropped, 1)
        report_lines = [line for line in self.drain_lines
                        if line.startswith("World processing telemetry:")]
        self.assertGreaterEqual(len(report_lines), QUEUE_CAPACITY,
                                "the full bounded queue must be delivered when the consumer recovers")
        # seq is monotonic per sink instance; strict increase proves the
        # drained lines arrived in enqueue order (with gaps where drops
        # happened).
        seqs = [int(re.search(r"seq=(\d+)", line).group(1)) for line in report_lines]
        self.assertTrue(all(a < b for a, b in zip(seqs, seqs[1:])),
                        "drained report lines must be in enqueue order")

    def test_shutdown_bounded_and_counters_reported(self):
        stop_lines = [line for line in self.shutdown_logs.splitlines()
                      if "World telemetry sink stopped:" in line]
        self.assertTrue(stop_lines, "sink stop counters must be reported at shutdown")
        self.assertRegex(stop_lines[-1], r"dropped_full=\d+ unsent=\d+")
        self.assertIn("Shutting down world...", self.shutdown_logs)
        # The self-test's deliberate unopenable path logs a fail-closed
        # line for its own temp path; only the production FIFO path must
        # open.
        self.assertNotIn("cannot open \'" + FIFO + "\'", self.shutdown_logs,
                         "the FIFO sink must open, not fail closed, in this lab")


if __name__ == "__main__":
    unittest.main()
