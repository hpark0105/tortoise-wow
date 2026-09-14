# Phase 2 handoff: personality and model integration

Status: **planned; not implemented or accepted by this handoff**.

## Entry gate and goal

Enter only after phase 1 is accepted. Add expressive companion personality and bounded
planning without making model availability a requirement for correct gameplay.

## Module boundary and code entry points

Extend the observation/policy/intent/executor boundary in
`src/game/PlayerBots/Companion/Policy.h` and `PlayerBotAI.cpp`.
Concrete service paths, protocol and deployment are design proposals until reviewed.
Keep transport, prompt/personality data, response parsing and deterministic behavior separate.

The model proposes schema-validated intents or preferences, never commands, SQL,
spell IDs invented from Wrath, or executable code. Use Turtle-learned abilities.
Async envelopes need identity, session/order generation, request ID, observation age
and a resolvable target GUID; never retain a raw Unit pointer across asynchronous work.
The executor rejects stale, unauthorized and unsupported responses by default.

## Exit scenarios

```gherkin
Feature: Optional personality with authoritative deterministic execution
  Scenario: Model unavailable
    Given an accepted deterministic companion
    When the model times out or returns malformed output
    Then deterministic gameplay remains available
    And no unsafe action is executed

  Scenario: An old response arrives after hold
    Given a planning request is outstanding
    When the owner orders hold and the old response arrives
    Then the response cannot restart movement or offense

  Scenario: Personality remains bounded
    Given a personality preference conflicts with an owner order or game rule
    When an intent is selected
    Then the owner order and game rule take precedence
```

## Next assignment and unresolved decisions

Write a bounded protocol/test story first: schema, timeout, cancellation, rate/queue
limits, authentication, host/container connectivity and a fake service harness.
Define measured latency/cost budgets before implementation. Treat chat and retrieved
text as untrusted data. Do not implement an arbitrary expression evaluator for triggers;
new observations require explicit supported code. Personality mistakes require an
approved gameplay policy, not unconstrained random failure.

Evidence: no phase-2 build, service integration, runtime or client checks recorded here.

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
