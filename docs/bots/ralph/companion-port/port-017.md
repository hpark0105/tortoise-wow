# PORT-017: Specify the planner protocol and fake-service contract

- Depends on: PORT-014, PORT-015, PORT-016
- Status: pending hosted dispatch; passes=false
- Roll-up: prerequisite to LLM-010
- Shared contract: [execution and review](README.md)

## Objective

Define and test the complete versioned protocol before implementing real model
transport. Use one immutable party observation and one shared response for the
party. Provide a deterministic fake service capable of success, delay, timeout,
malformed, oversized, unsupported and stale responses.

## Required envelope

Requests and responses identify protocol version, request ID, party/owner,
observation version and capture time. Each proposed step identifies bot GUID,
login generation, order/goal generation, allowlisted action or preference,
resolvable target GUID when required, and expiry. No raw pointer, raw log, prompt
transcript, credential, SQL, command, executable text or arbitrary coordinate is
allowed.

## Allowed edit candidates

- Protocol/schema documents under `docs/bots/`
- Value-only protocol types under `src/game/PlayerBots/Companion/`
- Fake service and contract tests under `docker/personality-service/` or a
  narrower head-approved test path
- No production HTTP client or real model call in this card

## Acceptance

Both sides enforce explicit size, count, enum, string-length and age limits.
Malformed, unknown-version, mismatched, expired and oversized responses fail
closed. The fake harness deterministically generates every required outcome.
Authentication and localhost/container trust boundaries are documented before a
transport is selected.

## Validation

Schema/round-trip and negative contract tests; parser fuzz-style bounded cases;
prove no response can express a game action outside the typed vocabulary. Record
measured payload sizes and proposed latency, queue and rate budgets.

