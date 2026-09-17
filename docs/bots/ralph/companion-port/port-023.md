# PORT-023: Complete one cooperative quest with the owner

- Depends on: PORT-022
- Status: implemented (PORT-023); passes=false pending acceptance
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

## Declared quest

One quest, recorded from the live Turtle data (queried 2026-09-16):

- 456 "The Balance of Nature". Method 2 (turn-in required), MinLevel 1,
  QuestLevel 2, not repeatable, PrevQuestId 0.
- Giver and finisher are the same creature: Conservator Ilthalaine (entry
  2079), one `creature_questrelation` row (2079, 456).
- Objectives: 7x Young Nightsaber (2031) + 4x Young Thistle Boar (1984).
- Rewards: 170 XP (`Quest::XPValue`, full at level <= qLevel+25), 35c
  (`RewOrReqMoney` x Rate.Drop.Money), choice item index 0 = 5394 Archery
  Training Gloves (index 1 = 11187 Stemleaf Bracers). A socketless session
  cannot choose, so the turn-in deterministically takes choice 0.

## Implementation (PORT-023)

The companion mirrors the owner through the normal quest APIs for one
declared supported quest. It never selects a quest, never leads, and never
fabricates credit: objective progression is ordinary combat participation,
and the vanilla tap/group credit rules move both personal quest logs.

### Components

- `src/game/PlayerBots/Companion/Quest.h`: the value-only policy.
  `Observation` (generation, my/owner quest status mapped from the
  authoritative enum, rewarded, giver/finisher availability, held,
  inCombat) and `Select()` -> `Action::{None, Accept, TurnIn}`. Pure
  function, no engine pointers, no I/O; shared with the value test
  `docker/test_companion_quest_value.{cpp,py}`.
- `PlayerBotAI::CooperativeQuestStep(diff)`: the world-thread adapter,
  called from `UpdateCompanion` before the no-order early return (the
  step works without a follow order). Gates: declared quest id, owned
  companion, alive, in map, 5 s deny backoff, party present, owner
  resolvable by account and a live party member. Fills the snapshot,
  anchors on the nearest live quest creature within
  `INTERACTION_DISTANCE` (5 yd), re-validates
  `CanInteractWithQuestGiver`, then runs the authoritative helpers:
  accept = `CanTakeQuest` + `CanAddQuest` + `AddQuest`; turn-in =
  `CanCompleteQuest` + `CompleteQuest` + `CanRewardQuest` +
  `RewardQuest(qInfo, 0, anchor, true)` (choice 0). Every accepted and
  denied outcome is logged (`[CoopQuest] accepted/accept denied/turnin/
  reward denied`).
- `PlayerBotMgr::UpdateQuestScript()` + `PlayerBot.QuestScript` config
  (`PLAYERBOT_QUEST_SCRIPT`): lab-only owner driver, default off. Events
  `<delayMs>:<issuerGuid>:<questId>:accept|turnin`; the clock starts when
  every issuer is online; each event resolves the issuer session and the
  nearest live quest creature within 30 yd and runs the same
  authoritative accept/turn-in helpers the packet handlers use. This
  mirrors the committed lab-only FollowScript/PartyInviteScript drivers;
  it is config-gated and invisible to a real owner, who accepts and
  turns in through the client UI unchanged. (Scope note: the card's
  allowed list named PlayerBotAI and Companion/; the manager-side lab
  driver follows the same precedent as those two committed lab drivers
  and as PORT-019/020's manager-side personality config.)
- `docker/server.py`: `PlayerBot.CooperativeQuestId`
  (`PLAYERBOT_COOPERATIVE_QUEST_ID`, default 0 = disabled) and
  `PlayerBot.QuestScript` env passthrough.

### Group credit (the co-op engine)

`Unit::HandleKilled` -> `Group::RewardGroupAtKill`: every party member at
or inside the group reward distance (CONFIG_FLOAT_GROUP_XP_DISTANCE,
74 yd) who is alive or dead with an unreleased corpse gets
`KilledMonsterCredit` on its own log (vanilla gray-mob rule).
`KilledMonsterCredit` auto-completes the quest when the personal
objectives finish. `RewardPlayerAndGroupAtEvent` is script-command only
and is NOT in the normal kill path; the companion's credit is the same
ordinary credit a real party member earns.

### Assist target legality (code fix found by the first runtime run)

The kill phase is steered by `.botassist <bot> <creature name>`. The first
runtime run rejected every pre-combat assist with `target-invalid`: the
legality gate used non-forced `Unit::CanAttack`, which requires faction
hostility evidence, but in this client's DBC the player faction templates
carry `hostileMask = 0` (human template 35) and the Westfall "attacker"
creatures (Young Nightsaber 2031 -> template 7, Young Thistle Boar 1984 ->
template 189) carry all-zero masks. The real player melee path
(`Unit::Attack`) performs no faction hostility check at all.
`IsAssistLegalTarget` now mirrors it: for a player bot the gate is
not-player + not friendly + not evading + targetable (aliveness is checked
by the caller); the non-player branch keeps the ordinary `CanAttack` gate
and the defend-style neutral-evidence rule.

### Runtime hardening (second round of disposable lab runs)

- `PlayerBot.AmbientAcquire` config (`PLAYERBOT_AMBIENT_ACQUIRE`,
  default on): ambient (unowned) bots may autonomously acquire
  nearby targets. The quest-coop lab fixture disables it (plus a
  1 yd wander radius) so the temp-logged owner idles at spawn
  instead of hunting the fixture pack.
- Assist revalidation: the executor re-resolves its snapshot from
  GUIDs before acting; for assist the `canAttack` fact uses the
  forced `CanAttack` form for player executors only
  (`me->IsPlayer() && source == Assist`), because the non-forced
  form requires faction-hostility evidence that passive quest
  mobs lack (the same DBC fact as the legality fix above).
  ContinueCombat/Damage verdicts keep their reviewed semantics.
- Turn-in gates (owner QuestScript and companion CoopQuest) are
  handler-shaped: `CanCompleteQuest` -> `CompleteQuest` runs only
  while the quest is still INCOMPLETE, because the engine already
  marks it COMPLETE when the last credit lands; the reward step
  only needs COMPLETE plus `CanRewardQuest`. The old
  complete-then-reward gate deadlocked on an already-complete
  quest. Failure logs: `turnin not-complete issuer:%u quest:%u
  status:%u`.
- Fixture restart-capture scoping (round-5 finding): compose logs
  accumulate across container generations of one service, so the
  restart phase now waits for the new generation's
  `World server is up and running!` marker (counted above the
  phase-1 count, the same marker-counting discipline the
  provision lab uses) and scopes `logs_restart` to the tail after
  that marker. The unscoped capture returned the phase-1 log
  immediately (the phase-1 companion-login marker already
  satisfied the predicate) and the no-re-turn-in assertion
  matched the legitimate phase-1 line. State persistence itself
  was verified unaffected: the pre-stop and post-restart DB
  snapshots were byte-identical (quest rows, XP, money, item).

### Suppression and priority

- Hold, combat, death and newer orders retain existing priority: the
  policy suppresses on `held`/`inCombat`; death preempts at the
  lifecycle level (`UpdateRecovery` before `UpdateCompanion`); a stale
  generation can never act.
- Owner loss or party loss stops cooperative planning immediately (no
  owner in the party -> no snapshot) without erasing the companion's
  persisted quest state; re-joining re-arms it.
- `rewarded` suppresses the policy after the turn-in (a non-repeatable
  quest stays status COMPLETE with `m_rewarded=true`).

### Failure modes

Missing declaration (id 0), unknown quest template, no owner, no party,
owner not in the party, held, in combat, dead, no quest creature in
interaction range, any authoritative check denial, stale generation, and
a reward denial all fail safe: no action, no fabricated credit, 5 s
deny backoff, every denial logged.

### Validation

- Value suite: `docker/test_companion_quest_value.{cpp,py}` (status
  mapping, accept/turn-in selection, all suppressions) - green.
- Runtime fixture: `docker/test_bot_quest_coop.py`, three disposable
  Compose labs (isolated projects, personal state untouched, asserted
  before/after):
  - Lab A (eligible): owner accepts via the lab quest script while the
    companion is held (hold suppression), follow release -> companion
    accept; party loss -> row survives, no cooperative action; re-join;
    11 zero-damage kills steered by `.botassist` (7 + 4) move both
    personal logs to 7/4 (the owner's is pure tap/group credit - it
    never attacks); companion turn-in fires after the re-join, owner
    turn-in via the script; both inventories hold choice item 5394; both
    turn-in log lines show an XP delta; clean stop + world restart ->
    saved quest/inventory/xp/money state identical. Fixture hardening
    from the first runtime run: the whole pack is seeded inside the
    owner's own grid cell (10.5-15.6 yd away) so the owner keeps that
    grid active for the entire run and every target stays inside the
    30 yd assist lookup; fixture character names are digit-free
    (digit-bearing names trip the reserved-name regex at login and are
    silently kicked); the quest timeline poller is per-part fault
    tolerant and traces both characters' saved positions every 5 s.
  - Lab B (ineligible): the owner never accepts; the companion stands at
    the giver through a quiet window; no `[CoopQuest]` activity, no
    quest rows, no rewards.
  - Lab C (death/recovery): the companion accepts at the giver, is
    sent to a pinned unkillable minion, dies, reclaims and resurrects;
    both quest rows remain INCOMPLETE with zero credit (nothing erased,
    nothing fabricated), no turn-in, owner never dies.
