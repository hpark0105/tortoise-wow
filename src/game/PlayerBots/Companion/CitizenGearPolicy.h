// Deterministic quality targets for the one-time and per-level citizen gear roll.
#ifndef TORTOISE_WOW_COMPANION_CITIZEN_GEAR_POLICY_H
#define TORTOISE_WOW_COMPANION_CITIZEN_GEAR_POLICY_H

#include <cstdint>

namespace Companion {
namespace CitizenGearPolicy {

constexpr std::uint8_t kPolicyVersion = 5u;
constexpr std::uint32_t kRollRange = 100u;
constexpr std::uint32_t kBlueGearPercent = 15u;
constexpr std::uint32_t kGreenGearPercent = 45u;
constexpr std::uint32_t kMaxEffectiveLevelGap = 3u;

enum class QualityTier : std::uint8_t
{
    Common,
    Green,
    Blue
};

inline QualityTier QualityForRoll(std::uint32_t roll)
{
    roll %= kRollRange;
    if (roll < kBlueGearPercent)
        return QualityTier::Blue;
    if (roll < kBlueGearPercent + kGreenGearPercent)
        return QualityTier::Green;
    return QualityTier::Common;
}

// WoW item quality values: poor 0, normal 1, uncommon 2, rare 3.
constexpr std::uint32_t FallbackQualityAt(std::uint32_t target, std::uint32_t rank)
{
    if (rank >= 4u)
        return 0xffffffffu;
    if (target == 3u)
    {
        constexpr std::uint32_t order[] = { 3u, 2u, 1u, 0u };
        return order[rank];
    }
    if (target == 2u)
    {
        constexpr std::uint32_t order[] = { 2u, 3u, 1u, 0u };
        return order[rank];
    }
    constexpr std::uint32_t order[] = { 1u, 2u, 3u, 0u };
    return order[rank];
}

constexpr bool IsEligibleEffectiveLevel(std::uint32_t citizenLevel, std::uint32_t effectiveLevel)
{
    return effectiveLevel <= citizenLevel && citizenLevel - effectiveLevel <= kMaxEffectiveLevelGap;
}

} // namespace CitizenGearPolicy
} // namespace Companion

#endif // TORTOISE_WOW_COMPANION_CITIZEN_GEAR_POLICY_H
