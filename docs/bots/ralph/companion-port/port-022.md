# PORT-022: Add bounded conversational party chat

- Depends on: PORT-021
- Status: implemented (PORT-022); passes=false pending acceptance
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

## Implementation (PORT-022)

A player who addresses a current party companion through party chat (or the
lab `.botpartymsg` command) gets at most one bounded, personality-consistent,
text-only reply from the accepted local-model adapter. Conversation has no
path to gameplay directives: the world sends only the sanitized message text
and the companion's persisted profile name, and says back only sanitized text.

### Components

- `src/game/PlayerBots/Companion/ConversationTransport.{h,cpp}`: the bounded
  nonblocking transport. One worker thread, one in-flight round per companion,
  a hard 4000 ms per-round deadline. `ConvRound` is a value-only state machine
  (submit / busy / reject, worker deliver ok/fail, stamp-mismatch discard,
  party-signature drop, staleness drop, one-shot consumption, invalidation)
  shared with the value test `docker/test_companion_converse_value.cpp`.
- `PlayerBotMgr::BotPartyMessage(issuer, text)`: the single intake. Sanitizes
  the message (single-line, printable ASCII, <= 200 bytes), requires the first
  token to name an owned companion in the issuer's current party (case
  insensitive), and submits one bounded round carrying the group id + leader as
  the party signature and the companion's persisted profile.
- `PlayerBotAI::ConversationRoundStep()`: the world-thread consumer. Polls for
  a ready reply whose party signature still matches and whose age is within
  budget, re-sanitizes (defense in depth), and `Say`s it. A dead companion is
  silent; an empty reply says nothing. Leaving the group invalidates
  outstanding work (no group -> Invalidate; a changed group or leader -> the
  poll drops the round).
- Intake wiring: the party-chat handler (`CHAT_MSG_PARTY`) calls
  `BotPartyMessage` after the normal broadcast (a no-op unless the first token
  names an in-party owned companion); `.botpartymsg <botname> [message]` is a
  lab-only command (SEC_PLAYER) that feeds the same dispatcher so a
  follow-script can drive it deterministically.

### Wire contract (adapter)

`POST /converse?profile=<none|reckless|cautious>` with the sanitized message
text as the body. `200` with a sanitized reply (<= 120 bytes, printable ASCII,
no leading dot) or non-200/empty for no reply. The strict model schema
(`{"reply": "..."}`) is enforced inside the adapter (`converse.py`); the
transport boundary is plain sanitized text on both sides. The model sees only
the static per-profile persona and the player's text; it never sees GUIDs,
coordinates, item ids or live world state, and unprovided game facts are
treated as unknown. The real and fake adapters share the single-flight model
lock with the planner, so a busy model simply yields no reply.

### Sanitization (both directions)

Inbound: strip control characters and newlines, keep printable ASCII, trim,
cap at 200 bytes. Outbound (adapter, and again in the world): keep printable
ASCII, trim, remove a leading dot (a reply must never read as a command), cap
at 120 bytes. Conversation prose can never be reinterpreted as a command,
target, spell, item, coordinate or typed directive, and can never enter the
deterministic action pipeline.

### Failure modes (all fail closed)

Offline, busy, slow (past the 4000 ms round deadline), malformed, oversized,
stale (party signature changed or reply age exceeded) and concurrent combat
all produce no reply and never delay a world tick: the I/O runs on the
transport worker, and the world thread only ever submits and polls.

### Deployment wiring (personal server)

`PLAYERBOT_CONVERSATION_SERVICE_URL` (env) -> `PlayerBot.ConversationServiceURL`.
Empty leaves the transport disabled (no thread, no I/O, no reply). The adapter
runs host-side (the only component that may see the model endpoint and key) and
can serve both `/plan` (PORT-018/021) and `/converse` on one port.

### Evidence

Value: `docker/test_companion_converse_value.{py,cpp}` (ConvRound state
machine) and `docker/test_companion_converse_adapter_value.py` (persona /
sanitize / schema). Runtime: `docker/test_bot_companion_converse.py` (one
disposable Compose lab; one world generation; the fake adapter; a deterministic
follow-script drives greeting / random question / unaddressed / slow /
malformed / empty / party-leave, asserting exactly two Say'd replies and that
the world thread never blocked on the up-to-5 s model rounds). A real-model
`/converse` probe runs when `PORT022_MODEL_URL` is set (recorded to
`local/tortoise-bot-converse-*/real-converse-probe.json`).
