# PORT-016: Implement damage assist and pull discipline

- Depends on: PORT-014, PORT-015
- Status: pending hosted dispatch; passes=false
- Roll-up: CMP-014
- Shared contract: [execution and review](README.md)

## Objective

Implement one deterministic damage policy for one declared melee or ranged
class. It assists the established tank target, delays until pull ownership is
valid, preserves crowd control, and uses the common combat executor without
acquiring unrelated enemies.

## Allowed edit candidates

- One damage policy under `src/game/PlayerBots/Companion/`
- Observation/intent additions strictly required by pull discipline
- `PlayerBotAI.cpp` registration/dispatch only
- One disposable damage/CC fixture under `docker/`
- `src/game/CMakeLists.txt` when a compiled source is added

## Acceptance

Damage begins only after the declared tank threat gate, stays on the approved
target, never damages the controlled or unrelated creature, respects Hold and
newer owner orders immediately, and uses learned legal attacks with verified
cast-result diagnostics and fallback. Ability selection excludes known but
currently unusable spells. A rejected cast does not consume the action window or
suppress the ordinary attack/next legal damage action. The declared damage
matrix identifies on-next-swing abilities explicitly. It may use one only when
PORT-012 proved the queued melee slot clears and later white attacks continue;
otherwise the ability is unsupported and excluded from the policy's claimed
rotation.

## Failure cases

Missing tank target, insufficient threat, crowd-control ambiguity, owner loss,
unreachable target or unsupported class yields a bounded wait/follow outcome.
The policy must not solve ambiguity by selecting the nearest hostile.

## Validation

Compile; disposable threat-delay, two-target, controlled-target, owner override,
stale directive and unsupported-class scenarios. Rerun tank, healer and Phase 1
combat regressions.
