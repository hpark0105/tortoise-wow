# PORT-007: Finish combat with bounded loot and regroup

- Depends on: PORT-006
- Status: pending hosted dispatch; passes=false
- Jira: not created; local story under KAP-543
- Shared contract: [execution and review](README.md)

## Objective

Reuse the current normal corpse-loot path within companion priorities; no gear generation, auto-equipping redesign or arbitrary quest selection.

## Allowed edit candidates

- `src/game/PlayerBots/PlayerBotAI.cpp`
- `src/game/PlayerBots/PlayerBotAI.h`
- `docker/test_bot_companion_regroup.py`

The hosted head narrows these to exact functions and approved new paths in a fresh hashed evidence card. These paths are a ceiling, not permission to read entire directories. Shared fixture changes require an explicitly revised card.

## Acceptance

After a supported kill, loot only an eligible corpse and return to owner within a configured leash/time budget. Compare actual saved inventory changes.

## Failure cases

Full bags, inaccessible corpse, missing loot rights or owner moving away cannot trap the bot in a retry loop. Respect group loot rules; report unsupported roll handling rather than bypass it.

## Validation

Compile; disposable eligible loot, full-bag/no-rights and regroup cases with inventory snapshots retained locally.

Apply the shared gates for changed file types. Report unexecuted checks explicitly. Return the diff, precise upstream provenance where used, tests and uncertainties; do not mark this story accepted.

