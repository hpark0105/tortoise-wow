# BL-004 worker evidence card v1

Date: 2026-09-22
Repository: `C:\Users\hpark\WebstormProjects\tortoise-wow`
Branch: `feature/kap-558-port-phase2`
Baseline: current uncommitted BL-001A/B/R, BL-002 and BL-003 worktree

## Objective

Add a pure, bounded tactical protocol v2 contract and deterministic fake adapter
for future companion policy proposals. Preserve PlannerProtocol v1 unchanged.
This card must not connect an LLM, change combat behavior, apply a plan, add SQL,
or add owner commands.

## Required behavior

- New `src/game/PlayerBots/Companion/TacticalProtocol.h` value-only module.
- Fixed-width request/response envelopes with protocol version, request id,
  observation/capability version, owner/party identity, immutable capture time,
  login/order generations, and bounded candidate steps.
- Closed action vocabulary limited to ability-priority candidate data: action
  slot/index, bounded ability id from the supplied capability catalog, optional
  gate/resource value, and expiry. No spell invention, targets, coordinates,
  code, SQL, prompts, or arbitrary strings.
- Pure all-or-nothing validator rejects invalid, oversized, stale, reordered,
  duplicate, capability-mismatched, owner-held, login-generation-mismatched,
  order-generation-mismatched, expired, and out-of-range responses.
- Fake adapter/value state machine exercises submit, one-in-flight replacement,
  response matching, immutable timestamps, owner hold, capability change,
  stale response, and fail-closed fallback. It never mutates PlayerBotAI.
- Keep v1 PlannerProtocol files byte-for-byte behavior-compatible; do not edit
  them unless a compile-only include is unavoidable.

## Allowed reads

This card, `docs/prd/bram-party-learning.prd`,
`src/game/PlayerBots/Companion/PlannerProtocol.h`,
`src/game/PlayerBots/Companion/PlannerTransport.h`,
`src/game/PlayerBots/Companion/Observation.h`, and the existing planner value
test/runner only. Do not list directories or inspect unrelated files.

## Allowed edits

- `src/game/PlayerBots/Companion/TacticalProtocol.h` (new)
- `docker/test_companion_tactical_protocol_value.cpp` (new)
- `docker/test_companion_tactical_protocol_value.py` (new)
- this worker card only for evidence updates are forbidden; the head updates handoff.

Do not commit, stage, push, deploy, start the realm, call retrieval, launch a
nested model, or edit PlannerProtocol/PlannerTransport/runtime AI.

## Validation

Run only `python docker/test_companion_tactical_protocol_value.py -v` and
`git diff --check`. Report exact tests, hashes, assumptions, and uncertainties.
