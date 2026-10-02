// Small value policies for population-shared citizen migration evidence.
#ifndef TORTOISE_WOW_COMPANION_CITIZEN_MIGRATION_HINT_H
#define TORTOISE_WOW_COMPANION_CITIZEN_MIGRATION_HINT_H

#include <algorithm>
#include <cstdint>

namespace Companion {
namespace CitizenMigrationHint {

// Bump when zone anchors, navigation content, or migration eligibility policy
// changes enough that old transition outcomes should no longer guide ranking.
constexpr uint8_t kEvidenceVersion = 1;
constexpr uint32_t kEvidenceMaxAgeSeconds = 90u * 24u * 60u * 60u;
constexpr uint32_t kMaximumEvidenceScore = 5;

inline uint32_t Score(uint32_t successes, uint32_t lastSuccess,
                      uint32_t now, uint8_t version)
{
    if (!successes || !lastSuccess || version != kEvidenceVersion ||
        lastSuccess > now || now - lastSuccess > kEvidenceMaxAgeSeconds)
        return 0;

    uint32_t const capped = std::min(successes, kMaximumEvidenceScore);
    uint32_t const age = now - lastSuccess;
    // Recent evidence gets full weight; older evidence fades before expiry.
    return age <= 30u * 24u * 60u * 60u ? capped : std::max<uint32_t>(1, capped / 2);
}

inline bool ShouldRecord(bool isCitizen, bool arrivedAtVerifiedAnchor,
                         bool observedWalkingProgress, uint32_t sourceZone,
                         uint32_t destinationZone)
{
    return isCitizen && arrivedAtVerifiedAnchor && observedWalkingProgress &&
           sourceZone != 0 && destinationZone != 0 &&
           sourceZone != destinationZone;
}

inline bool PreferCandidate(float travelYards, uint32_t levelGap, uint32_t hintScore,
                            float bestTravelYards, uint32_t bestLevelGap,
                            uint32_t bestHintScore, bool hasBest)
{
    if (!hasBest || travelYards < bestTravelYards ||
        (travelYards == bestTravelYards && levelGap < bestLevelGap) ||
        (travelYards == bestTravelYards && levelGap == bestLevelGap &&
         hintScore > bestHintScore))
        return true;
    return false;
}

} // namespace CitizenMigrationHint
} // namespace Companion

#endif
