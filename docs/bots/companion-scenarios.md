# Companion scenarios: owner-only follow/stop (TW-014, KAP-557)

Design and acceptance record for the deterministic follow/stop capability of
one owned companion. This is the last slice of the playable companion MVP
(KAP-543). No LLM service, no combat-role, and no natural-language scope is
involved.

## Contract

An owned companion (a roster bot with a valid `bot_ownership` binding) can be
followed and stopped only by its owner:

- `.botfollow <botname>` -- the companion follows the issuing player using
  normal pathfinding (a `MovePoint` goal to the owner's position, reissued
  while more than 2 yd away), preempts normal bot behavior while active, and
  holds within 2 yd of the owner once reached.
- `.botstop <botname>` -- the current follow goal is invalidated immediately:
  movement is cleared and the companion returns to normal behavior.
- Any other player issuing either command is rejected. The rejection is
  always logged (`follow/stop rejected not-owner bot:<name> issuer:<guid>
  acc:<issuer-acc> owner:<owner-acc>`); a bot without an owner binding is
  rejected as unowned.

Ownership is the `bot_ownership.owner_account_id` binding loaded with the
roster entry: the issuer's session account must equal the companion's
owner_account_id. Party membership is not required and not checked: the
acceptance story's "in the human's test party" given clause is satisfied
through the owner binding alone, because a party slot is not a security
boundary in this fork and ownership is the boundary the rest of the bot
system (provisioning, saves) already enforces. This is a deliberate,
documented deviation from the wording of the given clause.

`bot_ownership.account_id` keeps its existing meaning (contract C2): it is
the bot's own reserved session identity (>= 1e9, one session per account,
character account must equal it). A companion therefore keeps its own
reserved account and stays loggable while its owner is online; the human
owner relationship is expressed only by owner_account_id.

Both commands are `SEC_PLAYER` chat commands (`.botfollow`/`.botstop`), so
they work for any logged-in player character; the owner check is performed in
`PlayerBotMgr::BotFollow/BotStop`, not in the command security level.
`CONFIG_BOOL_PLAYER_COMMANDS` defaults to true, so no config change is
needed. `SendSysMessage` replies are safe for the socketless bot sessions the
lab uses: `WorldSession::SendPacket` null-guards the socket before any
send.

## Goal generation and stale semantics

Each accepted follow goal is stamped with a monotonically increasing sequence
number (`PlayerBotEntry::followSeq`, incremented by the manager only when a
goal is accepted; stops do not bump it). The AI holds `(leaderGuid, seq)` and:

- accepts a goal only when `seq > current`; a goal with `seq <= current` is
  stale and is rejected without side effects
  (`[PlayerBot][Follow] goal rejected stale seq:<n> current:<n> GUID:<guid>`);
- a stop clears the active goal immediately but keeps the current seq, so a
  delayed delivery of the just-stopped goal (same seq) is rejected by the
  guard instead of resuming follow.

The seq guard is what makes stop deterministic under delivery delay: the
invalidated goal can never come back from a late dispatch.

Leader lookup is same-map (`Map::GetPlayer`). If the leader is unavailable,
dead, or on another map, the goal stays active and the companion holds
position (debug log `leader unavailable` at most every 5 s) instead of
dropping or resuming it.

## Schema

New idempotent migration
`sql/database_updates/character/20260913120000_character.sql`:

```sql
ALTER TABLE `bot_ownership`
  ADD COLUMN IF NOT EXISTS `owner_account_id` INT(10) UNSIGNED NULL DEFAULT NULL
  COMMENT 'Human account allowed to command this companion (NULL = unowned)';
```

Fresh labs get it from the initdb hook (which applies the ordered
`sql/database_updates` directory); the existing personal database gets it
from mangosd's database auto-updater at world start
(`Database.AutoUpdate.Path`). NULL = unowned legacy roster bot; a human may
own several companions, so the column is not unique. `PlayerBotMgr::Load`
reads the column into `PlayerBotEntry::ownerAccountId` (0 = unowned).

## Command surface

- `src/game/Chat/Chat.cpp`: two table entries (`botfollow`, `botstop`,
  SEC_PLAYER) inserted after the `blacklist` entry of the main command
  table.
- `src/game/Chat/Chat.h`: three handler declarations (two table handlers +
  one shared body).
- `src/game/Commands/Commands.cpp`: `HandleBotFollowStopCommand` parses the
  single name token and delegates to `PlayerBotMgr`.
- `src/game/PlayerBots/PlayerBotMgr.h/.cpp`: `BotFollow`/`BotStop` with the
  always-logged acceptance/rejection audit (unknown / unowned / not-owner /
  offline), case-insensitive name lookup over the roster, the
  owner_account_id parse in `Load`, and the lab-only `PlayerBot.FollowScript`
  driver.
- `src/game/PlayerBots/PlayerBotAI.h/.cpp`: `FollowGoal`/`FollowStop` and the
  `UpdateFollow` state machine (preempts normal behavior in `UpdateAI` right
  after the alive check, before loot/quest/combat).

## Lab-only deterministic driver

`PlayerBot.FollowScript` (default empty; never set on the personal server)
arms a semicolon-separated event list, evaluated on the world thread:

- `<delayMs>:<issuerGuid>:<command text without leading dot>` -- delivered
  through the real chat path
  (`WorldSession::ProcessChatMessageAfterSecurityCheck` -> `ParseCommands` ->
  command table), exactly as a player's typed message would be;
- `<delayMs>:stale:<botName>:<leaderGuid>:<seq>` -- a synthetic delivery of
  an already-expired follow goal, called directly on the target AI so the
  seq guard can be exercised deterministically.

The clock starts when every chat-driven issuer entry is `PB_STATE_ONLINE`,
so no event is delivered into a missing session.

## Lab procedure (`docker/test_bot_follow.py`)

One disposable, port-free compose project (uuid name; teardown verifies the
project label and absence of port bindings). Three roster bots are seeded in
an emptied spawn box (natural creatures deleted in the box so the bots idle
deterministically): the owner 500110 (its own reserved account 1000500110,
no owner binding), the companion 500111 (its own reserved account
1000500111, owner_account_id = 1000500110, seeded 32 yd away) and the
intruder 500120 (account 1000500120, no owner binding). All Human Warriors,
level 10, in the Farming District. The script timeline:

| t      | issuer   | event                          | expected seq state |
|--------|----------|--------------------------------|--------------------|
| +5s    | owner    | `.botfollow Followcomp`        | goal 1 active      |
| +60s   | owner    | `.botstop Followcomp`          | goal 1 invalidated |
| +70s   | intruder | `.botfollow Followcomp`        | rejected (owner)   |
| +80s   | intruder | `.botstop Followcomp`          | rejected (owner)   |
| +90s   | owner    | `.botfollow Followcomp`        | goal 2 active      |
| +125s  | owner    | `.botstop Followcomp`          | goal 2 invalidated |
| +135s  | (stale)  | expired goal (leader, seq 2)   | rejected (stale)   |
| +145s  | owner    | `.botfollow Followcomp`        | goal 3 active      |
| +155s  | owner    | `.botstop Followcomp`          | goal 3 invalidated |

Assertions: all three seeded bots log in through the roster path (test-login
`first=1 second=0`); goal 1 accepted, active and reached with dist <= 2.0
from a 32 yd seed; DB position samples taken during the first follow window
(both bots saved on the same world save tick) show the companion within 12 yd
of the owner at least three times; each of the three stops logs accepted and
deactivates exactly once; both intruder commands are rejected with the
not-owner audit line (acc 1000500120 vs owner 1000500110); the stale seq-2 delivery after the seq-2 stop is
rejected and the seq-2 goal is activated exactly once (no resume); re-follow
after a stop reaches the owner again (goals 2 and 3); ownership rows intact
(companion still bound to the owner account); the clean stop with all three
bots online produces no SIGSEGV (regression guard for the World.cpp shutdown
fix); the personal server containers' StartedAt is unchanged.

## Result

PASSED 2026-09-13 (park-head local-Qwen). `python docker/test_bot_follow.py
-v` -> 10/10 in 264.2s in one disposable, port-free project (gitignored
evidence: `local/tortoise-bot-follow-ff2cc8744240-20260913T113637Z`).

- All three seeded bots logged in through the roster path (test-login
  `first=1 second=0` for 500110/500111/500120; login markers with the
  expected account identities).
- AC1: goal 1 accepted from the 32 yd seed
  (`follow accepted bot:Followcomp guid:500111 leader:500110 seq:1`,
  `[PlayerBot][Follow] active ... seq:1`), normal pathfinding reached the
  owner within range (`reached ... dist:1.80 seq:1`), and the owner's stop
  invalidated the goal exactly once (`stop accepted ... seq:1`,
  `[PlayerBot][Follow] inactive`). DB samples taken during the first follow
  window (same-save-tick snapshots) show the companion closing 28.42 yd ->
  6.25 yd -> 0.00 yd from the owner. Goals 2 and 3 re-followed and reached
  again (dist 1.90 / 1.92); all three stops logged accepted and deactivated
  exactly once (three `inactive` markers).
- AC2: both intruder commands were rejected with the audit line
  (`follow rejected not-owner bot:Followcomp issuer:500120 acc:1000500120
  owner:1000500110`, same for stop), and the stale seq-2 goal delivered
  after the seq-2 stop was rejected by the sequence guard
  (`[PlayerBot][Follow] goal rejected stale seq:2 current:2 GUID:500111`)
  with no second `active ... seq:2` - the invalidated goal did not resume.
- Ownership rows intact after the run: 500110 -> own session account, no
  owner; 500111 -> own session account with owner_account_id 1000500110;
  500120 -> own session account, no owner.
- Clean shutdown with all three bots online produced no SIGSEGV (regression
  guard for the World.cpp shutdown fix); personal server containers'
  StartedAt unchanged across the run.

Implementation notes: the fork's combat/movement APIs differ from stock
mangos - `Unit::CombatStop()` releases a current victim (there is no
StopAttackingOnUpdate) and `MotionMaster::empty()` reports movement (no
IsMoving); chat language is `LANG_UNIVERSAL`. A first lab run passed 9/10
with the single failure in the test's own sampling stop condition (it stopped
after three total snapshots, of which two were in-range); the test now
requires three consecutive in-range snapshots and the rerun is 10/10.

Gates: `docker compose build world` (tortoise-local:dev
sha256:62050951a377...), `python -m py_compile docker/test_bot_follow.py`,
`docker compose config --quiet`, `git diff --check` clean. Retrieval
remained operator-paused (embedding container work is post-MVP); bounded
direct-read fallback only. This completes TW-014/KAP-557 and the playable
companion MVP (KAP-543).
