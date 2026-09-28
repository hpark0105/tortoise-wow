// Value-only retreat progress policy for zone-citizen combat recovery.
//
// The policy scores an already-issued retreat (see PlayerBotAI::
// TryCitizenDisengage) from plain position/time samples supplied by the
// caller. It owns no engine state, takes no engine pointers and depends
// only on standard C++17 headers. All timing is accumulated from the
// supplied diff in milliseconds; wall-clock time is never read here.
#ifndef TORTOISE_WOW_COMPANION_CITIZEN_RECOVERY_H
#define TORTOISE_WOW_COMPANION_CITIZEN_RECOVERY_H

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace Companion {
namespace CitizenRecovery {

enum class Outcome { Inactive, Walking, Escaped, Stalled, TimedOut };

// Citizens normally retreat at 15%. If their current opponent is nearly
// defeated, let them finish the fight unless their own health reaches the
// critical 5% floor. A separate danger-zone signal still retreats at once.
constexpr std::uint32_t kRetreatHealthPercent = 15u;
constexpr std::uint32_t kDyingTargetHealthPercent = 15u;
constexpr std::uint32_t kCriticalHealthPercent = 5u;
// Voluntary combat joining (solo hunt pull or hunting-group assistance)
// requires at least this health percentage; below it the citizen rests or
// recovers instead. Necessary self-defense is never gated by it.
constexpr std::uint32_t kHuntReadyHealthPercent = 80u;

inline bool ShouldRetreat(std::uint32_t health, std::uint32_t maxHealth,
                          bool dangerZone, std::uint32_t targetHealth = 0u,
                          std::uint32_t targetMaxHealth = 0u)
{
    if (!maxHealth)
        return false;
    if (dangerZone)
        return true;

    std::uint64_t const healthPercent = static_cast<std::uint64_t>(health) * 100u;
    bool const atRetreatThreshold = healthPercent <=
        static_cast<std::uint64_t>(maxHealth) * kRetreatHealthPercent;
    bool const opponentNearlyDefeated = targetMaxHealth &&
        static_cast<std::uint64_t>(targetHealth) * 100u <=
        static_cast<std::uint64_t>(targetMaxHealth) * kDyingTargetHealthPercent;
    bool const criticalHealth = healthPercent <=
        static_cast<std::uint64_t>(maxHealth) * kCriticalHealthPercent;
    return atRetreatThreshold && (!opponentNearlyDefeated || criticalHealth);
}

// Escape: cleared of combat at least this far from the retreat start.
constexpr float kEscapeDistanceYards = 5.0f;
constexpr float kEscapeDistanceSquaredYards = kEscapeDistanceYards * kEscapeDistanceYards;
// Stall: less than this far from the last checkpoint across an interval.
constexpr float kStallDistanceYards = 2.0f;
constexpr float kStallDistanceSquaredYards = kStallDistanceYards * kStallDistanceYards;
// Stall is re-armed every accumulated interval.
constexpr std::uint32_t kStallCheckIntervalMs = 5000u;
// Absolute retreat lifetime; bounds even a walker that keeps making progress.
constexpr std::uint32_t kMaxLifetimeMs = 15000u;
// Accumulated-time saturation bound so huge diffs cannot wrap backwards.
constexpr std::uint32_t kSaturatedElapsedMs = std::numeric_limits<std::uint32_t>::max();

class Progress
{
public:
    Progress() = default;

    // Starts (or restarts) a tracked retreat from (x, y). Records the
    // initial position, seeds the checkpoint there and zeroes the
    // accumulated counters.
    void Begin(float x, float y)
    {
        _active = true;
        _initialX = x;
        _initialY = y;
        _checkpointX = x;
        _checkpointY = y;
        _elapsedMs = 0u;
        _nextCheckpointMs = kStallCheckIntervalMs;
    }

    // Cancels any active retreat and clears all counters.
    void Reset()
    {
        _active = false;
        _initialX = 0.0f;
        _initialY = 0.0f;
        _checkpointX = 0.0f;
        _checkpointY = 0.0f;
        _elapsedMs = 0u;
        _nextCheckpointMs = kStallCheckIntervalMs;
    }

    bool Active() const
    {
        return _active;
    }

    // Scores one movement sample diff ms after the previous sample.
    //
    // Outcome contract:
    // - Inactive when no retreat is tracked (also after any terminal).
    // - Escaped when combat is cleared and the sample is at least
    //   kEscapeDistanceYards from the retreat start. Ends the activity.
    // - TimedOut when the accumulated lifetime reaches kMaxLifetimeMs,
    //   even if the citizen kept making progress. Ends the activity.
    // - Stalled when a checkpoint sample (every kStallCheckIntervalMs)
    //   moved less than kStallDistanceYards from the last checkpoint, or
    //   when position data is nonfinite. Ends the activity.
    // - Walking otherwise.
    //
    // Boundary priority when several terminal conditions hold at once:
    // success (Escaped), then TimedOut, then Stalled. Nonfinite position
    // data is untrusted and fails Stalled without scoring escape at all.
    Outcome Update(std::uint32_t diff, float x, float y, bool inCombat)
    {
        if (!_active)
            return Outcome::Inactive;

        // Saturated addition: a huge diff clamps instead of wrapping.
        if (diff >= kSaturatedElapsedMs - _elapsedMs)
            _elapsedMs = kSaturatedElapsedMs;
        else
            _elapsedMs += diff;

        // Squared displacement from the retreat start. Nonfinite (including
        // overflow to +inf) data cannot be scored as escape progress.
        float const dx = x - _initialX;
        float const dy = y - _initialY;
        float const fromStartSq = dx * dx + dy * dy;
        if (!std::isfinite(fromStartSq))
        {
            Reset();
            return Outcome::Stalled;
        }

        if (!inCombat && fromStartSq >= kEscapeDistanceSquaredYards)
        {
            Reset();
            return Outcome::Escaped;
        }
        if (_elapsedMs >= kMaxLifetimeMs)
        {
            Reset();
            return Outcome::TimedOut;
        }

        if (_elapsedMs >= _nextCheckpointMs)
        {
            float const cx = x - _checkpointX;
            float const cy = y - _checkpointY;
            float const fromCheckpointSq = cx * cx + cy * cy;
            if (!std::isfinite(fromCheckpointSq) ||
                fromCheckpointSq < kStallDistanceSquaredYards)
            {
                Reset();
                return Outcome::Stalled;
            }
            _checkpointX = x;
            _checkpointY = y;
            // One sample per call is all the policy can honestly score, so
            // re-align the schedule to the next interval past now instead
            // of stepping it per interval (which a huge diff would make
            // loop ~2^32 / 5000 times). This line is only reachable with
            // _elapsedMs < kMaxLifetimeMs, so it cannot overflow.
            _nextCheckpointMs = (_elapsedMs / kStallCheckIntervalMs + 1u) *
                                kStallCheckIntervalMs;
        }

        return Outcome::Walking;
    }

private:
    bool _active = false;
    float _initialX = 0.0f;
    float _initialY = 0.0f;
    float _checkpointX = 0.0f;
    float _checkpointY = 0.0f;
    std::uint32_t _elapsedMs = 0u;
    std::uint32_t _nextCheckpointMs = kStallCheckIntervalMs;
};

// Bounded retry/backoff policy for repeated failed retreat attempts
// (TW-BOTS-002 S1). Tracks a saturating failure count, a cooldown that
// escalates once three or more failures have accumulated, and a fixed-size
// memory of recently failed endpoints so a caller can stop re-picking the
// same dead destinations. Like Progress it owns no engine state, reads no
// wall clock, uses no dynamic storage and depends only on standard C++17
// headers; all timing is driven by the caller-supplied diff in ms. The
// caller owns Reset (success, external interruption, or genuine
// combat/health recovery); the policy never resets itself on expiry.
constexpr std::uint32_t kMaxFailures = 255u;
// Cooldown after the first and second failures.
constexpr std::uint32_t kBaseRetryMs = 5000u;
// Cooldown once three or more failures have accumulated.
constexpr std::uint32_t kEscalatedRetryMs = 30000u;
// How long a failed endpoint stays excluded from re-selection.
constexpr std::uint32_t kEndpointLifetimeMs = 60000u;
// Fixed capacity of the remembered-endpoint memory.
constexpr std::size_t kEndpointCapacity = 3u;
// A candidate within this many yards of a remembered endpoint is excluded.
constexpr float kEndpointRadiusYards = 8.0f;
constexpr float kEndpointRadiusSquaredYards = kEndpointRadiusYards * kEndpointRadiusYards;

class RetryPolicy
{
public:
    RetryPolicy() = default;

    // Advances the cooldown and every remembered endpoint lifetime by diff
    // ms using saturating subtraction, so a huge diff clamps to zero rather
    // than wrapping backwards.
    void Tick(std::uint32_t diff)
    {
        _retryMs = diff >= _retryMs ? 0u : _retryMs - diff;
        for (std::size_t i = 0; i < kEndpointCapacity; ++i)
        {
            std::uint32_t& remaining = _endpoints[i].remainingMs;
            if (remaining != 0u)
                remaining = diff >= remaining ? 0u : remaining - diff;
        }
    }

    // Clears the failure count, the cooldown and every remembered endpoint.
    void Reset()
    {
        _failures = 0u;
        _retryMs = 0u;
        for (std::size_t i = 0; i < kEndpointCapacity; ++i)
        {
            _endpoints[i].x = 0.0f;
            _endpoints[i].y = 0.0f;
            _endpoints[i].remainingMs = 0u;
        }
    }

    // Records one failed attempt. The failure count always advances
    // (saturating at kMaxFailures) and the cooldown is re-armed from the
    // running count (base for the first two, escalated from the third on).
    // When hasEndpoint is set and (x, y) is finite, the failed endpoint is
    // remembered for kEndpointLifetimeMs; nonfinite data never reaches the
    // memory but the failure still counts.
    void Fail(bool hasEndpoint, float x, float y)
    {
        _failures = _failures < kMaxFailures ? _failures + 1u : kMaxFailures;
        _retryMs = _failures >= 3u ? kEscalatedRetryMs : kBaseRetryMs;
        if (hasEndpoint && std::isfinite(x) && std::isfinite(y))
            RememberEndpoint(x, y);
    }

    // True while the cooldown has fully elapsed.
    bool CanTry() const
    {
        return _retryMs == 0u;
    }

    // True once three or more failures have accumulated and the (escalated)
    // cooldown is still running.
    bool Deferred() const
    {
        return _failures >= 3u && _retryMs != 0u;
    }

    std::uint32_t Failures() const
    {
        return _failures;
    }

    std::uint32_t RetryMs() const
    {
        return _retryMs;
    }

    // True when a candidate destination must not be picked: it is nonfinite,
    // or it lies within kEndpointRadiusYards of any unexpired remembered
    // endpoint. (x, y) carry map scope that the caller manages and resets
    // via Reset; this policy is map-agnostic.
    bool Excludes(float x, float y) const
    {
        if (!std::isfinite(x) || !std::isfinite(y))
            return true;
        for (std::size_t i = 0; i < kEndpointCapacity; ++i)
        {
            if (_endpoints[i].remainingMs == 0u)
                continue;
            float const dx = x - _endpoints[i].x;
            float const dy = y - _endpoints[i].y;
            float const sq = dx * dx + dy * dy;
            if (std::isfinite(sq) && sq <= kEndpointRadiusSquaredYards)
                return true;
        }
        return false;
    }

private:
    struct Endpoint
    {
        float x = 0.0f;
        float y = 0.0f;
        std::uint32_t remainingMs = 0u;
    };

    Endpoint _endpoints[kEndpointCapacity]{};
    std::uint32_t _failures = 0u;
    std::uint32_t _retryMs = 0u;

    // Stores a finite failed endpoint for kEndpointLifetimeMs, preferring in
    // order: an unexpired endpoint within kEndpointRadiusYards (refreshed in
    // place), the first empty/expired slot, then -- all slots full -- the
    // shortest remaining lifetime with a deterministic lowest-index
    // tie-break.
    void RememberEndpoint(float x, float y)
    {
        for (std::size_t i = 0; i < kEndpointCapacity; ++i)
        {
            if (_endpoints[i].remainingMs == 0u)
                continue;
            float const dx = x - _endpoints[i].x;
            float const dy = y - _endpoints[i].y;
            float const sq = dx * dx + dy * dy;
            if (std::isfinite(sq) && sq <= kEndpointRadiusSquaredYards)
            {
                _endpoints[i].x = x;
                _endpoints[i].y = y;
                _endpoints[i].remainingMs = kEndpointLifetimeMs;
                return;
            }
        }
        for (std::size_t i = 0; i < kEndpointCapacity; ++i)
        {
            if (_endpoints[i].remainingMs == 0u)
            {
                _endpoints[i].x = x;
                _endpoints[i].y = y;
                _endpoints[i].remainingMs = kEndpointLifetimeMs;
                return;
            }
        }
        std::size_t victim = 0u;
        std::uint32_t shortest = _endpoints[0].remainingMs;
        for (std::size_t i = 1; i < kEndpointCapacity; ++i)
        {
            if (_endpoints[i].remainingMs < shortest)
            {
                shortest = _endpoints[i].remainingMs;
                victim = i;
            }
        }
        _endpoints[victim].x = x;
        _endpoints[victim].y = y;
        _endpoints[victim].remainingMs = kEndpointLifetimeMs;
    }
};

// Bounded regroup policy for autonomous hunting-group members whose group
// leader is dead or unavailable (TW-BOTS-002 S1G). A member's own survival
// always preempts group duty: while the member is threatened the caller
// must fall through to ordinary combat/retreat evaluation immediately.
// Unthreatened members wait for the leader up to kLeaderLostDeadlineMs of
// accumulated time, then the caller disbands and resumes solo citizen
// activity. Value-only like the rest of this header: no engine state, no
// wall clock; the caller accumulates waitedMs from supplied diffs
// (saturating at the deadline) and resets it when the leader is usable.
enum class LeaderLoss { FallThrough, Wait, Disband };
// Initial regroup deadline: a policy default to validate from observed
// leader recovery times, not a calibrated gameplay constant.
constexpr std::uint32_t kLeaderLostDeadlineMs = 60000u;

// Decides the member action for an unusable leader given the accumulated
// waitedMs and whether the member is currently threatened (in combat or
// holding a target).
inline LeaderLoss EvaluateLeaderLoss(std::uint32_t waitedMs, bool threatened)
{
    if (threatened)
        return LeaderLoss::FallThrough;
    if (waitedMs >= kLeaderLostDeadlineMs)
        return LeaderLoss::Disband;
    return LeaderLoss::Wait;
}
} // namespace CitizenRecovery
} // namespace Companion

#endif // TORTOISE_WOW_COMPANION_CITIZEN_RECOVERY_H
