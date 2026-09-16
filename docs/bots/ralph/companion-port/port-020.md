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
## Implementation (2026-09-16)

### Persistence row (ordered updater migration)

`sql/database_updates/character/20260916120000_character.sql`
creates `tw_char.bot_personality(char_guid PK, schema_version,
profile_id, assigned_at)` - idempotent (`CREATE TABLE IF NOT
EXISTS`), no prompts, no raw chat text, no learning history.
`test_bot_provision.boot_lab` applies it for every lab world, so
persona fixtures always see the schema.

### Roster and login wiring (PlayerBotMgr)

- The roster load LEFT-joins `bot_personality` per companion.
  `Companion::Personality::AcceptPersisted` (current schema,
  declared profile id) fills
  `PlayerBotEntry::personalitySchemaVersion/personalityProfile`;
  a missing row stays (0,0); a rejected row logs
  `Playerbot: personality row for %u rejected (schema=%u
  profile=%u); deterministic baseline` and runs the baseline.
- `OnBotLogin` calls `SyncPersonality` (owned companions only):
  a missing row is seeded once from the declared
  `PlayerBot.PersonalityProfile` config via `INSERT ... ON
  DUPLICATE KEY UPDATE char_guid = char_guid` - a no-op write
  that can never rewrite an existing or rejected row; the declared none profile writes no row at all (missing stays the documented baseline), and a failed write leaves the baseline in place without failing the login. Debug log:
  `[Personality] assigned GUID:%u profile:%s (config seed)`.
  A failed write never fails the login.

### Precedence

`PlayerBotAI::ApplyPlannerPreference` reads the profile from
`botEntry->personalityProfile` (the persisted identity, seeded
from config at first login). The config value never overrides a
persisted row; unknown schema/profile data fails closed to the
deterministic baseline and is never overwritten.

### Value and fixture evidence

- `test_companion_personality_value.cpp` (`.py` runner): eight
  `AcceptPersisted` checks (current row accepted; schema drift
  and unknown profile ids fail closed) added to the PORT-019
  suite.
- `test_bot_companion_personality_persist.py`: one disposable
  project, one database, three world generations. Gen 1 (config
  reckless, empty table): first login seeds exactly one row
  (schema 1, reckless) and the fake service chase proposal
  applies the reckless distance. Gen 2 (config changed to
  cautious, same DB): the re-invite keeps the persisted reckless
  profile - the config never overrides a persisted identity, the
  row and character state are unchanged, and exactly one seed
  log line exists across both logins. Gen 3 (row tampered to
  schema 99): the load fails closed - the transport still
  plans, no personality effect appears, and the tampered row is
  never rewritten.
