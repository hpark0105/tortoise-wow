"""Measure container resources or boot a fresh, port-free bot test world.

python docker/bot_baseline.py --seconds 120
python docker/bot_baseline.py --isolated --seconds 120
No player data is copied. This is an idle baseline, not a gameplay benchmark.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import re
import secrets
import statistics
import subprocess
import time
import uuid

ROOT = Path(__file__).resolve().parent.parent
PROJECT_PATTERN = r"tortoise-bot-lab-[a-f0-9]{12}"


def command(args, *, env=None, script=None, timeout=60):
    result = subprocess.run(args, cwd=ROOT, env=env,
                            input=script.encode("utf-8") if script is not None else None,
                            capture_output=True, timeout=timeout)
    if result.returncode:
        # Do not echo subprocess inputs, resolved Compose environment or credentials.
        raise RuntimeError(f"Command failed ({result.returncode}): {args[0]}")
    return result.stdout.decode("utf-8", errors="replace").strip()


def lab_config(root, image, telemetry_interval=0):
    """Standalone configuration: never inherit personal mounts or .env secrets."""
    bind = lambda source, target: {
        "type": "bind", "source": str(root / source), "target": target,
        "read_only": True,
    }
    world_env = {"DB_PASSWORD": "${BOT_LAB_DB_PASSWORD:?}"}
    if telemetry_interval:
        world_env["PERF_PROCESSING_TELEMETRY"] = str(telemetry_interval)
    return {
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
                "image": image, "command": ["world"],
                "environment": world_env,
                "volumes": ["world-state:/state", bind("data", "/data")],
                "stdin_open": True, "tty": True, "stop_grace_period": "2m",
            },
        },
        "volumes": {"database": {}, "world-state": {}},
    }


def verify_lab_containers(project, containers):
    """Fail closed before destroying resources from a generated project."""
    if not re.fullmatch(PROJECT_PATTERN, project):
        raise ValueError("Not a generated bot lab project")
    for container in containers:
        labels = container["Config"]["Labels"]
        if labels.get("com.docker.compose.project") != project:
            raise ValueError("Container belongs to a different project")
        if container["HostConfig"].get("PortBindings"):
            raise ValueError("Bot lab unexpectedly publishes ports")
        for mount in container["Mounts"]:
            if mount["Type"] == "volume" and mount["Name"] not in {
                project + "_database", project + "_world-state"
            }:
                raise ValueError("Unexpected volume in bot lab")
            if mount["Type"] == "bind" and mount["RW"]:
                raise ValueError("Writable host mount in bot lab")


def percentile(values, p):
    return sorted(values)[max(0, math.ceil(len(values) * p) - 1)]


def sample_resources(ids, seconds, interval, evidence):
    samples = []
    start = time.monotonic()
    next_progress = 30
    while time.monotonic() - start < seconds:
        raw = command(["docker", "stats", "--no-stream", "--format", "{{json .}}", *ids])
        for line in raw.splitlines():
            row = json.loads(line)
            samples.append({"elapsed_s": round(time.monotonic() - start, 3),
                            "name": row["Name"], "cpu_pct": float(row["CPUPerc"].rstrip("%")),
                            "memory": row["MemUsage"], "block_io": row["BlockIO"],
                            "network_io": row["NetIO"], "pids": row["PIDs"]})
        remaining = seconds - (time.monotonic() - start)
        if time.monotonic() - start >= next_progress:
            print(f"Sampled {round(time.monotonic() - start)} / {seconds} seconds.", flush=True)
            next_progress += 30
        if remaining > 0:
            time.sleep(min(interval, remaining))
    summary = {}
    for name in sorted({row["name"] for row in samples}):
        rows = [row for row in samples if row["name"] == name]
        cpu = [row["cpu_pct"] for row in rows]
        summary[name] = {"samples": len(rows), "cpu_mean_pct": round(statistics.mean(cpu), 2),
                         "cpu_p95_pct": percentile(cpu, .95), "cpu_max_pct": max(cpu),
                         "memory_first": rows[0]["memory"], "memory_last": rows[-1]["memory"]}
    (evidence / "samples.json").write_text(json.dumps(samples, indent=2) + "\n", encoding="utf-8")
    return {"elapsed_s": round(time.monotonic() - start, 2), "containers": summary}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--isolated", action="store_true")
    parser.add_argument("--image", default="tortoise-local:content-fixes")
    parser.add_argument("--seconds", type=int, default=120)
    parser.add_argument("--interval", type=int, default=5)
    parser.add_argument("--telemetry-interval", type=int, default=0)
    args = parser.parse_args()
    if args.seconds < 10 or not 1 <= args.interval <= 60 or not 0 <= args.telemetry_interval <= 3600:
        parser.error("Use at least 10 seconds, an interval from 1 to 60 seconds, and a telemetry interval from 0 to 3600 seconds")
    project = "tortoise-bot-lab-" + uuid.uuid4().hex[:12] if args.isolated else "tortoise-local"
    evidence = ROOT / "local" / (project + "-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ"))
    evidence.mkdir(parents=True)
    env = os.environ.copy()
    if args.isolated:
        env.update(BOT_LAB_ROOT_PASSWORD=secrets.token_hex(24), BOT_LAB_DB_PASSWORD=secrets.token_hex(24))
        config = evidence / "compose.json"
        config.write_text(json.dumps(lab_config(ROOT, args.image, args.telemetry_interval), indent=2), encoding="utf-8")
        empty_env = evidence / "empty.env"
        empty_env.touch()
        base = ["docker", "compose", "--env-file", str(empty_env), "-f", str(config), "-p", project]
    else:
        base = ["docker", "compose", "-f", str(ROOT / "compose.yaml"), "-p", project]
    run = lambda *parts, **kw: command(base + list(parts), env=env, **kw)

    def aggregate_counts():
        sql = ("SELECT COUNT(*) FROM tw_char.playerbot; "
               "SELECT COUNT(*) FROM tw_char.characters WHERE online=1; "
               "SELECT COUNT(*) FROM tw_char.characters;")
        output = run("exec", "-T", "db", "bash", script=
                     'set -e\nexport MYSQL_PWD="$MARIADB_ROOT_PASSWORD"\n'
                     "mariadb --user=root --batch --skip-column-names <<'BOT_SQL'\n" + sql + "\nBOT_SQL\n")
        roster, online, characters = map(int, output.splitlines())
        return {"roster_rows": roster, "online_character_rows": online, "character_rows": characters,
                "note": "DB online flags are not an authoritative active-session counter"}

    report = {"project": project, "isolated": args.isolated, "kind": "idle-resource-baseline",
              "source_commit": command(["git", "rev-parse", "HEAD"]),
              "harness_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              "started_utc": datetime.now(timezone.utc).isoformat(),
              "tick_processing_percentiles": "not measured", "gameplay_capacity": "not measured",
              "completed": False}
    try:
        if args.isolated:
            run("config", "--quiet")
            print("Bootstrapping fresh bot lab database (no personal data).", flush=True)
            run("up", "-d", "--wait", "--wait-timeout", "900", "db", timeout=960)
            run("up", "-d", "--no-deps", "world")
            deadline = time.monotonic() + 300
            while True:
                logs = run("logs", "--no-color", "world")
                (evidence / "startup.log").write_text(logs, encoding="utf-8")
                if "World server is up and running!" in logs:
                    break
                if time.monotonic() >= deadline or "world" not in run("ps", "--services", "--status", "running").splitlines():
                    raise RuntimeError("Lab world failed to become ready; inspect local startup.log")
                time.sleep(2)
            print("Fresh world ready; sampling zero-bot baseline.", flush=True)
        ids = run("ps", "-q", "db", "world").splitlines()
        if len(ids) != 2:
            raise RuntimeError("Expected running world and database")
        inspect = json.loads(command(["docker", "inspect", *ids]))
        if args.isolated:
            verify_lab_containers(project, inspect)
        report["images"] = {row["Name"].lstrip("/"): row["Image"] for row in inspect}
        report["docker"] = json.loads(command(["docker", "info", "--format", '{"cpus":{{.NCPU}},"memory_bytes":{{.MemTotal}}}']))
        # Read only bot keys; never copy the generated database connection strings.
        settings = run("exec", "-T", "world", "python3", "-c",
                       "from pathlib import Path; import re; print('\\n'.join(x for x in Path('/state/mangosd.conf').read_text().splitlines() if re.match(r'^PlayerBot\\.(Enable|MinBots|MaxBots)\\s*=', x)))")
        report["bot_settings"] = settings.splitlines()
        if not re.search(r"^PlayerBot.Enable\s*=\s*0\s*$", settings, re.M):
            raise RuntimeError("Zero-bot baseline requires PlayerBot.Enable=0")
        report["counts_before"] = aggregate_counts()
        if args.isolated and report["counts_before"]["character_rows"] != 0:
            raise RuntimeError("Fresh zero-bot lab unexpectedly contains characters")
        report["resources"] = sample_resources(ids, args.seconds, args.interval, evidence)
        if args.isolated and args.telemetry_interval:
            telemetry_lines = [line for line in run("logs", "--no-color", "world").splitlines()
                               if "World processing telemetry:" in line]
            (evidence / "telemetry.log").write_text(
                "\n".join(telemetry_lines) + ("\n" if telemetry_lines else ""), encoding="utf-8")
            report["tick_processing_percentiles"] = (telemetry_lines[-1] if telemetry_lines
                                                     else "no telemetry lines found in world logs")
        report["counts_after"] = aggregate_counts()
        report["completed"] = True
        print(json.dumps(report["resources"], indent=2), flush=True)
    finally:
        (evidence / "result.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        if args.isolated:
            ids = run("ps", "-a", "-q").splitlines()
            inspect = json.loads(command(["docker", "inspect", *ids])) if ids else []
            verify_lab_containers(project, inspect)
            run("down", "--volumes", timeout=300)
            report["cleanup_completed"] = True
            (evidence / "result.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
            print("Removed only generated bot lab containers and volumes.", flush=True)
        print("Evidence: " + str(evidence), flush=True)


if __name__ == "__main__":
    main()
