# Phase 1 handoff: one useful deterministic companion

Status: **PORT-002/003/004 implemented and fixture-verified; not accepted**. Last checkpoint: 2026-09-14.
Tracking: KAP-558 under the KAP-543 plan; no Jira transition made by this checkpoint.

## Goal and boundaries

Complete PORT-001 through PORT-010 in the order in [README](README.md).
Support one declared class/level companion through owner follow, assist/defend,
bounded loot/regroup, recovery, and persistent bench/recall. Tank/healer expansion,
general questing, population scale and model integration are not phase-1 acceptance.

## Current checkpoint

Branch: `feature/kap-558-port-companion-port`.
Reviewed baseline: `7b577bbd156f1c190d46a7b4a99f1071c9886d65`.

- `293743c`: excludes owned companions from ambient selection.
- `7b577bb`: follow/combat priority change. A commit title or earlier CI green
  does not establish the complete PORT-002/003 acceptance criteria.
- Repairs (committed after this checkpoint) add the policy boundary, eligible
  ambient counting, normal death-login preservation, hold cancellation
  generations, and the repaired fixtures.
- Canonical command is `.bothold <botname>`; `.bothyld` remains an alias.
  Hold suppresses autonomous offense until a new authorized order in this session.
- Disposable tests cover priority, bench/restart and hold order handling.

## Code entry points and invariants

- `src/game/PlayerBots/Companion/Policy.h`: value observations, typed intents,
  selection and generation checks. Policies must not own game pointers or perform I/O.
- `PlayerBotAI.cpp`: `UpdateCompanion`, `ExecuteCompanion`,
  `IsFollowOwnerAvailable`, `FollowGoal`, `Hold`. Execution re-resolves
  targets and validates current owner, group and world state; stale orders cannot resume.
- `PlayerBotMgr.cpp`: population reconciliation, owner authorization and order
  generation. Owned companions are not ambient population capacity.
- Chat/Commands registration: preserve legacy stop semantics separately from hold.
- `docker/test_bot_companion_priority.py`, `test_bot_companion_hold.py`,
  `test_bot_bench.py`: isolated synthetic labs, not evidence of human gameplay.

## Validation checkpoint

Image: `tortoise-local:dev` (config sha256:71f8957e5528b230af4968efb9ac4a703665e04eb5eef4905b0a22f4b8c2f430),
built with `docker compose build world` after all C++ repairs; newer than the last
source edit, so it contains the committed repair set.

Focused fixtures, all against that image, run 2026-09-14 (evidence under ignored
`local/`; run logs `bench-run1.log`, `priority-run4.log`, `hold-run2.log`):

- `test_bot_bench.py`: 6/6 OK in 148 s. Recruit, dismiss-to-bench, no
  population re-login between dismiss and recall, recall across an isolated world
  restart with unchanged saved character fields, ambient population maintained by
  eligible ambient bots only, personal containers untouched.
- `test_bot_companion_priority.py`: 5/5 OK in 80 s. Fixture: level-10 bots,
  owner 60 yd from the combat (outside the companion's 30 yd target range, so a
  single attacker), creature 80 = Kobold Laborer (level 3-4, ~95 HP, attackable
  faction). Combat outlives the 3 s follow goal, post-follow combat continues
  through `ExecuteCompanion`, the companion resumes follow and reaches the owner.
  `creature.health_percent > 100` is rejected at world load (ObjectMgr resets it
  to 100), so HP must come from the template; earlier 33x buff and Snufflesnout
  (204 HP, ~4 min fight near the wait limit) attempts are superseded, not valid evidence.
- `test_bot_companion_hold.py`: 1/1 OK in 118 s. Hold seq 2 and 4 active,
  stale goals (seq 2 at current 2, seq 3 at current 4) rejected, non-owner hold
  rejected, `.bothyld` alias works, exactly three follow activations, no crash.

Remaining gaps (fixture evidence does not cover these):

- Hold during active combat is exercised only by the priority path; a dedicated
  hold-while-fighting scenario is not present.
- Normal death/recovery persistence for the companion is not fixture-verified;
  the unconditional resurrect-on-login was removed, but an in-game death/relog
  check is still required (PORT-010 evidence).
- Full regression (follow/population/party suites) and independent review are
  not claimed. Synthetic saved-field comparison does not prove earned
  inventory/quest persistence.

## Next bounded assignment

Dispatch [PORT-005](port-005.md) (owner-selected assist target) with a hashed
evidence card against the committed repair baseline. Do not set prd.json
passes=true for PORT-002/003/004 from fixtures alone; acceptance still needs the
PORT-010 in-game pass and the tracked regression/review gates.

## Operational handoff

Do not restart or deploy to `tortoise-local-world-1` until phase 1 is complete;
the user verifies in-game after PORT-010. Never touch personal database volumes.
Retrieval/embedding is operator-paused. Local worker launch was blocked by
PowerShell execution policy; no local-worker review is claimed. Resolve the
approved launcher before local dispatch; do not weaken machine policy silently.

## Handoff update contract

Update this file at each session stop and phase exit. Never replace failed evidence
with a success summary: retain the failure and link the repair. Record:

- Date, active branch, baseline and resulting commit IDs; distinguish uncommitted work.
- Changed paths and relevant functions; why the change exists and its invariants.
- Commands actually run, exit results, exact tested source/image, and sanitized evidence paths.
- Acceptance scenarios passed, failed, or not run; separate fixture evidence from in-game observations.
- Known risks, unsupported behavior, dependencies, and the next bounded assignment.
- Worker/retrieval availability, independent review outcome, and any pending user decision.
- Deployment and Jira status independently; never infer them from a successful build.

Keep raw logs and synthetic lab artifacts under ignored `local/`. Never put secrets,
personal character data, or environment maps in these documents or worker cards.
Code comments should explain ownership, cancellation, lifetime and safety invariants,
not repeat syntax. Update affected comments when changing those contracts.
