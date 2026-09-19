# PORT-014: Implement one legal tank threat policy

- Depends on: PORT-013
- Status: implemented; runtime acceptance green (test_bot_tank.py 12/12 on
  tortoise-local:dev sha256:0d11023f115d3f91bc8f741c2fdd9dbcf329d726456091a493cdc01a2ebbd1bf);
  passes=false pending user approval
- Roll-up: CMP-012
- Shared contract: [execution and review](README.md)

## Declared matrix (the only supported tank build)

Recorded per the validation requirement; every other tank is unsupported
and keeps the ordinary companion offense path (gate: `IsDeclaredTank`,
constants in `Companion/Tank.h`).

- Class: Warrior (1)
- Level: 10 (Taunt 355 baseLevel 10, maxLevel 0; legal at any level >= 10)
- Stance: 131072 declared in the pinned spell data, but this build DOES
  enforce Stances against shapeshift form only (SpellEntry::
  GetErrorAtShapeshiftedCast): a neutral-stance player casting a spell
  with non-zero Stances fails with result 86
  (SPELL_FAILED_ONLY_SHAPESHIFT) unless the spell carries
  SPELL_ATTR_EX2_NOT_NEED_SHAPESHIFT. Taunt 355 was left un-normalized
  (the rest of the pinned set is 0, e.g. Whirlwind 974, Taunt 29060),
  so the data fix below normalizes it through spell_mod.
- Spells: { 355 Taunt } only. The gate is the learned-spell book:
  HasSpell(355) reads character_spell (spell ids; this build has no
  character_known_spells), which the fixture seeds. Note the pinned
  client's DBC is renumbered: the SkillLineAbility row for spell 355
  names skill line 257 ("Protection", classMask Warrior) while skill
  line 355 is the warlock "Affliction" line (classMask Warlock), so a
  character_skills row for 355 is rejected at login for a warrior.
  Skill lines (HasSkill) are not the gate; the fixture also seeds
  character_skills 257 for matrix fidelity. In the pinned spell data
  355 is the only
  true taunt (effect 114 SPELL_EFFECT_ATTACK_ME: threat catch-up to the
  current victim plus victim switch and the 1.11 taunt debuff; instant,
  0 rage, melee range, category 82 with a 10 s category recovery). The
  higher-rank rows 7390/7391/11588/11589/11590 named "Taunt" carry
  effect 63 (SPELL_EFFECT_THREAT, additive threat only) and do not force
  a victim change, so they are not taunts in this build.
- On-next-swing abilities: none declared (Taunt 355 carries no
  SPELL_ATTR_ON_NEXT_SWING attribute); per the PORT-012 UNSUPPORTED
  classification none may be required by the policy.
- Threat model: the victim switches above 1.1x the current victim's
  stored threat in melee (1.3x at range); the policy taunts when the
  protected member reaches that margin or is the victim, and never
  while the tank is already the victim (the core rejects that cast as
  a no-op). The 10 s category recovery is the adapter's tauntUsable
  gate: a cooled-down taunt degrades to the ordinary attack in the same
  evaluation, never a delayed retry.
- Data fix (versioned migration
  `sql/database_updates/world/20260916033000_world.sql`): spell_mod row
  Id=355 Stances=0 so a neutral-stance warrior can cast Taunt (also
  fixes the same cast failure for in-game warriors). The other
  un-normalized classic warrior stance rows (e.g. Shield Block 2565)
  are out of scope for this card.
- Bounded pull: the manager refuses a new owner-selected assist for the
  declared tank at or above two engaged live hostiles (count derived
  from the world at command time: live hostile victim, live hostile
  attackers, and nearby hostiles that still tie the tank in as victim or
  through stored threat in their threat list; no bookkeeping).

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

## Implementation notes

- `src/game/PlayerBots/Companion/Tank.h`: value-only observation,
  decision (NormalAttack | Taunt) and the declared matrix constants;
  no engine pointers, no world access.
- `PlayerBotAI`: the policy runs only on the Assist source (the tank
  engages only owner-selected pulls; it never autonomously acquires
  targets). `FillTankObservation` re-reads the threat list, victim
  state and cooldown facts each 2 s combat step; `TankTauntStep` casts
  through the ordinary cast path and reports with the PORT-012 cast
  vocabulary; a rejected cast falls back to the ordinary attack in the
  same evaluation. The `[Tank] threat` / `[Tank] taunt cast-*` lines
  are the fixture contract.
- `PlayerBotMgr::BotAssist`: the pull-cap refusal (declared-matrix
  gated) with the `assist rejected pull-cap` log line.
- Runtime evidence (disposable lab): measured normal threat before the
  first taunt (victim-is-tank with me > 0), taunt cast-accepted on A
  with the owner holding threat in the preceding observation, victim
  switched back to the tank after the cast, pull-cap refusal at
  engaged:2, all targets surviving, and three holds stopping tank
  actions. The follow leader is the protected member: the fixture
  binds it with `.botfollow` after recruit (a recruited companion is
  not followed implicitly).
- Value coverage: `docker/test_companion_tank_value.{cpp,py}`.
- Runtime fixture: `docker/test_bot_tank.py` (disposable lab; two-target
  pull, taunt on owner threat with victim switch, pull-cap refusal, hold
  cancellation, survival and online assertions).

## Validation

Compile; disposable one- and two-creature pulls with normal threat inspection,
resource gates, extra-target refusal, hold cancellation and recovery. Record the
exact supported class/level/stance/spells and leave all other tanks unsupported.
