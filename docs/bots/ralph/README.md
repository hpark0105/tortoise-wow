# Living-world local-LLM handoff

Updated 2026-09-11. Start here after a new session or context reset.

**Current review:** read [the native initialization result](../native-initialization-2026-09-12.md)
and [the hosted repair re-review](../review-2026-09-11-hosted-recheck.md).
Native creation and bounded partial-write recovery now pass disposable labs.
Custom-AI runtime
coverage and the remaining P2 queue are still open. Earlier implementation and
Jira completion claims below/in `progress.txt` are history, not acceptance.

## Outcome and requirements

Build a personal Turtle 1.18.1 build 7272 world with a 500+ persistent bot roster,
substantial concurrent gameplay and real activity in regions without a human.
Keep a measured 500-active benchmark; parked database identities do not count.
Provide 2–5 personal companions, with at most four accompanying the human in a
normal party. Benched companions retain state without unrelated offline progress.

Gameplay controllers handle movement, combat, roles and recovery. The optional
local LLM helps the party plan and interpret requests. It must not run every
combat tick, control all 500 bots through inference, issue arbitrary commands or
block the world thread. Preserve one shared model lease; busy/offline inference
leaves deterministic play working. No paid fallback, auction automation, GM
advantages or replacement of earned companion gear.

Full requirements and technical design: [product PRD](../../prd/living-world.prd).
Current source findings and measurements: [feasibility report](../feasibility.md).
Jira epic: [KAP-543](https://parkenstein.atlassian.net/browse/KAP-543).

## Where we stand

- Working personal fork: https://github.com/hpark0105/tortoise-wow, branch
  `personal-server`, Windows workspace
  `C:/Users/hpark/WebstormProjects/tortoise-wow`.
- Game server runs in Docker with persistent local data; prior human-character
  restart/restore checks passed. Bots remain disabled.
- Current pass added a repeatable resource sampler and a generated, port-free
  disposable database/world lab. Fresh boot, sampling and cleanup passed.
- Two-minute idle live baseline: world CPU mean 9.49% of one logical CPU,
  memory 1.163 GiB; no bot roster rows and zero online-character flags at both
  endpoints. This does not prove gameplay load or continuous session counts.
- The local head reported 42 passing tests; the hosted re-review records its
  separate, targeted build/test evidence and uncovered failure paths. Remote
  CI results for this handoff have not been claimed.
- Persistent ownership/save guards and timer/null fixes now exist in the
  uncommitted worktree. Native creation, save-boundary failure coverage, and
  earned-state restart/restore remain gates. MyISAM character/roster tables
  mean transaction blocks alone do not guarantee atomic provisioning.
- Population counts and companion role/recovery/quest behavior remain
  unverified for the intended living-world requirements.
- Candidate pinned to `deada5f33018d2bbddf21d07601e89c5469a5dcd` in
  `local/bot-engine-candidate`. It requires host hooks and additional
  dependencies. It is not installed on the personal realm.
- Candidate server CMake configuration passed. Full architecture-test
  configuration failed on missing `ForkBannerCandidates.inc` extraction
  marker; the four intended tests did not run.
- The separate candidate server compile finished with a gSOAP/glibc link
  incompatibility (`__isoc23_strtol`, `__isoc23_strtoul`, `strlcpy`). No candidate
  server image or focused test-adapter pass was produced.
- No 10-bot runtime, companion, LLM integration or 500-active capacity test has
  passed yet. P0 is incomplete.
- Embedding maintenance belongs to the operator for now. Retrieval is healthy
  but its index is incomplete; no new indexing job was started. The latest
  hosted re-review obtained local-worker analysis and independently verified
  its transaction finding; see that review for the worker execution limits.
- Keep unrelated `.idea/` changes untouched.

## What we are doing now

Continue with R6 telemetry, R5 save-boundary coverage, and R4 stale-completion
coverage as described in the latest native-initialization result. TW-011 follows
those gates; the personal realm remains unchanged.
The first execution wave below ends at a small persistent cohort and one
deterministic follow/stop companion. It does not authorize a wholesale fork port
or live rollout.

An active head/operator can check the existing build log locally:

```powershell
Get-Content local/bot-candidate-server-build.log -Tail 20
```

That is an operator diagnostic, not permission to put raw logs in local-worker
prompts or Jira. The original hosted tool session is 69317; it is session-local
and a future shell may not be able to poll it. Check process/build completion
before rerunning. A finished file without a linked binary or successful Docker
build result is not a server pass.

## Ralph-style execution

`prd.json` uses the conventional Ralph fields: project, branchName,
description, userStories, id, title, acceptanceCriteria, priority, passes and
notes. It adds explicit scope, dependencies and Jira mapping.
Reference: [Ralph example](https://github.com/snarktank/ralph/blob/main/prd.json.example).
This is a task-data handoff, **not an installed Ralph runner or a claim that its
Amp/Claude launch script supports park-agent**.

`acceptance.feature` contains the same Given/When/Then scenarios. These are
acceptance specifications, not executable step definitions or passing tests.
`progress.txt` carries compact decisions and results between fresh sessions.

Execute one small dependency-ready item per fresh session. The head verifies
current source and writes a versioned evidence card with hashes before delegating.
All items start with `passes: false`; only the active head may accept evidence and
change that flag. Never treat a worker's success statement as acceptance.
Dependencies are mandatory even if a generic Ralph runner ignores extra fields.
If a story expands beyond one bounded change, split it before implementation.

For a user-started `park-head` session, the local model is the head: follow the
global retrieval-first policy and do not launch another model session.
For a `park-agent` session, it is only a worker: follow the supplied card, do not
call retrieval or Jira, do not spawn another agent, and return findings/diff/test
evidence for independent head review. An occupied shared-model lock means defer,
not bypass it.

Suggested prompt for a new **head** session:

> Read AGENTS.md, docs/bots/ralph/README.md, progress.txt and prd.json. Confirm the
> current worktree and read docs/bots/native-initialization-2026-09-12.md. Start
> with R6 bounded nonblocking telemetry as a bounded change. Cover a blocked
> consumer and continued world progress with a regression test. Use current
> verified source; preserve unrelated work and do not deploy to the live realm.
> Independently verify the result. Preserve the ownership/save
> blocker, one-model rule and operator's embedding maintenance. Do not mark a
> story complete without independently checked evidence.

A hosted head delegating to `park-agent` should use [worker-card.md](worker-card.md)
as a template, fill actual hashes and permitted files, and give that worker only
the selected task and bounded evidence. Do not paste this whole session.

## Immediate backlog

| Local ID | Jira | Small task | Dependencies |
|---|---|---|---|
| TW-001 | [KAP-544](https://parkenstein.atlassian.net/browse/KAP-544) | Diagnose the pinned candidate test-configuration mismatch | None |
| TW-002 | [KAP-545](https://parkenstein.atlassian.net/browse/KAP-545) | Record the smallest viable bot-engine integration boundary | TW-001 |
| TW-003 | [KAP-546](https://parkenstein.atlassian.net/browse/KAP-546) | Add opt-in world-processing-time measurements | None |
| TW-004 | [KAP-547](https://parkenstein.atlassian.net/browse/KAP-547) | Specify persistent bot ownership and save invariants | TW-002 |
| TW-005 | [KAP-548](https://parkenstein.atlassian.net/browse/KAP-548) | Add versioned bot-ownership metadata migrations | TW-004 |
| TW-006 | [KAP-549](https://parkenstein.atlassian.net/browse/KAP-549) | Validate persistent bot ownership before session login | TW-005 |
| TW-007 | [KAP-550](https://parkenstein.atlassian.net/browse/KAP-550) | Enable saving only for explicitly owned persistent bots | TW-006 |
| TW-008 | [KAP-551](https://parkenstein.atlassian.net/browse/KAP-551) | Repair bundled bot initialization and null-entry safety | TW-002 |
| TW-009 | [KAP-552](https://parkenstein.atlassian.net/browse/KAP-552) | Make repeated and stale bot logins lifecycle-safe | TW-006, TW-008 |
| TW-010 | [KAP-553](https://parkenstein.atlassian.net/browse/KAP-553) | Provision one persistent test bot idempotently | TW-007, TW-009 |
| TW-011 | [KAP-554](https://parkenstein.atlassian.net/browse/KAP-554) | Prove one bot's earned state survives restart and restore | TW-010 |
| TW-012 | [KAP-555](https://parkenstein.atlassian.net/browse/KAP-555) | Make bot population boundaries deterministic | TW-002, TW-009 |
| TW-013 | [KAP-556](https://parkenstein.atlassian.net/browse/KAP-556) | Validate a paced ten-bot disposable cohort | TW-011, TW-012 |
| TW-014 | [KAP-557](https://parkenstein.atlassian.net/browse/KAP-557) | Add deterministic follow and stop for one owned companion | TW-011 |

Children were created with `parent=KAP-543`; dependency IDs are in their
descriptions and the manifest. Jira's connector search currently errors and its
read response omits parent, so parent readback remains unverified. No separate
"blocks" issue links or sprint moves were made by the original handoff. Later
local work changed Jira status: the review verified KAP-546, KAP-550, KAP-552 and
KAP-553 as Done. See the review before accepting those completion claims.

## Later phases — split before execution

| Phase | Requirement | Exit evidence |
|---|---|---|
| Capable companions | Tank threat on a marked pull; healer triage; damage assist; mana/interrupt/CC rules; death and lost-leader recovery | Separate small stories per behavior and class/level; controller-only tests and observed pulls |
| Supported questing | One declared quest at a time: prerequisites, navigation, credit, loot and turn-in | Normal progression and saved state; distinguish broken content from AI defects |
| Party controls | Owner, role, invite, bench/recall and loot policy | Human plus four bots; fifth substitute; owner stop always wins |
| Independent world | Persist regional routines and activity budgets away from the human | Actual remote actions and stable identities, not arrival-triggered crowds or fabricated XP |
| Load progression | 50, 100, 250, then 500 active sessions with paced activation | Thirty-minute stages, processing p95/p99, client impact and remote activity; eight-hour qualifying soak |
| Local reasoning | Windows shared-model broker, bounded observation/action schema, coalesced party requests | One lease, timeout/cancellation/stale-plan tests and 30-minute model-offline gameplay |
| Knowledge/memory | Scoped game facts plus bounded preferences; separate from code index | Provenance, save/restore reconciliation, useful operation without embeddings |
| Added intelligence | Compare controller-only against LLM-assisted planning | At least 20 paired scenarios showing improved outcomes without more avoidable wipes |
| Rollout | Independent flags and tested backup/rollback | Head-reviewed evidence and explicit maintenance scope before personal deployment |

## Validation and boundaries

Use relevant repo checks, not every expensive test on every edit:
`python -m unittest discover -s docker -p 'test_*.py'`, Python compilation,
`git diff --check`, Compose validation, and a server compile for C++ changes.
Game-state changes require disposable runtime and persistence fixtures. Keep
`-DALLOW_TURTLE_ADDONS=ON`, validated maps/DBC assets and normal game rules.

Never delete/reset the personal `tortoise-local_database` volume or import
fixtures into it. Do not expose credentials, environment maps, raw logs, chat
history or character backups in retrieval, Jira or worker evidence. Real gameplay
data stays local. Preserve current server availability during this wave.

After embedding maintenance, the active head should confirm retrieval health,
sync the changed registered repository and report real embedding/derived-write
counts. Do not keep starting full indexing jobs while the operator is fixing it.

For stories using the team pipeline, follow the global session-driven QA rule:
hosted head moves READY FOR QA to IN QA in the same session, runs deterministic
gates first, then exactly one fresh serialized read-only local QA assignment.
Only the head records pass/fail and moves the issue onward; a QA worker does not
fix source. This handoff has not started that workflow.
