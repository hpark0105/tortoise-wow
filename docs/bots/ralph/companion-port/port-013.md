# PORT-013: Establish the immutable observation and typed directive boundary

- Depends on: PORT-012
- Status: pending hosted dispatch; passes=false
- Tracking: KAP-543 / Phase 2 foundation
- Shared contract: [execution and review](README.md)

## Objective

Replace the minimal combined `Companion/Policy.h` contract with explicit,
versioned value types for observation, candidate actions, selected directives
and execution validation. Make Defend a first-class typed action and express the
complete deterministic priority in one selection path. Keep recovery as an
explicit lifecycle preemption above behavior selection. The existing Defend
action and priority at `796bc67` are baseline behavior to preserve, not a second
implementation assignment.

## Allowed edit candidates

- `src/game/PlayerBots/Companion/Observation.h`
- `src/game/PlayerBots/Companion/Intent.h`
- `src/game/PlayerBots/Companion/Policy.h`
- Optional matching `.cpp` files and `src/game/CMakeLists.txt`
- `src/game/PlayerBots/PlayerBotAI.cpp`
- `src/game/PlayerBots/PlayerBotAI.h`
- Focused pure-policy and behavior fixtures

## Acceptance

The snapshot contains no live pointers and is bounded and versioned. Hold,
Assist, ContinueCombat, Defend, Loot and Follow share one documented priority.
Every directive carries only allowlisted values and is revalidated against live
state immediately before execution. Adding a candidate policy requires no
session, login, ownership or database change. Pure value-level tests cover every
priority edge (`Hold > Assist > ContinueCombat > Defend > Loot > Follow`), stale
generation rejection, and the absence of retained world pointers.

## Failure cases

Do not introduce an abstract plugin loader, generic expression evaluator, raw
`Unit*`, arbitrary coordinates, spell IDs supplied by external text, or I/O in a
pure policy. A structural split alone is not acceptance; Phase 1 behavior must
remain unchanged.

## Validation

Compile; run pure priority/generation tests plus the complete Phase 1 companion
matrix. Prove stale directives, invalid owner state and changed target state are
rejected at execution.
