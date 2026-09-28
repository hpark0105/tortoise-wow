// Value-only arrival decision for zone-citizen progression routes
// (TW-BOTS-002 S1).
//
// The decision scores an already-computed approach (see PlayerBotAI::
// AdvanceProgressionTravel) from plain flags supplied by the caller. It
// owns no engine state, takes no engine pointers and depends only on
// standard C++17 headers.
#ifndef TORTOISE_WOW_COMPANION_CITIZEN_TRAVEL_H
#define TORTOISE_WOW_COMPANION_CITIZEN_TRAVEL_H

#include <cmath>

namespace Companion {
namespace CitizenTravel {

// Distance from the selected hunting anchor at which the approach is close
// enough to require the usable-ground verification.
constexpr float kArrivalRadiusYards = 12.0f;

enum class ArrivalDecision
{
    Continue,            // keep walking toward the anchor
    ArrivedAnchor,       // at the anchor on verified usable ground
    BlockedNearAnchor,   // near an unusable anchor, outside the expected zone
    MapLost              // expected map no longer matches; abort and defer
};

struct ArrivalInput
{
    bool sameMap = true;
    bool inTargetZone = false;
    float distanceToAnchorYards = 0.0f;
    // Only consulted inside kArrivalRadiusYards: the anchor ground resolves
    // to finite terrain and a short walk from the citizen to the anchor
    // succeeds.
    bool anchorWalkable = false;
};

// Arrival requires the expected map and zone, a finite nonnegative
// distance inside the radius, and verified usable ground. Zone entry or
// an invalid/unreachable anchor never counts as successful travel.
inline ArrivalDecision DecideArrival(ArrivalInput const& in)
{
    if (!in.sameMap)
        return ArrivalDecision::MapLost;
    if (!std::isfinite(in.distanceToAnchorYards) || in.distanceToAnchorYards < 0.0f)
        return ArrivalDecision::BlockedNearAnchor;
    if (in.distanceToAnchorYards >= kArrivalRadiusYards)
        return ArrivalDecision::Continue;
    return in.inTargetZone && in.anchorWalkable ? ArrivalDecision::ArrivedAnchor
                                               : ArrivalDecision::BlockedNearAnchor;
}

} // namespace CitizenTravel
} // namespace Companion

#endif // TORTOISE_WOW_COMPANION_CITIZEN_TRAVEL_H
