// Living-world presence cues for owned companions.
//
// Value-only catalog: PlayerBotAI supplies the live eligibility gates and
// executes the selected line. The catalog deliberately stays bounded and
// deterministic so ambient chatter cannot become a gameplay directive.
#ifndef TORTOISE_COMPANION_PRESENCE_H
#define TORTOISE_COMPANION_PRESENCE_H

#include "Personality.h"
#include <cstdint>

namespace Companion
{
namespace Presence
{

inline constexpr uint32_t kInitialDelayMs = 20000;
inline constexpr uint32_t kBaseIntervalMs = 90000;
inline constexpr uint32_t kIntervalJitterMs = 15000;
inline constexpr uint32_t kLineMaxLen = 64;

// A regroup cue is emitted only after a companion has been out of range and
// reaches its party slot again. The sequence makes variation deterministic
// per companion without persisting cosmetic state.
inline char const* RegroupLine(Personality::Profile profile, uint32_t sequence)
{
    static char const* reckless[] = {
        "I'm back with you. Lead on.",
        "The road is clear. Let's keep moving."
    };
    static char const* cautious[] = {
        "We're together again. I'll watch our backs.",
        "All clear here. Ready when you are."
    };
    static char const* baseline[] = {
        "Back with you. Ready when you are.",
        "I am with the party again."
    };
    char const** lines = baseline;
    switch (profile)
    {
        case Personality::Profile::Reckless: lines = reckless; break;
        case Personality::Profile::Cautious: lines = cautious; break;
        default: break;
    }
    return lines[sequence % 2];
}

} // namespace Presence
} // namespace Companion

#endif // TORTOISE_COMPANION_PRESENCE_H
