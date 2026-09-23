# BL-008B evidence card v2 — asynchronous status-query wiring

## Current bounded assignment (v2)

Implement only the missing asynchronous status-query path from
`PlayerBotMgr` into the existing `LearningStore` external-control reservation.
Do not add commands in this assignment.

### Verified provenance

- Repository/branch: `tortoise-wow` / `feature/kap-558-port-phase2`
- Indexed/base commit: `c5781974b444f6ad692b48a516b0994d8cf59bc5`
- Card hash before this v2 update: `0570bada35e7a65d29fc9fd41444dc5dffcdaada`
- `LearningStore.h`: `059fd3404277d88b2bac9de17161ffb197b7f769`
- `PlayerBotMgr.h`: `8bede41cb6c28a2ae2c095bc22fc7f0e1b541db9`
- `PlayerBotMgr.cpp`: `05887f74d44c56036912281e37871bd016cfbd40`
- value harness: `9440b30662381ff37dbb992ea0114fb76aa7adb5`

The worktree contains cumulative uncommitted BL-001A–BL-008B changes. Preserve
them exactly outside the allowed paths.

### Allowed files and edit permission

- `src/game/PlayerBots/Companion/LearningStore.h` (edit only if a narrowly
  necessary result-publication seam is missing)
- `src/game/PlayerBots/PlayerBotMgr.cpp`
- `src/game/PlayerBots/PlayerBotMgr.h`
- `docker/test_companion_learning_store_value.cpp`
- `docker/test_companion_learning_store_value.py`
- this card (report section only)

Edits are permitted only in this list. Do not commit, stage, push, deploy, or
start a realm/database.

### Required result

- Add a manager API that reserves the Store slot and calls the core asynchronous
  query API with `kControlStatusSqlTemplate`.
- Convert the returned row to a bounded `ControlStatus` containing only scalar
  values. No database-owned pointer may escape the callback/result boundary.
- Validate mode and numeric ranges, set `candidateState`/`evidenceCount` and
  `insufficientEvidence` honestly from available persisted evidence, or leave
  them explicitly bounded/insufficient if the current query has no such rows.
- Publish completion for world-thread consumption without blocking and without
  racing encounter, maintenance, or rollback work.
- Missing row, malformed row, query submission failure, and absent/invalid
  identity fail closed, release the reservation, and never overwrite the last
  confirmed status.
- Preserve the already accepted rollback transaction behavior.

### Validation and expected report

Run `python docker/test_companion_learning_store_value.py -v` and
`git diff --check`. If practical, add focused value/static assertions for the
manager query path. Report changed paths, commands/results, hashes, uncertainties,
and whether a world build is required. Do not claim final acceptance.

### Safety attestations

Do not read `.env`, credentials, personal character/account data, backups, raw
logs, or environment maps. Do not use retrieval, Jira, nested `park-agent`,
`park-head`, or another model. Do not alter architecture, SQL migrations,
commands, combat, planner code, or acceptance flags.

---

## Prior v1 assignment — transactional rollback and status result adapter

## Objective

Add bounded injected adapters so `LearningStore` can execute rollback as one async database transaction and receive a bounded profile/candidate status result. No commands in this card.

## Allowed files

- `src/game/PlayerBots/Companion/LearningStore.h`
- `src/game/PlayerBots/PlayerBotMgr.cpp`
- `src/game/PlayerBots/PlayerBotMgr.h`
- `docker/test_companion_learning_store_value.cpp`
- `docker/test_companion_learning_store_value.py`
- this card

## Verified APIs

- `Database::BeginTransaction(uint32 serialId)`
- statements issued while the transaction is active are queued into it
- `Database::CommitTransaction(std::function<void(bool)>*)` publishes transaction success
- `Database::AsyncQuery` publishes a `QueryResult*` on the world result queue
- existing Store submit callback is bool-only and cannot provide status rows or affected-row CAS evidence

## Requirements

- Inject a transaction submitter and bounded status-query submitter from `PlayerBotMgr`; `LearningStore` must not include database engine types.
- Rollback transaction must write audit provenance and reset active/expected versions to baseline together; failure leaves cached status unchanged.
- Status result contains only typed scalar fields already in `ControlStatus`; reject invalid modes/candidate states and explicitly report insufficient evidence.
- Share or coordinate with the Store one-in-flight state; no concurrent control mutation with encounter/maintenance writes.
- Preserve baseline fail-closed behavior when adapters are absent or submission/completion fails.

## Validation

Extend the value harness for transaction success/failure, no partial rollback, restart-safe status, malformed status, contention, and absent adapters. Run focused tests and `git diff --check`. Do not start a realm or database.

## Safety

No commands, combat/model wiring, SQL migration edits, commits, secrets, raw logs, or nested model sessions. Preserve unrelated work.

## Head result (2026-09-22)

- The required fresh `park-agent` request was launched once and serialized, but
  remained at model loading for several minutes with no `llama-server` or
  downloader process. The head stopped the hung session and used the documented
  unavailable-worker fallback.
- Implemented `PlayerBotMgr::QueueLearningStatus` with a world-result-queue
  callback that reduces the three-column result to `ControlStatus` scalars.
  Missing/malformed rows and submission failures release the Store reservation,
  increment the bounded failure count, and preserve the last confirmed status.
- Status explicitly reports zero candidate evidence and insufficient evidence;
  this slice does not invent an evaluator result.
- The first world build reached final link and exposed a missing
  `DatabaseImpl.h` template definition. After the bounded include repair, the
  full world build passed.
- Head validation: focused Python wiring/value runner passed (the standalone C++
  binary was skipped because no native/WSL compiler was available); full
  `docker compose build world` passed with `ALLOW_TURTLE_ADDONS=ON`;
  `git diff --check` passed.
