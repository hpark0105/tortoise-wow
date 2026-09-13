# PORT-004: Add explicit hold and cancel semantics

- Depends on: PORT-003
- Status: pending hosted dispatch; passes=false
- Jira: not created; local story under KAP-543
- Shared contract: [execution and review](README.md)

## Objective

Add a distinct owner-only hold command, preserving the documented legacy botstop behavior. Head fixes command spelling and cancellation contract in the evidence card.

## Allowed edit candidates

- `src/game/PlayerBots/PlayerBotAI.cpp`
- `src/game/PlayerBots/PlayerBotAI.h`
- `src/game/PlayerBots/PlayerBotMgr.cpp`
- `src/game/PlayerBots/PlayerBotMgr.h`
- `src/game/Chat/Chat.cpp`
- `src/game/Chat/Chat.h`
- `src/game/Commands/Commands.cpp`
- `docker/test_bot_companion_hold.py`

The hosted head narrows these to exact functions and approved new paths in a fresh hashed evidence card. These paths are a ceiling, not permission to read entire directories. Shared fixture changes require an explicitly revised card.

## Acceptance

Hold cancels movement and offensive goals, increments the action generation and persists until a new authorized order in this session. A delayed old action cannot restart offense.

## Failure cases

Foreign commands and stale goals are rejected. Define and test defensive behavior while held; do not silently resume autonomous nearest-target aggression.

## Validation

Compile; disposable hold during chase, hold during combat, stale delivery and foreign-owner cases.

Apply the shared gates for changed file types. Report unexecuted checks explicitly. Return the diff, precise upstream provenance where used, tests and uncertainties; do not mark this story accepted.

