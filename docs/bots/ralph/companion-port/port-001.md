# PORT-001: Pin upstream behavior and audit reuse

- Depends on: none
- Status: pending hosted dispatch; passes=false
- Jira: not created; local story under KAP-543
- Shared contract: [execution and review](README.md)

## Objective

Read-only analysis of maintained mod-playerbots follow, assist, defend and action-priority implementations. The hosted head supplies a pinned revision and bounded source files; do not use the old azerothcore/mod-playerbots repository.

## Allowed edit candidates

- `docs/bots/port-provenance.md`

The hosted head narrows these to exact functions and approved new paths in a fresh hashed evidence card. These paths are a ceiling, not permission to read entire directories. Shared fixture changes require an explicitly revised card.

## Acceptance

Record repository URL, immutable commit, exact file/symbol ranges and hashes, file-level license notices, dependencies and Turtle API mappings for each proposed reuse. Distinguish copied/adapted code from behavior-only reimplementation.

## Modular architecture acceptance

Scenario: Approve modular behavior boundaries
Given the current Turtle bot engine and pinned upstream evidence
When PORT-001 proposes the minimal architecture
Then lifecycle and persistence, immutable observations, behavior policies, action selection, and Turtle action execution have explicit responsibilities and dependency directions
And tank/healer/DPS and quest policies can propose typed intents without owning sessions or database writes
And an optional personality/planner adapter can only propose bounded typed goals or preferences and cannot execute game actions
And hosted Codex approves exact interfaces and file paths before implementation.

Propose the concrete interfaces and approved file list in
`docs/bots/companion-architecture.md`. Follow the README modularity contract.
This document is an additional allowed edit candidate. The worker proposes;
hosted Codex decides architecture. No runtime framework is implemented here.

## Failure cases

If license compatibility or a dependency is unclear, label that fragment blocked and propose a smaller alternative; do not copy runtime code or choose architecture.

## Validation

Head reviews the provenance matrix and explicitly approves the chosen minimal boundary; no server build for this documentation spike.

Apply the shared gates for changed file types. Report unexecuted checks explicitly. Return the diff, precise upstream provenance where used, tests and uncertainties; do not mark this story accepted.
