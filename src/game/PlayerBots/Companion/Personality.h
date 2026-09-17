// PORT-019 (KAP-558): the versioned personality catalog and bounded
// preference mapping.
//
// Value-only. A planner source (fake or real model) may only propose a
// Preference step (planner protocol v1, action 7) whose 32-bit field
// packs (id, value). The profile declared for the companion maps that
// proposal to exactly one concrete, safe outcome: a follow-chase
// trigger distance inside the fixed band, or one static allowlisted
// expression line. This header never reads the world, sends a packet or
// learns: PlayerBotAI owns execution, and every unknown profile, unknown
// id, out-of-set value or rate excess falls closed to the deterministic
// baseline (the pre-PORT-019 constants and no expression at all).

#ifndef TORTOISE_COMPANION_PERSONALITY_H
#define TORTOISE_COMPANION_PERSONALITY_H

#include <cstdint>
#include <cstring>

namespace Companion
{
namespace Personality
{

inline constexpr uint32_t kPersonalitySchemaVersion = 1;

// Declared profiles. Profile::None is the deterministic baseline: it
// accepts no preference and expresses nothing.
enum class Profile : uint8_t
{
    None = 0,
    Reckless = 1, // stays close, talks forward
    Cautious = 2  // keeps distance, talks covered
};

// Allowlisted preference ids carried by a Preference step. The packed
// 32-bit field is (id << 8) | value; the value set is declared per id.
enum class PrefId : uint8_t
{
    Invalid = 0,
    FollowChase = 1, // owner-follow chase trigger distance (index 0..2)
    Expression = 2   // allowlisted Say line (slot 0..1)
};
inline constexpr uint8_t kPrefIdCount = 3;
inline constexpr uint8_t kChaseValueCount = 3;   // 0=close 1=medium 2=far
inline constexpr uint8_t kExpressionSlotCount = 2;

// Fixed safe band for the owner-follow chase trigger distance (yards).
// Every profile table stays inside it; the pre-PORT-019 constant sits
// in the band, so Profile::None never changes behavior.
inline constexpr float kChaseBandMinYd = 15.0f;
inline constexpr float kChaseBandMaxYd = 35.0f;
inline constexpr float kChaseBaselineYd = 25.0f;

// Expression rate limit: no profile may express more often than this.
inline constexpr uint32_t kExprIntervalMs = 15000;
// Catalog lines are static and bounded; the value test enforces this.
inline constexpr uint32_t kExprLineMaxLen = 48;

inline constexpr uint32_t PackPreference(PrefId id, uint8_t value)
{
    return ((uint32_t)(uint8_t)id << 8) | (uint32_t)(value & 0xFFu);
}

// Fail-closed parse: id 0, ids at/above the declared count are rejected.
inline constexpr bool ParsePreference(uint32_t packed, PrefId& id, uint8_t& value)
{
    uint32_t const i = packed >> 8;
    if (i == 0 || i >= kPrefIdCount)
        return false;
    id = (PrefId)i;
    value = (uint8_t)(packed & 0xFFu);
    return true;
}

struct ProfileDecl
{
    Profile profile;
    float chaseYd[kChaseValueCount];
    char const* exprLine[kExpressionSlotCount];
    // PORT-025: deterministic status lines (one per pressure
    // episode, one for the no-vendor failure report); catalog content,
    // not persisted state, so the schema version is unchanged.
    char const* pressureLine;
    char const* noVendorLine;
};

// Both declared profiles allow both ids and the full value sets; they
// differ in the concrete values the same index maps to (the A/B
// contract: one identical proposal, two observably different, safe
// outcomes). None maps everything to the baseline and is rejected by
// the Map* functions regardless.
inline ProfileDecl const& Decl(Profile p)
{
    static ProfileDecl const reckless = {
        Profile::Reckless, {15.0f, 20.0f, 25.0f},
        {"Let's go, stay behind me!", "I'll take the hits!"},
        "Ugh, my bags are full!", "No vendor in sight! Keeping them all for now."};
    static ProfileDecl const cautious = {
        Profile::Cautious, {25.0f, 30.0f, 35.0f},
        {"I'll keep my distance.", "Stay close, stay safe."},
        "My bags are nearly full; I need space.", "No vendor reachable; nothing has been sold."};
    static ProfileDecl const none = {
        Profile::None, {kChaseBaselineYd, kChaseBaselineYd, kChaseBaselineYd},
        {"", ""},
        "My bags are full.", "No vendor nearby; I will hold my bags."};
    switch (p)
    {
        case Profile::Reckless: return reckless;
        case Profile::Cautious: return cautious;
        default: return none;
    }
}

// Map a FollowChase proposal to a concrete in-band distance.
inline bool MapChaseYd(Profile p, uint8_t value, float& outYd)
{
    if (p == Profile::None || value >= kChaseValueCount)
        return false;
    outYd = Decl(p).chaseYd[value];
    return outYd >= kChaseBandMinYd && outYd <= kChaseBandMaxYd;
}

// Map an Expression proposal to an allowlisted line (never dynamic text).
inline bool MapExpressionLine(Profile p, uint8_t slot, char const*& outLine)
{
    if (p == Profile::None || slot >= kExpressionSlotCount)
        return false;
    char const* line = Decl(p).exprLine[slot];
    if (!line || !line[0])
        return false;
    outLine = line;
    return true;
}

// Map the deterministic bag-pressure status line. Unlike the
// planner-driven Expression slot, the report itself is the
// contract (one bounded line per pressure episode), so it maps
// for every declared profile including the baseline.
inline bool MapPressureLine(Profile p, char const*& outLine)
{
    char const* line = Decl(p).pressureLine;
    if (!line || !line[0])
        return false;
    outLine = line;
    return true;
}

// Map the deterministic no-vendor failure report line (rate
// limited by the adapter; the companion waits for owner
// guidance without deleting or selling anything).
inline bool MapNoVendorLine(Profile p, char const*& outLine)
{
    char const* line = Decl(p).noVendorLine;
    if (!line || !line[0])
        return false;
    outLine = line;
    return true;
}

inline char const* ProfileName(Profile p)
{
    switch (p)
    {
        case Profile::Reckless: return "reckless";
        case Profile::Cautious: return "cautious";
        default: return "none";
    }
}

// PORT-020 (KAP-558): persisted-row acceptance. The schema must
// be the current one and the profile a declared id; anything
// else fails closed to the baseline and must never be
// overwritten by a config seed.
inline bool AcceptPersisted(uint8_t schemaVersion, uint8_t profileId,
                            uint8_t& outProfile)
{
    if (schemaVersion != kPersonalitySchemaVersion)
        return false;
    if (profileId > (uint8_t)Profile::Cautious)
        return false;
    outProfile = profileId;
    return true;
}

// Config parsing fails closed: unknown or empty names are the baseline.
inline Profile ProfileFromName(char const* name)
{
    if (!name)
        return Profile::None;
    if (std::strcmp(name, "reckless") == 0)
        return Profile::Reckless;
    if (std::strcmp(name, "cautious") == 0)
        return Profile::Cautious;
    return Profile::None;
}

} // namespace Personality
} // namespace Companion

#endif // TORTOISE_COMPANION_PERSONALITY_H
