# PORT-011: Close deterministic targeting and cast-result gaps

- Depends on: PORT-010
- Status: pending hosted dispatch; passes=false
- Tracking: KAP-543 / Phase 2 entry hardening
- Shared contract: [execution and review](README.md)

## Objective

Reconcile and preserve the Phase 1.1 combat hardening already landed through
`796bc67` before refactoring or model integration: `.botassist` chooses a legal
live hostile over dead or invalid same-name matches, and a rejected spell cast
cannot suppress a legal ordinary attack. Close the remaining cast-result
diagnostic ambiguity without reimplementing the accepted targeting or fallback
paths.

## Allowed edit candidates

- `src/game/PlayerBots/PlayerBotMgr.cpp`
- `src/game/PlayerBots/PlayerBotAI.cpp`
- `docker/test_bot_companion_assist.py`
- One new focused deterministic combat fixture under `docker/`

## Acceptance

A dead same-name corpse cannot mask a live legal assist target; a closer invalid
live match cannot mask a legal hostile; dead-only lookup still reports
`target-dead`. When a selected learned spell is rejected by the authoritative
core cast path, the bot does not arm a false ability delay and performs the
legal deterministic fallback without losing its target. Diagnostics identify
the selected spell and a bounded authoritative outcome/rejection category such
as success, insufficient power, range/LOS, cooldown, stance/form, facing or
invalid target; they do not treat an attempted cast as success. `NoEligibleAbility`
is distinct from an attempted cast and its rejection: `spell:0` plus an unknown
failure value must not be reported or consumed as a rejected cast. Ordinary
attack fallback is also recorded as its own bounded outcome.

## Failure cases

Do not weaken target legality, allow PvP selection, invent spell rules, or treat
an attempted cast as success. Do not manufacture a failure result when no spell
was selected. Name lookup remains synchronous and must not retain visitor pointers
after the command returns.

## Validation

Compile; run focused same-name live/dead/invalid assist scenarios and an
unusable-spell-with-legal-fallback scenario; rerun assist, defend, regroup,
leash and recovery fixtures. Record the exact class, learned spell, cast result,
target GUIDs and fallback observed.

Baseline evidence to preserve: assist 11/11 in 224.5 s at `796bc67` during the
2026-09-15 hosted review. This is starting evidence, not acceptance of the
remaining diagnostic contract or permission to mark `passes=true`.
