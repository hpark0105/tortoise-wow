// PORT-018 (KAP-558): the bounded nonblocking party-planner transport.
//
// The world thread never performs DNS, connect, read, write or wait: it
// submits one shared party request (queue depth 1; an in-service round
// refuses new submits until its result lands) and polls for validated
// offers. A single worker thread owns all I/O under a hard per-round
// deadline; every failure mode (offline, busy, slow, malformed, oversized,
// stale, duplicate, reordered) fails closed to the deterministic policies
// without delaying a world tick. Shutdown leaves the transport clean, so
// a later Init starts a fresh worker and an empty session table.
//
// Round is a value-only state machine: the transport drives it under its
// lock, and the value test
// (docker/test_companion_planner_transport_value.cpp) drives the same
// transitions directly, without sockets or threads.

#ifndef TORTOISE_COMPANION_PLANNER_TRANSPORT_H
#define TORTOISE_COMPANION_PLANNER_TRANSPORT_H

#include "PlannerProtocol.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace Companion
{
namespace Planner
{

inline constexpr uint32_t kMaxSessions = 8;        // party sessions held at once
inline constexpr uint64_t kRoundTimeoutMs = 5000;  // hard per-round I/O deadline
inline constexpr uint64_t kCooldownMs = 60000;     // backoff after repeated failures
inline constexpr uint32_t kMaxConsecutiveTimeouts = 3;
inline constexpr uint64_t kPlannerPaceMs = 2000;   // one shared round per party per 2 s

enum class RoundState { Idle, InFlight, Cooldown };

// Outcome of one I/O round on the worker.
enum class RoundResult { Ok, Timeout, ConnectFail, Oversized, HttpError };

// One party session's planner round. Value-only transitions: no engine
// pointers, no I/O, no threads - the same code runs under the transport
// lock and standalone in the value test.
struct Round
{
    RoundState state = RoundState::Idle;
    uint32_t partySessionGeneration = 0; // bumped on every party session change
    uint64_t submitStamp = 0;            // transport-issued monotonic stamp
    uint64_t requestClockMs = 0;         // engine clock at submission
    uint32_t timeoutCount = 0;           // consecutive failed rounds
    uint64_t cooldownUntilMs = 0;
    bool responseReady = false;
    uint8_t requestBytes[kRequestBytes] = {};
    uint32_t requestLen = 0;
    uint8_t responseBytes[kMaxPayloadBytes] = {};
    uint32_t responseLen = 0;

    enum class SubmitResult { Accepted, Superseded, Cooldown, Rejected };

    // req must be exactly kRequestBytes; a bad length is a caller bug and is
    // rejected fail-closed. A submit while a round is in flight keeps the
    // newest request (queue depth 1, newest wins): the worker discards the
    // older result through the stamp check in OnResult.
    SubmitResult Submit(uint8_t const* req, uint32_t reqLen, uint64_t nowMs,
                        uint64_t stamp)
    {
        if (!req || reqLen != kRequestBytes)
            return SubmitResult::Rejected;
        if (state == RoundState::Cooldown && nowMs < cooldownUntilMs)
            return SubmitResult::Cooldown;
        bool const supersede = (state == RoundState::InFlight);
        state = RoundState::InFlight;
        submitStamp = stamp;
        requestClockMs = nowMs;
        for (uint32_t i = 0; i < kRequestBytes; ++i)
            requestBytes[i] = req[i];
        requestLen = kRequestBytes;
        responseReady = false;
        responseLen = 0;
        return supersede ? SubmitResult::Superseded : SubmitResult::Accepted;
    }

    enum class ActionResult { Applied, Discarded, Failed, Cooldown, Completed };

    // The worker's outcome for the round stamped `stamp`. A result whose
    // stamp no longer matches (a newer submit or an invalidation landed
    // first) is discarded: it belongs to a dead round and must never touch
    // the live state.
    ActionResult OnResult(uint64_t stamp, RoundResult result,
                          uint8_t const* bytes, uint32_t len, uint64_t nowMs)
    {
        if (state != RoundState::InFlight || submitStamp != stamp)
            return ActionResult::Discarded;
        if (result != RoundResult::Ok)
        {
            ++timeoutCount;
            responseReady = false;
            responseLen = 0;
            if (timeoutCount >= kMaxConsecutiveTimeouts)
            {
                timeoutCount = 0;
                state = RoundState::Cooldown;
                cooldownUntilMs = nowMs + kCooldownMs;
                return ActionResult::Cooldown;
            }
            state = RoundState::Idle;
            return ActionResult::Failed;
        }
        timeoutCount = 0;
        if (bytes && len > 0 && len <= kMaxPayloadBytes)
        {
            for (uint32_t i = 0; i < len; ++i)
                responseBytes[i] = bytes[i];
            responseLen = len;
            responseReady = true;
        }
        state = RoundState::Idle;
        return ActionResult::Completed;
    }

    // Party session change (join, leave, removal, disband, leader change):
    // a fresh generation; every outstanding result of the prior generation
    // is dead (its stamp can no longer match a live round).
    void Invalidate()
    {
        ++partySessionGeneration;
        state = RoundState::Idle;
        submitStamp = 0;
        responseReady = false;
        responseLen = 0;
        timeoutCount = 0;
        cooldownUntilMs = 0;
    }

    bool InCooldown(uint64_t nowMs) const
    {
        return state == RoundState::Cooldown && nowMs < cooldownUntilMs;
    }
};

class PlannerTransport
{
    public:
        PlannerTransport() = default;
        ~PlannerTransport();
        PlannerTransport(PlannerTransport const&) = delete;
        PlannerTransport& operator=(PlannerTransport const&) = delete;

        // An empty or malformed url leaves the transport disabled: no
        // thread, no I/O, every call a no-op. That is the default, and the
        // deterministic regression path. `debug` gates the chatty per-round
        // lines (timeouts, drops and cooldowns are always logged).
        bool Init(std::string const& url, uint64_t nowMs, bool debug);
        bool Enabled() const;
        void Shutdown();

        // World-thread API (no I/O, no blocking).
        // One shared request per party (ownerLow = the party leader's low
        // GUID); queue depth 1 - a submit while a round is in service
        // is refused (the worker is the only sender) and the next
        // pace tick resubmits with fresh state.
        bool SubmitShared(uint32_t ownerLow, uint8_t const* req, uint32_t reqLen,
                          uint64_t nowMs);
        void InvalidateSession(uint32_t ownerLow);
        uint32_t SessionGeneration(uint32_t ownerLow) const;
        // Validate a ready response against the stored request (all
        // fail-closed protocol checks, at the engine clock) and hand back
        // the step for botLow. A rejected round is consumed; a valid
        // round loses only this bot's step, so the party's other bots
        // can still fetch theirs (unclaimed steps clear on the next
        // submit or invalidation).
        bool FetchOffer(uint32_t ownerLow, uint32_t botLow, uint64_t nowMs,
                        Step& out);
        // Diagnostics (world-thread reads).
        uint32_t CountSubmits() const;
        uint32_t CountOffers() const;
        uint32_t CountRejects() const;
        uint32_t CountTimeouts() const;

    protected:
        struct Slot
        {
            bool inUse = false;
            uint32_t ownerLow = 0;
            Round round;
        };
        static Slot* FindSlot(Slot* slots, uint32_t ownerLow);
        void WorkerLoop();
        // One bounded I/O round for `payload` (the worker thread only).
        RoundResult SendRound(uint8_t const* payload, uint8_t* out, uint32_t& outLen);

        mutable std::mutex m_lock;
        std::condition_variable m_cv;
        std::thread* m_worker = nullptr;
        std::atomic_bool m_enabled{false};
        bool m_stopping = false;
        bool m_debug = false;
        Slot m_slots[kMaxSessions];
        uint64_t m_nextStamp = 1;
        std::string m_host;
        uint16_t m_port = 0;
        uint32_t m_submits = 0;
        uint32_t m_offers = 0;
        uint32_t m_rejects = 0;
        uint32_t m_timeouts = 0;
};

} // namespace Planner
} // namespace Companion

#endif
