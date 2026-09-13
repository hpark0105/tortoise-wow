# PORT-003: Separate companion follow from combat priorities

- Depends on: PORT-002
- Status: pending hosted dispatch; passes=false
- Jira: not created; local story under KAP-543
- Shared contract: [execution and review](README.md)

## Objective

Implement only the head-approved minimal deterministic priority boundary. Keep existing autonomous behavior unchanged for bots outside companion control; no imported engine framework.

## Allowed edit candidates

- `src/game/PlayerBots/PlayerBotAI.cpp`
- `src/game/PlayerBots/PlayerBotAI.h`
- `docker/test_bot_companion_priority.py`

The hosted head narrows these to exact functions and approved new paths in a fresh hashed evidence card. These paths are a ceiling, not permission to read entire directories. Shared fixture changes require an explicitly revised card.

## Acceptance

An active follow goal no longer suppresses an existing legal combat action. After combat, the companion returns to its owner. Following without combat still works.

## Modular implementation acceptance

Scenario: Prove behavior extension without lifecycle coupling
Given separate follow and existing-combat policies using a shared observation and typed intent contract
When PORT-003 implements and exercises both policies
Then adding the second policy requires no changes to bot identity, login, save or bench logic
And shared action selection interrupts follow for legal combat and resumes the still-valid follow goal afterward
And the executor revalidates ownership, party, action generation, target and relevant game constraints immediately before action
And model unavailability does not affect deterministic behavior.

Scenario: Preserve goals safely across interruptions
Given a suspended follow goal and a newer owner order or invalid owner state
When combat ends
Then the suspended goal resumes only if its generation and prerequisites remain valid
And a canceled goal cannot be resumed by a policy or planner.

Follow the README modularity contract and PORT-001's approved architecture.
Candidate new components may live under `src/game/PlayerBots/Companion/`;
the head must narrow exact files and required build registration before dispatch.
Do not keep all policies as accumulating branches in PlayerBotAI or PlayerBotMgr.
No lifecycle changes are allowed by this extension proof. Split extraction into
another bounded card if the approved implementation cannot fit this assignment.

## Failure cases

No new target selection or unrelated pull is introduced. Missing/dead/different-map owner produces a bounded hold; persistent bots gain no free gear or progress.

## Validation

Compile; disposable existing-combat to follow transition plus existing follow/stop regression.

Apply the shared gates for changed file types. Report unexecuted checks explicitly. Return the diff, precise upstream provenance where used, tests and uncertainties; do not mark this story accepted.
