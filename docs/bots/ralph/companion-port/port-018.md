# PORT-018: Add bounded nonblocking shared party-planner transport

- Depends on: PORT-017
- Status: implemented and contract-verified; passes pending head sign-off (passes=false)
- Roll-up: LLM-010
- Related: `../mvp/llm-001-park-llama-concurrency.md`
- Shared contract: [execution and review](README.md)

## Objective

Implement a production-shaped but fake-service-backed transport that never
blocks the world thread. Submit one shared party request on bounded events, not
one request per bot or per tick. Offer current validated responses to the
deterministic controller; never execute them directly in transport code.

## Allowed edit candidates

- `PersonalityClient`/planner queue sources under `src/game/PlayerBots/Companion/`
- Narrow `PlayerBotMgr` lifecycle hooks and `PlayerBotAI` polling/offer hooks
- `src/game/CMakeLists.txt`
- Fake-service integration tests and required Docker/config passthrough
- No real prompt or model dependency in acceptance

## Acceptance

The world tick performs no DNS, connection, read, write or wait. Queue and
in-flight counts are capped; deadlines, response sizes and retry/backoff are
bounded. Only one shared request represents a party observation. A current
schema-valid response may offer allowlisted preferences/directives after all
identity, session, generation, target and age checks pass.

Only bots currently in a player's party may be included. Accepted party membership,
not proximity or ownership metadata alone, activates planner eligibility. Joining
creates a fresh party-session generation. Leaving, removal or party disbandment
stops new requests and invalidates every response from the prior generation.

## Failure cases

Busy, offline, disconnected, slow, malformed, stale, reordered or duplicate
responses produce no model-derived action and do not delay deterministic follow,
combat, healing, loot or recovery. Logout, party leave/removal/disband, Hold and
world shutdown cancel or invalidate outstanding work without use-after-free.
Cancellation must not delete persisted personality or normal character progression.

## Validation

Compile; fake-service success/failure matrix; thread/lifetime inspection; measured
world-tick latency with the service offline and delayed; logout/relogin, Hold,
party join/leave/rejoin/disband and shutdown while requests are outstanding. Prove
that an old response cannot cross a party-session generation. Run deterministic
role and Phase 1 regressions with transport disabled and unavailable.

## Implementation (2026-09-16)

### Sources

- `src/game/PlayerBots/Companion/PlannerTransport.{h,cpp}`: the transport
  (card's "planner queue sources"). `Round` is a value-only state machine
  (submit/supersede/reject/cooldown, stamp-based invalidation) driven under
  the transport lock in the engine and standalone by
  `docker/test_companion_planner_transport_value.cpp`.
- `PlayerBotMgr`: owns one `PlannerTransport` (initialized in `LoadConfig`
  from `PlayerBot.PlannerServiceURL`) and exposes `FindBotByGuid` (world-
  thread lookup, no allocation). The transport is shut down explicitly
  in `DeleteAll()` (the world shutdown path; the singleton destructor
  also calls it) so the bounded worker join always runs.
- `PlayerBotAI::PlannerRoundStep`: runs at the top of `UpdateCompanion`
  (before the no-order early return, because eligibility is party
  membership, not order state); `Remove()` invalidates the tracked
  session.
- `docker/personality-service/fake_planner_server.py`: live fake service
  (stdlib HTTP, scenario-switchable) reusing the `fake_planner.py`
  reference encoder.
- `docker/test_bot_planner_transport.py`: two-run fixture (disabled
  regression + live failure matrix).
- Config passthrough: `PLAYERBOT_PLANNER_SERVICE_URL` in
  `docker/server.py` (empty by default).

### Thread model and budgets

The world thread only calls `SubmitShared` / `InvalidateSession` /
`FetchOffer` / `SessionGeneration` (mutex-guarded, no I/O, no waits). One
worker thread owns all DNS (`getaddrinfo`), connect, read and write under a
hard `kRoundTimeoutMs = 5000 ms` per-round deadline. Shutdown is a bounded join (cannot outlive one in-flight round) and
runs from `DeleteAll()` on world shutdown as well as the destructor.
Budgets: `kMaxSessions = 8`,
`kPlannerPaceMs = 2000` (one shared round per party per 2 s), queue depth
1 per party (a submit while a round is in service is refused, not
superseded - the worker is the only sender, so supersede would starve the
failure count against a slow service; the next pace tick resubmits
fresh), request exactly 84 bytes, response
ceiling 4096 bytes, 3 consecutive failed rounds (timeout, connect-fail,
oversized, http-error) trigger a 60 s cooldown; a success or an
invalidation resets the counters.

Lifecycle: `Shutdown` leaves the transport clean (fresh `m_stopping`,
empty session table) so a later `Init` starts a live worker. Every
world boot exercises this: `PlayerBotMgr::Init` calls `DeleteAll()`
(which shuts the transport down) before the final `LoadConfig()` ->
`Init`, so the boot log shows a `transport stopped` / `transport
started` pair. Without the reset the re-Init worker exits on its
first `m_stopping` check and every round hangs in flight (observed
symptom: `submit busy` flood, zero `timeout` lines, no offers, no
request at the service).

### Party-session semantics

Session key = party leader's low GUID. Eligibility = accepted player-party
membership only: owned companion (`ownerAccountId != 0`) in a group whose
leader is a live player (bot-led parties are ineligible). Party signature
`(groupId, leaderLow)`; any change invalidates the prior session (fresh
generation, counters reset, immediate first round). `Remove()` (bot
logout/removal) invalidates the tracked session so no stale offer can
cross a membership boundary. The lowest-GUID owned companion in the party
builds and submits the shared request; every owned companion fetches its
own step. A worker result whose stamp no longer matches a live round is
discarded, so a slow response can never cross a party-session generation.

### Offer consumption

`FetchOffer` validates the whole response fail-closed against the stored
request (all PORT-017 checks at the engine clock). A rejected round is
consumed; a valid round loses only the fetched bot's step, so the party's
other bots fetch theirs from the same round (unclaimed steps clear on the
next submit or invalidation, and any later fetch is age-bounded). In
PORT-018 the offer is **recorded, not executed**: `PlayerBotAI` stores it
in `_plannerOffer` / `_plannerOfferValid` plus a bounded debug line.
Behavioral mapping of planner actions to tactics is PORT-019's boundary.

### loginGeneration protocol gap (v1)

The v1 request bot slot is guid + class + 3 reserved bytes: it cannot
carry per-bot login generations. The world-side live check therefore
compares `step.orderGeneration == _followSeq` plus identity, session and
age fields (the protocol). The `loginGeneration` echo is a service-side
freshness policy for the real planner (PORT-021); per-bot login generation
in the request is a protocol-v2 candidate (reserved bytes).

### Log contract (fixture)

`[Planner] transport started url:...` / `transport stopped` (always);
`submit owner:%u gen:%u superseded:%u`, `invalidate owner:%u gen:%u`,
`reject owner:%u bot:%u reason:<name>`, `offer owner:%u bot:%u action:%u
target:%u pref:%u ordergen:%u` (gated by `PlayerBot.Debug`);
`timeout owner:%u result:%d count:%u` and `cooldown owner:%u ms:60000
(repeated round failures)` (always, bounded).

### Fixture matrix

Run 1 (no URL): follow + recruit work, zero `[Planner]` lines (disabled
regression). Run 2 (host-side fake via compose `host-gateway`): started +
submit gen:0 + offer; scenario switches `stale` -> `reason:stale`,
`malformed` -> `reason:size-mismatch`, `unsupported` ->
`reason:unknown-version`; `timeout` -> three `timeout` lines then
`cooldown`, with request silence at the service for 10 s; script-driven
dismiss/recruit -> `invalidate`, a `submit ... gen:1` after the
invalidation (cooldown cleared) and new offers. Graceful `compose stop`
-> `transport stopped`, no `[CRASH]`. World-tick latency with the service
offline/delayed is bounded by construction (world thread never blocks) and
by the Run 1/Run 2 regression paths; the measured method is: compare
`PLAYERBOT_UPDATE_MS` tick cadence in the world log with the service
absent versus in the timeout scenario (no added ticks in either case).

Oversized is covered at the byte level by the PORT-017 contract suite
(content-length + buffer ceiling); the live fixture exercises it via the
same transport code path only if added later.
