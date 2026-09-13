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
