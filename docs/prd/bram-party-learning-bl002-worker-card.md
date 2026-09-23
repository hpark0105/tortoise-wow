# BL-002 worker evidence card v1

Date: 2026-09-22
Repository: `C:\Users\hpark\WebstormProjects\tortoise-wow`
Branch: `feature/kap-558-port-phase2`
Baseline commit: `c5781974b444f6ad692b48a516b0994d8cf59bc5` with accepted, uncommitted BL-001A/B/R work

## Objective

Implement observe-only, bounded encounter/value contracts and recorder hooks. This
card records what happened; it must not persist data, call a model, activate a
candidate rotation, change targeting, or change combat decisions.

## Required design

1. Add `src/game/PlayerBots/Companion/Encounter.h`, a pure value-only module with
   no engine pointers and no heap allocation. It must provide:
   - explicit Idle/Active/Complete states;
   - bounded event kinds for begin, decision, cast result, damage dealt, damage
     taken, owner override, death, and end;
   - explicit end/exclusion reasons;
   - a fixed-capacity event buffer (maximum 128 events);
   - monotonic encounter-local elapsed milliseconds;
   - target/source/route, selected spell and cast outcome fields;
   - effective hostile damage attributed to the companion, periodic attribution,
     damage taken, deaths, owner override, completeness and overflow;
   - idempotent end/summary behavior and fail-closed overflow (summary remains
     available but is marked incomplete/excluded; no efficacy eligibility);
   - matching-target attribution: unrelated-target damage is ignored.
2. Add no-op virtual damage observation callbacks to `PlayerAI`. They may report
   target/source unit, effective damage, spell id, periodic flag, and whether the
   victim died. Existing AIs must remain behaviorally unchanged.
3. At the end of successful non-zero `Unit::DealDamage`, call those callbacks for
   a direct player attacker and a direct player victim only. Compute effective
   damage from victim health before versus after the authoritative operation so
   overkill is not counted. Do not attribute pet/owner damage to the player in
   this card. Zero/blocked damage produces no callback.
4. `PlayerBotAI` owns one recorder and overrides the callbacks. Hook it so:
   - a legal shared combat engagement begins/continues one encounter and ticks
     duration once per `ExecuteCombat` call;
   - a new target closes the prior encounter as target-changed before beginning;
   - offense route/selected spell/cast result are recorded at the existing
     decision/cast boundaries, including Tank versus Rotation;
   - effective damage is counted only for the active target; periodic damage is
     distinguishable; matching target death completes the encounter;
   - direct damage taken by Bram is recorded while active; Bram death completes
     it as a safety outcome;
   - accepted Hold, FollowGoal, FollowStop, or replacement Assist orders close an
     active encounter as owner override before mutating target/order state;
   - invalid target and leash exits close the active encounter with their explicit
     reason;
   - existing combat behavior, timers, target slots, logs, and fallbacks remain
     unchanged.
5. Add a standalone C++ value test and Python runner for `Encounter.h`. Cover
   boundaries, matching/unrelated attribution, direct/periodic damage, damage
   taken, target/Bram death, owner override, new target, cap/overflow, duplicate
   summary/end, zero-duration metrics, and incomplete efficacy exclusion.

The recorder may retain only the current completed summary in memory for now.
No SQL, migration, writer thread, model protocol, commands, or runtime logging is
part of BL-002.

## Allowed reads

- this card
- `docs/prd/bram-party-learning.prd`
- `docs/prd/bram-party-learning-bl001r-audit.md`
- `src/game/AI/PlayerAI.h`
- `src/game/Objects/Unit.cpp`
- `src/game/Objects/Player.h` (only the `AI()` accessor area)
- `src/game/PlayerBots/PlayerBotAI.h`
- `src/game/PlayerBots/PlayerBotAI.cpp`
- `src/game/PlayerBots/Companion/Combat.h`
- `docker/test_companion_combat_value.cpp`
- `docker/test_companion_combat_value.py`

## Allowed edits

- `src/game/PlayerBots/Companion/Encounter.h` (new)
- `src/game/AI/PlayerAI.h`
- `src/game/Objects/Unit.cpp`
- `src/game/PlayerBots/PlayerBotAI.h`
- `src/game/PlayerBots/PlayerBotAI.cpp`
- `docker/test_companion_encounter_value.cpp` (new)
- `docker/test_companion_encounter_value.py` (new)

Do not edit anything else. Do not list directories or inspect Git metadata. Do not
commit, stage, push, deploy, start services, access secrets/private data, call
retrieval, or launch another model session.

## Verified provenance

- Retrieval healthy/fresh after BL-001R documentation sync.
- `PlayerAI.h`: `61F64248547CACCE44FDFDCC5C80D4017864AB59F33BB6EB34557B1C5DFB1D5E`
- `Unit.cpp`: `B30CB46974CA26C6E33F3C1A09F802327B7ED8F97EAE1E4F23A3E33B2CB9E37F`
- `PlayerBotAI.h`: `1660C58E567937820323A89F9F69B134AD0EAB51862BD038B7CEDB0B23AADB45`
- `PlayerBotAI.cpp`: `55C0706EF177BD47715F20B599D7DF0962053A7F4A69AF2868BFA78F1948A77B`
- `Combat.h`: `44A9B6176F1FED8402065D210593AFD40F9BD7E3A4DE148EB797B74CBE48ED8A`

## Validation

Run only:

`python -m unittest discover -s docker -p 'test_companion_encounter_value.py'`

`python -m unittest discover -s docker -p 'test_companion_combat_value.py'`

`git diff --check`

The hosted head performs the full server build and runtime acceptance. Report exact
results, changed paths, and uncertainty. State explicitly that no learned behavior
was enabled.
