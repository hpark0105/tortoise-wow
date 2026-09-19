# PORT-021: Connect the real local model through the accepted adapter

- Depends on: PORT-020
- Status: pending hosted dispatch; passes=false
- Tracking: KAP-543 / Phase 2 model integration
- Shared contract: [execution and review](README.md)

## Objective

Connect the accepted `PORT-018` transport and `PORT-019` schema to the approved
local model service. Keep provider-specific prompt construction and response
mapping outside the game executor. Do not make the world server manage model
weights, GPU allocation or another model process.

## Allowed edit candidates

- Narrow provider adapter and prompt/schema mappings under
  `docker/personality-service/`
- Configuration and health plumbing required by the accepted transport
- A small versioned Turtle/WoW companion-context primer derived from reviewed
  supported behavior, not a copied wiki or live database dump
- Focused real-adapter integration tests
- No direct model SDK or prompt construction in `PlayerBotAI`

## Acceptance

One bounded shared party request reaches the approved local service and returns
a schema-valid personality preference/expression that passes the same current
identity, generation, age and allowlist checks as the fake service. Provider
output cannot directly express a spell, target name, coordinate, command or
game action. With the provider busy or unavailable, deterministic gameplay
continues without waiting.

The adapter supplies a bounded default primer covering supported concepts such
as party membership, bags, vendors, equipment, quests, death and recovery. The
primer contains no live NPC GUIDs, item IDs, coordinates, routes or claims that
replace current server observations. When the model needs to express a preference
such as “my bags are full; I should sell junk,” C++ still determines whether bag
pressure exists, which items are protected, which vendor is valid and whether a
route or transaction is legal.

## Available operator-local candidates

The following checksum-verified Q8 GGUF files are already available under
`C:\Users\hpark\WebstormProjects\park-llama\models` for bounded comparison:

- `qwen3-4b-instruct-2507-q8_0.gguf` (3.99 GiB)
- `Qwen3.8-4B-Q8_0.gguf` (4.29 GiB)
- `Ministral-3-8B-Instruct-2512-Q8_0.gguf` (8.41 GiB)

Availability does not approve a model. Select one only after measuring the same
schema-bound workload through the accepted service. Do not commit, copy or load
GGUF weights from the game repository, and do not start more than one local
inference model at a time.

## Failure cases

Never send credentials, environment maps, raw logs, transcripts or private
character/chat data. Do not start a second embedding/model stack, bypass the
shared-model concurrency policy, retry without a bound, or couple service health
to world readiness. Model prose outside the response schema is rejected. Do not
use broad internet/wiki text as executable truth or let static context supply a
live target, vendor, item, route or action.

## Validation

Contract and integration tests against the approved local endpoint; one measured
current response; busy/offline/timeout/malformed/stale fallbacks; prompt and
payload inspection for bounded non-sensitive data; deterministic role and Phase
1 regressions with the provider disabled. For each candidate actually tested,
record the exact filename and digest, context size, latency distribution, peak
VRAM, schema-valid response rate and fallback behavior; do not infer suitability
from parameter count alone. Snapshot-test the versioned default primer and prove
that changing live vendor/item observations does not require changing it.

## Implementation (2026-09-16, park-head local session)

### Files

- `docker/personality-service/real_planner.py` - value-level adapter:
  bounded prompt construction (primer + request value fields only),
  strict JSON schema parser (closed no-target action vocabulary,
  bounded preference value sets), deterministic Hold fallback, and the
  capture policy (see planner-protocol.md).
- `docker/personality-service/model_client.py` - stdlib
  OpenAI-compatible client: key-file auth (park-llama key file, never
  logged), one bounded request per round, hard timeout, no retry.
- `docker/personality-service/real_planner_server.py` - host-side
  service speaking the accepted wire contract (POST /plan, GET /health,
  GET /primer). Single flight (one model call at a time); every
  provider failure (offline, busy, timeout, HTTP error, malformed)
  answers the Hold fallback, so the world never waits and every
  response passes the same protocol checks as the fake service.
- `docker/personality-service/context/primer-v1.txt` - versioned,
  digest-pinned (sha256 in `real_planner.PRIMER_SHA256`) bounded
  context primer: party membership, bags/vendors, equipment, quests,
  death and recovery, follow distance, expression, held. No live NPC
  GUIDs, item IDs, coordinates, routes or server-state claims.
- `docker/test_companion_real_planner_value.py` - value suite: primer
  pin and boundedness, prompt non-sensitivity (no GUIDs/ids), strict
  parser accept/reject matrix, prose tolerance, fallback determinism,
  wire validity at lab (1 s) and production (10 s) ticks, model-client
  failure mapping against local stubs.
- `docker/test_bot_companion_real_planner.py` - runtime fixture:
  phases A offline, B timeout, C schema-valid preference applied
  (reckless chase dist:20.0 through the real wire), D malformed,
  E busy (single flight), F optional live-model round with measured
  response recorded to evidence.
- `docker/personality-service/measure_candidates.py` - operator
  harness for the candidate comparison (below).
- `src/game/PlayerBots/PlayerBotAI.cpp` - **scope note**: the
  round-consumption order repair (fetch the prior ready round before
  the new submit in `PlannerRoundStep`). Without it, any tick cadence
  at or above the 2 s planner pace (the 10 s production default)
  starved every fetch and the accepted transport could never consume a
  response in the deployed world; the card's acceptance criterion is
  unreachable without this repair. Documented in
  planner-protocol.md (round consumption order).
- `docs/bots/ralph/companion-port/planner-protocol.md` - real-adapter
  capture policy, round consumption order, service-side
  loginGeneration echo, prompt boundary and fallback contract.

### Evidence status

- Value suite `test_companion_real_planner_value`: green (14 tests).
- `test_bot_planner_transport` (PORT-018 matrix) re-run green after the
  consumption-order repair.
- `test_bot_companion_real_planner`: runtime proof of offline/timeout/
  schema/malformed/busy phases; phase F records the measured live
  response when `PORT021_MODEL_URL` is set.
- Sensitive-data inspection: prompt carries only ordinals, class
  names, flags and generation (value test pins the absence of GUIDs
  and request ids); `GET /primer` exposes the exact primer for
  inspection; the model key never appears in any log line.

### Thinking-mode handling (measured)

Qwen3 family models default to an internal reasoning pass that is
returned as `reasoning_content`; with a 64-token completion budget the
reasoning consumes the whole budget and `content` stays empty
(measured against the loaded Qwen3.8-27B-UD service: 64/64 tokens to
reasoning, empty content). Two layered mitigations:

- the user prompt ends with a `/no_think` directive (Qwen3 switch;
  other models ignore the token and the strict parser slices the JSON);
- the request carries `chat_template_kwargs: {"enable_thinking":
  false}` (override `REAL_PLANNER_CHAT_TEMPLATE_KWARGS`), which the
  Qwen3 Jinja template honors and other templates ignore.

With both applied the same full-prompt round measured 547 ms
(752 prompt tokens, 12 completion tokens, exact JSON content) on the
27B service - inside the 5 s transport round deadline with margin.
This is an operator deployment detail: the adapter is model-agnostic
and no world-server change is involved.

### Live measurement recorded (2026-09-16)

Phase F of `test_bot_companion_real_planner` ran against the approved
local service serving `Qwen3.8-27B-UD-Q4_K_XL.gguf`
(`C:\Users\hpark\WebstormProjects\park-llama\models`): one bounded
shared party request reached the service and returned a schema-valid
response that passed the same identity/generation/age/allowlist
checks as the fake service (evidence under
`local/tortoise-bot-rplanner-*` / `live-model-measurement.json`).
Measured RT on the 27B is model-selection data, not approval: the
candidate comparison below still gates the deployed model.


### Candidate comparison - operator step (blocked by shared-model
concurrency)

The three candidate GGUFs are present under
`C:\Users\hpark\WebstormProjects\park-llama\models`. Selecting one
requires starting each in turn, which conflicts with the single-model
policy while a park-head session (this one) runs on the same
llama-server. Operator steps: stop the park-head session and any other
model, then from the repo root:

    python docker/personality-service/measure_candidates.py --out local/model-measure.json

The harness measures the same schema-bound workload for each candidate
(fixed request through the real adapter prompt/parse path), records
file name, sha256 digest, context size, latency distribution, peak
VRAM, schema-valid rate, fallback behavior and whether p95 fits the
5 s transport round deadline, then stops the server. Availability does
not approve a model; selection happens after this evidence exists.
The adapter is model-agnostic: once a candidate is selected, the
deployment points `REAL_PLANNER_MODEL_URL` at the service serving it
(no code change).

### Deployment wiring (personal server)

`PLAYERBOT_PLANNER_SERVICE_URL` (existing, PORT-018) points the world
at the adapter; the adapter runs host-side (it is the only component
that may see the model endpoint and key). No world-server env change
was required: the transport already treats the adapter as an
ordinary planner service.

