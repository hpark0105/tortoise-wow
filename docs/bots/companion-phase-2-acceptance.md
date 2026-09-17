# Companion Phase 2 cumulative acceptance (PORT-026)

- Date: 2026-09-17
- Branch: feature/kap-558-port-phase2; HEAD f96d4b2 (PORT-025), immediate
  predecessor 192624c (PORT-024)
- Image under test: tortoise-local:dev
  sha256:7f02bf278719a6b9a1727c84c3be2540b4b3e232ae201dcb82cec1b50a9e2e16,
  built 2026-09-17 from the committed C++ tree
- Session mode: standalone park-head (local Qwen). park-head holds the
  single-model mutex and cannot launch a park-agent QA worker, so the
  deterministic gates, the two failure reruns and the live local-model round
  were executed directly in this session.
- Status: park-head deterministic acceptance complete; hosted acceptance
  pending; passes=false

## Scope and environment

- Every runtime lab ran as a disposable, port-free Compose project with its
  own database volume under ignored local/; the personal tortoise-local
  server (localhost 3724/8085) and its character volumes were not started or
  touched.
- The 15 value suites ran against the committed tree (C++ compiled through
  WSL g++); the 21 runtime labs booted the image above.
- The live model round used this session's own local model at
  http://127.0.0.1:8090/v1 (Qwen3.8-27B-UD-Q4_K_XL.gguf). No second model
  server was loaded; the one-shared-model rule remains in force.

## Battery result (36 module suites)

First full battery (local/port026-summary.txt, 2026-09-17, about 96 minutes
wall clock): 34/36 PASS. Both failures were root-caused and rerun green
(Findings and repairs below); the original failure evidence is retained, not
replaced.

| Module | First run | Tests | Seconds |
| --- | --- | --- | --- |
| test_companion_inventory_value | PASS | 1 | 0.5 |
| test_companion_personality_value | PASS | 1 | 0.5 |
| test_companion_planner_protocol | PASS | 1 | 0.7 |
| test_companion_policy_value | PASS | 1 | 0.7 |
| test_companion_tank_value | PASS | 1 | 0.5 |
| test_companion_healer_value | PASS | 1 | 0.5 |
| test_companion_damage_value | PASS | 1 | 0.5 |
| test_companion_combat_value | PASS | 1 | 0.7 |
| test_companion_quest_value | PASS | 1 | 0.4 |
| test_companion_equipment_value | PASS | 2 | 0.4 |
| test_companion_converse_value | PASS | 1 | 0.8 |
| test_companion_converse_adapter_value | PASS | 16 | 0.0 |
| test_companion_planner_transport_value | PASS | 1 | 0.8 |
| test_companion_real_planner_value | PASS | 14 | 4.6 |
| test_personality_fake_contract | PASS | 5 | 1.4 |
| test_bot_planner_transport | PASS | 2 | 204.1 |
| test_bot_companion_personality | PASS | 3 | 196.3 |
| test_bot_companion_personality_persist | PASS | 1 | 106.3 |
| test_bot_companion_real_planner | PASS | 1 | 86.6 |
| test_bot_companion_converse | PASS | 1 | 237.4 |
| test_bot_quest_coop | PASS | 24 | 1018.2 |
| test_bot_equipment | FAIL(1) - rerun 17/17 OK | 9 | 718.3 |
| test_bot_vendor | PASS | 25 | 484.4 |
| test_bot_tank | PASS | 12 | 133.3 |
| test_bot_healer | PASS | 12 | 138.8 |
| test_bot_damage | PASS | 10 | 148.4 |
| test_bot_companion_assist | PASS | 11 | 223.2 |
| test_bot_companion_defend | PASS | 11 | 174.3 |
| test_bot_companion_hold | PASS | 1 | 117.5 |
| test_bot_companion_leash | PASS | 9 | 169.3 |
| test_bot_companion_priority | PASS | 5 | 128.5 |
| test_bot_companion_recovery | PASS | 11 | 343.5 |
| test_bot_companion_regroup | PASS | 15 | 288.9 |
| test_bot_follow | PASS | 10 | 263.3 |
| test_bot_combat_xp | FAIL(1) - rerun 1/1 OK | 1 | 236.7 |
| test_bot_quest | PASS | 7 | 314.4 |

## Acceptance evidence map

Every entry below is disposable-lab fixture evidence or measured lab output;
none is in-game client observation.

1. Legal tank/healer/damage behavior in the bounded scenario:
   test_bot_tank 12/12, test_bot_healer 12/12, test_bot_damage 10/10, plus
   the tank/healer/damage value suites and Phase 1 regression assist 11/11,
   defend 11/11.
2. One safe, personality-consistent chat reply with no gameplay action:
   test_bot_companion_converse 1/1 (237.4s) with the converse value suites
   (test_companion_converse_value 1 suite;
   test_companion_converse_adapter_value 16/16) asserting text-only responses
   and no gameplay effect; test_bot_companion_personality 3/3 and
   personality_persist 1/1 for consistency and persistence.
3. Cooperative quest with separate authoritative credit, turn-in, reward and
   persisted state: test_bot_quest_coop 24/24 (1018.2s).
4. Automatic class-ability learning up to level, no trainer: covered by the
   lab login checks in the companion runtime suites (learned spells asserted
   at boot without a trainer visit or purchase) and by the policy/quest value
   suites.
5. No free equipment refresh; one strict upgrade equipped through normal
   APIs; replaced gear retained; authoritative bag pressure; only the
   declared protected-safe junk set sold to a reachable vendor; equipment,
   inventory, money, spells, quest state and profile preserved across
   restart: test_bot_equipment rerun 17/17 (385.1s) and test_bot_vendor
   25/25 (484.4s), plus test_companion_equipment_value (2 tests).
6. No planner requests outside the party; stale response rejected on leave;
   re-invite starts a new party-session generation with the same persisted
   personality, skills, quest state and progression; AutoEquipForLevel not
   re-enabled on leave: test_bot_planner_transport 2/2 (204.1s),
   test_bot_companion_personality_persist 1/1 (106.3s),
   test_companion_planner_transport_value, and the equipment lab
   leave/rejoin cases (no generated gear on rejoin).
7. Current real response changes only approved preferences or expression;
   offline, slow, malformed and stale responses leave gameplay operational;
   hold blocks any old response: test_bot_companion_real_planner 1/1 (86.6s,
   phases A-E: offline, timeout, malformed, stale, held),
   test_companion_real_planner_value 14/14 and test_bot_companion_hold 1/1
   (117.5s). Phase F measured one schema-valid real response (below).
8. Phase 1/1.1 regression: assist 11/11, defend 11/11, hold 1/1, leash 9/9,
   priority 5/5, recovery 11/11, regroup 15/15, follow 10/10,
   test_bot_combat_xp (known flake; rerun 1/1, see Findings), test_bot_quest
   7/7.

## Phase F live real-model round

- Result: PASS, 1/1 in 89.9s (local/port026-real_planner_live.log).
- Measured: url http://127.0.0.1:8090/v1, model Qwen3.8-27B-UD-Q4_K_XL.gguf,
  reached:true, one schema-valid response in 1156 ms, 716/13 tokens, 1 step
  (local/tortoise-bot-rplanner-29ae2b3bed63-20260917T172235Z/live-model-measurement.json).
- This is a measured single real round, not a benchmark: no latency
  distribution, no candidate-model comparison and no tick-impact
  microbenchmark were run. The card's "measured offline/delayed tick impact"
  item is covered here only by the transport and real-adapter labs
  (test_bot_planner_transport 2/2, test_companion_planner_transport_value,
  test_companion_real_planner_value 14/14, real_planner phases A-E), which
  prove gameplay stays operational when the service is absent, slow,
  malformed or stale. The dedicated delayed-tick measurement was
  completed the same day (addendum below).

## Offline/delayed tick impact measurement (addendum, 2026-09-17)

The card's "measured offline/delayed tick impact" validation item was
completed the same day as a bounded park-head measurement: four sequential
disposable port-free labs (same image, same owner+companion party scenario,
Perf.ProcessingTelemetry=5, 120 s steady-state windows; last 22 samples of
each 5 s interval compared). Driver and evidence are gitignored under
local/.

| Run | Service state | p50 med | p95 med | p95 max | p99 med | p99 max | tick med | overflow | transport activity |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| A | disabled (no URL) | 5 ms | 6 ms | 39 ms | 29 ms | 168 ms | 50 ms | 1 | 0 lines |
| B | offline (closed port) | 5 ms | 6.5 ms | 26 ms | 33 ms | 220 ms | 50 ms | 1 | 5 timeouts, 2 cooldowns |
| C | delayed (400 ms, inside the 5 s deadline) | 5 ms | 7 ms | 59 ms | 23 ms | 83 ms | 50 ms | 1 | 53 delivered offers |
| D | timeout (past the 5 s deadline) | 5 ms | 6 ms | 28 ms | 25.5 ms | 136 ms | 50 ms | 0 | 4 timeouts, 2 cooldowns, 1 pre-settle offer |

All values in ms per 5 s interval; medians over the 22-sample steady-state
window. Result: no measurable world-tick degradation in any service state.
p95 medians move +0 to +1 ms against the disabled baseline and p99 maxima
stay within the baseline's own spread (A itself shows a 168 ms max and one
250 ms overflow-bucket sample); tick cadence is a steady 50 ms (20 Hz) in
every run. The transport's connect-fail retry, 60 s cooldown, delayed
delivery and deadline-timeout paths are all exercised (log counts above)
without disturbing tick processing. The single offer in D is the pre-settle
round answered before the timeout scenario was applied.

Limits: 120 s windows with 22 samples per run bound the tick-tail estimate;
the scenario is a single companion party, not population scale.

Evidence: local/port026-tick-impact-summary.json (summary + per-run metrics)
and, per run, local/tortoise-bot-ticka-a20172ccb221-20260917T192620Z/,
local/tortoise-bot-tickb-87617e099782-20260917T192937Z/,
local/tortoise-bot-tickc-1652168f40f4-20260917T193245Z/,
local/tortoise-bot-tickd-c7e9eb37990c-20260917T193554Z/ (world.log,
telemetry.log, metrics.json); driver local/port026_tick_impact.py.
Personal containers verified unchanged after every run.

## Deterministic gates (this session)

- 15 value suites in the battery: all OK, no skips (table above).
- python -m py_compile docker/server.py: OK.
- docker compose config --quiet: OK.
- git diff --check: clean.

## Findings and repairs

### 1. test_bot_equipment Lab B fixture staleness (repaired, rerun green)

First run: FAILED (errors=1), 9 tests, 718.3s.

Root cause: the Lab B bag filler was Tough Jerky (item 117), a quality-1
consumable, which is exactly the declared-sellable junk class introduced by
PORT-025. The new vendor cleanup therefore sold all 16 filler stacks
(money:0), clearing bag pressure (free:12) before the loot step, so the
Briarsteel Shortsword (15335) stored normally (before:0 after:1) and the
"equipment pressure ... stored:0" marker the test asserts on was never
emitted; the test timed out waiting for it.

Failure evidence: local/tortoise-bot-eq-b8378696f55e-20260917T161801Z/world-failure.log
(16 "sold item:117 count:20 money:0" lines, "pressure cleared free:12",
"corpse loot stored ... before:0 after:1").

Repair (docker/test_bot_equipment.py, committed with this acceptance):
- Lab B filler changed from Tough Jerky 117 to Earthroot 2449: class 7
  TRADE_GOODS, quality 1, which fails the q==0 junk-matrix gate in
  Inventory.h Classify and is therefore Protected; verified against
  sql/base/tw_world_item_template.sql. Bag pressure now persists through the
  loot step instead of being cleared by sales.
- Fixture charge seeds corrected from the 4-token '0 0 0 0' to the 5-token
  '0 0 0 0 0' required by Item::LoadFromDB (MAX_ITEM_PROTO_SPELLS == 5).
- _jerky_stacks renamed to _filler_stacks.

Rerun: 17/17 OK in 385.1s (local/port026-equipment-rerun1.log). Repaired
pressure-lab log
(local/tortoise-bot-eq-e2569c90e565-20260917T165619Z/world.log): "[Inventory]
pressure GUID:610401 free:0 stored:0" -> vendor found (dist 58.1) ->
"[Inventory] exhausted ... free:0" (nothing sellable; Earthroot protected)
-> "corpse loot stored ... before:0 after:0" -> "equipment pressure ...
stored:0" marker -> regroup.

### 2. test_bot_combat_xp known flake (rerun green; history retained)

First run: FAILED (failures=1), after_xp 0 not > 0; the bot never engaged.
This is a known three-day flake, not a regression of the Phase 2 diff: pass
2026-09-12 (after=54), fail 2026-09-13, fail 2026-09-17. The lab pins image
tortoise-local:mvp003-review, not the dev image under test here, so the
flake also spans images.

Isolated rerun on the same pin: 1/1 OK in 118.5s
(local/port026-combat_xp-rerun1.log). History evidence:
local/tortoise-bot-xp-442359fa43f9-20260912T213020Z/,
local/tortoise-bot-xp-51d307558393-20260913T215206Z/,
local/tortoise-bot-xp-0e3a7e1c4c23-20260917T171226Z/. The flake is recorded
as a known risk; it is not accepted as evidence of XP behavior on the new
image and should be revisited if it recurs.

## Explicitly pending (hosted acceptance / operator)

Required by PORT-026 and NOT performed in this session; park-head must not
run them:

- Hosted diff review of the cumulative Phase 2 diff on
  feature/kap-558-port-phase2 plus the fixture repair.
- Formal QA handoff: READY FOR QA -> IN QA transition with one fresh,
  serialized, read-only park-agent QA worker (no source edits).
- Hosted acceptance sign-off and passes=true on the card.
- Bounded in-game client evidence: chat, quest completion, earned gearing,
  bag-pressure reporting and protected vendor cleanup observed from the
  client.
- Jira/KAP state transitions for the Phase 2 stories.

## Evidence index (gitignored local/)

- local/port026-summary.txt - per-module battery results (fresh, 2026-09-17)
- local/port026-test_bot_*.log, local/port026-test_companion_*.log,
  local/port026-test_personality_fake_contract.log - per-module battery logs
- local/port026-equipment-rerun1.log, local/port026-combat_xp-rerun1.log
- local/port026-real_planner_live.log
- local/tortoise-bot-eq-b8378696f55e-20260917T161801Z/ - failed equipment lab
- local/tortoise-bot-eq-e2569c90e565-20260917T165619Z/ - repaired equipment lab
- local/tortoise-bot-rplanner-29ae2b3bed63-20260917T172235Z/ - live round
- local/tortoise-bot-xp-442359fa43f9-20260912T213020Z/,
  local/tortoise-bot-xp-51d307558393-20260913T215206Z/,
  local/tortoise-bot-xp-0e3a7e1c4c23-20260917T171226Z/ - combat_xp history
- local/port026-tick-impact-summary.json, local/tortoise-bot-tick[a-d]-<hex>-<ts>/ - tick-impact measurement (addendum)
