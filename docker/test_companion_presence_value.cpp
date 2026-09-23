// Value-level coverage for bounded companion regroup presence cues.
#include "Companion/Presence.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace CP = Companion::Personality;
namespace PR = Companion::Presence;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %d: %s\n", __LINE__, #cond); ++failures; } } while (0)

int main()
{
    CHECK(PR::kInitialDelayMs >= 10000);
    CHECK(PR::kBaseIntervalMs >= 60000);
    CHECK(PR::kIntervalJitterMs <= 30000);
    CHECK(PR::kOwnerLowHealthPct < PR::kOwnerRecoveredPct);
    CHECK(PR::kRegroupArmDistanceYd > 2.0f);

    for (CP::Profile profile : {CP::Profile::None, CP::Profile::Reckless, CP::Profile::Cautious})
    {
        char const* a = PR::RegroupLine(profile, 0);
        char const* b = PR::RegroupLine(profile, 1);
        CHECK(a && b && a[0] && b[0]);
        CHECK(std::strlen(a) <= PR::kLineMaxLen);
        CHECK(std::strlen(b) <= PR::kLineMaxLen);
        CHECK(a[0] != '.');
        CHECK(std::strchr(a, '\n') == nullptr);
        CHECK(std::strcmp(a, b) != 0);
        CHECK(std::strcmp(a, PR::RegroupLine(profile, 2)) == 0);
        char const* injured = PR::OwnerInjuredLine(profile, 0);
        CHECK(injured && injured[0]);
        CHECK(std::strlen(injured) <= PR::kLineMaxLen);
        CHECK(injured[0] != '.');
        CHECK(std::strchr(injured, '\n') == nullptr);
    }
    CHECK(std::strcmp(PR::RegroupLine(CP::Profile::Reckless, 0),
                     PR::RegroupLine(CP::Profile::Cautious, 0)) != 0);

    // Arrival consumes a real journey once. Standing still never produces a
    // second regroup line when the cooldown expires.
    PR::State travel;
    CHECK(travel.Arrived(true, 2) == PR::Cue::None);
    travel.MarkAway();
    CHECK(travel.Arrived(true, 2) == PR::Cue::None); // initial delay
    travel.Tick(PR::kInitialDelayMs);
    travel.MarkAway();
    CHECK(travel.Arrived(true, 2) == PR::Cue::Regroup);
    CHECK(travel.Arrived(true, 2) == PR::Cue::None);
    travel.Tick(PR::kBaseIntervalMs + 2 * PR::kIntervalJitterMs);
    CHECK(travel.Arrived(true, 2) == PR::Cue::None);
    travel.MarkAway();
    CHECK(travel.Arrived(false, 2) == PR::Cue::None);
    CHECK(travel.Arrived(true, 2) == PR::Cue::None);

    // Owner distress is a crossing into low health during combat, after a
    // prior healthy sample. Recovery re-arms it, but cooldown suppression
    // never queues an old line for later.
    PR::State distress;
    distress.Tick(PR::kInitialDelayMs);
    CHECK(distress.ObserveOwner(true, 100, false, true, 5) == PR::Cue::None);
    CHECK(distress.ObserveOwner(true, 35, true, true, 5) == PR::Cue::OwnerInjured);
    CHECK(distress.ObserveOwner(true, 20, true, true, 5) == PR::Cue::None);
    CHECK(distress.ObserveOwner(true, 60, false, true, 5) == PR::Cue::None);
    CHECK(distress.ObserveOwner(true, 30, true, true, 5) == PR::Cue::None);
    distress.Tick(PR::kBaseIntervalMs + 2 * PR::kIntervalJitterMs);
    CHECK(distress.ObserveOwner(true, 30, true, true, 5) == PR::Cue::None);
    CHECK(distress.ObserveOwner(true, 60, false, true, 5) == PR::Cue::None);
    CHECK(distress.ObserveOwner(true, 30, true, true, 5) == PR::Cue::OwnerInjured);

    PR::State loginLow;
    loginLow.Tick(PR::kInitialDelayMs);
    CHECK(loginLow.ObserveOwner(true, 25, true, true, 2) == PR::Cue::None);
    CHECK(loginLow.ObserveOwner(true, 25, true, true, 2) == PR::Cue::None);
    loginLow.ResetParty();
    CHECK(loginLow.ObserveOwner(true, 25, true, true, 2) == PR::Cue::None);
    CHECK(loginLow.ObserveOwner(false, 0, false, true, 2) == PR::Cue::None);
    CHECK(loginLow.ObserveOwner(true, 25, true, true, 2) == PR::Cue::None);

    std::printf("presence value tests: %s\n", failures ? "FAIL" : "ALL OK");
    return failures ? 1 : 0;
}
