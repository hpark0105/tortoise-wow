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


## Implementation (2026-09-16)

### Value catalog (`Companion/Personality.h`)

`kPersonalitySchemaVersion = 1`. Two declared profiles, `Reckless` and
`Cautious`, plus `None` as the deterministic baseline (accepts no
preference, expresses nothing). The preference step payload packs
`(id << 8) | value`; declared ids are `FollowChase` (value 0..2 =
close/medium/far) and `Expression` (value 0..1 = catalog slot). The
follow-chase band is fixed at [15, 35] yards and the pre-PORT-019
constant (25) sits in the band, so `None` never changes behavior.
Expression lines are static catalog strings (48 chars max), never
dynamic or echoed text, rate-limited to one per 15 s.

### World-side effects (`PlayerBotAI`, `PlayerBotMgr`)

- `PlayerBotMgr::LoadConfig` parses `PlayerBot.PersonalityProfile`
  (none/reckless/cautious; unknown names fail closed to none) once into
  `m_personalityProfile`. `docker/server.py` passes
  `PLAYERBOT_PERSONALITY_PROFILE` through (default none).
- `PlannerRoundStep` maps a fetched `Preference` step through
  `ApplyPlannerPreference`:
  - `FollowChase`: sets `_personalityChaseDist` from the profile table;
    the owner-follow chase branch is its only consumer. Out-of-set
    values keep the current distance.
  - `Expression`: a live bot outside the rate window says the
    allowlisted line once (`Player::Say`, LANG_UNIVERSAL). Rate excess
    or an out-of-set slot stays silent.
- Owner orders, Hold, recovery and role policy are untouched and win by
  construction: chase only runs in the no-order idle owner-follow
  branch, expression is cosmetic (it never moves, targets or
  interrupts), and a dead companion is silent.
- `Remove()` resets the runtime effects to the baseline.

Log contract (debug-gated): `[PlayerBotMgr] personality profile:<name>`
at config load; `[Personality] chase GUID:%u profile:%s dist:%.1f` on
change; `[Personality] expr GUID:%u profile:%s slot:%u` on each spoken
line.

### Value and fixture evidence

- `docker/test_companion_personality_value.cpp` (`.py` runner): catalog
  bounds, pack/parse round-trips and fail-closed ids, in-band ordering
  per profile, A/B distinctness, mappings, name round-trips.
- `docker/test_bot_companion_personality.py`: three disposable runs.
  A (reckless + fake service): the identical chase proposal applies
  dist 20.0 and the identical expression proposal speaks the reckless
  slot-0 line, with the rate bound holding across an in-window window.
  B (cautious + same proposals): dist 30.0 and the cautious line.
  C (reckless, no service): model absence is the deterministic
  baseline - zero `[Planner]`/`[Personality]` lines, follow and party
  work unchanged.
- Fake-service scenarios `chase` / `express` emit one `Preference` step
  (ACTION_PREFERENCE) with a packed payload.
- Wire ceiling repair: the PORT-018 protocol capped the step preference
  field at 255, so the packed payload (FollowChase index 1 = 257) was
  rejected out-of-range at fetch time and no effect could ever apply.
  `kMaxPreference` is now 0xFFFF (planner protocol v1; byte layout
  unchanged). The reference encoder, protocol value test, and
  planner-protocol.md were updated in lockstep; the catalog still
  enforces the stricter fail-closed bounds.
