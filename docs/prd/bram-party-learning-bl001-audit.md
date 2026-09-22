# BL-001 -- Bram tank rage-reserve capability audit

Status: AUDIT COMPLETE -- decision rendered (section 7).
Card: BL-001 (PRD section 9 row 1; PRD section 3).
Scope: read-only capability/experiment audit. No runtime behavior changed, no
C++ edited, no level changed, no ability invented, no scope widened. This is a
local-head (park-head) direct-read audit; no nested worker was launched and no
hosted-head participation is claimed.

Re-verified against current source on 2026-09-21 (park-head):
  - src/game/PlayerBots/Companion/Tank.h
      SHA-256 01D1C6515DA1B961B74EC82D409221A0C2AADD0473F1AD2879A60E9FAEFB50D3
      (matches the PRD-pinned value)
  - src/game/PlayerBots/PlayerBotAI.cpp
      SHA-256 4CE3B770C07627EBDFA828CF3FA659BB1142E20E445B166D9A519E1821F2FAA5
      (matches the PRD-pinned value)
  - Live enrolled tank (tw_char, read at audit time): level 9, class 1.
Baseline: tortoise-wow, branch feature/kap-558-port-phase2. The two commits since
the PRD baseline (016045a Item.cpp crash fix, 791a76a gear migration) do not touch
these files, so the PRD-pinned content is the current content.

## 1. Method and provenance

Read-only inspection of current source plus matching game data. Live level and the
learned-spell book were read from live data, not inferred from docs. This is a
pinned-client build: spell data lives in data/dbc/Spell.dbc (the SQL mirror
tw_world.spell is EMPTY), and the learned-spell book is tw_char.character_spell
(what Player::HasSpell(355) reads). The Taunt 355 DBC row is documented in Tank.h.
Per PRD section 3 the enrolled identity was resolved once from live data and is not
hardcoded into implementation.

## 2. Verified capability matrix (source: Companion/Tank.h)

The only supported tank build (every other tank runs the ordinary offense path):
  - class Warrior (1), level >= 10 (Taunt 355 baseLevel 10).
  - Gate = learned-spell book plus level: HasSpell(355) && level >= 10
    (PlayerBotAI.cpp:1361 IsDeclaredTank).
  - Action set: { NormalAttack, Taunt(355) } ONLY (Tank.h:106).
  - Taunt 355: instant, 0 rage, melee range, category 82 / 10 s recovery,
    effect 114 (EffectTaunt: threat catch-up + setCurrentVictimIfCan + 1.11
    debuff). Higher-rank "Taunt" rows (7390..11590) are effect 63 (additive
    threat only, no victim change) and are NOT taunts (Tank.h:29-37).
  - tauntUsable = HasSpell(355) && !HasSpellCooldown(355) && in-LOS && melee-
    reachable (PlayerBotAI.cpp:1390). It is gated by learned/cooldown/range,
    NEVER by rage.
  - On-next-swing abilities: NONE (Tank.h:38-41, kTauntOnNextSwing = false).
  - Pull cap 2; victim-switch margin 1.1x (melee); the tank never taunts while it
    is already the victim (EffectTaunt no-op rejection).
SelectAction (Tank.h:131): returns Taunt only when (tauntUsable && OwnerHolds-
Threat), else NormalAttack; a rejected or un-usable taunt degrades to the ordinary
attack in the same evaluation (never a delayed one).

## 3. Enrolled tank live state (read from live data at audit time)

Bram (the enrolled tank; identity resolved for this audit, not hardcoded into
implementation):
  - level 9, class 1 (warrior), race 1 (human).
  - learned-spell book (tw_char.character_spell): {100, 284, 772, 1715, 3127,
    6343, 21156, 45584}.
  - Decisive absences: NO 355 (Taunt); NO 78 (Heroic Strike); NO 7372
    (Hamstring); NO 871 (Shield Wall); NO 2565 (Shield Block). None of the
    present spells is a rage-gated, threat-maintaining protective/recovery action.
  - IsDeclaredTank() = FALSE (level 9 < 10 AND !HasSpell(355)).
Consequence: the declared-tank Assist branch (the only place threat recovery
happens) is NOT currently active for Bram. The capability window (level 10+ with
Taunt 355) is reachable by progression but is not his current state; this
experiment must not change his level (PRD section 3).

## 4. Path x gate selector map

Routing (PlayerBotAI.cpp ExecuteCombat, line 4498):
    if (source == Assist && IsDeclaredTank())  -> Tank branch (Tank::SelectAction)
    else if (!_abilityTimer && inLOS)          -> TryOffensiveCastOrAttack
                                                  -> SelectOffensiveSpell
    else                                       -> white attack
Only "Assist + declared-tank" uses the tank branch (PlayerBotAI.cpp:4498-4510).
All other sources, and below-gate Assist, use the general selector.
  Path            Above gate (lvl10+,Taunt)   Below gate (current, lvl9)
  Assist          Tank branch (Taunt/white)   General selector
  ContinueCombat  General selector            General selector
  Defend          General selector            General selector
The general selector (SelectOffensiveSpell, PlayerBotAI.cpp:1700; warrior list at
1717) for a warrior is { Charge(100), Hamstring(7372), Rend(772), Heroic Strike(78) }
-- all offensive damage abilities, all rage-costing (exact DBC PowerType/cost to be
pinned in the BL-001A fixtures; Spell.dbc-only data, no SQL mirror). NONE is a
protective or threat-recovery action.

## 5. Causal-mechanism analysis

Primary metric (PRD section 5, line 263): unprotected_fraction = hostile-time
attacking another party member / hostile-time attacking any party member. It is a
threat/protection metric (lower is better). Supporting metric (line 267):
rage_blocked_recovery_count = verified recovery opportunities blocked by rage;
"if none exist, do not claim benefit."

For reserve_rage (hold rage; suppress verified optional rage-spending offense) to
improve unprotected_fraction, retaining rage must ENABLE a SUPPORTED action that
improves protection. Every path was examined:
  1. The only threat-recovery action in the supported matrix is Taunt (355), which
     costs 0 rage and is gated by tauntUsable (learned/cooldown/range), never by
     rage. A rage reserve cannot enable or block it, so rage_blocked_recovery_count
     is structurally 0 in this matrix.
  2. Taunt is reachable only in the Assist + declared-tank branch, whose action set
     {Taunt, white attack} spends NO rage at all. A reserve added to the general
     selector cannot tune that branch (PRD section 3: "adding a reserve to the
     generic selector must not be presented as tuning that branch").
  3. In the general selector (ContinueCombat / Defend, and below-gate Assist) the
     rage spenders are OFFENSIVE. Suppressing them -- the reserve's only effect --
     reduces offensive threat generation and damage; it enables NO protective
     action. It cannot lower unprotected_fraction and is at best neutral, at worst
     harmful (less threat/DPS, longer engagements).
  4. No on-next-swing ability is declared (Tank.h:38-41), so "hold rage for a queued
     next-swing protective" is also unsupported (PRD section 3: queued on-next-swing
     abilities remain unsupported).
  5. Self-mitigation candidates (Shield Wall 871, Shield Block 2565) are rage-gated
     but reduce damage to Bram; they do not change which member the hostile targets,
     so they do not directly lower unprotected_fraction (and are not learned by the
     enrolled tank at level 9 anyway).
Conclusion: within the supported capability window, retaining rage has NO causal
path to unprotected_fraction. A legitimate optional rage spender exists (the
offense), but there is NO measurable reason retaining rage helps the primary metric.

## 6. Game-data references (exact rows / to be pinned)
  - Taunt 355 (DBC, documented in Tank.h): instant, 0 rage, melee, category 82 /
    10 s recovery, effect 114. <- the decisive protective-action row (0 rage).
  - Enrolled tank book (live character_spell): {100, 284, 772, 1715, 3127, 6343,
    21156, 45584}; 355 / 78 / 7372 / 871 / 2565 absent (level 9).
  - To be pinned from Spell.dbc in the BL-001A fixtures (DBC-only, no SQL mirror):
    PowerType + cost for 100 / 7372 / 772 / 78 (offense), and 871 / 2565
    (self-mitigation candidates) for the prerequisite evaluation. Enemy level bounds
    for eligibility (PRD section 5, line 289) also must be pinned from validated
    fixtures.

## 7. Decision (BL-001 gate)

NOT VIABLE as specified. reserve_rage has no supported causal path to the primary
metric unprotected_fraction:
  (a) the supported tank matrix has no rage-costing protective action; the sole
      recovery (Taunt 355) is 0-rage, so rage_blocked_recovery_count is
      structurally 0 (PRD section 5: "if none exist, do not claim benefit");
  (b) the only rage spenders are offensive; suppressing them cannot improve
      protection;
  (c) the enrolled tank is currently below the capability window (level 9, no
      Taunt 355), so the declared-tank branch -- the only place protection happens
      -- is not active.
This is the "no such action" branch of PRD section 3 (line 119): return a bounded
prerequisite proposal to the head; do not invent an ability or widen the
experiment. Observe-only mode remains useful (PRD section 3, line 120).

## 8. Bounded prerequisite proposal (to the head)

Do not invent a mechanic or widen the experiment. To make a rage-reserve
experiment viable, establish FIRST, as a separately-specified and
separately-accepted behavior change (PRD section 3, line 124-126: "If adding
actions to tank Assist is needed, specify and accept that behavior change
separately ... establish a new versioned baseline before evaluating learning
against it"):
  1. Add a rage-costing protective/recovery action to the supported tank matrix
     such that (i) its use is gated on holding rage and (ii) enabling it by
     retaining rage measurably improves unprotected_fraction (or yields non-zero
     rage_blocked_recovery_count). No existing pinned warrior spell is both
     rage-gated and threat-maintaining (the threat tool is the 0-rage Taunt), so
     this likely requires either a newly-defined protective action or a
     redefinition of the primary metric (for example tank survival / self-
     mitigation), both out of this audit's scope and to be accepted separately.
  2. Establish a new versioned baseline including that action before evaluating any
     reserve against it.
  3. Reach the capability window: the enrolled tank must be a level-10+ warrior
     with Taunt 355 (progression; out of experiment scope). Until then, observe-
     only baseline recording is the correct state.

## 9. What remains useful now
  - Observe-only mode (PRD section 3, line 120): the encounter recorder can enroll
    and record baseline metrics (unprotected_fraction, recovery_latency_ms, rage)
    under the current (non-tank) behavior, pinning the DBC/fixture facts, without
    running the (not-viable) reserve experiment.
  - The source/game-data findings here are reusable by BL-001A (baseline
    extraction) regardless of the reserve outcome (handoff "What exists today").
  - No existing Phase 2 acceptance flag is changed by this audit.

## 10. Source citations (verified current lines)
  - Companion/Tank.h: declared matrix 66-84; Observation 89-100; Action 106;
    SelectAction 131-140.
  - PlayerBotAI.cpp: IsDeclaredTank 1361-1368; FillTankObservation 1370-1395
    (tauntUsable 1390-1393); TankTauntStep 1397+; SelectOffensiveSpell 1700
    (warrior 1717-1723); ExecuteCombat routing 4498-4514.
  - PRD (docs/prd/bram-party-learning.prd): section 3 (91-134), section 5
    (248-291), section 9 (370-409).