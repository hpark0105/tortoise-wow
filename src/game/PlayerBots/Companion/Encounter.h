// BL-002 (KAP-558): observe-only, value-only encounter recorder for one
// owned companion.
//
// Pure functions over values: no engine pointers, no sessions, no world
// access, no heap allocation. The engine adapter (PlayerBotAI) drives the
// lifecycle from the shared combat executor, the owner-order handlers and
// the PlayerAI damage observation callbacks; this module never influences
// a combat decision. The recorder keeps only the current or the last
// completed encounter in memory: one fixed-capacity event buffer
// (kMaxEvents) plus bounded counters.
//
// Attribution is target-matching: damage is counted only while it hits the
// active target; unrelated-target damage is ignored. Effective damage is
// the caller-computed actual health loss (overkill excluded); zero or
// blocked damage is not recorded.
//
// Overflow fails closed: once the event buffer is exceeded, later events
// are dropped and the summary stays available but is marked incomplete and
// is never efficacy-eligible.
#ifndef TORTOISE_COMPANION_ENCOUNTER_H
#define TORTOISE_COMPANION_ENCOUNTER_H

#include <cstddef>
#include <cstdint>

namespace Companion
{
namespace Encounter
{

enum class State { Idle, Active, Complete };

// Bounded event kinds.
enum class EventKind
{
    Begin,          // encounter started (target/source/route context)
    Decision,       // offense decision at a decision boundary (selected spell; route flag)
    CastResult,     // cast attempt outcome (accepted / rejected / no eligible ability)
    DamageDealt,    // effective damage to the active target (direct or periodic)
    DamageTaken,    // effective damage taken by the companion
    OwnerOverride,  // an accepted owner order closed the encounter
    Death,          // target or companion death inside the encounter
    End             // encounter ended (explicit reason)
};

// Explicit end/exclusion reasons.
enum class EndReason
{
    None,
    TargetDeath,      // the active (matching) target died
    CompanionDeath,   // the companion died (safety outcome)
    TargetChanged,    // a new target closed the prior encounter
    TargetInvalid,    // the shared executor dropped the engagement
    LeashExpired,     // the pursuit reach budget expired
    OwnerOverride     // accepted Hold/FollowGoal/FollowStop/replacement Assist
};

enum class CastOutcome { None, NoEligibleAbility, Accepted, Rejected };

inline const char* StateName(State s)
{
    switch (s)
    {
        case State::Idle:     return "idle";
        case State::Active:   return "active";
        case State::Complete: return "complete";
    }
    return "unknown";
}

inline const char* EventKindName(EventKind k)
{
    switch (k)
    {
        case EventKind::Begin:         return "begin";
        case EventKind::Decision:      return "decision";
        case EventKind::CastResult:    return "cast-result";
        case EventKind::DamageDealt:   return "damage-dealt";
        case EventKind::DamageTaken:   return "damage-taken";
        case EventKind::OwnerOverride: return "owner-override";
        case EventKind::Death:         return "death";
        case EventKind::End:           return "end";
    }
    return "unknown";
}

inline const char* EndReasonName(EndReason r)
{
    switch (r)
    {
        case EndReason::None:           return "none";
        case EndReason::TargetDeath:    return "target-death";
        case EndReason::CompanionDeath: return "companion-death";
        case EndReason::TargetChanged:  return "target-changed";
        case EndReason::TargetInvalid:  return "target-invalid";
        case EndReason::LeashExpired:   return "leash-expired";
        case EndReason::OwnerOverride:  return "owner-override";
    }
    return "unknown";
}

inline const char* CastOutcomeName(CastOutcome c)
{
    switch (c)
    {
        case CastOutcome::None:              return "none";
        case CastOutcome::NoEligibleAbility: return "no-eligible-ability";
        case CastOutcome::Accepted:          return "accepted";
        case CastOutcome::Rejected:          return "rejected";
    }
    return "unknown";
}

// Event flag bits.
static constexpr uint8_t EF_PERIODIC       = 1u << 0; // damage event: periodic (DoT)
static constexpr uint8_t EF_TARGET_DIED    = 1u << 1; // damage/death: the target side died
static constexpr uint8_t EF_COMPANION_DIED = 1u << 2; // damage/death: the companion side died
static constexpr uint8_t EF_ROUTE_TANK     = 1u << 3; // decision: Tank route (clear = Rotation)

// One recorded event (fixed size; the buffer holds kMaxEvents of these).
struct Event
{
    uint32_t elapsedMs = 0;   // encounter-local monotonic elapsed milliseconds
    uint8_t kind = 0;         // EventKind
    uint8_t endReason = 0;    // EndReason (End events only)
    uint8_t castOutcome = 0;  // CastOutcome (CastResult events only)
    uint8_t flags = 0;        // EF_* bits
    uint32_t spellId = 0;     // selected (Decision), cast (CastResult) or damaging spell
    uint32_t damage = 0;      // effective damage (DamageDealt/DamageTaken events)
};

class Recorder
{
public:
    static constexpr uint32_t kMaxEvents = 128;

    struct Summary
    {
        uint64_t targetGuid = 0;
        uint32_t source = 0;  // engagement source (Companion::Combat::Source)
        uint32_t route = 0;   // initial offense route (Companion::Combat::OffenseRoute)
        uint32_t durationMs = 0;      // active combat time (sum of per-call ticks)
        uint32_t effectiveDamage = 0; // matching-target effective hostile damage
        uint32_t periodicDamage = 0;  // periodic (DoT) portion of effectiveDamage
        uint32_t damageTaken = 0;     // effective damage taken by the companion
        uint32_t deaths = 0;          // target/companion deaths inside the encounter
        uint32_t ownerOverrides = 0;  // accepted owner overrides (close the encounter)
        uint32_t decisions = 0;
        uint32_t castsAccepted = 0;
        uint32_t castsRejected = 0;
        uint32_t castsNoEligible = 0;
        uint32_t eventCount = 0;
        EndReason endReason = EndReason::None;
        bool complete = false;         // terminal end without overflow
        bool overflow = false;         // event buffer exceeded (fail-closed)
        bool efficacyEligible = false; // complete && ended by target death
    };

    // --- lifecycle -------------------------------------------------------

    // Begins a new encounter (from Idle or Complete). Refused while Active:
    // the adapter must close the prior encounter explicitly; the recorder
    // never replaces an active one implicitly. Also refused for a zero
    // target. A refused Begin leaves all state untouched.
    bool Begin(uint64_t targetGuid, uint32_t source, uint32_t route)
    {
        if (state == State::Active || targetGuid == 0)
            return false;
        reset();
        this->targetGuid = targetGuid;
        this->source = source;
        this->route = route;
        state = State::Active;
        PushEvent(static_cast<uint8_t>(EventKind::Begin));
        return true;
    }

    // Closes the active encounter with an explicit reason. Idempotent: a
    // repeat End (or an End while Idle) is a no-op that leaves the summary
    // and the event buffer untouched.
    bool End(EndReason reason)
    {
        if (state != State::Active)
            return false;
        Finish(reason);
        return true;
    }

    // True only while an encounter is active against the given target.
    bool ActiveFor(uint64_t targetGuid) const
    {
        return state == State::Active && targetGuid == this->targetGuid;
    }

    // Discards all state (e.g. at login): the next Begin starts fresh.
    void Reset()
    {
        reset();
    }

    // --- observation inputs (no-ops unless active) ------------------------

    // One tick of active combat time, once per legal ExecuteCombat call.
    // The elapsed time is encounter-local and monotonic.
    void Tick(uint32_t diffMs)
    {
        if (state == State::Active)
            elapsedMs += diffMs;
    }

    // Offense decision at a decision boundary: the selected spell (0 =
    // nothing executable, the fallback is the ordinary attack) and the
    // route (Tank vs Rotation).
    void RecordDecision(uint32_t spellId, bool tankRoute)
    {
        if (state != State::Active)
            return;
        ++decisions;
        PushEvent(static_cast<uint8_t>(EventKind::Decision), spellId, 0,
                  tankRoute ? EF_ROUTE_TANK : 0);
    }

    // Cast attempt outcome at the existing cast boundary.
    void RecordCastResult(uint32_t spellId, CastOutcome outcome)
    {
        if (state != State::Active)
            return;
        switch (outcome)
        {
            case CastOutcome::Accepted:          ++castsAccepted;   break;
            case CastOutcome::Rejected:          ++castsRejected;   break;
            case CastOutcome::NoEligibleAbility: ++castsNoEligible; break;
            case CastOutcome::None:              break;
        }
        PushEvent(static_cast<uint8_t>(EventKind::CastResult), spellId, 0, 0,
                  static_cast<uint8_t>(outcome));
    }

    // Effective damage dealt (actual health loss, overkill excluded).
    // Target-matching: counted only while the victim is the active target;
    // unrelated-target damage is ignored entirely. Zero is ignored. A
    // matching target death completes the encounter.
    void RecordDamageDealt(uint64_t victimGuid, uint32_t effective, uint32_t spellId,
                           bool periodic, bool targetDied)
    {
        if (state != State::Active || victimGuid != targetGuid || effective == 0)
            return;
        uint8_t flags = 0;
        if (periodic)
            flags |= EF_PERIODIC;
        if (targetDied)
            flags |= EF_TARGET_DIED;
        PushEvent(static_cast<uint8_t>(EventKind::DamageDealt), spellId, effective, flags);
        effectiveDamage += effective;
        if (periodic)
            periodicDamage += effective;
        if (targetDied)
        {
            ++deaths;
            PushEvent(static_cast<uint8_t>(EventKind::Death), 0, 0, EF_TARGET_DIED);
            Finish(EndReason::TargetDeath);
        }
    }

    // Effective damage taken by the companion while the encounter is
    // active. Zero is ignored. The companion's death completes the
    // encounter as a safety outcome.
    void RecordDamageTaken(uint32_t effective, uint32_t spellId, bool periodic,
                           bool companionDied)
    {
        if (state != State::Active || effective == 0)
            return;
        uint8_t flags = 0;
        if (periodic)
            flags |= EF_PERIODIC;
        if (companionDied)
            flags |= EF_COMPANION_DIED;
        PushEvent(static_cast<uint8_t>(EventKind::DamageTaken), spellId, effective, flags);
        damageTaken += effective;
        if (companionDied)
        {
            ++deaths;
            PushEvent(static_cast<uint8_t>(EventKind::Death), 0, 0, EF_COMPANION_DIED);
            Finish(EndReason::CompanionDeath);
        }
    }

    // An accepted owner order (Hold/FollowGoal/FollowStop/replacement
    // Assist) closes the active encounter before the adapter mutates
    // target/order state. A no-op when no encounter is active.
    void RecordOwnerOverride()
    {
        if (state != State::Active)
            return;
        ++ownerOverrides;
        PushEvent(static_cast<uint8_t>(EventKind::OwnerOverride));
        Finish(EndReason::OwnerOverride);
    }

    // --- summary and introspection ----------------------------------------

    // The current or last-completed summary. Idempotent across repeated
    // Ends; a new Begin replaces it.
    Summary GetSummary() const
    {
        Summary s;
        s.targetGuid = targetGuid;
        s.source = source;
        s.route = route;
        s.durationMs = elapsedMs;
        s.effectiveDamage = effectiveDamage;
        s.periodicDamage = periodicDamage;
        s.damageTaken = damageTaken;
        s.deaths = deaths;
        s.ownerOverrides = ownerOverrides;
        s.decisions = decisions;
        s.castsAccepted = castsAccepted;
        s.castsRejected = castsRejected;
        s.castsNoEligible = castsNoEligible;
        s.eventCount = eventCount;
        s.endReason = endReason;
        s.overflow = overflow;
        s.complete = (state == State::Complete) && !overflow;
        s.efficacyEligible = s.complete && s.endReason == EndReason::TargetDeath;
        return s;
    }

    State GetState() const { return state; }
    uint64_t GetTargetGuid() const { return targetGuid; }
    uint32_t GetElapsedMs() const { return elapsedMs; }
    uint32_t GetEventCount() const { return eventCount; }
    bool Overflowed() const { return overflow; }
    // Bounded introspection for value tests: nullptr out of range.
    Event const* EventAt(uint32_t index) const
    {
        return index < eventCount ? &events[index] : nullptr;
    }

private:
    // Terminal transition shared by End and the in-recorder death/override
    // paths. Completeness is derived in GetSummary after the End event
    // push attempt, so a buffer that fills on that very push fails closed.
    void Finish(EndReason reason)
    {
        state = State::Complete;
        endReason = reason;
        PushEvent(static_cast<uint8_t>(EventKind::End), 0, 0, 0, 0,
                  static_cast<uint8_t>(reason));
    }

    bool PushEvent(uint8_t kind, uint32_t spellId = 0, uint32_t damage = 0,
                   uint8_t flags = 0, uint8_t castOutcome = 0, uint8_t endReason = 0)
    {
        if (eventCount >= kMaxEvents)
        {
            overflow = true;
            return false;
        }
        Event ev;
        ev.elapsedMs = elapsedMs;
        ev.kind = kind;
        ev.spellId = spellId;
        ev.damage = damage;
        ev.flags = flags;
        ev.castOutcome = castOutcome;
        ev.endReason = endReason;
        events[eventCount++] = ev;
        return true;
    }

    void reset()
    {
        state = State::Idle;
        targetGuid = 0;
        source = 0;
        route = 0;
        elapsedMs = 0;
        effectiveDamage = 0;
        periodicDamage = 0;
        damageTaken = 0;
        deaths = 0;
        ownerOverrides = 0;
        decisions = 0;
        castsAccepted = 0;
        castsRejected = 0;
        castsNoEligible = 0;
        endReason = EndReason::None;
        overflow = false;
        eventCount = 0;
    }

    State state = State::Idle;
    uint64_t targetGuid = 0;
    uint32_t source = 0;
    uint32_t route = 0;
    uint32_t elapsedMs = 0;
    uint32_t effectiveDamage = 0;
    uint32_t periodicDamage = 0;
    uint32_t damageTaken = 0;
    uint32_t deaths = 0;
    uint32_t ownerOverrides = 0;
    uint32_t decisions = 0;
    uint32_t castsAccepted = 0;
    uint32_t castsRejected = 0;
    uint32_t castsNoEligible = 0;
    EndReason endReason = EndReason::None;
    bool overflow = false;
    uint32_t eventCount = 0;
    Event events[kMaxEvents];
};

} // namespace Encounter
} // namespace Companion
#endif