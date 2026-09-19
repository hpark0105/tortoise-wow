# PORT-016: Implement damage assist and pull discipline

- Depends on: PORT-014, PORT-015
- Status: implemented, runtime-verified in a disposable lab; passes=false (in-game
  verification sign-off pending)
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

## Implementation notes (KAP-558)

Policy: `src/game/PlayerBots/Companion/Damage.h` (version 2) - one declared melee
matrix (Rogue, class 4, level >= 1; 1752 Sinister Strike / 2098 Eviscerate /
703 Garrote reused from the per-class book; Garrote is stealth-gated and
therefore known-but-unusable in the bot flow; no on-next-swing ability is
declared - the PORT-012 audit found none usable). Pure value contract:
`Observation` (low GUIDs, threat facts, CC flag, reach/LOS/held/owner gates),
`PullEstablished` (tank is the target's victim, or tank stored threat >= 1.1x
the victim's - the Tank.h integer margin), and `Select` (held, owner loss,
missing tank/target, unestablished pull, controlled target, unreachable ->
None; otherwise Damage). Priority sits below Defend and above Loot.

The adapter (`PlayerBotAI.cpp`) fills the observation each tick only when the
selection would otherwise be Follow/Loot, and hands the shared executor the
**packed** object GUID (`Observation.targetRaw`) because the executor's map
lookup uses packed GUIDs (the assist contract: `GetObjectGuid().GetRawValue()`).
The policy and the log fixture keep the low-GUID contract. Crowd-control
preservation set (any source marks the target controlled): STUN, ROOT, CHARM,
CONFUSE, FEAR, PACIFY, TRANSFORM, FEIGN_DEATH. Failure cases are bounded
wait/follow with a 2 s `[Damage] wait` diagnostic line; a rejected engagement
is dropped (combat stopped, live slot released) and the prior order resumes -
never the nearest hostile.

## Evidence (disposable lab, feature/kap-558-port-phase2)

`docker/test_bot_damage.py` (10/10 OK): owner (priest, L40) + declared tank
(warrior L10, Taunt 355) + declared damage (rogue L14) + undeclared class-8
bot; the owner is deliberately not a warrior because
AutoLearnSpellsForLevel fills the class book and a warrior owner would pass
the declared-tank gate before the real tank. Pulls: the rooted, controlled mob
first (aura 17743, a 120 s self-cast root - the only CC that survives the run
in this fork's pinned DBC), the reactive vermin second (exercises the live
two-target cap with the rooted mob's stored threat).

Proven: pre-pull `[Damage] wait` lines with no established target; the rogue's
first engagement strictly after the tank's pull; `established:1 cc:1` hold on
the rooted target with zero damage lines; the unrelated bystander never named;
the owner hold override stops all rogue action; the undeclared class is inert;
B/C survive; personal containers untouched. Value tests (damage/policy/combat/
tank/healer) all OK; Phase 1 matrix (7 fixtures) + tank (12) + healer (12)
fixtures green against the same image.

Known limitation: this fixture proves the policy's timing, targeting and
suppression contracts; in-game combat rotation quality (ability sequencing,
energy pacing against real targets) is the later acceptance step.
