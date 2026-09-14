#ifndef TORTOISE_COMPANION_POLICY_H
#define TORTOISE_COMPANION_POLICY_H
#include <cstdint>
// Value-only observations: policies never retain world pointers or access sessions/DB.
namespace Companion
{
enum class Action { None, Hold, Follow, ContinueCombat, Assist };
struct Observation
{
    uint32_t generation = 0;
    uint64_t target = 0;
    uint64_t assistTarget = 0; // PORT-005: owner-selected hostile; the executor re-resolves it
    bool following = false;
    bool held = false;
    bool ownerAvailable = false;
};
struct Intent { Action action; uint32_t generation; uint64_t target; };
inline Intent FollowPolicy(Observation const& o)
{
    return {o.following ? Action::Follow : Action::None, o.generation, 0};
}
inline Intent ExistingCombatPolicy(Observation const& o)
{
    return {o.target ? Action::ContinueCombat : Action::None, o.generation, o.target};
}
inline Intent AssistPolicy(Observation const& o)
{
    return {o.assistTarget ? Action::Assist : Action::None, o.generation, o.assistTarget};
}
// Priority: Hold > Assist > ContinueCombat > Follow. An assist suspends the
// follow goal (it resumes once the assisted target is gone) and overrides an
// incidental engagement; only a hold (or a missing owner while following)
// stops everything.
inline Intent Select(Observation const& o)
{
    if (o.held || (o.following && !o.ownerAvailable))
        return {Action::Hold, o.generation, 0};
    Intent assist = AssistPolicy(o);
    if (assist.action != Action::None)
        return assist;
    Intent combat = ExistingCombatPolicy(o);
    return combat.action != Action::None ? combat : FollowPolicy(o);
}
inline bool IsCurrent(Intent const& i, uint32_t generation) { return i.generation == generation; }
}
#endif
