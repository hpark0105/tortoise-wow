# Phase 2 handoff: personality and model integration

Current status (2026-09-17, park-head local session): Phase 2 is implemented on feature/kap-558-port-phase2 through f96d4b2 (PORT-011..025). The PORT-026 cumulative battery is complete: 34/36 first-run, both failures repaired or isolated and rerun green, live 27B planner round PASS. Hosted acceptance is pending; passes=false. See the "Phase 2 cumulative battery" section below.

Status: **planned as PORT-011..026; not implemented or accepted by this handoff**.
Planning baseline: `796bc67` on `feature/kap-558-port-companion-port`
(2026-09-15). Phase 1 runtime acceptance was reported by the operator. The
Phase 1.1 hardening sequence (items 1-5) is committed through `796bc67` and was
independently reviewed with fresh assist 11/11 and defend 11/11 disposable-lab
runs. Tracked PORT-010 acceptance metadata still needs reconciliation before
dispatch; this handoff does not change Jira or `prd.json` status.

## Entry gate and goal

Enter only after Phase 1 acceptance is recorded and the PORT-011 deterministic
hardening card is selected. Add one supported tank, healer and damage policy,
expressive companion personality, and bounded shared-party planning without
making model availability a requirement for correct gameplay.

Phase 2 does not include population scale, general autonomous quest selection,
general cross-zone travel, autonomous learning, reflection, broad social memory
or model-directed game execution. It includes bounded conversational party chat,
one cooperative quest vertical slice and deterministic equipment progression
from loot the companion legitimately receives. Eligible class abilities continue
to be learned automatically up to the companion's level; no class-trainer trip or
training purchase is required.

## Module boundary and code entry points

The current Phase 1 seam is `src/game/PlayerBots/Companion/Policy.h` plus
`PlayerBotAI.cpp`. Before transport, split it into bounded immutable observation,
typed directive, deterministic policy and authoritative executor responsibilities.
Keep lifecycle/session/persistence in `PlayerBotMgr`; keep transport,
prompt/personality data, parsing and deterministic behavior separate.

The model is a non-authoritative preference source. C++ owns owner orders,
priority, target legality, learned-spell legality, range/LOS, resources,
movement, combat, loot, recovery and final execution. Role policies are
deterministic and remain operational when the service is disabled or unavailable.

The model proposes schema-validated intents or preferences, never commands, SQL,
spell IDs invented from Wrath, or executable code. Use Turtle-learned abilities.
Async envelopes need identity, session/order generation, request ID, observation age
and a resolvable target GUID; never retain a raw Unit pointer across asynchronous work.
The executor rejects stale, unauthorized and unsupported responses by default.

The Phase 1.1 selector deliberately excludes abilities carrying
`SPELL_ATTR_ON_NEXT_SWING_1/2` because this pinned core can leave a successful
queued melee spell in `CURRENT_MELEE_SPELL` and suppress later white damage.
That exclusion is a safe baseline, not implicit Phase 2 support. PORT-012 must
make an explicit, tested decision: repair or contain the one-shot melee-slot
lifecycle, or represent this ability category as unsupported. PORT-014..016 may
not advertise or depend on an excluded ability.

Recovery remains a lifecycle preemption above behavior selection. Transport never
blocks the world thread and uses one bounded request per party event, not one call
per companion or tick.

Party membership is the companion/LLM gate. Any bot that accepts a player's party
invitation may enter the planner path; proximity, ownership metadata and the
invitation mechanism alone do not enable it. Leaving or being removed from the
group, or disbanding the group, immediately stops new requests and invalidates
outstanding responses. Rejoining creates a fresh party-session generation while
restoring the bot's persisted personality. Normal character progression, including
already learned skills, remains attached to the bot and is never erased by leaving
the party.

## Canonical card sequence

| Card | Outcome | Roll-up relationship |
| --- | --- | --- |
| [PORT-011](port-011.md) | Preserve landed targeting/fallback; close cast-result telemetry gaps | Phase 2 entry hardening |
| [PORT-012](port-012.md) | Extract reviewed combat/loot state; resolve capability gaps | New foundation |
| [PORT-013](port-013.md) | Extract reviewed typed directives; add pure-policy coverage | New foundation |
| [PORT-014](port-014.md) | One declared tank threat policy | Implements CMP-012 |
| [PORT-015](port-015.md) | One declared healer triage policy | Implements CMP-013 |
| [PORT-016](port-016.md) | One declared damage/pull-discipline policy | Implements CMP-014 |
| [PORT-017](port-017.md) | Versioned schema and deterministic fake service | LLM-010 prerequisite |
| [PORT-018](port-018.md) | Nonblocking shared party-planner transport | Implements LLM-010 |
| [PORT-019](port-019.md) | Bounded personality preferences and expression | New personality slice |
| [PORT-020](port-020.md) | Minimal versioned personality persistence | Learning remains Phase 4 |
| [PORT-021](port-021.md) | Real local-model adapter through the accepted transport | Completes model integration |
| [PORT-022](port-022.md) | Bounded conversational party chat | Text-only, no gameplay authority |
| [PORT-023](port-023.md) | One cooperative quest with the owner | Normal quest APIs and credit |
| [PORT-024](port-024.md) | Persistent equipment progression from earned loot | No free companion gear refresh |
| [PORT-025](port-025.md) | Bag-pressure reporting and authoritative vendor cleanup | Normal sell/money APIs |
| [PORT-026](port-026.md) | Cumulative conversation, cooperation and progression acceptance | Hosted acceptance |

Cards run in order except PORT-014 and PORT-015 may be independently analyzed
after PORT-013; only one editing worker may operate in the worktree. PORT-016
depends on both role foundations. The existing CMP/LLM cards are roll-ups, not
duplicate dispatch units.

## Decisions fixed by this plan

- Use GUIDs and bounded scalar/value fields across asynchronous boundaries; the
  older raw-`Unit*` directive example is superseded.
- Use `PlayerBotEntry::loginGeneration` and the current owner-order generation
  in every planner envelope, plus request ID, observation version/age and expiry.
- Build and accept a deterministic fake service before real local-model calls.
- Use one shared party request and capped queue/in-flight state.
- Gate planner participation on accepted player-party membership. Leaving the group
  invalidates the old party-session generation; a later invitation restores the
  same persisted personality without accepting responses from the former session.
- Permit only allowlisted preferences and cosmetic expression initially. The
  model does not supply spell IDs, target names, coordinates or commands.
- Persist only profile identity and explicitly approved bounded preference state
  in Phase 2. Learning history, lessons and reflection belong to Phase 4.
- Treat model failure as no preference; it must not pause deterministic play.
- Represent `NoEligibleAbility`, `CastAttempted` plus its authoritative result,
  and `OrdinaryAttackFallback` as distinct bounded outcomes. A diagnostic such
  as `spell:0` plus `SPELL_FAILED_UNKNOWN` is not a cast rejection and must not
  cross the policy/planner boundary as one.
- Resolve on-next-swing abilities explicitly in PORT-012. Until their queued
  melee-slot lifecycle is proven, they remain unsupported deterministic actions
  even when learned; role stories must not imply otherwise.
- Continue calling the authoritative `AutoLearnSpellsForLevel` path for persistent
  companions on login and level-up. Automatic ability learning is independent of
  the optional model, costs no character money and requires no trainer goal.
- Separate skill learning from equipment progression. Ambient world bots may keep
  `AutoEquipForLevel`; a persistent companion must not receive free replacement
  gear on login, level-up, party leave or rejoin. It improves equipment only from
  items it legitimately receives through the accepted loot policy and normal
  inventory/equipment APIs.
- Conversational output is a text-only, rate-limited channel. It cannot become a
  gameplay directive, and raw chat history is not persisted by default.
- Give the model a small versioned Turtle/WoW companion primer for supported
  concepts such as bags, vendors, equipment, quests, parties and death. Static
  context explains rules, not live NPC identity, coordinates or routes. C++
  supplies bounded authoritative observations and resolves every actual vendor,
  item, path and transaction.

## Operator-local model candidates

The operator workstation already has three smaller Q8 GGUF candidates under
`C:\Users\hpark\WebstormProjects\park-llama\models` for Phase 2 evaluation:

- `qwen3-4b-instruct-2507-q8_0.gguf` (3.99 GiB)
- `Qwen3.8-4B-Q8_0.gguf` (4.29 GiB)
- `Ministral-3-8B-Instruct-2512-Q8_0.gguf` (8.41 GiB)

Their published SHA-256 digests were verified after download. These are
operator-local assets, not repository artifacts, deployment requirements or a
preselected production model. PORT-021 benchmarks the candidates through the
accepted adapter and records schema adherence, latency, context needs and VRAM
before one is approved. The world server does not load model files directly,
and the one-shared-model concurrency rule remains in force.

## Exit scenarios

```gherkin
Feature: Optional personality with authoritative deterministic execution
  Scenario: Model unavailable
    Given an accepted deterministic companion
    When the model times out or returns malformed output
    Then deterministic gameplay remains available
    And no unsafe action is executed

  Scenario: An old response arrives after hold
    Given a planning request is outstanding
    When the owner orders hold and the old response arrives
    Then the response cannot restart movement or offense

  Scenario: A companion leaves and later rejoins the party
    Given a bot has a persisted personality and learned character skills
    And a planning request is outstanding
    When the bot leaves the party
    Then new model requests stop and the outstanding response is rejected
    When the player later invites the same bot and it joins the party
    Then a new party-session generation begins
    And the same personality and learned skills are retained

  Scenario: Personality remains bounded
    Given a personality preference conflicts with an owner order or game rule
    When an intent is selected
    Then the owner order and game rule take precedence

  Scenario: The player chats with a companion
    Given the companion is in the player's current party
    When the player addresses it through the supported chat channel
    Then one bounded personality-consistent text response may be returned
    And the response cannot directly move, cast, equip, loot or alter a quest

  Scenario: The party completes the supported cooperative quest
    Given the player and companion are eligible for the declared quest
    When they accept and perform its objectives together
    Then each character receives only normal authoritative objective credit
    And the companion turns in and persists its own quest state normally

  Scenario: A companion earns an equipment upgrade
    Given a persistent companion legitimately receives a usable loot item
    When the deterministic equipment evaluator proves it is an upgrade
    Then the item is equipped through normal inventory APIs
    And leaving or rejoining the party cannot replace it with free generated gear

  Scenario: A companion runs out of bag space
    Given a persistent companion has reached the declared free-slot threshold
    When more loot cannot be stored safely
    Then it reports the bag pressure once at a bounded rate
    And the deterministic policy protects equipped, quest and upgrade items
    And it may sell only approved junk to an authoritative reachable vendor
    And normal money and inventory state persist after the sale
```

## Open decisions resolved by the owning card

- PORT-014..016 select exact supported classes, levels, spells, thresholds and
  fixture geometry from current Turtle source/data.
- PORT-012 owns the on-next-swing capability classification/lifecycle decision,
  bounded cast-outcome vocabulary, and value-level capability-filter tests.
  PORT-013 preserves the already-landed first-class Defend priority while adding
  pure priority/generation coverage around the extracted value contract.
- PORT-017 measures and fixes payload, latency, timeout, queue, rate and text
  limits before transport implementation.
- PORT-017 documents localhost/container connectivity and authentication; no
  deployment topology is assumed here.
- PORT-019 chooses the smallest allowlisted expression channels and profile
  vocabulary. Chat and retrieved text remain untrusted data.
- PORT-020 selects the ordered migration name and fail-closed schema-version
  behavior after inspecting the current character updater.
- PORT-022 fixes the supported player-input channel, addressing rules, response
  length/rate limits and whether any short-lived conversational context exists.
- PORT-023 selects one current Turtle quest, eligibility matrix, objective types,
  group-credit behavior and owner-driven start/stop contract. It does not create
  general autonomous quest selection or cross-zone travel.
- PORT-024 fixes one loot-distribution policy before implementation, then defines
  supported classes/slots and an explicit deterministic upgrade comparison. The
  model never supplies item IDs or directly equips an item.
- PORT-025 fixes the protected-item and sellable-item classes, free-slot threshold,
  notification cadence, supported vendor search radius and travel bound. A default
  context primer may explain what vendors do, but current Turtle data chooses the
  actual NPC and normal APIs perform every sale.

Do not implement an arbitrary expression evaluator for triggers. New observations
require explicit supported C++ fields. Personality mistakes require an approved
gameplay policy and test, not unconstrained random failure.

Phase 1.1 review evidence (2026-09-15): `python test_bot_companion_assist.py`
11/11 in 224.5 s; `python test_bot_companion_defend.py` 11/11 in 173.6 s;
Python compilation, Compose configuration and diff checks passed. Retrieval sync
was unavailable due the operator-known embedding-service failure, and a fresh
read-only local-worker review was blocked by the shared-model mutex. No Phase 2
build, service integration, runtime or client acceptance is claimed here.

## Phase 2 cumulative battery (2026-09-17, park-head local-Qwen session)

- Date 2026-09-17; branch feature/kap-558-port-phase2; recent baseline on
  the branch: 192624c (PORT-024) .. f96d4b2 (PORT-025, HEAD). Uncommitted
  tracked work at session start: none. This session changed
  docker/test_bot_equipment.py (fixture repair) plus
  docs/bots/companion-phase-2-acceptance.md, this handoff, the port-026
  status line and progress.txt; all committed together as the session-stop
  commit.
- Image under test: tortoise-local:dev
  sha256:7f02bf278719a6b9a1727c84c3be2540b4b3e232ae201dcb82cec1b50a9e2e16
  (built 2026-09-17 from the committed tree).
- Commands and results: full 36-module battery via local/port026-battery.ps1
  (after local/port026-battery-warm.ps1 pre-warm) -> 34/36 PASS
  (local/port026-summary.txt); test_bot_equipment rerun -> 17/17 OK in
  385.1s (local/port026-equipment-rerun1.log); test_bot_combat_xp isolated
  rerun -> 1/1 OK in 118.5s (local/port026-combat_xp-rerun1.log); Phase F
  live round -> 1/1 OK in 89.9s (local/port026-real_planner_live.log);
  python -m py_compile docker/server.py OK; docker compose config --quiet
  OK; git diff --check clean.
- Changed path and reason: docker/test_bot_equipment.py. Lab B filler item
  117 (Tough Jerky) is a quality-1 consumable, i.e. the PORT-025
  declared-sellable junk class, so the new vendor cleanup sold the filler
  and cleared the bag pressure the test asserts on (fixture staleness, not
  a product regression). Filler is now Earthroot 2449 (class 7 TRADE_GOODS,
  protected by the Inventory.h Classify junk matrix); fixture charge seeds
  corrected from 4 to 5 tokens to match Item::LoadFromDB
  (MAX_ITEM_PROTO_SPELLS == 5). Invariant restored: Lab B must keep bag
  pressure through the loot step so the pressure marker is observable.
- Acceptance scenarios: passed - all deterministic Phase 2 scenarios per
  docs/bots/companion-phase-2-acceptance.md (roles, chat safety, cooperative
  quest, equipment/vendor/pressure/restart, party-session generation and
  stale rejection, offline/timeout/malformed/stale/hold, live round).
  Failed on first run - test_bot_equipment (fixture staleness; repaired,
  failure evidence retained at
  local/tortoise-bot-eq-b8378696f55e-20260917T161801Z/) and
  test_bot_combat_xp (known 3-day flake; history retained under
  local/tortoise-bot-xp-*; rerun green on the old pinned image). Not run -
  hosted diff review, formal QA handoff with read-only worker, in-game
  client evidence, Jira transitions (all parked for hosted/operator).
- Known risks: combat_xp engagement flake across 09-12..09-17 and across
  images (the lab pins tortoise-local:mvp003-review, not dev); the equipment
  fixture was stale against the PORT-025 junk matrix, so future matrix
  changes must re-check lab filler items; the tick-impact measurement
  used 120 s windows with 22 samples per run, so it bounds rather than
  exhausts tick-tail behavior.
- Offline/delayed tick-impact measurement completed 2026-09-17 (four
  port-free labs: disabled, offline, 400 ms delayed, timeout; 120 s
  windows, Perf.ProcessingTelemetry=5, last 22 samples): no measurable
  tick degradation (proc p95 med 6-7 ms vs 6 ms disabled baseline; tick
  interval med 50 ms in every run). Evidence:
  local/port026-tick-impact-summary.json.
- Worker/retrieval: no park-agent worker (park-head holds the single-model
  mutex; the 27B at 127.0.0.1:8090 is this session's own backend).
  Retrieval sync unavailable: the bridge returns an MCP error on sync
  (embedding service down); bounded direct-read fallback used throughout;
  no sync, embedding or derived-write counts are claimed.
- Jira: untouched this session; Phase 2 stories stay In Progress until the
  operator's in-game validation and the hosted QA handoff.
- Next bounded assignment: hosted acceptance of PORT-026 (diff review +
  formal QA handoff + in-game evidence + sign-off).

## Handoff update contract

Update this file at each session stop and phase exit. Never replace failed evidence
with a success summary: retain the failure and link the repair. Record:

- Date, active branch, baseline and resulting commit IDs; distinguish uncommitted work.
- Changed paths and relevant functions; why the change exists and its invariants.
- Commands actually run, exit results, exact tested source/image, and sanitized evidence paths.
- Acceptance scenarios passed, failed, or not run; separate fixture evidence from in-game observations.
- Known risks, unsupported behavior, dependencies, and the next bounded assignment.
- Worker/retrieval availability, independent review outcome, and any pending user decision.
- Deployment and Jira status independently; never infer them from a successful build.

Keep raw logs and synthetic lab artifacts under ignored `local/`. Never put secrets,
personal character data, or environment maps in these documents or worker cards.
Code comments should explain ownership, cancellation, lifetime and safety invariants,
not repeat syntax. Update affected comments when changing those contracts.
