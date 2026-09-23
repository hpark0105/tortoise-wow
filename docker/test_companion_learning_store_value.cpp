// BL-003 (KAP-558): value-level coverage for the bounded async encounter
// summary persistence adapter (Companion/LearningStore.h).
//
// Standalone: no engine, no database, no Docker. Covers
//   * the pure FIFO contract: order, 64-item cap, drop accounting,
//     failure disabling, restart/reset, wraparound;
//   * the real Store (not a detached simulation): delivery identity
//     assignment by the store, FIFO delivery sequencing,
//     one-write-in-flight, completion and failure state, failure
//     disabling, submission failure, truncation fail-closed,
//     maintenance (stale repair / retention) ordering and retry,
//     shutdown, duplicate identity, and cross-thread completion;
//   * the product SQL contract text: profile gating, full 64-bit target
//     guid, idempotent identity, lifecycle statements.
// Compiled and run by docker/test_companion_learning_store_value.py.
#include "Companion/LearningStore.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace CL = Companion::Learning;

static_assert(sizeof(CL::SummaryFifo::Summary) <= 128,
              "summary must stay a small fixed value");
static_assert(sizeof(CL::SummaryFifo) < 16384,
              "FIFO must stay bounded (no heap)");
static_assert(CL::SummaryFifo::kMaxSummaries == 64,
              "FIFO capacity is part of the contract");

static int g_failures = 0;
#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            std::printf("FAIL %d: %s\n", __LINE__, #cond); \
            ++g_failures; \
        } \
    } while (0)

// ---------------------------------------------------------------------------
// Fake transport: stands in for CharacterDatabase.PExecuteCallback plus
// the SqlDelayThread. Records submitted SQL and delivers the callback on
// demand (simulating the DB worker thread). The callback is copied into
// the pending list exactly like SqlOperation::SetCallback copies it.
// ---------------------------------------------------------------------------
struct FakeDb
{
    struct Op
    {
        std::string sql;
        std::function<void(bool)> cb;
    };
    std::vector<Op> pending;
    int submitted = 0;
    int failedSubmits = 0; // scripted: this many next submits return false
    int fired = 0;

    bool Submit(char const* sql, std::function<void(bool)>* cb)
    {
        ++submitted;
        if (failedSubmits > 0)
        {
            --failedSubmits;
            return false; // per Database::Execute: callback never invoked
        }
        pending.push_back(Op{std::string(sql), *cb});
        return true;
    }
    void FireNext(bool success)
    {
        if (pending.empty())
            return;
        Op const op = pending.front();
        pending.erase(pending.begin());
        ++fired;
        op.cb(success);
    }
    void FireAll(bool success)
    {
        while (!pending.empty())
            FireNext(success);
    }
};

static void Wire(CL::Store& store, FakeDb& db)
{
    store.SetSubmitter([&db](char const* sql, std::function<void(bool)>* cb)
                       { return db.Submit(sql, cb); });
}

static CL::SummaryFifo::Summary MakeSummary(uint32_t charGuid)
{
    CL::SummaryFifo::Summary s;
    s.charGuid = charGuid;
    s.targetGuid = 0x0102030405060708ULL; // 72623859790382856
    s.durationMs = 1000;
    s.effectiveDamage = 50;
    s.complete = true;
    s.endReason = 1; // TargetDeath
    s.capturedAt = 1700000000;
    return s;
}

// ---------------------------------------------------------------------------
// Pure FIFO contract
// ---------------------------------------------------------------------------
static void TestFifoOrder()
{
    CL::SummaryFifo f;
    CHECK(f.Empty());
    CL::SummaryFifo::Summary out;
    CHECK(!f.Dequeue(out)); // dequeue from empty
    CHECK(f.EnqueueCount() == 0);

    CL::SummaryFifo::Summary s1 = MakeSummary(100);
    CL::SummaryFifo::Summary s2 = MakeSummary(200);
    CL::SummaryFifo::Summary s3 = MakeSummary(300);
    s1.deliverySeq = 1;
    s2.deliverySeq = 2;
    s3.deliverySeq = 3;
    CHECK(f.Enqueue(s1));
    CHECK(f.Enqueue(s2));
    CHECK(f.Enqueue(s3));
    CHECK(f.Size() == 3);
    CHECK(f.EnqueueCount() == 3);
    CHECK(!f.Full());
    CHECK(!f.Empty());

    CHECK(f.Dequeue(out));
    CHECK(out.deliverySeq == 1 && out.charGuid == 100);
    CHECK(f.Dequeue(out));
    CHECK(out.deliverySeq == 2 && out.charGuid == 200);
    CHECK(f.Dequeue(out));
    CHECK(out.deliverySeq == 3 && out.charGuid == 300);
    CHECK(f.Empty());
    CHECK(!f.Dequeue(out));
}

static void TestCap()
{
    CL::SummaryFifo f;
    for (uint32_t i = 1; i <= CL::SummaryFifo::kMaxSummaries; ++i)
        CHECK(f.Enqueue(MakeSummary(100)));
    CHECK(f.Full());
    CHECK(f.Size() == CL::SummaryFifo::kMaxSummaries);
    CHECK(f.EnqueueCount() == CL::SummaryFifo::kMaxSummaries);
    CHECK(f.FullCount() == 0); // not yet full-rejected
    CHECK(f.DropCount() == 0);

    // Next enqueue is rejected (full): EnqueueCount must not grow.
    CHECK(!f.Enqueue(MakeSummary(100)));
    CHECK(f.FullCount() == 1);
    CHECK(f.DropCount() == 1);
    CHECK(f.EnqueueCount() == CL::SummaryFifo::kMaxSummaries);
    CHECK(f.Size() == CL::SummaryFifo::kMaxSummaries);

    // Dequeue one, then enqueue succeeds again.
    CL::SummaryFifo::Summary out;
    CHECK(f.Dequeue(out));
    CHECK(f.Size() == CL::SummaryFifo::kMaxSummaries - 1);
    CHECK(!f.Full());
    CHECK(f.Enqueue(MakeSummary(100)));
    CHECK(f.Full());
    CHECK(f.EnqueueCount() == CL::SummaryFifo::kMaxSummaries + 1);
}

static void TestDropAccounting()
{
    CL::SummaryFifo f;
    for (uint32_t i = 1; i <= CL::SummaryFifo::kMaxSummaries; ++i)
        f.Enqueue(MakeSummary(100));
    CHECK(!f.Enqueue(MakeSummary(100)));
    CHECK(!f.Enqueue(MakeSummary(100)));
    CHECK(!f.Enqueue(MakeSummary(100)));
    CHECK(f.FullCount() == 3);
    CHECK(f.DropCount() == 3);
    CHECK(f.FailureCount() == 0); // not disabled, just full
    CHECK(f.Size() == CL::SummaryFifo::kMaxSummaries);
}

static void TestFailureDisabling()
{
    CL::SummaryFifo f;
    CHECK(f.Enqueue(MakeSummary(100)));
    CHECK(f.Size() == 1);

    // Disable: simulates writer failure.
    f.Disable();
    CHECK(f.Disabled());

    // All further enqueues fail and count as failures.
    CHECK(!f.Enqueue(MakeSummary(200)));
    CHECK(!f.Enqueue(MakeSummary(300)));
    CHECK(f.FailureCount() == 2);
    CHECK(f.FullCount() == 0); // not full, just disabled
    CHECK(f.Size() == 1); // existing items remain

    // Dequeue still works after disable (drain existing).
    CL::SummaryFifo::Summary out;
    CHECK(f.Dequeue(out));
    CHECK(out.charGuid == 100);
    CHECK(f.Empty());
}

static void TestRestartReset()
{
    CL::SummaryFifo f;
    for (uint32_t i = 1; i <= 10; ++i)
        f.Enqueue(MakeSummary(100));
    for (uint32_t i = 11; i <= CL::SummaryFifo::kMaxSummaries + 5; ++i)
        f.Enqueue(MakeSummary(100));
    f.Disable();
    CHECK(!f.Enqueue(MakeSummary(99)));

    // Reset: everything clears.
    f.Reset();
    CHECK(f.Empty());
    CHECK(!f.Full());
    CHECK(!f.Disabled());
    CHECK(f.Size() == 0);
    CHECK(f.FullCount() == 0);
    CHECK(f.DropCount() == 0);
    CHECK(f.FailureCount() == 0);
    CHECK(f.EnqueueCount() == 0);

    // Fresh operation after reset.
    CL::SummaryFifo::Summary s = MakeSummary(200);
    CHECK(f.Enqueue(s));
    CHECK(f.Size() == 1);
    CHECK(f.EnqueueCount() == 1);
    CL::SummaryFifo::Summary out;
    CHECK(f.Dequeue(out));
    CHECK(out.charGuid == 200);
}

static void TestWraparound()
{
    CL::SummaryFifo f;
    for (int round = 0; round < 3; ++round)
    {
        for (uint32_t i = 1; i <= CL::SummaryFifo::kMaxSummaries; ++i)
        {
            CL::SummaryFifo::Summary s = MakeSummary(100 + static_cast<uint32_t>(round));
            s.deliverySeq = i;
            CHECK(f.Enqueue(s));
        }
        CHECK(f.Full());
        for (uint32_t i = 1; i <= CL::SummaryFifo::kMaxSummaries; ++i)
        {
            CL::SummaryFifo::Summary out;
            CHECK(f.Dequeue(out));
            CHECK(out.deliverySeq == i);
        }
        CHECK(f.Empty());
    }
}

// ---------------------------------------------------------------------------
// Real Store behavior through the fake transport
// ---------------------------------------------------------------------------
static void TestNonce()
{
    for (int i = 0; i < 8; ++i)
        CHECK(CL::MakeProcessNonce() != 0);
    // 64 samples must be unique (per-pair collision ~ 2^-32).
    std::vector<uint64_t> seen;
    for (int i = 0; i < 64; ++i)
    {
        uint64_t const n = CL::MakeProcessNonce();
        bool const dup =
            std::find(seen.begin(), seen.end(), n) != seen.end();
        CHECK(!dup);
        seen.push_back(n);
    }
}

static void TestStoreIdentity()
{
    FakeDb db;
    CL::Store store;
    uint64_t const nonce = CL::MakeProcessNonce();
    store.Init(nonce);
    CHECK(store.Nonce() == nonce);
    CHECK(nonce != 0);
    Wire(store, db);

    CL::SummaryFifo::Summary s = MakeSummary(100);
    CHECK(store.Enqueue(s));
    CHECK(store.Fifo().EnqueueCount() == 1);
    store.Pump();
    CHECK(db.pending.size() == 1);
    std::string const sql1 = db.pending[0].sql;
    // Delivery identity is store-assigned: process nonce + seq 1.
    CHECK(sql1.find(std::to_string(nonce) + ",1,100,") != std::string::npos);
    // The full 64-bit target guid is in the SQL (never truncated).
    CHECK(sql1.find(std::to_string(0x0102030405060708ULL)) != std::string::npos);
    db.FireNext(true);
    store.Pump();
    CHECK(db.pending.empty());

    // Monotonic sequence: the second delivery gets seq 2.
    CHECK(store.Enqueue(s));
    store.Pump();
    CHECK(db.pending.size() == 1);
    std::string const sql2 = db.pending[0].sql;
    CHECK(sql2 != sql1);
    CHECK(sql2.find(std::to_string(nonce) + ",2,100,") != std::string::npos);
    db.FireNext(true);
    store.Pump();
    CHECK(db.pending.empty());
    CHECK(store.Seq() == 2);
    CHECK(store.Fifo().EnqueueCount() == 2);
}

static void TestDeliverySequencing()
{
    FakeDb db;
    CL::Store store;
    store.Init(CL::MakeProcessNonce());
    Wire(store, db);
    CHECK(store.Enqueue(MakeSummary(111)));
    CHECK(store.Enqueue(MakeSummary(222)));
    CHECK(store.Enqueue(MakeSummary(333)));

    // FIFO order: 111, then 222, then 333.
    store.Pump();
    CHECK(db.pending.size() == 1);
    CHECK(db.pending[0].sql.rfind("INSERT IGNORE INTO bot_learning_encounter", 0) == 0);
    // Profile gate is part of the product SQL.
    CHECK(db.pending[0].sql.find("FROM bot_learning_profile WHERE char_guid=111 AND mode NOT IN (0,4)") != std::string::npos);
    db.FireNext(true);

    store.Pump();
    CHECK(db.pending.size() == 1);
    CHECK(db.pending[0].sql.find("FROM bot_learning_profile WHERE char_guid=222 AND mode NOT IN (0,4)") != std::string::npos);
    db.FireNext(true);

    store.Pump();
    CHECK(db.pending.size() == 1);
    CHECK(db.pending[0].sql.find("FROM bot_learning_profile WHERE char_guid=333 AND mode NOT IN (0,4)") != std::string::npos);
    db.FireNext(true);

    store.Pump();
    CHECK(db.pending.empty());
    CHECK(store.PendingCount() == 0);
    CHECK(!store.WriterDisabled());
    CHECK(db.submitted == 3);
}

static void TestOneInFlight()
{
    FakeDb db;
    CL::Store store;
    store.Init(CL::MakeProcessNonce());
    Wire(store, db);
    CHECK(store.Enqueue(MakeSummary(100)));
    CHECK(store.Enqueue(MakeSummary(200)));

    store.Pump();
    CHECK(db.pending.size() == 1);
    CHECK(store.InFlight());
    store.Pump(); // still in flight: no second submission
    CHECK(db.pending.size() == 1);
    CHECK(db.submitted == 1);

    db.FireNext(true);
    store.Pump();
    CHECK(db.pending.size() == 1);
    db.FireNext(true);
    store.Pump();
    CHECK(db.pending.empty());
    CHECK(db.submitted == 2);
    CHECK(!store.InFlight());
}

static void TestFailureDisablesStore()
{
    FakeDb db;
    CL::Store store;
    store.Init(CL::MakeProcessNonce());
    Wire(store, db);
    CHECK(store.Enqueue(MakeSummary(100)));
    CHECK(store.Enqueue(MakeSummary(200)));

    store.Pump();
    CHECK(db.pending.size() == 1);
    db.FireNext(false); // DB write failed
    store.Pump();       // reap: disable happens on the world thread
    CHECK(store.WriterDisabled());
    CHECK(store.Fifo().Disabled());
    CHECK(!store.InFlight());
    CHECK(store.DbFailureCount() == 1);
    CHECK(store.PendingCount() == 1); // second summary stays unsubmitted (bounded)

    store.Pump();
    CHECK(db.submitted == 1); // no further submission
    CHECK(!store.Enqueue(MakeSummary(300)));
    CHECK(store.Fifo().FailureCount() == 1);
}

static void TestSubmitFailure()
{
    FakeDb db;
    db.failedSubmits = 1;
    CL::Store store;
    store.Init(CL::MakeProcessNonce());
    Wire(store, db);
    CHECK(store.Enqueue(MakeSummary(100)));
    store.Pump(); // submission fails: not in flight, writer failure
    CHECK(db.pending.empty());
    CHECK(db.fired == 0);          // the callback is never invoked
    CHECK(!store.InFlight());
    CHECK(store.WriterDisabled()); // fail closed
    CHECK(store.SubmitFailureCount() == 1);
    store.Pump();
    CHECK(db.submitted == 1); // nothing resubmitted
}

static void TestTruncationFailClosed()
{
    CL::SummaryFifo::Summary s = MakeSummary(100);
    s.deliveryNonce = 42;
    s.deliverySeq = 7;
    char small[16];
    CHECK(!CL::Store::BuildInsertSql(small, sizeof(small), s)); // truncated: refused

    char big[2048];
    CHECK(CL::Store::BuildInsertSql(big, sizeof(big), s));
    std::string const sql = big;
    CHECK(sql.find("42,7,100,") != std::string::npos);
    // The full 64-bit target guid is present (a 32-bit truncation would
    // yield 858993459, which is not the full value below).
    CHECK(sql.find(std::to_string(0x0102030405060708ULL)) != std::string::npos);
    CHECK(sql.find("UNIX_TIMESTAMP() FROM bot_learning_profile WHERE char_guid=100 AND mode NOT IN (0,4)") != std::string::npos);
}

static void TestMaintenanceOrdering()
{
    FakeDb db;
    CL::Store store;
    store.Init(CL::MakeProcessNonce());
    Wire(store, db);
    store.MarkStaleInterrupted();
    store.QueueRetention();
    CHECK(store.MaintenancePending() == 3);
    CHECK(store.Enqueue(MakeSummary(100)));

    // Maintenance runs before summary writes, one per tick, and shares
    // the single-in-flight slot.
    store.Pump();
    CHECK(db.pending.size() == 1);
    CHECK(db.pending[0].sql.rfind("UPDATE bot_learning_encounter SET end_reason = 7", 0) == 0);
    db.FireNext(true);

    store.Pump();
    CHECK(db.pending.size() == 1);
    CHECK(db.pending[0].sql.rfind("DELETE FROM bot_learning_encounter", 0) == 0);
    db.FireNext(true);

    store.Pump();
    CHECK(db.pending.size() == 1);
    CHECK(db.pending[0].sql.rfind("UPDATE bot_learning_profile SET mode = 4", 0) == 0);
    db.FireNext(true);

    store.Pump();
    CHECK(db.pending.size() == 1);
    CHECK(db.pending[0].sql.rfind("INSERT IGNORE INTO bot_learning_encounter", 0) == 0);
    db.FireNext(true);

    store.Pump();
    CHECK(db.pending.empty());
    CHECK(store.MaintenancePending() == 0);
    CHECK(!store.WriterDisabled());
    CHECK(db.submitted == 4);
}

static void TestStaleRetryOnSubmitFailure()
{
    FakeDb db;
    db.failedSubmits = 1;
    CL::Store store;
    store.Init(CL::MakeProcessNonce());
    Wire(store, db);
    store.MarkStaleInterrupted();
    CHECK(store.Enqueue(MakeSummary(100)));

    store.Pump(); // stale submit fails (DB not ready): kept pending (retry)
    CHECK(db.pending.empty());
    CHECK(db.submitted == 1);
    CHECK(store.MaintenancePending() == 1);
    CHECK(!store.InFlight());

    store.Pump(); // retry: the stale repair is submitted before the summary
    CHECK(db.pending.size() == 1);
    CHECK(db.pending[0].sql.rfind("UPDATE bot_learning_encounter SET end_reason = 7", 0) == 0);
    db.FireNext(true);

    store.Pump();
    CHECK(db.pending.size() == 1);
    CHECK(db.pending[0].sql.rfind("INSERT IGNORE INTO bot_learning_encounter", 0) == 0);
    db.FireNext(true);
    store.Pump();
    CHECK(db.pending.empty());
    CHECK(store.MaintenancePending() == 0);
}

static void TestMaintenanceFailureDoesNotDisableStore()
{
    FakeDb db;
    CL::Store store;
    store.Init(CL::MakeProcessNonce());
    Wire(store, db);
    store.QueueRetention();
    CHECK(store.Enqueue(MakeSummary(100)));
    store.Pump();
    db.FireNext(false); // encounter retention write failed
    store.Pump();       // reap: maintenance failure is logged, store stays up;
                        // then the playbook retention is submitted
    CHECK(!store.WriterDisabled());
    CHECK(store.MaintenanceFailureCount() == 1);
    CHECK(db.pending.size() == 1);
    CHECK(db.pending[0].sql.rfind("UPDATE bot_learning_profile SET mode = 4", 0) == 0);
    db.FireNext(true);
    store.Pump(); // reap: success; the summary is submitted in the same tick
    CHECK(db.pending.size() == 1);
    CHECK(db.pending[0].sql.rfind("INSERT IGNORE INTO bot_learning_encounter", 0) == 0);
    db.FireNext(true);
    store.Pump();
    CHECK(db.pending.empty());
    CHECK(store.MaintenancePending() == 0);
}

static void TestCrossThreadCompletion()
{
    FakeDb db;
    CL::Store store;
    store.Init(CL::MakeProcessNonce());
    Wire(store, db);
    CHECK(store.Enqueue(MakeSummary(100)));
    store.Pump();
    CHECK(db.pending.size() == 1);
    // Simulate the DB worker thread: deliver the callback concurrently
    // while the world thread pumps.
    std::thread worker([&db]()
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        db.FireNext(true);
    });
    for (int i = 0; i < 2000 && store.InFlight(); ++i)
    {
        store.Pump();
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    worker.join();
    CHECK(!store.InFlight());
    CHECK(!store.WriterDisabled());
    CHECK(db.fired == 1);
}

static void TestShutdown()
{
    FakeDb db;
    CL::Store store;
    store.Init(CL::MakeProcessNonce());
    Wire(store, db);
    CHECK(store.Enqueue(MakeSummary(100)));
    store.Shutdown();
    store.Pump();
    CHECK(db.submitted == 0);
    CHECK(!store.Enqueue(MakeSummary(200)));
    CHECK(store.ShutdownFlag());
}

static void TestDuplicateIdentity()
{
    FakeDb db;
    CL::Store store;
    store.Init(CL::MakeProcessNonce());
    Wire(store, db);
    CL::SummaryFifo::Summary const same = MakeSummary(100);
    CHECK(store.Enqueue(same));
    CHECK(store.Enqueue(same)); // identical payload: the store still assigns
                                // a fresh identity (dedupe is SQL-side)
    store.Pump();
    CHECK(db.pending.size() == 1);
    std::string const first = db.pending[0].sql;
    db.FireNext(true);
    store.Pump();
    CHECK(db.pending.size() == 1);
    CHECK(db.pending[0].sql != first);
    db.FireNext(true);
    store.Pump();
    CHECK(db.pending.empty());
}

static void TestLifecycleSqlContract()
{
    // The product lifecycle statements carry the contract properties.
    std::string const stale = CL::kStaleInterruptedSql;
    CHECK(stale.find("end_reason = 7") != std::string::npos);
    CHECK(stale.find("is_complete = 0 AND completed_at = 0") != std::string::npos);
    CHECK(stale.find("delivery_nonce") == std::string::npos); // covers every earlier process

    std::string const enc = CL::kEncounterRetentionSql;
    CHECK(enc.find("2592000") != std::string::npos); // 30 days
    CHECK(enc.find(">= 500") != std::string::npos);  // 500 per character
    CHECK(enc.find("bot_learning_candidate WHERE state = 0") != std::string::npos); // evidence protected

    std::string const pb = CL::kPlaybookRetentionSql;
    CHECK(pb.find("COUNT(*) > 20") != std::string::npos); // over-budget profiles pause
    CHECK(pb.find("SET mode = 4") != std::string::npos); // fail closed without pruning evidence
}

static void TestLearningControlQueue()
{
    FakeDb db;
    CL::Store store;
    store.Init(CL::MakeProcessNonce());
    Wire(store, db);
    CHECK(!store.QueueControl(CL::ControlAction::Start, 0));
    CHECK(!store.QueueControl(CL::ControlAction::Resume, 42, CL::ControlMode::Paused));
    CHECK(store.QueueControl(CL::ControlAction::Start, 42));
    CHECK(store.QueueControl(CL::ControlAction::Pause, 42));
    store.Pump();
    CHECK(db.pending.size() == 1);
    CHECK(db.pending[0].sql.rfind("INSERT INTO bot_learning_profile", 0) == 0);
    CHECK(db.pending[0].sql.find("VALUES (42, 1, 1") != std::string::npos);
    CHECK(db.pending[0].sql.find("ON DUPLICATE KEY UPDATE") != std::string::npos);
    db.FireNext(true); store.Pump();
    CHECK(db.pending.size() == 1);
    CHECK(db.pending[0].sql.find("SET mode = 4") != std::string::npos);
    db.FireNext(true); store.Pump();
    CHECK(db.pending.empty());

    CHECK(!store.QueueControl(CL::ControlAction::Rollback, 42));
    CHECK(db.pending.empty()); // fail closed: no partial audit or rollback
}

static void TestExternalControlReservation()
{
    FakeDb db;
    CL::Store store;
    store.Init(CL::MakeProcessNonce());
    Wire(store, db);
    CHECK(store.BeginExternalControl());
    CHECK(store.ExternalControlInFlight());
    CHECK(!store.BeginExternalControl());
    CHECK(store.Enqueue(MakeSummary(7)));
    store.Pump();
    CHECK(db.pending.empty());
    CL::ControlStatus bad;
    bad.valid = true; bad.charGuid = 7; bad.mode = 99;
    store.CompleteExternalControl(true, &bad);
    CHECK(!store.LastControlStatus().valid);
    store.Pump();
    CHECK(db.pending.size() == 1);
    db.FireNext(true); store.Pump();

    CHECK(store.BeginExternalControl());
    CL::ControlStatus good;
    good.valid = true; good.charGuid = 7;
    good.mode = static_cast<uint32_t>(CL::ControlMode::Observe);
    good.insufficientEvidence = true;
    store.CompleteExternalControl(true, &good);
    CHECK(store.LastControlStatus().valid);
    CHECK(store.LastControlStatus().insufficientEvidence);
    CHECK(store.BeginExternalControl());
    store.CompleteExternalControl(false);
    CHECK(store.ControlFailureCount() == 1);

    CHECK(store.BeginExternalControl(7, true));
    std::function<void(bool)> completion = store.ExternalCompletionCallback();
    completion(true); // simulates DB-thread transaction completion
    store.Pump();      // world thread publishes the baseline status
    CHECK(!store.ExternalControlInFlight());
    CHECK(store.LastControlStatus().valid);
    CHECK(store.LastControlStatus().activePlaybookVersion == 0);
}

int main()
{
    TestFifoOrder();
    TestCap();
    TestDropAccounting();
    TestFailureDisabling();
    TestRestartReset();
    TestWraparound();
    TestNonce();
    TestStoreIdentity();
    TestDeliverySequencing();
    TestOneInFlight();
    TestFailureDisablesStore();
    TestSubmitFailure();
    TestTruncationFailClosed();
    TestMaintenanceOrdering();
    TestStaleRetryOnSubmitFailure();
    TestMaintenanceFailureDoesNotDisableStore();
    TestCrossThreadCompletion();
    TestShutdown();
    TestDuplicateIdentity();
    TestLifecycleSqlContract();
    TestLearningControlQueue();
    TestExternalControlReservation();
    if (g_failures != 0)
    {
        std::printf("learning store value tests: %d FAILURES\n", g_failures);
        return 1;
    }
    std::printf("learning store value tests: ALL OK\n");
    return 0;
}
