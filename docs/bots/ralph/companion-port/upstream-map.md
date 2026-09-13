# Upstream source map

Main reference: https://github.com/mod-playerbots/mod-playerbots (master).
Matching integration reference, only when needed:
https://github.com/mod-playerbots/azerothcore-wotlk (Playerbot branch).
The module README identifies that custom fork as required for installing the
module. We are using source references for Turtle adaptation, not installing it.

GitHub tree API resolved master to b6696bdbd3740e575598d167d69f39f68cc0b907 during this planning pass.
Paths below were verified in that tree. Follow, attack, stay and DPS-assist symbols
were also checked in raw source at that revision. This is a discovery pin, not
completed PORT-001 license/dependency review. Exact fragment hashes, notices,
headers, dependency traces and copy decisions remain required before dispatch.

| Stories | Pinned upstream source | Intended use |
|---|---|---|
| PORT-003 | [src/Bot/Engine/Engine.cpp](https://github.com/mod-playerbots/mod-playerbots/blob/b6696bdbd3740e575598d167d69f39f68cc0b907/src/Bot/Engine/Engine.cpp) | Action scheduling reference; adapt a minimal priority policy, not the whole engine. |
| PORT-003 / PORT-008 | [src/Ai/Base/Actions/FollowActions.cpp](https://github.com/mod-playerbots/mod-playerbots/blob/b6696bdbd3740e575598d167d69f39f68cc0b907/src/Ai/Base/Actions/FollowActions.cpp) | FollowAction::Execute; inspect formation/movement dependencies and exclude unneeded transport code. |
| PORT-004 | [src/Ai/Base/Actions/StayActions.cpp](https://github.com/mod-playerbots/mod-playerbots/blob/b6696bdbd3740e575598d167d69f39f68cc0b907/src/Ai/Base/Actions/StayActions.cpp) | StayActionBase::Stay and StayAction::Execute; Turtle cancellation generations remain local. |
| PORT-005 | [src/Ai/Base/Actions/AttackAction.cpp](https://github.com/mod-playerbots/mod-playerbots/blob/b6696bdbd3740e575598d167d69f39f68cc0b907/src/Ai/Base/Actions/AttackAction.cpp) | AttackMyTargetAction::Execute and AttackAction::Attack; translate checks to Turtle APIs. |
| PORT-005 / PORT-006 | [src/Ai/Base/Strategy/DpsAssistStrategy.cpp](https://github.com/mod-playerbots/mod-playerbots/blob/b6696bdbd3740e575598d167d69f39f68cc0b907/src/Ai/Base/Strategy/DpsAssistStrategy.cpp) | DpsAssistStrategy::InitTriggers; trace referenced target values/triggers during PORT-001 before selecting defensive rules. |
| PORT-007 | [src/Ai/Base/Actions/LootAction.cpp](https://github.com/mod-playerbots/mod-playerbots/blob/b6696bdbd3740e575598d167d69f39f68cc0b907/src/Ai/Base/Actions/LootAction.cpp) | Loot action candidate; preserve existing Turtle ownership/group-loot semantics. |
| PORT-008 | [src/Ai/Base/Actions/MovementActions.cpp](https://github.com/mod-playerbots/mod-playerbots/blob/b6696bdbd3740e575598d167d69f39f68cc0b907/src/Ai/Base/Actions/MovementActions.cpp) | Movement/reachability candidate; dependencies and relevant functions still to be bounded. |
| PORT-009 | [src/Ai/Base/Actions/ReviveFromCorpseAction.cpp](https://github.com/mod-playerbots/mod-playerbots/blob/b6696bdbd3740e575598d167d69f39f68cc0b907/src/Ai/Base/Actions/ReviveFromCorpseAction.cpp) | Recovery candidate, not permission to import automatic resurrection shortcuts. |
| PORT-009 | [src/Ai/Base/Strategy/DeadStrategy.cpp](https://github.com/mod-playerbots/mod-playerbots/blob/b6696bdbd3740e575598d167d69f39f68cc0b907/src/Ai/Base/Strategy/DeadStrategy.cpp) | Dead-state trigger candidate; final supported recovery path selected by hosted head. |
| Later POP-020 | [src/Bot/RandomPlayerbotMgr.cpp](https://github.com/mod-playerbots/mod-playerbots/blob/b6696bdbd3740e575598d167d69f39f68cc0b907/src/Bot/RandomPlayerbotMgr.cpp) | Population scheduling reference; not part of first companion port. |

PORT-002 bench persistence is a local lifecycle repair; no upstream fragment is
required. CMP-010 reviews existing Turtle party code. PORT-010 is acceptance only.
No exact defend fragment is approved yet: PORT-001 must trace attackers/target
values from the assist triggers and provide the bounded dependency closure.

## Clone for reference

Run from the Tortoise repository root. local/ is ignored.

```powershell
git clone --branch master https://github.com/mod-playerbots/mod-playerbots.git local/azerothcore-playerbots-reference
git -C local/azerothcore-playerbots-reference checkout --detach b6696bdbd3740e575598d167d69f39f68cc0b907
```

Optional matching core reference (only needed to inspect integration hooks):

```powershell
git clone --branch Playerbot --single-branch https://github.com/mod-playerbots/azerothcore-wotlk.git local/azerothcore-playerbots-core-reference
```

Record that core checkout's actual commit before using it. Do not place either
checkout in modules/ or build them into Turtle. Workers read only head-selected
files; they do not ingest the complete repository. Preserve applicable license
and attribution on copied/adapted fragments; verify compatibility against this
repository and each file, not just GitHub's repository-level license badge.

