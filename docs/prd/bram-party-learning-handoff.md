# Bram party learning: local-worker handoff

Last updated: 2026-09-21

This is the compact continuity record for the work defined in
[`bram-party-learning.prd`](bram-party-learning.prd). Read that PRD for the
complete requirements; use this document to avoid rediscovering scope, current
status, and non-negotiable safety contracts each session.

## Current state

| Item | State |
| --- | --- |
| Branch | `feature/kap-558-port-phase2` |
| Baseline at planning | `ae06f56604ecd5b1ea3570868a79a3c179bcf794` |
| Feature implementation | BL-001A/B accepted; BL-001R accepted measurable candidate; BL-002 next |
| PRD | v1.2, tracked implementation specification only |
| Phase 2 | Historical deterministic battery recorded; hosted acceptance and client evidence remain pending |
| Retrieval | Services restored on 2026-09-21; repository catch-up completed through `c578197`. Confirm status/freshness before each card. |
| Personal realm | Not touched; no deployment or migration authorized by this handoff |

Do not modify unrelated working-tree changes: `src/game/Objects/Item.cpp` and
`.idea/` are user-owned. Do not stage, commit, push, deploy, reset, or alter the
personal character database as part of any card.

## Goal in one sentence

Let Bram, the owner's warrior companion, retain an evidence-backed tactical
playbook while adventuring in the owner's party; the LLM proposes bounded data,
and C++ enforces rules, measures outcomes, and promotes or rolls back changes.

This is not model-weight training and does not let the LLM write C++, SQL,
spell IDs, coordinates, targets, or arbitrary conditions. It lets the LLM propose
data for a finite rotation plan over cataloged actions Bram already knows.

## What exists today

- High-level companion intent selection is already pure/value-based in
  `src/game/PlayerBots/Companion/Policy.h`.
- Combat legality and executable-ability filtering are already shared/value-based
  in `src/game/PlayerBots/Companion/Combat.h`.
- `PlayerBotAI::SelectOffensiveSpell` still creates hardcoded class-specific
  ordered action lists. There is no runtime playbook or configurable rotation.
- The dedicated warrior tank behavior requires level 10 and learned Taunt 355.
  On the Assist path it selects Taunt or ordinary attack and bypasses the generic
  offensive selector. This is a future capability layer; it does not prevent a
  level-9 warrior from using the current general offensive selector.
- Existing planner output affects only bounded follow-distance preferences and
  cosmetic expression. It receives class/party flags, not tactical observations
  or encounter outcomes.
- Dedicated healer coverage is druid-only and declared damage discipline is
  rogue-only. The framework must be reusable, but only warrior learning is in
  scope for this release.

## Required order

**Current order overrides the historical numbered text below:** BL-001 is closed
with `reserve_rage` rejected. Execute BL-001A, then BL-001B, then BL-001R to audit
a current-level rotation-plan candidate, followed by BL-002 through BL-010. The
numbered list is retained only as history of the original proposal.

Do not implement the learning loop as one large change. Work in this order:

1. **BL-001 — audit.** Prove whether Bram has a legal optional rage spender and
   a valid reason preserving rage improves a supported protective action. Audit
   Assist, ContinueCombat, and Defend separately, including `.botinit` flow.
   If this premise fails, stop and return a bounded prerequisite proposal.
2. **BL-001A — extract contracts.** Add a shared ability catalog, runtime
   playbook, and decision interface while preserving existing behavior. Prove
   warrior and mage baseline parity with value tests.
3. **BL-001B — integrate engagement sources.** Route Assist, ContinueCombat,
   and Defend through the common interface while retaining their source-specific
   rules. Prove the Taunt bypass/default behavior explicitly.
4. **BL-002 through BL-010.** Recorder, persistence, protocol v2, warrior
   setting, prompts, evaluator, owner commands, disposable qualification, then
   owner party-play pilot.

The complete acceptance criteria and validation for each card remain in the PRD.
One fresh, serialized local worker handles one card only.

## Architecture contract

```text
authoritative snapshot
  -> bounded LLM tactic proposal
  -> live C++ validation and execution
  -> encounter recorder
  -> coded evaluator
  -> versioned playbook (retain / rollback)
```

The desired common selector is conceptually:

```cpp
Decision SelectAction(Observation, CapabilitySet, Playbook, EngagementContext)
```

The baseline playbook must exactly preserve current behavior. Owner hold, party
membership, target legality, recovery, emergency protection, range, cooldowns,
resources, and source-specific restrictions are outside LLM control. Unknown,
stale, unsupported, incompatible, or late proposals fall back to baseline.

Initial LLM control is a validated finite warrior priority plan: ordered catalog
actions with finite gates. The LLM may rearrange or omit eligible actions only; it
cannot introduce an ability, condition, target, pull, or mechanic.

## Experiment and evaluation contract

BL-001 closed `reserve_rage` as NOT viable: Taunt costs no rage, and the existing
rage spenders are offensive, so it has no causal path to the original protection
metric. This does not block learning at level 9.

The first candidate is instead a current-level `rotation_plan`: one change to the
established priority or finite gate among ability-catalog actions Bram currently
knows and can execute. The catalog always derives the legal set from Bram's live
capability fingerprint. A level change, new learned spell, equipment change, or
policy version changes that fingerprint and returns the companion to observation.

Before a plan enters shadow mode, BL-001R must declare its single changed priority
or gate, causal hypothesis, fixture context, primary metric, and safety limits.
Rotation candidates use effective hostile damage per active combat second and
action-effect rate; protection candidates use unprotected fraction and recovery
latency. Every candidate preserves owner orders, target legality, no-new-pull,
and emergency behavior. No candidate means baseline play plus observation.

Mode progression is:

```text
disabled -> observe -> shadow -> trial -> retained | rejected | inconclusive
```

- Collect 20 complete matching observations before shadow mode.
- Require 10 schema-valid, authority-safe shadow proposals before trial.
- Randomly preassign baseline/candidate in 1:1 persisted blocks; the model cannot
  select which encounters test its proposal.
- Evaluate after 30 complete comparable encounters per arm. Promote only if the
  predeclared primary metric improves by at least 10% relatively, its 95% bootstrap
  interval supports improvement, and safety/damage/duration limits pass.
- Any candidate-arm death, owner-hold violation, unauthorized action, new pull,
  or suppressed emergency recovery immediately suspends the trial and restores
  baseline. This is a safety stop, not proof the candidate caused the event.

Do not count incomplete, overflowed, incomparable, owner-overridden, or
inapplicable-policy encounters as efficacy evidence. Record why they were excluded.

## Local worker constraints

Before a substantial card, hosted Codex must verify retrieval/current-source
evidence and create a new evidence card from `docs/bots/ralph/worker-card.md`.
The local worker must:

- read only the explicitly allowed files and synthetic/disposable evidence;
- never read `.env`, credentials, character data, backups, raw personal logs, or
  environment maps;
- not call retrieval, Jira, nested `park-agent`/`park-head`, or another model;
- not commit, stage, push, deploy, transition Jira, edit acceptance flags, or
  declare final acceptance;
- report hashes, actual commands/tests, changed paths, failures, and uncertainty.

The head verifies every worker diff, runs appropriate tests, owns scope changes,
and reports retrieval sync counts after tracked-source changes.

## Update block: append after every card

Copy this block at the end of this document after each completed or stopped card:

```text
### [BL-XXX] — YYYY-MM-DD
- Status: not started | in progress | stopped | complete pending head review | accepted
- Baseline/result commit: <hash> / <hash or uncommitted>
- Worker: <fresh session identifier or none>; edit permission: <yes/no>
- Retrieval: <repository/status/search/current-source hash verification or fallback>
- Changed paths: <bounded list>
- Decision and rationale: <what was selected or deferred>
- Validation: <commands, fixture/image identity, result>
- Evidence: <sanitized local paths or source citations>
- Open risks/uncertainty: <specific>
- Next permitted card: <BL-XXX or stop reason>
```

Never replace prior failed evidence with a success-only summary. Retain the failed
attempt, its cause, and the repair/retest evidence.

### [BL-001] -- 2026-09-21
- Status: complete pending head review (decision rendered; operator direction pending)
- Baseline/result commit: ae06f56 / audit-doc commit
- Worker: none (park-head local-Qwen direct reads); edit permission: no
- Retrieval: direct current-source reads for this read-only closeout; Tank.h and PlayerBotAI.cpp SHA-256 match the PRD-pinned values (unchanged since baseline); live tw_char read for enrolled tank level/spellbook. No new retrieval search run in this session; the substantive BL-001 investigation used retrieval in the prior session.
- Changed paths: docs/prd/bram-party-learning-bl001-audit.md (new)
- Decision and rationale: NOT VIABLE as specified. The supported tank matrix has no rage-costing protective action; the sole recovery Taunt(355) is 0-rage (gated by learned/cooldown/range, never rage), so rage_blocked_recovery_count is structurally 0; the only rage spenders are offensive (suppressing them cannot improve protection); the enrolled tank is below the capability window (level 9, no Taunt 355). Bounded prerequisite proposed (section 8 of the audit).
- Validation: read-only audit; no code, no SQL, no tests (not applicable). Verified cited source lines: Tank.h 66-140; PlayerBotAI.cpp 1361-1395, 1700-1723, 4498-4514.
- Evidence: docs/prd/bram-party-learning-bl001-audit.md (sections 2-6, 10); live tw_char characters + character_spell for the enrolled tank.
- Open risks/uncertainty: reserve_rage has no supported causal path to unprotected_fraction under the current matrix; a viable candidate requires a newly-defined rage-gated protective action or a primary-metric redefinition (both out of scope, to be accepted separately); offense DBC PowerType/cost not yet pinned (Spell.dbc-only, no SQL mirror).
- Next permitted card: BL-001A (shared contracts + pure baseline extraction), then BL-001B and BL-001R for a viable current-level rotation candidate. A protective-action prerequisite is only needed for a future protection candidate, not for current-level rotation learning.

### [Head specification update] -- 2026-09-21
- Status: complete pending review
- Baseline/result commit: c578197 / uncommitted
- Worker: none; edit permission: documentation only
- Retrieval: service recovery is in progress; do not rely on a stale index without fresh status/hash verification.
- Changed paths: docs/prd/bram-party-learning.prd; docs/prd/bram-party-learning-handoff.md
- Decision and rationale: accept BL-001's technical rejection of `reserve_rage`, but replace its incorrect product implication. Bram begins with the legal abilities he has now; learning is a finite, evaluated priority/gate plan, not a level-10 tank-only or rage-reserve feature.
- Validation: git diff --check pending after documentation update.
- Evidence: BL-001 audit and current source citations retained in docs/prd/bram-party-learning-bl001-audit.md.
- Open risks/uncertainty: actual level-9 ability catalog, action effects, and first measurable rotation delta must be verified by BL-001A/BL-001R against matching game data.
- Next permitted card: BL-001A.

### [BL-001A] -- 2026-09-21
- Status: accepted foundation; no learned behavior is connected.
- Baseline/result commit: `c5781974b444f6ad692b48a516b0994d8cf59bc5` / uncommitted.
- Worker: two fresh serialized `park-agent` sessions; edit permission: yes. The first submitted a heap-allocating draft and was rejected. The second repaired it; the hosted head independently added the fail-closed oversized-plan check and current warrior/mage parity fixture.
- Retrieval: `tortoise-wow` current-source evidence verified before work: `Combat.h` `0523691ffd564ba3db7ea51a1476ee693d4ccc72c276c8535b5ac53d55495474`; `PlayerBotAI.cpp` `4ce3b770c07627ebdfa828cf3fa659bb1142e20e445b166d9a519e1821f2faa5`.
- Changed paths: `src/game/PlayerBots/Companion/Combat.h`, `src/game/PlayerBots/PlayerBotAI.cpp`, and `docker/test_companion_combat_value.cpp`.
- Decision and rationale: introduce only a bounded, index-only `RotationPlan`. Default construction or any incompatible plan falls back to the exact existing table order. The runtime currently supplies the identity plan, so no learned decision is active. The plan uses fixed storage for at most eight catalog entries; creation, validation, and selection perform no heap allocation.
- Validation: `python -m unittest discover -s docker -p 'test_companion_combat_value.py'` (passed), `docker compose config --quiet`, `docker compose build world` (passed; includes `PlayerBotAI.cpp`), `python -m py_compile docker/server.py` (passed), and `git diff --check` (passed). A prior full Python discovery was stopped after a long-running incomplete `.F....` progress stream; it was not used as acceptance evidence. No personal realm was started or changed.
- Evidence: `PlayerBotAI::SelectOffensiveSpell` builds the baseline identity plan and invokes the plan-aware selector. Synthetic fixtures cover identity parity, reordering, null/wrong-size/duplicate/out-of-range/negative/oversized fallback, index-only scope, and live warrior/mage table ordering.
- Open risks/uncertainty: this is a source/build contract only. It has no runtime playbook persistence, no LLM input, and no in-game behavior evidence; those remain later cards. Re-run the focused C++ harness on a host with a compiler when available.
- Next permitted card: BL-001B, after acceptance and retrieval sync.

### [BL-001B] -- 2026-09-22
- Status: accepted foundation; no learned behavior is connected.
- Baseline/result commit: `c5781974b444f6ad692b48a516b0994d8cf59bc5` / uncommitted cumulative BL-001A/B worktree.
- Worker: fresh serialized `park-agent` session `01a0c951-0a9e-7172-8795-4a999a81cf88`; edit permission: yes for `Combat.h`, `PlayerBotAI.cpp`, and the focused value test only.
- Retrieval: healthy/fresh after a no-op `tortoise-wow` sync. Verified starting hashes: `PlayerBotAI.cpp` `04cfe82c28b9bc25d696e9941241aab5e8b522ae1cf16f837e4839a4be42cf2d`; `PlayerBotAI.h` `1660c58e567937820323a89f9f69b134ad0eab51862bd038b7cedb0b23aadb45`; `Combat.h` `deb3f05722adc2114229778dacb2d6e259af100f5bd7b7a7dd840da3fd88e2b4`; value test `e5601b74d024d05d7b087f2b3dacae433819e26f031fc938f69c10c976051976`.
- Changed paths: `src/game/PlayerBots/Companion/Combat.h`, `src/game/PlayerBots/PlayerBotAI.cpp`, and `docker/test_companion_combat_value.cpp`.
- Decision and rationale: extract pure `RouteOffense(Source, declaredTank)` with two bounded routes. Only Assist plus the declared-tank gate selects the existing Tank/Taunt branch. Low-level Assist, ContinueCombat, Defend, and Damage retain the ordinary rotation path. The head preserved the original short-circuit so non-Assist sources do not perform the tank capability lookup.
- Validation: focused C++ value harness passed (all eight source/gate matrix rows); `docker compose build world` passed with `ALLOW_TURTLE_ADDONS=ON`; `docker compose config --quiet` and `git diff --check` passed. Disposable `.botinit` fixture passed 10 tests in 249.156 seconds; disposable reactive-defend fixture passed 11 tests in 174.182 seconds. Neither fixture changed personal containers.
- Evidence: the runtime adapter consumes the pure route inside `ExecuteCombat`; existing Tank observation/Taunt/white-attack behavior and rotation fallback remain unchanged. The `.botinit` and defend fixtures exercise deferred setup, defend enablement, hold cancellation, re-engagement, and owner-only authority through real chat paths.
- Open risks/uncertainty: runtime fixtures prove `.botinit` and Defend baseline behavior but do not yet exercise a non-baseline learned plan. The worker performed metadata-only `docs/prd` listing and `git status` checks outside the literal file read allowlist; no unlisted source contents or private data were read, but this process deviation is retained rather than hidden.
- Next permitted card: BL-001R current-level Bram rotation-candidate audit.

### [BL-001R] -- 2026-09-22
- Status: accepted audit; candidate is viable to measure, not proven better.
- Baseline/result commit: `c5781974b444f6ad692b48a516b0994d8cf59bc5` / uncommitted audit document.
- Worker: fresh serialized read-only `park-agent` session `01a0c963-1442-7773-b9e0-5ccc0ed4badf`; edit permission: no.
- Retrieval: healthy/fresh after the accepted BL-001B sync. Verified starting hashes: `PlayerBotAI.cpp` `55c0706ef177bd47715f20b599d7df0962053a7f4a69af2868bfa78f1948a77b`; `Combat.h` `44a9b6176f1fed8402065d210593afd40f9bd7e3a4de148eb797b74cbe48ed8a`; value test `4c5f3a114cf7f98fad28f5909fcf79c34d5bb7b3d24acf2c376580118eecbdae`.
- Changed paths: `docs/prd/bram-party-learning-bl001r-audit.md` and this handoff block; no runtime source changed.
- Decision and rationale: accept one current-level priority candidate: `Charge,Rend,Hamstring,Heroic Strike` versus baseline `Charge,Hamstring,Rend,Heroic Strike`. The legal difference occurs when Charge is unavailable, both aura-gated melee actions are legal, and rage permits exposure. This does not wait for level 10 and never applies to the Tank/Taunt route.
- Validation: read-only live capability check found level-9 warrior ranks Charge 100, Heroic Strike 284, Rend 772, and Hamstring 1715; current source proves rank-chain resolution, aura gating, on-next-swing exclusion, and the pure permutation selector. The audit defines a deterministic synthetic choice fixture and a later disposable-world experiment. No behavior was changed and no improvement is claimed.
- Evidence: `docs/prd/bram-party-learning-bl001r-audit.md`; live data was reduced to a sanitized capability fingerprint before local-worker use.
- Open risks/uncertainty: authoritative Rend effect timing and Hamstring slow consequences were outside worker evidence. Rage must be comparable and sufficient for both actions (or explicitly modeled), and fight duration must be stratified. BL-002/BL-007 must measure the result and may honestly reject or declare it inconclusive.
- Next permitted card: BL-002 encounter/value contracts and observe-only recorder hooks.

### [BL-002] -- 2026-09-22
- Status: accepted observe-only foundation; no learned behavior, persistence, or model call is connected.
- Baseline/result commit: `c5781974b444f6ad692b48a516b0994d8cf59bc5` / uncommitted cumulative BL-001A/B/R and BL-002 worktree.
- Worker: fresh serialized `park-agent` session `01a0c96b-461a-7233-a50d-242626733115`; edit permission: yes for the seven files named by the BL-002 evidence card. The hosted head stopped a repetitive post-compaction review loop after implementation and focused tests completed.
- Retrieval: healthy/fresh before assignment. Verified starting hashes: `PlayerAI.h` `61f64248547c...d5e`; `Unit.cpp` `b30cb46974ca...e37f`; `PlayerBotAI.h` `1660c58e5679...b45`; `PlayerBotAI.cpp` `55c0706ef177...77b`; `Combat.h` `44a9b6176f1f...ed8a`. Full hashes and bounded evidence are retained in `docs/prd/bram-party-learning-bl002-worker-card.md`.
- Changed paths: `src/game/PlayerBots/Companion/Encounter.h`, `src/game/AI/PlayerAI.h`, `src/game/Objects/Unit.cpp`, `src/game/PlayerBots/PlayerBotAI.h`, `src/game/PlayerBots/PlayerBotAI.cpp`, `docker/test_companion_encounter_value.cpp`, and `docker/test_companion_encounter_value.py`.
- Decision and rationale: accept a fixed-capacity, allocation-free encounter recorder and observe-only engine hooks. It records legal engagement duration, route/decision/cast outcomes, target-matched effective direct/periodic damage, damage taken, deaths, owner overrides, explicit terminal reasons, completeness, and overflow. Overflow and interrupted encounters fail closed for efficacy. Direct player attacker/victim callbacks use authoritative health loss and do not attribute pet damage. Existing combat decisions and fallbacks remain unchanged.
- Validation: encounter value harness passed (1 test, 2.502 seconds); combat value harness passed (1 test, 2.524 seconds); Python compile and `git diff --check` passed after repairing two pre-existing BL-001R documentation whitespace findings; `docker compose build world` passed with `ALLOW_TURTLE_ADDONS=ON`, including `PlayerAI.cpp`, `Unit.cpp`, and `PlayerBotAI.cpp`. No personal realm was started or changed.
- Evidence: `docs/prd/bram-party-learning-bl002-worker-card.md`; fixed-value tests cover lifecycle, matching/unrelated attribution, direct/periodic damage, damage taken, target/Bram death, owner override, target replacement, cast/decision outcomes, overflow, zero duration, duplicate end/summary, and exclusion gates.
- Open risks/uncertainty: recorder output is intentionally memory-only and has no runtime log/export surface, so this card proves contracts and engine compilation rather than an in-world persisted observation. Persistence, queueing, schema evolution, and restart recovery remain BL-003. The worker used byte-writing helper scripts instead of the preferred patch helper and performed limited Git-status/diff inspection despite the literal worker-card restriction; the head independently reviewed the resulting paths and diff.
- Next permitted card: BL-003 persistence schema and bounded asynchronous observation writer.

### [BL-003] -- 2026-09-22
- Status: implementation validated; no learned behavior or LLM call is enabled.
- Baseline/result commit: `c5781974b444f6ad692b48a516b0994d8cf59bc5` / uncommitted cumulative worktree.
- Worker: fresh serialized repair `park-agent` session `01a0c9a6-0e41-7fa0-af96-1e468f23d0fd`; the initial pass was independently rejected for missing persistence wiring and unsafe callback/FIFO ownership. The repair worker was stopped after its focused value test passed and its SQL exploration became repetitive; the hosted head completed the final schema/test corrections.
- Retrieval: `tortoise-wow` registered; database, embedding service, and schema healthy. Final sync: added 8, modified 4, unchanged 3387, skipped 257, chunks embedded 121, derived writes 137. The index metadata remains at the base commit because this work is uncommitted; the worktree is intentionally not claimed fresh until commit.
- Changed paths: `src/game/PlayerBots/Companion/LearningStore.h/.cpp`, `src/game/PlayerBots/PlayerBotAI.h/.cpp`, `src/game/PlayerBots/PlayerBotMgr.h/.cpp`, `sql/database_updates/character/20260922120000_character.sql`, `docker/test_companion_learning_store_value.cpp/.py`, `docker/test_bot_learning_migration.py`, and this worker evidence/repair card.
- Decision and rationale: add a fixed 64-summary FIFO and one-in-flight async character writer. The store assigns a nonzero process nonce plus monotonic sequence, persists only scalar recorder summaries, gates inserts by profile mode, deduplicates delivery identity, queues startup stale-row interruption, and fails closed on writer/submission failure. Completion callbacks publish only shared atomics; the world pump applies FIFO disablement. Completed encounters enqueue before recorder reset on target/Bram death, owner override, target replacement, invalid target, and leash expiry, for owned companions only. Retention is explicit; when citation-safe ranking cannot be expressed safely in MariaDB, the product pauses the profile and preserves evidence rather than deleting it.
- Validation: `python docker/test_companion_learning_store_value.py -v` passed; `python docker/test_bot_learning_migration.py -v` passed (9 tests); `docker compose build world` passed with `ALLOW_TURTLE_ADDONS=ON`; `git diff --check` passed. The disposable migration uses a fresh, port-free Compose project/volume and was torn down; the personal realm was not started or modified.
- Evidence: focused FIFO tests cover order, cap/drop/failure counters, reset, one-in-flight completion, submission failure, duplicate identity, and SQL contract checks. MariaDB tests cover twice-applied migration, profile gating, duplicate delivery, CAS, rollback/audit, stale interruption, retention protection/fail-closed pause, and restart identity continuity.
- Open risks/uncertainty: the implementation is observe-only; no candidate evaluator, LLM planner, promotion, owner command, or in-world Bram pilot exists yet. Runtime persistence against a live personal realm remains intentionally untested. The repair worker repeated directory/source-scope discovery and used byte-writing helper scripts rather than the preferred patch helper; the head independently reviewed and validated the resulting diff. Retention currently pauses an overfull profile rather than pruning ranked history, preserving evidence safely but requiring a later dedicated pruning design if automatic compaction is desired.
- Next permitted card: BL-004 tactical protocol v2, then BL-005/BL-006 for candidate control and LLM learning; do not claim Bram has learned or improved until BL-009/BL-010 evidence exists.

### [BL-004] -- 2026-09-22
- Status: validated pure protocol foundation; no runtime integration, LLM call, or learned behavior.
- Baseline/result commit: `c5781974b444f6ad692b48a516b0994d8cf59bc5` / uncommitted cumulative worktree.
- Worker: fresh serialized BL-004 `park-agent` session was stopped after repeated PowerShell/heredoc write failures before it produced an accepted patch. The hosted head completed the bounded pure module and test; no worker acceptance authority was used.
- Retrieval: healthy `tortoise-wow` index. Final sync after tracked-intent files: added 4, modified 0, unchanged 3399, skipped 257, chunks embedded 10, derived writes 14. Index metadata remains at the base commit because the worktree is uncommitted.
- Changed paths: `src/game/PlayerBots/Companion/TacticalProtocol.h`, `docker/test_companion_tactical_protocol_value.cpp`, `docker/test_companion_tactical_protocol_value.py`, and this evidence card.
- Decision and rationale: introduce a separate v2 value contract for ability-priority candidates. It carries immutable request identity, observation/capability versions, login/order generations and bounded candidate expiry. Validation is all-or-nothing and rejects owner hold, stale/mismatched generations, capability changes, duplicate/reordered abilities, expired candidates, and out-of-range values. A small value-only round state machine invalidates in-flight results on hold or capability change and otherwise fails closed.
- Validation: `python docker/test_companion_tactical_protocol_value.py -v` passed using the WSL C++ compiler fallback; `git diff --check` passed. No world build was required because the new header is not connected to runtime sources. PlannerProtocol v1 and PlannerTransport were not edited. No realm or personal database was started or changed.
- Open risks/uncertainty: the protocol is intentionally not wired into Bram yet; it cannot affect combat until BL-005 adds a validated playbook adapter and evaluator. The worker attempted Unix heredocs in PowerShell and failed to write, so the head performed the final bounded edits.
- Next permitted card: BL-005 validated warrior playbook adapter and candidate execution seam; still no automatic promotion until BL-007/BL-009.

### [BL-005] -- 2026-09-22
- Status: validated pure warrior playbook adapter; runtime remains baseline and no LLM/persistence/promotion is connected.
- Worker: fresh serialized session `01a0ca48-fdde-7eb2-afc1-9fd0b3050f58` was stopped after it accidentally restored `Combat.h` and then produced a compile-breaking partial edit. The hosted head independently repaired the bounded header and tests.
- Changed paths: `src/game/PlayerBots/Companion/Combat.h`, `docker/test_companion_combat_value.cpp`, and this handoff/card.
- Decision and rationale: `WarriorPlaybookAdapter` accepts only a complete permutation of the current bounded catalog, maps ability IDs to fixed `RotationPlan` indices, and fails closed to baseline for null, unknown, duplicate, missing, oversized, or capability-version-mismatched candidates. Existing eligibility, on-next-swing exclusion, tank routing, and class behavior remain unchanged because only scan order is adapted.
- Validation: `python docker/test_companion_combat_value.py -v` passed; adapter tests cover the BL-001R ordering and duplicate/stale fallback; `git diff --check` passed. No world build or realm was started.
- Open risks/uncertainty: the adapter is not wired into `PlayerBotAI`, there is no evaluator or LLM request, and no improvement claim is made. The worker's restore operation required head reconstruction of the prior uncommitted rotation-plan seam; worker used a prohibited directory listing during discovery.
- Next permitted card: BL-006 bounded candidate evaluation/planner seam, still observe-only until evaluator evidence and promotion gates exist.

### [BL-006] -- 2026-09-22
- Status: validated bounded sidecar learning-prompt contract; no runtime Bram control, candidate evaluation, persistence authority, or live model call.
- Worker: fresh `park-agent` session `01a0ca4e-852c-79c3-bc76-209e52b8168d` was stopped after extended discovery without producing an edit. The hosted head completed the narrow allowed files and independently validated them.
- Changed paths: `docker/personality-service/real_planner.py`, `docker/personality-service/test_learning_prompts.py`, this card, and handoff.
- Decision and rationale: add schema-versioned, deterministic bounded request construction for capability/playbook context, at most three lessons and five summaries, with fabricated/sensitive evidence rejection, size limits, and value-only latency/contention metadata. Existing v1 planner functions remain unchanged.
- Validation: `python docker/personality-service/test_learning_prompts.py -v` passed (3 tests); `git diff --check` passed. No C++ or realm changes.
- Open risks/uncertainty: this is a pure prompt boundary, not proof of model quality or tactical improvement. BL-007 must implement evaluator matching, assignment, and mode transitions; direct DB access and runtime wiring remain prohibited here. Worker used repeated broad discovery and did not complete the implementation.
- Next permitted card: BL-007 candidate evaluator, matching, assignment, and automatic mode transitions.

### [BL-007] -- 2026-09-22
- Status: validated pure synthetic evaluator and deterministic arm assignment; no runtime mode transition or promotion is enabled.
- Changed paths: `docker/personality-service/learning_evaluator.py`, `docker/personality-service/test_learning_evaluator.py`, and this handoff.
- Decision and rationale: compare only eligible, complete samples; require minimum sample count; fail closed to `inconclusive` for sparse or incomplete evidence; classify improvement only with a bounded 10% reduction in unprotected fraction and no death regression; classify death increase as regression. Candidate exposure is deterministic alternating assignment and otherwise baseline.
- Validation: `python docker/personality-service/test_learning_evaluator.py -v` passed (3 tests); `git diff --check` passed. No C++ or realm changes.
- Open risks/uncertainty: this evaluator uses synthetic value inputs and is not connected to persisted encounter summaries, capability matching, restart state, or automatic promotion. It does not prove Bram improvement. Those integrations remain for later cards.
- Next permitted card: BL-008 owner commands, notices, status, explain, pause/resume, and rollback contracts.

### [BL-008] -- 2026-09-22
- Status: pure Python authorization sketch validated, but runtime card reopened after current-source audit found no C++ profile/candidate control repository and no command wiring. BL-008 is not complete.
- Changed paths: `docker/personality-service/owner_controls.py`, `docker/personality-service/test_owner_controls.py`.
- Validation: 2 focused tests passed; intruders and unknown commands fail closed; pause/resume/rollback transitions are bounded.
- Blocking prerequisite: BL-008A must add typed, asynchronous C++ learning-control state operations before command handlers can safely mutate or report learning state. The actual handlers live in `src/game/Commands/Commands.cpp`, which was outside the first runtime card.
- BL-008A progress: typed control modes, candidate-state vocabulary, bounded status value, and SQL templates were added to `LearningStore.h`, but the async operation queue and control tests are not complete. The head reconciled the stale retention assertions with the accepted fail-closed behavior (`kPlaybookRetentionSql` pauses over-budget profiles and preserves evidence); the focused learning-store harness is green again. BL-008A remains in progress; do not register `.botlearn` yet.
- BL-008A serialized seam update: start, pause, and resume now enqueue through the Store's existing maintenance/encounter single-in-flight slot; invalid identity and invalid resume modes fail closed. Rollback deliberately returns false because the current bool-only submit callback cannot atomically verify both audit insertion and baseline activation or expose affected-row/query state. Focused tests prove serialized ordering and that rollback produces no partial SQL. The remaining prerequisite is a bounded async query/transaction result adapter.
- BL-008B audit: current core APIs provide `BeginTransaction`/`CommitTransaction(callback)` and `AsyncQuery(QueryResult*)`, so no shared-database rewrite is required. The safe integration boundary is injected transaction/status adapters in `PlayerBotMgr`, with only scalar `ControlStatus` crossing into `LearningStore`. A fresh worker completed the source/API audit but could not produce a bounded patch because its Windows command exceeded process limits; no BL-008B implementation was accepted. Evidence card: `docs/prd/bram-party-learning-bl008b-worker-card.md`.
- BL-008B slice 1: `LearningStore` now exposes a bounded external-control reservation/completion seam over its existing one-in-flight state. Encounter and maintenance submissions cannot overlap an injected transaction/query; invalid status cannot replace the last confirmed snapshot; failed external work increments a bounded counter. The focused value harness covers contention, malformed status, confirmed insufficient-evidence status, failure, and resumption of queued encounter writes. Manager transaction/query wiring remains next.
- BL-008B slice 2: `PlayerBotMgr::QueueLearningRollback` now reserves the Store slot, queues the rollback audit and baseline profile reset in one character-database transaction, and commits with a lifetime-safe callback consumed by the Store on the world thread. Begin/statement/commit failures fail closed and release the reservation without publishing baseline status. The focused value harness passed and `docker compose build world` completed successfully with `ALLOW_TURTLE_ADDONS=ON`. Status-query wiring and command authorization remain next; no realm was started.
- BL-008B slice 3: asynchronous status-query wiring is complete. The manager
  reserves the Store slot, submits the bounded profile query through the core
  async result queue, reduces the result to scalar `ControlStatus`, and rejects
  missing/malformed/invalid results without replacing confirmed state. A fresh
  worker request hung while loading the local model and was stopped; the hosted
  head used the documented fallback. The first build exposed a missing database
  template include at final link; after repair, the full world image built.
- BL-008 runtime command: `.botlearn start|status|pause|resume|explain|rollback
  <botname>` is registered at `SEC_PLAYER` and reuses the established ownership
  binding. Start idempotently creates a missing profile or moves disabled to
  observe; status/explain are asynchronous and explicitly report insufficient
  evidence; pause/resume serialize through the Store; rollback uses the accepted
  transaction. No candidate execution or improvement claim is enabled.
- Pilot-ready boundary: ready for an owner observation/persistence pilot after
  the normal backup/session/migration startup checks. In game: `.botinit`, then
  `.botlearn start Bram`, ordinary party combat, `.botlearn status Bram`, logout
  and orderly restart, then `.botlearn status Bram` again. Expected status is
  `observe`, baseline playbook `0`, and insufficient evidence. Shadow/trial and
  learned-improvement claims remain out of scope until evaluator/runtime wiring
  and real evidence exist.

### [BL-009] -- 2026-09-22
- Status: deterministic qualification summary contract validated; no live model or realm qualification performed.
- Changed paths: `docker/personality-service/qualification.py`, `docker/personality-service/test_qualification.py`.
- Validation: 2 focused tests passed; qualification requires explicit real-model evidence and trial data, otherwise reports `pending-real-world-evidence`.
- Open risk: runtime budgets, persistence restart, and actual model latency still require disposable/in-world testing.

### [BL-010] -- 2026-09-22
- Status: final pilot gate remains pending; no personal realm was started and no owner-party observations were fabricated.
- Decision: the implementation is not accepted as “Bram learned” until a controlled `.botinit` party session supplies real observations, restart continuity, and evidence-backed status. Synthetic cards are complete but are not a substitute for the pilot.
- Next step: controlled pilot execution and final report, with baseline fallback and honest inconclusive outcome allowed.
