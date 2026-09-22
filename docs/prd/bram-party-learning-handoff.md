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
| Feature implementation | Not started |
| PRD | v1.1, untracked, review/implementation specification only |
| Phase 2 | Historical deterministic battery recorded; hosted acceptance and client evidence remain pending |
| Retrieval | Services restored on 2026-09-21; `tortoise-wow` catch-up running at handoff time. Confirm status before relying on it. |
| Personal realm | Not touched; no deployment or migration authorized by this handoff |

Do not modify unrelated working-tree changes: `src/game/Objects/Item.cpp` and
`.idea/` are user-owned. Do not stage, commit, push, deploy, reset, or alter the
personal character database as part of any card.

## Goal in one sentence

Let Bram, the owner's warrior companion, retain an evidence-backed tactical
playbook while adventuring in the owner's party; the LLM proposes bounded data,
and C++ enforces rules, measures outcomes, and promotes or rolls back changes.

This is not model-weight training and does not let the LLM write C++, SQL,
rotations, spell IDs, coordinates, targets, or arbitrary conditions.

## What exists today

- High-level companion intent selection is already pure/value-based in
  `src/game/PlayerBots/Companion/Policy.h`.
- Combat legality and executable-ability filtering are already shared/value-based
  in `src/game/PlayerBots/Companion/Combat.h`.
- `PlayerBotAI::SelectOffensiveSpell` still creates hardcoded class-specific
  ordered action lists. There is no runtime playbook or configurable rotation.
- The dedicated warrior tank behavior requires level 10 and learned Taunt 355.
  On the Assist path it selects Taunt or ordinary attack and bypasses the generic
  offensive selector. Do not claim a generic rage-reserve setting changes that
  path unless the integration proves it.
- Existing planner output affects only bounded follow-distance preferences and
  cosmetic expression. It receives class/party flags, not tactical observations
  or encounter outcomes.
- Dedicated healer coverage is druid-only and declared damage discipline is
  rogue-only. The framework must be reusable, but only warrior learning is in
  scope for this release.

## Required order

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

Initial LLM control is only a validated warrior tactical setting. Learned ability
priority reordering is a later feature with its own finite condition vocabulary
and acceptance work.

## Experiment and evaluation contract

The first candidate is `reserve_rage` with allowed values 0, 10, or 20 displayed
rage points. It may suppress only verified optional offense when the post-cast
resource would breach the reserve. It must never suppress an eligible emergency
recovery. If no protected action benefits from retained rage, do not implement
this candidate.

Mode progression is:

```text
disabled -> observe -> shadow -> trial -> retained | rejected | inconclusive
```

- Collect 20 complete matching observations before shadow mode.
- Require 10 schema-valid, authority-safe shadow proposals before trial.
- Randomly preassign baseline/candidate in 1:1 persisted blocks; the model cannot
  select which encounters test its proposal.
- Evaluate after 30 complete comparable encounters per arm. Promote only if the
  primary protection metric improves by at least 10% relatively, its 95% bootstrap
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
