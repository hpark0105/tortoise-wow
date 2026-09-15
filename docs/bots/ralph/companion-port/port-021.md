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
