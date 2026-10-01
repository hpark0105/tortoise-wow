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
#include <cstdint>

namespace Companion {
namespace CitizenTravel {

// Distance from the selected hunting anchor at which the approach is close
// enough to require the usable-ground verification.
constexpr float kArrivalRadiusYards = 12.0f;
constexpr float kMinimumPartialProgressYards = 35.0f;
constexpr float kWaypointSpacingYards = 200.0f;
constexpr float kRepeatedPartialEndpointToleranceYards = 20.0f;
constexpr uint8_t kMaximumPartialRouteReplans = 16;
constexpr uint32_t kBlockedAnchorCooldownMs = 5 * 60 * 1000;

enum class RouteFailureReason : uint8_t
{
    None = 0,
    NoPath = 1,
    Stalled = 2,
    AnchorBlocked = 3
};

enum class PathKind : uint8_t
{
    Complete,
    Partial,
    Unsupported
};

enum class PathDecision : uint8_t
{
    Follow,
    FollowPartial,
    Reject
};

struct PathEvidence
{
    PathKind kind = PathKind::Unsupported;
    float pathLengthYards = 0.0f;
    float progressYards = 0.0f;
};

inline PathDecision DecidePath(PathEvidence const& evidence)
{
    if (evidence.kind == PathKind::Unsupported ||
        !std::isfinite(evidence.pathLengthYards) || evidence.pathLengthYards <= 0.0f ||
        !std::isfinite(evidence.progressYards) || evidence.progressYards < 0.0f)
        return PathDecision::Reject;
    if (evidence.kind == PathKind::Complete)
        return PathDecision::Follow;
    if (evidence.progressYards < kMinimumPartialProgressYards)
        return PathDecision::Reject;
    return PathDecision::FollowPartial;
}

inline bool CanResumeRoute(uint8_t intent, uint32_t targetMap, uint32_t currentMap,
                           uint32_t targetZone, uint32_t currentZone,
                           uint32_t productiveShare, uint32_t minimumProductiveShare)
{
    // A citizen can already have crossed into its destination zone when the
    // process stops. The persisted anchor remains authoritative until arrival
    // is verified there, so zone equality must not discard the route.
    (void)currentZone; // Zone entry does not complete the persisted anchor route.
    return intent == 1 && targetMap == currentMap && targetZone != 0 &&
           productiveShare >= minimumProductiveShare;
}

inline uint32_t NextCandidateCursor(uint32_t cursor, uint32_t examined, uint32_t total)
{
    if (!total)
        return 0;
    return (cursor % total + examined % total) % total;
}

struct Waypoint
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct BlockedAnchor
{
    bool active = false;
    uint32_t mapId = 0;
    uint32_t zoneId = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

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
