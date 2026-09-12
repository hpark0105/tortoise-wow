# NEXT-001: Configure persistent-bot identity

- Epic: KAP-543
- Jira mapping: not created
- Depends on: TW-011
- Milestone: after the first playable companion MVP

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
