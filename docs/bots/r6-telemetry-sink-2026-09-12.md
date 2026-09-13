# R6: bounded nonblocking telemetry sink - 2026-09-12

Status: complete and validated on branch `feature/kap-543-bot-living-world`.
Closes the R6 finding from the 2026-09-11 review (KAP-546 / TW-003): a blocked
telemetry consumer must not stall the world thread.

## Problem

`PerformanceMonitor::ReportProcessingTime` emitted its periodic report
synchronously through `sLog.outInfo`. The shared Log takes its global lock and
flushes its stdout/file stream, so a telemetry consumer that blocks (slow or
full file, blocked pipe, wedged logging) couples its stall to the world
thread on every report tick. Moving the call to another thread is not enough
by itself: shared stdout/file stream locks would still couple the two.

## Design

`BoundedTelemetrySink` (src/game/PerformanceMonitor.{h,cpp}):

- World-thread contract: `Enqueue()` only. A fixed-capacity queue
  (64 lines x 512 bytes = 32 KiB bound) either accepts the line or drops it
  and counts it. The world thread never touches the sink's stream and never
  waits on the consumer.
- Isolated consumer: a dedicated writer thread owns its own `FILE*`
  (`fopen`, its own buffer, its own flush) - never the shared sLog/stdout
  stream. The file is opened inside the writer thread, so a consumer that
  blocks on open (a FIFO with no reader) blocks only the writer.
- Drops are exposed two ways: when the consumer recovers, the writer emits a
  bounded in-band notice line before the first drained report line
  (`World telemetry: N report line(s) dropped while the sink queue was
  full`), and `Stop()` logs cumulative counters
  (`World telemetry sink stopped: dropped_full=N unsent=N`).
- Bounded shutdown: `Stop()` waits at most 30 s for the writer to exit, then
  detaches it (the writer keeps its own shared_ptr to the state object, so
  detach is memory-safe) so a permanently blocked consumer cannot hang world
  shutdown. Lines still queued at stop are counted `unsent`, not drained
  forever. `Stop()` reads the writer-exit flag under the state lock (no
  data race) and records whether it joined (`LastWriterJoined()`).
- Fail closed: an unopenable sink disables emission with a startup error,
  counts queued lines `unsent`, and shutdown stays bounded. I/O errors stop
  writing but keep draining. `Stop()` captures the final counters before
  releasing the state (`LastDroppedFull()` / `LastUnsent()`), so callers can
  inspect them afterwards.
- Report lines carry a monotonic `seq=` per sink instance, so delivery order
  is verifiable from content (gaps show where drops happened).
  `ReportProcessingTime` formats into a bounded 512-byte buffer (with a
  bounded truncation fallback) and enqueues; it no longer calls `sLog`.
- `BoundedTelemetrySink::SelfTest()` runs at telemetry startup next to the
  histogram self-test and gates telemetry on itself:
  - Block 1: a burst producer (256 lines x up to 128 bursts, then a 128-line
    tail) outpaces any realistic writer, so the queue fills and drops start.
    Bursts keep the drain bounded in both extremes: a slow writer drops
    early (small backlog), a fast writer keeps the queue drained (and then
    drains fast). The test waits up to 60 s for the queue to drain, checks
    the file contents when the writer joined (in-order lines, notice
    accounting, and the emission/drop/unsent conservation law) or falls back
    to a counters-only check, and removes its temp file on every exit path.
  - Block 2: bounded shutdown with pending lines; Stop stays within its
    bound and pending lines are counted `unsent` (verified via the
    `Last*` counters captured by Stop).
  - Block 3: the fail-closed path on an unopenable sink. A fast open failure
    verifies emission-disabled + unsent accounting + bounded stop. If the
    open is still pending after 15 s (a consumer stalling the open - exactly
    the R6 scenario, observed on some volume filesystems), the test accepts
    a bounded shutdown of the stalled writer (Stop within 30 s) instead of
    failing telemetry.

Configuration: `Perf.ProcessingTelemetryFile` (default
`world_processing_telemetry.log` in the world process working directory, i.e.
`/state/...` in Docker). `docker/server.py` passes
`PERF_PROCESSING_TELEMETRY_FILE` and `PLAYER_SAVE_INTERVAL` (lab only;
defaults preserve current behavior). `docker/bot_baseline.py` now reads
telemetry evidence from the sink file instead of world logs.

## Validation

- [x] Docker build with `BUILD_JOBS=2` and `-DALLOW_TURTLE_ADDONS=ON` into the
  dedicated `tortoise-local:r6-review` image (personal `tortoise-local:dev`
  and the running server untouched). Log: `local/build-r6.log`.
- [x] Fast gates: `docker/test_telemetry_conf.py` 6/6, `python -m py_compile`
  on the changed scripts, `docker compose config --quiet`, `git
  diff --check`.
- [x] R6 lab (`docker/test_bot_telemetry_sink.py`), 3/3 in 133.2 s, fresh
  port-free lab world with 1 s telemetry interval and a FIFO sink that no
  process reads. Evidence:
  `local/tortoise-bot-tel-c936f4496d40-20260912T133053Z`:
  - Startup self-test passed (telemetry enabled): block 1 accounted all 384
    lines (64 emitted + 320 dropped + 0 unsent, file-content checks passed);
    block 2 stopped bounded with 3 pending lines counted unsent; block 3
    verified the fail-closed path on its temp path (emission disabled, 2
    lines counted unsent, bounded stop).
  - World updates continued while the consumer was blocked: the provisioned
    bot's character row moved in the database (from
    -8949.95, -132.493, 83.5312) during the 80 s blocked window
    (`positions.txt`, `PlayerSave.Interval=5000`).
  - On consumer recovery the reader got the in-band drop notice first
    (`World telemetry: 18 report line(s) dropped while the sink queue was
    full`), then the full 64-line bounded queue in `seq` order (seq 1..64),
    then live lines (seq 83..84) - `sink-read.txt.flush`, 67 lines total.
  - Graceful shutdown stayed bounded and reported the production sink
    counters: `World telemetry sink stopped: dropped_full=18 unsent=0`
    followed by `Shutting down world...` (`world-shutdown.log`).
- [x] No disposable lab containers or volumes remain; personal volumes, the
  running personal server, and `tortoise-local:dev` untouched.

Iteration evidence (all in `local/`): first lab run
`tortoise-bot-tel-c913c8bf373f-20260912T124618Z` exposed the v1 self-test
(5 s drain deadline too tight for volume storage);
`tortoise-bot-probe-54d2c8246168-20260912T125603Z` and
`tortoise-bot-tel-0f3c0b903498-20260912T132147Z` localized a second defect
(counters read after `Stop()` had released the state, which also masked the
real block-2 check in v1); `tortoise-bot-tel-71b6cc6c11ba-20260912T132657Z`
passed 2/3 and showed the self-test fail-closed line that an over-broad log
assertion had to be scoped to the FIFO path. The final green run is cited
above.

Retrieval synchronization remains paused by the operator; bounded direct
current-source reads are the declared fallback, and no embedding or
derived-write counts are claimed.

This closes R6. It is not an earned-gameplay, capacity, or companion claim.
Next: R5 save-boundary failure coverage, R4 stale-completion coverage, then
TW-011 earned-state restart/restore.