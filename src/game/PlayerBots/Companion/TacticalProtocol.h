#ifndef TORTOISE_COMPANION_TACTICAL_PROTOCOL_H
#define TORTOISE_COMPANION_TACTICAL_PROTOCOL_H

#include <cstdint>

namespace Companion { namespace Tactical {

static constexpr uint32_t kProtocolVersion = 2;
static constexpr uint32_t kMaxCandidates = 8;
static constexpr uint64_t kMaxResponseAgeMs = 2000;
static constexpr uint64_t kMaxCandidateLifetimeMs = 5000;

struct Envelope
{
    uint64_t requestId = 0;
    uint32_t ownerGuid = 0;
    uint32_t botGuid = 0;
    uint32_t observationVersion = 0;
    uint32_t capabilityVersion = 0;
    uint32_t loginGeneration = 0;
    uint32_t orderGeneration = 0;
    uint64_t captureTimeMs = 0;
};

struct Candidate
{
    uint32_t slotIndex = 0;
    uint32_t abilityId = 0;
    uint32_t gateValue = 0;
    uint64_t expiresAtMs = 0;
};

struct CapabilityCatalog
{
    uint32_t version = 0;
    uint32_t count = 0;
    uint32_t abilityIds[kMaxCandidates] = {};
};

enum class Reject : uint8_t { Ok, BadId, OwnerHeld, Stale, Mismatch,
    Capability, Reordered, Duplicate, Expired, OutOfRange };

struct Verdict { Reject reject = Reject::Ok; bool legal() const { return reject == Reject::Ok; } };

inline bool Contains(CapabilityCatalog const& c, uint32_t id)
{
    for (uint32_t i = 0; i < c.count && i < kMaxCandidates; ++i)
        if (c.abilityIds[i] == id) return true;
    return false;
}

inline Verdict Validate(Envelope const& req, Envelope const& res,
                        Candidate const* candidates, uint32_t count,
                        CapabilityCatalog const& catalog, bool ownerHeld,
                        uint64_t nowMs)
{
    if (!req.requestId || !req.ownerGuid || !req.botGuid || !candidates) return {Reject::BadId};
    if (ownerHeld) return {Reject::OwnerHeld};
    if (count == 0 || count > kMaxCandidates) return {Reject::OutOfRange};
    if (res.requestId != req.requestId || res.ownerGuid != req.ownerGuid ||
        res.botGuid != req.botGuid || res.observationVersion != req.observationVersion ||
        res.capabilityVersion != req.capabilityVersion || res.loginGeneration != req.loginGeneration ||
        res.orderGeneration != req.orderGeneration) return {Reject::Mismatch};
    if (nowMs < req.captureTimeMs || nowMs - req.captureTimeMs > kMaxResponseAgeMs) return {Reject::Stale};
    uint32_t previousSlot = 0;
    for (uint32_t i = 0; i < count; ++i)
    {
        Candidate const& c = candidates[i];
        if (c.slotIndex >= kMaxCandidates || (i && c.slotIndex <= previousSlot)) return {Reject::Reordered};
        if (i && c.abilityId == candidates[i - 1].abilityId) return {Reject::Duplicate};
        if (!Contains(catalog, c.abilityId) || catalog.version != req.capabilityVersion) return {Reject::Capability};
        if (c.gateValue > 0xFFFFu || c.expiresAtMs < nowMs || c.expiresAtMs - nowMs > kMaxCandidateLifetimeMs) return {Reject::Expired};
        previousSlot = c.slotIndex;
    }
    return {};
}

enum class RoundState : uint8_t { Idle, InFlight, OnHold, CapabilityStale };
struct Round
{
    RoundState state = RoundState::Idle;
    uint64_t stamp = 0;
    uint64_t captureTimeMs = 0;
    bool responseReady = false;

    bool Submit(uint64_t nowMs, uint64_t newStamp)
    {
        if (state == RoundState::OnHold || state == RoundState::CapabilityStale) return false;
        state = RoundState::InFlight; captureTimeMs = nowMs; stamp = newStamp; responseReady = false; return true;
    }
    bool Complete(uint64_t responseStamp, bool valid)
    {
        if (state != RoundState::InFlight || responseStamp != stamp) return false;
        state = RoundState::Idle; responseReady = valid; return valid;
    }
    void OwnerHold() { state = RoundState::OnHold; stamp = 0; responseReady = false; }
    void CapabilityChanged() { state = RoundState::CapabilityStale; stamp = 0; responseReady = false; }
    void Resume() { if (state != RoundState::InFlight) state = RoundState::Idle; }
};

} }
#endif
