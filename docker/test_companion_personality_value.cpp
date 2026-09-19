// PORT-019 (KAP-558): value-level coverage for the personality catalog
// and bounded preference mapping (Companion/Personality.h). Standalone:
// no engine, no Docker. Compiled and run by
// docker/test_companion_personality_value.py.
#include "Companion/Personality.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <type_traits>

namespace CP = Companion::Personality;

static int g_failures = 0;
#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            std::printf("FAIL %d: %s\n", __LINE__, #cond); \
            ++g_failures; \
        } \
    } while (0)

static_assert(std::is_trivially_copyable<CP::ProfileDecl>::value &&
              std::is_standard_layout<CP::ProfileDecl>::value,
              "profile decl must be a bounded value type");

int main()
{
    // -- constants: the contract is bounded -----------------------------
    CHECK(CP::kPersonalitySchemaVersion == 1);
    CHECK(CP::kChaseBaselineYd >= CP::kChaseBandMinYd);
    CHECK(CP::kChaseBaselineYd <= CP::kChaseBandMaxYd);
    CHECK(CP::kChaseBandMinYd < CP::kChaseBandMaxYd);
    CHECK(CP::kExprIntervalMs >= 5000);
    CHECK(CP::kPrefIdCount == 3);
    CHECK(CP::kChaseValueCount == 3);
    CHECK(CP::kExpressionSlotCount == 2);

    // -- pack/parse round-trip and fail-closed ids ----------------------
    for (uint8_t id = 1; id < CP::kPrefIdCount; ++id)
        for (uint32_t v = 0; v < 256; v += 100)
        {
            uint32_t packed = CP::PackPreference((CP::PrefId)id, v);
            CP::PrefId outId = CP::PrefId::Invalid;
            uint8_t outVal = 0;
            CHECK(CP::ParsePreference(packed, outId, outVal));
            CHECK((uint8_t)outId == id);
            CHECK(outVal == v);
        }
    {
        CP::PrefId id = CP::PrefId::Invalid;
        uint8_t v = 0;
        CHECK(!CP::ParsePreference(0, id, v));                          // id 0
        CHECK(!CP::ParsePreference(CP::PackPreference(CP::PrefId::Invalid, 1), id, v));
        CHECK(!CP::ParsePreference(((uint32_t)CP::kPrefIdCount << 8) | 3, id, v)); // count
        CHECK(!CP::ParsePreference(((uint32_t)(CP::kPrefIdCount + 1) << 8), id, v));
    }

    // -- declared catalog: in-band, ordered, observably different -------
    for (CP::Profile p : {CP::Profile::Reckless, CP::Profile::Cautious})
    {
        CP::ProfileDecl const& d = CP::Decl(p);
        for (uint8_t i = 0; i < CP::kChaseValueCount; ++i)
        {
            CHECK(d.chaseYd[i] >= CP::kChaseBandMinYd);
            CHECK(d.chaseYd[i] <= CP::kChaseBandMaxYd);
        }
        CHECK(d.chaseYd[0] <= d.chaseYd[1]);
        CHECK(d.chaseYd[1] <= d.chaseYd[2]);
        for (uint8_t s = 0; s < CP::kExpressionSlotCount; ++s)
        {
            CHECK(d.exprLine[s] && d.exprLine[s][0]);
            CHECK(std::strlen(d.exprLine[s]) <= CP::kExprLineMaxLen);
            // Expression is static safe text: no command-looking leading
            // dot and no embedded newlines.
            CHECK(d.exprLine[s][0] != '.');
            CHECK(std::strstr(d.exprLine[s], "\n") == nullptr);
        }
        CHECK(std::strcmp(d.exprLine[0], d.exprLine[1]) != 0);
    }
    // A/B: the same index maps to different outcomes per profile.
    CHECK(CP::Decl(CP::Profile::Reckless).chaseYd[2] <
          CP::Decl(CP::Profile::Cautious).chaseYd[2]);
    CHECK(std::strcmp(CP::Decl(CP::Profile::Reckless).exprLine[0],
                      CP::Decl(CP::Profile::Cautious).exprLine[0]) != 0);
    CHECK(std::strcmp(CP::Decl(CP::Profile::Reckless).exprLine[1],
                      CP::Decl(CP::Profile::Cautious).exprLine[1]) != 0);

    // -- mapping: fail closed -------------------------------------------
    float yd = 0.0f;
    CHECK(CP::MapChaseYd(CP::Profile::Reckless, 0, yd) && yd == 15.0f);
    CHECK(CP::MapChaseYd(CP::Profile::Reckless, 1, yd) && yd == 20.0f);
    CHECK(CP::MapChaseYd(CP::Profile::Reckless, 2, yd) && yd == 25.0f);
    CHECK(CP::MapChaseYd(CP::Profile::Cautious, 0, yd) && yd == 25.0f);
    CHECK(CP::MapChaseYd(CP::Profile::Cautious, 1, yd) && yd == 30.0f);
    CHECK(CP::MapChaseYd(CP::Profile::Cautious, 2, yd) && yd == 35.0f);
    CHECK(!CP::MapChaseYd(CP::Profile::None, 0, yd));   // baseline: no effect
    CHECK(!CP::MapChaseYd(CP::Profile::Reckless, 3, yd)); // out of set

    char const* line = nullptr;
    CHECK(CP::MapExpressionLine(CP::Profile::Reckless, 0, line) &&
          std::strcmp(line, "Let's go, stay behind me!") == 0);
    CHECK(CP::MapExpressionLine(CP::Profile::Cautious, 1, line) &&
          std::strcmp(line, "Stay close, stay safe.") == 0);
    CHECK(!CP::MapExpressionLine(CP::Profile::None, 0, line));
    CHECK(!CP::MapExpressionLine(CP::Profile::Reckless, 2, line));

    // -- PORT-025 status lines: bounded, distinct, all profiles map --
    for (CP::Profile p2 : {CP::Profile::None, CP::Profile::Reckless, CP::Profile::Cautious})
    {
        CP::ProfileDecl const& d = CP::Decl(p2);
        CHECK(d.pressureLine && d.pressureLine[0]);
        CHECK(std::strlen(d.pressureLine) <= CP::kExprLineMaxLen);
        CHECK(d.pressureLine[0] != '.');
        CHECK(std::strstr(d.pressureLine, "\n") == nullptr);
        CHECK(d.noVendorLine && d.noVendorLine[0]);
        CHECK(std::strlen(d.noVendorLine) <= CP::kExprLineMaxLen);
        CHECK(d.noVendorLine[0] != '.');
        CHECK(std::strstr(d.noVendorLine, "\n") == nullptr);
    }
    CHECK(std::strcmp(CP::Decl(CP::Profile::Reckless).pressureLine,
                      CP::Decl(CP::Profile::Cautious).pressureLine) != 0);
    CHECK(std::strcmp(CP::Decl(CP::Profile::Reckless).noVendorLine,
                      CP::Decl(CP::Profile::Cautious).noVendorLine) != 0);
    {
        char const* status = nullptr;
        CHECK(CP::MapPressureLine(CP::Profile::Reckless, status) &&
              std::strcmp(status, "Ugh, my bags are full!") == 0);
        CHECK(CP::MapPressureLine(CP::Profile::None, status) &&
              std::strcmp(status, "My bags are full.") == 0);
        CHECK(CP::MapNoVendorLine(CP::Profile::Cautious, status) &&
              std::strcmp(status, "No vendor reachable; nothing has been sold.") == 0);
        CHECK(CP::MapNoVendorLine(CP::Profile::None, status) &&
              std::strcmp(status, "No vendor nearby; I will hold my bags.") == 0);
    }

    // -- names round-trip; unknowns fail to the baseline ----------------
    CHECK(CP::ProfileFromName("reckless") == CP::Profile::Reckless);
    CHECK(CP::ProfileFromName("cautious") == CP::Profile::Cautious);
    CHECK(CP::ProfileFromName("none") == CP::Profile::None);
    CHECK(CP::ProfileFromName("") == CP::Profile::None);
    CHECK(CP::ProfileFromName("RECKLESS") == CP::Profile::None); // case-sensitive
    CHECK(CP::ProfileFromName(nullptr) == CP::Profile::None);
    CHECK(std::strcmp(CP::ProfileName(CP::Profile::Reckless), "reckless") == 0);
    CHECK(std::strcmp(CP::ProfileName(CP::Profile::Cautious), "cautious") == 0);
    CHECK(std::strcmp(CP::ProfileName(CP::Profile::None), "none") == 0);

    // -- PORT-020: persisted-row acceptance -------------------------
    uint8_t outP = 255;
    CHECK(CP::AcceptPersisted(1, 0, outP) && outP == 0);  // none
    CHECK(CP::AcceptPersisted(1, 1, outP) && outP == 1);  // reckless
    CHECK(CP::AcceptPersisted(1, 2, outP) && outP == 2);  // cautious
    CHECK(!CP::AcceptPersisted(0, 1, outP));   // no schema = no row
    CHECK(!CP::AcceptPersisted(2, 1, outP));   // future schema
    CHECK(!CP::AcceptPersisted(99, 1, outP));  // unknown schema
    CHECK(!CP::AcceptPersisted(1, 3, outP));   // undeclared profile
    CHECK(!CP::AcceptPersisted(1, 255, outP));

    if (g_failures)
    {
        std::printf("value tests: %d FAILURES\n", g_failures);
        return 1;
    }
    std::printf("value tests: ALL OK\n");
    return 0;
}
