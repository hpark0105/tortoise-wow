Feature: First living-world implementation wave
  Build safe persistent bots before scaling or adding model planning.

  @TW-001
  Scenario: Diagnose the pinned candidate test-configuration mismatch
  Given a pinned candidate and a head-provided build status
  When the regression extraction marker is compared with its current source
  Then the report identifies the missing contract and distinguishes unexecuted tests from failed tests

  Scenario: Reject or handle the failure path
  Given a proposed repair would change the meaning of the tested behavior
  When this condition is detected
  Then the worker records that uncertainty rather than deleting the assertion to obtain a pass

  @TW-002
  Scenario: Record the smallest viable bot-engine integration boundary
  Given the bundled-engine blockers and pinned candidate findings
  When the head reviews the proposed execution-engine boundary
  Then an ADR lists selected reuse, required host hooks, excluded changes and unresolved runtime proof

  Scenario: Reject or handle the failure path
  Given a dependency requires unrelated core scheduling or content changes
  When this condition is detected
  Then it is separated for review rather than silently importing the whole fork

  @TW-003
  Scenario: Add opt-in world-processing-time measurements
  Given a disposable zero-bot world with measurements enabled
  When world updates and intentional frame sleeps run
  Then bounded telemetry reports processing-time counts and p95/p99 separately from elapsed tick intervals

  Scenario: Reject or handle the failure path
  Given telemetry is disabled or its output consumer is unavailable
  When this condition is detected
  Then the world remains nonblocking and retains bounded memory

  @TW-004
  Scenario: Specify persistent bot ownership and save invariants
  Given persistent and ephemeral bots have different intended lifetimes
  When identity, login, save and logout paths are specified
  Then the contract binds a persistent bot to a real owned account and character while preserving ephemeral no-save behavior

  Scenario: Reject or handle the failure path
  Given session account identity disagrees with character ownership
  When this condition is detected
  Then the contract rejects login/save and never reassigns a human character

  @TW-005
  Scenario: Add versioned bot-ownership metadata migrations
  Given a disposable character database and an approved identity contract
  When the ownership metadata migration is applied and the updater is rerun
  Then stable bot identity, ownership, type and provisioning version are retained without duplicate records

  Scenario: Reject or handle the failure path
  Given a metadata row references an unowned or incompatible character
  When this condition is detected
  Then validation reports the mismatch without changing human records

  @TW-006
  Scenario: Validate persistent bot ownership before session login
  Given a registered persistent bot and matching real account/character ownership
  When the bot requests a session
  Then login uses the approved account identity and preserves the character's stored owner

  Scenario: Reject or handle the failure path
  Given ownership is mismatched or a conflicting session exists
  When this condition is detected
  Then login is rejected without replacing the human session or rewriting ownership

  @TW-007
  Scenario: Enable saving only for explicitly owned persistent bots
  Given an authenticated bot session satisfies the approved persistent ownership contract
  When normal character save executes
  Then earned state is written under the existing real owner through normal save paths

  Scenario: Reject or handle the failure path
  Given the bot is ephemeral or ownership validation fails
  When this condition is detected
  Then the existing no-save protection remains effective and unrelated human records are unchanged

  @TW-008
  Scenario: Repair bundled bot initialization and null-entry safety
  Given a newly constructed or detached bundled bot controller
  When its first update executes
  Then all decision timers have deterministic initialization and a null player is checked before dereference

  Scenario: Reject or handle the failure path
  Given a valid player is completing a teleport
  When this condition is detected
  Then required acknowledgement handling remains reachable and correct

  @TW-009
  Scenario: Make repeated and stale bot logins lifecycle-safe
  Given one owned bot is loading or online
  When the same add request is repeated
  Then one session and one matching state transition exist with accurate loading/online counters

  Scenario: Reject or handle the failure path
  Given an old asynchronous completion arrives after cancellation and retry
  When this condition is detected
  Then its generation is rejected and the current session is preserved

  @TW-010
  Scenario: Provision one persistent test bot idempotently
  Given an empty disposable lab and one requested bot identity
  When provisioning runs twice
  Then exactly one valid owned account and character exist with resumable provisioning status

  Scenario: Reject or handle the failure path
  Given the first run stops between account and character creation
  When this condition is detected
  Then retry completes the same identity without deleting unrelated records

  @TW-011
  Scenario: Prove one bot's earned state survives restart and restore
  Given one lab bot has legitimately earned XP, an item and a supported quest update
  When the world restarts and a backup is restored into a second disposable project
  Then the same bot identity, owner and earned records match the pre-restart evidence

  Scenario: Reject or handle the failure path
  Given the save or backup is incomplete
  When this condition is detected
  Then the check fails with bounded evidence and cannot overwrite the personal volume

  @TW-012
  Scenario: Make bot population boundaries deterministic
  Given roster sizes of zero, one and ten with explicit min/max targets
  When the director reconciles requested population
  Then exact target boundaries are respected and loading, online and offline counts remain distinct

  Scenario: Reject or handle the failure path
  Given the requested target exceeds valid roster capacity or a login fails
  When this condition is detected
  Then actual capacity is reported without underflow, a fake online count or an unbounded retry

  @TW-013
  Scenario: Validate a paced ten-bot disposable cohort
  Given ten valid persistent bot identities in a disposable project
  When a bounded login schedule activates the cohort
  Then ten distinct successful sessions are observed with saved ownership intact and recorded resource/tick measurements

  Scenario: Reject or handle the failure path
  Given a bot cannot load or resource thresholds are exceeded
  When this condition is detected
  Then new logins back off and the report identifies the achieved count without repeatedly recreating identities

  @TW-014
  Scenario: Add deterministic follow and stop for one owned companion
  Given one owned companion in the human's test party and no LLM service
  When the owner issues follow and then stop
  Then normal pathfinding follows the leader and stop invalidates the current goal immediately

  Scenario: Reject or handle the failure path
  Given a different player commands the bot or an old goal is delivered after stop
  When this condition is detected
  Then the command is rejected and the bot does not resume the invalidated goal
