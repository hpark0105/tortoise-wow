# BL-008A evidence card v1 — learning control repository seam

## Objective

Add a bounded C++ repository/value seam for owner learning control state. This is the prerequisite for `.botlearn`; do not register commands in this card.

## Allowed files

- `src/game/PlayerBots/Companion/LearningStore.h`
- `src/game/PlayerBots/Companion/LearningStore.cpp`
- `docker/test_companion_learning_store_value.cpp`
- `docker/test_companion_learning_store_value.py`
- this card

## Required behavior

- Define typed modes: disabled, observe, shadow, trial, paused.
- Define bounded status values: active/expected playbook version, candidate state/evidence count, and explicit insufficient-evidence state.
- Provide parameterized async operations for start, pause, resume, and rollback; no direct SQL from command handlers.
- Pause/rollback must record audit provenance and select baseline atomically or fail closed.
- Status reads must be bounded and must never fabricate improvement.
- Preserve one-in-flight writer and world-thread completion ownership.

## Validation

Extend the existing value harness for transitions, unauthorized/invalid modes, CAS failure, rollback, DB failure, restart-safe status, and insufficient evidence. Run the focused harness and `git diff --check`. No personal realm or database.

## Safety

No commands, combat wiring, model calls, commits, secrets, raw logs, or nested model sessions. Preserve unrelated uncommitted work.
