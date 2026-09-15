# PORT-014: Implement one legal tank threat policy

- Depends on: PORT-013
- Status: pending hosted dispatch; passes=false
- Roll-up: CMP-012
- Shared contract: [execution and review](README.md)

## Objective

Implement one deterministic tank policy for one declared Turtle class, level,
talent/stance and learned-spell matrix. Establish threat on a bounded two-target
pull, protect the owner or healer, and refuse unsafe pulls. Reuse the shared
typed observation and authoritative combat executor.

## Allowed edit candidates

- One tank policy under `src/game/PlayerBots/Companion/`
- Observation/intent additions strictly required by the declared policy
- `PlayerBotAI.cpp` registration/dispatch only
- One disposable tank fixture under `docker/`
- `src/game/CMakeLists.txt` when a compiled source is added

## Acceptance

The tank establishes measured normal threat before damage engages, changes or
taunts target when a protected party member holds threat, respects Hold and
owner-selected targets, and does not pull an unrelated creature. All abilities
must be learned and currently usable for the pinned Turtle build. Every attempted
ability uses the PORT-012 cast outcome diagnostics; rejection immediately falls
back to the next legal tank action or ordinary attack without a false delay. The
declared matrix names whether any ability is on-next-swing. Such an ability may
be used only if PORT-012 accepted and behaviorally proved its queue/clear
lifecycle; otherwise it is explicitly unsupported and cannot be required for
the tank policy or its threat claim.

## Failure cases

Low healer resources, uncontrolled adds, invalid threat evidence, unreachable
targets or missing legal abilities produce a bounded wait/hold/report outcome.
Do not grant spells, stats, threat or hidden regeneration to satisfy the test.

## Validation

Compile; disposable one- and two-creature pulls with normal threat inspection,
resource gates, extra-target refusal, hold cancellation and recovery. Record the
exact supported class/level/stance/spells and leave all other tanks unsupported.
