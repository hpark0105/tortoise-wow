# BL-001B worker evidence card v1

Date: 2026-09-22
Repository: `C:\Users\hpark\WebstormProjects\tortoise-wow`
Branch: `feature/kap-558-port-phase2`
Baseline commit: `c5781974b444f6ad692b48a516b0994d8cf59bc5` with accepted, uncommitted BL-001A changes

## Objective

Extract the smallest pure, value-only routing contract that makes rotation-plan
applicability explicit for combat engagement sources, then use it in
`PlayerBotAI::ExecuteCombat` without changing behavior.

Required routing matrix:

- Assist + declared tank: retain the existing Tank threat/Taunt branch and bypass rotation.
- Assist + not declared tank: use the ordinary rotation selector.
- ContinueCombat: use the ordinary rotation selector regardless of tank capability.
- Defend: use the ordinary rotation selector regardless of tank capability.
- Damage: preserve current ordinary rotation behavior; do not expand this card into damage-policy work.

BL-001B does not add persistence, LLM input, a learned plan, abilities, targeting,
or gameplay mechanics. The baseline identity plan from BL-001A stays active.

## Allowed reads

- this card
- `docs/prd/bram-party-learning.prd`
- `src/game/PlayerBots/Companion/Combat.h`
- `src/game/PlayerBots/PlayerBotAI.cpp`
- `src/game/PlayerBots/PlayerBotAI.h`
- `docker/test_companion_combat_value.cpp`
- `docker/test_companion_combat_value.py`

## Allowed edits

- `src/game/PlayerBots/Companion/Combat.h`
- `src/game/PlayerBots/PlayerBotAI.cpp`
- `docker/test_companion_combat_value.cpp`

Do not edit any other file. Do not commit, stage, push, deploy, start services,
read secrets/private data, call retrieval, or launch another model session.

## Verified provenance

- Retrieval was healthy/fresh after a no-op sync on 2026-09-22.
- `PlayerBotAI.cpp` SHA-256: `04CFE82C28B9BC25D696E9941241AAB5E8B522AE1CF16F837E4839A4BE42CF2D`
- `PlayerBotAI.h` SHA-256: `1660C58E567937820323A89F9F69B134AD0EAB51862BD038B7CEDB0B23AADB45`
- `Combat.h` SHA-256: `DEB3F05722ADC2114229778DACB2D6E259AF100F5BD7B7A7DD840DA3FD88E2B4`
- `test_companion_combat_value.cpp` SHA-256: `E5601B74D024D05D7B087F2B3DACAE433819E26F031FC938F69C10C976051976`
- Current source already funnels Assist, ContinueCombat, Defend, and Damage through
  `ExecuteCombat`; only Assist plus `IsDeclaredTank()` owns the Tank branch.

## Implementation constraints

- Add a pure routing decision in `Companion::Combat` with a bounded enum/result.
- The routing decision accepts only `Source` and whether the declared-tank gate is true.
- Replace the inline Assist/tank condition in `ExecuteCombat` with that decision.
- Preserve the Tank observation, logs, Taunt decision, white-attack fallback,
  `_abilityTimer`, LOS, pursuit, target validation, and source cancellation semantics.
- Add value tests for every row in the required matrix and a stable name/value for
  applicability diagnostics if useful. Do not add runtime heap allocation.
- Keep all BL-001A tests passing.

## Validation

Run:

`python -m unittest discover -s docker -p 'test_companion_combat_value.py'`

`git diff --check`

Do not run the personal realm or database fixtures. The hosted head will perform
the Docker world build and final acceptance.

## Expected report

Report changed paths, exact tests/results, the routing matrix implemented, and
any uncertainty. State explicitly that no learned behavior was enabled.
