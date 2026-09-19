# One useful Turtle companion: hosted-governed port queue

## Phase handoffs (required at each session stop and phase exit)

- [Phase 1: deterministic companion and current checkpoint](phase-1-handoff.md)
- [Phase 2: personality and model integration](phase-2-handoff.md)
- [Phase 3: populated world and recruitment](phase-3-handoff.md)
- [PORT-033 draft: provision a bounded owned companion party](port-033.md)
- [PORT-034: companion cohort provisioning and level-appropriate skills](port-034.md)
- [Phase 4: learning, progression and social behavior](phase-4-handoff.md)

Each handoff records code entry points, invariants, acceptance gates, evidence,
known gaps and the next bounded assignment. Planned phases are not implementation
claims. Follow the handoff update contract before transferring work to another session.

Jira API review and scope mapping: [KAP-543 reconciliation](jira-reconciliation.md).
Local passing evidence and Jira delivery status are tracked separately.

This is the current entry point. The old README status sections are historical.
The machine queue remains ../prd.json. Retrieval/embedding maintenance is
operator-paused; use bounded current-source evidence and report no sync counts.
Tracking story: [KAP-558](https://parkenstein.atlassian.net/browse/KAP-558),
created in To Do with Gherkin acceptance scenarios and parent KAP-543 submitted.
The connector omits parent in readback, so parent linkage is not independently verified.
PORT cards remain bounded local work units under this single tracking story;
they have not been created as individual Jira issues.

## Execution order

- [PORT-001: Pin upstream behavior and audit reuse](port-001.md) — followed by existing CMP-010 party review.
- [PORT-002: Keep dismissed companions benched](port-002.md)
- [PORT-003: Separate companion follow from combat priorities](port-003.md)
- [PORT-004: Add explicit hold and cancel semantics](port-004.md)
- [PORT-005: Assist one owner-selected hostile target](port-005.md)
- [PORT-006: Defend the owner without additional pulls](port-006.md)
- [PORT-007: Finish combat with bounded loot and regroup](port-007.md)
- [PORT-008: Bound unreachable-target and owner-loss recovery](port-008.md)
- [PORT-009: Support one normal companion death and recovery path](port-009.md)
- [PORT-010: Qualify one useful companion and review commits](port-010.md)

Phase 2 begins only after the PORT-010 acceptance record is reconciled. Its
canonical execution order is:

- [PORT-011: Preserve landed targeting/fallback and close cast-result telemetry gaps](port-011.md)
- [PORT-012: Extract the reviewed combat executor and resolve capability gaps](port-012.md)
- [PORT-013: Extract the reviewed typed-intent boundary and add pure-policy coverage](port-013.md)
- [PORT-014: Implement one legal tank threat policy](port-014.md)
- [PORT-015: Implement one legal healer triage policy](port-015.md)
- [PORT-016: Implement damage assist and pull discipline](port-016.md)
- [PORT-017: Specify the planner protocol and fake-service contract](port-017.md)
- [PORT-018: Add bounded nonblocking shared party-planner transport](port-018.md)
- [PORT-019: Add bounded personality preferences and expression](port-019.md)
- [PORT-020: Persist minimal versioned personality state](port-020.md)
- [PORT-021: Connect the real local model through the accepted adapter](port-021.md)
- [PORT-022: Add bounded conversational party chat](port-022.md)
- [PORT-023: Complete one cooperative quest with the owner](port-023.md)
- [PORT-024: Progress companion equipment from earned loot](port-024.md)
- [PORT-025: Report bag pressure and sell protected junk at a vendor](port-025.md)
- [PORT-026: Qualify Phase 2 conversation, cooperation, progression and personality](port-026.md)

PORT-001 precedes CMP-010; then PORT-002 through PORT-010 run sequentially.
CMP-011 is a hosted roll-up, not another implementation assignment.
PORT-011 through PORT-026 are the Phase 2 decomposition. The old CMP-012,
CMP-013, CMP-014 and LLM-010 cards become roll-up acceptance references for
PORT-014, PORT-015, PORT-016 and PORT-017..018 respectively; do not dispatch
both versions as duplicate implementation work. General autonomous questing,
population scale and learned behavior remain later phases; Phase 2 contains one
bounded cooperative quest only. The prior follow/stop MVP
remains historical evidence; it is not acceptance of this useful-companion
milestone.

## Worker and commit governance

1. Hosted Codex inventories the current branch, dirty files and active workers.
   Existing party edits and docker/test_bot_party.py belong to the user. Establish
   a reviewed baseline or an isolated worktree before any overlapping assignment.
2. Hosted Codex verifies exact local and upstream sources, fills ../worker-card.md
   with hashes, selected functions, explicit edit scope, prerequisites, test
   commands and expected output. Story cards alone are not dispatch permission.
3. Launch one fresh serialized park-agent exec assignment. No nested models.
   Workers propose patches and evidence; they do not commit, stage, push, deploy,
   edit passes flags, change Jira, or accept work. If a pre-existing worker commit
   is supplied, review that exact commit and its parent instead of trusting its title.
4. Hosted Codex independently reviews scope, cumulative diff, upstream provenance,
   ownership/cancellation behavior and relevant tests against the actual changed
   source/image. Record failures as a narrow repair card in a fresh session.
5. Only after acceptance, hosted Codex may make one story-scoped commit when
   committing is authorized. Record parent/result commit IDs, source/image IDs,
   checks run or skipped, uncertainties and acceptance in progress.txt and prd.json.
   Never stage unrelated user changes. No automatic push or personal deployment.
6. Final PORT-010 uses deterministic gates first, then one fresh read-only local
   QA assignment and actual client observations. Missing human gameplay evidence
   stays pending. Existing Jira pipeline rules apply only if that pipeline is used.

If `park-agent` is blocked by another active local session, the shared-model
mutex, or the approved launcher, report the exact fallback reason. Do not weaken
machine policy or run concurrent local-model requests. Hosted Codex can continue
bounded direct review while local execution is unavailable.

## Shared validation gates

- C++: compile the server with docker compose build world, preserving the existing
  BUILD_JOBS and ALLOW_TURTLE_ADDONS setting. A build does not prove behavior.
- Each behavior: explicitly separate port-free disposable Compose project, isolated
  fixture characters, matching validated assets and a source-matched candidate image.
  Record exact supported class/level, target, time bounds and normal game APIs.
- Changed Python: compile and run its focused tests. Shared fixture edits additionally
  require affected regressions. Changed shell/PowerShell: relevant parser checks.
- Changed Compose/config: docker compose config --quiet. All patches: git diff --check.
- No personal-volume changes, live restart, external writes, broad logs, secrets or
  private character data in worker prompts. Raw runtime evidence stays local; the
  head supplies only minimal sanitized synthetic-fixture facts where needed.
- If an implementation requires more than its listed files or a new gameplay
  policy, return that dependency to the head; split it instead of widening the patch.

## Required modularity contract

This is a requirement of KAP-558's local execution plan, not a claim that the
current engine already has these boundaries. PORT-001 proposes concrete interfaces
and exact paths; hosted Codex owns their approval. PORT-003 implements the minimum
separation needed for follow and existing combat.

- Lifecycle owns identity, sessions, ownership, save and bench/recall.
- Observations expose bounded immutable game-state snapshots; policies do not
  retain unsafe pointers or perform arbitrary database queries.
- Behavior policies consume observations and propose typed intents. Follow,
  combat roles and future questing remain independently extensible.
- Action selection resolves priorities, interruptions and valid goal resumption.
- Turtle execution revalidates intents against authoritative live state and uses
  normal movement, spell, loot and quest APIs. Policies do not bypass it.
- A future personality/planner adapter proposes schema-validated goals or bounded
  preferences. Prompt text never becomes executable commands. Model timeout,
  rejection or absence leaves deterministic play operational.

Dependencies flow from observations to policies to selection to execution.
Lifecycle provides identity/context; behavior modules do not own or mutate its
persistence internals. Concrete APIs, ownership/lifetime rules, execution context,
cancellation generations and registration points must be documented by PORT-001.

Implement compiled C++ components, not a dynamic plugin loader. Do not add generic
frameworks, LLM transport, new role tactics or quest automation in this slice.
Future tank/healer/DPS policies share legal actions; future quest acceptance uses
a validated quest action; personality cannot override ownership or game rules.
New policies may require new action handlers but must not require rewriting login
or save logic.

Acceptance: demonstrate separate follow and existing-combat policies, combat
interrupting follow, valid follow resumption, and rejection of stale resumption.
Adding the second policy must leave identity/login/save/bench code unchanged.
Use behavioral tests, not tests that merely assert class names or file layout.
If extraction exceeds one bounded assignment, hosted Codex splits it before dispatch.

## Upstream reference

See [source map](upstream-map.md). PORT-001 must approve individual fragments
before copying. Existence of a file is not proof of compatibility.
