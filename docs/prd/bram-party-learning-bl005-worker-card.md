# BL-005 worker evidence card v1

Date: 2026-09-22
Repository: `C:\Users\hpark\WebstormProjects\tortoise-wow`
Branch: `feature/kap-558-port-phase2`

## Objective

Add the pure warrior playbook adapter that converts a validated BL-004 candidate
ability ordering into the existing fixed `Combat::RotationPlan` selector. Keep
the runtime on the exact baseline until a later evaluator explicitly assigns a
trial. No LLM, persistence, promotion, owner command, or automatic behavior
change is part of this card.

## Required behavior

- Extend `Companion/Combat.h` with a value-only warrior playbook adapter.
- Accept only a bounded list of ability IDs from the current live catalog and
  produce a fixed index permutation for `SelectExecutable`.
- Reject unknown, duplicate, missing, oversized, or capability-version-mismatch
  candidates and return the baseline identity plan.
- Preserve every existing eligibility check, on-next-swing exclusion, tank
  route, owner/emergency preemption and class behavior.
- Add focused value coverage for Bram’s BL-001R candidate
  `Charge,Rend,Hamstring,Heroic Strike`, baseline parity, invalid plans,
  unsupported classes, and candidate exposure changing one legal selector
  choice in a synthetic fixture.

## Allowed reads

This card, `docs/prd/bram-party-learning.prd`,
`src/game/PlayerBots/Companion/Combat.h`,
`src/game/PlayerBots/Companion/TacticalProtocol.h`,
`docker/test_companion_combat_value.cpp`, and the BL-001R audit only.

## Allowed edits

- `src/game/PlayerBots/Companion/Combat.h`
- `docker/test_companion_combat_value.cpp`
- this card is evidence-only; the head updates the handoff.

Do not edit PlayerBotAI, PlannerProtocol v1, persistence, SQL, CMake, commit,
stage, push, deploy, start the realm, call retrieval, or launch another model.

## Validation

Run only `python docker/test_companion_combat_value.py -v` and `git diff --check`.
