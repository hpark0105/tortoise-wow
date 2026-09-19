# PORT-012: Extract authoritative combat execution and explicit target state

- Depends on: PORT-011
- Status: pending hosted dispatch; passes=false
- Tracking: KAP-543 / Phase 2 foundation
- Shared contract: [execution and review](README.md)

## Objective

Remove behavior drift between assist, defend and continue-combat by extracting
one deterministic target validator and combat executor. Separate a live combat
target from a defeated corpse selected for loot; replace ambiguous cleanup such
as `ClearTarget()` with explicit state transitions.

## Allowed edit candidates

- `src/game/PlayerBots/PlayerBotAI.cpp`
- `src/game/PlayerBots/PlayerBotAI.h`
- `src/game/PlayerBots/Companion/Combat.h`
- `src/game/PlayerBots/Companion/Combat.cpp`
- `src/game/CMakeLists.txt`
- Focused companion combat fixtures under `docker/`

## Acceptance

Assist, defend and existing-combat requests share the same live re-resolution,
legality, range/LOS, pursuit, cast-result and fallback rules while retaining
source-specific cancellation messages. A defeated combat target transitions
explicitly to a loot-corpse GUID. Clearing combat state cannot silently clear an
unrelated owner order, and clearing loot state cannot revive a combat target.

The shared executor distinguishes known from currently usable abilities before
selection: passive, obsolete-rank, unaffordable, cooldown-blocked,
stance/form-restricted, reagent-blocked and target-inappropriate spells are not
selected as executable casts. It returns and records distinct bounded outcomes
for no eligible ability, an attempted cast plus its authoritative result, and
ordinary-attack fallback. A failed cast does not consume the AI action window;
the bot immediately attempts the next legal deterministic action or maintains
its ordinary attack.

Audit the pinned core's `CURRENT_MELEE_SPELL` lifecycle for abilities carrying
`SPELL_ATTR_ON_NEXT_SWING_1/2`. Either implement and behaviorally prove a bounded
one-shot queue/clear lifecycle, or classify these learned abilities explicitly
as unsupported in the capability snapshot. The Phase 1.1 blanket exclusion is
preserved until that decision is accepted; extraction must not silently make an
excluded ability appear usable to a role policy or planner.

## Failure cases

This is a behavior-preserving extraction, not new tactics. Do not move ownership,
session, database, loot execution or recovery authority into the combat module.
Do not retain `Unit*` across ticks. Do not solve the on-next-swing freeze by
granting replacement damage, bypassing the normal spell path, or allowing a
policy/model to queue raw spell IDs.

## Validation

Compile; run the complete Phase 1 companion regression matrix and compare
target, attack, leash, loot and regroup outcomes. Add unit-level/value-level
coverage for capability filtering and every supported cast-result category where
possible, including no-eligible-ability versus rejected-attempt outcomes and the
accepted on-next-swing classification. If one-shot abilities are supported, prove
the queued slot clears and later white swings continue; if unsupported, prove
they cannot be selected. Do not substitute structural tests for behavior.

Current checkpoint: the live/loot target split and shared Assist,
ContinueCombat and Defend executor already exist at `796bc67`. Preserve them as
reviewed baseline behavior and extract them without dispatching a duplicate
implementation.
