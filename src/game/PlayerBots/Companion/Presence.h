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
inline constexpr uint8_t kOwnerLowHealthPct = 35;
inline constexpr uint8_t kOwnerRecoveredPct = 50;
inline constexpr float kRegroupArmDistanceYd = 8.0f;

enum class Cue { None, Regroup, OwnerInjured };

// A single cosmetic cooldown covers both cues. Transitions are consumed even
// when speech is suppressed, so an old event cannot fire later at rest.
struct State
{
    uint32_t remainingMs = kInitialDelayMs;
    uint32_t sequence = 0;
    bool away = false;
    bool ownerSampled = false;
    bool ownerLow = false;

    void Tick(uint32_t diff)
    {
        remainingMs = diff >= remainingMs ? 0 : remainingMs - diff;
    }

    void ResetParty()
    {
        away = false;
        ownerSampled = false;
        ownerLow = false;
    }

    void MarkAway() { away = true; }

    Cue Arrived(bool canSpeak, uint32_t botLow)
    {
        bool const hadJourney = away;
        away = false;
        if (!hadJourney || !canSpeak || remainingMs)
            return Cue::None;
        ArmCooldown(botLow);
        return Cue::Regroup;
    }

    Cue ObserveOwner(bool available, uint8_t healthPct, bool inCombat,
                     bool canSpeak, uint32_t botLow)
    {
        if (!available)
        {
            ownerSampled = false;
            ownerLow = false;
            return Cue::None;
        }
        if (!ownerSampled)
        {
            ownerSampled = true;
            ownerLow = healthPct <= kOwnerLowHealthPct;
            return Cue::None;
        }
        if (healthPct >= kOwnerRecoveredPct)
        {
            ownerLow = false;
            return Cue::None;
        }
        if (healthPct > kOwnerLowHealthPct || ownerLow)
            return Cue::None;
        ownerLow = true;
        if (!inCombat || !canSpeak || remainingMs)
            return Cue::None;
        ArmCooldown(botLow);
        return Cue::OwnerInjured;
    }

private:
    void ArmCooldown(uint32_t botLow)
    {
        remainingMs = kBaseIntervalMs +
            (botLow % 3) * kIntervalJitterMs;
    }
};

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

inline char const* OwnerInjuredLine(Personality::Profile profile, uint32_t sequence)
{
    static char const* reckless[] = {
        "Stay behind me! We've got you.",
        "Hold on! I'll keep them off you."
    };
    static char const* cautious[] = {
        "You're hurt. Take cover for a moment.",
        "Careful! We need to keep you standing."
    };
    static char const* baseline[] = {
        "You're hurt. Stay close to us.",
        "Watch your health. We're with you."
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
