# PORT-008: Bound unreachable-target and owner-loss recovery

- Depends on: PORT-007
- Status: pending hosted dispatch; passes=false
- Jira: not created; local story under KAP-543
- Shared contract: [execution and review](README.md)

## Objective

Add timer-bounded pursuit and regroup behavior using existing Turtle movement. No teleport-to-owner shortcut or general transport implementation.

## Allowed edit candidates

- `src/game/PlayerBots/PlayerBotAI.cpp`
- `src/game/PlayerBots/PlayerBotAI.h`
- `docker/test_bot_companion_leash.py`

The hosted head narrows these to exact functions and approved new paths in a fresh hashed evidence card. These paths are a ceiling, not permission to read entire directories. Shared fixture changes require an explicitly revised card.

## Acceptance

Unreachable or over-leash targets are abandoned within an explicit tested deadline, and the companion returns to an available owner without selecting unrelated enemies.

## Failure cases

Owner logout, death or map change cancels offensive work and holds safely; owner return can resume the current valid follow goal. No per-tick expensive full path recomputation.

## Validation

Compile; disposable unreachable target, owner logout/rejoin and map mismatch checks; report actual elapsed bounds.

Apply the shared gates for changed file types. Report unexecuted checks explicitly. Return the diff, precise upstream provenance where used, tests and uncertainties; do not mark this story accepted.

