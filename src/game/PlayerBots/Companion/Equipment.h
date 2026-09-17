// PORT-024 (KAP-558): value-only deterministic equipment policy for
// owned companions. A companion's gear progresses only from items it
// legitimately receives through the accepted loot policy; the model
// never selects, scores or equips an item.
//
// The engine adapter (PlayerBotAI) fills ItemStats from the item
// prototype and calls the authoritative CanEquipItem for legality and
// slot (class, level, skill, proficiency, unique rules, live combat
// state). This header owns only the score and the strict-upgrade
// verdict: pure functions of values, no engine pointers, no I/O.
#ifndef TORTOISE_COMPANION_EQUIPMENT_H
#define TORTOISE_COMPANION_EQUIPMENT_H
#include <cstdint>

namespace Companion
{
namespace Equipment
{

inline constexpr uint32_t kEquipmentObservationVersion = 1;

// The prototype fields the equipment score depends on. Mirrors the
// inputs PlayerBotAI::AutoEquipForLevel reads (RequiredLevel,
// ItemLevel, Quality) plus the player level used as the last-resort
// effective level.
struct ItemStats
{
    uint32_t version = kEquipmentObservationVersion;
    uint32_t requiredLevel = 0; // 0 = unknown
    uint32_t itemLevel = 0;
    uint32_t quality = 0;       // 0 poor .. 5 legendary
    uint32_t playerLevel = 1;   // fallback effective level
};

// The same formula AutoEquipForLevel ranks by:
// effectiveLevel*1000 + quality*10 + itemLevel, where
// effectiveLevel = requiredLevel, else max(1, itemLevel*2/3),
// else the player level.
inline uint32_t Score(ItemStats const& s)
{
    uint32_t effectiveLevel = s.requiredLevel;
    if (!effectiveLevel)
    {
        uint32_t estimate = s.itemLevel ? (s.itemLevel * 2) / 3 : s.playerLevel;
        effectiveLevel = estimate > 1 ? estimate : 1;
    }
    return effectiveLevel * 1000 + s.quality * 10 + s.itemLevel;
}

enum class Verdict
{
    Equip, // strictly better: equip the received instance
    Keep   // sidegrade or downgrade: leave equipment unchanged
};

// Only a strict upgrade replaces equipment; an equal score is a
// sidegrade and never replaces gear.
inline Verdict Compare(uint32_t newScore, uint32_t equippedScore)
{
    return newScore > equippedScore ? Verdict::Equip : Verdict::Keep;
}

} // namespace Equipment
} // namespace Companion

#endif // TORTOISE_COMPANION_EQUIPMENT_H
