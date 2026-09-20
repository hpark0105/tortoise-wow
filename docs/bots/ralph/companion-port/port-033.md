# PORT-033: Provision a bounded owned companion party

- Status: implemented; cohort lab 17/17 (port034-cohort11) + personal deploy live (2026-09-19/20); awaiting operator review
- Phase: 3, bounded owned-party scale before ambient population scale
- Depends on: Phase 2 quest turn-in and mirror-scope repairs, PORT-026 acceptance
- Scope: one human owner and up to four owned companions in a normal five-member party

## Goal

Let the operator declare companions by name, class and race in startup configuration. The immediate target is the existing companion plus one priest, one rogue and one mage. Reapplying the same declaration must find the same characters, preserve their progress and never create duplicate accounts, characters or roster bindings. The declaration provisions and binds identities; normal `.botrecall` and `.botrecruit` remain the actions that bring them online and into the party.

Proposed operator input:

```dotenv
PLAYERBOT_COMPANION_OWNER_ACCOUNT_ID=<existing human account ID>
PLAYERBOT_COMPANION_SPECS=Healer:priest:human;Scout:rogue:human;Arcanist:mage:human
```

The names above are examples only. Keep the existing `PLAYERBOT_PROVISION` path as a compatibility alias for a single unowned identity; do not silently change an existing character's owner, class, race or appearance. Resolve class and race names through explicit allowlists and validate each race/class pairing with `GetPlayerInfo`. Reject duplicate names, unknown labels, invalid owner account, and a declaration exceeding the available four companion slots before creating any new identity. Do not print account details in logs.

## Implementation seams

1. Parse the bounded list in `docker/server.py` and `PlayerBotMgr::LoadConfig`. Keep parsing separate from database writes. Add a pure parser/validator for `Name:class:race` records. Cap the list at four total owned companions for this owner, including an already bound existing companion; reject over-capacity rather than truncate.
2. Extend `ProvisionPersistentBot` to accept a validated identity and owner account. Reuse its native character creation, idempotent provision marker and roster publication. Persist `bot_ownership.owner_account_id` with the new binding. For an existing identity, verify all immutable identity and ownership fields; mismatches fail closed. Preserve the old single-name provision behavior.
3. Reconcile declared identities at startup one at a time with bounded error reporting. A failure must leave a resumable marker for that identity and must not corrupt previously provisioned companions. On the next startup, complete or safely reject each identity without creating a second character.
4. Give each following companion a stable party slot offset instead of the current common right-side point. Compute it from the party's ordered companion identities, with pathfinding and the existing leash rules retained. A membership change recomputes slots without teleporting or overwriting an active hold/assist order.
5. Keep one shared planner request per party. The protocol already supports four bot slots; verify collection, response routing, stale-session rejection and fallback with all four occupied. Do not increase the planner or normal-party cap in this port.

## Acceptance

- A disposable project provisions three named class/race identities alongside the existing owned companion. Database rows have unique GUIDs and bot accounts, the intended owner binding, valid native state and the requested classes. Restarting with the same declaration leaves identity, spells, equipment, inventory, money, quest progress and personality unchanged.
- An invalid declaration and a conflicting existing name fail before any unintended character is published. A staged failure of one identity resumes safely on restart without duplicating the successful identities.
- The owner recalls and recruits four companions into a normal party. A fifth companion recruit is rejected as full; an unauthorized player cannot recall, recruit or command them. Dismissed companions stay out of ambient population reconciliation.
- In a disposable world fixture, priest heals, rogue and mage use legal learned abilities, and all four follow without converging on one point. Hold, assist, defend, death/recovery and owner relog remain independent per companion.
- One planner round includes exactly four bot slots and offers reach the matching bot only. Offline, slow and malformed model responses leave deterministic party play operational. Measure world tick time, pathfinding load and model request rate against the one-companion baseline before live rollout.
- After a real restart, the same four companions can be recalled and rejoin without duplicated sessions or lost character state. Live gameplay claims require an in-game party check, not just fixture logs.

## Safety and rollout

Use a separate Compose project and synthetic characters for creation, failure and restore tests. Do not run provisioning experiments against the personal character database. Back up the personal database before any eventual live migration or first multi-companion rollout; check for an online player before maintenance. Keep character/account data and raw logs out of tracked evidence. This port does not authorize deployment, raid expansion, ambient bot population growth or autonomous quest selection.

## Personal deployment notes (2026-09-19/20)

The implemented shape is the `PLAYERBOT_PROVISION` semicolon list with short
`Name,race,class,gender` specs (PORT-034 provision-list change), not the
`PLAYERBOT_COMPANION_SPECS` sketch above. Live cohort on the personal server:

- Bram - Human Warrior (race 1, class 1), guid 2, reserved account 1000000000.
  Renamed from "Companion" by migration 20260919090000_character so the
  provision finds the existing character and is an idempotent no-op.
- Rowan - Human Hunter (race 1, class 3), guid 5, account 1000000001.
- Elowen - High Elf Mage (race 10, class 8), guid 6, account 1000000002.
- Clem - Dwarf Priest (race 3, class 5), guid 7, account 1000000003.

All four are owned by account 4 (bot_ownership) and start at level 8.

Faction: this fork's ChrRaces.dbc carries ten races and
`Player::TeamForRace` (Player.cpp:7761) derives the team from
`baseLanguage` (LANG_COMMON 7 = ALLIANCE, LANG_ORCISH 1 = HORDE). In this
fork High Elf (10) is Alliance and race 3 is Dwarf, so the whole cohort is
Alliance - matching Candra (Human Warlock). Note the fork race table differs
from stock TBC numbering (race 9 Goblin, race 10 High Elf), which is why
identities must be read from data/dbc/ChrRaces.dbc, not from expansion
assumptions.

Level 8 was applied as a personal DB change (characters.level = 8, xp = 0)
while the bots were offline. The engine re-derives everything at login:
`InitStatsForLevel` fills stats from player_levelstats /
player_classlevelstats by (race, class, level), `UpdateSkillsForLevel`
reconciles skill ranks, and the companion's `AutoLearnSpellsForLevel`
re-learns the full legal spell set for the level (no persisted
character_spell rows existed for the new bots).

Post-review update (2026-09-20, KAP-558): the ownership binding above
was applied manually at the time; provisioning now binds the
configured owner itself (`PLAYERBOT_OWNER_ACCOUNT_ID` ->
`PlayerBotMgr::PublishBotOwnership`), so future cohorts need no manual
SQL, and a pre-existing binding is never overwritten. The personal
.env carries `PLAYERBOT_OWNER_ACCOUNT_ID=4`.

## Review assignment

A fresh read-only `park-agent` assignment may review the parser, ownership and resume contract, party formation, planner four-slot behavior and fixture design from bounded verified source cards. Hosted Codex owns implementation choices, diff review, deterministic gates and final acceptance.
