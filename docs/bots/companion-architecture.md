# Companion architecture: lively human-like bots in a living world

Definitive architecture document for the companion bot system. Covers the
single-companion MVP through populated-world scaling. Written for handoff:
a new developer or session should be able to pick this up and implement
without additional context.

## Vision

A WoW server where bots feel like people playing the game. Your companion
has a personality, makes mistakes, learns from experience, and expresses
itself. The world is populated with other bots who exist independently.
You can recruit any of them into your party, and they become "alive" --
gaining LLM-driven personality, learning, and expression.

The LLM is the brain. The C++ engine is the body. The LLM never touches
game APIs directly; it proposes typed intents that the C++ layer validates
and executes.

## Design principles

1. **The bot is a character, not a state machine.** It has a name, a
   personality, a memory of past encounters, and it expresses itself in
   ways a human player would.

2. **LLM shapes, engine executes.** The LLM decides what to do and how to
   feel about it. The C++ engine decides how to physically do it (pathing,
   range checks, cooldown tracking, valid target validation).

3. **Safe by construction.** The LLM can only propose typed intents from a
   fixed vocabulary. The execution layer validates every intent against the
   current world state. A hallucinated "teleport into the sun" is rejected
   before it reaches the game world.

4. **Graceful degradation.** If the LLM service is unavailable, the bot
   falls back to a deterministic policy (still competent, just less
   expressive). The bot never freezes or crashes due to LLM unavailability.

5. **Progressive competence.** The bot starts as a "reckless noob" and
   improves over time through accumulated experience. This is emergent from
   the LLM reading its own combat history and adjusting its behavior.

6. **Two-tier cost model.** World bots (population) run on cheap
   deterministic AI. Companion bots (your party) get expensive LLM-driven
   personality. The LLM only works for bots you have recruited.

## Two-tier bot system

Not all bots are equal. This is the core scaling constraint.

    +------------------------------------------------------------------+
    |                    WORLD SERVER (single process)                  |
    |                                                                  |
    |  +---------------------------+  +---------------------------+    |
    |  |   WORLD BOTS (50-200+)    |  |  COMPANION BOTS (1-5/player)|   |
    |  |                           |  |                           |    |
    |  |  - Deterministic AI only  |  |  - LLM-driven personality |    |
    |  |  - No LLM queries         |  |  - Event-driven LLM calls |    |
    |  |  - Simple state machine   |  |  - Full combat policy     |    |
    |  |  - Wander/idle/basic flee |  |  - Learning + expression  |    |
    |  |  - Cheap (minimal CPU)    |  |  - Expensive (LLM + AI)   |    |
    |  |  - No personality         |  |  - Personality stage      |    |
    |  |  - Can be recruited       |  |  - Owned by a player      |    |
    |  +---------------------------+  +---------------------------+    |
    |                                                                  |
    |  +-----------------------------------------------------------+   |
    |  |       PERSONALITY SERVICE (local sidecar, Python)         |   |
    |  |  - localhost HTTP (port 8811)                             |   |
    |  |  - Receives: situation JSON from companion bots           |   |
    |  |  - Returns: PersonalityDirective (intents + expression)   |   |
    |  |  - ONLY called for companion bots (m_isCompanion == true) |   |
    |  |  - Batch mode: N companions in one LLM call               |   |
    |  +-----------------------------------------------------------+   |
    |                                                                  |
    |  +-----------------------------------------------------------+   |
    |  |       MARIADB (tortoise-local_database)                 |   |
    |  |  - characters table (all bots, world + companion)       |   |
    |  |  - bot_behavior_memory table (companion bots only)      |   |
    |  +-----------------------------------------------------------+   |
    +------------------------------------------------------------------+

### World bots (population)

- Spawn in towns, near quest hubs, at fishing spots
- Run a simple deterministic loop: wander -> idle -> basic react
- Can be in "groups" (2-4 bots standing together)
- Have a name, class, level (visible to players)
- Do NOT have personality, LLM, or learning
- Can be recruited by any player via `.botrecruit <name>`
- When recruited, they transition to companion tier
- When dismissed, they transition back to world tier (personality paused)

World bot AI (deterministic, no LLM):

    void PlayerBotAI::WorldBotTick(uint32 diff)
    {
        switch (m_worldState) {
        case WORLD_WANDER:
            if (m_wanderTimer.Expired()) {
                PickRandomWanderTarget();
                m_wanderTimer.Reset(5000 + urand(0, 10000));
            }
            MoveToCurrentWanderTarget();
            break;
        case WORLD_IDLE:
            if (m_idleEmoteTimer.Expired()) {
                PlayRandomEmote(); // "yawn", "stretch"
                m_idleEmoteTimer.Reset(30000 + urand(0, 60000));
            }
            if (urand(0, 100) < 1) m_worldState = WORLD_WANDER;
            break;
        case WORLD_REACT:
            if (IsInCombat()) {
                if (GetHealthPct() < 0.3f) FleeToSpawn();
                else AttackCurrentVictim();
            } else {
                m_worldState = WORLD_IDLE;
            }
            break;
        }
    }

### Companion bots (your party)

- Recruited from world bots via `.botrecruit <name>`
- Get a personality assigned at recruit time (random from class pool)
- Full LLM-driven behavior: personality, learning, expression
- Persistent BehaviorMemory saved to DB
- Limited count: max 5 per player, bounded total per server
- LLM queries are event-driven (not per-tick)
- Fall back to deterministic policy if LLM unavailable

Companion bot AI (full policy, LLM or deterministic):

    void PlayerBotAI::CompanionBotTick(uint32 diff)
    {
        CompanionObservation obs = BuildObservation();
        CompanionDirective directive = m_policy->Evaluate(obs);
        ExecuteDirective(directive);
    }

### The tier transition

    void PlayerBotMgr::BotRecruit(Player* owner, string const& botName)
    {
        // ... find bot, validate, add to party ...
        PlayerBotAI* botAI = bot->GetBotAI();
        botAI->m_isCompanion = true;

        if (!botAI->HasAssignedPersonality()) {
            string personalityId = PickRandomPersonality(bot->getClass());
            botAI->AssignPersonality(personalityId);
            SaveBehaviorMemory(bot);
        }
        botAI->ResumeCompanionMode();
    }

    void PlayerBotMgr::BotDismiss(Player* owner, string const& botName)
    {
        // ... remove from party ...
        PlayerBotAI* botAI = bot->GetBotAI();
        botAI->m_isCompanion = false;
        SaveBehaviorMemory(bot);
        botAI->PauseCompanionMode();
        botAI->m_worldState = WORLD_IDLE;
    }

## Timescale separation

    TICK (100ms, C++):     movement, attack maintenance, follow, cooldowns
    SLOW (5-30s, C++):     target re-evaluation, loot decisions, group checks
    EVENT (on trigger):    LLM query for personality/strategy/expressions
    REFLECTION (post-X):   LLM reviews combat log, updates BehaviorMemory
    WORLD (5-60s, C++):    world bot state machine (wander/idle/react)

The C++ engine never blocks on an LLM call. LLM queries are async: the bot
continues its current behavior until a response arrives, then adjusts.

## Module responsibilities

    PlayerBotMgr (lifecycle + persistence + ownership + tier management)
      - Provision, login, logout, recruit, dismiss, recall
      - Party membership, owner tracking, save/restore (DB)
      - Follow/stop command routing
      - Tier transition (world <-> companion)
      - Persists BehaviorMemory alongside character data
      - World bot spawn/despawn management (Phase 3)
      - Does NOT: targeting, abilities, personality, LLM interaction

    PlayerBotAI (per-bot body, extends PlayerAI)
      - UpdateAI(diff): dispatches to WorldBotTick or CompanionBotTick
      - WorldBotTick: simple deterministic state machine (no LLM)
      - CompanionBotTick: full policy evaluation + intent execution
      - Validates intents before execution (safety layer)
      - Tracks cooldowns, ranges, movement state
      - Does NOT: decide what to do, express personality, call LLM directly

    BehaviorPolicy (decision brain for companions -- pluggable)
      - DeterministicPolicy: priority-ordered if/else (fallback, always available)
      - LlmPolicy: queries PersonalityService, maps response to typed intents
      - Selects active policy based on config + LLM availability
      - Does NOT: execute game actions, own sessions, write DB

    PersonalityService (LLM sidecar interface, local HTTP)
      - Receives: BehaviorMemory + current situation (structured JSON)
      - Returns: PersonalityDirective (typed intents + expression + risk modifiers)
      - Local only (localhost:8811), uses park_llama or equivalent
      - Timeout: 5s; on timeout/error, caller falls back to DeterministicPolicy
      - ONLY called for companion bots (m_isCompanion == true)
      - Batch mode: can process multiple companions in one LLM call
      - Does NOT: access game server directly, mutate BehaviorMemory

    BehaviorMemory (persistent per companion bot, saved to DB)
      - personalityId: string (e.g., "reckless", "steady", "competitive")
      - personalityStage: enum (NOOB, LEARNING, COMPETENT, VETERAN)
      - stageProgress: float 0.0-1.0 within current stage
      - learnedRotations: list of ability sequences per encounter type
      - combatHistory: rolling window of recent encounters (max 50)
      - lessons: list of LLM-generated behavioral notes (max 100)
      - quirks: personality traits (max 20)
      - totalEncounters / totalDeaths: uint32 counters
      - Does NOT: contain credentials, full combat logs, or unbounded data

## Dependency direction

    PlayerBotMgr  -->  PlayerBotAI          (lifecycle events, tier transitions)
    PlayerBotMgr  -->  BehaviorMemory       (load/save on login/logout/recruit/dismiss)
    PlayerBotAI   -->  BehaviorPolicy       (companion only: "what should I do?")
    BehaviorPolicy --> PersonalityService   (LLM query, async, companion only)
    BehaviorPolicy --> DeterministicPolicy  (fallback, always available)
    BehaviorPolicy --> CompanionObservation (reads current world state)
    PlayerBotAI   -->  Unit/Player/Creature (executes validated intents)
    PlayerBotAI   -->  WorldBotTick         (world bots: simple state machine)

    PersonalityService is a separate process. It does NOT include game headers.
    It communicates via localhost HTTP (JSON). The C++ side has a thin client.
    World bots NEVER call PersonalityService.

## The typed intent vocabulary

Shared language between the policy layer (LLM or deterministic) and the
execution layer (C++). The LLM must express its output as one or more of
these intents. Anything outside this vocabulary is rejected.

    // src/game/PlayerBots/CompanionIntent.h

    enum class CompanionIntent : uint8_t {
        // Movement
        Follow,            // move toward owner
        Hold,              // stop and stay in place
        Kite,              // maintain distance from current target (move away)
        Retreat,           // move away from combat (to owner or safe distance)

        // Combat
        Attack,            // attack a specific Unit* (target carried in intent)
        CastSpell,         // cast a specific spell by entry (validated)
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
        React,             // set facial expression (cosmetic)

        // Meta
        Idle,              // no action, wander gently
        Reflect,           // trigger a post-event LLM reflection (no game action)
    };

    struct CompanionDirective {
        std::vector<CompanionIntent> intents;  // ordered, first executable wins
        std::vector<Unit*> targets;            // for Attack/Defend
        std::vector<uint32_t> spellEntries;    // for CastSpell
        std::string expression;                // optional chat text (for Say)
        uint32_t emoteId = 0;                  // for Emote
        float riskModifier = 0.5f;             // 0.0 (cautious) to 1.0 (reckless)
        uint32_t rotationSeq = 0;              // which learned rotation to follow
    };

### Intent validation (C++ safety layer)

Every intent passes through validation before execution. This is the
"body checking" layer -- even if the LLM proposes something nonsensical,
it gets rejected here.

    bool PlayerBotAI::ValidateIntent(CompanionIntent intent,
                                      CompanionDirective const& dir)
    {
        switch (intent) {
        case CompanionIntent::Attack: {
            if (dir.targets.empty() || !dir.targets[0]) return false;
            Unit* target = dir.targets[0];
            if (!target->IsInWorld() || target->isDead()) return false;
            if (bot->IsFriendlyTo(target)) return false;
            if (!bot->IsWithinLOSInMap(target)) return false;
            if (!bot->IsValidAttackTarget(target)) return false;
            if (sPlayerbotAIConfig.IsPvpProhibited(
                    bot->GetZoneId(), bot->GetAreaId())) return false;
            return true;
        }
        case CompanionIntent::CastSpell: {
            if (dir.spellEntries.empty()) return false;
            uint32_t spellId = dir.spellEntries[0];
            if (!bot->HasSpell(spellId)) return false;
            if (bot->HasSpellCooldown(spellId)) return false;
            SpellInfo const* si = SpellInfoStore::LookupSpell(spellId);
            if (si && !dir.targets.empty() && dir.targets[0] &&
                !bot->IsWithinDistInMap(dir.targets[0],
                    si->GetMaxRange(false)))
                return false;
            return true;
        }
        case CompanionIntent::Say: {
            if (dir.expression.size() > 255) return false;
            if (!dir.expression.empty() && dir.expression[0] == '.') return false;
            return true;
        }
        default:
            return true;
        }
    }

    void PlayerBotAI::ExecuteDirective(CompanionDirective const& dir)
    {
        for (size_t i = 0; i < dir.intents.size(); ++i) {
            CompanionIntent intent = dir.intents[i];
            if (!ValidateIntent(intent, dir)) continue;
            if (ExecuteSingleIntent(intent, dir, i))
                return;  // first success wins
        }
        ExecuteSingleIntent(CompanionIntent::Idle, dir, 0);
    }

## Personality system

### Personality pool (per class)

Personality is assigned at recruit time: a random draw from the
class-appropriate pool. Stored in BehaviorMemory.personalityId.

    # data/bot-knowledge/personalities/warrior.yaml
    personalities:
      - id: reckless
        name: "The Reckless One"
        description: "Charges first, dies often, learns slowly"
        base_risk: 0.9
        base_expressions: ["LET'S GO!", "EASY!", "one more!", "oooops"]
        starting_lessons: []
        starting_mistakes:
          - "charges into 3+ mobs without checking"
          - "uses burst CDs on trash mobs"
          - "ignores HP until below 20%"

      - id: steady
        name: "The Steady One"
        description: "Cautious, methodical, rarely takes unnecessary risks"
        base_risk: 0.3
        base_expressions: ["Watch the adds", "Saving my CD", "Almost ready"]
        starting_lessons: ["check for adds before pulling"]
        starting_mistakes: ["too passive, misses kill opportunities"]

      - id: competitive
        name: "The Competitive One"
        description: "Obsessed with damage numbers, gets annoyed at inefficiency"
        base_risk: 0.6
        base_expressions: ["That was a bad pull", "Watch the rotation"]
        starting_lessons: ["prioritize burst windows"]
        starting_mistakes: ["gets frustrated when owner plays slowly"]

(One YAML file per class: warrior, mage, priest, rogue, etc.)

### Personality stages and progression

    NOOB (stage 0):
      riskModifier = personality.base_risk
      rotation: none (spams abilities on cooldown)
      expressions: personality.base_expressions (frequent)
      target selection: attacks nearest, no threat awareness
      mistakes: personality.starting_mistakes (happen regularly)

    LEARNING (stage 1):
      riskModifier = base_risk * 0.7
      rotation: simple 2-3 ability sequences per class
      expressions: fewer exclamation, more tactical
      target selection: focuses weakest, avoids adding
      mistakes: fewer, more situational

    COMPETENT (stage 2):
      riskModifier = base_risk * 0.4
      rotation: full class rotation with situational swaps
      expressions: calm, occasional humor, confident
      target selection: threat-aware, peels when owner is focused
      mistakes: rare, only under pressure

    VETERAN (stage 3):
      riskModifier = base_risk * 0.2
      rotation: optimized, minimal waste
      expressions: dry wit, mentor tone
      target selection: optimal, assists owner perfectly
      mistakes: essentially none (except novel encounters)

Stage progression: C++ tracks objective metrics (encounters survived,
deaths, avg DPS, reaction time, rotation violations). LLM reviews at
reflection time and recommends stage changes. C++ applies the change.

### When LLM is queried (event triggers, companion bots only)

    | Trigger                          | Frequency     | Purpose                    |
    |----------------------------------|---------------|----------------------------|
    | Combat start (no current target) | Per encounter | Target, opening, expression|
    | Target dies / combat end         | Per encounter | Loot, regroup, expression  |
    | Self HP below 30%                | Emergency     | Retreat? Heal? Defend?     |
    | Owner HP below 30%               | Emergency     | Defend? Heal?              |
    | New ability learned (level up)   | Rare          | "What can I do now?"       |
    | Death                            | Per death     | Reflection: what went wrong|
    | Stage transition check           | Every 20 enc. | Should we advance?         |
    | Idle >60s in combat zone         | Periodic      | "What should I be doing?"  |
    | Owner says something to bot      | On trigger    | Social response            |

Between triggers, the C++ deterministic policy handles mechanical
execution. The LLM is NOT called every tick.

### LLM query protocol

Request (C++ -> PersonalityService, POST /directive):

    {
      "bot_name": "Companion",
      "class": "warrior",
      "spec": "arms",
      "level": 12,
      "personality_id": "reckless",
      "personality_stage": "NOOB",
      "stage_progress": 0.3,
      "situation": {
        "type": "combat_start",
        "owner_in_combat": true,
        "owner_target": "Wolf (level 8)",
        "self_hp_pct": 1.0,
        "self_rage_pct": 0.4,
        "abilities_available": [
          {"name": "Shield Slam", "cooldown": 0, "range": 5},
          {"name": "Thunder Clap", "cooldown": 3.2, "range": 5},
          {"name": "Charge", "cooldown": 0, "range": 25}
        ],
        "nearest_enemies": [
          {"name": "Wolf", "level": 8, "dist": 5.2, "hp_pct": 1.0}
        ],
        "distance_to_owner": 3.1,
        "enemy_count": 1
      },
      "recent_lessons": ["don't charge into 3+ mobs"],
      "quirks": ["charges first enemy seen", "says 'easy' before trash pulls"]
    }

Response (PersonalityService -> C++, within 5s):

    {
      "intents": ["Attack", "Say"],
      "targets": ["Wolf"],
      "spells": [],
      "expression": "Easy. Watch this.",
      "risk_modifier": 0.9,
      "rotation_seq": 0
    }

Batch request (party of N, POST /party_directive):

    {
      "party": [
        {"bot_name": "Companion", "class": "warrior", "situation": {...}},
        {"bot_name": "HealerBot", "class": "priest", "situation": {...}}
      ]
    }

Batch response:

    {
      "directives": [
        {"bot_name": "Companion", "intents": ["Attack"], "expression": "..."},
        {"bot_name": "HealerBot", "intents": ["CastSpell"], "spells": [2060]}
      ]
    }

Reflection (POST /reflect):

    {
      "bot_name": "Companion",
      "event": "combat_end",
      "encounter_summary": {
        "duration_s": 12.3,
        "dmg_dealt": 4520,
        "dmg_taken": 800,
        "died": false,
        "rotation_violations": 1
      },
      "stage_metrics": {
        "encounters_survived": 23,
        "encounters_total": 28,
        "deaths": 5,
        "avg_dps": 365.2
      },
      "stage": "NOOB",
      "stage_progress": 0.3
    }

Reflection response:

    {
      "new_lessons": ["Thunder Clap before Shield Slam for CC"],
      "stage_progress_delta": 0.05,
      "stage_change": null,
      "note": "Good encounter. Started using CC. Next: learn kiting."
    }

## Fallback: deterministic policy

When the LLM is unavailable (service down, timeout, first boot), the
DeterministicPolicy handles everything. Same priority list for all
personality stages -- competence without personality:

    CompanionDirective DeterministicPolicy::Evaluate(CompanionObservation const& obs)
    {
        if (!obs.selfAlive) {
            if (obs.atCorpse && obs.canSelfResurrect)
                return {CompanionIntent::Resurrect};
            return {CompanionIntent::Idle};
        }
        if (obs.inCombat && obs.currentVictim && obs.currentVictimAlive
            && obs.inMeleeRange)
            return {CompanionIntent::Attack};
        if (!obs.inCombat && obs.ownerInCombat && obs.ownerTarget
            && ValidateTarget(obs.ownerTarget))
            return {CompanionIntent::Attack, obs.ownerTarget};
        if (!obs.inCombat && obs.ownerAttacker
            && ValidateTarget(obs.ownerAttacker))
            return {CompanionIntent::Defend, obs.ownerAttacker};
        if (!obs.inCombat && obs.nearestLootableCorpse && obs.inLootRange)
            return {CompanionIntent::Loot};
        if (obs.holdActive)
            return {CompanionIntent::Hold};
        if (obs.ownerAlive && obs.sameMap
            && obs.distToOwner > obs.followRange)
            return {CompanionIntent::Follow};
        return {CompanionIntent::Idle};
    }

## Safety invariants

1. **LLM cannot execute.** It returns JSON. The C++ layer parses,
   validates, and executes. No code path where LLM output directly calls
   a game API.

2. **Intent validation is strict.** Every intent is checked (see
   ValidateIntent above). Any intent that fails validation is skipped.

3. **Bounded memory.** BehaviorMemory has hard size limits:
   combatHistory: last 50 encounters (~200 bytes each). Lessons: max 100.
   Quirks: max 20. Total DB row: < 64KB.

4. **No unbounded LLM loops.** Rate-limited: max 1 query per 5s per bot,
   max 10 per minute per bot. Excess triggers dropped (expression before
   combat).

5. **LLM timeout is non-blocking.** Async HTTP with 5s timeout. On
   timeout, continue with current deterministic behavior. No thread
   blocking, no tick delay.

6. **World bots never query the LLM.** The m_isCompanion flag gates all
   personality service calls. World bots are pure C++.

7. **Companion count is bounded.** Config: MaxCompanionsPerPlayer (5),
   MaxTotalCompanions (20). Excess recruits rejected with a message.

## Configuration

    # In world server config (generated by docker/server.py)

    # Tier counts
    AiPlayerbot.MaxCompanionsPerPlayer = 5
    AiPlayerbot.MaxTotalCompanions = 20
    AiPlayerbot.MaxWorldBots = 100

    # LLM service
    AiPlayerbot.PersonalityServiceUrl = "http://localhost:8811"
    AiPlayerbot.PersonalityServiceTimeoutMs = 5000
    AiPlayerbot.PersonalityServiceEnabled = 1

    # World bot behavior
    AiPlayerbot.WorldBotWanderRadius = 10.0
    AiPlayerbot.WorldBotIdleMinMs = 20000
    AiPlayerbot.WorldBotIdleMaxMs = 60000
    AiPlayerbot.WorldBotSpawnIntervalMs = 30000

    # Companion behavior (deterministic fallback)
    AiPlayerbot.FollowDistance = 20.0
    AiPlayerbot.ReactDistance = 50.0
    AiPlayerbot.MeleeDistance = 2.0
    AiPlayerbot.LootDistance = 2.0
    AiPlayerbot.FleeHpPct = 0.3

## File list (phased)

### Phase 1: Deterministic foundation (PORT-002..010)

Proves the bot is mechanically competent. No LLM required.

    | File                                          | Status   | Purpose                          |
    |-----------------------------------------------|----------|----------------------------------|
    | src/game/PlayerBots/PlayerBotMgr.h/.cpp       | Existing | Lifecycle, persistence, party    |
    | src/game/PlayerBots/PlayerBotAI.h/.cpp        | Existing | Per-tick execution, validation   |
    | src/game/PlayerBots/CompanionObservation.h    | New      | Read-only world snapshot         |
    | src/game/PlayerBots/CompanionIntent.h         | New      | Intent enum + directive + valid  |
    | src/game/PlayerBots/DeterministicPolicy.h/.cpp| New      | Fallback priority policy         |
    | sql/migrations/add_bot_behavior_memory.sql    | New      | DB table for BehaviorMemory      |

### Phase 2: Personality + LLM integration

Adds the "alive" layer on top of the deterministic foundation.

    | File                                          | Status | Purpose                        |
    |-----------------------------------------------|--------|--------------------------------|
    | src/game/PlayerBots/BehaviorMemory.h/.cpp     | New    | Persistent personality state   |
    | src/game/PlayerBots/LlmPolicy.h/.cpp          | New    | LLM-backed policy (async)      |
    | src/game/PlayerBots/PersonalityClient.h/.cpp  | New    | HTTP client to localhost       |
    | docker/personality-service/main.py            | New    | Python LLM sidecar (FastAPI)   |
    | docker/personality-service/prompts.py         | New    | Prompt templates               |
    | docker/personality-service/models.py          | New    | Pydantic request/response      |
    | data/bot-knowledge/personalities/*.yaml       | New    | Personality pool per class     |
    | data/bot-knowledge/classes/*.yaml             | New    | Class rotation knowledge       |

### Phase 3: Populated world (world bots + recruitment)

    | File                                          | Status | Purpose                    |
    |-----------------------------------------------|--------|----------------------------|
    | src/game/PlayerBots/WorldBotSpawner.h/.cpp    | New    | Spawn/despawn world bots   |
    | src/game/PlayerBots/WorldBotAI.h/.cpp         | New    | Simple deterministic AI    |
    | data/bot-spawns/*.json                        | New    | Spawn points per zone      |
    | docker/personality-service/batch.py           | New    | Batched party directives   |

### Phase 4: Learning + progression + social

    | File                                          | Status | Purpose                    |
    |-----------------------------------------------|--------|----------------------------|
    | docker/personality-service/rotation_learner.py| New    | Combat history analysis    |
    | docker/personality-service/stage_manager.py   | New    | Stage progression          |
    | docker/personality-service/social.py          | New    | Bot-to-bot conversations   |
    | src/game/PlayerBots/CombatRecorder.h/.cpp     | New    | Per-encounter stats        |
    | sql/migrations/add_world_memory.sql           | New    | Shared world events        |

## Scaling notes

### LLM capacity planning

    | Scenario                          | LLM queries/min | Model needed      |
    |-----------------------------------|-----------------|-------------------|
    | 1 companion, casual play          | 2-5             | 7B local          |
    | 5 companions, active combat       | 10-20 (batched) | 7B local          |
    | 5 companions + 100 world bots     | Same (world=0)  | 7B local          |
    | 4 players x 5 = 20 total          | 40-80 (batched) | 7B or 13B local   |
    | Future: distilled per-archetype   | Lower latency   | 3B distilled      |

Key: world bots cost ZERO LLM capacity. They are pure C++ state machines.
Only recruited companions query the LLM.

### C++ performance

Each companion bot tick is ~O(1): build observation, check policy, execute
intent. LLM query is async and does not block the tick. World bot tick is
even cheaper (simple state machine, no observation building).

At 100 world bots + 5 companions, AI processing is negligible compared to
the world server's existing load (pathfinding, VMAP, combat).

### Database

- characters table: one row per bot (world + companion). Trivial.
- bot_behavior_memory: one row per companion bot only. Max 20 rows on a
  personal server. World bots do NOT have a behavior_memory row.

## What is NOT in scope (this wave: PORT-002..010)

- LLM integration (Phase 2)
- World bot population (Phase 3)
- Learning/progression automation (Phase 4)
- Multi-bot personality interaction
- Player-customized personality selection
- Voice/sound generation
- Cross-server memory
- LLM fine-tuning
- Tank/healer specialization beyond basic assist
- Quest-driven autonomous behavior
- Economic behavior (buying/selling, auction house)

## Handoff notes

### For a new developer or session:

1. Read this document top to bottom.
2. Read docs/bots/port-provenance.md for upstream code mapping.
3. Read docs/bots/ralph/companion-port/README.md for execution order.
4. Read docs/bots/ralph/prd.json for the card queue (PORT-002 is next).
5. Existing code: src/game/PlayerBots/ (4 files: Mgr + AI, .h/.cpp).
6. Upstream reference: local/azerothcore-playerbots-reference/ (commit
   b6696bdb, read only, do not build).
7. Build: docker compose build world
8. Test: python -m unittest discover -s docker -p 'test_*.py'
9. Server: localhost:3724 (realmd) / localhost:8085 (world)
10. In-game: .botrecruit, .botdismiss, .botrecall, .botfollow, .botstop

### What "done" looks like per phase:

- Phase 1: Bot follows, fights, loots, dies, resurrects. All deterministic.
- Phase 2: Bot talks, makes personality-appropriate mistakes, learns,
  expresses itself. LLM sidecar running. Graceful fallback.
- Phase 3: 50+ world bots in towns. Recruit any of them. They have variety.
- Phase 4: Bot measurably improved after 50 encounters. Stage advanced.
  Remembers specific lessons. References past encounters.

### Open questions for hosted head approval:

1. World bot spawn zones: starting zones + Ironforge/Org City?
2. World bot level range: zone level +/- 3?
3. Personality pool size: 3 per class minimum?
4. LLM model: park_llama 7B? (13B option for multi-player)
5. Companion cap: 5 per player, 20 total?
6. World bot names: random per-race name generator?
