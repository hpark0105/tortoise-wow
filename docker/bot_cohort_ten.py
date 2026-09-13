"""TW-013: observe ten persistent bots for a bounded 30-minute lab run."""
import argparse
import json
import statistics
import time
import uuid
from datetime import datetime, timezone

import test_bot_population as population
import test_bot_provision as p


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds", type=int, default=1800)
    parser.add_argument("--interval", type=int, default=10)
    args = parser.parse_args()
    if args.seconds < 60 or args.interval < 1:
        parser.error("use at least 60 seconds and a positive sample interval")

    project = "tortoise-bot-cohort-" + uuid.uuid4().hex[:12]
    evidence = p.ROOT / "local" / (
        project + "-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ"))
    evidence.mkdir(parents=True)
    world = p.world_env_for("")
    world.update(PLAYERBOT_ENABLE="1", PLAYERBOT_MIN_BOTS="10",
                 PLAYERBOT_MAX_BOTS="10", PLAYERBOT_REFRESH="30000",
                 PLAYERBOT_UPDATE_MS="1000", PLAYERBOT_DEBUG="1",
                 PLAYERBOT_PROVISION="", PLAYERBOT_QUEST_ID="0",
                 PLAYER_SAVE_INTERVAL="15000", PERF_PROCESSING_TELEMETRY="5")
    base = env = None
    report = {"project": project, "seconds_requested": args.seconds,
              "started_utc": datetime.now(timezone.utc).isoformat(), "completed": False}
    try:
        base, env = p.boot_lab(project, evidence, world, population.seed_roster(10))
        p.command(["docker", "compose"] + base + ["up", "-d", "--no-deps", "world"], env=env)
        expected = ["[PlayerBot][Login]  'Popbot%s' GUID:%d" % (chr(97 + i), 501000 + i)
                    for i in range(10)]
        p.wait_for(base, env, lambda text: all(marker in text for marker in expected), deadline=300)
        ids = p.command(["docker", "compose"] + base + ["ps", "-q", "db", "world"],
                        env=env).splitlines()
        samples = []
        start = time.monotonic()
        next_progress = 60
        while time.monotonic() - start < args.seconds:
            raw = p.command(["docker", "stats", "--no-stream", "--format", "{{json .}}", *ids],
                            timeout=60)
            for line in raw.splitlines():
                row = json.loads(line)
                samples.append({"elapsed_s": round(time.monotonic() - start, 2),
                                "name": row["Name"],
                                "cpu_pct": float(row["CPUPerc"].rstrip("%")),
                                "memory": row["MemUsage"], "pids": int(row["PIDs"])})
            elapsed = time.monotonic() - start
            if elapsed >= next_progress:
                print("Observed %d/%d seconds" % (round(elapsed), args.seconds), flush=True)
                next_progress += 60
            time.sleep(min(args.interval, max(0, args.seconds - elapsed)))

        logs = p.command(["docker", "compose"] + base + ["logs", "--no-color", "world"],
                         env=env, timeout=60)
        telemetry = p.command(["docker", "compose"] + base + ["exec", "-T", "world", "sh", "-c",
                               "cat /state/world_processing_telemetry.log 2>/dev/null || true"],
                              env=env, timeout=60)
        ownership = p.db_int(base, env,
                             "SELECT COUNT(*) FROM bot_ownership WHERE char_guid BETWEEN 501000 AND 501009")
        online = p.db_int(base, env,
                          "SELECT COUNT(*) FROM characters WHERE guid BETWEEN 501000 AND 501009 AND online=1")
        summary = {}
        for name in sorted({row["name"] for row in samples}):
            cpu = [row["cpu_pct"] for row in samples if row["name"] == name]
            summary[name] = {"samples": len(cpu), "cpu_mean_pct": round(statistics.mean(cpu), 2),
                             "cpu_p95_pct": sorted(cpu)[max(0, int(len(cpu) * .95) - 1)],
                             "cpu_max_pct": max(cpu)}
        report.update(completed=True, elapsed_s=round(time.monotonic() - start, 2),
                      login_count=sum(logs.count(marker) for marker in expected),
                      ownership_count=ownership, online_db_count=online,
                      actions={"wander": logs.count("[PlayerBot] wander GUID:"),
                               "fight": logs.count("[PlayerBot] fighting GUID:"),
                               "loot": logs.count("[PlayerBot] corpse loot processed")},
                      resources=summary,
                      telemetry_lines=sum(1 for line in telemetry.splitlines()
                                          if "World processing telemetry:" in line),
                      crashed="[CRASH]" in logs or "Received SIGSEGV" in logs)
        (evidence / "world.log").write_text(logs, encoding="utf-8")
        (evidence / "telemetry.log").write_text(telemetry, encoding="utf-8")
        (evidence / "samples.json").write_text(json.dumps(samples, indent=2) + "\n", encoding="utf-8")
        if report["login_count"] != 10 or ownership != 10 or report["crashed"]:
            raise AssertionError("cohort acceptance failed: %r" % report)
    finally:
        (evidence / "result.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        if base is not None:
            p.teardown_lab(base, env, project, evidence)
        print("Evidence: %s" % evidence, flush=True)
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
