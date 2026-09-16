// PORT-022 (KAP-558): value-level coverage for the bounded conversation
// round state machine (Companion/ConversationTransport.h).
// Standalone: no engine, no sockets, no threads, no Docker. Compiled and
// run by docker/test_companion_converse_value.py.
//
// The same ConvRound transitions run under the transport lock in the
// engine and here: submit/busy/reject, worker deliver ok/fail,
// stamp-mismatch discards, party-signature drops, staleness drops,
// one-shot consumption and invalidation resets.

#include "Companion/ConversationTransport.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace CC = Companion::Conversation;

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
    CHECK(CC::kMaxBots == 8);
    CHECK(CC::kRoundTimeoutMs == 4000);
    CHECK(CC::kReplyAgeMs == 10000);
    CHECK(CC::kMaxTextBytes == 200);
    CHECK(CC::kMaxReplyBytes == 120);

    CC::ConvRound r;
    CHECK(r.state == CC::ConvState::Idle);

    // Fresh submit: accepted and the request is stored byte-exact.
    CHECK(r.Submit(610002, 42, 610001, 1, "Companion hello", 1000, 1)
          == CC::ConvRound::SubmitResult::Accepted);
    CHECK(r.state == CC::ConvState::InFlight);
    CHECK(r.botLow == 610002);
    CHECK(r.partyGroup == 42);
    CHECK(r.partyLeader == 610001);
    CHECK(r.profileCode == 1);
    CHECK(r.text == "Companion hello");
    CHECK(r.submitStamp == 1);
    CHECK(r.submitClockMs == 1000);
    CHECK(r.reply.empty());

    // Empty or oversize text: rejected fail-closed, state untouched.
    CC::ConvRound r2;
    CHECK(r2.Submit(610002, 42, 610001, 1, "", 1000, 1)
          == CC::ConvRound::SubmitResult::Rejected);
    std::string big(201, 'a');
    CHECK(r2.Submit(610002, 42, 610001, 1, big, 1000, 1)
          == CC::ConvRound::SubmitResult::Rejected);
    std::string maxText(200, 'a');
    CHECK(r2.Submit(610002, 42, 610001, 1, maxText, 1000, 1)
          == CC::ConvRound::SubmitResult::Accepted);
    CHECK(r2.text.size() == 200);

    // One pending round per companion: a submit while in service is Busy,
    // the live round is intact (no supersede; the worker is the only sender).
    CHECK(r.Submit(610002, 42, 610001, 1, "again", 3000, 2)
          == CC::ConvRound::SubmitResult::Busy);
    CHECK(r.submitStamp == 1);
    CHECK(r.text == "Companion hello");

    // Stamp mismatch: the result belongs to a dead round and is discarded.
    CHECK(r.OnResult(2, true, "reply", 4000) == CC::ConvRound::Result::Discarded);
    CHECK(r.state == CC::ConvState::InFlight);
    CHECK(!r.reply.empty() == false);

    // A matching ok result with a valid reply is Applied and Ready.
    CHECK(r.OnResult(1, true, "Boldly, let us go.", 4000)
          == CC::ConvRound::Result::Applied);
    CHECK(r.state == CC::ConvState::Ready);
    CHECK(r.reply == "Boldly, let us go.");
    CHECK(r.replyClockMs == 4000);

    // Poll: matching party signature and a fresh reply is consumed once.
    std::string out;
    CHECK(r.Poll(42, 610001, 4500, out) == true);
    CHECK(out == "Boldly, let us go.");
    CHECK(r.state == CC::ConvState::Idle);
    CHECK(r.reply.empty());
    // Consumed: a second poll finds nothing.
    CHECK(r.Poll(42, 610001, 4600, out) == false);

    // Failure outcomes: no reply, back to Idle.
    CC::ConvRound r3;
    CHECK(r3.Submit(610002, 42, 610001, 1, "hi", 1000, 10)
          == CC::ConvRound::SubmitResult::Accepted);
    CHECK(r3.OnResult(10, false, "ignored", 1100) == CC::ConvRound::Result::Failed);
    CHECK(r3.state == CC::ConvState::Idle);
    CHECK(r3.reply.empty());
    CC::ConvRound r4;
    CHECK(r4.Submit(610002, 42, 610001, 1, "hi", 1000, 11)
          == CC::ConvRound::SubmitResult::Accepted);
    CHECK(r4.OnResult(11, true, "", 1100) == CC::ConvRound::Result::Failed);
    CHECK(r4.state == CC::ConvState::Idle);
    std::string bigReply(121, 'x');
    CC::ConvRound r5;
    CHECK(r5.Submit(610002, 42, 610001, 1, "hi", 1000, 12)
          == CC::ConvRound::SubmitResult::Accepted);
    CHECK(r5.OnResult(12, true, bigReply, 1100) == CC::ConvRound::Result::Failed);
    CHECK(r5.state == CC::ConvState::Idle);
    CHECK(r5.reply.empty());

    // Party-signature drop: a ready reply is dead once the group or leader
    // changes (leave/kick/disband/leader change).
    CC::ConvRound r6;
    CHECK(r6.Submit(610002, 42, 610001, 1, "hi", 1000, 20)
          == CC::ConvRound::SubmitResult::Accepted);
    CHECK(r6.OnResult(20, true, "reply", 2000) == CC::ConvRound::Result::Applied);
    std::string o6;
    CHECK(r6.Poll(43, 610001, 2100, o6) == false);  // group changed
    CHECK(r6.state == CC::ConvState::Idle);
    CHECK(r6.reply.empty());

    CC::ConvRound r7;
    CHECK(r7.Submit(610002, 42, 610001, 1, "hi", 1000, 30)
          == CC::ConvRound::SubmitResult::Accepted);
    CHECK(r7.OnResult(30, true, "reply", 2000) == CC::ConvRound::Result::Applied);
    std::string o7;
    CHECK(r7.Poll(42, 610999, 2100, o7) == false);  // leader changed
    CHECK(r7.state == CC::ConvState::Idle);

    // Staleness: a ready reply older than the budget is dropped.
    CC::ConvRound r8;
    CHECK(r8.Submit(610002, 42, 610001, 1, "hi", 1000, 40)
          == CC::ConvRound::SubmitResult::Accepted);
    CHECK(r8.OnResult(40, true, "reply", 5000) == CC::ConvRound::Result::Applied);
    std::string o8;
    CHECK(r8.Poll(42, 610001, 5000 + CC::kReplyAgeMs + 1, o8) == false);
    CHECK(r8.state == CC::ConvState::Idle);

    // Invalidation: clears the round; a later submit starts clean.
    CC::ConvRound r9;
    CHECK(r9.Submit(610002, 42, 610001, 1, "hi", 1000, 50)
          == CC::ConvRound::SubmitResult::Accepted);
    r9.Invalidate();
    CHECK(r9.state == CC::ConvState::Idle);
    CHECK(r9.submitStamp == 0);
    CHECK(r9.text.empty());
    CHECK(r9.reply.empty());
    CHECK(r9.Submit(610002, 42, 610001, 1, "again", 2000, 51)
          == CC::ConvRound::SubmitResult::Accepted);
    CHECK(r9.submitStamp == 51);

    if (g_failures)
    {
        std::printf("value tests: %d FAILURES\n", g_failures);
        return 1;
    }
    std::printf("value tests: ALL OK\n");
    return 0;
}