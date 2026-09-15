# Phase 4 handoff: learning, progression and social behavior

Status: **planned; not implemented or accepted by this handoff**.

## Entry gate and goal

Build on accepted deterministic safety, optional personality and bounded population.
Add persistent, inspectable experience and social expression; claim improvement only
when measured against an equivalent baseline.

## Module boundaries

Keep memory storage/retrieval and personality progression outside the game executor.
Learned suggestions pass through the same typed-intent validation as other proposals.
Use versioned, attributable observations and bounded retention. Do not store secrets
or private chat indiscriminately. Specific paths/schema require a reviewed story.

## Exit scenarios

```gherkin
Feature: Bounded and measurable companion learning
  Scenario: A learned suggestion is unsafe or obsolete
    Given memory proposes an action incompatible with current game state
    When the executor validates the action
    Then it rejects the action and deterministic behavior remains available

  Scenario: Learning is claimed to improve performance
    Given matched baseline and learned-policy evaluation scenarios
    When both are run with recorded versions and comparable conditions
    Then the report includes measured outcomes and regressions
    And anecdotal personality changes are not reported as combat improvement

  Scenario: Persistent memory is reset or rolled back
    Given a versioned memory record
    When an authorized reset or rollback occurs
    Then the change is auditable
    And character progression and normal game persistence are preserved
```

## Next assignment and unresolved decisions

First define one learnable behavior, the outcome metric, evidence provenance,
retention/reset policy and paired evaluation fixture. Decide which personality
progression affects expression versus gameplay. Social text cannot override
ownership, hold, combat rules or user control. Defer broad autonomous learning
until this narrow experiment passes.

Evidence: no phase-4 memory implementation, learning evaluation or social acceptance
recorded here. Embedding maintenance remains a separate operator decision.

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
