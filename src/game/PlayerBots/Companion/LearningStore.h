// BL-003 (KAP-558): bounded asynchronous encounter-summary persistence
// adapter for owned companions.
//
// Two layers:
//   1. SummaryFifo: a pure fixed-capacity FIFO contract (max 64
//      summaries, no heap allocation in enqueue/dequeue, deterministic
//      full/drop/failure counters and disable-on-writer-failure
//      behavior). Fully header-only; covered by standalone value tests.
//   2. Store: the game adapter. It submits at most one write at a time
//      through an injected submitter that wraps the repository's
//      CharacterDatabase.PExecuteCallback / SqlDelayThread convention.
//      The world thread only performs bounded queue/pump work and never
//      waits for database I/O. The DB callback publishes a bounded
//      atomic completion status on a shared payload and touches nothing
//      else; Pump applies success/failure on the world thread and
//      disables the FIFO there.
//
// The implementation is header-inline on purpose: src/game/CMakeLists.txt
// names the PlayerBot sources explicitly (no glob) and CMake is outside
// the BL-003 edit set, so every symbol must be visible to the already
// listed translation units.
//
// No learned behavior is enabled by this module. It persists
// observations and proves version lifecycle mechanics only.
#ifndef TORTOISE_COMPANION_LEARNINGSTORE_H
#define TORTOISE_COMPANION_LEARNINGSTORE_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <random>
#if defined(_WIN32)
#  include <windows.h>
#else
#  include <ctime>
#  include <unistd.h>
#endif

namespace Companion
{
namespace Learning
{

// ---------------------------------------------------------------------------
// Pure fixed-capacity FIFO for encounter summaries. No heap allocation in
// enqueue/dequeue. Disable-on-writer-failure: once Disable() is called,
// Enqueue always returns false. World thread only.
// ---------------------------------------------------------------------------
class SummaryFifo
{
public:
    static constexpr uint32_t kMaxSummaries = 64;

    // Bounded scalar summary (no heap, no strings, no pointers).
    struct Summary
    {
        uint64_t deliveryNonce = 0;  // assigned by the Store, never the caller
        uint32_t deliverySeq = 0;    // assigned by the Store, never the caller
        uint32_t charGuid = 0;
        uint64_t targetGuid = 0;     // full 64-bit creature GUID (never truncated)
        uint64_t sessionId = 0;      // opaque; 0 in BL-003 (no account data)
        uint32_t sequence = 0;       // per-session encounter sequence (1-based)
        uint32_t policyVersion = 0;
        uint32_t playbookVersion = 0;
        uint32_t source = 0;
        uint32_t route = 0;
        uint32_t durationMs = 0;
        uint32_t effectiveDamage = 0;
        uint32_t periodicDamage = 0;
        uint32_t damageTaken = 0;
        uint32_t deaths = 0;
        uint32_t ownerOverrides = 0;
        uint32_t decisions = 0;
        uint32_t castsAccepted = 0;
        uint32_t castsRejected = 0;
        uint32_t castsNoEligible = 0;
        uint32_t eventCount = 0;
        uint32_t endReason = 0;      // 0..6 = BL-002 EndReason, 7 = interrupted
        bool complete = false;
        bool overflow = false;
        bool efficacyEligible = false;
        uint32_t capturedAt = 0;     // UNIX seconds
    };

    // No-heap enqueue. Returns false if full or disabled.
    // When full: increments FullCount and DropCount.
    // When disabled: increments FailureCount.
    // On success: increments EnqueueCount.
    bool Enqueue(Summary const& s)
    {
        if (m_disabled)
        {
            ++m_failureCount;
            return false;
        }
        if (m_size >= kMaxSummaries)
        {
            ++m_fullCount;
            ++m_dropCount;
            return false;
        }
        uint32_t const tail = (m_head + m_size) % kMaxSummaries;
        m_items[tail] = s;
        ++m_size;
        ++m_enqueueCount;
        return true;
    }

    // No-heap dequeue. Returns false if empty.
    bool Dequeue(Summary& out)
    {
        if (m_size == 0)
            return false;
        out = m_items[m_head];
        m_head = (m_head + 1) % kMaxSummaries;
        --m_size;
        return true;
    }

    uint32_t Size() const { return m_size; }
    bool Full() const { return m_size >= kMaxSummaries; }
    bool Empty() const { return m_size == 0; }
    bool Disabled() const { return m_disabled; }

    uint32_t FullCount() const { return m_fullCount; }
    uint32_t DropCount() const { return m_dropCount; }
    uint32_t FailureCount() const { return m_failureCount; }
    uint32_t EnqueueCount() const { return m_enqueueCount; }

    // Disable: once called, Enqueue always returns false.
    void Disable() { m_disabled = true; }

    // Reset for restart: clears all state (items, counters, flags).
    void Reset()
    {
        m_head = 0;
        m_size = 0;
        m_fullCount = 0;
        m_dropCount = 0;
        m_failureCount = 0;
        m_enqueueCount = 0;
        m_disabled = false;
    }

private:
    Summary m_items[kMaxSummaries];
    uint32_t m_head = 0;
    uint32_t m_size = 0;
    uint32_t m_fullCount = 0;
    uint32_t m_dropCount = 0;
    uint32_t m_failureCount = 0;
    uint32_t m_enqueueCount = 0;
    bool m_disabled = false;
};

// ---------------------------------------------------------------------------
// Product SQL contract. These literals are the product's lifecycle
// statements; the disposable migration test executes the exact text
// extracted from this file (it does not keep its own copy).
//
// end_reason vocabulary for bot_learning_encounter:
//   0..6 = BL-002 Companion::Encounter::EndReason values
//   7    = interrupted (written by kStaleInterruptedSql)
// ---------------------------------------------------------------------------
static constexpr uint32_t kEndReasonInterrupted = 7;

// Startup repair: rows that a previous process captured but never
// completed (is_complete = 0 AND completed_at = 0, any delivery nonce)
// are marked interrupted and excluded from efficacy. Covers rows from
// every earlier process; idempotent (completed_at is set, so a second
// run matches nothing); cannot race the encounter writer, which runs
// through the same single-in-flight slot and always inserts
// completed_at > 0 atomically with the row.
static char const* const kStaleInterruptedSql =
    "UPDATE bot_learning_encounter SET end_reason = 7, is_complete = 0, is_efficacy_eligible = 0, completed_at = UNIX_TIMESTAMP() WHERE is_complete = 0 AND completed_at = 0";

// Encounter retention: at most 500 rows per character and at most 30
// days (2592000 s). Characters with a pending candidate (state = 0) are
// not pruned at all: their cited evidence stays and pruning is paused
// for that character instead of deleting evidence.
static char const* const kEncounterRetentionSql =
    "DELETE FROM bot_learning_encounter WHERE id IN (SELECT id FROM (SELECT e.id FROM bot_learning_encounter AS e WHERE e.char_guid NOT IN (SELECT char_guid FROM bot_learning_candidate WHERE state = 0) AND (e.captured_at < UNIX_TIMESTAMP() - 2592000 OR (SELECT COUNT(*) FROM bot_learning_encounter AS e2 WHERE e2.char_guid = e.char_guid AND (e2.captured_at > e.captured_at OR (e2.captured_at = e.captured_at AND e2.id > e.id))) >= 500)) AS victims)";

// Playbook retention: at most the 20 newest historical (state = 1)
// versions per character (ranked by created_at DESC, id DESC). Active
// versions and versions cited as parent_version by another version of
// the same character always survive.
static char const* const kPlaybookRetentionSql =
    "UPDATE bot_learning_profile SET mode = 4, updated_at = UNIX_TIMESTAMP() WHERE char_guid IN (SELECT char_guid FROM bot_learning_playbook GROUP BY char_guid HAVING COUNT(*) > 20)";

// Profile-gated idempotent insert. INSERT IGNORE on the unique
// (delivery_nonce, delivery_seq, char_guid) key makes re-delivery of the
// same identity a no-op (no double counting). The SELECT from
// bot_learning_profile gates the insert: no profile row, or mode
// disabled (0) / paused (4), means zero rows are inserted. Only scalar
// summary values are persisted: no raw chat, prompts, Unit pointers,
// account IDs, credentials, or event-buffer dumps. %llu appears 3 times
// (nonce, target_guid, session_id); %u appears 24 times; argument order
// is part of the contract.
static char const* const kEncounterInsertSqlTemplate =
    "INSERT IGNORE INTO bot_learning_encounter (`delivery_nonce`,`delivery_seq`,`char_guid`,`target_guid`,`session_id`,`sequence`,`policy_version`,`playbook_version`,`source`,`route`,`duration_ms`,`effective_damage`,`periodic_damage`,`damage_taken`,`deaths`,`owner_overrides`,`decisions`,`casts_accepted`,`casts_rejected`,`casts_no_eligible`,`event_count`,`end_reason`,`is_complete`,`is_overflow`,`is_efficacy_eligible`,`captured_at`,`completed_at`) SELECT %llu,%u,%u,%llu,%llu,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,UNIX_TIMESTAMP() FROM bot_learning_profile WHERE char_guid=%u AND mode NOT IN (0,4)";

// ---------------------------------------------------------------------------
// BL-008A: learning control mode and status types.
// ---------------------------------------------------------------------------

// Typed learning control modes (match bot_learning_profile.mode).
enum class ControlMode : uint32_t
{
    Disabled = 0,
    Observe  = 1,
    Shadow   = 2,
    Trial    = 3,
    Paused   = 4,
    kCount   = 5,
};

// Validate a mode value against the typed vocabulary.
inline bool IsValidControlMode(uint32_t m)
{
    return m < static_cast<uint32_t>(ControlMode::kCount);
}

// True for modes that represent an active learning session.
inline bool IsActiveControlMode(uint32_t m)
{
    return m >= static_cast<uint32_t>(ControlMode::Observe) &&
           m <= static_cast<uint32_t>(ControlMode::Trial);
}

// Candidate states. Insufficient is a derived sentinel (never stored in
// the database); it represents the explicit "not enough evidence" state
// that status reads report when the pending candidate lacks evidence.
enum class CandidateState : uint32_t
{
    None         = 0,  // no candidate row
    Pending      = 1,  // evidence accumulating (state = 0 in DB)
    Insufficient = 2,  // derived: pending but not enough evidence
    Accepted     = 3,  // state = 1 in DB
    Rejected     = 4,  // state = 2 in DB
    Expired      = 5,  // state = 3 in DB
};

enum class ControlAction : uint8_t { Start, Pause, Resume, Rollback };

// Bounded control status snapshot. World-thread only. Never fabricates
// improvement: fields reflect the last confirmed state from a successful
// operation or status read. An invalid status (valid = false) means the
// character has no profile row or the status has never been read.
struct ControlStatus
{
    bool valid = false;
    uint32_t charGuid = 0;
    uint32_t mode = 0;                    // ControlMode value
    uint32_t activePlaybookVersion = 0;   // 0 = baseline
    uint32_t expectedPlaybookVersion = 0; // CAS target for activation
    uint32_t candidateState = 0;          // CandidateState value
    uint32_t evidenceCount = 0;
    bool insufficientEvidence = false;    // explicit: not enough evidence
};

// ---------------------------------------------------------------------------
// BL-008A: control operation SQL templates. Each statement is CAS-
// protected: the WHERE clause includes the expected current mode, so a
// concurrent state change causes zero affected rows (CAS failure).
// No command handler ever constructs these; only the Store does.
// ---------------------------------------------------------------------------

// Start (enroll): create a missing profile in observe mode or CAS an existing
// disabled profile back to observe. Existing active/paused modes are unchanged.
static char const* const kControlStartSqlTemplate =
    "INSERT INTO bot_learning_profile (char_guid, mode, schema_version, created_at, updated_at) VALUES (%u, %u, 1, UNIX_TIMESTAMP(), UNIX_TIMESTAMP()) ON DUPLICATE KEY UPDATE updated_at = IF(mode = 0, VALUES(updated_at), updated_at), mode = IF(mode = 0, VALUES(mode), mode)";
// BOTLEARN-START-ALL: batch start for every persistent registered
// companion owned by one human account. Same idempotent start semantics
// as kControlStartSqlTemplate (missing profile inserted in observe mode,
// disabled profile CAS back to observe, every other mode untouched), but
// scoped by the ownership binding in one statement: the candidate set is
// bot_ownership rows bound to the account joined to registered roster
// rows. One maintenance slot regardless of roster size; a per-bot
// QueueControl loop would be partially applied above the four-slot
// maintenance capacity.
static char const* const kControlStartAllSqlTemplate =
    "INSERT INTO bot_learning_profile (char_guid, mode, schema_version, created_at, updated_at) SELECT b.char_guid, %u, 1, UNIX_TIMESTAMP(), UNIX_TIMESTAMP() FROM bot_ownership b JOIN playerbot p ON p.char_guid = b.char_guid WHERE b.owner_account_id = %u ON DUPLICATE KEY UPDATE updated_at = IF(mode = 0, VALUES(updated_at), updated_at), mode = IF(mode = 0, VALUES(mode), mode)";

// Pause: CAS from an active mode to paused (4).
static char const* const kControlPauseSqlTemplate =
    "UPDATE bot_learning_profile SET mode = 4, updated_at = UNIX_TIMESTAMP() WHERE char_guid = %u AND mode IN (1, 2, 3)";

// Resume: CAS from paused (4) to a target active mode.
static char const* const kControlResumeSqlTemplate =
    "UPDATE bot_learning_profile SET mode = %u, updated_at = UNIX_TIMESTAMP() WHERE char_guid = %u AND mode = 4";

// Rollback audit: record provenance (event = 5, actor = owner) with the
// current active_playbook_version as version_ref. CAS-protected: only
// inserts when the profile is in an active or paused mode.
static char const* const kControlRollbackAuditSqlTemplate =
    "INSERT INTO bot_learning_audit (char_guid, event, actor, reason, version_ref, created_at) SELECT char_guid, 5, 'owner', 'rollback', active_playbook_version, UNIX_TIMESTAMP() FROM bot_learning_profile WHERE char_guid = %u AND mode IN (1, 2, 3, 4)";

// Rollback profile: reset to baseline (active_playbook_version = 0,
// expected_version = 0) and disable. CAS-protected: only updates when
// the profile is in an active or paused mode.
static char const* const kControlRollbackProfileSqlTemplate =
    "UPDATE bot_learning_profile SET mode = 0, active_playbook_version = 0, expected_version = 0, updated_at = UNIX_TIMESTAMP() WHERE char_guid = %u AND mode IN (1, 2, 3, 4)";

// Status read: bounded SELECT of the profile state. Used for restart-
// safe status refresh.
static char const* const kControlStatusSqlTemplate =
    "SELECT mode, active_playbook_version, expected_version FROM bot_learning_profile WHERE char_guid = %u";

// ---------------------------------------------------------------------------
// A robust nonzero 64-bit process nonce without secrets or persistence.
// High 32 bits: wall-clock milliseconds at process start (unique across
// process starts for ~49 days). Low 32 bits: system entropy mixed with
// the OS process id. Colliding with a nonce already used by a previous
// process would require the same start millisecond (mod 2^32) and the
// same entropy sample.
// ---------------------------------------------------------------------------
inline uint64_t MakeProcessNonce()
{
    uint64_t wallMs = 0;
    uint32_t pid = 0;
#if defined(_WIN32)
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    wallMs = u.QuadPart / 10000ULL; // 100 ns ticks since 1601 -> ms
    pid = static_cast<uint32_t>(GetCurrentProcessId());
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    wallMs = static_cast<uint64_t>(ts.tv_sec) * 1000ULL +
             static_cast<uint64_t>(ts.tv_nsec / 1000000);
    pid = static_cast<uint32_t>(getpid());
#endif
    std::random_device rd;
    uint32_t const entropy = static_cast<uint32_t>(rd());
    uint32_t const mix = static_cast<uint32_t>(
        (static_cast<uint64_t>(pid) * 0x9E3779B97F4A7C15ULL) >> 32);
    uint64_t const nonce =
        (static_cast<uint64_t>(wallMs & 0xFFFFFFFFULL) << 32) |
        static_cast<uint64_t>(entropy ^ mix);
    return nonce != 0 ? nonce : 1;
}

// ---------------------------------------------------------------------------
// The game adapter: one-in-flight async writer. The world thread calls
// Pump(), which performs at most one bounded dequeue and one submission
// per call and never blocks. At most one write (encounter insert or
// maintenance) is ever in flight.
// ---------------------------------------------------------------------------
class Store
{
public:
    // Submits one SQL statement and stores the completion callback the
    // same way CharacterDatabase.PExecuteCallback does. Returns false on
    // submission failure; per Database::Execute the callback is never
    // invoked in that case.
    using SubmitFn = std::function<bool(char const*, std::function<void(bool)>*)>;

    // Initialize with a nonzero process nonce (zero is rejected). Call
    // once at startup, on the world thread, never while a write is in
    // flight.
    void Init(uint64_t nonce)
    {
        if (nonce == 0)
            nonce = 1; // never-zero delivery nonce
        m_nonce = nonce;
        m_seq = 0;
        m_completion = std::make_shared<CompletionState>();
        m_inFlight.store(false, std::memory_order_release);
        m_externalControl.store(false, std::memory_order_release);
        m_externalDone.store(false, std::memory_order_release);
        m_externalSuccess.store(false, std::memory_order_release);
        m_externalCharGuid = 0;
        m_externalBaselineOnSuccess = false;
        m_inFlightMaintenance = false;
        m_shutdown.store(false, std::memory_order_release);
        m_maintenanceSize = 0;
        m_submitFailureCount = 0;
        m_dbFailureCount = 0;
        m_truncatedCount = 0;
        m_maintenanceFailureCount = 0;
        m_maintenanceDroppedCount = 0;
        m_controlFailureCount = 0;
        m_controlStatus = ControlStatus{};
        m_fifo.Reset();
    }

    // Wire the async transport (the manager injects the
    // CharacterDatabase.PExecuteCallback wrapper). World thread.
    void SetSubmitter(SubmitFn submit) { m_submit = std::move(submit); }

    // Reserve the one-in-flight slot for an injected transaction/query
    // adapter. This keeps database engine types outside this value module.
    // External work may start only when all Store-owned work is idle.
    bool BeginExternalControl(uint32_t charGuid = 0, bool baselineOnSuccess = false)
    {
        if (m_shutdown.load(std::memory_order_acquire) ||
            m_inFlight.load(std::memory_order_acquire) ||
            m_maintenanceSize != 0 || !m_fifo.Empty())
            return false;
        m_externalControl.store(true, std::memory_order_release);
        m_externalDone.store(false, std::memory_order_release);
        m_externalSuccess.store(false, std::memory_order_release);
        m_externalCharGuid = charGuid;
        m_externalBaselineOnSuccess = baselineOnSuccess;
        m_inFlight.store(true, std::memory_order_release);
        return true;
    }

    std::function<void(bool)> ExternalCompletionCallback()
    {
        return [this](bool success)
        {
            m_externalSuccess.store(success, std::memory_order_release);
            m_externalDone.store(true, std::memory_order_release);
        };
    }

    // Called on the world thread after an injected adapter publishes its
    // result. Invalid status never replaces the last confirmed snapshot.
    void CompleteExternalControl(bool success, ControlStatus const* status = nullptr)
    {
        if (!m_externalControl.load(std::memory_order_acquire))
            return;
        if (success && status && status->valid &&
            IsValidControlMode(status->mode) && status->charGuid != 0)
            m_controlStatus = *status;
        else if (!success)
            ++m_controlFailureCount;
        m_externalControl.store(false, std::memory_order_release);
        m_inFlight.store(false, std::memory_order_release);
    }

    // Enqueue a completed summary. The delivery identity (nonzero
    // process nonce plus the per-store monotonic sequence) is assigned
    // here and only here; the caller never manufactures delivery ids.
    // Returns false when un-initialized, shut down, full or disabled.
    bool Enqueue(SummaryFifo::Summary const& in)
    {
        if (m_shutdown.load(std::memory_order_acquire) || m_nonce == 0)
            return false;
        SummaryFifo::Summary s = in;
        s.deliveryNonce = m_nonce;
        s.deliverySeq = NextSeq();
        return m_fifo.Enqueue(s);
    }

    // Bounded queue/pump work for the world thread. Never blocks, never
    // performs database I/O: at most one completion reap and at most
    // one submission per call.
    void Pump()
    {
        if (m_shutdown.load(std::memory_order_acquire))
            return;
        if (!m_submit || m_fifo.Disabled())
            return; // fail-closed: the maintenance queue is intentionally
                    // left undrained (paused) while the writer is disabled
        if (m_externalControl.load(std::memory_order_acquire))
        {
            if (!m_externalDone.load(std::memory_order_acquire))
                return; // injected transaction/query owns the shared slot
            bool const ok = m_externalSuccess.load(std::memory_order_acquire);
            if (ok && m_externalBaselineOnSuccess && m_externalCharGuid != 0)
            {
                ControlStatus status;
                status.valid = true;
                status.charGuid = m_externalCharGuid;
                status.mode = static_cast<uint32_t>(ControlMode::Disabled);
                m_controlStatus = status;
            }
            else if (!ok)
                ++m_controlFailureCount;
            m_externalDone.store(false, std::memory_order_release);
            m_externalControl.store(false, std::memory_order_release);
            m_inFlight.store(false, std::memory_order_release);
        }

        // 1) Reap the in-flight completion published by the DB thread.
        //    The callback never touches the FIFO: disable-on-writer-
        //    failure happens here, on the world thread.
        if (m_inFlight.load(std::memory_order_acquire))
        {
            CompletionState& cs = *m_completion;
            if (!cs.done.load(std::memory_order_acquire))
                return; // still in flight; at most one write in flight
            bool const ok = cs.success.load(std::memory_order_acquire);
            cs.done.store(false, std::memory_order_release);
            m_inFlight.store(false, std::memory_order_release);
            if (!ok)
            {
                if (m_inFlightMaintenance)
                    ++m_maintenanceFailureCount;
                else
                {
                    ++m_dbFailureCount;
                    m_fifo.Disable(); // writer failure: fail closed (bounded)
                    return;
                }
            }
        }

        // 2) One submission this tick: maintenance first (the startup
        //    stale repair, then retention), then the next queued summary.
        if (m_maintenanceSize > 0)
        {
            MaintenanceOp const op = m_maintenance[0];
            bool const ok = SubmitSql(op.sql, /*maintenance=*/true);
            if (ok || !op.retry)
                ShiftMaintenance();
            // retry=true and submit failed: keep pending; the database
            // is not ready and the next tick retries before new work.
            return;
        }

        SummaryFifo::Summary s;
        if (!m_fifo.Dequeue(s))
            return; // nothing to submit

        char sql[1024];
        if (!BuildInsertSql(sql, sizeof(sql), s))
        {
            // snprintf truncation: never submit a truncated statement.
            ++m_truncatedCount;
            m_fifo.Disable();
            return;
        }
        if (!SubmitSql(sql, /*maintenance=*/false))
        {
            // Submission failure: per Database::Execute the write did not
            // enter the queue. Writer failure: fail closed (bounded).
            m_fifo.Disable();
        }
    }

    // Safe shutdown: no further Pump call submits work. An in-flight
    // write keeps its shared completion state alive for the DB thread;
    // the delay thread drains its queue on teardown (see
    // SqlDelayThread). No flush is performed.
    void Shutdown()
    {
        m_shutdown.store(true, std::memory_order_release);
    }

    // Async startup repair of rows that earlier processes captured but
    // never completed. Queued for the next idle Pump tick; runs through
    // the same single-in-flight slot as encounter writes (no race) and
    // retries while the database is not ready.
    void MarkStaleInterrupted()
    {
        EnqueueMaintenance(kStaleInterruptedSql, /*retryOnSubmitFailure=*/true);
    }

    // Queue the bounded lifecycle pruning (see kEncounterRetentionSql /
    // kPlaybookRetentionSql). Pending candidate evidence is protected;
    // a submit failure drops the cycle (the next pace tick retries)
    // instead of blocking summary writes.
    void QueueRetention()
    {
        EnqueueMaintenance(kEncounterRetentionSql, /*retryOnSubmitFailure=*/false);
        EnqueueMaintenance(kPlaybookRetentionSql, /*retryOnSubmitFailure=*/false);
    }

    // Queue an owner-authorized learning control mutation through the same
    // serialized async slot as maintenance and encounter writes. Callers
    // provide identity/authorization; this layer owns all SQL construction.
    bool QueueControl(ControlAction action, uint32_t charGuid,
                      ControlMode resumeMode = ControlMode::Observe)
    {
        if (charGuid == 0 || m_shutdown.load(std::memory_order_acquire))
            return false;
        char sql[1024];
        unsigned const guid = static_cast<unsigned>(charGuid);
        int n = -1;
        switch (action)
        {
            case ControlAction::Start:
                n = std::snprintf(sql, sizeof(sql), kControlStartSqlTemplate,
                                  guid, static_cast<unsigned>(ControlMode::Observe));
                break;
            case ControlAction::Pause:
                n = std::snprintf(sql, sizeof(sql), kControlPauseSqlTemplate, guid);
                break;
            case ControlAction::Resume:
                if (!IsActiveControlMode(static_cast<uint32_t>(resumeMode)))
                    return false;
                n = std::snprintf(sql, sizeof(sql), kControlResumeSqlTemplate,
                                  static_cast<unsigned>(resumeMode), guid);
                break;
            case ControlAction::Rollback:
                // Audit + baseline activation must be one verified DB
                // transaction. The current bool-only submitter cannot prove
                // affected rows or return query state, so fail closed until
                // the transactional/query adapter is added.
                return false;
        }
        return n >= 0 && static_cast<size_t>(n) < sizeof(sql) &&
               EnqueueMaintenance(sql, false);
    }

    // BOTLEARN-START-ALL: queue the batch start for one account's owned
    // companions. The account identity is supplied by the authorized
    // caller (the manager validated the issuing player); this layer owns
    // the SQL and serializes through the same one-in-flight maintenance
    // slot as control mutations and maintenance.
    bool QueueStartAll(uint32_t ownerAccountId)
    {
        if (ownerAccountId == 0 || m_shutdown.load(std::memory_order_acquire))
            return false;
        char sql[1024];
        int const n = std::snprintf(sql, sizeof(sql),
                                    kControlStartAllSqlTemplate,
                                    static_cast<unsigned>(ControlMode::Observe),
                                    static_cast<unsigned>(ownerAccountId));
        return n >= 0 && static_cast<size_t>(n) < sizeof(sql) &&
               EnqueueMaintenance(sql, false);
    }

    // Build the profile-gated idempotent insert for one summary.
    // Returns false on snprintf truncation (the caller fails closed);
    // a truncated statement is never submitted.
    static bool BuildInsertSql(char* buf, size_t bufSize,
                               SummaryFifo::Summary const& s)
    {
        int const n = std::snprintf(buf, bufSize, kEncounterInsertSqlTemplate,
            static_cast<unsigned long long>(s.deliveryNonce),
            static_cast<unsigned>(s.deliverySeq),
            static_cast<unsigned>(s.charGuid),
            static_cast<unsigned long long>(s.targetGuid),
            static_cast<unsigned long long>(s.sessionId),
            static_cast<unsigned>(s.sequence),
            static_cast<unsigned>(s.policyVersion),
            static_cast<unsigned>(s.playbookVersion),
            static_cast<unsigned>(s.source),
            static_cast<unsigned>(s.route),
            static_cast<unsigned>(s.durationMs),
            static_cast<unsigned>(s.effectiveDamage),
            static_cast<unsigned>(s.periodicDamage),
            static_cast<unsigned>(s.damageTaken),
            static_cast<unsigned>(s.deaths),
            static_cast<unsigned>(s.ownerOverrides),
            static_cast<unsigned>(s.decisions),
            static_cast<unsigned>(s.castsAccepted),
            static_cast<unsigned>(s.castsRejected),
            static_cast<unsigned>(s.castsNoEligible),
            static_cast<unsigned>(s.eventCount),
            static_cast<unsigned>(s.endReason),
            static_cast<unsigned>(s.complete ? 1u : 0u),
            static_cast<unsigned>(s.overflow ? 1u : 0u),
            static_cast<unsigned>(s.efficacyEligible ? 1u : 0u),
            static_cast<unsigned>(s.capturedAt),
            static_cast<unsigned>(s.charGuid));
        return n >= 0 && static_cast<size_t>(n) < bufSize;
    }

    // Introspection (world thread, non-blocking).
    uint32_t PendingCount() const { return m_fifo.Size(); }
    uint32_t MaintenancePending() const { return m_maintenanceSize; }
    bool InFlight() const { return m_inFlight.load(std::memory_order_acquire); }
    bool WriterDisabled() const { return m_fifo.Disabled(); }
    bool ExternalControlInFlight() const { return m_externalControl.load(std::memory_order_acquire); }
    ControlStatus const& LastControlStatus() const { return m_controlStatus; }
    uint32_t ControlFailureCount() const { return m_controlFailureCount; }
    bool ShutdownFlag() const { return m_shutdown.load(std::memory_order_acquire); }
    uint64_t Nonce() const { return m_nonce; }
    uint32_t Seq() const { return m_seq; }
    uint32_t SubmitFailureCount() const { return m_submitFailureCount; }
    uint32_t DbFailureCount() const { return m_dbFailureCount; }
    uint32_t TruncatedCount() const { return m_truncatedCount; }
    uint32_t MaintenanceFailureCount() const { return m_maintenanceFailureCount; }
    uint32_t MaintenanceDroppedCount() const { return m_maintenanceDroppedCount; }
    SummaryFifo const& Fifo() const { return m_fifo; }

private:
    struct CompletionState
    {
        std::atomic<bool> done{false};
        std::atomic<bool> success{false};
    };

    struct MaintenanceOp
    {
        char sql[1024];
        bool retry = false;
    };

    static constexpr uint32_t kMaxMaintenance = 4;

    uint32_t NextSeq() { return ++m_seq; }

    bool EnqueueMaintenance(char const* sql, bool retryOnSubmitFailure)
    {
        if (m_maintenanceSize >= kMaxMaintenance)
        {
            ++m_maintenanceDroppedCount;
            return false;
        }
        int const n = std::snprintf(m_maintenance[m_maintenanceSize].sql,
                                    sizeof(m_maintenance[0].sql), "%s", sql);
        if (n < 0 || static_cast<size_t>(n) >= sizeof(m_maintenance[0].sql))
        {
            ++m_maintenanceDroppedCount;
            return false;
        }
        m_maintenance[m_maintenanceSize].retry = retryOnSubmitFailure;
        ++m_maintenanceSize;
        return true;
    }

    void ShiftMaintenance()
    {
        for (uint32_t i = 0; i + 1 < m_maintenanceSize; ++i)
            m_maintenance[i] = m_maintenance[i + 1];
        --m_maintenanceSize;
    }

    bool SubmitSql(char const* sql, bool maintenance)
    {
        // The callback captures only the shared completion state, never
        // `this`: if the store (or the manager owning it) is destroyed
        // while the DB thread still holds the queued operation, the
        // callback remains safe, because its only reachable state is
        // the shared payload. No singleton lifetime is assumed.
        std::shared_ptr<CompletionState> const state = m_completion;
        std::function<void(bool)> callback = [state](bool success)
        {
            state->success.store(success, std::memory_order_release);
            state->done.store(true, std::memory_order_release);
        };
        state->success.store(false, std::memory_order_release);
        state->done.store(false, std::memory_order_release);
        m_inFlight.store(true, std::memory_order_release);
        m_inFlightMaintenance = maintenance;
        bool const ok = m_submit(sql, &callback);
        if (!ok)
        {
            // Submission failed: per Database::Execute the callback is
            // never invoked, so the slot is cleared here. It is never
            // overwritten by a racing callback.
            m_inFlight.store(false, std::memory_order_release);
            ++m_submitFailureCount;
        }
        return ok;
    }

    SummaryFifo m_fifo;
    SubmitFn m_submit;
    std::shared_ptr<CompletionState> m_completion;
    uint64_t m_nonce = 0;
    uint32_t m_seq = 0;
    std::atomic<bool> m_inFlight{false};
    std::atomic<bool> m_externalControl{false};
    std::atomic<bool> m_externalDone{false};
    std::atomic<bool> m_externalSuccess{false};
    uint32_t m_externalCharGuid = 0;
    bool m_externalBaselineOnSuccess = false;
    bool m_inFlightMaintenance = false; // world thread only
    std::atomic<bool> m_shutdown{false};
    MaintenanceOp m_maintenance[kMaxMaintenance];
    uint32_t m_maintenanceSize = 0;
    uint32_t m_submitFailureCount = 0;
    uint32_t m_dbFailureCount = 0;
    uint32_t m_truncatedCount = 0;
    uint32_t m_maintenanceFailureCount = 0;
    uint32_t m_maintenanceDroppedCount = 0;
    uint32_t m_controlFailureCount = 0;
    ControlStatus m_controlStatus;
};

} // namespace Learning
} // namespace Companion
#endif
