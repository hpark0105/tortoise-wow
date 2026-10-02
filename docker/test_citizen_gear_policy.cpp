// Value-level checks for citizen equipment quality probabilities.
#include "CitizenGearPolicy.h"

#include <cstdint>
#include <cstdio>

namespace {
int failures = 0;

void Check(bool condition, char const* label)
{
    if (!condition)
    {
        std::printf("FAIL: %s\n", label);
        ++failures;
    }
}
}

int main()
{
    using Companion::CitizenGearPolicy::QualityForRoll;
    using Companion::CitizenGearPolicy::QualityTier;

    std::uint32_t blue = 0;
    std::uint32_t green = 0;
    std::uint32_t common = 0;
    for (std::uint32_t roll = 0; roll < 100; ++roll)
    {
        switch (QualityForRoll(roll))
        {
            case QualityTier::Blue: ++blue; break;
            case QualityTier::Green: ++green; break;
            case QualityTier::Common: ++common; break;
        }
    }
    Check(blue == 15, "quality roll: exactly 15 percent blue");
    Check(green == 45, "quality roll: exactly 45 percent green");
    Check(common == 40, "quality roll: remaining 40 percent common");
    Check(QualityForRoll(14) == QualityTier::Blue, "quality roll: blue upper boundary");
    Check(QualityForRoll(15) == QualityTier::Green, "quality roll: green lower boundary");
    Check(QualityForRoll(59) == QualityTier::Green, "quality roll: green upper boundary");
    Check(QualityForRoll(60) == QualityTier::Common, "quality roll: common lower boundary");
    Check(QualityForRoll(100) == QualityTier::Blue, "quality roll: wraps at 100");

    using Companion::CitizenGearPolicy::FallbackQualityAt;
    Check(FallbackQualityAt(3, 0) == 3 && FallbackQualityAt(3, 1) == 2 &&
          FallbackQualityAt(3, 2) == 1 && FallbackQualityAt(3, 3) == 0,
          "blue target fallback order");
    Check(FallbackQualityAt(2, 0) == 2 && FallbackQualityAt(2, 1) == 3 &&
          FallbackQualityAt(2, 2) == 1 && FallbackQualityAt(2, 3) == 0,
          "green target fallback order");
    Check(FallbackQualityAt(1, 0) == 1 && FallbackQualityAt(1, 1) == 2 &&
          FallbackQualityAt(1, 2) == 3 && FallbackQualityAt(1, 3) == 0,
          "normal target fallback order");
    Check(FallbackQualityAt(1, 4) == 0xffffffffu, "fallback out-of-range sentinel");
    using Companion::CitizenGearPolicy::IsEligibleEffectiveLevel;
    Check(IsEligibleEffectiveLevel(14, 14), "level 14 gear is eligible");
    Check(IsEligibleEffectiveLevel(14, 11), "three levels below remains eligible");
    Check(!IsEligibleEffectiveLevel(14, 10), "more than three levels below is excluded");
    Check(!IsEligibleEffectiveLevel(14, 15), "above-level gear is excluded");

    using Companion::CitizenGearPolicy::ExistingItemAtLeastAsGood;
    Check(ExistingItemAtLeastAsGood(2, 8, 1, 14),
          "equipped green is retained over higher-item-level gray");
    Check(ExistingItemAtLeastAsGood(3, 5, 2, 14),
          "equipped blue is retained over higher-item-level green");
    Check(ExistingItemAtLeastAsGood(2, 14, 2, 13),
          "same-quality higher-item-level gear is retained");
    Check(!ExistingItemAtLeastAsGood(1, 10, 2, 8),
          "higher-quality candidate can replace lower-quality gear");
    Check(!ExistingItemAtLeastAsGood(2, 10, 2, 11),
          "same-quality higher-item-level candidate can replace gear");

    if (failures)
        return 1;
    std::puts("gear policy tests: ALL OK");
    return 0;
}
