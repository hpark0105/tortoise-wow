// Value-level coverage for bounded companion regroup presence cues.
#include "Companion/Presence.h"

#include <cstdio>
#include <cstring>

namespace CP = Companion::Personality;
namespace PR = Companion::Presence;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %d: %s\n", __LINE__, #cond); ++failures; } } while (0)

int main()
{
    CHECK(PR::kInitialDelayMs >= 10000);
    CHECK(PR::kBaseIntervalMs >= 60000);
    CHECK(PR::kIntervalJitterMs <= 30000);

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
    }
    CHECK(std::strcmp(PR::RegroupLine(CP::Profile::Reckless, 0),
                     PR::RegroupLine(CP::Profile::Cautious, 0)) != 0);

    std::printf("presence value tests: %s\n", failures ? "FAIL" : "ALL OK");
    return failures ? 1 : 0;
}
