# BL-003 repair worker evidence card v1

Date: 2026-09-22
Repository: `C:\Users\hpark\WebstormProjects\tortoise-wow`
Branch: `feature/kap-558-port-phase2`
Baseline commit: `c5781974b444f6ad692b48a516b0994d8cf59bc5`

## Objective

Repair the existing, unaccepted BL-003 implementation so it actually persists each
completed BL-002 encounter exactly once through a bounded, thread-safe asynchronous
store. Preserve combat choices and keep learned behavior disabled.

## Hosted-head findings that must be resolved

1. `TryPersistLearningSummary` is declared but not implemented or called. Every path
   that completes an encounter must persist the completed old summary before reset or
   replacement: target death, companion death, owner override, target replacement,
   invalid target, and leash expiry.
2. `Store::Enqueue` is declared `bool` but defined `void`. It must return success and
   assign a nonzero process nonce plus a monotonically increasing store sequence inside
   the store before queueing. PlayerBotAI must not manufacture delivery IDs.
3. The DB callback currently mutates `SummaryFifo` from the DB thread. The FIFO is
   world-thread-only. The callback may only publish bounded atomic completion status;
   `Pump` applies success/failure and disables the FIFO on the world thread.
4. Check and handle `PExecuteCallback` submission failure. At most one write may be in
   flight. Do not overwrite completion state or silently submit a second request.
5. Avoid callback use-after-free during shutdown. Use bounded lifetime-safe callback
   state (for example a shared completion state) or another demonstrably safe design;
   do not claim singleton lifetime without proving destruction order.
6. `WorldTimer::getMSTime()` is not a process-unique nonce. Generate a robust nonzero
   64-bit process nonce without secrets or persistence. Startup must call the async
   stale-unfinished-row repair. That repair must cover rows from earlier processes,
   mark an explicit interrupted end reason, and not race the encounter writer.
7. `SummaryFifo::EnqueueCount` never increments. Fix counters and tests. Remove the
   leaking `*new Summary` test pattern. Test actual store delivery sequencing and
   completion/failure state where feasible, not a detached simulation presented as
   store coverage.
8. Preserve full 64-bit target GUID if persisted, or deliberately remove it from the
   summary contract. Do not silently truncate it. Check `snprintf` truncation before SQL
   submission.
9. Retention SQL must be part of the product contract, not exist only inside tests.
   Define explicit bounded pruning SQL/adapter behavior that protects pending candidate
   evidence, or fail closed/paused when safe pruning is impossible. Tests must verify
   protected evidence, not only the no-candidate case.
10. Keep profile gating, duplicate delivery idempotence, CAS/rollback/audit lifecycle,
    disposable-volume safety, and no behavior changes.

## Allowed reads and edits

Read and edit only the existing BL-003 allowed paths listed in
`docs/prd/bram-party-learning-bl003-worker-card.md`, plus this repair card. Do not
inspect other files or directories. Do not edit the PRD/handoff, commit, stage, push,
deploy, start the personal realm, access secrets/private data, call retrieval, or
launch another model session.

## Validation

Run the two focused test runners and `git diff --check`. Do not run the full world
build. Report exact failures and remaining uncertainties; do not declare acceptance.
