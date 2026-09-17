// PORT-023 (KAP-558): value-only observation and policy for the owner-driven
// cooperative quest slice. One declared supported quest (config
// PlayerBot.CooperativeQuestId); the companion mirrors the owner through the
// normal quest APIs - it never leads, never selects a quest, and never
// fabricates objective credit.
//
// The observation is filled each tick by the engine adapter (PlayerBotAI)
// from live re-resolution; the policy below is a pure function of that
// snapshot. The executor re-validates every field against live state
// (interact range, quest log state, party membership, combat) immediately
// before calling the authoritative quest helpers (CanTakeQuest/CanAddQuest,
// CanCompleteQuest/CompleteQuest/CanRewardQuest/RewardQuest) - the same
// calls the packet handlers make.
//
// Priority (documented in companion-architecture.md): hold, combat safety,
// death/recovery (lifecycle preemption) and owner loss all suppress the
// cooperative step exactly as they suppress every other behavior; leaving
// the party stops cooperative planning without erasing the companion's
// persisted quest state (the step simply stops firing).
#ifndef TORTOISE_COMPANION_QUEST_H
#define TORTOISE_COMPANION_QUEST_H
#include <cstdint>
#include <type_traits>

namespace Companion
{
namespace Quest
{

inline constexpr uint32_t kQuestObservationVersion = 1;

// The three quest-log states the policy can reason about. The engine
// adapter maps the raw QuestStatus enum (0 NONE, 1 COMPLETE, 2 UNAVAILABLE,
// 3 INCOMPLETE, 4 AVAILABLE, 5 FAILED) into these; every unmapped state
// (none, unavailable, failed) is policy-neutral and reads as None.
enum Status
{
    kStatusNone = 0,
    kStatusInProgress = 1,
    kStatusComplete = 2,
};

inline Status MapQuestStatus(uint32_t raw)
{
    switch (raw)
    {
        case 1: return kStatusComplete;
        case 3: return kStatusInProgress;
        default: return kStatusNone;
    }
}

struct Observation
{
    uint32_t version = kQuestObservationVersion;
    // The owner order generation (same source as the behavior policy);
    // carried for the value contract and stale rejection at execution.
    uint32_t generation = 0;
    uint8_t myStatus = kStatusNone;      // the companion's own log state
    uint8_t ownerStatus = kStatusNone;   // the owner's log state (read-only)
    // The companion's row is already rewarded: the turn-in finished and
    // must not fire again (a non-repeatable COMPLETE row persists).
    bool rewarded = false;
    // A live, interactable creature with the declared quest inside
    // INTERACTION_DISTANCE of the companion (accept anchor / turn-in
    // anchor). The executor re-resolves and re-validates before acting.
    bool giverAvailable = false;
    bool finisherAvailable = false;
    bool held = false;         // an owner hold is active
    bool inCombat = false;     // the companion is in combat
    // The owner (the party member whose session account owns the entry) is
    // a member of the companion's party. Leaving the party stops all
    // cooperative planning.
    bool ownerInParty = false;
    bool ownerAvailable = false; // the owner resolved to an available player
};

static_assert(std::is_trivially_copyable<Observation>::value &&
              std::is_standard_layout<Observation>::value,
              "observation must stay a bounded value type with no retained pointers");

enum class Action { None, Accept, TurnIn };

struct Intent
{
    Action action = Action::None;
    uint32_t generation = 0;
};

static_assert(std::is_trivially_copyable<Intent>::value &&
              std::is_standard_layout<Intent>::value,
              "intent must stay a bounded value type");

// One cooperative action per tick at most:
//   - Accept: the owner has started the declared quest (any non-none log
//     state), the companion has not, and a quest giver is interactable.
//     The companion never accepts before the owner - the owner's own log is
//     the authoritative start signal.
//   - TurnIn: the companion's objectives are complete and a finisher is
//     interactable; the normal reward path grants the declared choice index
//     0 (a socketless session cannot choose).
// Everything else (objective progression) is normal combat participation:
// the vanilla tap/group credit rules move both personal quest logs.
inline Intent Select(Observation const& o)
{
    if (!o.ownerAvailable || !o.ownerInParty || o.held || o.inCombat || o.rewarded)
        return {Action::None, o.generation};
    if (o.myStatus == kStatusNone && o.ownerStatus != kStatusNone && o.giverAvailable)
        return {Action::Accept, o.generation};
    if (o.myStatus == kStatusComplete && o.finisherAvailable)
        return {Action::TurnIn, o.generation};
    return {Action::None, o.generation};
}

} // namespace Quest
} // namespace Companion
#endif
