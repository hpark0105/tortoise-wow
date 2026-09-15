# PORT-018: Add bounded nonblocking shared party-planner transport

- Depends on: PORT-017
- Status: pending hosted dispatch; passes=false
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
