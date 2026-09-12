# Review repairs R1/R2/R3 - 2026-09-11

Session: local-Qwen park-head (standalone local-head mode). No park-agent
workers were used. The retrieval bridge was unavailable (embedding sync paused
at operator direction around 22:05 CT), so bounded direct reads were the
declared fallback; no indexing was performed in this session, so there are no
embedding/derived-write counts to report. Hosted Codex did not participate in
the repair work; hosted re-review is the next gate.

Review under repair: docs/bots/review-2026-09-11.md (decision: revisions
required before acceptance). All three P1 findings were verified in source
before repair: PlayerBotMgr.cpp SHA-256
9686947A7CCBDCC1F4269AF9795AC4660ED347B397E12C649CD5D91EE50600C5 matched the
review fingerprint and the line citations were accurate at repair time (line
numbers shifted afterwards as the code changed).

## R1 - Orphan account identity is now reserved (finding 2, TW-010 / KAP-553)

- Where: PlayerBotMgr::AllocateReservedBotAccount (src/game/PlayerBots/PlayerBotMgr.cpp).
- What: the allocator now takes the max of the reserved base,
  MAX(account.id)+1, MAX(bot_ownership.account_id)+1, and NEW
  MAX(characters.account WHERE account >= 1e9)+1. A reserved owner already
  present in characters without a binding (the orphan state left by an
  interrupted provisioning run) can no longer be handed to a fresh bot.
- Why it matters: distinct session identity per bot; prevents later orphan
  completion from colliding with the unique bot_ownership account constraint
  (uq_bot_ownership_account).
- Evidence: BotProvisionReviewRepairTests (docker/test_bot_provision.py): seed
  an unbound orphan under a reserved account, then provision a fresh bot - the
  two get DISTINCT accounts; on orphan resume the orphan keeps its original
  account/guid and the fresh bot is untouched. ReviewRepair 4/4 OK (101s).

## R2 - Every inconsistent binding fails closed (finding 3, TW-010 / KAP-553)

- Where: PlayerBotMgr::ProvisionPersistentBot (same file).
- What:
  - Binding + roster are read BEFORE any write.
  - ANY existing binding is validated regardless of roster state:
    bound==owner, owner >= 1e9, provision_version==2. A mismatch logs
    "inconsistent binding ... rejected, nothing modified (fail closed)" and
    returns with zero writes.
  - A consistent binding with a missing roster row completes the roster only.
  - Fresh create (characters + binding + roster) and orphan completion
    (binding + roster) each run in ONE transaction, with per-statement
    PExecute result checks and RollbackTransaction on failure - addressing the
    review note that character insertion sat outside the transaction.
- Evidence: BotProvisionFailClosedTests: three world starts against poisoned
  fixtures - (a) mismatched binding, no roster row; (b) mismatched binding
  WITH a roster row; (c) provision_version=1. Pre/post DB snapshots of
  characters / bot_ownership / playerbot are identical in all three cases and
  the rejection marker is present in the world log. FailClosed 4/4 OK (86s).

## R3 - Supported login paths preserved (finding 1, TW-006/KAP-549 + TW-009/KAP-552)

- Where: PlayerBotMgr::AddBot overloads + a Load() probe.
- What:
  - The C2 no-stacking guard now applies to PRE-EXISTING in-memory entries
    only; a freshly created temporary entry has no prior session and cannot
    stack a login on itself.
  - Fresh temporary entries are created OFFLINE and flip to LOADING only after
    every guard passes (previously the entry was constructed in LOADING and
    then rejected by its own just-set state).
  - Custom AI bots are exempt from the character-exists ownership check: their
    character is created later by the AI under the entry's synthetic account
    (the upstream auto-testing contract), so no character row exists at queue
    time. Persistent/roster bots keep the full ownership check.
  - Rejected fresh entries are cleaned up (map erase + deletes), so a failed
    login leaves no stale entry that would block a later legitimate login.
  - AddBot(PlayerBotAI*) now returns the delegated login result instead of a
    hardcoded true.
  - Probe: new OPTIONAL config PlayerBot.TestLogin (PLAYERBOT_TEST_LOGIN env,
    default empty; rendered by docker/server.py - personal server unaffected).
    At Load, each comma-separated guid is passed to AddBot twice and logged as
    "test-login <guid> first=<d> second=<d> (R3 probe)".
- Evidence: in-world probe test with PLAYERBOT_TEST_LOGIN='500010,509999':
  "first=1 second=0" for both guids and a real login queued exactly once; a
  nonexistent guid logged first=0 second=0 (no crash, no entry). The existing
  TW-006 (4/4) and TW-009 (4/4) suites are green within the full run.

## Validation evidence

- Build: image tortoise-local:dev rebuilt from this worktree, BUILD_JOBS=2,
  BUILD EXIT=0 (local/build-tw010r.log).
- Standalone: Idempotent 3/3, Resume 3/3, ReviewRepair 4/4 (101s),
  FailClosed 4/4 (86s).
- Full suite: python -m unittest discover -s docker -p 'test_*.py' -v ->
  "Ran 42 tests in 715.948s" + OK, exit 0 (local/test-tw010r-full.log).
- Gates: marker audit TOTAL=56 BAD=1 (known ForkBannerCandidates.inc);
  ADR D3 intact (docs/bots/engine-decision.md:39); candidate pin
  deada5f33018d2bbddf21d07601e89c5469a5dcd intact (docs/bots/feasibility.md:86);
  git diff --check exit 0 (benign LF->CRLF warnings on patched files);
  py_compile docker/server.py clean.
- Jira result comments posted (local park-head, statuses left as Done, no
  transitions): KAP-553 comment 23889 (R1+R2), KAP-552 comment 23890 (R3),
  KAP-549 comment 23891 (cross-reference: the regressed guards came from
  TW-006). The Atlassian MCP exposes no comment-read tool, so writes were
  verified via the server-echoed created comment objects (id, created, author,
  body) rather than a separate read-back.

## Remaining queue (P2) - for the next head, in recommended order

1. Finding 4 (KAP-553) - native-creation departure: needs an explicit design
   decision before TW-011. Options: (a) document the minimal L10 insert +
   load-repair as the sanctioned provisioning contract in
   persistence-contract.md (the current implementation; legal non-copied
   values, no account row per C5), or (b) implement native account/character
   creation and prove equivalence (starting inventory, action setup). Until
   resolved, provisioning output should be treated as disposable-fixture
   grade, not normal-playgrade.
2. R6 (KAP-546 / TW-003) - telemetry consumer blocking: bounded asynchronous
   emission with a drop policy at WorldRunnable.cpp:107-116 /
   PerformanceMonitor.cpp:262 (synchronous sLog.outInfo takes the Log shared
   lock and flushes stdout/file, so a blocked consumer can stall the world
   thread); needs a consumer-failure test.
3. R5 (KAP-550 / TW-007) - save-boundary test: feasible in the lab - change
   ownership via db_exec while a bot session is active, trigger logout, and
   assert character ownership and gameplay state remain unchanged.
4. R4 (KAP-552 / TW-009) - stale-completion test: no in-fork runtime trigger
   exists today (AddBot is never re-invoked for the same guid with a live
   session; sessions are in-memory), so a test hook or source-level review is
   required; TW-011 restart/restore remains the integration proof.

## State at pause (2026-09-11, ~23:00 CT)

- Branch personal-server, HEAD 8a7f25a; ALL repair work is UNCOMMITTED
  (src/game/PlayerBots/PlayerBotMgr.{h,cpp}, docker/server.py,
  docker/test_bot_provision.py, this doc, progress entries).
- Embedding sync remains PAUSED per operator direction (durable state at
  pause: 1881/3209 files, 8955 chunks, last_sync_at NULL; the frontier was in
  sql/, so none of the repaired src/ files were indexed). Resume procedure:
  local/retrieval-sync-resume.txt. Resuming after the work is committed will
  index the repaired code.
- Personal server containers untouched; no live migration; no secrets sent
  anywhere.
- Next: hosted Codex re-review of R1/R2/R3; then the P2 queue above before
  TW-011 (KAP-554, earned-state persistence across restart/restore).
