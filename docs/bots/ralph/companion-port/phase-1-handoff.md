# Phase 1 handoff: one useful deterministic companion

Status: **PORT-002..008 implemented and fixture-verified; not accepted**. Last checkpoint: 2026-09-14.
Tracking: KAP-558 under the KAP-543 plan; no Jira transition made by this checkpoint.

## Goal and boundaries

Complete PORT-001 through PORT-010 in the order in [README](README.md).
Support one declared class/level companion through owner follow, assist/defend,
bounded loot/regroup, recovery, and persistent bench/recall. Tank/healer expansion,
general questing, population scale and model integration are not phase-1 acceptance.

## Current checkpoint

Branch: `feature/kap-558-port-companion-port`.
Reviewed baseline at this checkpoint: `c91a0b1` (PORT-007 corpse loot +
regroup). Resulting commits: `d12cc8a` (PORT-008 pursuit leash + owner-loss
recovery probe) and `8664037` (order-robust party recall/dismiss race
fixture). Working tree is clean apart from ignored `local/` evidence and the
user's untracked `.idea/`.

- `293743c`: excludes owned companions from ambient selection.
- `7b577bb`: follow/combat priority change. A commit title or earlier CI green
  does not establish the complete PORT-002/003 acceptance criteria.
- `3fa6b13` (repair set): policy boundary, eligible ambient counting, normal
  death-login preservation, hold cancellation generations, repaired fixtures.
- `c5da5a9` (PORT-005): `.botassist <botname> <targetname>` owner-selected
  assist - logged rejection ladder (unknown, unowned, not-owner, offline,
  not-in-world, not-in-party, target-not-found, target-is-player, target-dead,
  target-friendly, target-invalid, target-not-in-sight),
  seq-gated assist intent with per-tick target re-resolution and no
  substitution, policy priority Hold > Assist > ContinueCombat > Follow,
  hold and follow cancel an active assist, and melee facing repairs in the
  legacy, continue-combat and assist branches.
- `0305a5e` (PORT-006): `.botdefend <botname> on|off` owner-toggled reactive
  defend. `BotDefendScan` candidates: the creature's victim is the
  owner/companion, or the owner/companion is in the creature's attacker set
  (victim-equality flaps between the owner's 2 s legacy melee checks; the
  attacker set is the stable signal and bystanders never enter it). A 5 s
  grace (`kDefendTargetGraceMs`, `_defendTargetGrace`) keeps a locked target
  through flap gaps while `ExecuteDefend` re-validates world state each tick;
  the companion's first swing then hands the fight to continue-combat.
  Priority stays Hold > Assist > ContinueCombat > Defend > Follow. A
  debug-gated 2 s probe (`PLAYERBOT_DEBUG=1`, `_defendProbeTimer`,
  `BotDefendProbe`) logs the nearest defend-radius creature with
  victim/evade/combat/react/attackers/threat state - the diagnostic that
  found the flap; it stays for later cards. The flag is session-scoped
  (`PlayerBotEntry.defendEnabled`) and the authorization ladder mirrors
  BotStop/BotHold.
- `c91a0b1` (PORT-007): a dead, in-world corpse the companion last fought
  becomes a first-class Loot intent (Hold > Assist > ContinueCombat > Loot >
  Follow). `CorpseLootStep` is extracted from the legacy fallback and shared
  with `ExecuteLoot`; the attempt is bounded by a 20 s diff-countdown window
  (`kLootWindowMs`), reset on every clear path, so a denied or unreachable
  corpse never traps the companion - on release or expiry the follow goal
  resumes and the companion regroups to the owner. `UpdateFollow` re-arms the
  reached latch while out of range so the `reached` log marks every completed
  regroup, not just the first approach. New lab-only `PlayerBot.WanderRadius`
  config (default 0 = legacy frand(8,20)) clamps idle wander for seeded
  fixtures; `docker/server.py` passes `PLAYERBOT_WANDER_RADIUS`.
- `d12cc8a` (PORT-008): bounded pursuit leash + owner-loss recovery.
  `PursuitLeashTick` budgets any pursuit the companion cannot land a melee
  hit on to `kPursuitLeashMs` (30 s): it arms on the first out-of-reach
  tick, disarms when `CanReachWithMeleeAutoAttack` is true or the target is
  gone, and on expiry the caller abandons the pursuit exactly like an
  invalid target (assist guid cleared, CombatStop, motion cleared, target
  dropped) so the prior order resumes. Wired into the assist, held-target
  continue-combat and defend branches; the budget resets on every
  order/clear path (follow, stop, hold, new assist, invalid drop, defend
  clear). The follow path is re-issued at most every
  `kFollowPathRefreshMs` (5 s) unless the owner moved more than 2.0 yd (2D)
  or the motion master is empty (`_followPathX/Y/Z`, `_followPathAgeMs`), so
  a finished walk to a stopped owner no longer re-paths every tick.
  Lab-only `PlayerBot.TestLogoutScript` probe (default off,
  `UpdateTestLogoutScript`, `m_logoutProbe*`): `<guid>,<logoutMs>,<reloginMs>`
  with offsets relative to the probed bot's first observed ONLINE state;
  `DeleteBot` at the logout offset and `AddBot` at the relogin offset (after
  the old session drops). `docker/server.py` passes
  `PLAYERBOT_TEST_LOGOUT_SCRIPT`. The card ceiling extended from PlayerBotAI
  to PlayerBotMgr + server.py for the lab probe (PORT-007
  `PlayerBot.WanderRadius` precedent).
- `8664037` (test repair, not card scope): `test_bot_party.py`
  `test_pending_recall_is_invalidated_by_dismiss` hard-coded one outcome of
  the login-completion race. Every 09-13 run rejected the 55000 recall
  completion as missing-in-world (the player object was not yet findable in
  `sObjectAccessor` when the completion ran), but a faster registration
  window accepts it as seq:4, making the 70000 dismiss a legitimate accepted
  kick instead of "rejected membership". Both orderings satisfy the
  invalidation invariants; the wait and assertions now check the
  order-independent ones (the 80000 pair always ends in "cancelled pending
  recall", at least one async completion rejected, no recruit accepted
  after the final dismiss, bot out of the final group).
- Canonical command is `.bothold <botname>`; `.bothyld` remains an alias.
  Hold suppresses autonomous offense until a new authorized order in this session.
- Disposable tests cover priority, bench/restart, hold, assist and defend
  order handling.

## Code entry points and invariants

- `src/game/PlayerBots/Companion/Policy.h`: value observations, typed intents,
  selection and generation checks. Policies must not own game pointers or perform I/O.
- `PlayerBotAI.cpp`: `UpdateCompanion`, `ExecuteCompanion`,
  `IsFollowOwnerAvailable`, `FollowGoal`, `Hold`, `AssistTarget`,
  `SelectDefendTarget`, `ExecuteDefend`, `SetDefendTarget`,
  `ClearDefendTarget`, `BotDefendScan`, `BotDefendProbe`,
  `CorpseLootStep`, `ExecuteLoot` (window: `kLootWindowMs`, `_lootWindowMs`),
  `PursuitLeashTick` (`kPursuitLeashMs`), follow path throttle
  (`kFollowPathRefreshMs`, `_followPathX/Y/Z`, `_followPathAgeMs`). Execution
  re-resolves targets and validates current owner, group and world state;
  stale orders cannot resume. The assist re-validates the named creature
  (alive, attackable, non-friendly, LOS, 35 yd) on every execution and, when
  it becomes illegal, drops it and resumes the prior order - it never
substitutes an unrelated enemy. Defend only fires on a would-be Follow
intent, and a locked target is dropped (never substituted) when the scan
reads the owner safe past the grace window. The Loot intent re-resolves the
named corpse and re-validates it (dead, in-world) on every execution tick,
  and the defend gate also interrupts an active Loot goal (life over loot).
  The pursuit leash budgets any target the companion cannot land a melee
  hit on to 30 s; on expiry the pursuit is abandoned exactly like an
  invalid target and the prior order resumes - a fight within reach has
  unbounded kill time.
- `PlayerBotMgr.cpp`: population reconciliation, owner authorization and
  order generation; `BotAssist` owns the `.botassist` validation ladder and
  the 30 yd nearest-name grid lookup; `BotDefend` owns the `.botdefend`
  ladder (unknown, unowned, not-owner, offline) and the session-scoped flag.
  Owned companions are not ambient population capacity. Lab-only probes
  (default off): `UpdateTestLogoutScript` (`m_logoutProbe*`) drives a
  deterministic owner logout/relogin for the PORT-008 fixture.
- Chat/Commands registration: `.botassist`, `.botdefend` (assist: the target
  is the remainder of the args, creature names contain spaces); preserve
  legacy stop semantics separately from hold.
- `docker/test_bot_companion_priority.py`, `test_bot_companion_hold.py`,
  `test_bot_companion_assist.py`, `test_bot_companion_defend.py`,
  `test_bot_companion_regroup.py`, `test_bot_companion_leash.py`,
  `test_bot_bench.py`: isolated synthetic labs, not evidence of human gameplay.

## Validation checkpoint

Repair-set image: `tortoise-local:dev`
(config sha256:71f8957e5528b230af4968efb9ac4a703665e04eb5eef4905b0a22f4b8c2f430),
built with `docker compose build world` after the `3fa6b13` C++ repairs.
Current image: `tortoise-local:dev`
(image sha256:80f5628caa70b98865cc5efe455e8b6674d2ec3af82b38813c30595222e863b6),
rebuilt 2026-09-14 after the PORT-008 C++ (pursuit leash + follow path
throttle + TestLogoutScript probe; `local/port008-build1.log`). The previous
current was `f54d197f7440402940b469e1eaa8b97dd25a7dd06d2b5f60f55a37fc48554e02`
(PORT-007).

Image history for the defend arc (all `tortoise-local:dev` tags):

- `a4241f19a9babf4b99c106dc6dd54ef7a55b3af194c42f225425f1e5aef87a32`:
  through PORT-005 (all earlier suites pass on it).
- `8e2e9081da0d` (short ID; runs 1-3): + PORT-006 defend C++ before the probe.
- manifest sha256:1ecc6ae5333a39910e9734b408c4ca01326b144d757d610c9730d589df9b7c69
  (run 4): + debug probe only.
- `c14486d92bb3...` (run 5): + grace hysteresis and attacker-set
  scan widening (PORT-006).
- `d059e3a` (short id): + PORT-007 Loot intent/window, before the re-arm.
- manifest sha256:e9c74e69efaad6a0f3609272547f41f8720ade4312a54111152eb6b97ad7b651:
  + the `UpdateFollow` reached re-arm.
- `f54d197f7440402940b469e1eaa8b97dd25a7dd06d2b5f60f55a37fc48554e02`:
  + the `PlayerBot.WanderRadius` clamp (PORT-007).
- `80f5628caa70b98865cc5efe455e8b6674d2ec3af82b38813c30595222e863b6` (current):
  + pursuit leash, follow path throttle, TestLogoutScript probe (PORT-008).

Focused fixtures against the repair-set image, 2026-09-14 (evidence under
ignored `local/`; run logs `bench-run1.log`, `priority-run4.log`,
`hold-run2.log`):

- `test_bot_bench.py`: 6/6 OK in 148 s. Recruit, dismiss-to-bench, no
  population re-login between dismiss and recall, recall across an isolated world
  restart with unchanged saved character fields, ambient population maintained by
  eligible ambient bots only, personal containers untouched.
- `test_bot_companion_priority.py`: 5/5 OK in 80 s. Fixture: level-10 bots,
  owner 60 yd from the combat (outside the companion's 30 yd target range, so a
  single attacker), creature 80 = Kobold Laborer (level 3-4, ~95 HP, attackable
  faction). Combat outlives the 3 s follow goal, post-follow combat continues
  through `ExecuteCompanion`, the companion resumes follow and reaches the owner.
  `creature.health_percent > 100` is rejected at world load (ObjectMgr resets it
  to 100), so HP must come from the template; earlier 33x buff and Snufflesnout
  (204 HP, ~4 min fight near the wait limit) attempts are superseded, not valid evidence.
- `test_bot_companion_hold.py`: 1/1 OK in 118 s. Hold seq 2 and 4 active,
  stale goals (seq 2 at current 2, seq 3 at current 4) rejected, non-owner hold
  rejected, `.bothyld` alias works, exactly three follow activations, no crash.

Re-run against the current image after the PORT-005 C++ work (same day):

- `test_bot_companion_priority.py`: 5/5 OK in 76 s (`priority-run11.log`).
  Seed repair: entry 80 pinned 300/300 with regeneration = 0 - wild creatures
  regen maxHP/3 per 4 s tick (Creature::RegenerateAll), which kept the
  creature alive indefinitely and blocked the follow (run 10).
- `test_bot_companion_assist.py`: 10/10 OK in 218 s (`assist-run3.log`).
  First assist suite: not-in-party and not-owner rejections, hold before
  recruit (the recruit guard rejects a bot in combat, and the companion's
  30 yd auto-aggro covered the 25 yd snufflesnout), two accepted assists
  (snufflesnout seq 2, vermin seq 4), hold seq 3 cancelling the active assist
  with the target still alive, chase plus completed vermin kill, target-dead
  and target-not-found rejections, follow seq 6 reaching the owner within
  2.0 yd. Creature templates pinned by the seed: snufflesnout 900/900 so it
  deterministically outlives the assist window (it is passive and the follow
  intent never acquires targets), vermin 50/50 for a deterministic kill, both
  regeneration = 0.
- `test_bot_companion_hold.py`: 1/1 OK in 118 s (`hold-run3.log`).
- `test_bot_bench.py`: 6/6 OK in 155 s (`bench-run2.log`).

Defend fixtures (this checkpoint), 2026-09-14, `test_bot_companion_defend.py`
(evidence under ignored `local/`):

- Attacker = Snufflesnout entry 51600 pinned 1200/1200, regeneration 0, 10 yd
  north of the owner; 1200 keeps the kill inside the t24-t50 window once the
  companion adds DPS. No hostile bystander exists in the fixture: any hostile
  inside the companion's 30 yd defend scan is also inside the owner's 30 yd
  legacy acquisition (the companion rests within 2 yd of the owner), and the
  owner's post-fight idle wander drifts ~30 yd in 27 s (run2 measured), so any
  in-box placement eventually becomes a legitimate defend target. This is
  unsatisfiable by construction and is documented in the test docstring;
  negative coverage instead asserts that every defend target line references
  only the attacker GUID and that no companion line references the stranger.
- run5 on `c14486d9...`: **11/11 OK in 191.8 s** (`defend-run5.log`,
  evidence `local/tortoise-bot-defend-c0b414a18a99-20260914T082624Z/`):
  logged-in, defend toggled by the owner only, one stable lock before the
  t18 hold (cleared reason:hold), re-engagement after the t24 release, a
  single owner-safe clear after the defender death, follow resumes (seq 4
  within 2.0 yd), stop withdraws the goal, not-owner rejected, stranger never
  referenced, personal volumes untouched.

PORT-007 fixtures (this checkpoint), 2026-09-14,
`test_bot_companion_regroup.py` (evidence under ignored `local/`):

- run4 on `f54d197f...`: **15/15 OK in 254 s** (`port007-run4.log`). Two
  labs: loot + regroup (owner 610100 pinned at -130.5, companion 610101
  spawn -200.5, Kobold Vermin entry 6 pinned 150 HP / regen 0 at -163.5,
  loot 990101 -> item 117) and lootless no-trap (610400/610401, 2500040,
  loot_id 0). In both: the assist lands mid-follow-walk (18-19 yd from the
  vermin), the companion is the sole party damager, the Loot intent fires on
  the death, Lab A stores 117 (log + saved-inventory DB delta = 1), Lab B's
  tap succeeds and stores 0 (delta 0, attempt terminates), and the companion
  walks back ~33 yd and logs `reached dist:<2.0`. Personal containers
  untouched in both labs. In-game verification remains PORT-010.

PORT-008 fixtures (this checkpoint), 2026-09-14,
`test_bot_companion_leash.py` (evidence under ignored `local/`):

- run3 on `80f5628c...`: **9/9 OK in 168 s** (`port008-run3.log`; run2
  evidence `local/tortoise-bot-ls-880df23ec6fb-20260914T122709Z/`). One lab:
  owner 610200 and companion 610201 pinned, recruited, followed; at tau20
  the owner orders an assist on a Kobold Vermin (2500200, entry 6) pinned
  150 HP / regen 0 / no loot, hovering 20 yd ABOVE ground (z 103.53 vs 83.53)
  30 yd south (NO_AGGRO|FIXED_Z|NO_TARGET, so it never acquires a target,
  descends, or moves) - a deterministic valid-but-unreachable hostile.
  Assist accepted; the leash arms on the first out-of-melee tick and
  expired after the budget: 15 x 2 s "[Assist] fighting" lines between
  armed and expired (~30 s, the nominal budget); the assist is abandoned
  and the companion regroups (reached within 2.0 yd). At tau68/tau80 the
  lab-only TestLogoutScript drops and re-adds the owner: the 2-person party
  disbands on the leader logout, the companion holds safe (no offensive
  lines, position frozen) while absent, and the tau105 re-issued follow
  reaches immediately; tau120 stops it. Personal containers untouched.
- Full regression on the same image (`port008-reg-*.log`): follow 10/10 in
  264 s, priority 5/5 in 74 s, hold 1/1 in 112 s, assist 10/10 in 218 s,
  defend 11/11 in 173 s, regroup 15/15 in 257 s, bench 6/6 in 148 s, party
  6/6 in 143 s (after the `8664037` repair; the pre-repair first run was
  5/6 on the ordering race documented above).

## Superseded failure evidence

Retained per the update contract; none of it is valid acceptance evidence.
All runs 2026-09-13 21:12 to 2026-09-14 08:45 local; evidence under ignored
`local/`.

PORT-005 debug arc (retained from the previous checkpoint):

- `priority-run1/2`: FAILED - no combat observed / no `[Follow] reached`;
  earliest priority fixture runs before the fixture and C++ repairs settled.
- Two build link failures during the PORT-005 C++ work were repaired in-tree;
  no logs were saved for them, the final successful build is
  `local/build-port005-facing.log`.
- `priority-run5`: FAILED - "No combat activity after follow goal - combat
  was suppressed"; first occurrence of the suppression class.
- `priority-run6/7/8`: FAILED - `[Follow] reached` never logged; root cause
  bot facing: the companion was not rotated to face its target before the
  melee check, so combat never landed. Repaired by the `SetFacingToObject`
  calls in the legacy, continue-combat and assist branches (`c5da5a9`).
- `priority-run9`: FAILED - "combat was suppressed" again; root cause
  attributed to fixture seed nondeterminism (a seed coin flip); the seed is
  now fully deterministic content.
- `priority-run10`: FAILED - `[Follow] reached` never logged; root cause wild
  creature regeneration: entry 80 HP oscillated under maxHP/3-per-tick regen
  and never died. Repaired by pinning the template (fixed HP,
  regeneration = 0) in the seed SQL.
- `assist-run1`: FAILED - the vermin assist was never accepted (first assist
  run, pre-final fixture).
- `assist-run2`: FAILED 6/10 - `party recruit rejected combat bot:Assistcomp`:
  the companion auto-engaged the 25 yd snufflesnout before the recruit, and
  the recruit guard rejects a bot in combat; every later order then failed
  not-in-party. Repaired by the redesigned timeline (hold the companion at
  t=+4 s before the t=+8 s recruit) plus the pinned creature templates;
  `assist-run3` is 10/10.

PORT-007 debug arc (this checkpoint):

- `port007-run1`: FAILED in setUpClass - the unowned stranger in the original
  3-bot fixture auto-aggroed the vermin (~14 yd away) and looted it before
  the t30 assist; `assist rejected target-dead`. The stranger was removed
  (2-bot roster).
- `port007-run2` (image `d059e3a`): 12/15 - all loot mechanics green in both
  labs, but the post-loot `reached` anchor was missing: `_followReached`
  latched true on the first approach and never re-armed; and the owner's 30 yd
  legacy aggro had carried it to the corpse, so the companion never left
  follow range. Fixed by the out-of-range re-arm in `UpdateFollow`.
- `port007-run3` (image `e9c74e69`, vermin at 15 yd): 12/15 - the owner and
  the companion auto-aggroed from 14-29 yd at spawn and both fought the
  vermin before the scripted holds landed. Established the stationary
  impossibility: the assist lookup is 30 yd from the companion while the
  owner's aggro needs the vermin beyond ~31.2 yd (size-adjusted) of the
  owner, so a companion parked on the owner can satisfy neither. Final
  geometry: vermin 33 yd from the owner, assist fires mid-walk; the
  idle-wander flake (the legacy 8-20 yd wander could drift the owner into
  aggro radius in a large share of runs) is killed by
  `PLAYERBOT_WANDER_RADIUS=0.5`.

Defend debug arc (this checkpoint):

- `defend-run1/2` (4/11 each, image `8e2e9081da0d`): the old bystander test
  was designed-correct defend behavior - the snufflesnout sat 26 yd east,
  inside the owner's 30 yd legacy acquisition, was picked up at t31,
  retaliated, and defending it was legitimate (run2 shows defend locks, a
  sustained victim window, and companion corpse loot). The vermin (entry 6)
  victim state only flapped in sub-1 s windows. The fixture was redesigned:
  pinned snufflesnout, no hostile bystander at all.
- `defend-run3` (4/11, same image): zero defend lines for the whole run.
  The gate and scan were verified correct in source; the mob's victim was
  simply never non-null at a 1 s scan tick (owner melee state flaps:
  AttackStop between the 2 s legacy checks drops the owner from the attacker
  set, the threat list empties, the mob releases its victim).
- `defend-run4` (9/11, probe image): the debug probe proved the flap cycle
  directly - `v:610300 atk:1 threat:1` -> `v:0 atk:1 threat:0` ->
  `v:0 atk:0` -> `v:610300 atk:1` - with react:2 (REACT_AGGRESSIVE; entry
  51600 is type 7 Beast, not a critter, confirmed against
  `sql/base/tw_world_creature_template.sql`). The two failures were the late
  first lock (after the t18 hold) and early flap-clears (the first
  owner-safe clear precedes the release). Repaired by the attacker-set
  widening plus the 5 s grace; `defend-run5` is 11/11.

PORT-008 debug arc (this checkpoint):

- `port008-run1` (170 s) and `port008-run2` (171 s): FAILED 8/9 - only
  `test_leash_budget_honored`, "leash expired far too early (0 lines)".
  Root cause was the fixture, not the leash: `ASSIST_FIGHTING` has exactly
  one capture group, so `re.findall` returned a list of strings and `m[0]`
  was the first character ('6'), never equal to the full GUID. The window
  actually contained 15 fighting lines (verified in-process against run2's
  saved world log). Fixed the assertion to compare the group itself;
  `port008-run3` is 9/9.
- `port008-reg-test_bot_party` first run (128 s): FAILED 5/6 -
  `test_pending_recall_is_invalidated_by_dismiss`, the login-completion
  ordering race documented under `8664037` above (order-a: the 55000 recall
  completion was accepted as seq:4, so the 70000 dismiss was a legitimate
  accepted kick). Repaired in `8664037`; the re-run is 6/6.

Remaining gaps (fixture evidence does not cover these):

- PORT-007 residual gaps (not fixture-satisfiable this wave): full-bag
  no-store, missing loot rights, inaccessible corpse, and the owner moving
  away during the loot window (documented in the test docstring).

- Hold during active combat is covered by the priority path, the assist
  fixture's hold-cancels-active-assist window, and now the defend fixture's
  mid-fight hold; a dedicated legacy-combat hold scenario is still not present.
- Normal death/recovery persistence for the companion is not fixture-verified;
  the unconditional resurrect-on-login was removed, but an in-game death/relog
  check is still required (PORT-010 evidence).
- The owner's own 2 s legacy melee flap (attack state flaps null/set between
  combat checks) is pre-existing bot behavior; defend is now robust to it,
  but it may still matter for combat-XP/corpse-loot acceptance quality.
- Full regression ran clean on the PORT-008 image (all suites above);
  independent review and in-game verification (PORT-010) are still not
  claimed. Synthetic saved-field comparison does not prove earned
  inventory/quest persistence.
- Owner death and a map change share the follow-owner gate predicate
  (`!leader || !leader->IsAlive()` in `IsFollowOwnerAvailable`) with the
  executed owner-absence case but are not separately executed; they are
  reported as unexecuted shared-predicate checks.
- The leash fixture's vertical target never makes the companion walk toward
  it: `MoveChase` yields an airborne point at the target's Z, the path
  finder returns no path and the chase generator no-ops, so the companion
  kept its follow spline and parked. Kept as-is (vertical targets are rare;
  the leash still armed, counted, expired, abandoned and regrouped exactly
  per acceptance); real over-leash is a fleeing or path-blocked ground
  target where `MoveChase` works.
- The 2-person party disband-on-leader-logout is engine behavior the
  owner-loss flow relies on: the owner relogs ungrouped, `BotFollow` is
  owner-gated only, and the re-issued follow succeeds with the group check
  skipped.

## Next bounded assignment

Dispatch [PORT-007](port-007.md) (bounded loot + regroup) against `0305a5e`.
Upstream reference: `LootAction.cpp`; preserve Turtle group-loot semantics.
Do not set prd.json passes=true for PORT-002..006 from fixtures alone;
acceptance still needs the PORT-010 in-game pass and the tracked
regression/review gates. prd.json is unchanged at this checkpoint
(PORT-002..006 passes=false); no Jira transition was made.

## Operational handoff

Do not restart or deploy to `tortoise-local-world-1` until phase 1 is complete;
the user verifies in-game after PORT-010. Never touch personal database volumes.
Retrieval/embedding is operator-paused. Local worker launch was blocked by
PowerShell execution policy; no local-worker review is claimed. Resolve the
approved launcher before local dispatch; do not weaken machine policy silently.

## Handoff update contract

Update this file at each session stop and phase exit. Never replace failed evidence
with a success summary: retain the failure and link the repair. Record:

- Date, active branch, baseline and resulting commit IDs; distinguish uncommitted work.
- Changed paths and relevant functions; why the change exists and its invariants.
- Commands actually run, exit results, exact tested source/image, and sanitized evidence paths.
- Acceptance scenarios passed, failed, or not run; separate fixture evidence from in-game observations.
- Known risks, unsupported behavior, dependencies, and the next bounded assignment.
- Worker/retrieval availability, independent review outcome, and any pending user decision.
- Deployment and Jira status independently; never infer them from a successful build.

Keep raw logs and synthetic lab artifacts under ignored `local/`. Never put secrets,
personal character data, or environment maps in these documents or worker cards.
Code comments should explain ownership, cancellation, lifetime and safety invariants,
not repeat syntax. Update affected comments when changing those contracts.
