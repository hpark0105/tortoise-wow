# Phase 1 companion code review

Date: 2026-09-14  
Branch: `feature/kap-558-port-companion-port`  
Reviewed committed baseline: `e6a9f8a`  
Status supplied by operator: Phase 1 completed and tested in game; behavior is
mostly correct, with an observed `.botassist` failure around dead targets.

## Scope and evidence

This is a read-only architecture and behavior review of the Phase 1 companion
implementation, with emphasis on target selection, combat execution, and the
transition into Phase 2. It does not replace the operator's in-game acceptance
evidence.

At review time, the worktree also contained an uncommitted change in
`PlayerBotMgr.cpp` that prefers a live `.botassist` name match over a dead match,
plus an untracked `docs/bots/companion-commands.md`. Those are treated as work in
progress and are not attributed to `e6a9f8a`.

Retrieval was intentionally deferred until after MVP because the embedding
service is unavailable. The review used bounded direct reads of the current
source. A fresh read-only `park-agent` review could not start because another
`park-head` or `park-agent` session held the shared-model lock. No delegated
review is claimed. No build or Docker fixture was run for this review.

## Executive assessment

Phase 1 is a strong deterministic foundation. It has the right core safety
properties for later model integration:

- Value-based policy observations.
- Long-lived targets stored as GUIDs and re-resolved before execution.
- Monotonic owner-order and login-session generations.
- Hold and recovery paths that preempt normal behavior.
- Bounded pursuit, loot, recovery, and path retry behavior.
- Deterministic gameplay that does not require a model.

The implementation should receive a small Phase 1.1 hardening pass before the
Phase 2 transport or real model is connected. The main risks are not the overall
design; they are duplicated combat execution, ambiguous target state, incomplete
cast-failure handling, and a policy vocabulary that does not yet describe all
of the behavior actually executed by `PlayerBotAI`.

## Findings

### 1. Failed spell casts can suppress useful attacks

Severity: high

`SelectOffensiveSpell()` checks learned spells, cooldown, basic power, range,
and selected aura constraints. The assist, continue-combat, and defend paths
then call `CastSpell()`, arm `_abilityTimer`, and return without inspecting the
`SpellCastResult`.

A cast can still be rejected by the core for stance, combat state, control,
target restrictions, or another spell-specific rule. When that happens, the
bot does not perform the melee fallback on that evaluation and waits for the
new ability timer. Repeated selection of the same unusable preferred spell can
make the companion appear passive or intermittent.

Relevant code:

- `PlayerBotAI.cpp::SelectOffensiveSpell`
- The spell branches in assist, continue-combat, and defend execution
- `PlayerAI::CanCastSpell` and the `SpellCastResult` returned by `CastSpell`

Recommended direction:

- Treat a spell as executed only when `CastSpell()` reports success.
- Arm `_abilityTimer` only after a successful cast.
- On failure, continue to the deterministic melee/ranged fallback in the same
  evaluation where legal.
- Add a regression where the preferred known spell is unusable but an ordinary
  attack remains legal.

### 2. The live-over-dead assist fix matches the observed defect

Severity: high for the reported gameplay symptom

The previous `.botassist` name search allowed a nearby dead unit with the same
name to win over a live unit. That explains the operator's observation that an
assist command was accepted or attempted but the companion did not meaningfully
attack after corpses accumulated.

The current uncommitted `BotAssistNameCheck` change tracks the nearest live and
nearest dead matches separately and returns the live match when one exists. The
approach looks correct for the specific live-versus-corpse ambiguity.

It still selects the nearest *live unit*, not the nearest *legal live hostile
creature*. A closer player, friendly NPC, nonattackable NPC, or other invalid
live unit with the same name can hide a valid hostile farther away. Later
validation rejects the chosen unit without trying the valid candidate.

Recommended candidate order:

1. Nearest legal live hostile creature.
2. Nearest invalid live match, retained only for a precise rejection reason.
3. Nearest dead creature, retained for `target-dead` when no live match exists.

Required focused coverage before accepting the fix:

- One dead and one live creature with the same name are both in range; the live
  GUID is accepted and attacked.
- A closer invalid live match does not mask a farther legal live hostile.
- With only a dead match, the existing `target-dead` diagnostic remains intact.

Longer term, an owner-selected target GUID would be less ambiguous than a name.
Name lookup can remain a convenient command fallback.

### 3. Combat execution is duplicated

Severity: medium

Assist, continue-combat, and defend separately implement target revalidation,
pursuit-leash handling, ability timers, movement/facing, spell selection,
ordinary attack fallback, and loot-target recording. These branches have already
developed small differences in LOS and cleanup behavior.

Phase 2 role or personality preferences would multiply those differences if
they were added directly to each branch.

Recommended direction:

- Introduce one deterministic combat request/executor shared by all three
  sources.
- Include target GUID, source (`assist`, `defend`, or existing combat), order
  generation, maximum distance, and pursuit policy as values.
- Re-resolve and validate the target inside that executor.
- Keep spell and melee execution authoritative in C++; a model must not bypass
  it or execute game actions directly.

### 4. `_lootTargetGuid` represents two different states

Severity: medium

`_lootTargetGuid` is used first as a remembered live combat target and later as
the corpse to loot. `GetAliveHeldTarget()` interprets it as combat state, while
`TryLootDefeatedTarget()` interprets the same field according to whether the
creature is dead. `ClearTarget()` sounds general but clears only this loot/combat
field and related loot counters.

The life-state transition makes the current behavior work, but it creates an
implicit state machine that is difficult to expose safely in a Phase 2
observation.

Recommended direction:

- Separate `combatTargetGuid` from `lootCorpseGuid`.
- Use explicit cleanup operations such as `ClearCombatTarget()` and
  `ClearLootTarget()`.
- Make transitions from defeated combat target to loot corpse deliberate and
  observable.

### 5. Defend is not a first-class typed intent

Severity: medium

`Companion::Action` contains `None`, `Hold`, `Follow`, `ContinueCombat`,
`Assist`, and `Loot`. Defend selection is injected imperatively in
`PlayerBotAI::UpdateCompanion()` after `Companion::Select()` returns.

As a result, the documented priority that includes Defend is not represented by
the supposedly authoritative policy vocabulary. A future planner and the
deterministic policy could otherwise reason over different action sets.

Recommended direction:

- Represent the defend candidate in the immutable observation.
- Add `Defend` to the typed action vocabulary.
- Express the complete deterministic priority in one policy selection path.
- Continue to re-resolve and revalidate the defend GUID in the executor.

Recovery may remain a lifecycle preemption above the policy layer; it should be
documented explicitly as such.

### 6. The current intent is not an asynchronous response envelope

Severity: medium and a Phase 2 entry requirement

The Phase 1 intent contains only action, order generation, and target. That is
appropriate for same-tick deterministic selection but insufficient for a model
response that can arrive after logout, relog, a new owner command, a map change,
or observation expiry.

Every Phase 2 request and response should contain at least:

- Bot GUID.
- Owner or party identity as appropriate.
- Login/session generation.
- Owner-order or goal generation.
- Request ID.
- Observation version and capture time.
- Map identity.
- Resolvable target GUID, never a retained `Unit*`.
- Absolute or relative expiration deadline.

`PlayerBotEntry::loginGeneration` and `followSeq` already provide two important
inputs. Both must match before a response is offered to deterministic selection.
Timeout, malformed, unsupported, stale, or mismatched responses must result in
no model action and uninterrupted deterministic behavior.

### 7. Temporary combat diagnostics should be narrowed

Severity: low

The PORT-009 assist probe emits a large record on each paced combat evaluation
while debug logging is enabled. It was useful for finding the cinematic and
targetability defect, but Phase 2 will add request, response, timeout, rejection,
and fallback diagnostics.

Recommended direction:

- Remove the temporary probe after the defect is closed, or place it behind a
  narrower diagnostic setting.
- Prefer bounded structured event records for planner observability.
- Never send raw logs to the model service.

### 8. Phase documentation and implemented boundaries have drifted

Severity: medium for maintainability

The older architecture describes separate observation, intent, and deterministic
policy files, but Phase 1 currently combines the minimal policy types in
`Companion/Policy.h`. That was a reasonable bounded implementation choice, but
Phase 2 should create explicit boundaries before adding transport code.

The older `CompanionDirective` example also contains raw `Unit*` targets. This
conflicts with the newer and safer Phase 2 rule that asynchronous work must carry
GUIDs and never retain world pointers. The newer rule is authoritative and the
older example should be reconciled.

The tracked Phase 1 handoff and `port-010.md` also still describe acceptance as
pending. The operator has now supplied in-game completion evidence, with the
dead-target assist defect noted. Record that evidence and the bounded follow-up
so the repository's Phase 2 entry gate reflects the actual state.

## Recommended Phase 1.1 hardening sequence

Complete these in order before connecting the real model:

1. Finish the live-over-dead assist correction and add same-name candidate
   regressions.
2. Correct failed-cast handling and prove a legal attack continues when a
   preferred spell is rejected.
3. Extract common deterministic combat validation and execution.
4. Separate live combat target state from corpse-loot state.
5. Make Defend a first-class typed intent and centralize priority selection.
6. Define immutable Phase 2 observation, directive, and asynchronous envelope
   schemas with GUIDs and generation checks.
7. Build a fake asynchronous planner harness and prove timeout, malformed
   output, unsupported intent, stale response, hold cancellation,
   logout/relogin, map change, and deterministic fallback.
8. Connect the real local model only after the fake-service contract passes.

## Recommended Phase 2 authority boundary

The model should initially propose bounded personality-level preferences, not
game actions. Suitable proposals include:

- A cautious or aggressive posture within deterministic limits.
- An expression or emote from an allowlist.
- A preference among C++-defined, prevalidated tactics.
- A non-authoritative explanation or party comment.

The model should not select raw spell IDs, arbitrary target names, live object
pointers, movement coordinates, SQL, commands, or executable code. C++ remains
authoritative for owner orders, target legality, range, LOS, cooldowns, resource
checks, movement, combat, loot, recovery, and final execution.

Model unavailability must be behaviorally equivalent to receiving no preference:
the deterministic companion continues functioning without delay.

## Phase 2 readiness conclusion

Phase 1 will transition well after the focused hardening above. The difficult
safety primitives already exist. The next step should be clarification and
consolidation of the deterministic boundary, followed by a fake asynchronous
protocol—not direct LLM calls from `PlayerBotAI::UpdateAI()`.
