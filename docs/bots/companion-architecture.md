# Companion architecture: lively human-like bot

This document proposes the module boundaries and interface contracts for the
companion bot. The design goal is a bot that feels like a person playing the
game: it has personality, it makes mistakes, it learns, it gets better over
time. The LLM is the brain; the C++ engine is the body.

## Design principles

1. **The bot is a character, not a state machine.** It has a name, a
   personality stage, a memory of past encounters, and it expresses itself
   (chat, emotes, ability choices) in ways a human player would.

2. **LLM shapes, engine executes.** The LLM decides *what to do and how to
   feel about it*. The C++ engine decides *how to physically do it* (pathing,
   range checks, cooldown tracking, valid target validation). The LLM never
   calls game APIs directly.

3. **Safe by construction.** The LLM can only propose typed intents from a
   fixed vocabulary. The execution layer validates every intent against the
   current world state. A hallucinated "teleport into the sun" is rejected
   before it reaches the game world.

4. **Graceful degradation.** If the LLM service is unavailable, the bot falls
   back to a deterministic policy (still competent, just less expressive).
   The bot never freezes or crashes due to LLM unavailability.

5. **Progressive competence.** The bot starts as a "reckless noob" and
   improves over time through accumulated experience. This is not a quest
   system — it's emergent from the LLM reading its own combat history and
   adjusting its behavior profile.

## Timescale separation

```
TICK (100ms, C++):     movement, attack maintenance, follow, cooldowns
SLOW (5-30s, C++):     target re-evaluation, loot decisions, group checks
EVENT (on trigger):    LLM query for personality/strategy/expressions
REFLECTION (post-X):   LLM reviews combat log, updates BehaviorMemory
```

The C++ engine never blocks on an LLM call. LLM queries are async: the bot
continues its current behavior until a response arrives, then adjusts.

## Module responsibilities

```
PlayerBotMgr (lifecycle + persistence + ownership)
  - Provision, login, logout, recruit, dismiss, recall
  - Party membership, owner tracking, save/restore (DB)
  - Follow/stop command routing
  - Persists BehaviorMemory alongside character data
  - Does NOT: targeting, abilities, personality, LLM interaction

PlayerBotAI (per-bot body, extends PlayerAI)
  - UpdateAI(diff): per-tick mechanical execution
  - Executes intents from the policy layer
  - Tracks cooldowns, ranges, movement state
  - Validates intents before execution (safety layer)
  - Does NOT: decide what to do, express personality, call LLM

BehaviorPolicy (decision brain — pluggable)
  - DeterministicPolicy: priority-ordered if/else (fallback, always available)
  - LlmPolicy: queries PersonalityService, maps response to typed intents
  - Selects active policy based on config + LLM availability
  - Does NOT: execute game actions, own sessions, write DB

PersonalityService (LLM sidecar interface, local HTTP)
  - Receives: BehaviorMemory + current situation (structured JSON)
  - Returns: PersonalityDirective (typed intents + expression + risk modifiers)
  - Local only (localhost), uses park_llama or equivalent
  - Timeout: 5s; on timeout/error, caller falls back to DeterministicPolicy
  - Does NOT: access game server directly, mutate BehaviorMemory

BehaviorMemory (persistent per-bot, saved to DB)
  - personalityStage: enum (NOOB, LEARNING, COMPETENT, VETERAN)
  - stageProgress: float 0.0-1.0 within current stage
  - learnedRotations: list of ability sequences per encounter type
  - combatHistory: rolling window of recent encounters (summary, not raw)
  - lessons: list of LLM-generated behavioral notes ("kite before nuke")
  - quirks: personality traits ("charges first", "says 'easy' before trash")
  - Does NOT: contain credentials, full combat logs, or unbounded data
```

## Dependency direction

```
PlayerBotMgr  -->  PlayerBotAI          (lifecycle events)
PlayerBotMgr  -->  BehaviorMemory       (load/save on login/logout)
PlayerBotAI   -->  BehaviorPolicy       (asks: "what should I do?")
BehaviorPolicy --> PersonalityService   (LLM query, async)
BehaviorPolicy --> DeterministicPolicy  (fallback)
BehaviorPolicy --> CompanionObservation (reads current world state)
PlayerBotAI   -->  Unit/Player/Creature (executes validated intents)

PersonalityService is a separate process. It does NOT include game headers.
It communicates via localhost HTTP (JSON). The C++ side has a thin client.
```

## The typed intent vocabulary

This is the shared language between the policy layer (LLM or deterministic)
and the execution layer (C++). The LLM must express its output as one or
more of these intents. Anything outside this vocabulary is rejected.

```cpp
enum class CompanionIntent {
    // Movement
    Follow,            // move toward owner
    Hold,              // stop and stay in place
    Kite,              // maintain distance from current target (move away)
    Retreat,           // move away from combat (to owner or safe distance)

    // Combat
    Attack,            // attack a specific Unit* (target carried in intent)
    CastSpell,         // cast a specific spell by entry (validated: known,
                       //   off-cooldown, valid target, in range)
    UseItem,           // use a specific item (healing potion, etc.)
    Defend,            // target the entity attacking the owner

    // Recovery
    Resurrect,         // self-resurrect at corpse
    HealSelf,          // cast a self-heal (if available)

    // Interaction
    Loot,              // loot a specific corpse
    PickUp,            // pick up an item from the ground

    // Expression (non-mechanical, flavor only)
    Say,               // send chat message (text carried in intent)
    Emote,             // perform emote (EMOTE_ID carried in intent)
    React,             // set facial expression / stance (cosmetic)

    // Meta
    Idle,              // no action, wander gently
    Reflect,           // trigger a post-event LLM reflection (no game action)
};

struct CompanionDirective {
    std::vector<CompanionIntent> intents;  // ordered, first executable wins
    std::string expression;                // optional chat/emote text
    float riskModifier;                    // 0.0 (cautious) to 1.0 (reckless)
    uint32_t rotationSeq;                  // which learned rotation to follow
};
```

The LLM returns a `CompanionDirective` (as JSON). The C++ execution layer
iterates the intents in order and executes the first one that passes
validation. If all fail, it falls back to `Idle`.

## Personality stages and progression

```
NOOB (stage 0):
  - riskModifier: 0.8-1.0 (charges in, no kiting)
  - rotation: none (spams abilities on cooldown)
  - expressions: "LETS GO!", "one more!", dies a lot, "oops"
  - target selection: attacks nearest, no threat awareness
  - learns: basic "don't die" lessons, starts recognizing when to retreat

LEARNING (stage 1):
  - riskModifier: 0.4-0.7 (kites occasionally, uses stuns)
  - rotation: simple 2-3 ability sequences per class
  - expressions: less exclamation, more tactical ("stun first", "backing up")
  - target selection: focuses weakest, avoids adding
  - learns: proper rotation order, when to use defensives

COMPETENT (stage 2):
  - riskModifier: 0.2-0.5 (measured, kites consistently)
  - rotation: full class rotation with situational swaps
  - expressions: calm, occasional humor, "got this"
  - target selection: threat-aware, peels when owner is focused
  - learns: encounter-specific adjustments, item usage timing

VETERAN (stage 3):
  - riskModifier: 0.1-0.3 (conservative, efficient)
  - rotation: optimized, minimal waste
  - expressions: dry wit, mentor tone ("you're pulling too many")
  - target selection: optimal, assists owner perfectly
  - learns: advanced mechanics, pre-pulls, positioning
```

Stage progression is driven by accumulated experience (encounters survived,
deaths avoided, efficiency metrics). The LLM reviews combat history at
reflection points and may recommend a stage change. The C++ layer tracks
the objective metrics; the LLM provides the narrative ("you've stopped
dying to adds, time to learn kiting").

## The LLM query protocol

**Request (C++ → PersonalityService, POST /directive):**
```json
{
  "bot_name": "Companion",
  "class": "warrior",
  "level": 12,
  "personality_stage": "NOOB",
  "stage_progress": 0.3,
  "situation": {
    "type": "combat_start",
    "owner_in_combat": true,
    "owner_target": "Wolf (level 8)",
    "self_hp_pct": 1.0,
    "self_mana_pct": 1.0,
    "abilities_available": ["Shield Slam", "Thunder Clap", "Charge"],
    "nearest_enemies": [{"name":"Wolf","level":8,"dist":5.2}],
    "distance_to_owner": 3.1
  },
  "recent_lessons": ["don't charge into 3+ mobs"],
  "quirks": ["charges first enemy seen", "says 'easy' before trash pulls"]
}
```

**Response (PersonalityService → C++, within 5s):**
```json
{
  "intents": ["Attack", "CastSpell", "Say"],
  "targets": ["Wolf"],
  "spells": [428],
  "expression": "Easy. Watch this.",
  "risk_modifier": 0.9,
  "rotation_seq": 0
}
```

**Reflection trigger (C++ → PersonalityService, POST /reflect):**
```json
{
  "event": "combat_end",
  "encounter_summary": {
    "duration_s": 12.3,
    "dmg_dealt": 4520,
    "dmg_taken": 800,
    "spells_used": [{"entry":428,"count":4},{"entry":6438,"count":2}],
    "died": false,
    "owner_died": false
  },
  "current_lessons": ["don't charge into 3+ mobs"],
  "stage": "NOOB",
  "stage_progress": 0.3
}
```

**Reflection response:**
```json
{
  "new_lessons": ["Thunder Clap before Shield Slam for crowd control"],
  "stage_progress_delta": 0.05,
  "stage_change": null,
  "note": "Good encounter. Started using CC. Next: learn to kite the pack."
}
```

## When LLM is queried (event triggers)

The C++ layer does NOT query the LLM every tick. It queries at these moments:

| Trigger | Frequency | Purpose |
|---------|-----------|---------|
| Combat start (no current target) | Per encounter | Target selection, opening rotation, expression |
| Target dies / combat end | Per encounter | Loot decision, regroup, expression |
| Self HP below 30% | Emergency | Retreat? Heal? Defend? |
| Owner HP below 30% | Emergency | Defend? Heal? Call for help? |
| New ability learned (level up) | Rare | "What can I do now?" — adds to rotation |
| Death | Per death | Reflection: what went wrong? |
| Stage transition check | Every N encounters | Should we advance personality stage? |
| Idle for >60s in combat zone | Periodic | "What should I be doing?" — quest hint, loot check |

Between triggers, the C++ deterministic policy handles the mechanical
execution (maintaining attack, following, cooldown management).

## Fallback: deterministic policy

When the LLM is unavailable (service down, timeout, first boot before any
LLM interaction), the `DeterministicPolicy` kicks in. It uses the same
priority list from the original PORT-001 proposal:

1. Dead → recovery
2. In combat → maintain attack on current target
3. Owner in combat → assist (attack owner's target)
4. Owner being attacked → defend
5. Corpse in range → loot
6. Owner far → follow
7. Hold active → stay
8. Idle → wander

This ensures the bot is always *competent* even without personality.
The LLM layer adds *character*, not *competence*.

## File list (proposed, phased)

### Phase 1: Deterministic foundation (PORT-002..010)
| File | Status | Purpose |
|------|--------|---------|
| `src/game/PlayerBots/PlayerBotMgr.h/.cpp` | Existing | Lifecycle, persistence, party |
| `src/game/PlayerBots/PlayerBotAI.h/.cpp` | Existing | Per-tick execution, intent validation |
| `src/game/PlayerBots/CompanionObservation.h` | New | Read-only world snapshot |
| `src/game/PlayerBots/CompanionIntent.h` | New | Intent enum + directive struct |
| `src/game/PlayerBots/DeterministicPolicy.h/.cpp` | New | Fallback policy (extracted from UpdateAI) |

### Phase 2: Personality + LLM integration (post-PORT-010)
| File | Status | Purpose |
|------|--------|---------|
| `src/game/PlayerBots/BehaviorMemory.h/.cpp` | New | Persistent personality/learning state |
| `src/game/PlayerBots/LlmPolicy.h/.cpp` | New | LLM-backed policy (queries sidecar) |
| `src/game/PlayerBots/PersonalityClient.h/.cpp` | New | Thin HTTP client to localhost sidecar |
| `docker/personality-service/` | New | Python LLM sidecar (separate container or process) |
| SQL migration | New | `bot_behavior_memory` table (per-bot persistence) |

### Phase 3: Learning + progression (post Phase 2)
| File | Status | Purpose |
|------|--------|---------|
| `docker/personality-service/rotation_learner.py` | New | Analyzes combat history, suggests rotations |
| `docker/personality-service/stage_manager.py` | New | Tracks progression, recommends stage changes |
| `src/game/PlayerBots/CombatRecorder.h/.cpp` | New | Records per-encounter stats for reflection |

## Safety invariants

1. **LLM cannot execute.** It returns JSON. The C++ layer parses, validates,
   and executes. There is no code path where LLM output directly calls a
   game API.

2. **Intent validation is strict.** Every intent is checked:
   - `Attack`: target in-world, not friendly, not dead, LOS, valid
   - `CastSpell`: spell known, off cooldown, valid target, in range, mana
   - `Say`: length < 255, no GM commands
   - `Kite`/`Retreat`: valid movement target, not into wall
   - Any intent that fails validation is skipped, not logged as error

3. **Bounded memory.** `BehaviorMemory` has hard size limits:
   - combatHistory: last 50 encounters (summarized)
   - lessons: max 100 entries
   - quirks: max 20 entries
   - Total DB row: < 64KB

4. **No unbounded LLM loops.** The C++ layer rate-limits LLM queries:
   max 1 query per 5 seconds, max 10 per minute. Excess triggers are
   queued or dropped (prefer dropping expression queries over combat ones).

5. **LLM timeout is non-blocking.** The C++ layer uses an async HTTP
   request with a 5s timeout. On timeout, it continues with the current
   deterministic behavior. No thread blocking, no tick delay.

## What is explicitly NOT in scope (this wave)

- Multi-bot personality interaction (bots talking to each other)
- Player-customized personality (choosing your bot's personality)
- Voice/sound generation
- Cross-server or cross-realm memory
- LLM fine-tuning on bot combat data (future)
- Tank/healer role specialization (post-PORT-010)
- Quest-driven behavior (the LLM can *comment* on quests but not
  autonomously complete them in this wave)

## Approval required

Hosted Codex must approve:
1. The timescale separation (tick vs. event vs. reflection).
2. The intent vocabulary (any additions before Phase 2).
3. The LLM query protocol (JSON schema, endpoint, timeout).
4. The personality stage model (4 stages, progression criteria).
5. The safety invariants (especially #1 and #4).
6. The phased file list (Phase 1 files are dispatchable now; Phase 2/3
   are architecture-only until Phase 1 is accepted).

Changes to any of these require a revised architecture document.
