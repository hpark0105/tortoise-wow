#ifndef TORTOISE_COMPANION_COMBAT_H
#define TORTOISE_COMPANION_COMBAT_H
#include <cfloat>
#include <cstddef>
#include <cstdint>
#include <initializer_list>

// PORT-012 (KAP-558): value-only combat decision module for one owned
// companion. Like Policy.h, everything here is a pure function over
// values: no engine pointers, no sessions, no world access. The engine
// adapter (PlayerBotAI) re-resolves every target from its GUID against the
// world each tick, fills the snapshots/profiles below, and applies the
// returned decisions; it never stores engine pointers across ticks.
//
// One deterministic target validator and combat decision core is shared by
// the Assist, ContinueCombat and Defend intents: live re-resolution,
// legality, range/LOS, pursuit, cast-result and fallback rules live here so
// the three sources cannot drift. Source-specific cancellation messages,
// owner-order state and all engine I/O stay in the adapter.
//
// SPELL_ATTR_ON_NEXT_SWING_1/2 audit (accepted classification): in the
// pinned core a swing spell, once triggered, stays in the
// CURRENT_MELEE_SPELL slot; AttackerStateUpdate then routes every later
// auto-attack through the swing-spell path and returns before white damage
// while UpdateMeleeAttackingState keeps re-arming the swing timer - the
// bot lands one swing spell and then deals no damage for the rest of the
// engagement (evidence: assist lab runs 2026-09-15; the blanket exclusion
// landed in 796bc67). A bounded one-shot queue/clear lifecycle is NOT
// implemented here: these learned abilities are explicitly classified
// UNSUPPORTED (Block::OnNextSwing), the Phase 1.1 exclusion is preserved,
// and no role policy or planner may ever see them as executable. Supporting
// them is future work that must behaviorally prove the queued slot clears
// and later white swings continue.
namespace Companion
{
namespace Combat
{

// ---------------------------------------------------------------------------
// Engagement source and per-tick request
// ---------------------------------------------------------------------------
enum class Source { Assist, ContinueCombat, Defend, Damage };
enum class OffenseRoute { Rotation, Tank };
inline OffenseRoute RouteOffense(Source source, bool declaredTank)
{ return source == Source::Assist && declaredTank ? OffenseRoute::Tank : OffenseRoute::Rotation; }
inline const char* OffenseRouteName(OffenseRoute r)
{ return r == OffenseRoute::Tank ? "tank" : "rotation"; }

// Values only: the target GUID, the source intent, the order generation and
// the engagement distance limit. The executor re-resolves the target from
// the world each tick and never stores engine pointers.
struct Request
{
    uint64_t target = 0;
    Source source = Source::Assist;
    uint32_t generation = 0;
    float maxDistance = 0.0f;
};

// ---------------------------------------------------------------------------
// Target validator
// ---------------------------------------------------------------------------
enum class Reject
{
    None,
    Missing,          // not resolved in the world this tick
    Dead,
    NotInWorld,
    Friendly,
    CannotAttack,
    NotDefendTarget,  // defend: no live attack evidence on a targetable unit
    NotCurrentTarget, // continue-combat: neither the current victim nor the held target
    NotEstablishedTarget, // damage: not the declared tank's established target
    TargetUnderCC,    // damage: the target holds a controlling aura
    NoLos,
    OutOfRange,
};

inline const char* RejectName(Reject r)
{
    switch (r)
    {
        case Reject::None:             return "none";
        case Reject::Missing:          return "missing";
        case Reject::Dead:             return "dead";
        case Reject::NotInWorld:       return "not-in-world";
        case Reject::Friendly:         return "friendly";
        case Reject::CannotAttack:     return "cannot-attack";
        case Reject::NotDefendTarget:  return "not-defend-target";
        case Reject::NotCurrentTarget: return "not-current-target";
        case Reject::NotEstablishedTarget: return "not-established-target";
        case Reject::TargetUnderCC:    return "target-under-cc";
        case Reject::NoLos:            return "no-los";
        case Reject::OutOfRange:       return "out-of-range";
    }
    return "unknown";
}

// One per-tick snapshot of a candidate target, filled by the adapter.
// Fields beyond exists/alive/inWorld are only consulted when those pass,
// mirroring the short-circuit order of the baseline executor.
struct TargetSnapshot
{
    bool exists = false;
    bool alive = false;
    bool inWorld = false;
    bool canAttack = false;
    bool friendly = false;
    bool inLos = false;
    float distance = 0.0f;
    // ContinueCombat evidence (the fight must still be against this target).
    bool isVictim = false;
    bool isHeld = false;
    // Defend evidence (mirrors DefendTargetLegal).
    bool targetable = false;
    bool victimIsProtected = false; // its victim is me or the owner
    bool attackingMe = false;       // me is in its attacker set
    bool attackingOwner = false;    // the owner is in its attacker set
    // Damage evidence (PORT-016): the target is still the declared
    // tank's established target (the tank is its victim, or the tank's
    // threat reaches the switch margin against the victim's), and it
    // does not hold a controlling aura (crowd-control preservation).
    bool establishedTarget = false;
    bool targetUnderCC = false;
};

// A unit is a legal defend target when it is not friendly and either
// attackable outright or targetable with live attack evidence: its victim
// is a protected unit, or a protected unit is still in its attacker set
// (the victim state can flap between the owner's melee swings).
inline bool DefendTargetLegal(TargetSnapshot const& s)
{
    if (s.friendly)
        return false;
    if (s.canAttack)
        return true;
    if (!s.targetable)
        return false;
    return s.victimIsProtected || s.attackingMe || s.attackingOwner;
}

struct Verdict { bool legal; Reject reject; };

// The deterministic legality rules, shared by all three sources. The check
// order mirrors the reviewed baseline executor so the source-specific
// diagnostics keep their meaning. ContinueCombat has no LOS rule (the fight
// is already live) and requires the target to be the current victim or the
// held target; assist and defend require line of sight within the limit.
inline Verdict Verify(Request const& req, TargetSnapshot const& s)
{
    if (!s.exists)
        return {false, Reject::Missing};
    if (!s.alive)
        return {false, Reject::Dead};
    if (!s.inWorld)
        return {false, Reject::NotInWorld};
    switch (req.source)
    {
        case Source::Assist:
        case Source::ContinueCombat:
            if (!s.canAttack)
                return {false, Reject::CannotAttack};
            if (s.friendly)
                return {false, Reject::Friendly};
            break;
        case Source::Defend:
            if (!DefendTargetLegal(s))
                return {false, Reject::NotDefendTarget};
            break;
        case Source::Damage:
            if (!s.canAttack)
                return {false, Reject::CannotAttack};
            if (s.friendly)
                return {false, Reject::Friendly};
            if (s.targetUnderCC)
                return {false, Reject::TargetUnderCC};
            if (!s.establishedTarget)
                return {false, Reject::NotEstablishedTarget};
            break;
    }
    if (req.source == Source::ContinueCombat)
    {
        if (s.distance > req.maxDistance)
            return {false, Reject::OutOfRange};
        if (!s.isVictim && !s.isHeld)
            return {false, Reject::NotCurrentTarget};
        return {true, Reject::None};
    }
    if (!s.inLos)
        return {false, Reject::NoLos};
    if (s.distance > req.maxDistance)
        return {false, Reject::OutOfRange};
    return {true, Reject::None};
}

// ---------------------------------------------------------------------------
// Pursuit reach budget (PORT-008)
// ---------------------------------------------------------------------------
struct Leash
{
    static constexpr uint32_t kArmedMs = 30000;

    enum class Step { Continue, Armed, Expired, Disarmed };

    uint32_t remainingMs = 0;

    bool Armed() const { return remainingMs != 0; }
    void Disarm() { remainingMs = 0; }

    // One tick. inMeleeRange disarms (a fresh budget for the next pursuit);
    // the first out-of-range tick arms; on expiry the engagement is
    // abandoned exactly like an invalid target and the prior order resumes.
    Step Tick(bool inMeleeRange, uint32_t diff)
    {
        if (inMeleeRange)
        {
            Disarm();
            return Step::Disarmed;
        }
        if (remainingMs == 0)
        {
            remainingMs = kArmedMs;
            return Step::Armed;
        }
        remainingMs = (remainingMs > diff) ? remainingMs - diff : 0;
        return remainingMs == 0 ? Step::Expired : Step::Continue;
    }
};

// ---------------------------------------------------------------------------
// Explicit combat/loot target slots
// ---------------------------------------------------------------------------
// The live combat target and the defeated corpse pending loot are separate
// slots with named transitions. Releasing combat state never touches the
// corpse slot, and releasing loot state can never resurrect a live combat
// target. Owner orders (hold, follow, assist, defend) live in the adapter
// and are never part of this state.
struct TargetSlots
{
    uint64_t live = 0;   // live combat target (0 = none)
    uint64_t corpse = 0; // dead, in-world corpse pending loot (0 = none)

    enum class Transition
    {
        None,
        LiveAcquired,   // a new live target was remembered
        LiveToCorpse,   // the live target died: it became the pending loot corpse
        CorpseToLive,   // the pending corpse re-embodied: it is the live target again
        LiveCleared,
        CorpseCleared,
        AllCleared,     // an owner order (hold/stop) or a full cleanup
    };

    bool Empty() const { return live == 0 && corpse == 0; }

    // Remembers the live target; idempotent while it is the same guid.
    Transition AcquireLive(uint64_t guid)
    {
        if (guid == 0 || live == guid)
            return Transition::None;
        live = guid;
        return Transition::LiveAcquired;
    }

    // Per-tick re-resolution of the live slot: a vanished target clears it;
    // a defeated one promotes explicitly to the loot-corpse slot.
    Transition OnLiveResolved(bool exists, bool alive)
    {
        if (live == 0)
            return Transition::None;
        if (!exists)
        {
            live = 0;
            return Transition::LiveCleared;
        }
        if (!alive)
        {
            corpse = live;
            live = 0;
            return Transition::LiveToCorpse;
        }
        return Transition::None;
    }

    // Per-tick re-resolution of the corpse slot: a vanished corpse clears
    // it; a re-embodied one hands the target back to the combat side.
    Transition OnCorpseResolved(bool exists, bool alive)
    {
        if (corpse == 0)
            return Transition::None;
        if (!exists)
        {
            corpse = 0;
            return Transition::CorpseCleared;
        }
        if (alive)
        {
            live = corpse;
            corpse = 0;
            return Transition::CorpseToLive;
        }
        return Transition::None;
    }

    // Combat-state release only: never touches the corpse slot.
    Transition ReleaseLive()
    {
        if (live == 0)
            return Transition::None;
        live = 0;
        return Transition::LiveCleared;
    }

    // Loot-state release only: never resurrects a live combat target.
    Transition ReleaseCorpse()
    {
        if (corpse == 0)
            return Transition::None;
        corpse = 0;
        return Transition::CorpseCleared;
    }

    Transition ClearAll()
    {
        if (Empty())
            return Transition::None;
        live = 0;
        corpse = 0;
        return Transition::AllCleared;
    }
};

// ---------------------------------------------------------------------------
// Capability model: known vs currently usable
// ---------------------------------------------------------------------------
enum class Block
{
    None,
    Passive,             // auras/buffs are not offensive casts
    ObsoleteRank,        // an older rank of a known chain
    OnNextSwing,         // explicitly unsupported (see the audit above)
    Unaffordable,        // power cost exceeds the current power
    Cooldown,
    StanceForm,          // stance/form restriction does not hold
    Reagent,             // reagents unavailable
    TargetInappropriate, // e.g. the required-missing-aura evidence already holds
    OutOfRange,          // outside the min/max/melee reach band
};

inline const char* BlockName(Block b)
{
    switch (b)
    {
        case Block::None:                return "none";
        case Block::Passive:             return "passive";
        case Block::ObsoleteRank:        return "obsolete-rank";
        case Block::OnNextSwing:         return "on-next-swing";
        case Block::Unaffordable:        return "unaffordable";
        case Block::Cooldown:            return "cooldown";
        case Block::StanceForm:          return "stance-form";
        case Block::Reagent:             return "reagent";
        case Block::TargetInappropriate: return "target-inappropriate";
        case Block::OutOfRange:          return "out-of-range";
    }
    return "unknown";
}

// One spell of the companion's rotation, filled with current engine facts
// by the adapter. Profiles with known=false are not scanned at all: the
// known/usable split means only known spells can ever become usable.
struct AbilityProfile
{
    uint32_t id = 0;
    bool known = false;
    uint8_t powerType = 0; // Powers index (adapter fills)
    uint32_t powerCost = 0;
    bool passive = false;
    bool obsoleteRank = false;
    bool onNextSwing = false;
    bool onCooldown = false;
    bool stanceOk = true;
    bool reagentOk = true;
    bool targetOk = true;
    float minRange = 0.0f;
    float maxRange = 0.0f;
    bool meleeOnly = false;
};

struct AbilityQuery
{
    static constexpr int kPowerSlots = 5; // MAX_POWERS (adapter fills all)
    uint32_t power[kPowerSlots] = {0, 0, 0, 0, 0};
    float distance = 0.0f;   // combat distance (includes the target's reach)
    bool meleeReach = false;
};

struct AbilityDecision
{
    uint32_t selected = 0;    // 0 = nothing executable; the fallback is the ordinary attack
    bool anyKnown = false;    // at least one profile was known
    Block firstBlock = Block::None; // first blocker among known profiles (diagnostic)
};

struct RotationPlan
{
    static constexpr int kMaxProfiles = 8;
    int8_t order[kMaxProfiles] = {};
    uint8_t count = 0;
    RotationPlan() = default;
    RotationPlan(std::initializer_list<int> values)
    {
        count = static_cast<uint8_t>(values.size() <= kMaxProfiles ? values.size() : kMaxProfiles + 1);
        size_t i = 0;
        for (int v : values) { if (i < kMaxProfiles) order[i++] = static_cast<int8_t>(v); }
    }
    static RotationPlan Baseline(int n)
    {
        RotationPlan p; if (n < 0 || n > kMaxProfiles) return p;
        p.count = static_cast<uint8_t>(n); for (int i=0;i<n;++i) p.order[i]=static_cast<int8_t>(i); return p;
    }
    bool CompatibleWith(int n) const
    {
        if (n < 0 || n > kMaxProfiles || count != n) return false;
        bool seen[kMaxProfiles] = {};
        for (int i=0;i<n;++i) { int v=order[i]; if(v<0||v>=n||seen[v]) return false; seen[v]=true; }
        return true;
    }
};

// Deterministic selection: the first profile in table order that is known
// and currently usable wins. A known-but-blocked profile never becomes the
// selected cast; its first blocker is recorded (diagnostic) so the adapter
// can keep "no eligible ability" distinct from "rejected attempt".
template <typename ProfileRange>
inline AbilityDecision SelectExecutable(ProfileRange const& profiles, AbilityQuery const& q)
{
    AbilityDecision d;
    for (AbilityProfile const& p : profiles)
    {
        if (!p.known)
            continue;
        d.anyKnown = true;
        Block b = Block::None;
        if (p.obsoleteRank)
            b = Block::ObsoleteRank;
        else if (p.passive)
            b = Block::Passive;
        else if (p.onNextSwing)
            b = Block::OnNextSwing;
        else if (p.powerType < AbilityQuery::kPowerSlots && p.powerCost > q.power[p.powerType])
            b = Block::Unaffordable;
        else if (p.onCooldown)
            b = Block::Cooldown;
        else if (!p.stanceOk)
            b = Block::StanceForm;
        else if (!p.reagentOk)
            b = Block::Reagent;
        else if (!p.targetOk)
            b = Block::TargetInappropriate;
        else if ((p.minRange > 0.0f && q.distance < p.minRange) ||
                 (p.maxRange > 0.0f && q.distance > p.maxRange) ||
                 (p.meleeOnly && !q.meleeReach))
            b = Block::OutOfRange;
        if (b == Block::None)
        {
            d.selected = p.id;
            return d;
        }
        if (d.firstBlock == Block::None)
            d.firstBlock = b;
    }
    return d;
}

template <typename ProfileRange>
inline AbilityDecision SelectExecutable(ProfileRange const& profiles, AbilityQuery const& q, RotationPlan const& plan)
{
    int n = static_cast<int>(profiles.size());
    if (!plan.CompatibleWith(n)) return SelectExecutable(profiles, q);
    AbilityDecision d;
    for (int index : plan.order)
    {
        if (index < 0 || index >= n) continue;
        AbilityProfile const& p = profiles[static_cast<size_t>(index)];
        if (!p.known) continue; d.anyKnown = true; Block b = Block::None;
        if (p.obsoleteRank) b=Block::ObsoleteRank; else if(p.passive) b=Block::Passive; else if(p.onNextSwing) b=Block::OnNextSwing;
        else if(p.powerType<AbilityQuery::kPowerSlots && p.powerCost>q.power[p.powerType]) b=Block::Unaffordable; else if(p.onCooldown) b=Block::Cooldown;
        else if(!p.stanceOk) b=Block::StanceForm; else if(!p.reagentOk) b=Block::Reagent; else if(!p.targetOk) b=Block::TargetInappropriate;
        else if((p.minRange>0.0f&&q.distance<p.minRange)||(p.maxRange>0.0f&&q.distance>p.maxRange)||(p.meleeOnly&&!q.meleeReach)) b=Block::OutOfRange;
        if (b==Block::None) { d.selected=p.id; return d; } if(d.firstBlock==Block::None) d.firstBlock=b;
    }
    return d;
}

// ---------------------------------------------------------------------------
// BL-005: value-only warrior playbook adapter
// ---------------------------------------------------------------------------
// Converts a validated candidate ability-ID ordering (from the BL-004
// tactical protocol) into a fixed RotationPlan index permutation for
// SelectExecutable. Pure value-only: no heap allocation, no engine access,
// no LLM, no persistence.
//
// The adapter validates the candidate against the live capability catalog:
//   - unknown: any ability ID not present in the catalog
//   - duplicate: the same ability ID appears more than once
//   - missing: the candidate count does not match the catalog count
//   - oversized: the candidate count exceeds kMaxProfiles
//   - capability-version-mismatch: the expected version does not match
//     the live catalog version
//
// On any rejection, returns the baseline identity plan (no behavior change).
// Every existing eligibility check, on-next-swing exclusion, tank route,
// and class behavior is preserved: the adapter only reorders the scan;
// it does not alter profile facts or block classification.
struct WarriorPlaybookAdapter
{
    static RotationPlan Adapt(uint32_t const* candidateIds, uint32_t candidateCount,
                              uint32_t expectedVersion,
                              uint32_t catalogVersion,
                              uint32_t const* catalogIds,
                              uint32_t catalogCount)
    {
        RotationPlan baseline = RotationPlan::Baseline(static_cast<int>(catalogCount));

        if (!candidateIds || !catalogIds)
            return baseline;
        if (catalogCount == 0 || candidateCount > RotationPlan::kMaxProfiles)
            return baseline;
        if (expectedVersion != catalogVersion)
            return baseline;
        if (candidateCount != catalogCount)
            return baseline;

        // Map each candidate ID to its catalog index; reject on unknown or duplicate.
        int8_t indices[RotationPlan::kMaxProfiles] = {};
        for (uint32_t i = 0; i < candidateCount; ++i)
        {
            uint32_t id = candidateIds[i];
            for (uint32_t j = 0; j < i; ++j)
                if (candidateIds[j] == id)
                    return baseline;
            int32_t idx = -1;
            for (uint32_t k = 0; k < catalogCount; ++k)
            {
                if (catalogIds[k] == id)
                {
                    idx = static_cast<int32_t>(k);
                    break;
                }
            }
            if (idx < 0)
                return baseline;
            indices[i] = static_cast<int8_t>(idx);
        }

        RotationPlan plan;
        for (uint32_t i = 0; i < candidateCount; ++i)
        {
            plan.order[i] = indices[i];
            plan.count = static_cast<uint8_t>(i + 1);
        }
        return plan;
    }
};

// ---------------------------------------------------------------------------
// Cast-result vocabulary (PORT-011 outcomes, value form)
// ---------------------------------------------------------------------------
enum class CastOutcome { None, NoEligibleAbility, Accepted, Rejected };

enum class CastReject
{
    None,
    InsufficientPower,
    RangeLos,
    Cooldown,
    StanceForm,
    Facing,
    InvalidTarget,
    Other,
};

inline const char* CastRejectName(CastReject c)
{
    switch (c)
    {
        case CastReject::None:              return "none";
        case CastReject::InsufficientPower: return "insufficient-power";
        case CastReject::RangeLos:          return "range-los";
        case CastReject::Cooldown:          return "cooldown";
        case CastReject::StanceForm:        return "stance-form";
        case CastReject::Facing:            return "facing";
        case CastReject::InvalidTarget:     return "invalid-target";
        case CastReject::Other:             return "other";
    }
    return "other";
}

// SPELL_CAST_OK in the pinned core (SpellDefines.h); the value test pins
// the mapping. The raw SpellCastResult -> CastReject mapper stays in the
// adapter, the only place that sees the engine enum.
static constexpr uint32_t kCastOk = 0xFF;

struct CastReport
{
    CastOutcome outcome = CastOutcome::None;
    uint32_t spellId = 0;
    uint32_t rawResult = 0;
    CastReject reject = CastReject::None;
    // A failed cast or no eligible ability falls back to the ordinary
    // attack in the same evaluation and does not consume the AI action
    // window: only an accepted cast arms the ability timer.
    bool meleeFallback = false;
};

inline CastReport ReportNoEligible()
{
    CastReport r;
    r.outcome = CastOutcome::NoEligibleAbility;
    r.meleeFallback = true;
    return r;
}

inline CastReport ReportCast(uint32_t spellId, uint32_t rawResult, CastReject reject)
{
    CastReport r;
    r.spellId = spellId;
    r.rawResult = rawResult;
    if (rawResult == kCastOk)
    {
        r.outcome = CastOutcome::Accepted;
    }
    else
    {
        r.outcome = CastOutcome::Rejected;
        r.reject = reject;
        r.meleeFallback = true;
    }
    return r;
}

} // namespace Combat
} // namespace Companion
#endif
