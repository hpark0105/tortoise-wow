# PORT-010: Qualify one useful companion and review commits

- Depends on: PORT-009
- Status: pending hosted dispatch; passes=false
- Jira: not created; local story under KAP-543
- Shared contract: [execution and review](README.md)

## Objective

Hosted acceptance gate, not autonomous worker implementation. Use one fresh read-only QA worker after deterministic checks; the human/hosted head supplies in-game observations.

## Allowed edit candidates

- `docs/bots/companion-port-acceptance.md`

The hosted head narrows these to exact functions and approved new paths in a fresh hashed evidence card. These paths are a ceiling, not permission to read entire directories. Shared fixture changes require an explicitly revised card.

## Acceptance

Review each accepted commit and cumulative diff; run relevant deterministic regressions, then observe recruit/follow, assist/defend, loot/regroup, declared death recovery, dismiss/recall, and saved state across relog/restart.

## Failure cases

Missing client evidence remains pending, never inferred from source tests. Failures become bounded repair assignments; QA never edits source. Clearly list unsupported dungeon/transport/class behaviors.

## Validation

Head records commit and image IDs, tests actually run, skipped checks and in-game evidence. No live rollout, push or Jira transition implied.

Apply the shared gates for changed file types. Report unexecuted checks explicitly. Return the diff, precise upstream provenance where used, tests and uncertainties; do not mark this story accepted.

