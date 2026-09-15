# PORT-020: Persist minimal versioned personality state

- Depends on: PORT-019
- Status: pending hosted dispatch; passes=false
- Tracking: KAP-543 / Phase 2 personality
- Shared contract: [execution and review](README.md)

## Objective

Persist only the minimal Phase 2 state needed for stable personality identity:
schema version, assigned profile ID and explicitly approved bounded preference
state. Preserve character progression and existing companion lifecycle. Learning,
combat-history retention, lessons and social memory remain Phase 4.

## Allowed edit candidates

- Versioned character-database migration in the ordered updater directory
- Bounded persistence types under `src/game/PlayerBots/Companion/`
- Narrow `PlayerBotMgr` load/save/recruit/dismiss hooks
- Disposable persistence/rollback fixture under `docker/`

## Acceptance

An assigned supported profile survives normal logout, world restart, party
leave/rejoin and isolated backup/restore without changing character state or
deterministic behavior. Reinviting the same bot restores the same profile rather
than assigning a new personality. Learned spells and other normal character
progression remain persisted by the authoritative character systems and are not
copied into or deleted with personality state. Missing rows use a documented
default. Unknown schema/profile data fails closed to the deterministic baseline
and remains diagnosable.

## Failure cases

Do not edit an applied migration, store prompts/raw chat/raw logs, create
unbounded collections, or couple character saving to service availability. A
failed personality save must not fail normal character persistence.

## Validation

Back up before any live migration; use only a disposable Compose project for
schema and restore tests. Verify idempotent migration, exact bounded rows,
restart/party leave/rejoin behavior, unknown-version fallback, and unchanged
character/inventory/quest/spell persistence.
