// PORT-013 (KAP-558): the immutable companion observation snapshot.
// Value-only and versioned: no world pointers, no sessions, no database.
// The engine adapter (PlayerBotAI) fills one fresh snapshot each tick from
// live re-resolution; policies and executors never see the engine and never
// retain anything across ticks. Raising kObservationVersion is required
// whenever a field is added or removed, so a stale snapshot cannot be
// silently misread.
#ifndef TORTOISE_COMPANION_OBSERVATION_H
#define TORTOISE_COMPANION_OBSERVATION_H
#include <cstdint>
#include <type_traits>

namespace Companion
{

inline constexpr uint32_t kObservationVersion = 3; // PORT-025: vendorTarget + bagPressure added

struct Observation
{
    uint32_t version = kObservationVersion;
    // Order generation: owner orders bump it. A directive stamped with an
    // older generation is stale and rejected at execution (IsCurrent).
    uint32_t generation = 0;
    uint64_t target = 0;         // live combat target (victim or held); the executor re-resolves it
    uint64_t assistTarget = 0;   // PORT-005: owner-selected hostile; the executor re-resolves it
    uint64_t lootTarget = 0;     // PORT-007: dead, in-world corpse to loot; the executor re-resolves it
    uint64_t defendTarget = 0;   // PORT-006: owner-enabled reactive defend candidate; the executor re-resolves it
    uint64_t damageTarget = 0;   // PORT-016: the declared tank's established victim; the executor re-resolves it
    uint64_t vendorTarget = 0;    // PORT-025: resolved live vendor within the declared radius; the executor re-resolves it
    bool bagPressure = false;     // PORT-025: the declared bag-pressure trigger is active
    bool following = false;      // an active follow goal exists
    bool held = false;           // an owner hold is active
    bool ownerAvailable = false; // the follow leader resolved to an available owner this tick
};

static_assert(std::is_trivially_copyable<Observation>::value &&
              std::is_standard_layout<Observation>::value,
              "observation must stay a bounded value type with no retained pointers");

} // namespace Companion
#endif
