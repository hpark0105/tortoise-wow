// PORT-022 (KAP-558): the bounded nonblocking companion-conversation
// transport.
//
// A player who addresses a current party companion through one declared
// chat channel gets at most one bounded, personality-consistent, text-only
// reply from the accepted local-model adapter. Conversation has no path to
// gameplay directives: the world sends only the sanitized message text and
// the companion's persisted profile name, and says back only sanitized
// text.
//
// The world thread never performs DNS, connect, read, write or wait: it
// submits one request per companion (queue depth 1; a companion already in
// service refuses new messages) and polls for a ready reply. A single
// worker thread owns all I/O under a hard per-round deadline; every failure
// (offline, busy, slow, malformed, oversized) produces no reply and never
// delays a world tick. A reply is consumed only while the party signature
// (group id + leader) captured at submit still matches and the reply is
// fresh - leaving the group invalidates outstanding work.
//
// Wire contract with the adapter (POST /converse?profile=<name>):
//   request body   = sanitized message text (UTF-8, <= kMaxTextBytes)
//   response 200   = sanitized reply (UTF-8, <= kMaxReplyBytes)
//   any other      = no reply
// The strict model output schema ({"reply": "..."}) is enforced inside the
// adapter; the transport boundary is plain sanitized text on both sides.
//
// ConvRound is a value-only state machine: the transport drives it under
// its lock, and the value test drives the same transitions directly,
// without sockets or threads.

#ifndef TORTOISE_COMPANION_CONVERSATION_TRANSPORT_H
#define TORTOISE_COMPANION_CONVERSATION_TRANSPORT_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace Companion
{
namespace Conversation
{

inline constexpr uint32_t kMaxBots = 8;           // companions held at once
inline constexpr uint32_t kRoundTimeoutMs = 4000; // hard per-round I/O deadline
inline constexpr uint64_t kReplyAgeMs = 10000;    // reply freshness budget
inline constexpr uint32_t kMaxTextBytes = 200;    // sanitized inbound text
inline constexpr uint32_t kMaxReplyBytes = 120;   // sanitized outbound reply

enum class ConvState { Idle, InFlight, Ready };

// One companion's conversation round. Value-only: no engine pointers, no
// I/O, no threads - the same code runs under the transport lock and
// standalone in the value test.
struct ConvRound
{
    ConvState state = ConvState::Idle;
    uint32_t botLow = 0;
    uint32_t partyGroup = 0;   // group id captured at submit (0 = none)
    uint32_t partyLeader = 0;  // leader low GUID captured at submit
    uint64_t submitStamp = 0;
    uint64_t submitClockMs = 0;
    uint32_t profileCode = 0;  // Personality::Profile at submit
    std::string text;          // sanitized inbound message
    std::string reply;         // sanitized outbound reply (Ready only)
    uint64_t replyClockMs = 0;

    enum class SubmitResult { Accepted, Busy, Rejected };

    // One pending round per companion. A submit while in service is Busy
    // (the worker is the only sender; a slow service would otherwise
    // drop the newer message). Empty or oversize text is Rejected.
    SubmitResult Submit(uint32_t botLowIn, uint32_t group, uint32_t leader,
                        uint32_t profileCodeIn, std::string const& textIn,
                        uint64_t nowMs, uint64_t stamp)
    {
        if (textIn.empty() || textIn.size() > kMaxTextBytes)
            return SubmitResult::Rejected;
        if (state == ConvState::InFlight)
            return SubmitResult::Busy;
        state = ConvState::InFlight;
        botLow = botLowIn;
        partyGroup = group;
        partyLeader = leader;
        profileCode = profileCodeIn;
        text = textIn;
        submitStamp = stamp;
        submitClockMs = nowMs;
        reply.clear();
        replyClockMs = 0;
        return SubmitResult::Accepted;
    }

    enum class Result { Applied, Discarded, Failed };

    // The worker's outcome for the round stamped `stamp`. A result whose
    // stamp no longer matches (a newer round or an invalidation landed
    // first) is discarded: it belongs to a dead round.
    Result OnResult(uint64_t stamp, bool ok, std::string const& replyIn,
                    uint64_t nowMs)
    {
        if (state != ConvState::InFlight || submitStamp != stamp)
            return Result::Discarded;
        if (!ok || replyIn.empty() || replyIn.size() > kMaxReplyBytes)
        {
            state = ConvState::Idle;
            reply.clear();
            replyClockMs = 0;
            return Result::Failed;
        }
        reply = replyIn;
        replyClockMs = nowMs;
        state = ConvState::Ready;
        return Result::Applied;
    }

    // Party membership change (leave, kick, disband) or companion offline:
    // outstanding work is dead and new requests are refused until the
    // companion re-joins (the caller re-submits with a fresh signature).
    void Invalidate()
    {
        state = ConvState::Idle;
        submitStamp = 0;
        submitClockMs = 0;
        text.clear();
        reply.clear();
        replyClockMs = 0;
    }

    // World-thread poll. A ready reply is consumed only while the party
    // signature is unchanged and the reply is fresh; otherwise the round is
    // dropped. Returns true and fills `out` on a valid consumption.
    bool Poll(uint32_t group, uint32_t leader, uint64_t nowMs, std::string& out)
    {
        if (state != ConvState::Ready)
            return false;
        if (group != partyGroup || leader != partyLeader)
        {
            Invalidate();
            return false;
        }
        if (nowMs - replyClockMs > kReplyAgeMs)
        {
            Invalidate();
            return false;
        }
        out = reply;
        state = ConvState::Idle;
        reply.clear();
        return true;
    }
};

class ConversationTransport
{
    public:
        ConversationTransport() = default;
        ~ConversationTransport();
        ConversationTransport(ConversationTransport const&) = delete;
        ConversationTransport& operator=(ConversationTransport const&) = delete;

        // An empty or malformed url leaves the transport disabled: no
        // thread, no I/O, every call a no-op (the deterministic regression
        // path). `debug` gates the chatty per-round lines.
        bool Init(std::string const& url, uint64_t nowMs, bool debug);
        bool Enabled() const;
        void Shutdown();

        // World-thread API (no I/O, no blocking).
        bool Submit(uint32_t botLow, uint32_t partyGroup, uint32_t partyLeader,
                    uint32_t profileCode, std::string const& text,
                    uint64_t nowMs);
        bool Poll(uint32_t botLow, uint32_t partyGroup, uint32_t partyLeader,
                  uint64_t nowMs, std::string& outReply);
        void Invalidate(uint32_t botLow);

        // Diagnostics (world-thread reads).
        uint32_t CountSubmits() const;
        uint32_t CountReplies() const;
        uint32_t CountFails() const;

    protected:
        struct Slot
        {
            bool inUse = false;
            ConvRound round;
            uint8_t attempt = 0; // PORT-031: 1 = first service, 2 = bounded retry
        };
        static Slot* FindSlot(Slot* slots, uint32_t botLow);
        void WorkerLoop();
        // One bounded I/O round (worker thread only).
        bool SendRound(std::string const& path, std::string const& body,
                       std::string& outReply);

        mutable std::mutex m_lock;
        std::condition_variable m_cv;
        std::thread* m_worker = nullptr;
        std::atomic_bool m_enabled{false};
        bool m_stopping = false;
        bool m_debug = false;
        Slot m_slots[kMaxBots];
        uint64_t m_nextStamp = 1;
        std::string m_host;
        uint16_t m_port = 0;
        uint32_t m_submits = 0;
        uint32_t m_replies = 0;
        uint32_t m_fails = 0;
};

} // namespace Conversation
} // namespace Companion

#endif