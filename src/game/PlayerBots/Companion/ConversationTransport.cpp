// PORT-022 (KAP-558): bounded nonblocking companion-conversation transport.
// POSIX implementation: one worker thread, one in-flight round per
// companion, a hard per-round deadline and fail-closed outcomes. The world
// thread only ever calls Submit/Poll/Invalidate (see
// Companion/ConversationTransport.h); every socket, poll and DNS call in
// this file runs on the worker.

#include "Companion/ConversationTransport.h"
#include "Companion/Personality.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "Log.h"
#include "Timer.h"

namespace
{
constexpr uint32_t kBufBytes = 16 * 1024; // header (<= 8K) + body (<= 128B) + slack
} // namespace

namespace Companion
{
namespace Conversation
{

ConversationTransport::~ConversationTransport()
{
    Shutdown();
}

bool ConversationTransport::Enabled() const
{
    return m_enabled.load();
}

bool ConversationTransport::Init(std::string const& url, uint64_t /*nowMs*/, bool debug)
{
    std::lock_guard<std::mutex> lk(m_lock);
    if (m_enabled.load() || m_worker)
        return m_enabled.load();
    // http://host:port only: no path, no auth, no https. The service is a
    // localhost or lab-bridge process; anything else is a misconfiguration
    // and fails closed to "no conversation".
    if (url.empty())
        return false; // default: disabled, no thread, no I/O, no log line
    std::string const prefix = "http://";
    if (url.rfind(prefix, 0) != 0 || url.size() <= prefix.size())
    {
        sLog.outError("[Conversation] bad service url (want http://host:port); transport disabled");
        return false;
    }
    std::string const hostPort = url.substr(prefix.size());
    size_t colon = hostPort.find(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= hostPort.size())
    {
        sLog.outError("[Conversation] bad service url (want http://host:port); transport disabled");
        return false;
    }
    std::string const host = hostPort.substr(0, colon);
    for (char c : host)
    {
        if (!(c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' ||
              c >= '0' && c <= '9' || c == '.' || c == '-'))
        {
            sLog.outError("[Conversation] bad service url host; transport disabled");
            return false;
        }
    }
    uint32_t port = 0;
    for (size_t i = colon + 1; i < hostPort.size(); ++i)
    {
        char c = hostPort[i];
        if (c < '0' || c > '9')
        {
            sLog.outError("[Conversation] bad service url port; transport disabled");
            return false;
        }
        port = port * 10 + (uint32_t)(c - '0');
    }
    if (port < 1 || port > 65535)
    {
        sLog.outError("[Conversation] bad service url port; transport disabled");
        return false;
    }
    m_host = host;
    m_port = (uint16_t)port;
    m_debug = debug;
    m_stopping = false; // a re-Init after Shutdown must start a live worker
    m_enabled.store(true);
    m_worker = new std::thread([this]() { WorkerLoop(); });
    sLog.outString("[Conversation] transport started url:%s", url.c_str());
    return true;
}

void ConversationTransport::Shutdown()
{
    std::thread* worker = nullptr;
    {
        std::lock_guard<std::mutex> lk(m_lock);
        if (!m_worker)
            return;
        m_stopping = true;
        m_enabled.store(false);
        m_cv.notify_all();
        worker = m_worker;
        m_worker = nullptr;
    }
    // Bounded: the in-flight round has a hard deadline, so the join cannot
    // outlive it.
    worker->join();
    delete worker;
    {
        // A re-Init starts from a clean table: a round left InFlight by the
        // dead lifetime must not refuse submits as busy afterwards.
        std::lock_guard<std::mutex> lk(m_lock);
        for (auto& slot : m_slots)
            slot = Slot{};
    }
    sLog.outString("[Conversation] transport stopped");
}

ConversationTransport::Slot* ConversationTransport::FindSlot(Slot* slots, uint32_t botLow)
{
    for (uint32_t i = 0; i < kMaxBots; ++i)
        if (slots[i].inUse && slots[i].round.botLow == botLow)
            return &slots[i];
    return nullptr;
}

bool ConversationTransport::Submit(uint32_t botLow, uint32_t partyGroup,
                                   uint32_t partyLeader, uint32_t profileCode,
                                   std::string const& text, uint64_t nowMs,
                                   ConvKind kind)
{
    std::lock_guard<std::mutex> lk(m_lock);
    if (!m_enabled.load())
        return false;
    Slot* s = FindSlot(m_slots, botLow);
    if (!s)
    {
        for (auto& slot : m_slots)
        {
            if (!slot.inUse)
            {
                slot.inUse = true;
                slot.round.botLow = botLow;
                s = &slot;
                break;
            }
        }
    }
    if (!s)
    {
        // A full table is a caller bug at this scale: skip the round fail
        // closed.
        sLog.outError("[Conversation] session table full bot:%u; message dropped", botLow);
        return false;
    }
    ConvRound::SubmitResult const r =
        s->round.Submit(botLow, partyGroup, partyLeader, profileCode, text, nowMs, m_nextStamp++, kind);
    if (r == ConvRound::SubmitResult::Rejected)
    {
        sLog.outError("[Conversation] submit rejected bot:%u len:%u", botLow, (uint32)text.size());
        return false;
    }
    if (r == ConvRound::SubmitResult::Busy)
    {
        if (m_debug)
            sLog.outString("[Conversation] submit busy bot:%u", botLow);
        return false;
    }
    ++m_submits;
    s->attempt = 1; // PORT-031: fresh round: first service
    if (m_debug)
        sLog.outString("[Conversation] submit bot:%u group:%u leader:%u profile:%u len:%u",
                       botLow, partyGroup, partyLeader, profileCode, (uint32)text.size());
    m_cv.notify_all();
    return true;
}

bool ConversationTransport::Poll(uint32_t botLow, uint32_t partyGroup,
                                 uint32_t partyLeader, uint64_t nowMs,
                                 std::string& outReply)
{
    std::lock_guard<std::mutex> lk(m_lock);
    Slot* s = FindSlot(m_slots, botLow);
    if (!s)
        return false;
    if (s->round.Poll(partyGroup, partyLeader, nowMs, outReply))
    {
        ++m_replies;
        return true;
    }
    return false;
}

void ConversationTransport::Invalidate(uint32_t botLow)
{
    std::lock_guard<std::mutex> lk(m_lock);
    Slot* s = FindSlot(m_slots, botLow);
    if (!s)
        return;
    s->round.Invalidate();
    if (m_debug)
        sLog.outString("[Conversation] invalidate bot:%u", botLow);
    m_cv.notify_all();
}

uint32_t ConversationTransport::CountSubmits() const
{
    std::lock_guard<std::mutex> lk(m_lock);
    return m_submits;
}
uint32_t ConversationTransport::CountReplies() const
{
    std::lock_guard<std::mutex> lk(m_lock);
    return m_replies;
}
uint32_t ConversationTransport::CountFails() const
{
    std::lock_guard<std::mutex> lk(m_lock);
    return m_fails;
}

void ConversationTransport::WorkerLoop()
{
    for (;;)
    {
        uint32_t botLow = 0;
        uint64_t stamp = 0;
        std::string text;
        std::string path;
        {
            std::unique_lock<std::mutex> lk(m_lock);
            if (m_stopping)
                return;
            // The oldest waiting round: InFlight means a message is stored
            // and the worker has not serviced it yet (the worker is the only
            // sender).
            uint64_t oldestParty = 0;
            uint64_t oldestWorld = 0;
            uint32_t worldBotLow = 0;
            uint64_t worldStamp = 0;
            for (auto const& s : m_slots)
            {
                if (!s.inUse || s.round.state != ConvState::InFlight)
                    continue;
                if (s.round.kind == ConvKind::Party &&
                    (!oldestParty || s.round.submitStamp < oldestParty))
                {
                    oldestParty = s.round.submitStamp;
                    botLow = s.round.botLow;
                    stamp = s.round.submitStamp;
                }
                else if (s.round.kind == ConvKind::WorldIntent &&
                         (!oldestWorld || s.round.submitStamp < oldestWorld))
                {
                    oldestWorld = s.round.submitStamp;
                    worldBotLow = s.round.botLow;
                    worldStamp = s.round.submitStamp;
                }
            }
            if (!botLow)
            {
                botLow = worldBotLow;
                stamp = worldStamp;
            }
            if (!botLow)
            {
                m_cv.wait_for(lk, std::chrono::milliseconds(100));
                continue;
            }
            Slot* s = FindSlot(m_slots, botLow);
            if (!s)
                continue;
            text = s->round.text;
            // The profile name is a safe enum string (reckless|cautious|none):
            // it is the only request content placed in the URL path.
            char const* profileName =
                Personality::ProfileName((Personality::Profile)s->round.profileCode);
            path = std::string(s->round.kind == ConvKind::Party ?
                               "/converse?profile=" : "/world-intent?profile=") + profileName;
        }
        // Bounded I/O off the world thread; the deadline is hard.
        std::string reply;
        bool const ok = SendRound(path, text, reply);
        std::lock_guard<std::mutex> lk(m_lock);
        Slot* s = FindSlot(m_slots, botLow);
        if (!s)
            continue;
        ConvRound::Result const r =
            s->round.OnResult(stamp, ok, reply, WorldTimer::getMSTime());
        if (r == ConvRound::Result::Discarded)
        {
            // A newer round or an invalidation owns the session now; the
            // result belongs to a dead round: keep the loop, drop it.
        }
        if (r == ConvRound::Result::Failed)
        {
            if (s->round.kind == ConvKind::Party && s->attempt < 2)
            {
                // PORT-031 (KAP-558): one bounded retry. A single lost
                // race (service busy, model timeout) must not silence an
                // addressed player message; the second failure stays
                // silent by design (no reply is never fabricated). The
                // party signature is re-validated on consumption (Poll).
                ++s->attempt;
                s->round.state = ConvState::InFlight;
                s->round.submitStamp = m_nextStamp++;
                s->round.submitClockMs = WorldTimer::getMSTime();
                if (m_debug)
                    sLog.outString("[Conversation] retry bot:%u attempt:2", botLow);
                m_cv.notify_all();
            }
            else
            {
                ++m_fails;
                if (m_debug)
                    sLog.outString("[Conversation] fail bot:%u (no reply)", botLow);
            }
        }
    }
}

bool ConversationTransport::SendRound(std::string const& path,
                                      std::string const& body,
                                      std::string& outReply)
{
    outReply.clear();
    using clock = std::chrono::steady_clock;
    clock::time_point const deadline =
        clock::now() + std::chrono::milliseconds(kRoundTimeoutMs);
    auto remainingMs = [&deadline]() -> int
    {
        int64_t const r = std::chrono::duration_cast<std::chrono::milliseconds>(
                              deadline - clock::now())
                              .count();
        return r < 0 ? 0 : (int)r;
    };
    // The only DNS this feature performs, and it runs here, never on the
    // world thread.
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    std::string const portStr = std::to_string(m_port);
    if (getaddrinfo(m_host.c_str(), portStr.c_str(), &hints, &res) != 0 || !res)
        return false;
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        freeaddrinfo(res);
        return false;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    int rc = ::connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc != 0 && errno != EINPROGRESS)
    {
        ::close(fd);
        return false;
    }
    if (rc != 0)
    {
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLOUT;
        pfd.revents = 0;
        rc = ::poll(&pfd, 1, remainingMs());
        if (rc <= 0)
        {
            ::close(fd);
            return false;
        }
        int soErr = 0;
        socklen_t elen = sizeof(soErr);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soErr, &elen) != 0 || soErr != 0)
        {
            ::close(fd);
            return false;
        }
    }
    char header[256];
    int const hlen = snprintf(header, sizeof(header),
                              "POST %s HTTP/1.1\r\nHost: %s:%u\r\n"
                              "Content-Type: text/plain; charset=utf-8\r\n"
                              "Content-Length: %u\r\nConnection: close\r\n\r\n",
                              path.c_str(), m_host.c_str(), m_port,
                              (uint32_t)body.size());
    if (hlen <= 0 || (size_t)hlen >= sizeof(header))
    {
        ::close(fd);
        return false;
    }
    uint8_t const* chunks[2] = {(uint8_t const*)header, (uint8_t const*)body.data()};
    uint32_t lens[2] = {(uint32_t)hlen, (uint32_t)body.size()};
    for (int c = 0; c < 2; ++c)
    {
        if (lens[c] == 0)
            continue;
        uint32_t off = 0;
        while (off < lens[c])
        {
            if (remainingMs() <= 0)
            {
                ::close(fd);
                return false;
            }
            ssize_t n = ::send(fd, chunks[c] + off, lens[c] - off, 0);
            if (n < 0)
            {
                if (errno == EINTR)
                    continue;
                ::close(fd);
                return false;
            }
            off += (uint32_t)n;
        }
    }
    uint8_t buf[kBufBytes];
    uint32_t total = 0;
    uint32_t bodyStart = 0;
    uint64_t bodyWanted = 0; // content length, or 0 = read to close
    bool headersDone = false;
    for (;;)
    {
        if (remainingMs() <= 0)
        {
            ::close(fd);
            return false;
        }
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int pr = ::poll(&pfd, 1, remainingMs());
        if (pr < 0)
        {
            if (errno == EINTR)
                continue;
            ::close(fd);
            return false;
        }
        if (pr == 0)
        {
            ::close(fd);
            return false;
        }
        ssize_t n = ::recv(fd, buf + total, (socklen_t)(sizeof(buf) - total), 0);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            ::close(fd);
            return false;
        }
        if (n == 0)
            break; // connection closed: what arrived is the whole response
        total += (uint32_t)n;
        if (total >= sizeof(buf))
        {
            // Cannot hold more: fail closed without draining.
            ::close(fd);
            return false;
        }
        if (!headersDone)
        {
            bool found = false;
            for (uint32_t i = 0; i + 3 < total; ++i)
            {
                if (buf[i] == '\r' && buf[i + 1] == '\n' &&
                    buf[i + 2] == '\r' && buf[i + 3] == '\n')
                {
                    bodyStart = i + 4;
                    found = true;
                    break;
                }
            }
            if (!found)
                continue;
            headersDone = true;
            // Status check: a non-2xx answer carries no reply.
            if (bodyStart >= 12 && memcmp(buf, "HTTP/", 5) == 0 &&
                buf[9] >= '0' && buf[9] <= '9' && buf[10] >= '0' && buf[10] <= '9' &&
                buf[11] >= '0' && buf[11] <= '9')
            {
                uint32_t const status = (uint32_t)(buf[9] - '0') * 100 +
                                        (uint32_t)(buf[10] - '0') * 10 +
                                        (uint32_t)(buf[11] - '0');
                if (status < 200 || status >= 300)
                {
                    ::close(fd);
                    return false;
                }
            }
            std::string const head((const char*)buf, bodyStart);
            size_t cl = head.find("Content-Length:");
            if (cl == std::string::npos)
                cl = head.find("content-length:");
            if (cl != std::string::npos)
            {
                size_t v = cl + strlen("Content-Length:");
                while (v < head.size() && head[v] == ' ')
                    ++v;
                uint64_t len = 0;
                bool any = false;
                for (; v < head.size() && head[v] >= '0' && head[v] <= '9'; ++v)
                {
                    len = len * 10 + (uint64_t)(head[v] - '0');
                    any = true;
                }
                if (!any)
                    continue; // malformed length: read to close, capped
                if (len > kMaxReplyBytes)
                {
                    ::close(fd); // oversized reply: fail closed, do not drain
                    return false;
                }
                bodyWanted = (uint32_t)len;
            }
        }
        if (headersDone && bodyWanted && total - bodyStart >= bodyWanted)
            break;
    }
    ::close(fd);
    if (!headersDone || total <= bodyStart)
        return false; // no parsable response body
    uint32_t bodyLen = total - bodyStart;
    if (bodyWanted && bodyLen > bodyWanted)
        bodyLen = bodyWanted;
    if (bodyLen > kMaxReplyBytes)
        return false;
    outReply.assign((const char*)buf + bodyStart, bodyLen);
    return true;
}

} // namespace Conversation
} // namespace Companion
