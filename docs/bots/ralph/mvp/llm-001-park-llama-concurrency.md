# LLM-001: Serve multiple companion brain requests locally

- Epic: KAP-543
- Jira mapping: future platform story
- Depends on: MVP-008 and the first deterministic companion controller
- Status: post-MVP architecture task

## Goal

Allow multiple companions to ask the local `park-llama` service for bounded
planning decisions without making the game loop wait on one global throttle.
The model service remains local; no game state, credentials, or raw logs leave
the host.

## Acceptance

```gherkin
Scenario: Fairly schedule companion requests
Given N companions submit bounded brain requests
When the service is at its configured concurrency limit
Then requests are queued with per-companion fairness and an explicit timeout
And one slow companion cannot starve the others
```

```gherkin
Scenario: Keep gameplay responsive during model saturation
Given the local model is unavailable, busy, or over its queue budget
When a companion needs a decision
Then the deterministic role controller uses its safe fallback policy
And the game tick does not block on the model
```

## Technical plan

1. Add a single local broker in `park-llama` with a bounded priority queue,
   per-companion rate limits, cancellation, queue-depth metrics, and a worker
   pool or replica count chosen from measured hardware capacity.
2. Define a small request contract: companion id, role, location/combat state,
   available actions, deadline, and compact retrieved context. Responses are
   structured actions, never executable SQL or unrestricted server commands.
3. Add a game-side asynchronous client. It submits a request and immediately
   returns control to the deterministic controller; stale responses are
   discarded by generation and state checks.
4. Measure one-request latency, queue wait, tokens, CPU/GPU use, and fallback
   rate at 1, 2, 4, and 8 concurrent companions before raising the limit.
5. Keep the model disabled by default until deterministic companion actions,
   persistence, and observability are complete.

## Open decisions

- Whether concurrency uses one model process with a scheduler, multiple model
  replicas, or prompt batching.
- Maximum queue age and per-companion request budget.
- Context retrieval policy and the exact action schema.
- Hardware target after the 500-active-bot load stages are measured.
