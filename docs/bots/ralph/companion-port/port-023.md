# PORT-023: Complete one cooperative quest with the owner

- Depends on: PORT-022
- Status: pending hosted dispatch; passes=false
- Tracking: KAP-543 / Phase 2 cooperative questing
- Shared contract: [execution and review](README.md)

## Objective

Implement one owner-driven cooperative quest vertical slice for one declared
Turtle quest. The player and companion use their own normal quest logs, accept
through the authoritative quest path, perform the bounded objectives together
and turn in independently. This is not general autonomous quest selection.

## Allowed edit candidates

- One quest observation/policy under `src/game/PlayerBots/Companion/`
- Narrow reuse/extraction of the existing authoritative quest helpers
- `PlayerBotAI.cpp/.h` registration and dispatch only as required
- One disposable player-plus-companion quest fixture under `docker/`
- `src/game/CMakeLists.txt` when compiled sources are added

## Acceptance

The owning card records the exact quest, prerequisites, giver/finisher, supported
objective types and group-credit rules from current Turtle data. When both
characters are eligible, the owner starts the supported cooperative goal and the
companion accepts normally, receives only authoritative personal/group credit,
reaches completion and turns in through the normal reward path. Quest state,
earned rewards and inventory survive logout and world restart.

Hold, combat safety, death/recovery, owner loss and newer orders retain their
existing priority. Leaving the party stops cooperative planning without erasing
the companion's persisted quest state.

## Failure cases

Ineligible or mismatched quest state, a full quest log, missing prerequisite,
unsupported objective type, unavailable giver/finisher, full inventory, stale
generation or party loss fails safely. Do not fabricate objective credit, share
quest items illegally, teleport, invent a route, auto-select another quest or
alter the player's quest rows.

## Validation

Compile; disposable eligible/ineligible pair, normal acceptance, supported kill
and/or loot objective credit, Hold and party-loss interruption, death/recovery,
turn-in/reward and restart persistence. Record separate player and companion
quest/inventory state before and after; add client evidence for the same quest.
