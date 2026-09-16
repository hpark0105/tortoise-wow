// PORT-017 (KAP-558): the versioned planner protocol.
// Value-only, fixed-width little-endian, no strings, no floats, no
// pointers: one immutable party observation (request) and one shared
// response per planner round. The byte layout, limits and failure
// modes are specified in docs/bots/ralph/companion-port/planner-protocol.md.
// Nothing in this header talks to a transport, a model or the world:
// the transport (PORT-018) must satisfy the budgets there, and the
// real model connection (PORT-021) consumes these same types through
// an adapter.
#ifndef TORTOISE_COMPANION_PLANNER_PROTOCOL_H
#define TORTOISE_COMPANION_PLANNER_PROTOCOL_H
#include <cstdint>
#include <type_traits>

namespace Companion
{
namespace Planner
{

inline constexpr uint32_t kProtocolVersion = 1;
inline constexpr uint32_t kMagic = 0x31504C43u; // "CLP1" little-endian tag

// Layout
inline constexpr uint32_t kEnvelopeBytes = 40;
inline constexpr uint32_t kRequestBodyBytes = 44;
inline constexpr uint32_t kStepBytes = 32;
inline constexpr uint32_t kRequestBytes = kEnvelopeBytes + kRequestBodyBytes; // 84, fixed
inline constexpr uint32_t kMaxResponseBytes = kEnvelopeBytes + 8 * kStepBytes; // 296

// Limits (both sides enforce; see the protocol doc)
inline constexpr uint32_t kMaxSteps = 8;
inline constexpr uint32_t kMaxPartyBots = 4;
inline constexpr uint32_t kMaxPayloadBytes = 4096;
inline constexpr uint64_t kMaxResponseAgeMs = 1000;
inline constexpr uint64_t kMaxStepLifetimeMs = 5000;
inline constexpr uint32_t kMaxPreference = 0xFFFF; // packed (id<<8)|value payload (PORT-019)

// The closed action vocabulary: a response can only express these.
// No field in the protocol can express a command, coordinate, log
// line, prompt transcript or arbitrary text.
enum class Action : uint8_t
{
    None = 0,       // no step (idle suggestion); target must be 0
    Hold = 1,       // .bothold; target must be 0
    Follow = 2,     // .botfollow; target must be 0
    Assist = 3,     // .botassist; target must be a resolvable hostile
    Defend = 4,     // .botdefend; owner-locked, target must be 0
    Loot = 5,       // loot goal; target must be a resolvable corpse
    Regroup = 6,    // regroup goal; target must be 0
    Preference = 7, // personality expression only (PORT-019); target 0
};
inline constexpr uint8_t kActionCount = 8;

inline constexpr bool RequiresTarget(Action a)
{
    return a == Action::Assist || a == Action::Loot;
}
inline constexpr bool ForbidsTarget(Action a) { return !RequiresTarget(a); }

// Shared envelope (first 40 bytes of request and response)
struct Envelope
{
    uint32_t magic = kMagic;
    uint32_t protocolVersion = kProtocolVersion;
    uint64_t requestId = 0;          // non-zero; the response echoes it
    uint32_t ownerGuid = 0;          // party owner; echoed
    uint32_t observationVersion = 0; // must equal kObservationVersion
    uint64_t captureTimeMs = 0;      // engine clock at capture
    uint32_t stepCount = 0;          // 0 for requests, 1..kMaxSteps for responses
    uint32_t totalSize = 0;          // must equal the received byte length
};

struct BotSlot
{
    uint32_t botGuid = 0; // non-zero for used slots
    uint8_t cls = 0;      // class of the bot
    uint8_t reserved[3] = {0, 0, 0};
};

// Request body (fixed 44 bytes; unused slots zero-filled)
struct RequestBody
{
    uint32_t generation = 0; // order generation at capture
    uint8_t flags = 0;       // bit0 held, bit1 following, bit2 ownerAvailable
    uint8_t reserved[3] = {0, 0, 0};
    uint32_t botCount = 0;   // 1..kMaxPartyBots
    BotSlot bots[kMaxPartyBots];
};

// One proposed step (32 bytes). The member order is natural-alignment, not
// the wire order: the 64-bit field sits at offset 16 so the struct is
// exactly 32 bytes without packing pragmas. EncodeStep/DecodeStep below own
// the wire mapping (target_guid @16, expires_at_ms @20, preference @28).
struct Step
{
    uint32_t botGuid = 0;         // one owned companion
    uint32_t loginGeneration = 0; // must equal the bot's current login generation
    uint32_t orderGeneration = 0; // must equal the current order generation
    uint8_t action = 0;           // one of Action
    uint8_t reserved[3] = {0, 0, 0};
    uint64_t expiresAtMs = 0;     // step deadline on the engine clock
    uint32_t targetGuid = 0;      // resolvable object GUID, or 0
    uint32_t preference = 0;      // 0..kMaxPreference; packed (id<<8)|value (PORT-019)
};

static_assert(std::is_trivially_copyable<Envelope>::value &&
              std::is_standard_layout<Envelope>::value, "envelope must stay a bounded value");
static_assert(sizeof(Envelope) == kEnvelopeBytes, "envelope layout drift");
static_assert(sizeof(BotSlot) == 8, "bot slot layout drift");
static_assert(sizeof(RequestBody) == kRequestBodyBytes, "request body layout drift");
static_assert(sizeof(Step) == kStepBytes, "step layout drift");

// ---------------------------------------------------------------------------
// Fail-closed validation. Every check is a pure value comparison; a bad
// response is all-or-nothing: one bad step rejects the whole response and
// the caller keeps the current deterministic behavior.
// ---------------------------------------------------------------------------
enum class Reject : uint32_t
{
    Ok = 0,
    BadMagic,        // magic bytes do not match
    UnknownVersion,  // protocol or observation version not 1 / kObservationVersion
    SizeMismatch,    // received length != the envelope's expected length
    Oversized,       // received length > kMaxPayloadBytes
    TooManySteps,    // step or bot count outside the range
    BadId,           // zero request id, owner, bot guid or capture time
    BadReserved,     // a reserved byte is non-zero
    UnknownAction,   // action byte outside the closed vocabulary
    MissingTarget,   // an action that requires a target carries none
    ForbiddenTarget, // an action that forbids a target carries one
    OutOfRange,      // preference above kMaxPreference
    Expired,         // step lifetime beyond the bound, or already past expiry
    Stale,           // response age beyond kMaxResponseAgeMs
    Mismatched,      // response does not echo the request envelope
};

struct Verdict
{
    Reject reject = Reject::Ok;
    bool legal() const { return reject == Reject::Ok; }
};

const char* RejectName(Reject r);

// expectedRequestBytes is kRequestBytes; expectedResponseBytes is
// kEnvelopeBytes + kStepBytes * stepCount.
Verdict ValidateEnvelope(Envelope const& env, uint32_t receivedBytes,
                         uint32_t expectedBytes, bool isRequest,
                         uint64_t nowMs);
Verdict ValidateRequestBody(RequestBody const& body);
Verdict ValidateStep(Step const& step, Envelope const& env, uint64_t nowMs);
// req is the request envelope the response answers; it must be echoed.
Verdict ValidateResponse(Envelope const& req, Envelope const& res,
                         Step const* steps, uint32_t receivedBytes,
                         uint64_t nowMs);
Verdict ValidateRequest(Envelope const& env, RequestBody const& body,
                        uint32_t receivedBytes, uint64_t nowMs);

// ---------------------------------------------------------------------------
// Fixed-width little-endian pack/unpack. The golden vectors in the tests
// pin the exact bytes across languages (the Python reference encoder in
// docker/personality-service/fake_planner.py must stay byte-identical).
// ---------------------------------------------------------------------------
void EncodeEnvelope(uint8_t* out, Envelope const& e);
Envelope DecodeEnvelope(uint8_t const* in);
void EncodeRequestBody(uint8_t* out, RequestBody const& b);
RequestBody DecodeRequestBody(uint8_t const* in);
void EncodeStep(uint8_t* out, Step const& s);
Step DecodeStep(uint8_t const* in);

} // namespace Planner
} // namespace Companion

#include "Observation.h" // kObservationVersion

namespace Companion
{
namespace Planner
{

inline constexpr uint32_t ReadU32(uint8_t const* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
inline constexpr uint64_t ReadU64(uint8_t const* p)
{
    return (uint64_t)ReadU32(p) | ((uint64_t)ReadU32(p + 4) << 32);
}
inline void WriteU32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
inline void WriteU64(uint8_t* p, uint64_t v)
{
    WriteU32(p, (uint32_t)v);
    WriteU32(p + 4, (uint32_t)(v >> 32));
}

inline void EncodeEnvelope(uint8_t* out, Envelope const& e)
{
    WriteU32(out + 0, e.magic);
    WriteU32(out + 4, e.protocolVersion);
    WriteU64(out + 8, e.requestId);
    WriteU32(out + 16, e.ownerGuid);
    WriteU32(out + 20, e.observationVersion);
    WriteU64(out + 24, e.captureTimeMs);
    WriteU32(out + 32, e.stepCount);
    WriteU32(out + 36, e.totalSize);
}

inline Envelope DecodeEnvelope(uint8_t const* in)
{
    Envelope e;
    e.magic = ReadU32(in + 0);
    e.protocolVersion = ReadU32(in + 4);
    e.requestId = ReadU64(in + 8);
    e.ownerGuid = ReadU32(in + 16);
    e.observationVersion = ReadU32(in + 20);
    e.captureTimeMs = ReadU64(in + 24);
    e.stepCount = ReadU32(in + 32);
    e.totalSize = ReadU32(in + 36);
    return e;
}

inline void EncodeRequestBody(uint8_t* out, RequestBody const& b)
{
    WriteU32(out + 0, b.generation);
    out[4] = b.flags;
    out[5] = b.reserved[0]; out[6] = b.reserved[1]; out[7] = b.reserved[2];
    WriteU32(out + 8, b.botCount);
    for (uint32_t i = 0; i < kMaxPartyBots; ++i)
    {
        uint8_t* s = out + 12 + 8 * i;
        WriteU32(s + 0, b.bots[i].botGuid);
        s[4] = b.bots[i].cls;
        s[5] = b.bots[i].reserved[0];
        s[6] = b.bots[i].reserved[1];
        s[7] = b.bots[i].reserved[2];
    }
}

inline RequestBody DecodeRequestBody(uint8_t const* in)
{
    RequestBody b;
    b.generation = ReadU32(in + 0);
    b.flags = in[4];
    b.reserved[0] = in[5]; b.reserved[1] = in[6]; b.reserved[2] = in[7];
    b.botCount = ReadU32(in + 8);
    for (uint32_t i = 0; i < kMaxPartyBots; ++i)
    {
        uint8_t const* s = in + 12 + 8 * i;
        b.bots[i].botGuid = ReadU32(s + 0);
        b.bots[i].cls = s[4];
        b.bots[i].reserved[0] = s[5];
        b.bots[i].reserved[1] = s[6];
        b.bots[i].reserved[2] = s[7];
    }
    return b;
}

inline void EncodeStep(uint8_t* out, Step const& s)
{
    WriteU32(out + 0, s.botGuid);
    WriteU32(out + 4, s.loginGeneration);
    WriteU32(out + 8, s.orderGeneration);
    out[12] = (uint8_t)s.action;
    out[13] = s.reserved[0]; out[14] = s.reserved[1]; out[15] = s.reserved[2];
    WriteU32(out + 16, s.targetGuid);
    WriteU64(out + 20, s.expiresAtMs);
    WriteU32(out + 28, s.preference);
}

inline Step DecodeStep(uint8_t const* in)
{
    Step s;
    s.botGuid = ReadU32(in + 0);
    s.loginGeneration = ReadU32(in + 4);
    s.orderGeneration = ReadU32(in + 8);
    s.action = in[12];
    s.reserved[0] = in[13]; s.reserved[1] = in[14]; s.reserved[2] = in[15];
    s.targetGuid = ReadU32(in + 16);
    s.expiresAtMs = ReadU64(in + 20);
    s.preference = ReadU32(in + 28);
    return s;
}

inline const char* RejectName(Reject r)
{
    switch (r)
    {
        case Reject::Ok: return "ok";
        case Reject::BadMagic: return "bad-magic";
        case Reject::UnknownVersion: return "unknown-version";
        case Reject::SizeMismatch: return "size-mismatch";
        case Reject::Oversized: return "oversized";
        case Reject::TooManySteps: return "too-many-steps";
        case Reject::BadId: return "bad-id";
        case Reject::BadReserved: return "bad-reserved";
        case Reject::UnknownAction: return "unknown-action";
        case Reject::MissingTarget: return "missing-target";
        case Reject::ForbiddenTarget: return "forbidden-target";
        case Reject::OutOfRange: return "out-of-range";
        case Reject::Expired: return "expired";
        case Reject::Stale: return "stale";
        case Reject::Mismatched: return "mismatched";
    }
    return "unknown";
}

inline Verdict ValidateEnvelope(Envelope const& env, uint32_t receivedBytes,
                                uint32_t expectedBytes, bool isRequest,
                                uint64_t nowMs)
{
    Verdict v;
    if (receivedBytes > kMaxPayloadBytes)
        v.reject = Reject::Oversized; // size enforced before any field parse
    else if (receivedBytes != expectedBytes)
        v.reject = Reject::SizeMismatch;
    else if (env.magic != kMagic)
        v.reject = Reject::BadMagic;
    else if (env.protocolVersion != kProtocolVersion ||
             env.observationVersion != kObservationVersion)
        v.reject = Reject::UnknownVersion;
    else if (env.requestId == 0 || env.ownerGuid == 0 || env.captureTimeMs == 0)
        v.reject = Reject::BadId;
    else if (isRequest ? env.stepCount != 0 :
             (env.stepCount == 0 || env.stepCount > kMaxSteps))
        v.reject = Reject::TooManySteps;
    else if (env.totalSize != receivedBytes)
        v.reject = Reject::SizeMismatch;
    else if (!isRequest && nowMs >= env.captureTimeMs &&
             nowMs - env.captureTimeMs > kMaxResponseAgeMs)
        v.reject = Reject::Stale;
    return v;
}

inline Verdict ValidateRequestBody(RequestBody const& body)
{
    Verdict v;
    if (body.reserved[0] || body.reserved[1] || body.reserved[2])
        v.reject = Reject::BadReserved;
    else if (body.botCount == 0 || body.botCount > kMaxPartyBots)
        v.reject = Reject::TooManySteps;
    else
    {
        for (uint32_t i = 0; v.legal() && i < kMaxPartyBots; ++i)
        {
            BotSlot const& b = body.bots[i];
            bool used = i < body.botCount;
            if (used && b.botGuid == 0)
                v.reject = Reject::BadId;
            else if (!used && (b.botGuid != 0 || b.cls != 0 ||
                               b.reserved[0] || b.reserved[1] || b.reserved[2]))
                v.reject = Reject::BadReserved;
            else if (used && (b.reserved[0] || b.reserved[1] || b.reserved[2]))
                v.reject = Reject::BadReserved;
        }
    }
    return v;
}

inline Verdict ValidateStep(Step const& s, Envelope const& env, uint64_t nowMs)
{
    Verdict v;
    if (s.botGuid == 0)
        v.reject = Reject::BadId;
    else if (s.reserved[0] || s.reserved[1] || s.reserved[2])
        v.reject = Reject::BadReserved;
    else if (s.action >= kActionCount)
        v.reject = Reject::UnknownAction;
    else if (RequiresTarget(Action(s.action)) && s.targetGuid == 0)
        v.reject = Reject::MissingTarget;
    else if (ForbidsTarget(Action(s.action)) && s.targetGuid != 0)
        v.reject = Reject::ForbiddenTarget;
    else if (s.preference > kMaxPreference)
        v.reject = Reject::OutOfRange;
    else if (nowMs >= s.expiresAtMs)
        v.reject = Reject::Expired;
    else if (s.expiresAtMs > env.captureTimeMs &&
             s.expiresAtMs - env.captureTimeMs > kMaxStepLifetimeMs)
        v.reject = Reject::Expired;
    return v;
}

inline Verdict ValidateResponse(Envelope const& req, Envelope const& res,
                                Step const* steps, uint32_t receivedBytes,
                                uint64_t nowMs)
{
    uint32_t const expected = kEnvelopeBytes + kStepBytes * res.stepCount;
    Verdict v = ValidateEnvelope(res, receivedBytes, expected, false, nowMs);
    if (!v.legal())
        return v;
    if (res.requestId != req.requestId || res.ownerGuid != req.ownerGuid)
        return Verdict{Reject::Mismatched};
    for (uint32_t i = 0; v.legal() && i < res.stepCount; ++i)
        v = ValidateStep(steps[i], res, nowMs);
    return v;
}

inline Verdict ValidateRequest(Envelope const& env, RequestBody const& body,
                               uint32_t receivedBytes, uint64_t nowMs)
{
    (void)nowMs; // requests have no age budget; the transport does
    Verdict v = ValidateEnvelope(env, receivedBytes, kRequestBytes, true, nowMs);
    if (!v.legal())
        return v;
    return ValidateRequestBody(body);
}

} // namespace Planner
} // namespace Companion
#endif
