# Planner protocol v1 (PORT-017, KAP-558)

Versioned, value-only protocol between the world process and the local
party-planner service. This document is the single source of truth for the
byte layout, the field limits, the trust boundary and the failure modes.
The value types live in `src/game/PlayerBots/Companion/PlannerProtocol.h`;
the deterministic fake service and the cross-language contract test live
under `docker/personality-service/`.

## What this is and is not

- One **immutable party observation** (request) and one **shared response**
  for the whole party per planner round.
- Deterministic and fully offline-testable: fixed-width little-endian
  integers, no floating point, no strings, no variable-length fields.
- NOT a transport: no HTTP client, no real model call, no queueing
  implementation in this card. The transport (PORT-018) must satisfy the
  budgets and trust boundary below; the protocol itself does not depend on
  the transport.

## Why binary, not JSON

Both sides enforce explicit size, count, enum and age limits. A fixed-width
layout makes "oversized" a single integer comparison, makes fuzz-style
bounded mutation trivial, and removes an entire class of parser
differences (string escaping, number formats, key order). JSON would also
permit arbitrary keys; this layout permits none.

## Trust boundary and authentication (documented before transport)

- The service runs only on `127.0.0.1` (same host) or the Docker bridge
  network used by the lab/personal Compose project. It is never exposed on
  a public interface.
- The world process is the only client. The request carries
  `kMagic` + `request_id` + `owner_guid`; the response must echo
  `request_id`, `owner_guid` and `protocol_version` exactly. Anything else
  is a mismatch and fails closed.
- The transport layer (PORT-018) adds one shared secret token, delivered
  out-of-band through the Compose environment (never in the payload),
  checked before the payload is parsed. Until then, the localhost-only
  binding plus the envelope check is the complete boundary; the payload
  itself carries no credential and never will.
- The service has no authority: a response can only express the typed
  action vocabulary below. It cannot express a command, a coordinate, a
  SQL statement, an executable string, a log line or a prompt transcript.
  There is no field in the layout for any of those.

## Envelope (request and response share it)

| offset | size | field               | constraint                                   |
|-------:|-----:|---------------------|----------------------------------------------|
| 0      | 4    | magic               | `0x31504C43` ("CLP1")                         |
| 4      | 4    | protocol_version    | must equal `kProtocolVersion` (1)             |
| 8      | 8    | request_id          | non-zero; response echoes it exactly          |
| 16     | 4    | owner_guid          | party owner (follow leader); echoed exactly   |
| 20     | 4    | observation_version | must equal `kObservationVersion`              |
| 24     | 8    | capture_time_ms     | engine clock when the observation was taken   |
| 32     | 4    | step_count          | 0 for requests; 1..`kMaxSteps` (8) responses  |
| 36     | 4    | total_size          | must equal the received byte length           |
| 40     | 32   | step entries        | `step_count` x `kStepBytes` (32)              |

Total payload: 72 bytes (1 step) to 296 bytes (8 steps). Both sides
reject any other length before parsing.

## Step entry (32 bytes)

| offset | size | field           | constraint                                            |
|-------:|-----:|-----------------|-------------------------------------------------------|
| 0      | 4    | bot_guid        | non-zero; one owned companion                         |
| 4      | 4    | login_generation| must equal the bot's current login generation         |
| 8      | 4    | order_generation| must equal the current order generation (freshness)   |
| 12     | 1    | action          | one of the 8 values below                             |
| 13     | 3    | reserved        | zero                                                 |
| 16     | 4    | target_guid     | resolvable object GUID, or 0; see action rules       |
| 20     | 8    | expires_at_ms   | step deadline on the engine clock                     |
| 28     | 4    | preference      | 0..`kMaxPreference` (0xFFFF); packed (id<<8)\|value personality payload (PORT-019) |

## Action vocabulary (closed set)

| value | name        | target rule                  | maps to                       |
|------:|-------------|------------------------------|-------------------------------|
| 0     | NoAction    | must be 0                    | no step (idle suggestion)     |
| 1     | Hold        | must be 0                    | `.bothold` (intent Hold)      |
| 2     | Follow      | must be 0                    | `.botfollow` (intent Follow)  |
| 3     | Assist      | must be a resolvable hostile | `.botassist` (intent Assist)  |
| 4     | Defend      | must be 0 (owner-locked)     | `.botdefend` (intent Defend)  |
| 5     | Loot        | must be a resolvable corpse  | loot goal (intent Loot)       |
| 6     | Regroup     | must be 0                    | regroup goal (intent regroup) |
| 7     | Preference  | must be 0                    | personality expression only   |

No response can express any other game action: the enum is 8 values, the
validator rejects every other byte, and no field carries free text.
Coordinates are not expressible at all - movement is derived by the
existing goals from owner/party state, never proposed.

## Request body (after the 40-byte envelope, no step entries)

| offset | size | field             | note                              |
|-------:|-----:|-------------------|-----------------------------------|
| 0      | 4    | generation        | order generation at capture       |
| 4      | 1    | flags             | bit0 held, bit1 following, bit2 ownerAvailable |
| 5      | 3    | reserved          | zero                              |
| 8      | 4    | bot_count         | 1..`kMaxPartyBots` (4)            |
| 12     | 32   | bot slots         | 4 fixed slots x 8 bytes (guid 4, class 1, reserved 3); unused zero-filled |

Request payload: 84 bytes, fixed (the bot slots are always four
fixed-width entries; unused slots are zero-filled). The request is
the planner's only world knowledge; nothing else crosses the boundary.

## Limits (both sides enforce)

| limit                  | value        | enforced by                          |
|------------------------|--------------|--------------------------------------|
| max payload bytes      | 4096         | `ValidateEnvelope` (received length, pre-parse) |
| max steps / response   | 8            | `ValidateEnvelope` (count check)     |
| max party bots / req   | 4            | `ValidateRequestBody` (count check)  |
| max preference value   | 0xFFFF       | `ValidateStep` (above 0xFFFF -> `OutOfRange`) |
| max response age       | 1000 ms      | `ValidateEnvelope` (age check)       |
| max step lifetime      | 5000 ms      | `ValidateStep` (rejects as `Expired`)|
| max transport latency  | 500 ms P95   | transport budget (PORT-018)          |
| transport timeout      | 5000 ms      | transport budget (PORT-018)          |
| queue depth            | 1 / party    | newest request wins, older dropped   |
| planner rate           | 1 round / 2 s| the caller paces, not the service    |

## Failure modes (all fail closed)

| case                    | verdict                     |
|-------------------------|-----------------------------|
| bad magic               | `BadMagic`                  |
| unknown protocol version| `UnknownVersion`            |
| wrong observation version| `UnknownVersion`           |
| received size mismatch  | `SizeMismatch`              |
| oversize payload        | `Oversized`                 |
| step count out of range | `TooManySteps`              |
| zero request_id/owner/bot | `BadId`                 |
| non-zero reserved bytes | `BadReserved`               |
| action byte out of range| `UnknownAction`             |
| target rule violated    | `MissingTarget`/`ForbiddenTarget` |
| preference above 0xFFFF | `OutOfRange`                 |
| step expired at arrival | `Expired`                  |
| response age > 1000 ms  | `Stale`                     |
| response request_id/owner not echoed | `Mismatched` |
(the response's own version is checked as `UnknownVersion`, not echoed) |

On any rejection the caller discards the round, keeps the current
deterministic behavior (existing policies), and never applies a partial
response: a response is all-or-nothing per step, and one bad step rejects
the whole response.

## Measured payload sizes

Request: 84 bytes, fixed. Response: 72-296 bytes. Both far below the 4096
byte ceiling, leaving headroom for protocol v2 fields (personality state,
PORT-019/020) without a transport change.

## Proposed budgets (for PORT-018 to enforce)

- Latency: P95 <= 500 ms round trip on the local bridge; hard timeout 5000 ms.
- Queue: depth 1 per party; a new request supersedes a pending one (the
  planner answers the newest observation only).
- Rate: at most one round per 2 seconds per party (the companion tick
  pacing already bounds this); the service must not be polled tighter.
- Backoff: after 3 consecutive timeouts the caller disables planner rounds
  for 60 seconds and continues on the deterministic policies.

## Preference payload semantics (PORT-019)

The `preference` field of a `Preference` step packs `(id << 8) |
value`. Declared ids: `1` = FollowChase (value 0..2, close/medium/far
index), `2` = Expression (value 0..1, catalog slot). The world maps the
packed value per the companion's declared profile
(`Companion/Personality.h`); unknown ids, out-of-set values, an
undeclared profile, rate excess or model absence all fail closed to the
deterministic baseline. PORT-019 raised the wire ceiling from 255 to
0xFFFF to carry the packed payload; the field remains an opaque
bounded integer at this layer and the catalog enforces the stricter
fail-closed bounds.


## Real-adapter capture policy and round consumption (PORT-021)

The real-model adapter (`docker/personality-service/real_planner_server.py`)
is a planner *service*: it owns prompt construction, the model call and
the mapping of model JSON to protocol steps. The world sees only the
protocol bytes and never the model output.

### Capture policy

The v1 response envelope carries the service's *claimed* capture time;
the world validates age (<= 1000 ms from the claim) and step lifetime
(<= 5000 ms from the claim) against its own clock. The service does not
know the world's next tick, so the adapter claims

    capture = request capture + offset
    offset  = clamp(max(2000, measured_round_ms) + configured_tick_ms,
                    upper bound 15000)

with `configured_tick_ms` the expected world tick (`PlayerBot.UpdateMs`;
10000 in the personal deployment, 1000 in lab fixtures). The claim
lands at or just past the next expected fetch, inside the age budget,
for any round that completes inside the 5000 ms transport deadline.
The step lifetime stays the reference 1000 ms, so a plan can be
applied at most `tick + round` after the party snapshot it answers.
The staleness risk is bounded by construction: the only behavioral
vocabulary is the profile-allowlisted Preference (chase distance inside
the fixed 15-35 yd band, rate-limited static expression lines), and a
party-session change invalidates the round before any stale result can
land.

### Round consumption order

A submit supersedes a recorded-but-unfetched response on the same tick.
`PlayerBotAI::PlannerRoundStep` therefore **fetches the prior ready
round before submitting the next one**. Without this order, any tick
cadence at or above the 2 s planner pace (including the 10 s
production default) starved every fetch: the submit on each tick
superseded the previous round's response before `FetchOffer` ran, and
no offer was ever applied. Lab fixtures (1 s tick < 2 s pace) are the
only cadence where the old order happened to work.

### Service-side loginGeneration echo

The v1 request bot slot (guid + class + 3 reserved bytes) cannot carry
per-bot login generations, so the adapter echoes the constant 1
(matches the fake service). The world does not compare the echoed value
against the entry's login generation; freshness is enforced by the
order-generation echo, the identity/session fields and the age/lifetime
bounds. Per-bot login generation in the request is a protocol-v2
candidate (the reserved bytes).

### Prompt boundary and fallback contract

- The prompt contains only the versioned static primer
  (`context/primer-v1.txt`, digest-pinned) plus the request's value
  fields: bot ordinals, class names, flags and order generation.
  Never GUIDs, coordinates, item ids, names or live observations.
- The model may express only the closed action vocabulary WITHOUT
  targets: `none, hold, follow, defend, regroup, preference`. Assist
  and Loot require a resolvable world target GUID, which the request
  does not carry and the provider can therefore never name. Provider
  prose outside the strict JSON schema is rejected.
- Every response has >= 1 step. Provider offline, busy (single flight),
  timeout, HTTP error or malformed output answers the deterministic
  Hold fallback for every bot, so the world never waits for the model
  and always receives a response that passes the same checks as the
  fake service.

## Conversation channel (PORT-022)

A separate, deliberately simple channel alongside the binary planner
protocol: bounded, text-only companion conversation. It is not part of
protocol v1 and shares no bytes with it; the same adapter process serves
both `/plan` (binary) and `/converse` (text) on one port and guards the
model with the same single-flight lock.

- Request: `POST /converse?profile=<none|reckless|cautious>`; the body is
  the world-sanitized message text (printable ASCII, <= 200 bytes, no
  leading dot, no control characters).
- Response: `200` with the sanitized reply (printable ASCII, <= 120 bytes,
  no leading dot) or a non-200 / empty body for no reply. The transport
  treats any other outcome as fail-closed.
- Model boundary: the strict schema is `{"reply": "<text>"}`, enforced
  inside the adapter (`converse.py`). The model sees only the static
  per-profile persona and the player's text; it never sees GUIDs,
  coordinates, item ids or live world state, and unprovided game facts are
  stated as unknown, never invented.
- Transport (C++): one worker thread, one in-flight round per companion,
  a hard 4000 ms per-round deadline, and a 10000 ms reply-freshness
  budget. A reply is consumed only while the party signature (group id +
  leader low) captured at submit still matches; leaving the group or a
  leader change invalidates outstanding work. The world thread never
  performs I/O or waits: it submits and polls only.
