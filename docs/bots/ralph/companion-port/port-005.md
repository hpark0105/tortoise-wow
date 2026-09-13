# PORT-005: Assist one owner-selected hostile target

- Depends on: PORT-004
- Status: pending hosted dispatch; passes=false
- Jira: not created; local story under KAP-543
- Shared contract: [execution and review](README.md)

## Objective

Port only the approved upstream assist decision rules. One owned melee damage companion and one explicit owner target; use learned abilities and normal Turtle combat APIs.

## Allowed edit candidates

- `src/game/PlayerBots/PlayerBotAI.cpp`
- `src/game/PlayerBots/PlayerBotAI.h`
- `src/game/PlayerBots/PlayerBotMgr.cpp`
- `src/game/PlayerBots/PlayerBotMgr.h`
- `src/game/Chat/Chat.cpp`
- `src/game/Chat/Chat.h`
- `src/game/Commands/Commands.cpp`
- `docker/test_bot_companion_assist.py`

The hosted head narrows these to exact functions and approved new paths in a fresh hashed evidence card. These paths are a ceiling, not permission to read entire directories. Shared fixture changes require an explicitly revised card.

## Acceptance

A same-party companion attacks the owner's selected legal creature through normal chase/combat, earns only normal credit, and resumes following after the kill.

## Failure cases

Reject friendly/player/missing/dead targets and foreign or non-party issuers. Revalidate target and action generation at execution; hold cancels pursuit. Never pick a replacement unrelated enemy.

## Validation

Compile; disposable assist/kill/follow, invalid-target and cancellation fixtures; preserve the autonomous player-target protection.

Apply the shared gates for changed file types. Report unexecuted checks explicitly. Return the diff, precise upstream provenance where used, tests and uncertainties; do not mark this story accepted.

