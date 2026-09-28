// Value-level tests for Companion::CitizenTravel::DecideArrival (TW-BOTS-002
// S1). Every check executes the real policy; explicit checks are used
// instead of assert so the suite also runs under NDEBUG.
#include "CitizenTravel.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <initializer_list>

namespace {

int g_failures = 0;

void Check(bool condition, char const* label)
{
    if (!condition)
    {
        std::printf("FAIL: %s\n", label);
        ++g_failures;
    }
}

using Companion::CitizenTravel::ArrivalDecision;
using Companion::CitizenTravel::ArrivalInput;
using Companion::CitizenTravel::DecideArrival;
using Companion::CitizenTravel::kArrivalRadiusYards;

void TestMapLostOutranksEverything()
{
    ArrivalInput in;
    in.sameMap = false;
    Check(DecideArrival(in) == ArrivalDecision::MapLost,
          "map-lost: a wrong map aborts even at the anchor");

    in.inTargetZone = true;
    in.distanceToAnchorYards = 0.0f;
    in.anchorWalkable = true;
    Check(DecideArrival(in) == ArrivalDecision::MapLost,
          "map-lost: outranks anchor arrival and zone entry");
}

void TestContinueFarFromAnchor()
{
    ArrivalInput in;
    in.distanceToAnchorYards = 61.0f;
    Check(DecideArrival(in) == ArrivalDecision::Continue,
          "continue: far from the anchor keeps walking");

    in.distanceToAnchorYards = kArrivalRadiusYards;
    Check(DecideArrival(in) == ArrivalDecision::Continue,
          "continue: exactly the arrival radius is not arrival");
}

void TestArrivedAnchor()
{
    ArrivalInput in;
    in.inTargetZone = true;
    in.distanceToAnchorYards = kArrivalRadiusYards - 0.01f;
    in.anchorWalkable = true;
    Check(DecideArrival(in) == ArrivalDecision::ArrivedAnchor,
          "arrived: just inside the radius with usable ground");

    in.inTargetZone = true;
    in.distanceToAnchorYards = 0.0f;
    Check(DecideArrival(in) == ArrivalDecision::ArrivedAnchor,
          "arrived: at the anchor on usable ground inside the zone");
}

void TestZoneEntryAloneIsNotArrival()
{
    ArrivalInput in;
    in.inTargetZone = true;
    in.distanceToAnchorYards = 200.0f;
    Check(DecideArrival(in) == ArrivalDecision::Continue,
          "zone-entry: inside the target zone but far from the anchor keeps walking");
}

void TestBlockedNearAnchor()
{
    ArrivalInput in;
    in.inTargetZone = false;
    in.distanceToAnchorYards = 3.0f;
    in.anchorWalkable = false;
    Check(DecideArrival(in) == ArrivalDecision::BlockedNearAnchor,
          "blocked: near an unusable anchor outside the zone defers");
}

void TestUnusableAnchorNeverArrives()
{
    ArrivalInput in;
    in.inTargetZone = true;
    in.distanceToAnchorYards = 3.0f;
    in.anchorWalkable = false;
    Check(DecideArrival(in) == ArrivalDecision::BlockedNearAnchor,
          "blocked: unusable anchor in expected zone is not arrival");
}

void TestNonfiniteDistance()
{
    ArrivalInput in;
    in.distanceToAnchorYards = std::nanf("");
    in.anchorWalkable = true;
    Check(DecideArrival(in) == ArrivalDecision::BlockedNearAnchor,
          "nonfinite: NaN distance cannot score arrival outside the zone");

    in.inTargetZone = true;
    Check(DecideArrival(in) == ArrivalDecision::BlockedNearAnchor,
          "nonfinite: NaN distance in expected zone is not arrival");

    in.distanceToAnchorYards = std::numeric_limits<float>::infinity();
    in.inTargetZone = false;
    Check(DecideArrival(in) == ArrivalDecision::BlockedNearAnchor,
          "nonfinite: infinite distance cannot score arrival");
}


void TestWrongZoneAndNegativeDistance()
{
    ArrivalInput in;
    in.distanceToAnchorYards = 1.0f;
    in.anchorWalkable = true;
    Check(DecideArrival(in) == ArrivalDecision::BlockedNearAnchor,
          "wrong-zone: walkable nearby anchor is not arrival");
    for (bool inZone : {false, true})
    {
        in.inTargetZone = inZone;
        in.distanceToAnchorYards = -1.0f;
        Check(DecideArrival(in) == ArrivalDecision::BlockedNearAnchor,
              "negative distance cannot prove arrival");
    }
}

} // namespace

int main()
{
    TestMapLostOutranksEverything();
    TestContinueFarFromAnchor();
    TestArrivedAnchor();
    TestZoneEntryAloneIsNotArrival();
    TestBlockedNearAnchor();
    TestUnusableAnchorNeverArrives();
    TestNonfiniteDistance();
    TestWrongZoneAndNegativeDistance();

    if (g_failures != 0)
    {
        std::printf("value tests: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("value tests: ALL OK\n");
    return 0;
}
