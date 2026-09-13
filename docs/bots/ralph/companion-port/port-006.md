# PORT-006: Defend the owner without additional pulls

- Depends on: PORT-005
- Status: pending hosted dispatch; passes=false
- Jira: not created; local story under KAP-543
- Shared contract: [execution and review](README.md)

## Objective

Adapt bounded defensive target selection for one companion; no tank threat engine or PvP behavior.

## Allowed edit candidates

- `src/game/PlayerBots/PlayerBotAI.cpp`
- `src/game/PlayerBots/PlayerBotAI.h`
- `src/game/PlayerBots/PlayerBotMgr.cpp`
- `src/game/PlayerBots/PlayerBotMgr.h`
- `src/game/Chat/Chat.cpp`
- `src/game/Chat/Chat.h`
- `src/game/Commands/Commands.cpp`
- `docker/test_bot_companion_defend.py`

The hosted head narrows these to exact functions and approved new paths in a fresh hashed evidence card. These paths are a ceiling, not permission to read entire directories. Shared fixture changes require an explicitly revised card.

## Acceptance

When enabled by the owner, select a legal creature actually attacking the owner or companion, handle it, and return to follow. Explicit hold has the precedence approved in PORT-004.

## Failure cases

A nearby neutral/unengaged creature is never pulled; invalid owner/party state clears the goal. Multiple attackers use a deterministic documented tie-break.

## Validation

Compile; disposable attacked-owner, unrelated-bystander, owner-loss and hold-precedence fixtures.

Apply the shared gates for changed file types. Report unexecuted checks explicitly. Return the diff, precise upstream provenance where used, tests and uncertainties; do not mark this story accepted.

