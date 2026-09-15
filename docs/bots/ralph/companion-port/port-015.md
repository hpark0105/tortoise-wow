# PORT-015: Implement one legal healer triage policy

- Depends on: PORT-013
- Status: pending hosted dispatch; passes=false
- Roll-up: CMP-013
- Shared contract: [execution and review](README.md)

## Objective

Implement deterministic triage for one declared Turtle healer class and level.
Choose among self, owner, tank and other supported party members using explicit
health/resource thresholds, normal range/LOS checks and learned legal spells.

## Allowed edit candidates

- One healer policy under `src/game/PlayerBots/Companion/`
- Observation/intent additions strictly required by triage
- Authoritative positive-spell execution in `PlayerBotAI.cpp/.h`
- One disposable healer fixture under `docker/`
- `src/game/CMakeLists.txt` when a compiled source is added

## Acceptance

The healer selects the declared highest-priority injured legal party member,
casts only a learned affordable heal, preserves enough mana for the approved
small-pull policy, follows to regain range without unsafe teleporting, and
returns to deterministic follow or recovery afterward. Every attempted heal uses
the PORT-012 cast outcome diagnostics. A rejected heal does not create a false
success or delay and immediately produces the next legal triage/follow outcome.

## Failure cases

No mana, no legal spell, owner loss, map mismatch, unreachable or out-of-LOS
targets, death, Hold, or a newer order prevents the heal and produces a bounded
safe outcome. Do not fabricate regeneration, spell ranks or health changes.

## Validation

Compile; deterministic competing-injury, low-mana, range/LOS, hold/stale-order
and post-pull recovery scenarios. Record the exact class/level/spells and actual
mana and health deltas.
