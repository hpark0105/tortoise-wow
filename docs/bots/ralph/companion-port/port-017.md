# PORT-017: Specify the planner protocol and fake-service contract

- Depends on: PORT-014, PORT-015, PORT-016
- Status: implemented, contract-verified (value + cross-language); passes=false pending head sign-off
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


## Implementation notes

- `src/game/PlayerBots/Companion/PlannerProtocol.h` is value-only: fixed-width
  little-endian pack/unpack and pure fail-closed validators; no engine pointers,
  no I/O, no transport. It is not compiled into the server yet; PORT-018 wires
  it through the transport and the controller boundary.
- Layout: envelope 40 bytes (magic, protocol version, request id, owner guid,
  observation version, capture time, step count, total size); request body 44
  bytes (generation, flags, four fixed 8-byte bot slots); step 32 bytes. The
  request is a fixed 84 bytes; a response is 72-296 bytes. The in-memory Step
  struct orders its 64-bit field at offset 16 so the natural struct size is
  exactly 32 bytes; the codec, not the member order, owns the wire mapping.
- Fail-closed order: envelope checks (oversized, size, magic, versions, ids,
  counts, total size, age) then per-step checks (ids, reserved, action
  vocabulary, target rules, preference range, expiry). One bad step rejects the
  whole response; the caller keeps the current deterministic policies.
- The deterministic fake (`docker/personality-service/fake_planner.py`) maps
  (golden request, scenario) to exact bytes for success, delay (400 ms, inside
  the 500 ms P95 budget), timeout (no payload, 5000 ms deadline), malformed,
  oversized, unsupported and stale. It performs no I/O and no model call.
- Cross-language golden pin: the 84-byte request and 104-byte response vectors
  are embedded in `docker/test_companion_planner_protocol.cpp` (value tests,
  exe `p017_value`), in `docker/test_personality_fake_contract.py`, and are the
  reference for `docker/personality-service/planner_validator_cli.cpp`, the
  byte-exact C++ anchor the Python contract test shells out to. A bounded
  400-mutation LCG fuzz (seed 12345) never crashes and exercises at least six
  distinct reject classes.
- Budgets for PORT-018: P95 <= 500 ms, timeout 5000 ms, queue depth 1 per
  party (newest wins), one round per 2 seconds, 60 s backoff after 3
  consecutive timeouts.
