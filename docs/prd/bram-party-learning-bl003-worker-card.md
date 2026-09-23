# BL-003 worker evidence card v1

Date: 2026-09-22
Repository: `C:\Users\hpark\WebstormProjects\tortoise-wow`
Branch: `feature/kap-558-port-phase2`
Baseline commit: `c5781974b444f6ad692b48a516b0994d8cf59bc5` with accepted, uncommitted BL-001A/B/R and BL-002 work

## Objective

Implement the ordered character-database learning schema plus a bounded asynchronous
encounter-summary persistence adapter. This card persists observations and proves
version lifecycle mechanics; it must not call an LLM, enable a candidate rotation,
add owner commands, or alter combat choices.

## Required design

1. Add one ordered, idempotent character migration
   `sql/database_updates/character/20260922120000_character.sql` defining:
   - `bot_learning_profile`: character key, mode (disabled/observe/shadow/trial/paused),
     schema version, active playbook version, expected-version/CAS support, timestamps;
   - `bot_learning_playbook`: immutable per-character versions, parent version,
     bounded capability/context fingerprint and settings payload, state, provenance,
     timestamps; unique character/version;
   - `bot_learning_encounter`: idempotent delivery identity, character, session and
     sequence, policy/playbook version, source/route, duration and recorder metrics,
     end reason, complete/overflow/efficacy flags, captured/completed timestamps;
   - `bot_learning_candidate`: bounded proposal, cited evidence payload, state,
     evaluator version and timestamps;
   - `bot_learning_audit`: append-only enroll/pause/override/promotion/rollback event
     with actor/reason/version references and timestamp.
   Use foreign keys only where compatible with this repository's character schema and
   updater conventions; otherwise use indexed character keys and explicit cleanup.
   No migration may mutate gameplay state.
2. Add `Companion/LearningStore.h/.cpp` with two layers:
   - a pure fixed-capacity FIFO contract (maximum 64 summaries, no heap allocation in
     enqueue/dequeue, deterministic full/drop/failure counters and disable-on-writer-
     failure behavior) covered by a standalone value test;
   - the game adapter that submits at most one write at a time through the existing
     `CharacterDatabase.PExecuteCallback`/SQL delay-thread convention. The world thread
     only performs bounded queue/pump work and never waits for database I/O. The DB
     callback communicates completion through atomics/mutex-safe bounded state. Do not
     launch a new database thread and do not change shared Database internals.
3. Encounter delivery identity must be generated game-side from a process/session
   nonce plus per-store monotonic sequence and character key. Re-delivery of the same
   identity must be idempotent (`INSERT ... ON DUPLICATE KEY` without double counting).
   Persist only scalar summary values; never persist raw chat, prompts, Unit pointers,
   account IDs, credentials, or event-buffer dumps.
4. Hook completed BL-002 summaries for owned companions only. A helper must enqueue
   exactly once on target/Bram death, owner override, target replacement, invalid
   target, or leash expiry, then reset/advance the recorder safely. A new target must
   persist the old completion before beginning the new encounter. Persistence is
   profile-gated in SQL: if no `bot_learning_profile` row exists or its mode is
   disabled/paused, no encounter row is inserted. Combat behavior remains unchanged.
5. Pump the adapter from the existing `PlayerBotMgr::Update` cadence and shut it down
   safely from manager teardown. Startup must asynchronously mark stale unfinished
   encounter rows interrupted/excluded; no synchronous DB query on the world thread.
6. Retention/lifecycle SQL must be explicit and testable: encounter retention is at
   most 500 rows per character and 30 days, with pending candidate evidence protected;
   playbook retention is at most 20 non-active/non-cited historical versions. If safe
   pruning cannot be expressed without deleting candidate basis, leave rows and expose
   a paused/exhausted state instead of deleting evidence.
7. Add:
   - standalone C++/Python value tests for FIFO order, 64-item cap, drop accounting,
     one-in-flight completion, failure disabling, restart/reset and duplicate identity;
   - a disposable MariaDB migration/lifecycle test that loads the real base schema,
     applies the migration twice, tests duplicate encounter delivery, CAS activation,
     rollback/audit, stale unfinished interruption, retention/protected evidence, and
     restart continuity. It must use a fresh verified Compose project/volume and never
     touch `tortoise-local_database`.

No model protocol, prompts, evaluator, candidate execution, owner commands, or live
deployment is part of BL-003. No learned behavior is enabled.

## Allowed reads

- this card and `docs/prd/bram-party-learning.prd`
- `src/game/PlayerBots/Companion/Encounter.h`
- `src/game/PlayerBots/PlayerBotAI.h/.cpp`
- `src/game/PlayerBots/PlayerBotMgr.h/.cpp`
- `src/shared/Database/Database.h/.cpp`, `DatabaseImpl.h`,
  `SqlOperations.h/.cpp`, `SqlDelayThread.cpp` (convention evidence only)
- `sql/database_updates/character/20260911174500_character.sql`
- `sql/database_updates/character/20260916120000_character.sql`
- `sql/database_updates/character/20260920120000_character.sql`
- `docker/test_bot_ownership_migration.py`
- BL-002 value test/runner

## Allowed edits

- `src/game/PlayerBots/Companion/LearningStore.h` (new)
- `src/game/PlayerBots/Companion/LearningStore.cpp` (new)
- `src/game/PlayerBots/PlayerBotAI.h`
- `src/game/PlayerBots/PlayerBotAI.cpp`
- `src/game/PlayerBots/PlayerBotMgr.h`
- `src/game/PlayerBots/PlayerBotMgr.cpp`
- `sql/database_updates/character/20260922120000_character.sql` (new)
- `docker/test_companion_learning_store_value.cpp` (new)
- `docker/test_companion_learning_store_value.py` (new)
- `docker/test_bot_learning_migration.py` (new)

Do not edit shared Database implementation, CMake, configuration, existing migrations,
the PRD/handoff, or any other file. Do not commit, stage, push, deploy, start the
personal realm, access secrets/private character data, call retrieval, or launch
another model session.

## Verified provenance

- Retrieval sync after BL-002: added 4, modified 6, chunks embedded 207, derived
  writes 223; database/embedding/schema healthy and fresh for this worktree.
- `Encounter.h`: `04A356CA392600C331ECDA8B93F8DC1D1C772F854FACFEB9CF7ABD48A1AEAF07`
- `PlayerBotAI.h`: `A3B87C56B84A3A0C37F244FAB0D4A0AEE9D1B886877BFD156CF1F69AAE891665`
- `PlayerBotAI.cpp`: `8ED30284F11505C91C61654DE290BC726B1E173AD55766B5CD44B680F24DB738`
- `PlayerBotMgr.h`: `29B676AA38EF87C3327B78FA1A3FFDDFA06A69AFF1305B39FFE9BB3C9DAB7E2D`
- `PlayerBotMgr.cpp`: `886ACF1F183BAD92D32862CC3E0C24D4D7CD69DFCAE7BC1737F89B5C72207243`
- `Database.h`: `FAD0F957919882F5A8E8AFE9885581DC6B6D7603366BF7147FC5F07612CCD63B`
- `Database.cpp`: `EADB868E3DC3439A6FD9E67A70D1993442C9503E570A3C14B0A1A64287537C0A`
- `SqlDelayThread.cpp`: `40F6E282287E1480503A60FD81ABF38EDBA18E2BE0D0921D97B9E7E8BA30D1E4`
- Current source proves `Database::Execute` queues `SqlPlainRequest` when async
  transactions are enabled and `SqlDelayThread` invokes write callbacks on the DB
  worker after execution. `PExecuteCallback` is therefore the supported adapter.

## Validation

Run only the two new focused Python test runners and `git diff --check`. The hosted
head performs the Docker world build, disposable MariaDB integration acceptance,
cumulative runtime tests, diff review, retrieval sync, and final acceptance.

Report exact changed paths, commands/results, failures and uncertainty. State
explicitly that no learned behavior was enabled.
