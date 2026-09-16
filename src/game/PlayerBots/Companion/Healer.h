// PORT-015 (KAP-558): value-only healer triage policy for ONE declared
// companion build. Everything here is a pure function over values: no
// engine pointers, no sessions, no world access. The engine adapter
// (PlayerBotAI) fills one fresh observation each tick from the live world
// (party slots, health, mana, range/LOS facts), applies the decision, and
// reports the cast outcome through the PORT-012 cast vocabulary
// (Companion/Combat.h ReportCast); a rejected or blocked heal produces the
// next legal triage/follow outcome in the same evaluation - never a
// delayed one.
//
// DECLARED MATRIX (the only supported healer build; every other companion
// keeps the ordinary companion path untouched):
//   class   Druid (11)
//   level   12 (Regrowth 8936 baseLevel 12; maxLevel 17)
//   spells  { 8936 Regrowth } only, the declared matrix constant.
//             The gate is the learned-spell book: Player::HasSpell(8936)
//             reads character_spell (spell ids, loaded at login by
//             _LoadSpells). In the pinned renumbered DBC 8936 is
//             "Regrowth": effect1 10 (SPELL_EFFECT_HEAL), heal 83 + d15,
//             96 mana, 0 recovery / 0 category recovery, castingTimeIndex
//             5 (2000 ms), rangeIndex 5 (0-40 yd), interruptFlags 15
//             (MOVEMENT | DAMAGE | EMOTE | UNKNOWN: an in-progress cast is
//             interrupted by moving more than 0.5 yd - the adapter holds
//             the healer stationary for the cast window and keeps the
//             follow goal from re-issuing movement while the cast is in
//             flight; auraInterruptFlags is 0). Higher ranks
//             (8937+... level 18+) and the
//             instant Swiftmend line are out of the declared window and
//             are never advertised or cast by this policy.
//   on-next-swing abilities: NONE (Regrowth carries no
//             SPELL_ATTR_ON_NEXT_SWING attribute), per the PORT-012
//             UNSUPPORTED classification none may be required.
//
// TRIAGE MODEL:
//   * A slot is injured below the declared 70% health threshold
//     (per-mille integer math, no floats in the comparison).
//   * Declared priority: owner (the follow leader) > self > other party
//     members, most-injured first (lowest health fraction), ties broken
//     by the lower GUID so the selection is total and deterministic.
//   * A cast is affordable only while the post-cast mana stays at or
//     above the declared flat reserve: the reserve is what keeps one
//     declared small pull (the PORT-014 two-mob cap) sustainably healed.
//     cost + reserve > mana is a bounded safe outcome (no cast), not an
//     error and never a delayed retry of the same failed decision.
//   * Range/LOS are per-slot facts the adapter re-resolves from the live
//     world each tick; an out-of-range injured slot is ineligible this
//     tick and the follow goal (the adapter's behavior path) is what
//     regains range - the policy never requests a teleport.
//   * The adapter re-validates the whole decision against live state
//     immediately before casting (stale slot, map flap, cooldown/mana
//     flap); a blocked cast is a bounded skip and the next tick re-triages.
//   * One flight: an in-flight heal sits in the generic spell container
//     (state PREPARING during the cast time), and the adapter's busy fact
//     is the withDelayed form of IsNonMeleeSpellCasted; a second cast is
//     refused until the cast completes (the core would otherwise replace
//     the in-flight cast - the re-accept storm this card pins against).
//   * Hold position: on an accepted cast the adapter stops the walk in
//     progress, and the follow goal holds position while the cast is in
//     flight (the declared heal is movement-interruptible); the behavior
//     path resumes on the first tick after the cast ends.
#ifndef TORTOISE_COMPANION_HEALER_H
#define TORTOISE_COMPANION_HEALER_H
#include <cstdint>
#include <type_traits>

namespace Companion
{
namespace Healer
{

// The declared matrix as bounded values (the adapter maps them onto the
// engine: class id, spell id and level are stable DBC/enum values).
inline constexpr uint32_t kDeclaredHealerClass = 11;  // CLASS_DRUID
inline constexpr uint32_t kDeclaredHealerMinLevel = 12; // Regrowth 8936 baseLevel
// The declared heal: the learned-spell book key (character_spell /
// HasSpell), the CastSpell key and the HasSpellCooldown key all use the
// same spell id in this build.
inline constexpr uint32_t kDeclaredHealerHeal = 8936;  // Regrowth (12-17)
inline constexpr uint32_t kDeclaredHealCost = 96;      // 8936 manaCost
// rangeIndex 5 of the pinned SpellRange data: 0-40 yd. The policy only
// casts strictly inside this radius; the core's own cast check adds a
// small player leeway on top, so staying inside the declared radius is
// always inside the core's acceptance.
inline constexpr uint32_t kDeclaredHealRangeYd = 40;
// Heal below this health fraction (per mille): 700 = 70%.
inline constexpr uint32_t kDeclaredHealMinPerMille = 700;
// Flat post-cast mana reserve (see TRIAGE MODEL above).
inline constexpr uint32_t kDeclaredManaReserve = 32;
// Self + up to four party members (a 1.12 party is five).
inline constexpr uint32_t kMaxSlots = 5;

inline constexpr uint32_t kHealerVersion = 1;

// One triage candidate. The adapter fills it from live re-resolution and
// never retains anything across ticks; an empty slot has guid 0.
struct Slot
{
    uint32_t guid = 0;
    uint32_t hp = 0;
    uint32_t maxHp = 0;
    bool alive = false;
    // Live range/LOS facts for THIS tick: a slot on another map (or not
    // line-of-sight) is inLos false and never castable through triage.
    bool inLos = false;
    float distance = 0.0f;
    bool isOwner = false; // the follow leader (the protected member)
    bool isSelf = false;
};

// One fresh triage evaluation of a declared healer. The adapter fills one
// each tick; policies never see the engine and never retain anything.
struct Observation
{
    uint32_t version = kHealerVersion;
    uint32_t mana = 0;
    uint32_t maxMana = 0;
    uint32_t spellCost = kDeclaredHealCost;
    uint32_t manaReserve = kDeclaredManaReserve;
    // The authoritative live range (the adapter reads it from the pinned
    // range data); the declared constant is the default.
    uint32_t healRangeYd = kDeclaredHealRangeYd;
    // Engine cast facts: learned, off cooldown, and not casting or
    // channeling anything. A false here is a bounded no-cast outcome.
    bool canCast = false;
    Slot slots[kMaxSlots];
};

static_assert(std::is_trivially_copyable<Observation>::value &&
              std::is_standard_layout<Observation>::value,
              "healer observation must stay a bounded value type");

enum class Action { None, SelfHeal, PartyHeal };

struct Decision
{
    Action action = Action::None;
    uint32_t target = 0; // 0 for SelfHeal/None
};

// Injured below the declared threshold (per-mille integer math).
inline bool Injured(Slot const& s)
{
    return s.guid != 0 && s.alive && s.maxHp != 0 &&
           (uint64_t)s.hp * 1000 < (uint64_t)s.maxHp * kDeclaredHealMinPerMille;
}

// Castable through triage this tick: line of sight strictly inside the
// declared radius. Self is trivially in range (the adapter fills it with
// inLos true / distance 0, and the checks below stay uniform).
inline bool Castable(Slot const& s, uint32_t rangeYd)
{
    return s.inLos && s.distance < (float)rangeYd;
}

// Affordable while the post-cast mana stays at or above the reserve.
inline bool Affordable(uint32_t mana, uint32_t cost, uint32_t reserve)
{
    return cost + reserve <= mana;
}

// Deterministic triage. The declared priority is owner > self > other
// (most injured first, ties by lower GUID). No cast is ever taken on
// incomplete evidence (a slot must be alive, injured and in range) and
// never while the heal is not affordable or not castable - the fallback
// is the same evaluation (the next legal outcome is None, so the
// behavior path - follow - resumes immediately).
inline Decision Select(Observation const& o)
{
    if (!o.canCast || !Affordable(o.mana, o.spellCost, o.manaReserve))
        return {Action::None, 0};
    const Slot* owner = nullptr;
    const Slot* self = nullptr;
    const Slot* best = nullptr;
    for (uint32_t i = 0; i < kMaxSlots; ++i)
    {
        Slot const& s = o.slots[i];
        if (!Injured(s) || !Castable(s, o.healRangeYd))
            continue;
        if (s.isSelf)
        {
            self = &s;
            continue;
        }
        if (s.isOwner)
        {
            owner = &s;
            continue;
        }
        // Most injured = lowest hp*1000/maxHp; cross-multiply to keep the
        // math integer; exact ties go to the lower GUID (total order).
        if (!best ||
            (uint64_t)s.hp * best->maxHp < (uint64_t)best->hp * s.maxHp ||
            ((uint64_t)s.hp * best->maxHp == (uint64_t)best->hp * s.maxHp &&
             s.guid < best->guid))
            best = &s;
    }
    if (owner)
        return {Action::PartyHeal, owner->guid};
    if (self)
        return {Action::SelfHeal, 0};
    if (best)
        return {Action::PartyHeal, best->guid};
    return {Action::None, 0};
}

} // namespace Healer
} // namespace Companion
#endif
