# PORT-002: Keep dismissed companions benched

- Depends on: CMP-010, PORT-001
- Status: pending hosted dispatch; passes=false
- Jira: not created; local story under KAP-543
- Shared contract: [execution and review](README.md)

## Objective

Separate persistent companion availability from the ambient population target. Head must approve the exact additive migration path and schema before dispatch; do not edit applied migrations.

## Allowed edit candidates

- `src/game/PlayerBots/PlayerBotMgr.cpp`
- `src/game/PlayerBots/PlayerBotMgr.h`
- `sql/wip_updates/bot_ownership.sql`
- `docker/test_bot_bench.py`

The hosted head narrows these to exact functions and approved new paths in a fresh hashed evidence card. These paths are a ceiling, not permission to read entire directories. Shared fixture changes require an explicitly revised card.

## Acceptance

Dismiss an owned companion with a positive population target, run at least two refresh intervals, and prove it stays offline; recall reactivates it, preserving ownership and earned state. Repeat across a world restart.

## Failure cases

Ambient bots remain eligible; a rejected foreign dismiss cannot change availability. Existing ownership rows receive an explicit compatible default.

## Validation

Compile; run the disposable bench/recall/restart fixture and relevant population/party regressions. No personal-volume operations.

Apply the shared gates for changed file types. Report unexecuted checks explicitly. Return the diff, precise upstream provenance where used, tests and uncertainties; do not mark this story accepted.

