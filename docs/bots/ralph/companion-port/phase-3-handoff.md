# Phase 3 handoff: populated world and recruitment

Status: **planned; not implemented or accepted by this handoff**.

## Entry gate and goal

Enter after the accepted companion lifecycle and phase-2 boundaries are stable.
Expand to multiple independent world bots and recruitable companions with explicit
resource budgets. Walking NPCs alone are not evidence of independently playing bots.

## Code entry points and boundaries

Start with `PlayerBotMgr.cpp` lifecycle/population reconciliation and the companion
policy boundary. Keep ambient scheduling separate from owned-party behavior.
Recruitment must atomically validate capacity and ownership; dismissal of an owned
companion means bench, not accidental re-entry into ambient spawning.
Keep tank/healer/DPS, quest interaction and travel as independently tested
policies. Build broader quest coverage on the single cooperative Phase 2 quest;
do not treat that vertical slice as general autonomous questing.

## Exit scenarios

```gherkin
Feature: Bounded population with safe recruitment
  Scenario: Owned companion is dismissed
    Given a companion belongs to an account
    When its owner dismisses it
    Then ambient reconciliation cannot reactivate it
    And recall preserves its saved progress

  Scenario: Recruitment races or exceeds party capacity
    Given recruitment cannot satisfy current ownership or party capacity
    When a player attempts recruitment
    Then the request is rejected without duplicate ownership or lost progress

  Scenario: Population performs useful activity within budget
    Given a declared population size and supported activity set
    When the fixture runs for the agreed measurement window
    Then recorded activity meets the approved coverage criteria
    And update latency and resource usage remain within measured budgets
```

## Next assignment and unresolved decisions

Define a population experiment before increasing counts: supported zones, classes,
quest/combat activities, lifecycle transitions, party slots, pathfinding cost and
deterministic load metrics. Start with one human plus a bounded companion party;
specify what happens to an extra recruit before implementing it.
Stage independent activity separately: travel, valid combat, loot and real quest
progress each need evidence. Never equate a population counter with a lively server.

Evidence: no phase-3 performance, recruitment concurrency or independent-activity
qualification recorded here. No personal-world scale change is authorized by this file.

## Handoff update contract

Update this file at each session stop and phase exit. Never replace failed evidence
with a success summary: retain the failure and link the repair. Record:

- Date, active branch, baseline and resulting commit IDs; distinguish uncommitted work.
- Changed paths and relevant functions; why the change exists and its invariants.
- Commands actually run, exit results, exact tested source/image, and sanitized evidence paths.
- Acceptance scenarios passed, failed, or not run; separate fixture evidence from in-game observations.
- Known risks, unsupported behavior, dependencies, and the next bounded assignment.
- Worker/retrieval availability, independent review outcome, and any pending user decision.
- Deployment and Jira status independently; never infer them from a successful build.

Keep raw logs and synthetic lab artifacts under ignored `local/`. Never put secrets,
personal character data, or environment maps in these documents or worker cards.
Code comments should explain ownership, cancellation, lifetime and safety invariants,
not repeat syntax. Update affected comments when changing those contracts.
