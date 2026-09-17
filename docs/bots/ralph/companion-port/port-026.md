# PORT-026: Qualify Phase 2 conversation, cooperation, progression and personality

- Depends on: PORT-025
- Status: park-head cumulative battery complete 2026-09-17 (34/36 first-run; equipment fixture repaired + rerun 17/17, combat_xp flake rerun 1/1, live 27B round PASS); hosted acceptance pending; passes=false
- Tracking: KAP-543 / Phase 2 acceptance
- Execution mode: hosted acceptance with one fresh read-only QA worker
- Shared contract: [execution and review](README.md)

## Objective

Review the cumulative Phase 2 diff and qualify deterministic roles, one shared
planner request, bounded personality, conversational party chat, one cooperative
quest, automatic level-appropriate skill learning, loot-earned equipment,
bounded vendor cleanup, minimal persistence and real local-model integration.
This is an acceptance gate, not an autonomous implementation assignment.

## Allowed edit candidates

- `docs/bots/companion-phase-2-acceptance.md`
- Phase 2 handoff and queue status after hosted acceptance
- No tracked source edits in the QA worker session

## Acceptance

The declared party completes the bounded scenario with legal tank/healer/damage
behavior. The player can address the companion and receive one safe,
personality-consistent reply to a greeting and a random question without causing
a gameplay action. The player and companion complete the declared cooperative
quest with separate authoritative credit, turn-in, reward and persisted state.

The companion automatically knows every eligible class ability up to its level
without visiting or paying a trainer. It receives no free equipment refresh once
in persistent companion progression, equips one legitimately received strict
upgrade through normal APIs, retains replaced gear according to normal inventory
rules, reports authoritative bag pressure, sells only the declared protected-safe
junk set to a valid reachable vendor, and preserves equipment, inventory, money,
spells, quest state and profile across restart.

While the bot is outside the player's party it produces no new planner request.
Leaving with work outstanding rejects the old response. Reinviting the same bot
starts a new party-session generation and restores the same persisted personality,
learned skills, quest state, equipment, inventory, money and other normal
character progression. Leaving the party does not re-enable `AutoEquipForLevel`.

A current real response changes only approved preferences or expression. Offline,
slow, malformed and stale responses leave gameplay operational. Hold during an
outstanding request prevents any old response from restarting movement, offense,
quest work, vendor travel, selling or equipment changes.

## Failure cases

Missing client evidence remains pending. Failures become bounded repair cards;
QA does not edit source. Do not infer general conversational knowledge, general
autonomous questing, cross-zone travel, equipment/vendor support outside the
declared matrices, population scale, learning quality or deployment readiness.

## Validation

All deterministic gates and Phase 1/2 regressions; fake- and real-service
matrices; measured offline/delayed tick impact; conversation safety; cooperative
quest and reward persistence; automatic spell learning; loot/equipment/vendor
money and restart persistence; one fresh serialized read-only local QA review;
hosted diff review; bounded in-game evidence for chat, quest completion, earned
gearing, bag-pressure reporting and protected vendor cleanup.
