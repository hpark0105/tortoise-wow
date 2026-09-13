# PORT-009: Support one normal companion death and recovery path

- Depends on: PORT-008
- Status: pending hosted dispatch; passes=false
- Jira: not created; local story under KAP-543
- Shared contract: [execution and review](README.md)

## Objective

Implement one head-selected normal recovery path after source inspection: corpse recovery in one supported outdoor area, or accepting a legal player resurrection. The selected path must be explicit; do not attempt both in one worker assignment.

## Allowed edit candidates

- `src/game/PlayerBots/PlayerBotAI.cpp`
- `src/game/PlayerBots/PlayerBotAI.h`
- `docker/test_bot_companion_recovery.py`

The hosted head narrows these to exact functions and approved new paths in a fresh hashed evidence card. These paths are a ceiling, not permission to read entire directories. Shared fixture changes require an explicitly revised card.

## Acceptance

A dead companion stops offensive work, completes the declared legal recovery path, resumes follow and preserves earned inventory/XP under normal death rules.

## Failure cases

Unavailable/unreachable recovery holds and reports its state with bounded retries; no free resurrection, invented destination or bypass of normal penalties.

## Validation

Compile; disposable death/recovery and unavailable-path fixtures. Any second recovery mechanism is a new story.

Apply the shared gates for changed file types. Report unexecuted checks explicitly. Return the diff, precise upstream provenance where used, tests and uncertainties; do not mark this story accepted.

