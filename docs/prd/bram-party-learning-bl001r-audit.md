# BL-001R: current-level Bram rotation-candidate audit

Date: 2026-09-22
Verdict: **VIABLE AS A MEASURABLE CANDIDATE; IMPROVEMENT NOT YET MEASURED**

## Candidate

Baseline priority:

`Charge -> Hamstring -> Rend -> Heroic Strike`

Candidate priority:

`Charge -> Rend -> Hamstring -> Heroic Strike`

The only change is swapping Rend ahead of Hamstring. The candidate introduces no
ability, target, pull, condition, or mechanic.

## Verified capability window

A read-only live query established the sanitized current fingerprint:

- class: warrior;
- level: 9;
- relevant learned ranks: Charge 100, Heroic Strike 284, Rend 772, Hamstring 1715;
- declared-tank route: false because the current gate requires both level 10+
  and learned Taunt 355;
- Heroic Strike remains unavailable to the selector because on-next-swing
  abilities are deliberately classified unsupported.

The runtime catalog starts from base chains 100, 7372, 772, and 78 and resolves
the highest learned rank. Bram therefore has a current-level legal choice between
Hamstring and Rend without waiting for level 10.

## Applicability and causal difference

The candidate is applicable on the Rotation route: Assist when the declared-tank
gate is false, ContinueCombat, and Defend. It is not applied to the Tank/Taunt
branch.

A decision is candidate-exposing only when:

- Charge is unavailable because the target is inside its minimum range or Charge
  is on cooldown;
- both Hamstring and Rend are known, melee-reachable, not on cooldown, and their
  corresponding target auras are absent;
- effective rage at the first decision is sufficient for the selected action;
- the encounter can observe whether the later action remains affordable and is
  actually applied;
- Heroic Strike remains blocked identically in both arms.

Baseline selects Hamstring at that decision. Candidate selects Rend. Once the
first aura is present, the next eligible decision can select the other action.
The hypothesis is that beginning Rend's hostile damage-over-time earlier improves
Bram-attributed effective hostile damage per active combat second. This is a
causal hypothesis, not an improvement claim.

## Metrics

Primary metric:

- Bram-attributed hostile HP lost / active combat seconds, in HP/s. Periodic
  damage must remain attributed to Bram.

Secondary metrics:

- intended effect applications / candidate-exposing decisions, in percent;
- latency from the first candidate-exposing decision to the first Bram-attributed
  Rend damage tick, in milliseconds;
- time from first exposing decision to successful application of both Rend and
  Hamstring, in milliseconds.

Safety metrics:

- party damage taken / active combat seconds, in HP/s;
- encounter duration, in seconds;
- Bram deaths and party deaths, counts;
- owner-order violations, unauthorized actions, new pulls, and suppressed
  emergency actions, counts (any non-zero candidate event suspends the trial);
- rage capped without spending, in rage units and milliseconds at cap.

## Required fixtures

Synthetic value fixture:

- four known profiles in live warrior catalog order;
- Charge range 8-25 and query distance 5;
- Hamstring and Rend melee-reachable with both aura gates open;
- Heroic Strike marked on-next-swing;
- effective rage high enough for both actions;
- identity plan `{0,1,2,3}` must select resolved Hamstring 1715;
- candidate plan `{0,2,1,3}` must select Rend 772.

Disposable-world fixture:

- isolated Compose project and verified non-personal volume;
- level-9 warrior with the pinned capability fingerprint;
- fresh non-elite hostile, Bram already in melee, neither aura present;
- Charge unavailable and initial effective rage at least the sum of the effective
  Rend and Hamstring costs, unless the fixture explicitly models rage generation;
- all party members alive and above the fixture's health floor;
- persisted 1:1 preassigned baseline/candidate blocks;
- enough target durability and observation time to expose the order difference,
  while reporting fight-duration strata rather than choosing only favorable fights.

## Exclusions and confounders

Exclude, with a stored reason, encounters with pre-existing Rend/Hamstring auras,
no exposing decision, unmatched Charge state, insufficient or incomparable rage,
capability/policy changes, owner override, death, new pull, suppressed emergency
behavior, incomplete/overflowed recording, or ambiguous damage attribution.

Fight duration is a required stratum, not a post-hoc filter: long fights may make
total Rend damage equal across arms; very short fights may truncate ticks. Hamstring's
slow can alter uptime or incoming damage, so those outcomes must be measured rather
than assumed neutral.

## Fingerprint invalidation

Return to observation when any of these changes:

- class or level;
- learned/rank-resolved Charge, Hamstring, Rend, Heroic Strike, or Taunt;
- declared-tank gate result;
- effective action costs or maximum power;
- on-next-swing classification or aura-gate semantics;
- catalog/schema/policy version;
- equipment or modifiers included in the declared fingerprint;
- engagement route or plan applicability version.

## BL-002 recording requirements

Per decision record: monotonic timestamp, encounter/session ID, persisted arm,
source/route, fingerprint hash and relevant fields, plan/version and ordered indices,
full profile scan with block reason, target aura facts, distance/reach, cooldowns,
effective power and costs, selected spell/rank/reason, cast result, effect application,
owner/emergency state, and overflow/completeness flags.

Per encounter summary: Bram-attributed hostile damage with periodic attribution,
active-combat duration, first Rend tick and both-effect latency, damage taken,
deaths, pulls, overrides/emergencies, rage-cap exposure, exclusion reason, and
schema/policy/model versions. BL-007 must compute comparisons only from preassigned,
complete, matching fingerprints and must retain inconclusive results.

## Uncertainty

The allowed audit evidence does not include authoritative Rend tick/duration or
Hamstring slow values. The legal choice difference is proven; its sign and size
are not. BL-002 records the needed authoritative observations, and later disposable
trials determine whether the candidate improves, regresses, or is inconclusive.
