# PORT-019: Add bounded personality preferences and expression

- Depends on: PORT-018
- Status: pending hosted dispatch; passes=false
- Tracking: KAP-543 / Phase 2 personality
- Shared contract: [execution and review](README.md)

## Objective

Add a small versioned personality catalog and let a current validated planner
response propose cosmetic expression and bounded preferences among deterministic
approved tactics. The model never supplies raw spell IDs, target names,
coordinates or executable game commands.

## Allowed edit candidates

- Personality value types/policy adapter under `src/game/PlayerBots/Companion/`
- A small versioned catalog under a head-approved data/config path
- Allowlisted Say/Emote execution and rate limiting
- Fake personality-service mappings and focused tests
- No learning, reflection or unbounded memory in this card

## Acceptance

At least two declared profiles produce observably different but safe expression
or bounded tactical preferences in the same scripted situation. Owner orders,
Hold, recovery, target legality and deterministic role safety always win.
Expression uses allowlisted channels/emotes, bounded length and frequency, and
cannot echo instructions or arbitrary retrieved text.

## Failure cases

Unknown profile, unsupported preference, unsafe text, rate excess, stale response
or model absence yields the deterministic baseline. A personality cannot create
random mechanical failure, break crowd control, start a pull, override healing
triage or weaken the executor.

## Validation

Compile; deterministic A/B profile fixture; preference-conflict and expression
sanitization/rate tests; full offline/malformed/stale fallback matrix. Record
behavioral differences separately from combat-performance claims.

