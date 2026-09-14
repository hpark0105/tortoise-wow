# Phase 1 handoff: one useful deterministic companion

Status: **PORT-002..006 implemented and fixture-verified; not accepted**. Last checkpoint: 2026-09-14.
Tracking: KAP-558 under the KAP-543 plan; no Jira transition made by this checkpoint.

## Goal and boundaries

Complete PORT-001 through PORT-010 in the order in [README](README.md).
Support one declared class/level companion through owner follow, assist/defend,
bounded loot/regroup, recovery, and persistent bench/recall. Tank/healer expansion,
general questing, population scale and model integration are not phase-1 acceptance.

## Current checkpoint

Branch: `feature/kap-558-port-companion-port`.
Reviewed baseline at this checkpoint: `c5da5a9` (PORT-005 assist), docs head
`b329bc1` (PORT-005 checkpoint in this file).
Resulting commit: `0305a5e` (PORT-006 defend). Working tree is clean apart
from ignored `local/` evidence and the user's untracked `.idea/`.

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
  `ClearDefendTarget`, `BotDefendScan`, `BotDefendProbe`. Execution
  re-resolves targets and validates current owner, group and world state;
  stale orders cannot resume. The assist re-validates the named creature
  (alive, attackable, non-friendly, LOS, 35 yd) on every execution and, when
  it becomes illegal, drops it and resumes the prior order - it never
  substitutes an unrelated enemy. Defend only fires on a would-be Follow
  intent, and a locked target is dropped (never substituted) when the scan
  reads the owner safe past the grace window.
- `PlayerBotMgr.cpp`: population reconciliation, owner authorization and
  order generation; `BotAssist` owns the `.botassist` validation ladder and
  the 30 yd nearest-name grid lookup; `BotDefend` owns the `.botdefend`
  ladder (unknown, unowned, not-owner, offline) and the session-scoped flag.
  Owned companions are not ambient population capacity.
- Chat/Commands registration: `.botassist`, `.botdefend` (assist: the target
  is the remainder of the args, creature names contain spaces); preserve
  legacy stop semantics separately from hold.
- `docker/test_bot_companion_priority.py`, `test_bot_companion_hold.py`,
  `test_bot_companion_assist.py`, `test_bot_companion_defend.py`,
  `test_bot_bench.py`: isolated synthetic labs, not evidence of human gameplay.

## Validation checkpoint

Repair-set image: `tortoise-local:dev`
(config sha256:71f8957e5528b230af4968efb9ac4a703665e04eb5eef4905b0a22f4b8c2f430),
built with `docker compose build world` after the `3fa6b13` C++ repairs.
Current image: `tortoise-local:dev`
(image sha256:c14486d92bb31806a2431db7e7ca476948177d7c379719d0905f578620e7eb0c),
rebuilt 2026-09-14 08:31Z after the PORT-006 grace/widening C++ work
(`local/build-probe3.log`).

Image history for the defend arc (all `tortoise-local:dev` tags):

- `a4241f19a9babf4b99c106dc6dd54ef7a55b3af194c42f225425f1e5aef87a32`:
  through PORT-005 (all earlier suites pass on it).
- `8e2e9081da0d` (short ID; runs 1-3): + PORT-006 defend C++ before the probe.
- manifest sha256:1ecc6ae5333a39910e9734b408c4ca01326b144d757d610c9730d589df9b7c69
  (run 4): + debug probe only.
- `c14486d92bb3...` (run 5, current): + grace hysteresis and attacker-set
  scan widening.

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

Remaining gaps (fixture evidence does not cover these):

- Hold during active combat is covered by the priority path, the assist
  fixture's hold-cancels-active-assist window, and now the defend fixture's
  mid-fight hold; a dedicated legacy-combat hold scenario is still not present.
- Normal death/recovery persistence for the companion is not fixture-verified;
  the unconditional resurrect-on-login was removed, but an in-game death/relog
  check is still required (PORT-010 evidence).
- The owner's own 2 s legacy melee flap (attack state flaps null/set between
  combat checks) is pre-existing bot behavior; defend is now robust to it,
  but it may still matter for combat-XP/corpse-loot acceptance quality.
- Full regression (follow/population/party suites) and independent review are
  not claimed. Synthetic saved-field comparison does not prove earned
  inventory/quest persistence.

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
