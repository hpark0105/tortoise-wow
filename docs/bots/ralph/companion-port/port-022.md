# PORT-022: Add bounded conversational party chat

- Depends on: PORT-021
- Status: pending hosted dispatch; passes=false
- Tracking: KAP-543 / Phase 2 conversation
- Shared contract: [execution and review](README.md)

## Objective

Let a player address a current party companion through one declared chat channel
and receive a bounded personality-consistent response from the accepted local
model adapter. Conversation is text-only and has no path to gameplay directives,
commands or raw executor inputs.

## Allowed edit candidates

- A bounded conversation request/response schema under the personality service
- Narrow party-chat or whisper intake and allowlisted Say/Whisper output
- Addressing, sanitization, length/rate and timeout controls
- Fake- and real-adapter conversation fixtures
- Minimal dispatcher wiring; no gameplay executor changes

## Acceptance

Only a bot in the player's current party may receive a request. One clearly
addressed player message produces at most one bounded response with the bot's
persisted personality. Random questions and ordinary conversation are allowed,
but unprovided game facts are treated as unknown rather than invented. Leaving
the group invalidates outstanding work and prevents new requests.

Conversation output is sanitized text only. It cannot contain or be reinterpreted
as a command, target, spell, item, coordinate, quest action or typed gameplay
directive. The world thread never waits for the response.

## Failure cases

Do not persist raw transcripts by default, send private chat indiscriminately,
include secrets or environment data, echo prompt-injection text, answer once per
tick, or let conversational prose enter the deterministic action pipeline.
Offline, slow, malformed, stale or unsafe output produces no response and does
not affect gameplay.

## Validation

Fake and real local-model tests for a greeting, a random question and a
personality-distinct reply; addressed/unaddressed messages; party leave/rejoin;
rate, length, sanitization, timeout, malformed and stale responses; concurrent
combat with no world-thread delay or gameplay action caused by chat.
