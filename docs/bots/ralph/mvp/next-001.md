# NEXT-001: Configure persistent-bot identity

- Epic: KAP-543
- Jira mapping: not created
- Depends on: TW-011
- Milestone: after the first playable companion MVP
- Status: passed 2026-09-13 (accepted by park-head local-Qwen)

## Decision

Keep the first demo as one native Human male Warrior. Multi-role companions need explicit race/class/gender/appearance configuration later; random selection would make parties and persistence harder to reproduce.

## Allowed scope

- `docker/server.py`
- `src/game/PlayerBots/PlayerBotMgr.cpp`
- `src/game/PlayerBots/PlayerBotMgr.h`
- focused tests under `docker/`

## Acceptance

```gherkin
Scenario: Create a configured legal identity
Given a name and legal race, class, gender and appearance specification
When native provisioning runs
Then Player::Create receives that specification
And homebind derives from the matching playercreateinfo row
```

```gherkin
Scenario: Reject an illegal or changed identity
Given an illegal combination or a specification conflicting with an existing marker
When provisioning runs
Then provisioning fails closed without changing the existing character
```

Use a disposable, port-free project. The configuration format is an architecture decision owned by the hosted head.

The PlayerBot.Provision setting now accepts `Name` (legacy: Human male
Warrior, zeroed appearance) or
`Name,race,class,gender,skin,face,hairStyle,hairColor,facialHair`
(eight values 0..255). The identity is persisted on the
`bot_provision_state` marker by the idempotent additive migration
`sql/database_updates/character/20260913190000_character.sql`
(defaults equal the legacy identity, so existing markers stay
consistent). `Player::Create` receives the spec directly, the homebind
derives from the matching `playercreateinfo` row, and a spec that
conflicts with an existing marker is rejected before any mutation
(fail closed).

Validation: `docker compose build world` passed with
`-DALLOW_TURTLE_ADDONS=ON` (image
sha256:95ebacf6cad068e1cc0bcd6022be3d0fcc1fd408b89611817087ea18cebfdaab).
`python docker/test_bot_identity_spec.py -v` passed 4/4 in 80.525s in one
disposable, port-free project. A configured female Dwarf Hunter
(`Specbot,3,3,1,0,0,0,0,0`) was created with character, marker, homebind
and playercreateinfo values matching; a changed appearance for the same
name was rejected as "conflicts with its existing marker" with the row
unchanged; an illegal Human Shaman spec was rejected before any character
was created. No [CRASH] in any of the three world runs. Evidence is under
the gitignored `local/tortoise-bot-spec-98615ec264d5-20260913T144739Z`.
