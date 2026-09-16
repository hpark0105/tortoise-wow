// PORT-016 (KAP-558): value-only damage policy for ONE declared melee
// damage class (Companion/Damage.h). Everything here is a pure function
// over values: no engine pointers, no sessions, no world access. The
// engine adapter (PlayerBotAI) fills one fresh observation each tick
// from live re-resolution (group, threat list, CC facts), applies the
// decision, and drives the engagement through the shared combat
// executor (Companion/Combat.h Source::Damage); the offense step is the
// common cast-or-attack path (the per-class spell table IS the declared
// damage matrix), and every cast is reported through the PORT-012
// cast-result vocabulary. A rejected cast falls back to the ordinary
// attack in the same evaluation, never a delayed one.
//
// DECLARED MATRIX (the only supported damage build; every other class
// is unsupported and never runs this policy):
//   class   Rogue (4)
//   level   1 (1752 Sinister Strike and 2098 Eviscerate are baseLevel 1
//             in the pinned spell data)
//   spells  { 2098 Eviscerate (energy 30), 1752 Sinister Strike
//             (energy 40), 703 Garrote (energy 50, baseLevel 14, bleed
//             aura, stances 0x20000000 = stealth-only in the pinned
//             1.12 data) }. The gate is the learned-spell book
//             (character_spell / HasSpell); AutoLearnSpellsForLevel
//             fills it at login. Garrote can only be cast from stealth
//             and the companion never stealths: in the bot flow it is a
//             KNOWN-BUT-CURRENTLY-UNUSABLE entry - selection excludes
//             it (a cast would be rejected by the core and fall back to
//             the ordinary attack in the same evaluation).
//   on-next-swing abilities: NONE. None of the three carries
//             SPELL_ATTR_ON_NEXT_SWING_1/2 (attributes bits 0x4/0x400)
//             in the pinned spell data, so per the PORT-012
//             UNSUPPORTED classification none may be required and the
//             declared rotation is fully supported.
//
// PULL MODEL (measured in the pinned core; the same integer math as
// Companion/Tank.h):
//   * The declared tank (Companion/Tank.h matrix) owns the pull. The
//     damage companion targets only the tank's current victim and never
//     acquires any other hostile: "nearest hostile" is not a rule of
//     this policy.
//   * The victim switches to a new attacker only above 1.1x the current
//     victim's threat (ThreatContainer::selectNextVictim). The tank's
//     pull is therefore established when the tank is the target's
//     current victim, or its stored threat reaches the 1.1x switch
//     margin against the current victim's threat (the core switches the
//     victim to the tank at that point).
//
// CROWD CONTROL PRESERVATION:
//   * If the established target holds any controlling aura from any
//     source (the adapter maps the pinned core AuraType set: stun,
//     root, charm, confuse, fear, pacify, transform, feign death) it is
//     "controlled", and the damage source neither engages it nor keeps
//     it. Ambiguity is resolved conservatively: controlled means a
//     bounded wait/follow, never damage.
//
// FAILURE CASES: hold, owner loss, missing tank, missing established
// target, insufficient threat, controlled target and unreachable target
// are all bounded wait/follow outcomes (the policy returns None and the
// prior order resumes); the policy never resolves ambiguity by
// selecting a hostile.
#ifndef TORTOISE_COMPANION_DAMAGE_H
#define TORTOISE_COMPANION_DAMAGE_H
#include <cstdint>
#include <type_traits>

namespace Companion
{
namespace Damage
{

// The declared matrix as bounded values (the adapter maps them onto the
// engine: class id, spell id and level are stable DBC/enum values).
inline constexpr uint32_t kDeclaredDamageClass = 4;  // CLASS_ROGUE
inline constexpr uint32_t kDeclaredDamageMinLevel = 1; // 1752/2098 baseLevel
inline constexpr uint32_t kDeclaredDamageEviscerate = 2098;
inline constexpr uint32_t kDeclaredDamageSinisterStrike = 1752;
inline constexpr uint32_t kDeclaredDamageGarrote = 703;
inline constexpr bool kDamageOnNextSwing = false;   // declared: none
// Engagement distance: the damage companion engages only within the
// tank's pull; beyond it the companion keeps waiting/following. The
// shared executor enforces the same cap on the engagement request.
inline constexpr float kEngageDistance = 35.0f;
// Victim-switch margin from ThreatContainer::selectNextVictim: a new
// attacker takes the victim above 1.1x the current victim's threat in
// melee. Kept as integer parts (11/10) so the value math never touches
// floats (the same convention as Companion/Tank.h).
inline constexpr uint32_t kSwitchMarginNum = 11;
inline constexpr uint32_t kSwitchMarginDen = 10;

inline constexpr uint32_t kDamageVersion = 2;

// One fresh per-tick damage evaluation of a declared damage companion.
// The adapter fills it from the live world each tick and never retains
// anything across ticks.
struct Observation
{
    uint32_t version = kDamageVersion;
    uint32_t damageGuid = 0;   // the declared damage companion itself
    uint32_t tankGuid = 0;     // declared tank in the party (0 = none)
    uint32_t targetGuid = 0;   // established target: the tank's current victim
    uint64_t targetRaw = 0;    // packed object GUID for the shared executor
                               // (adapter contract; the policy reasons on targetGuid)
    uint32_t tankThreat = 0;   // stored threat: the target on the tank
    uint32_t victimThreat = 0; // stored threat: the target on its current victim
                               // (0 when the tank is the victim or there is none)
    float distance = 0.0f;     // damage companion to the target
    bool targetHasThreatList = false; // the target keeps a threat list
    bool tankIsVictim = false;  // the tank is the target's current victim
    bool targetUnderCC = false;  // the target holds a controlling aura
    bool canAttack = false;      // the target is attackable by the companion
    bool inLos = false;          // the target is within line of sight
    bool held = false;           // an owner hold is active (suppression)
    bool ownerAvailable = false; // the follow owner resolved available this tick
};

static_assert(std::is_trivially_copyable<Observation>::value &&
              std::is_standard_layout<Observation>::value,
              "damage observation must be a bounded value type");

// The tank's pull is established when the tank is the target's current
// victim, or its stored threat is at or above the victim-switch margin
// against the current victim's threat. A missing tank, a missing
// target, no threat list, or a tank holding no threat (while not the
// victim) never establishes a pull.
inline bool PullEstablished(Observation const& o)
{
    if (o.tankGuid == 0 || o.targetGuid == 0 || !o.targetHasThreatList)
        return false;
    if (o.tankIsVictim)
        return true;
    if (o.tankThreat == 0)
        return false;
    return (uint64_t)o.tankThreat * kSwitchMarginDen >=
           (uint64_t)o.victimThreat * kSwitchMarginNum;
}

enum class Action { None, Damage };

struct Decision
{
    Action action = Action::None;
};

// The deterministic damage decision. Every failure case (hold, owner
// loss, missing tank or established target, pull not established,
// controlled target, unreachable) is a bounded wait/follow: the policy
// returns None and never selects a target of its own - the only
// candidate is the tank's current victim, so "nearest hostile" is
// never an outcome of this policy.
inline Decision Select(Observation const& o)
{
    if (o.held || !o.ownerAvailable)
        return {Action::None};
    if (o.tankGuid == 0 || o.targetGuid == 0)
        return {Action::None};
    if (!PullEstablished(o))
        return {Action::None};
    if (o.targetUnderCC)
        return {Action::None};
    if (!o.canAttack || !o.inLos || o.distance > kEngageDistance)
        return {Action::None};
    return {Action::Damage};
}

} // namespace Damage
} // namespace Companion
#endif