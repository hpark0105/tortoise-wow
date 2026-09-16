// PORT-013 (KAP-558): the typed companion directive. Value-only: an action,
// the order generation it was selected under, and one allowlisted target
// GUID. A directive is produced either as a candidate (one policy) or
// selected (Select); the executor re-validates every field against live
// state immediately before acting and never stores engine pointers.
#ifndef TORTOISE_COMPANION_INTENT_H
#define TORTOISE_COMPANION_INTENT_H
#include <cstdint>
#include <type_traits>

namespace Companion
{

inline constexpr uint32_t kDirectiveVersion = 1;

// The complete behavior vocabulary. Hold, Assist, ContinueCombat, Defend,
// Loot and Follow share the one documented priority in Policy::Select
// (Hold > Assist > ContinueCombat > Defend > Damage > Loot > Follow);
// recovery (corpse reclaim) preempts behavior selection at the
// lifecycle level and is not a behavior action.
enum class Action { None, Hold, Follow, ContinueCombat, Assist, Loot, Defend, Damage };

struct Intent
{
    Action action;
    uint32_t generation;
    uint64_t target;
    uint32_t version = kDirectiveVersion;
};

static_assert(std::is_trivially_copyable<Intent>::value &&
              std::is_standard_layout<Intent>::value,
              "directive must stay a bounded value type with no retained pointers");

// Stale-generation rejection: a directive selected under an older order
// generation must not execute after a newer owner order landed.
inline bool IsCurrent(Intent const& i, uint32_t generation)
{
    return i.generation == generation;
}

} // namespace Companion
#endif
