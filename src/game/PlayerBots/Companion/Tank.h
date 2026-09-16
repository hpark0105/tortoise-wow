// PORT-014 (KAP-558): value-only tank threat policy for ONE declared
// companion build. Everything here is a pure function over values: no
// engine pointers, no sessions, no world access. The engine adapter
// (PlayerBotAI) fills one fresh observation each offense evaluation from
// live re-resolution (threat list, victim state, cooldown facts), applies
// the decision, and reports the cast outcome through the PORT-012 cast
// vocabulary (Companion/Combat.h ReportCast); a rejected cast falls back
// to the ordinary attack in the same evaluation, never a delayed one.
//
// DECLARED MATRIX (the only supported tank build; every other tank is
// unsupported and runs the ordinary companion offense path):
//   class   Warrior (1)
//   level   10 (Taunt 355 baseLevel 10, maxLevel 0: legal at 10+)
//   stance  131072 (declared; 1.12 has no player stance state and the
//             core does not enforce spell Stances masks for players, so
//             the stance is mechanically inert)
//   spells  { 355 Taunt } only. The gate is the learned-spell book:
//             Player::HasSpell(355) reads the character_spell table
//             (spell ids, loaded at login by _LoadSpells; this build
//             has no character_known_spells). Note the pinned client's
//             DBC is renumbered: the SkillLineAbility row for spell 355
//             names skill line 257 ("Protection", classMask Warrior)
//             while skill line 355 is the warlock "Affliction" line
//             (classMask Warlock), so a character_skills row for 355
//             is rejected at login as a forbidden race/class
//             combination. Skill lines (HasSkill) are NOT the gate;
//             the fixture seeds character_spell 355 and, for matrix
//             fidelity, character_skills 257. In the pinned spell data
//             355 is the only true taunt: instant, 0 rage, melee range,
//             category 82 with
//             a 10 s category recovery, effect 114
//             (SPELL_EFFECT_ATTACK_ME -> Spell::EffectTaunt: threat
//             catch-up to the current victim plus setCurrentVictimIfCan
//             and the 1.11 taunt debuff). The higher-rank "Taunt" rows
//             (7390/7391/11588/11589/11590) carry effect 63
//             (SPELL_EFFECT_THREAT, additive threat only) and do NOT
//             force a victim change, so they are not taunts here.
//   on-next-swing abilities: NONE. Taunt 355 carries no
//             SPELL_ATTR_ON_NEXT_SWING attribute; the matrix therefore
//             declares no on-next-swing ability, per the PORT-012
//             UNSUPPORTED classification none may be required.
//
// THREAT MODEL (measured in the pinned core, ThreatManager):
//   * The victim switches to a new attacker only above 1.1x the current
//     victim's threat in melee (1.3x at range). "The owner holds threat"
//     is therefore owner threat at or above the 1.1x switch margin, or
//     the owner being the current victim outright.
//   * EffectTaunt is a no-op rejection when the tank is already the
//     victim, so the policy never taunts in that state.
//   * The 10 s category recovery (HasSpellCooldown) is the adapter's
//     tauntUsable gate: a cooled-down taunt reads as "not usable" and the
//     policy falls to the ordinary attack without attempting a cast that
//     the core would reject.
#ifndef TORTOISE_COMPANION_TANK_H
#define TORTOISE_COMPANION_TANK_H
#include <cstdint>
#include <type_traits>

namespace Companion
{
namespace Tank
{

// The declared matrix as bounded values (the adapter maps them onto the
// engine: class id, spell id and level are stable DBC/enum values).
inline constexpr uint32_t kDeclaredTankClass = 1;    // CLASS_WARRIOR
inline constexpr uint32_t kDeclaredTankMinLevel = 10; // Taunt 355 baseLevel
// The Taunt spell id: the learned-spell book key (character_spell /
// HasSpell), the CastSpell key and the HasSpellCooldown key all use
// the same spell id in this build.
inline constexpr uint32_t kDeclaredTankTaunt = 355;  // the only true taunt
inline constexpr bool kTauntOnNextSwing = false;     // declared: none
// Bounded pull: a declared tank may hold at most this many live hostile
// engagements; a new owner-selected assist beyond the cap is refused
// manager-side (no uncontrolled adds).
inline constexpr uint32_t kDeclaredTankPullCap = 2;
// Victim-switch margin from ThreatContainer::selectNextVictim: a new
// attacker takes the victim above 1.1x the current victim's threat in
// melee. Kept as integer parts (11/10) so the value math never touches
// floats.
inline constexpr uint32_t kSwitchMarginNum = 11;
inline constexpr uint32_t kSwitchMarginDen = 10;

inline constexpr uint32_t kTankVersion = 1;

// One fresh offense evaluation of a declared tank in combat with an
// owner-selected target. The adapter fills it from the live world each
// evaluation and never retains anything across evaluations.
struct Observation
{
    uint32_t version = kTankVersion;
    uint32_t tankGuid = 0;
    uint32_t ownerGuid = 0;     // the protected party member (follow leader)
    uint32_t targetGuid = 0;    // the owner-selected hostile
    uint32_t victimGuid = 0;    // the target's current victim (0 = none)
    uint32_t tankThreat = 0;    // stored threat: the target on the tank
    uint32_t ownerThreat = 0;   // stored threat: the target on the owner
    bool targetHasThreatList = false; // the target keeps a threat list
    bool tauntUsable = false;   // learned, off cooldown, melee-reachable
};

static_assert(std::is_trivially_copyable<Observation>::value &&
              std::is_standard_layout<Observation>::value,
              "tank observation must stay a bounded value type");

enum class Action { None, NormalAttack, Taunt };

struct Decision
{
    Action action = Action::None;
};

// The protected member holds threat when it is the current victim or its
// stored threat reaches the victim-switch margin against the tank.
inline bool OwnerHoldsThreat(Observation const& o)
{
    if (o.ownerThreat == 0)
        return false;
    if (o.ownerGuid != 0 && o.victimGuid == o.ownerGuid)
        return true;
    return (uint64_t)o.ownerThreat * kSwitchMarginDen >=
           (uint64_t)o.tankThreat * kSwitchMarginNum;
}

// Deterministic tank decision. No taunt is ever taken on incomplete
// evidence (missing target or no threat list) and never while the tank
// already is the victim (EffectTaunt would reject it as a no-op); a
// not-usable taunt (cooldown, range, not learned) degrades to the
// ordinary attack immediately - the fallback is the same evaluation,
// never a delayed one.
inline Decision SelectAction(Observation const& o)
{
    if (o.tankGuid == 0 || o.targetGuid == 0 || !o.targetHasThreatList)
        return {Action::NormalAttack};
    if (o.victimGuid == o.tankGuid)
        return {Action::NormalAttack};
    if (o.tauntUsable && OwnerHoldsThreat(o))
        return {Action::Taunt};
    return {Action::NormalAttack};
}

} // namespace Tank
} // namespace Companion
#endif
