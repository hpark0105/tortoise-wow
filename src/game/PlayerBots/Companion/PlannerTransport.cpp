// PORT-018 (KAP-558): the bounded nonblocking party-planner transport.
// POSIX implementation: one worker thread, one in-flight round at a time,
// a hard per-round deadline and fail-closed outcomes. The world thread only
// ever calls SubmitShared/InvalidateSession/FetchOffer (see
// Companion/PlannerTransport.h); every socket, poll and DNS call in this
// file runs on the worker.

#include "Companion/PlannerTransport.h"

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
constexpr uint32_t kBufBytes = 16 * 1024; // header (<= 8K) + body (<= 4K) + slack
} // namespace

namespace Companion
{
namespace Planner
{

PlannerTransport::~PlannerTransport()
{
    Shutdown();
}

bool PlannerTransport::Enabled() const
{
    return m_enabled.load();
}

bool PlannerTransport::Init(std::string const& url, uint64_t /*nowMs*/, bool debug)
{
    std::lock_guard<std::mutex> lk(m_lock);
    if (m_enabled.load() || m_worker)
        return m_enabled.load();
    // http://host:port only: no path, no auth, no https. The service is a
    // localhost or lab-bridge process; anything else is a misconfiguration
    // and fails closed to the deterministic policies.
    if (url.empty())
        return false; // default: disabled, no thread, no I/O, no log line
    std::string const prefix = "http://";
    if (url.rfind(prefix, 0) != 0 || url.size() <= prefix.size())
    {
        sLog.outError("[Planner] bad service url (want http://host:port); transport disabled");
        return false;
    }
    std::string const hostPort = url.substr(prefix.size());
    size_t colon = hostPort.find(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= hostPort.size())
    {
        sLog.outError("[Planner] bad service url (want http://host:port); transport disabled");
        return false;
    }
    std::string const host = hostPort.substr(0, colon);
    for (char c : host)
    {
        if (!(c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' ||
              c >= '0' && c <= '9' || c == '.' || c == '-'))
        {
            sLog.outError("[Planner] bad service url host; transport disabled");
            return false;
        }
    }
    uint32_t port = 0;
    for (size_t i = colon + 1; i < hostPort.size(); ++i)
    {
        char c = hostPort[i];
        if (c < '0' || c > '9')
        {
            sLog.outError("[Planner] bad service url port; transport disabled");
            return false;
        }
        port = port * 10 + (uint32_t)(c - '0');
    }
    if (port < 1 || port > 65535)
    {
        sLog.outError("[Planner] bad service url port; transport disabled");
        return false;
    }
    m_host = host;
    m_port = (uint16_t)port;
    m_debug = debug;
    m_stopping = false; // a re-Init after Shutdown must start a live worker
    m_enabled.store(true);
    m_worker = new std::thread([this]() { WorkerLoop(); });
    sLog.outString("[Planner] transport started url:%s", url.c_str());
    return true;
}

void PlannerTransport::Shutdown()
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
        // A re-Init starts from a clean table: a round left
        // InFlight by the dead lifetime must not refuse submits
        // as busy afterwards.
        std::lock_guard<std::mutex> lk(m_lock);
        for (auto& slot : m_slots)
            slot = Slot{};
    }
    sLog.outString("[Planner] transport stopped");
}

PlannerTransport::Slot* PlannerTransport::FindSlot(Slot* slots, uint32_t ownerLow)
{
    for (uint32_t i = 0; i < kMaxSessions; ++i)
        if (slots[i].inUse && slots[i].ownerLow == ownerLow)
            return &slots[i];
    return nullptr;
}

bool PlannerTransport::SubmitShared(uint32_t ownerLow, uint8_t const* req,
                                    uint32_t reqLen, uint64_t nowMs)
{
    std::lock_guard<std::mutex> lk(m_lock);
    if (!m_enabled.load())
        return false;
    Slot* s = FindSlot(m_slots, ownerLow);
    if (!s)
    {
        for (auto& slot : m_slots)
        {
            if (!slot.inUse)
            {
                slot.inUse = true;
                slot.ownerLow = ownerLow;
                s = &slot;
                break;
            }
        }
    }
    if (!s)
    {
        // A full table is a caller bug at this scale: skip the round fail
        // closed.
        sLog.outError("[Planner] session table full owner:%u; round skipped", ownerLow);
        return false;
    }
    if (s->round.state == RoundState::InFlight)
    {
        // One in-service round per session: the worker is the only
        // sender, so a newer observation never supersedes a live
        // round (a slow service would then starve the failure
        // count and the backoff). The next pace tick resubmits
        // with fresh state; cooldowns block the submit entirely.
        if (m_debug)
            sLog.outString("[Planner] submit busy owner:%u gen:%u",
                           ownerLow, s->round.partySessionGeneration);
        return false;
    }
    Round::SubmitResult const r = s->round.Submit(req, reqLen, nowMs, m_nextStamp++);
    if (r == Round::SubmitResult::Rejected || r == Round::SubmitResult::Cooldown)
        return false;
    ++m_submits;
    if (m_debug)
        sLog.outString("[Planner] submit owner:%u gen:%u superseded:%u",
                       ownerLow, s->round.partySessionGeneration,
                       (uint32)(r == Round::SubmitResult::Superseded));
    m_cv.notify_all();
    return true;
}

void PlannerTransport::InvalidateSession(uint32_t ownerLow)
{
    std::lock_guard<std::mutex> lk(m_lock);
    Slot* s = FindSlot(m_slots, ownerLow);
    if (!s)
        return;
    s->round.Invalidate();
    if (m_debug)
        sLog.outString("[Planner] invalidate owner:%u gen:%u",
                       ownerLow, s->round.partySessionGeneration);
    m_cv.notify_all();
}

uint32_t PlannerTransport::SessionGeneration(uint32_t ownerLow) const
{
    std::lock_guard<std::mutex> lk(m_lock);
    for (auto const& s : m_slots)
        if (s.inUse && s.ownerLow == ownerLow)
            return s.round.partySessionGeneration;
    return 0;
}

bool PlannerTransport::FetchOffer(uint32_t ownerLow, uint32_t botLow,
                                  uint64_t nowMs, Step& out)
{
    std::lock_guard<std::mutex> lk(m_lock);
    if (!m_enabled.load())
        return false;
    Slot* s = FindSlot(m_slots, ownerLow);
    if (!s || !s->round.responseReady)
        return false;
    uint32_t const respLen = s->round.responseLen;
    if (respLen < kEnvelopeBytes)
    {
        // Cannot even hold an envelope: dead for every bot; consume.
        s->round.responseReady = false;
        s->round.responseLen = 0;
        ++m_rejects;
        if (m_debug)
            sLog.outString("[Planner] reject owner:%u bot:%u reason:size-mismatch len:%u",
                           ownerLow, botLow, respLen);
        return false;
    }
    uint8_t resp[kMaxPayloadBytes];
    for (uint32_t i = 0; i < respLen; ++i)
        resp[i] = s->round.responseBytes[i];
    Envelope const reqEnv = DecodeEnvelope(s->round.requestBytes);
    Envelope const resEnv = DecodeEnvelope(resp);
    uint32_t const expected = (uint32_t)(kEnvelopeBytes + kStepBytes * (uint64_t)resEnv.stepCount);
    Step steps[kMaxSteps] = {};
    bool const decoded = (expected == respLen && resEnv.stepCount > 0 &&
                          resEnv.stepCount <= kMaxSteps);
    if (decoded)
        for (uint32_t i = 0; i < resEnv.stepCount; ++i)
            steps[i] = DecodeStep(resp + kEnvelopeBytes + kStepBytes * i);
    Verdict const v = ValidateResponse(reqEnv, resEnv, decoded ? steps : nullptr,
                                       respLen, nowMs);
    if (!v.legal())
    {
        // A rejected round is dead for every bot: consume.
        s->round.responseReady = false;
        s->round.responseLen = 0;
        ++m_rejects;
        if (m_debug)
            sLog.outString("[Planner] reject owner:%u bot:%u reason:%s",
                           ownerLow, botLow, RejectName(v.reject));
        return false;
    }
    // Remove only this bot's step: the party's other bots fetch
    // theirs from the same round; a step nobody claims is cleared by
    // the next submit or invalidation (and by the response age bound
    // on any later fetch).
    for (uint32_t i = 0; i < resEnv.stepCount; ++i)
    {
        if (steps[i].botGuid != botLow)
            continue;
        out = steps[i];
        uint32_t remaining = 0;
        for (uint32_t j = 0; j < resEnv.stepCount; ++j)
        {
            if (j == i)
                continue;
            for (uint32_t b = 0; b < kStepBytes; ++b)
                s->round.responseBytes[kEnvelopeBytes + kStepBytes * remaining + b] =
                    resp[kEnvelopeBytes + kStepBytes * j + b];
            ++remaining;
        }
        Envelope updated = resEnv;
        updated.stepCount = remaining;
        updated.totalSize = kEnvelopeBytes + kStepBytes * remaining;
        EncodeEnvelope(s->round.responseBytes, updated);
        if (remaining)
            s->round.responseLen = updated.totalSize;
        else
        {
            s->round.responseReady = false;
            s->round.responseLen = 0;
        }
        ++m_offers;
        if (m_debug)
            sLog.outString("[Planner] offer owner:%u bot:%u action:%u target:%u pref:%u ordergen:%u",
                           ownerLow, botLow, (uint32)steps[i].action,
                           steps[i].targetGuid, steps[i].preference,
                           steps[i].orderGeneration);
        return true;
    }
    // No step for this bot: leave the round in place for its
    // other members.
    return false;
}

uint32_t PlannerTransport::CountSubmits() const
{
    std::lock_guard<std::mutex> lk(m_lock);
    return m_submits;
}
uint32_t PlannerTransport::CountOffers() const
{
    std::lock_guard<std::mutex> lk(m_lock);
    return m_offers;
}
uint32_t PlannerTransport::CountRejects() const
{
    std::lock_guard<std::mutex> lk(m_lock);
    return m_rejects;
}
uint32_t PlannerTransport::CountTimeouts() const
{
    std::lock_guard<std::mutex> lk(m_lock);
    return m_timeouts;
}

void PlannerTransport::WorkerLoop()
{
    for (;;)
    {
        uint32_t ownerLow = 0;
        uint64_t stamp = 0;
        uint8_t payload[kRequestBytes] = {};
        {
            std::unique_lock<std::mutex> lk(m_lock);
            if (m_stopping)
                return;
            // The oldest waiting round: InFlight means a request is stored
            // and the worker has not serviced it yet (the worker is the
            // only sender).
            uint64_t oldest = 0;
            for (auto const& s : m_slots)
            {
                if (s.inUse && s.round.state == RoundState::InFlight &&
                    (!oldest || s.round.submitStamp < oldest))
                {
                    oldest = s.round.submitStamp;
                    ownerLow = s.ownerLow;
                    stamp = s.round.submitStamp;
                }
            }
            if (!ownerLow)
            {
                m_cv.wait_for(lk, std::chrono::milliseconds(100));
                continue;
            }
            Slot* s = FindSlot(m_slots, ownerLow);
            if (!s)
                continue;
            for (uint32_t i = 0; i < kRequestBytes; ++i)
                payload[i] = s->round.requestBytes[i];
        }
        // Bounded I/O off the world thread; the deadline is hard.
        uint8_t resp[kMaxPayloadBytes] = {};
        uint32_t respLen = 0;
        RoundResult const result = SendRound(payload, resp, respLen);
        std::lock_guard<std::mutex> lk(m_lock);
        Slot* s = FindSlot(m_slots, ownerLow);
        if (!s)
            continue;
        Round::ActionResult const a =
            s->round.OnResult(stamp, result, resp, respLen, WorldTimer::getMSTime());
        switch (a)
        {
            case Round::ActionResult::Discarded:
                // A newer round or an invalidation owns the session now;
                // the result belongs to a dead generation.
                break;
            case Round::ActionResult::Failed:
                ++m_timeouts;
                sLog.outString("[Planner] timeout owner:%u result:%d count:%u",
                               ownerLow, (int)result, s->round.timeoutCount);
                break;
            case Round::ActionResult::Cooldown:
                ++m_timeouts;
                sLog.outString("[Planner] cooldown owner:%u ms:%u (repeated round failures)",
                               ownerLow, (uint32)kCooldownMs);
                break;
            case Round::ActionResult::Completed:
                break;
            case Round::ActionResult::Applied:
                break;
        }
    }
}

RoundResult PlannerTransport::SendRound(
    uint8_t const* payload, uint8_t* out, uint32_t& outLen)
{
    outLen = 0;
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
        return RoundResult::ConnectFail;
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        freeaddrinfo(res);
        return RoundResult::ConnectFail;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    int rc = ::connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc != 0 && errno != EINPROGRESS)
    {
        ::close(fd);
        return RoundResult::ConnectFail;
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
            return RoundResult::Timeout;
        }
        int soErr = 0;
        socklen_t elen = sizeof(soErr);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soErr, &elen) != 0 || soErr != 0)
        {
            ::close(fd);
            return RoundResult::ConnectFail;
        }
    }
    char header[256];
    int const hlen = snprintf(header, sizeof(header),
                              "POST /plan HTTP/1.1\r\nHost: %s:%u\r\n"
                              "Content-Type: application/octet-stream\r\n"
                              "Content-Length: %u\r\nConnection: close\r\n\r\n",
                              m_host.c_str(), m_port, (uint32_t)kRequestBytes);
    if (hlen <= 0 || (size_t)hlen >= sizeof(header))
    {
        ::close(fd);
        return RoundResult::ConnectFail;
    }
    uint8_t const* chunks[2] = {(uint8_t const*)header, payload};
    uint32_t lens[2] = {(uint32_t)hlen, kRequestBytes};
    for (int c = 0; c < 2; ++c)
    {
        uint32_t off = 0;
        while (off < lens[c])
        {
            if (remainingMs() <= 0)
            {
                ::close(fd);
                return RoundResult::Timeout;
            }
            ssize_t n = ::send(fd, chunks[c] + off, lens[c] - off, 0);
            if (n < 0)
            {
                if (errno == EINTR)
                    continue;
                ::close(fd);
                return RoundResult::ConnectFail;
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
            return RoundResult::Timeout;
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
            return RoundResult::ConnectFail;
        }
        if (pr == 0)
        {
            ::close(fd);
            return RoundResult::Timeout;
        }
        ssize_t n = ::recv(fd, buf + total, (socklen_t)(sizeof(buf) - total), 0);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            ::close(fd);
            return RoundResult::ConnectFail;
        }
        if (n == 0)
            break; // connection closed: what arrived is the whole response
        total += (uint32_t)n;
        if (total >= sizeof(buf))
        {
            // Cannot hold more (header > 8K or body > 4K + slack): fail
            // closed without draining.
            ::close(fd);
            return RoundResult::Oversized;
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
            // Status check: a non-2xx answer carries no planner payload.
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
                    return RoundResult::HttpError;
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
                if (len > kMaxPayloadBytes)
                {
                    ::close(fd); // oversized: fail closed, do not drain
                    return RoundResult::Oversized;
                }
                bodyWanted = (uint32_t)len;
            }
        }
        if (headersDone && bodyWanted && total - bodyStart >= bodyWanted)
            break;
    }
    ::close(fd);
    if (!headersDone || total <= bodyStart)
        return RoundResult::HttpError; // no parsable response body
    uint32_t bodyLen = total - bodyStart;
    if (bodyWanted && bodyLen > bodyWanted)
        bodyLen = bodyWanted;
    if (bodyLen > kMaxPayloadBytes)
        return RoundResult::Oversized;
    for (uint32_t i = 0; i < bodyLen; ++i)
        out[i] = buf[bodyStart + i];
    outLen = bodyLen;
    return RoundResult::Ok;
}

} // namespace Planner
} // namespace Companion
