# Hosted repair re-review - 2026-09-11

Scope: independently review the local-head R1/R2/R3 repairs, reproduce remaining
failure cases, and set the next gate before TW-011. This supersedes the original
P1 findings only to the extent of the evidence below; it is not broad gameplay
or story acceptance.

## Findings and correction

- R1: the allocator now includes reserved owners in `characters`, including
  unbound orphans. The targeted tests cover fresh creation followed by orphan
  completion without identity reuse.
- R2: inconsistent existing bindings are checked before roster writes, with or
  without a roster row. Fresh and orphan writes now share transaction blocks,
  but the mixed storage engines do not provide full atomicity (see below).
- Remaining R2 error reproduced: `PExecute` queues statements during a
  transaction; it does not report the eventual SQL result. Ignoring
  `CommitTransaction` allowed a failed transaction to log success. In disposable
  project `tortoise-bot-prov-830a5a6b1b73`, an orphan whose account was already
  bound elsewhere retained zero bindings/roster rows after the SQL error, yet
  logged `completed (existing character)`. Data rollback worked; reporting was
  false. Evidence: `local/tortoise-bot-prov-830a5a6b1b73/result.json`.
- Hosted fix: check transaction creation and synchronous commit results, and
  report success only after successful execution. No change to SQL transaction
  internals. New `docker/test_bot_provision_commit.py` injects an actual database
  failure using a disposable trigger and tests partial-state safety, failure
  reporting, and successful retry for the same orphan identity.
- Storage-engine discovery: the first hosted run failed its fresh-character
  rollback assertion. The bundled `characters` and `playerbot` tables are
  MyISAM, while `bot_ownership` is InnoDB. A binding insert failure leaves an
  unbound character. C6 permits this resumable state, so the regression now
  checks absence of binding/roster, accurate failure reporting, and successful
  retry preserving both GUID and account. This does not prove atomicity or
  recovery from every possible commit failure. Native creation must explicitly
  handle partial persistence; wrapping native saves in a transaction alone
  cannot satisfy that gate.
- R3: fresh temporary entries start OFFLINE, persistent ownership checks remain,
  custom creation bypasses the premature character-exists check, and the
  custom overload returns the delegated result. The current runtime probe
  tests `AddBot(guid)`; it does **not** directly exercise `AddBot(PlayerBotAI*)`,
  custom creation, or ephemeral no-save through logout. That coverage remains
  open. The custom wrapper also leaves its allocated OFFLINE entry in the map
  if delegated login fails; its failure ownership/cleanup needs a direct test.

## Evidence and limits

- Local-head full-suite log was inspected: 42 tests, 715.948 seconds, OK. This is
  inherited evidence, not a hosted rerun of all 42 tests.
- Hosted Docker build: `tortoise-local:review`, BUILD_JOBS=2 (matching the latest
  local-head cache), ALLOW_TURTLE_ADDONS=ON retained. Explicit exit 0 in
  `local/build-repair-head-confirm.log`. This image was used only in labs.
- Hosted fast checks: 11/11 (baseline 5, telemetry configuration 4, DBC helpers 2).
  Python compilation and `git diff --check` pass.
- Hosted original repair regressions: 8/8 passed in the combined run at
  `local/test-head-repairs.log`. That run ended with one failed assertion out of
  11 tests (251.625 seconds): it incorrectly expected MyISAM character rollback.
  The corrected four-test partial-state/retry run is pending completion at
  `local/test-head-commit-retry.log`. Do not treat a pending run as a pass.
- Reviewed/fixed PlayerBotMgr.cpp SHA256:
  `F5543132185359DAB0C90361B54B283E39A1D34068F64562361716F114E14FBA`.

## Local worker and retrieval

Retrieval list/status succeeded: service healthy, repository registered, index
stale/incomplete (1882 files, 8988 chunks, no completed last sync). Sync remains
paused per operator instruction. Bounded current-source reads were used; no
embedding or derived writes were requested, and no sync counts are claimed.

One fresh serialized read-only park-agent assignment used
`local/repair-review-card-v2.md`, containing source hashes and a bounded scope.
It produced a report independently identifying the ignored commit result and
custom-AI test gap. The wrapper returned exit 1 despite producing the report;
its trace includes a PowerShell `2>nul` error. Its report is corroborating
analysis, not a successful deterministic gate. The worker also read a few
adjacent database declarations outside the card's explicit file list; no
source edits or acceptance authority were delegated. Hosted source inspection
and the disposable runtime reproduction establish the finding independently.

## Next gate

The hosted decision in `persistence-contract.md` requires native character
initialization before TW-011. The raw level-10 SQL insert stays fixture-grade.
The decision preserves stable synthetic accounts and ephemeral no-save, with
explicit native-state equivalence and failure/retry Gherkin scenarios.

Then complete R6 asynchronous telemetry, R5 save-boundary coverage, and R4
stale-completion coverage. For R6, moving `sLog.outInfo` to another thread is
insufficient by itself: shared stdout/file stream locks can still couple its
blocked consumer to world-thread logging. Use an isolated nonblocking or
bounded sink, bound queue memory and shutdown, expose drops, and inject a
blocked consumer while checking world progress. No 500-bot capacity or companion
play-quality claim is justified yet.

No Jira statuses/comments changed in this hosted session. Nothing committed,
no personal server deployment, and no personal volume mutation.
