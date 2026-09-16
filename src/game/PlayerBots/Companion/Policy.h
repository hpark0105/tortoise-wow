// PORT-013 (KAP-558): the deterministic companion selection policy.
// Value-only: every candidate policy and the selection below are pure
// functions over the versioned Observation snapshot; adding a candidate
// policy requires no session, login, ownership or database change - only a
// new allowlisted observation field, a candidate function and a Select
// branch. The selected directive is revalidated against live state
// immediately before execution in the adapter (stale generation, owner
// availability and target state are all re-checked there).
#ifndef TORTOISE_COMPANION_POLICY_H
#define TORTOISE_COMPANION_POLICY_H
#include "Observation.h"
#include "Intent.h"

namespace Companion
{

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
inline Intent LootPolicy(Observation const& o)
{
    return {o.lootTarget ? Action::Loot : Action::None, o.generation, o.lootTarget};
}
inline Intent DefendPolicy(Observation const& o)
{
    return {o.defendTarget ? Action::Defend : Action::None, o.generation, o.defendTarget};
}
inline Intent DamagePolicy(Observation const& o)
{
    return {o.damageTarget ? Action::Damage : Action::None, o.generation, o.damageTarget};
}
// The complete deterministic priority in one selection path:
// Hold > Assist > ContinueCombat > Defend > Damage > Loot > Follow. An
// assist
// suspends the follow goal (it resumes once the assisted target is gone)
// and overrides an incidental engagement; a live engagement is never
// abandoned for a new defender; an owner-enabled reactive defend
// interrupts loot and follow but never an ongoing fight; the declared
// damage companion engages only the established tank target (the tank-pull
// discipline lives in Companion/Damage.h; the slot is filled only when the
// selection would otherwise be Follow or Loot, like the defend slot); a
// dead corpse the
// companion is meant to loot is collected before the follow resumes; only
// a hold (or a missing owner while following) stops everything. Recovery
// (corpse reclaim) preempts this whole selection at the lifecycle level:
// while dead, no behavior policy runs.
inline Intent Select(Observation const& o)
{
    if (o.held || (o.following && !o.ownerAvailable))
        return {Action::Hold, o.generation, 0};
    Intent assist = AssistPolicy(o);
    if (assist.action != Action::None)
        return assist;
    Intent combat = ExistingCombatPolicy(o);
    if (combat.action != Action::None)
        return combat;
    Intent defend = DefendPolicy(o);
    if (defend.action != Action::None)
        return defend;
    Intent damage = DamagePolicy(o);
    if (damage.action != Action::None)
        return damage;
    Intent loot = LootPolicy(o);
    if (loot.action != Action::None)
        return loot;
    return FollowPolicy(o);
}

} // namespace Companion
#endif
