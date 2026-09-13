# Companion architecture: minimal boundaries

This document proposes the module boundaries and interface contracts for the
one-useful-companion port (PORT-002..010). Hosted Codex approves or revises
before implementation. No runtime framework is introduced.

## Design constraint

One companion per owner. No population, no LLM runtime, no generic strategy
engine. The existing two-file structure (`PlayerBotMgr` + `PlayerBotAI`) is
retained and extended in place.

## Module responsibilities

```
PlayerBotMgr (lifecycle + persistence + ownership)
  - Provision, login, logout, recruit, dismiss, recall
  - Party membership and owner tracking
  - Save/restore (DB), stale-login detection
  - Follow/stop command routing
  - Does NOT: target selection, ability casting, loot decisions

PlayerBotAI (per-bot behavior policy, extends PlayerAI)
  - UpdateAI(diff): the single per-tick policy loop
  - Priority-ordered behavior checks (see below)
  - Does NOT: lifecycle, DB writes, party management, config

CompanionObservation (read-only value struct, proposed new)
  - Owner pointer, owner-alive, owner-in-combat, owner-victim
  - Self: alive, hp-pct, mana-pct, in-combat, current-victim, selection
  - Distance to owner, LOS to owner, owner-target (if any)
  - Populated once per tick at the top of UpdateAI
  - No methods; no mutation; no side effects

CompanionPolicy (behavior decision, inlined in PlayerBotAI for now)
  - Takes a CompanionObservation
  - Returns a typed intent: Follow, Hold, Attack(target), Loot(corpse),
    Resurrect, Idle, Recover
  - Does NOT: execute game actions directly; does NOT own sessions or DB
  - One policy class per role (DPS companion for this wave)
```

## Dependency direction

```
PlayerBotMgr  -->  PlayerBotAI  (lifecycle events: login, recall, follow/stop)
PlayerBotAI   -->  CompanionObservation  (reads world state each tick)
PlayerBotAI   -->  CompanionPolicy       (asks for intent)
PlayerBotAI   -->  Unit/Player/Creature  (executes the intent via native APIs)

No reverse dependencies. No circular references.
CompanionObservation and CompanionPolicy do not include PlayerBotMgr headers.
```

## UpdateAI priority (DPS companion)

Each tick, `PlayerBotAI::UpdateAI(diff)` builds a `CompanionObservation`,
then walks this ordered list. First match wins; execution is immediate:

1. **Dead/Recovery** (PORT-009)
   - If self is dead: if within corpse range and can self-resurrect, do it.
     Otherwise: idle (wait for owner to rez or for login-rez on recall).
   - Upstream ref: `DeadStrategy::InitTriggers` priority order.

2. **Attack current target** (already implemented in MVP)
   - If in combat and has a valid living victim in range: maintain attack.
   - If victim is dead or out of range: clear victim, fall through.

3. **Assist owner's target** (PORT-005)
   - If not in combat, owner is in combat, owner has a hostile target:
     Validate target (in-world, not-friendly, not-dead, LOS, valid).
     Attack it.
   - Upstream ref: `DpsAssistStrategy` trigger "not dps target active" +
     `AttackMyTargetAction::Execute`.

4. **Defend owner** (PORT-006)
   - If not in combat, owner is being attacked (owner->GetVictim() is
     attacking owner, or owner has a hostile attacker within aggro range):
     Attack the owner's attacker.
   - Upstream ref: assist trigger with target = owner's attacker.
   - Distinction from PORT-005: assist targets the owner's *selection*;
     defend targets the *attacker of the owner*.

5. **Loot** (PORT-007)
   - If not in combat, a party-owned corpse is within loot range:
     Loot it.
   - Upstream ref: `LootAction` decision structure (simplified).

6. **Follow owner** (PORT-003, already in MVP)
   - If owner is alive, on same map, and distance > follow range:
     Move toward owner.
   - If owner is dead: do not follow (wait in place).
   - Upstream ref: `FollowAction::isUseful` distance check +
     `CanDeadFollow` guard.

7. **Hold/Stay** (PORT-004)
   - If a hold command is active (seq matches): stop moving, stay in place.
   - Upstream ref: `StayActionBase::Stay` (clear movement, stop, clear states).

8. **Idle/Wander** (existing)
   - If none of the above apply: gentle wander near current position.

## Typed intents

```cpp
enum class CompanionIntent {
    None,
    Follow,           // move toward owner
    Hold,             // stop and stay
    Attack,           // attack a specific Unit* (carried in intent)
    Loot,             // loot a specific Corpse* (carried in intent)
    Resurrect,        // self-resurrect at corpse
    Idle,             // wander
};

struct CompanionDecision {
    CompanionIntent intent;
    Unit* target = nullptr;       // for Attack
    Corpse* corpse = nullptr;     // for Loot
};
```

The policy function is a free function or a small class method:

```cpp
CompanionDecision EvaluateCompanionPolicy(CompanionObservation const& obs);
```

For this wave, this is inlined in `PlayerBotAI::UpdateAI`. If it grows
beyond ~100 lines, extract to `PlayerBotPolicy.cpp/.h` in the same
directory. No separate policy registry or plugin system.

## What is explicitly NOT in scope

- Generic strategy/action engine (no string-keyed registry, no trigger trees)
- Tank/healer role policies (wait for post-PORT-010)
- Multi-bot formations or group movement
- Transport/boarding AI
- LLM or personality adapter (the architecture allows a future
  `CompanionPersonality` that proposes bounded preferences into the
  observation, but it cannot execute actions)
- Quest logic, vendor/bank interactions, BG behavior

## File list (proposed)

| File | Status | Purpose |
|------|--------|---------|
| `src/game/PlayerBots/PlayerBotMgr.h/.cpp` | Existing | Lifecycle, persistence, party |
| `src/game/PlayerBots/PlayerBotAI.h/.cpp` | Existing | Per-bot AI tick, executes intents |
| `src/game/PlayerBots/CompanionObservation.h` | New (PORT-003) | Read-only world snapshot struct |
| `src/game/PlayerBots/PlayerBotPolicy.h/.cpp` | New (PORT-005, if needed) | Policy evaluation (extracted from UpdateAI) |

No new directories. No new CMake targets. All changes are within the
existing `PlayerBots/` source group.

## Approval required

Hosted Codex must approve:
1. The priority order above (especially the assist-vs-defend distinction).
2. The typed intent enum (any additions before PORT-005).
3. The file list and extraction threshold (100 lines).
4. The explicit exclusion list.

Changes to any of these require a revised architecture document.
