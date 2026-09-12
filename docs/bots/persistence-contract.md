# Persistent Bot Ownership and Save Contract (TW-004 / KAP-547)

Status: source-backed design, head-reviewed. No schema or save-path changes in this story.
Follow-on: TW-005 (ownership metadata migrations), TW-006 (login validation), TW-007 (opt-in save).
Precedes and is ported atomically with any save change (engine decision D4, docs/bots/engine-decision.md).

Evidence base: worktree 8a7f25a (branch personal-server), verified 2026-09-11 by direct read
(retrieval index was mid-sync at writing time; bounded direct-read fallback per repo policy).

## 1. Definitions

- Persistent bot: roster member of the `playerbot` table (tw_char database). Intended lifetime
  spans server restarts. Must be stably bound to one (account, character) pair and, once TW-007
  lands, may save state.
- Ephemeral bot: any bot session that is not a verified roster member. Includes temporary
  chat bots (`m_tempBots`) and ad-hoc bots created via `PlayerBotMgr::AddBot(ai)`. Never saved;
  lifetime is the process.

## 2. Current behavior (source evidence)

| Behavior | Location |
|---|---|
| Roster schema `playerbot(char_guid PK, chance, comment, ai)` - no account column | sql/create_databases.sql:1972 |
| Bot account ids are synthetic in-memory values: `_maxAccountId = MAX(account.id) + 10000`, then `GenBotAccountId()` per roster row at load | src/game/PlayerBots/PlayerBotMgr.cpp:75, 88 |
| Roster load assigns a fresh synthetic account to each row on every restart; the binding is never persisted | src/game/PlayerBots/PlayerBotMgr.cpp:84-99 |
| Non-roster bots load the character's real account (`GetPlayerAccountIdByGUID`) | src/game/PlayerBots/PlayerBotMgr.cpp:299 |
| Bot session is created directly, no auth handshake; dummy anticheat session; `session->SetBot(entry)` | src/game/PlayerBots/PlayerBotMgr.cpp:324-328; src/game/WorldSession.h:541-542 |
| Save blanket block: `SaveToDB` returns false for any bot session ("Pas de sauvegarde des bots"); second guard `m_DbSaveDisabled` (faction change) | src/game/Objects/Player.cpp:18217-18219, 18220 |
| Load account-match check is bypassed for every bot session | src/game/Objects/Player.cpp:16604-16608 |
| Bot logout: entry state OFFLINE triggers `LogoutPlayer(true)` and session removal; destructor also logs out a residual player | src/game/WorldSession.cpp:377-382, 121-123 |
| Account online flag is cleared only in the socket-close branch; bot sessions never have a socket and bot accounts have no `account` row; `GetSecurity` returns SEC_PLAYER for unknown ids | src/game/WorldSession.cpp:384-396; src/game/AccountMgr.cpp:250-255 |
| LOADING entry with no resolvable session waits indefinitely (no timeout) | src/game/PlayerBots/PlayerBotMgr.cpp (Update, connection loop) |
| Temp-bot expiry erases the roster entry and flips state to OFFLINE; the surviving session is then dropped by the WorldSession bot-offline branch | src/game/PlayerBots/PlayerBotMgr.cpp (Update temp loop); src/game/WorldSession.cpp:377-382 |

Consequences this contract must close:

1. Bot account binding is non-deterministic across restarts (regenerated at load; stable only
   incidentally because roster rows are iterated in `char_guid` map order).
2. The load bypass at Player.cpp:16604 admits any bot session to any character, including
   characters owned by real human accounts.
3. Temp/chat bots run inside a real human account session with the save path still blocked by
   the blanket guard; if the guard ever becomes per-bot, unverified sessions must stay blocked.
4. No timeout exists for a bot stuck in PB_STATE_LOADING.

## 3. Invariants

C1 Ownership binding. A persistent bot is bound to exactly one (account_id, char_guid) pair,
persisted in the database. The binding is created at provisioning and is immutable except by an
explicit, versioned, head-reviewed migration. Bot account ids are allocated from the reserved
synthetic range (above MAX(account.id)) and persisted with the binding; they are never reused
across restarts and never collide with real accounts.

C2 Identity agreement at login. Before a character is attached to a session, the session account
must equal the character owner account. Human sessions load only their own characters (existing
check, Player.cpp:16604). The bot bypass must be narrowed to verified sessions only: a bot
session may load a character only when (a) the session is bound to the character's persisted
bot account, or (b) the session is an ephemeral session that constructed itself from the
character's real owner account (PlayerBotMgr.cpp:299), which is consistent by construction.
Any other combination rejects the load: error log, no attach, no reassignment, no deletion.

C3 Human characters are never reassigned. No code path may update `characters.account` (or any
equivalent ownership column) for a character whose owner is a real, non-bot account. Provisioning
a persistent bot onto an existing character owned by a non-bot account fails closed and is
logged; the character is never adopted, renamed, or deleted.

C4 Save policy. Ephemeral bots never reach the `SaveToDB` write path; the current blanket block
(Player.cpp:18217) remains the default. Persistent bots may save only when the session is a
verified roster member whose C2 binding passed. The guard becomes per-bot (verified persistent:
allow; everything else: block), implemented in TW-007. On persistent-bot logout and on clean
shutdown, exactly one final save is performed and is idempotent.

C5 Session teardown. Session teardown has one owner: the WorldSession bot-offline branch
(WorldSession.cpp:377-382) and the destructor (WorldSession.cpp:121-123). Bot accounts have no
`account` row; no code may create, update, or clear online state for synthetic account ids.

C6 Interrupted provisioning. Provisioning (create character + insert roster row + persist binding)
is transactional or idempotent. After a crash mid-provisioning the database may contain at most:
an orphan character owned by a reserved bot account (never saved, safe to delete) and no roster
row pointing at a character that is not owned by its bound bot account. Roster validation at load
(TW-006) must detect and quarantine: (a) roster row with no matching character (log, skip bot),
(b) roster row whose character owner is a non-bot account (log, quarantine row, C3 applies).

C7 Bounded and duplicate-safe callbacks. A bot in PB_STATE_LOADING whose session does not appear
must time out after a bounded wait (candidate value: 2x confBotsRefresh), be returned to
PB_STATE_OFFLINE with an error log, and be retryable. A second `AddBot` for a character guid
with a live session is rejected. Temp-bot expiry semantics (entry erased, state flipped, session
dropped by C5's single owner) are preserved.

C8 Ephemeral no-save preserved. Until C4's per-bot guard ships, every bot session is unsaved,
which is the required ephemeral behavior and must remain true end to end (AC1).

## 4. Identity failure matrix (AC2)

| Session account | Character owner | Outcome |
|---|---|---|
| Human | Same human account | Normal load (existing path) |
| Human | Bot-reserved account | Reject load (existing check, Player.cpp:16604); never reassign |
| Verified bot (C1 binding holds) | Same bot account | Load |
| Verified bot | Human account | Reject load; error log; no reassignment |
| Unverified bot | Any account | Reject load (post TW-006); error log |
| Any session | Owner mismatch at save | Reject save; error log (C4) |
| Roster row | Character owned by human account | Quarantine row at load; C3; no UPDATE characters |

## 5a. Ownership metadata (TW-005)

- Table `bot_ownership` (tw_char): `char_guid` (PK), `account_id` (UNIQUE),
  `bot_type` (1 = persistent roster bot), `provision_version`
  (1 = migration-seeded, 2 = runtime-provisioned), `provisioned_at`.
- Reserved synthetic bot account range: ids >= 1000000000. A roster character receives
  a binding only when its current `characters.account` already sits in the reserved
  range; the migration never touches characters owned by real (below-range) accounts (C3).
- Migration: sql/database_updates/character/20260911174500_character.sql. Idempotent
  (CREATE TABLE IF NOT EXISTS + guarded INSERT); the auto-updater records the file hash
  in the per-database `migrations` table and skips already-applied files on rerun.
- Read-only validation query (report only, never writes) with three issue classes:
  `unowned_character` (metadata row without a matching character),
  `character_owned_by_non_bot_account` (bound character owned by a below-range account),
  `roster_without_binding` (roster character lacking metadata). See
  docker/test_bot_ownership_migration.py for the executable form.
- Runtime provisioning (TW-010) allocates a fresh reserved-range account, sets
  `characters.account`, and inserts the `bot_ownership` row with provision_version 2,
  all inside one transaction (C6).

## 5. Story mapping

- TW-005: versioned migrations adding the ownership metadata (persisted bot account binding per
  roster row) with C6 idempotency; no save-path change.
- TW-006: C2/C6 login validation, quarantine logs, LOADING timeout (C7), duplicate-login reject.
- TW-007: C4 per-bot save guard for verified persistent bots; final-save idempotency (C4/C5).
- Candidate module port (engine decision D2/D4): the candidate's synthetic-session save posture
  (candidate Player.cpp:16831) must comply with C2-C5 before any candidate code lands.

## 6. Validation

- Every file:line citation above verified against worktree 8a7f25a on 2026-09-11.
- No source, schema, or migration changes were made by this story.
- TW-005 migration validated in a disposable MariaDB (separate port-free Compose project,
  real repository base schema, fixtures including an existing human character): idempotent rerun,
  mismatch reporting, human records byte-identical; 14/14 docker unit tests pass.
- Prior-story regression gate re-verified before close (TW-001 marker audit 56/1 known-bad;
  TW-002 ADR intact; TW-003 telemetry evidence present).
## Hosted decision: native character initialization before TW-011 (2026-09-11)

Storage-engine correction: the bundled `characters` and `playerbot` tables are
MyISAM; `bot_ownership` is InnoDB. A shared SQL transaction does not make writes
across these tables atomic. A hosted injected binding failure left an unbound
character, as permitted by C6's resumability alternative. Earlier descriptions
of complete rollback/atomic provisioning must not be read as guarantees. Native
provisioning must explicitly handle partial writes and cache publication, or
propose a separately validated storage-engine migration. Do not silently convert
the personal database as part of bot provisioning.

The current minimal level-10 SQL insert is a disposable fixture mechanism. It is
not accepted as the normal companion provisioning path. Implement native
`Player::Create` initialization and persist its results before earned-state
restart testing. Preserve the stable synthetic identity policy in C1/C5 for
this slice; introducing real login accounts would be a separate contract change.
Native character initialization does not require opening bot network logins.

Evidence checked in current source: `Player.cpp`, `Player::Create` initializes
race/class start location, configured starting level/money, stats, taxi nodes,
talents, reputation, spells, and starting inventory. `CharacterHandler.cpp`,
`HandleCharCreateOpcode` uses Create, SaveToDB, and updates the character cache.
The current provisioning insert bypasses these operations. Loading a character
repairs some defaults, but is not evidence of equivalent creation.

Implementation requirements for the next bounded slice:

- Reuse native character initialization with a dedicated provisioning context;
  keep ephemeral bot saves blocked. Do not remove the global save guard to make
  provisioning work. Review SaveToDB transaction ownership before nesting it.
- Persist native inventory, spell/action state as applicable, homebind, and
  character state together with a verified ownership/roster outcome. Cache
  insertion must happen only after successful persistence. If native saving
  cannot join one transaction, explicitly design and test resumable phases.
- Allocate GUIDs consistently with the core allocator and reserve stable bot
  accounts; never copy or adopt personal characters. Fail closed on conflicts.
- Start at the native configured level and money. Any accelerated level-10
  training preset must be explicit, use native initialization, and have its own
  validation. Existing fixture bots must not be silently upgraded in place.
- Compare a disposable native baseline and provisioned bot of the same
  race/class/configuration. Check initial inventory/equipment, spells, action
  initialization, location/homebind, health/powers and persisted identity.

```gherkin
Feature: Native initialization of persistent companions
  Scenario: Fresh creation matches native starting state
    Given an empty disposable bot roster and a native creation baseline
    When one persistent Human Warrior is provisioned
    Then its starting state matches the baseline for the same configuration
    And exactly one stable ownership binding and roster entry exist
    And no personal character or login account is modified

  Scenario: Persistence failure cannot publish a playable bot
    Given a database failure during native character persistence
    When provisioning runs
    Then it reports failure without publishing a ready roster or cache entry
    And retry completes one identity without duplicate inventory or bindings

  Scenario: Ephemeral creation remains unsaved
    Given an ephemeral custom AI bot
    When it is created and logged out
    Then its gameplay state is not persisted
```

Implementation evidence: [native persistent-bot initialization](native-initialization-2026-09-12.md).
The gate is implemented for fresh provisioning and disposable recovery tests;
it is not an earned-gameplay persistence claim. After this slice: R6 asynchronous
telemetry, R5 save-boundary failure coverage, R4 stale-completion coverage, then
TW-011 earned-state restart/restore. Custom-AI login remains a direct runtime
coverage requirement alongside these gates.
