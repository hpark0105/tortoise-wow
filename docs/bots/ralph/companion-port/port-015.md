# PORT-015: Implement one legal healer triage policy

- Depends on: PORT-013
- Status: implemented; value coverage green (test_companion_healer_value.py
  1/1) and runtime acceptance green (test_bot_healer.py 12/12) on
  tortoise-local:dev sha256:21a0e9d2832d6fbf76dee855472dbb7dc87703e5aca0b7e870f887bbadb01744;
  passes=false pending user approval
- Roll-up: CMP-013
- Shared contract: [execution and review](README.md)

## Declared matrix (the only supported healer build)

Recorded per the validation requirement; every other companion keeps the
ordinary companion path untouched (gate: `IsDeclaredHealer`, constants in
`Companion/Healer.h`).

- Class: Druid (11)
- Level: 12 (Regrowth 8936 baseLevel 12, maxLevel 17)
- Spells: { 8936 Regrowth } only. The gate is the learned-spell book:
  HasSpell(8936) reads character_spell (spell ids, loaded at login by
  _LoadSpells); the fixture seeds it. In the pinned renumbered DBC 8936 is
  "Regrowth": effect1 10 (SPELL_EFFECT_HEAL), heal 83 + d15, 96 mana, 0
  recovery / 0 category recovery, castingTimeIndex 5 (2000 ms), rangeIndex 5
  (0-40 yd), interruptFlags 15 (MOVEMENT | DAMAGE | EMOTE | UNKNOWN: an
  in-progress cast is interrupted by moving more than 0.5 yd;
  auraInterruptFlags is 0). Higher ranks (8937+, level 18+) and the instant
  Swiftmend line are out of the declared window and are never advertised or
  cast by this policy. Note: spells load from the spell_template DB table in
  this build (not the DBC); the loader maps spell_template column 21 to
  InterruptFlags and column 22 to AuraInterruptFlags - the earlier matrix
  draft pinned the aura column by mistake, which is how the movement
  interrupt was missed.
- On-next-swing abilities: none (Regrowth carries no
  SPELL_ATTR_ON_NEXT_SWING attribute); per the PORT-012 UNSUPPORTED
  classification none may be required.
- Triage model: a slot is injured below the declared 70% health threshold
  (per-mille integer math). Declared priority: owner (the follow leader) >
  self > other party members, most-injured first, ties by lower GUID (total
  deterministic order). A cast is affordable only while the post-cast mana
  stays at or above the declared flat reserve (96 + 32): cost + reserve >
  mana is a bounded no-cast outcome, never a delayed retry. Range/LOS are
  per-slot facts the adapter re-resolves each tick; an out-of-range injured
  slot is ineligible this tick and the follow goal regains range - the
  policy never requests a teleport.
- One flight: a timed cast sits in the current generic spell slot in
  SPELL_STATE_PREPARING for the whole cast window (it becomes DELAYED only
  at completion). The authoritative busy fact is therefore
  IsNonMeleeSpellCasted(withDelayed=true); a second cast is refused until
  the cast completes (the core otherwise replaces the in-flight cast - the
  re-accept storm this card pins against: an earlier build produced 16-17
  accepted casts in one window, all at the identical pre-cast mana).
- Hold position: the declared heal is movement-interruptible, so an
  accepted cast stops the walk in progress (MotionMaster::Clear(false)) and
  the follow goal holds position while the cast is in flight (its
  out-of-range branch clears motion instead of re-issuing MovePoint while
  IsNonMeleeSpellCasted(withDelayed) is true); the approach resumes on the
  first tick after the cast ends.

## Objective

Implement deterministic triage for one declared Turtle healer class and
level. Choose among self, owner, tank and other supported party members
using explicit health/resource thresholds, normal range/LOS checks and
learned legal spells.

## Allowed edit candidates

- One healer policy under `src/game/PlayerBots/Companion/`
- Observation/intent additions strictly required by triage
- Authoritative positive-spell execution in `PlayerBotAI.cpp/.h`
- One disposable healer fixture under `docker/`
- `src/game/CMakeLists.txt` when a compiled source is added

## Acceptance

The healer selects the declared highest-priority injured legal party
member, casts only a learned affordable heal, preserves enough mana for the
approved small-pull policy, follows to regain range without unsafe
teleporting, and returns to deterministic follow or recovery afterward.
Every attempted heal uses the PORT-012 cast outcome diagnostics. A rejected
heal does not create a false success or delay and immediately produces the
next legal triage/follow outcome.

## Failure cases

No mana, no legal spell, owner loss, map mismatch, unreachable or out-of-LOS
targets, death, Hold, or a newer order prevents the heal and produces a
bounded safe outcome. Do not fabricate regeneration, spell ranks or health
changes.

## Implementation notes

- `src/game/PlayerBots/Companion/Healer.h`: value-only slots/observation,
  `Injured` / `Castable` / `Affordable` predicates and the deterministic
  `Select` (owner > self > most-injured, ties by lower GUID); no engine
  pointers, no world access. The busy gate (canCast) and the mana floor
  (Affordable) are folded into `Select` as bounded no-cast outcomes;
  `HealerTriageStep` additionally re-validates before casting and logs the
  distinct bounded skip reasons (target-stale / busy / cooldown / mana-floor)
  when the live re-resolution flaps.
- `PlayerBotAI::FillHealerObservation`: one fresh observation per tick from
  the live party slots (health, alive, map/LOS, distance, owner/self flags)
  and the withDelayed cast fact; `PlayerBotAI::HealerTriageStep` re-resolves
  the target from the GUID, re-checks world/ownership/group/range/LOS/
  cooldown/mana and casts through the ordinary spell path, reporting through
  the PORT-012 cast vocabulary (`[Healer] heal cast-accepted/rejected`).
- The hold-position behavior above is the shared-path change that kills the
  storm: before it, the follow goal (2 yd follow range) kept walking the
  healer during the 2 s cast, every >0.5 yd move canceled the in-flight cast
  (Spell::update movement-interrupt check), the slot freed early and the
  triage re-accepted - 16-17 accepted casts per window. After it, the
  measured runtime shows exactly 3 accepted casts (owner, owner, self) with
  clean mana deltas (215 -> 150 -> 147).
- Measured fixture dynamics recorded for later cards: the druid login
  clamps its health to roughly 316 of 576 (below its own 70% threshold
  regardless of the seeded value), the owner's seeded health is honored,
  the owner's out-of-combat regen is a few HP/s, and Regrowth lands 84-98
  on the declared targets with the mana charged at cast completion.
- Value coverage: `docker/test_companion_healer_value.{cpp,py}` (floor
  boundary at 128/127, 70% threshold, priority order owner > self > other,
  most-injured tie-break, range/LOS gates, busy gate).
- Runtime fixture: `docker/test_bot_healer.py` (disposable lab; two roster
  bots, pinned 20000 HP retaliating attacker, scripted recruit/follow/
  assist/hold; assertions: bounded accepted-cast count with the floor held
  on every acceptance, no owner cast above the threshold, self-heal only
  after the owner leaves the threshold window, cast vocabulary, behavior
  path continuation with victim:0, hold silence, owner survival/recovery,
  attacker survival, personal volumes untouched).

## Validation

Compile; deterministic competing-injury, low-mana, range/LOS, hold/stale-order
and post-pull recovery scenarios. Record the exact class/level/spells and
actual mana and health deltas. Done: value suite 1/1 and the disposable
runtime lab 12/12 on the image above; the unguarded re-accept storm (16-17
accepted casts) is gone (3 accepted casts with clean pre-cast mana). The
Phase 1 regression matrix (value combat/policy/tank plus the runtime
assist/defend/leash/priority/hold/regroup/recovery fixtures) is green
against this build (value 3x 1/1; assist 11/11, defend 11/11, leash
9/9, priority 5/5, hold 1/1, regroup 15/15, recovery 11/11) because the
hold-position guard and the cast-accepted Clear(false) touch the shared
follow/cast path. The final committed tree recompiles clean as
tortoise-local:dev sha256:647725a40f9879ccbcb12c5cfa7427858767135c4ad77e0ac24b4cd998926dba.
