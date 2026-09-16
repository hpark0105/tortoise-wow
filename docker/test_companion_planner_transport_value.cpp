// PORT-018 (KAP-558): value-level coverage for the bounded planner
// transport round state machine (Companion/PlannerTransport.h).
// Standalone: no engine, no sockets, no threads, no Docker. Compiled and
// run by docker/test_companion_planner_transport_value.py.
//
// The same Round transitions run under the transport lock in the engine
// and here: submit/supersede/reject/cooldown, stamp-mismatch discards,
// invalidation resets, timeout counting, cooldown expiry and bounded
// response storage.

#include "Companion/PlannerTransport.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace CP = Companion::Planner;

static int g_failures = 0;
#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            std::printf("FAIL %d: %s\n", __LINE__, #cond);         \
            ++g_failures;                                          \
        }                                                          \
    } while (0)

int main()
{
    // Budgets and layout pins.
    CHECK(CP::kMaxSessions == 8);
    CHECK(CP::kRoundTimeoutMs == 5000);
    CHECK(CP::kCooldownMs == 60000);
    CHECK(CP::kMaxConsecutiveTimeouts == 3);
    CHECK(CP::kPlannerPaceMs == 2000);
    CHECK(CP::kRequestBytes == 84);
    CHECK(CP::kMaxPayloadBytes == 4096);

    uint8_t req[CP::kRequestBytes];
    for (size_t i = 0; i < sizeof(req); ++i)
        req[i] = (uint8_t)i;
    uint8_t resp[64] = {};
    for (size_t i = 0; i < sizeof(resp); ++i)
        resp[i] = (uint8_t)(0xA0 + i);

    // Fresh submit: accepted and the request is stored byte-exact.
    CP::Round r;
    CHECK(r.state == CP::RoundState::Idle);
    CHECK(r.Submit(req, CP::kRequestBytes, 1000, 1) == CP::Round::SubmitResult::Accepted);
    CHECK(r.state == CP::RoundState::InFlight);
    CHECK(r.submitStamp == 1);
    CHECK(r.requestClockMs == 1000);
    CHECK(r.requestLen == CP::kRequestBytes);
    CHECK(std::memcmp(r.requestBytes, req, CP::kRequestBytes) == 0);

    // Bad length or null: rejected fail-closed, state untouched.
    CP::Round r2;
    CHECK(r2.Submit(req, CP::kRequestBytes - 1, 1000, 1) == CP::Round::SubmitResult::Rejected);
    CHECK(r2.Submit(nullptr, CP::kRequestBytes, 1000, 1) == CP::Round::SubmitResult::Rejected);
    CHECK(r2.state == CP::RoundState::Idle);
    CHECK(r2.submitStamp == 0);

    // Queue depth 1: a submit while in flight supersedes with a new stamp,
    // and the worker result of the old round is discarded by stamp.
    CHECK(r.Submit(req, CP::kRequestBytes, 3000, 2) == CP::Round::SubmitResult::Superseded);
    CHECK(r.submitStamp == 2);
    CHECK(r.OnResult(1, CP::RoundResult::Ok, resp, sizeof(resp), 4000)
          == CP::Round::ActionResult::Discarded);
    CHECK(!r.responseReady);
    CHECK(r.state == CP::RoundState::InFlight); // the live round is intact

    // Invalidation: fresh generation, idle, dead stamp; the worker's
    // outstanding result for the old stamp is discarded, and the next
    // round starts clean (no carried timeout count or cooldown).
    CP::Round r3;
    CHECK(r3.Submit(req, CP::kRequestBytes, 100, 10) == CP::Round::SubmitResult::Accepted);
    uint32_t const gen0 = r3.partySessionGeneration;
    r3.Invalidate();
    CHECK(r3.partySessionGeneration == gen0 + 1);
    CHECK(r3.state == CP::RoundState::Idle);
    CHECK(r3.submitStamp == 0);
    CHECK(!r3.responseReady);
    CHECK(r3.responseLen == 0);
    CHECK(r3.cooldownUntilMs == 0);
    CHECK(r3.OnResult(10, CP::RoundResult::Ok, resp, sizeof(resp), 200)
          == CP::Round::ActionResult::Discarded);
    CHECK(r3.Submit(req, CP::kRequestBytes, 300, 11) == CP::Round::SubmitResult::Accepted);
    CHECK(r3.timeoutCount == 0);

    // Consecutive round failures cool the session down after exactly
    // kMaxConsecutiveTimeouts; submits inside the window are refused.
    CP::Round r4;
    uint64_t t = 1000;
    for (int i = 1; i <= (int)CP::kMaxConsecutiveTimeouts; ++i)
    {
        CHECK(r4.Submit(req, CP::kRequestBytes, t, 100 + (uint64_t)i)
              == CP::Round::SubmitResult::Accepted);
        t += 100;
        CP::Round::ActionResult const a =
            r4.OnResult(100 + (uint64_t)i, CP::RoundResult::Timeout, nullptr, 0, t);
        if (i < (int)CP::kMaxConsecutiveTimeouts)
            CHECK(a == CP::Round::ActionResult::Failed);
        else
            CHECK(a == CP::Round::ActionResult::Cooldown);
    }
    CHECK(r4.state == CP::RoundState::Cooldown);
    CHECK(r4.InCooldown(t));
    t += 1000;
    CHECK(r4.Submit(req, CP::kRequestBytes, t, 200) == CP::Round::SubmitResult::Cooldown);
    // Cooldown expiry (inclusive) reopens the session.
    t = r4.cooldownUntilMs;
    CHECK(r4.Submit(req, CP::kRequestBytes, t, 201) == CP::Round::SubmitResult::Accepted);

    // A success resets the failure count and stores the response.
    CP::Round r5;
    t = 1000;
    CHECK(r5.Submit(req, CP::kRequestBytes, t, 300) == CP::Round::SubmitResult::Accepted);
    CHECK(r5.OnResult(300, CP::RoundResult::Timeout, nullptr, 0, t + 100)
          == CP::Round::ActionResult::Failed);
    CHECK(r5.timeoutCount == 1);
    t += 100;
    CHECK(r5.Submit(req, CP::kRequestBytes, t, 301) == CP::Round::SubmitResult::Accepted);
    CHECK(r5.OnResult(301, CP::RoundResult::Ok, resp, sizeof(resp), t + 100)
          == CP::Round::ActionResult::Completed);
    CHECK(r5.responseReady);
    CHECK(r5.responseLen == sizeof(resp));
    CHECK(std::memcmp(r5.responseBytes, resp, sizeof(resp)) == 0);
    CHECK(r5.timeoutCount == 0);

    // Oversized responses are never stored (bounded), the round completes.
    CP::Round r6;
    t = 1000;
    CHECK(r6.Submit(req, CP::kRequestBytes, t, 400) == CP::Round::SubmitResult::Accepted);
    static uint8_t big[CP::kMaxPayloadBytes + 1];
    CHECK(r6.OnResult(400, CP::RoundResult::Ok, big, sizeof(big), t + 100)
          == CP::Round::ActionResult::Completed);
    CHECK(!r6.responseReady);
    CHECK(r6.responseLen == 0);

    if (g_failures)
    {
        std::printf("value tests: %d FAILURES\n", g_failures);
        return 1;
    }
    std::printf("value tests: ALL OK\n");
    return 0;
}
