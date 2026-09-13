# PORT-001: Upstream provenance and reuse audit

## Repository pin

| Field | Value |
|-------|-------|
| URL | https://github.com/mod-playerbots/mod-playerbots |
| Branch | master |
| Commit | b6696bdbd3740e575598d167d69f39f68cc0b907 |
| Local checkout | `local/azerothcore-playerbots-reference/` |
| License | GNU GPL v2 (or later) — see LICENSE and per-file headers |
| Date pinned | 2026-09-13 |

Integration reference (core hooks only, not ported):
https://github.com/mod-playerbots/azerothcore-wotlk (Playerbot branch). Not cloned;
not required for behavior porting.

## License and compatibility

The mod-playerbots module is GPL v2 or later. Each source file carries the header:

```
This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file
for Copyright information; released under GNU GPL v2 license, redistribute/modify
under version 2 of the License, or (at your option) any later version.
```

**Decision:** We do not copy source code into this repository. We reimplement
behavior using the upstream as a design reference. This avoids GPL contamination
of the Turtle codebase (which has its own license terms). Where we adapt a
specific algorithm or decision rule, we cite the upstream file and commit in a
comment. No source lines are reproduced.

If any future card requires direct code copying (e.g., a complex pathfinding
heuristic), that fragment must be:
1. Clearly marked with upstream attribution and license notice.
2. Isolated in a separate translation unit.
3. Reviewed for GPL compatibility with the surrounding code before merge.

## Provenance matrix

Each row maps a PORT story to the upstream source it draws on. "Reuse type"
distinguishes design reference (behavior only) from potential code adaptation.

| PORT | Upstream file (relative) | Key symbols | Reuse type | Notes |
|------|--------------------------|-------------|------------|-------|
| PORT-003 | `src/Ai/Base/Actions/FollowActions.cpp` | `FollowAction::Execute`, `FollowAction::isUseful`, `FollowAction::CanDeadFollow` | Design reference | Transport/boarding logic is AzerothCore-specific; the distance-threshold follow pattern and dead-follow guard are portable concepts. |
| PORT-003 | `src/Ai/Base/Actions/StayActions.cpp` | `StayActionBase::Stay`, `StayAction::Execute`, `StayAction::isUseful` | Design reference | Stay = clear last movement, stop moving, clear chase/follow states. Simple; reimplement natively. |
| PORT-004 | `src/Ai/Base/Actions/StayActions.cpp` | Same as above | Design reference | Hold/cancel = StayAction with a generation counter. Upstream uses strategy add/remove; we use a seq counter. |
| PORT-005 | `src/Ai/Base/Actions/AttackAction.cpp` | `AttackAction::Execute`, `AttackMyTargetAction::Execute`, `AttackAction::Attack` | Design reference | Validation sequence (in-world, not-flying, PvP-prohibited, not-friendly, not-dead, LOS, valid-target) is portable as a checklist. The actual attack call maps to Turtle's `Unit::Attack`. |
| PORT-005 | `src/Ai/Base/Strategy/DpsAssistStrategy.cpp` | `DpsAssistStrategy::InitTriggers` | Design reference | Single trigger: "not dps target active" → NextAction("dps assist", 50.0). The assist decision is: if the bot has no current target AND the master has a hostile target, attack the master's target. |
| PORT-006 | `src/Ai/Base/Strategy/DpsAssistStrategy.cpp` | Same as above + `AttackAction::Attack` | Design reference | Defend = assist trigger fires when the master (or a party member) is the victim. The "dps target active" value checks if the bot already has a valid current target in combat. |
| PORT-007 | `src/Ai/Base/Actions/LootAction.cpp` | `LootAction::Execute` (17.6 KB file) | Design reference | Upstream loots all corpses in range. We restrict to party-owned loot only (Turtle group-loot semantics). Read for decision structure only. |
| PORT-008 | `src/Ai/Base/Actions/MovementActions.cpp` | Movement/reachability actions (105 KB file) | Design reference | Too large to audit fully. The relevant pattern: "if target unreachable after N seconds, fall back to follow/stay." We implement a simpler timeout. |
| PORT-009 | `src/Ai/Base/Strategy/DeadStrategy.cpp` | `DeadStrategy::InitTriggers` | Design reference | Trigger priority: self-resurrect > find-corpse > revive-from-corpse > accept-resurrect > repop. For Turtle: auto-resurrect on login (already done) + optional "walk to corpse and self-resurrect" if within range. |
| PORT-009 | `src/Ai/Base/Actions/ReviveFromCorpseAction.cpp` | `ReviveFromCorpseAction::Execute` | Design reference | Casts revive spell at corpse. Turtle equivalent: `player->ResurrectPlayer(factionMod, applyPenalties)` at corpse position. |

## Dependencies and Turtle API mapping

| Upstream concept | Turtle equivalent | Notes |
|-----------------|-------------------|-------|
| `PlayerbotAI` (per-bot context) | `PlayerBotAI` (extends `PlayerAI`) | Already exists; holds per-bot state. |
| `Engine::ExecuteAction(name)` | Direct method calls on `PlayerBotAI` or `PlayerBotMgr` | We do not need a string-keyed action registry for one companion. |
| `Strategy` / `TriggerNode` tree | Priority-ordered if/else in `PlayerBotAI::UpdateAI` | One companion = one policy; no need for a generic strategy engine. |
| `AI_VALUE(Unit*, "current target")` | `bot->GetVictim()` / `bot->GetSelection()` | Native AzerothCore/Turtle APIs. |
| `AI_VALUE(Unit*, "group leader")` | `bot->GetGroup() ? bot->GetGroup()->GetLeader() : nullptr` | Standard. |
| `botAI->GetMaster()` | Owner reference stored in `PlayerBotMgr` per-bot entry | Already tracked via party membership. |
| `ServerFacade::instance().GetDistance2d(a, b)` | `a->GetExactDist2d(b)` or `a->GetDistance2d(b)` | Direct. |
| `botAI->ChangeEngine(BOT_STATE_COMBAT)` | Implicit: entering combat via `Unit::Attack` transitions state | No explicit state machine needed. |
| `Formation*` / `Formation::GetMaxDistance()` | `sPlayerbotAIConfig.followDistance` (config) | Already configured. |
| `LastMovement` / `MovementPriority` | Not needed for single-companion scope | Multi-bot formation tracking is out of scope. |
| `Transport` / boarding logic | Out of scope for companion port | Bots do not need to board boats. |

## Blocked fragments

| Fragment | Reason | Alternative |
|----------|--------|-------------|
| `MovementActions.cpp` (105 KB) | Too large; contains dungeon pathing, BG-specific logic, vehicle handling | Implement a simple "unreachable timeout" (PORT-008) without the full movement action set. |
| `LootAction.cpp` (17.6 KB) | Depends on `LootObjectStack`, `LootRollAction`, group-roll timers | Use Turtle's existing `Corpse::Loot` and `Player::Loot` APIs with a simple "loot if in range and party-owned" check. |
| Transport/boarding code in `FollowActions.cpp` | AzerothCore `Transport` API; not present in Turtle | Skip. Companion follows on foot; if owner boards a transport, bot waits (PORT-008 unreachable timeout). |

## Summary

No source code is copied. All upstream references are design-level: decision
sequences, validation checklists, and trigger priorities. The Turtle
implementation uses native APIs and a flat priority policy in `PlayerBotAI::UpdateAI`
rather than a generic strategy/action engine. This keeps the companion
self-contained within the existing two-file `PlayerBotMgr` + `PlayerBotAI`
structure without introducing a framework.

## Upstream code as body-knowledge for the LLM brain

The mod-playerbots source is not just a design reference for our
deterministic fallback. It is the **complete mechanical knowledge base**
that our C++ "body" needs, and the **class skill data** that feeds the
LLM "brain."

### 1. Mechanical execution (C++ body)

The upstream code contains validated implementations of every physical
action the bot can perform. We do not copy these, but we use them as the
specification for our intent validation layer in `PlayerBotAI`:

| Intent | Upstream validation sequence (specification) |
|--------|----------------------------------------------|
| `Attack` | `AttackAction::Attack`: in-world, not flying, PvP-prohibited zone check, not friendly, not dead, LOS, valid attack target, not already attacking same target |
| `CastSpell` | `CastSpellAction::isPossible`: has spell, off cooldown, in range, valid target, not in vehicle (if ground spell) |
| `Kite` | `MovementActions`: maintain distance by moving away while maintaining threat; use `MovePoint` with pathing |
| `Follow` | `FollowAction::Execute`: distance check vs. `followDistance`, dead-follow guard, transport handling (skip) |
| `Hold` | `StayActionBase::Stay`: clear last movement, stop moving, clear chase/follow unit states |
| `Loot` | `LootAction`: in range, corpse not looted, party ownership check |
| `Resurrect` | `ReviveFromCorpseAction`: at corpse, can cast, mana available |

These validation sequences become the **safety layer** in our C++ engine.
Every intent the LLM proposes passes through these checks before execution.

### 2. Class skill knowledge (LLM context)

The per-class strategy files define complete combat rotations with
priority-ordered triggers. This is the "skill knowledge" the LLM uses to
make informed decisions. We extract this into a structured format for the
personality service:

**Source: `src/Ai/Class/Warrior/ArmsWarriorStrategy.cpp`**

Default rotation (fallback when no trigger fires):
```
bladestorm > mortal strike > sunder armor > melee
```

Conditional triggers (priority order, higher fires first):
```
ACTION_EMERGENCY:
  critical health        -> intimidating shout
  medium health          -> enraged regeneration
  almost full health     -> retaliation (2+ melee attackers)

ACTION_INTERRUPT:
  victory rush           -> victory rush (kill proc)
  shattering throw       -> shattering throw (break DS/IB, 30yd)

ACTION_HIGH + 10:
  enemy out of melee     -> charge
  battle stance needed   -> switch to battle stance

ACTION_HIGH + 9:
  battle shout available -> battle shout (buff)

ACTION_HIGH + 8:
  rend available         -> rend (DoT)
  rend on attacker       -> rend on attacker (defensive DoT)

ACTION_HIGH + 5:
  target critical health -> execute
  sudden death proc      -> execute

ACTION_HIGH + 4:
  overpower / TfB proc   -> overpower (bonus strike)

ACTION_HIGH + 3:
  mortal strike ready    -> mortal strike

ACTION_HIGH + 2:
  bloodrage ready        -> bloodrage (burst CD)
  death wish ready       -> death wish (burst CD)

ACTION_HIGH + 1:
  high rage available    -> slam (rage > ~50)

ACTION_HIGH:
  hamstring available    -> piercing howl (AoE CC)

ACTION_DEFAULT:
  (default rotation above)
```

**Source: `src/Ai/Class/Warrior/WarriorTriggers.cpp`** (trigger conditions)

Each trigger has a condition function that checks game state. Examples:
- "mortal strike": spell known, off cooldown, target alive, in melee range
- "high rage available": `bot->GetRageValue() > threshold`
- "target critical health": `target->HealthPct() < 20`
- "critical health": `bot->HealthPct() < 30`
- "almost full health": `bot->HealthPct() > 80` AND 2+ melee attackers
- "shattering throw trigger": enemy within 25yd has DS/IB/BoP aura

### 3. How this feeds the LLM

The personality service receives a **class knowledge block** as part of
the system prompt. For a level 12 Arms Warrior companion:

```
You are playing an Arms Warrior in World of Warcraft (Turtle WoW 1.18.1).

Your rotation priority (highest to lowest):
1. EMERGENCY: If your HP is below 30%, use Intimidating Shout. If below 50%, use Enraged Regeneration.
2. INTERRUPT: If Victory Rush is off cooldown, cast it. If an enemy has Divine Shield/Ice Block within 30yd, use Shattering Throw.
3. If target is out of melee range, Charge.
4. Maintain Battle Shout buff. Apply Rend if off cooldown.
5. If target is below 20% HP, use Execute.
6. If Overpower/Taste for Blood procced, use Overpower.
7. Use Mortal Strike when off cooldown.
8. If Bloodrage or Death Wish is ready, use it for burst.
9. Spend excess rage (>50) on Slam or Heroic Strike.
10. Default: Bladestorm > Mortal Strike > Sunder Armor > Melee auto-attack.

Your personality stage: NOOB (reckless)
- You charge into combat without thinking
- You use burst CDs (Bloodrage, Death Wish) too eagerly
- You ignore defensive triggers until it's too late
- You say enthusiastic things: "LET'S GO!", "EASY!", "one more!"
- You sometimes pull multiple mobs and get overwhelmed
- You are learning: start paying attention to HP, save defensives for emergencies
```

As the bot progresses to LEARNING stage, the personality service modifies
the prompt:
```
Your personality stage: LEARNING
- You now kite when HP is below 50% instead of fighting through
- You save Bloodrage for when you have Mortal Strike up
- You use Intimidating Shout proactively at 40% instead of 30%
- You check for multiple enemies before charging
- You are calmer: "stun first", "backing up", "got this"
```

### 4. Config parameters (physical constraints)

From `PlayerbotAIConfig.h`, these are the physical constants our C++
engine uses (not LLM-decided):

| Parameter | Default | Purpose |
|-----------|---------|---------|
| `followDistance` | 20.0f | Max distance before follow triggers |
| `reactDistance` | 50.0f | Distance at which bot reacts to enemies |
| `reactDelay` | 1500ms | Delay between AI ticks |
| `meleeDistance` | 2.0f | Melee attack range |
| `spellDistance` | 30.0f | Max spell range |
| `sightDistance` | 50.0f | Detection range |
| `fleeDistance` | 40.0f | Distance to flee when low HP |
| `tooCloseDistance` | 5.0f | Minimum comfortable distance |
| `aoeRadius` | 8.0f | AoE ability radius |
| `maxWaitForMove` | 30000ms | Max wait for pathing |
| `sitDelay` | 10000ms | Delay before sitting when idle |
| `lootDistance` | 2.0f | Distance to loot a corpse |

These become constants in our C++ engine. The LLM does not override them;
it works within them.

### 5. What we do NOT use from upstream

| Upstream feature | Reason to exclude |
|-----------------|-------------------|
| `Transport` / boarding logic | Complex, AzerothCore-specific, not needed for companion |
| `Formation` system | Multi-bot formation; we have one companion |
| `RandomPlayerbotMgr` | Population management; out of scope |
| `Dungeon` / `Raid` / `World` strategy trees | Instance-specific content; companion starts in open world |
| `BisListMgr` / `RandomItemMgr` | Gear scoring; not relevant to companion behavior |
| `TravelMgr` | Travel routing; companion follows owner, doesn't travel independently |
| `PlayerbotDungeonRepository` | Dungeon-specific data; future scope |
| Full `MovementActions.cpp` (105KB) | Contains BG/dungeon/vehicle logic; we extract only kiting/fleeing patterns |

### 6. Extraction plan for class knowledge

To feed the LLM, we extract the per-class strategies into a structured
JSON/YAML format at build time (or as static data files):

```yaml
# data/bot-knowledge/warrior-arms.yaml
class: warrior
spec: arms
level_range: [1, 60]
rotation:
  default: [bladestorm, mortal_strike, sunder_armor, melee]
  triggers:
    - name: critical_health
      priority: EMERGENCY
      condition: "self_hp_pct < 0.30"
      action: cast(intimidating_shout)
    - name: victory_rush
      priority: INTERRUPT
      condition: "spell_off_cooldown(victory_rush)"
      action: cast(victory_rush)
    # ... (full trigger list)
personality_modifiers:
  NOOB:
    risk: 0.9
    rotation_mod: "uses burst CDs without checking rotation priority"
    expressions: ["LET'S GO!", "EASY!", "one more!", "oooops"]
    mistakes: ["charges into 3+ mobs", "wastes Bloodrage on trash", "ignores HP until 10%"]
  LEARNING:
    risk: 0.6
    rotation_mod: "starts saving burst for priority targets"
    expressions: ["stun first", "backing up", "got this"]
    lessons: ["check for adds before pulling", "save Intimidating Shout for real danger"]
  # ... (COMPETENT, VETERAN)
```

This file lives in the personality service (Python sidecar), not in the
C++ game server. The C++ server only knows the intent vocabulary and
validation rules. The LLM reads the class knowledge + personality stage
and produces intents.

This keeps the game server lean and the "brain" (LLM + knowledge) in a
separate, updatable process. New classes or specs can be added by dropping
a new YAML file into the personality service without rebuilding the server.
