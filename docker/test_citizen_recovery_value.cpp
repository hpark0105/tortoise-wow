// Value-level tests for Companion::CitizenRecovery::Progress (TW-BOTS-002
// S1). Every check executes the real helper; explicit checks are used
// instead of assert so the suite also runs under NDEBUG.
#include "CitizenRecovery.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool condition, char const* label)
{
    if (!condition)
    {
        std::printf("FAIL: %s\n", label);
        ++g_failures;
    }
}

using Companion::CitizenRecovery::Outcome;
using Companion::CitizenRecovery::Progress;
using Companion::CitizenRecovery::EvaluateLeaderLoss;
using Companion::CitizenRecovery::LeaderLoss;
using Companion::CitizenRecovery::RetryPolicy;
using Companion::CitizenRecovery::FollowProgress;
using Companion::CitizenRecovery::ShouldPauseGroupDuty;
using Companion::CitizenRecovery::ShouldRetreat;
using Companion::CitizenRecovery::HasDangerAreaEvidence;
using Companion::CitizenRecovery::ShouldAvoidDangerCell;
using Companion::CitizenRecovery::DangerCellBufferYards;
using Companion::CitizenRecovery::IsOutsideDangerCell;
using Companion::CitizenRecovery::FirstUnsafeRememberedCell;
using Companion::CitizenRecovery::IsUnsafeForRememberedCell;
using Companion::CitizenRecovery::RememberedDangerCell;

void TestDangerCellGeometry()
{
    Check(!IsOutsideDangerCell(5.0f, 5.0f, 0, 0, 0.0f),
          "danger-cell: point inside remembered cell is unsafe");
    Check(!IsOutsideDangerCell(45.0f, 20.0f, 0, 0, 5.0f),
          "danger-cell: buffer boundary is unsafe");
    Check(IsOutsideDangerCell(45.01f, 20.0f, 0, 0, 5.0f),
          "danger-cell: point beyond buffer is safe");
    Check(!IsOutsideDangerCell(-45.0f, -20.0f, -2, -1, 5.0f),
          "danger-cell: negative grid cells use floor-coordinate geometry");
    Check(IsOutsideDangerCell(0.0f, 0.0f, -2, -1, 5.0f),
          "danger-cell: distant point outside negative grid cell is safe");
    float const nan = std::nanf("");
    Check(!IsOutsideDangerCell(nan, 0.0f, 0, 0, 0.0f),
          "danger-cell: nonfinite candidate is unsafe");
    Check(!IsOutsideDangerCell(100.0f, 100.0f, 0, 0, nan),
          "danger-cell: nonfinite buffer is unsafe");
    Check(!IsOutsideDangerCell(100.0f, 100.0f, 0, 0, -1.0f),
          "danger-cell: negative buffer is unsafe");
}

void TestDangerMemoryEvidenceAndExpiry()
{
    Check(HasDangerAreaEvidence(1u),
          "danger-memory: any recorded death confirms the pending fatal zone");
    Check(!HasDangerAreaEvidence(0u),
          "danger-memory: an empty history does not confirm a fatal zone");
    Check(ShouldAvoidDangerCell(true, 1u, 30000u),
          "danger-memory: active cooldown keeps the cell avoided");
    Check(ShouldAvoidDangerCell(false, 3u, 21599u),
          "danger-memory: repeated-death cell is avoided within six hours");
    Check(!ShouldAvoidDangerCell(false, 3u, 21600u),
          "danger-memory: old cell avoidance expires at six hours");
    Check(!ShouldAvoidDangerCell(false, 2u, 1u),
          "danger-memory: low-count history alone does not avoid a cell");
    Check(DangerCellBufferYards(true, 1u) == 120.0f,
          "danger-memory: active cooldown uses widest buffer");
    Check(DangerCellBufferYards(false, 3599u) == 80.0f,
          "danger-memory: recent repeated deaths use medium buffer");
    Check(DangerCellBufferYards(false, 3600u) == 40.0f,
          "danger-memory: older repeated deaths use narrow buffer");
}

void TestRetreatHealthThreshold()
{
    Check(ShouldRetreat(15u, 100u, false), "health threshold: retreat at 15 percent");
    Check(!ShouldRetreat(16u, 100u, false), "health threshold: keep fighting above 15 percent");
    Check(ShouldRetreat(10u, 100u, false), "health threshold: retreat below 15 percent");
    Check(!ShouldRetreat(10u, 100u, false, 10u, 100u), "dying target: finish fight above critical health");
    Check(ShouldRetreat(5u, 100u, false, 10u, 100u), "dying target: retreat at critical health");
    Check(ShouldRetreat(80u, 100u, true), "health threshold: danger zone overrides health");
    Check(!ShouldRetreat(0u, 0u, true), "health threshold: zero maximum health is invalid");
}

void TestNoStart()
{
    Progress p;
    Check(!p.Active(), "no-start: inactive before Begin");
    Check(p.Update(1000, 1.0f, 1.0f, false) == Outcome::Inactive,
          "no-start: Update before Begin is Inactive");
    Check(!p.Active(), "no-start: still inactive after Update");
}

void TestResetAndRestart()
{
    Progress p;
    p.Begin(0.0f, 0.0f);
    Check(p.Active(), "reset: active after Begin");
    Check(p.Update(1000, 1.0f, 0.0f, true) == Outcome::Walking,
          "reset: walking mid-retreat");
    p.Reset();
    Check(!p.Active(), "reset: inactive after Reset");
    Check(p.Update(1000, 10.0f, 0.0f, false) == Outcome::Inactive,
          "reset: Update after Reset is Inactive");

    // A fresh Begin restarts with fresh counters.
    p.Begin(10.0f, 10.0f);
    Check(p.Active(), "restart: active after second Begin");
    Check(p.Update(1000, 10.0f, 10.0f, true) == Outcome::Walking,
          "restart: fresh counters, no stall before the first interval");
    Check(p.Update(4000, 10.0f, 10.0f, true) == Outcome::Stalled,
          "restart: 5000 ms of no movement stalls from the fresh checkpoint");
    Check(!p.Active(), "restart: inactive after the stall");
}

void TestNoMovementCombatClearedIsNotSuccess()
{
    Progress p;
    p.Begin(0.0f, 0.0f);
    Check(p.Update(1000, 0.0f, 0.0f, false) == Outcome::Walking,
          "no-movement: combat cleared with zero displacement is not success");
    Check(p.Update(4000, 0.0f, 0.0f, false) == Outcome::Stalled,
          "no-movement: stalls at the first checkpoint");
    Check(!p.Active(), "no-movement: inactive after stall");
}

void TestFiveYardEscape()
{
    Progress p;
    p.Begin(0.0f, 0.0f);
    Check(p.Update(1000, 4.99f, 0.0f, false) == Outcome::Walking,
          "escape: 4.99 yd with combat cleared is not enough");
    Check(p.Update(1000, 5.0f, 0.0f, false) == Outcome::Escaped,
          "escape: exactly 5 yd from start with combat cleared is success");
    Check(!p.Active(), "escape: activity ends on success");
    Check(p.Update(1000, 9.0f, 0.0f, false) == Outcome::Inactive,
          "escape: Inactive after a terminal outcome");
}

void TestMovementWithoutCombatClearance()
{
    Progress p;
    p.Begin(0.0f, 0.0f);
    Check(p.Update(6000, 10.0f, 0.0f, true) == Outcome::Walking,
          "no-clear: 10 yd while in combat is not success");
    Check(p.Active(), "no-clear: still active while threatened");
    Check(p.Update(1000, 10.0f, 0.0f, false) == Outcome::Escaped,
          "no-clear: displacement from start counts once combat clears");
    Check(!p.Active(), "no-clear: inactive after escape");
}

void TestTwoYardThreshold()
{
    Progress p;
    p.Begin(0.0f, 0.0f);
    Check(p.Update(5000, 2.0f, 0.0f, true) == Outcome::Walking,
          "threshold: exactly 2 yd over an interval is not stalled");
    Check(p.Active(), "threshold: still active at exactly 2 yd");
    Check(p.Update(5000, 2.5f, 0.0f, true) == Outcome::Stalled,
          "threshold: 0.5 yd since the checkpoint is stalled");

    Progress q;
    q.Begin(0.0f, 0.0f);
    Check(q.Update(5000, 1.9f, 0.0f, true) == Outcome::Stalled,
          "threshold: under 2 yd over an interval is stalled");
}

void TestFiveSecondStalledCheck()
{
    Progress p;
    p.Begin(0.0f, 0.0f);
    Check(p.Update(4999, 1.0f, 0.0f, true) == Outcome::Walking,
          "stall: no stall check before 5000 ms accumulated");
    Check(p.Update(1, 1.0f, 0.0f, true) == Outcome::Stalled,
          "stall: 1 yd across the 5000 ms boundary stalls");
    Check(!p.Active(), "stall: inactive after stall");
}

void TestTimeoutEvenWithMovement()
{
    Progress p;
    p.Begin(0.0f, 0.0f);
    Check(p.Update(5000, 5.0f, 0.0f, true) == Outcome::Walking,
          "timeout: first 5 yd progress leg under threat");
    Check(p.Update(5000, 10.0f, 0.0f, true) == Outcome::Walking,
          "timeout: second 5 yd progress leg under threat");
    Check(p.Update(5000, 15.0f, 0.0f, true) == Outcome::TimedOut,
          "timeout: 15000 ms ends the retreat even with progress");
    Check(!p.Active(), "timeout: inactive after timeout");
}

void TestSuccessPriorityOverTimeout()
{
    Progress p;
    p.Begin(0.0f, 0.0f);
    Check(p.Update(5000, 5.0f, 0.0f, true) == Outcome::Walking,
          "priority: first leg under threat");
    Check(p.Update(5000, 10.0f, 0.0f, true) == Outcome::Walking,
          "priority: second leg under threat");
    Check(p.Update(5000, 10.0f, 0.0f, false) == Outcome::Escaped,
          "priority: success beats timeout at the 15000 ms boundary");
}

void TestSuccessPriorityOverStall()
{
    Progress p;
    p.Begin(0.0f, 0.0f);
    Check(p.Update(5000, 6.0f, 0.0f, true) == Outcome::Walking,
          "priority: checkpoint at 6 yd under threat");
    Check(p.Update(5000, 5.0f, 0.0f, false) == Outcome::Escaped,
          "priority: 5 yd from start beats the 1 yd checkpoint regression");
}

void TestSuccessiveProgressCheckpoints()
{
    Progress p;
    p.Begin(0.0f, 0.0f);
    float x = 0.0f;
    for (int i = 0; i < 2; ++i)
    {
        x += 3.0f;
        Check(p.Update(5000, x, 0.0f, true) == Outcome::Walking,
              "checkpoints: 3 yd per 5000 ms interval keeps walking");
    }
    Check(p.Active(), "checkpoints: still active at 10000 ms");
    x += 3.0f;
    Check(p.Update(5000, x, 0.0f, true) == Outcome::TimedOut,
          "checkpoints: lifetime still bounds a walker at 15000 ms");
    Check(!p.Active(), "checkpoints: inactive after timeout");
}

void TestHugeDiffOverflow()
{
    std::uint32_t const maxMs = std::numeric_limits<std::uint32_t>::max();

    Progress p;
    p.Begin(0.0f, 0.0f);
    Check(p.Update(maxMs, 3.0f, 0.0f, true) == Outcome::TimedOut,
          "overflow: a max-size diff saturates into timeout, not wrap/crash");
    Check(!p.Active(), "overflow: inactive after timeout");

    Progress q;
    q.Begin(0.0f, 0.0f);
    Check(q.Update(maxMs, 10.0f, 0.0f, false) == Outcome::Escaped,
          "overflow: success still outranks the saturated lifetime");
    Check(!q.Active(), "overflow: inactive after escape");

    Progress r;
    r.Begin(0.0f, 0.0f);
    r.Update(maxMs, 0.0f, 0.0f, true);
    Check(!r.Active(), "overflow: saturated retreat ends");
    Check(r.Update(maxMs, 0.0f, 0.0f, true) == Outcome::Inactive,
          "overflow: no revival after a terminal outcome");
}

void TestNonfiniteCoords()
{
    float const nan = std::nanf("");
    float const inf = std::numeric_limits<float>::infinity();

    Progress a;
    a.Begin(0.0f, 0.0f);
    Check(a.Update(1000, nan, 0.0f, false) == Outcome::Stalled,
          "nonfinite: NaN x cannot escape, fails stalled");
    Check(!a.Active(), "nonfinite: inactive after stalled");

    Progress b;
    b.Begin(0.0f, 0.0f);
    Check(b.Update(1000, inf, 0.0f, false) == Outcome::Stalled,
          "nonfinite: inf x fails stalled");
    Check(!b.Active(), "nonfinite: inactive after stalled");

    Progress c;
    c.Begin(nan, 0.0f);
    Check(c.Update(1000, 1.0f, 0.0f, false) == Outcome::Stalled,
          "nonfinite: NaN start fails stalled on first scoring");
    Check(!c.Active(), "nonfinite: inactive after stalled");
}

void TestFailureCounting()
{
    RetryPolicy r;
    Check(r.Failures() == 0u, "count: zero before any failure");
    Check(r.CanTry(), "count: ready before any failure");

    r.Fail(true, 0.0f, 0.0f);
    Check(r.Failures() == 1u, "count: first failure recorded");
    Check(r.RetryMs() == 5000u, "count: first failure uses the base cooldown");
    Check(!r.CanTry(), "count: not ready after the first failure");
    Check(!r.Deferred(), "count: first failure is not deferred");

    r.Fail(true, 100.0f, 0.0f);
    Check(r.Failures() == 2u, "count: second failure recorded");
    Check(r.RetryMs() == 5000u, "count: second failure uses the base cooldown");
    Check(!r.CanTry(), "count: not ready after the second failure");
    Check(!r.Deferred(), "count: second failure is not deferred");

    r.Fail(true, 200.0f, 0.0f);
    Check(r.Failures() == 3u, "count: third failure recorded");
    Check(r.RetryMs() == 30000u, "count: third failure escalates the cooldown");
    Check(!r.CanTry(), "count: not ready after the third failure");
    Check(r.Deferred(), "count: third failure defers retries");
}

void TestCooldownBoundaryAndHugeDiff()
{
    RetryPolicy r;
    Check(r.CanTry(), "cooldown: can try before any failure");
    r.Fail(true, 0.0f, 0.0f);
    Check(!r.CanTry(), "cooldown: cannot try during the base cooldown");
    Check(r.RetryMs() == 5000u, "cooldown: base cooldown is 5000 ms");
    r.Tick(4999);
    Check(r.RetryMs() == 1u, "cooldown: 4999 ms leaves 1 ms");
    Check(!r.CanTry(), "cooldown: still one ms from ready");
    r.Tick(1);
    Check(r.RetryMs() == 0u, "cooldown: exactly 5000 ms clears the cooldown");
    Check(r.CanTry(), "cooldown: ready at the exact boundary");

    // A huge diff clamps instead of wrapping, from both cooldown tiers.
    std::uint32_t const maxMs = std::numeric_limits<std::uint32_t>::max();
    RetryPolicy h;
    h.Fail(true, 0.0f, 0.0f);
    h.Tick(maxMs);
    Check(h.RetryMs() == 0u, "cooldown: huge diff clamps a base cooldown to zero");
    Check(h.CanTry(), "cooldown: ready after a huge base-cooldown diff");
    h.Fail(true, 100.0f, 0.0f);
    h.Fail(true, 200.0f, 0.0f);
    Check(h.RetryMs() == 30000u, "cooldown: escalated cooldown re-armed at three");
    h.Tick(maxMs);
    Check(h.RetryMs() == 0u, "cooldown: huge diff clamps an escalated cooldown to zero");
    Check(h.CanTry(), "cooldown: ready after a huge escalated-cooldown diff");
}

void TestFurtherFailuresRemainDeferred()
{
    RetryPolicy r;
    r.Fail(true, 0.0f, 0.0f);
    r.Fail(true, 100.0f, 0.0f);
    r.Fail(true, 200.0f, 0.0f);
    Check(r.Deferred(), "defer: three failures defer");
    r.Fail(true, 300.0f, 0.0f);
    Check(r.Failures() == 4u, "defer: fourth failure recorded");
    Check(r.RetryMs() == 30000u, "defer: fourth failure re-arms the escalated cooldown");
    Check(r.Deferred(), "defer: still deferred after a fourth failure");

    // The cooldown lapses but the failure count must persist (no implicit
    // reset), so the next failure re-defers immediately.
    r.Tick(30000);
    Check(r.CanTry(), "defer: the escalated cooldown lapses");
    Check(r.Failures() == 4u, "defer: failure count persists after the lapse");
    Check(!r.Deferred(), "defer: not deferred while the cooldown is zero");
    r.Fail(true, 400.0f, 0.0f);
    Check(r.Failures() == 5u, "defer: fifth failure recorded");
    Check(r.RetryMs() == 30000u, "defer: fifth failure re-arms the escalated cooldown");
    Check(r.Deferred(), "defer: re-defers immediately with the persisted count");
}

void TestEndpointExclusion()
{
    RetryPolicy r;
    Check(!r.Excludes(0.0f, 0.0f), "exclude: nothing excluded before any failure");

    r.Fail(true, 0.0f, 0.0f);
    Check(r.Excludes(0.0f, 0.0f), "exclude: the remembered endpoint is excluded");
    Check(r.Excludes(1.0f, 1.0f), "exclude: a point within 8 yards is excluded");
    Check(!r.Excludes(9.0f, 0.0f), "exclude: a point beyond 8 yards is allowed");
    Check(!r.Excludes(20.0f, 20.0f), "exclude: a far alternate endpoint is allowed");

    // Exactly 8 yards is still excluded (<= boundary, both axes).
    RetryPolicy edge;
    edge.Fail(true, 0.0f, 0.0f);
    Check(edge.Excludes(8.0f, 0.0f), "exclude: exactly 8 yards on x is excluded");
    Check(edge.Excludes(0.0f, 8.0f), "exclude: exactly 8 yards on y is excluded");
}

void TestEndpointExpiry()
{
    RetryPolicy r;
    r.Fail(true, 0.0f, 0.0f);
    Check(r.Excludes(0.0f, 0.0f), "expiry: remembered before the lifetime lapses");
    r.Tick(59999);
    Check(r.Excludes(0.0f, 0.0f), "expiry: one ms of lifetime still excludes");
    r.Tick(1);
    Check(!r.Excludes(0.0f, 0.0f), "expiry: the endpoint no longer excludes at 60000 ms");
    Check(r.Failures() == 1u, "expiry: memory lapse does not reset the failure count");
}

void TestDuplicateRefresh()
{
    RetryPolicy r;
    r.Fail(true, 0.0f, 0.0f);
    Check(r.Excludes(0.0f, 0.0f), "refresh: first endpoint remembered");
    r.Fail(true, 3.0f, 0.0f);
    Check(r.Excludes(3.0f, 0.0f), "refresh: a nearby failure refreshes the same endpoint");
    r.Tick(50000);
    Check(r.Excludes(3.0f, 0.0f), "refresh: the re-armed lifetime survives 50000 ms");
    r.Fail(true, 6.0f, 0.0f);
    Check(r.Excludes(6.0f, 0.0f), "refresh: the endpoint tracks the newest position");
    // The two free slots are taken by far endpoints; a 4th far endpoint must
    // evict the single cluster slot, proving the cluster used one slot only.
    r.Fail(true, 100.0f, 0.0f);
    r.Fail(true, 200.0f, 0.0f);
    Check(r.Excludes(6.0f, 0.0f), "refresh: the cluster endpoint survives far fills");
    r.Fail(true, 300.0f, 0.0f);
    Check(!r.Excludes(6.0f, 0.0f), "refresh: the cluster slot is evicted by the 4th far endpoint");
    Check(r.Excludes(300.0f, 0.0f), "refresh: the newest far endpoint is remembered");
}

void TestMemoryBoundedEviction()
{
    // Fill three slots with far endpoints at different lifetimes so the
    // eviction target is unambiguous.
    RetryPolicy r;
    r.Fail(true, 0.0f, 0.0f);
    r.Tick(20000);
    r.Fail(true, 100.0f, 0.0f);
    r.Tick(20000);
    r.Fail(true, 200.0f, 0.0f);
    Check(r.Excludes(0.0f, 0.0f), "evict: first endpoint remembered");
    Check(r.Excludes(100.0f, 0.0f), "evict: second endpoint remembered");
    Check(r.Excludes(200.0f, 0.0f), "evict: third endpoint remembered");
    // A fourth far endpoint evicts the shortest-lived (the 20000 ms one).
    r.Fail(true, 300.0f, 0.0f);
    Check(!r.Excludes(0.0f, 0.0f), "evict: the shortest-lived endpoint is dropped");
    Check(r.Excludes(300.0f, 0.0f), "evict: the newest endpoint is remembered");
    Check(r.Excludes(100.0f, 0.0f), "evict: the middle-lifetime endpoint is kept");
    Check(r.Excludes(200.0f, 0.0f), "evict: the longest-lifetime endpoint is kept");

    // Deterministic tie-break: three equal lifetimes evict the lowest index.
    RetryPolicy t;
    t.Fail(true, 0.0f, 0.0f);
    t.Fail(true, 100.0f, 0.0f);
    t.Fail(true, 200.0f, 0.0f);
    t.Fail(true, 300.0f, 0.0f);
    Check(!t.Excludes(0.0f, 0.0f), "evict: a lifetime tie evicts the lowest index");
    Check(t.Excludes(100.0f, 0.0f), "evict: the tie keeps a higher index");
    Check(t.Excludes(300.0f, 0.0f), "evict: the tie keeps the newest endpoint");
}

void TestInvalidCoordinates()
{
    float const nan = std::nanf("");
    float const inf = std::numeric_limits<float>::infinity();

    RetryPolicy r;
    // Nonfinite endpoint data counts as a failure but never poisons memory.
    r.Fail(true, nan, 0.0f);
    Check(r.Failures() == 1u, "invalid: a NaN endpoint still counts a failure");
    Check(r.RetryMs() == 5000u, "invalid: a NaN endpoint still sets the cooldown");
    Check(!r.Excludes(0.0f, 0.0f), "invalid: a NaN endpoint never enters memory");

    r.Fail(true, inf, 0.0f);
    Check(r.Failures() == 2u, "invalid: an inf endpoint still counts a failure");
    Check(!r.Excludes(0.0f, 0.0f), "invalid: an inf endpoint never enters memory");

    // hasEndpoint=false counts a failure but remembers nothing.
    r.Fail(false, 5.0f, 5.0f);
    Check(r.Failures() == 3u, "invalid: hasEndpoint=false still counts a failure");
    Check(!r.Excludes(5.0f, 5.0f), "invalid: hasEndpoint=false remembers nothing");

    // Nonfinite candidates are always excluded regardless of memory.
    Check(r.Excludes(nan, 0.0f), "invalid: a NaN candidate is excluded");
    Check(r.Excludes(inf, 0.0f), "invalid: an inf candidate is excluded");
    Check(r.Excludes(0.0f, nan), "invalid: a NaN y candidate is excluded");

    // A valid endpoint remembered after the invalid ones still works.
    r.Reset();
    r.Fail(true, 10.0f, 10.0f);
    Check(r.Excludes(10.0f, 10.0f), "invalid: a valid endpoint is remembered after reset");
}

void TestFailureSaturation()
{
    RetryPolicy r;
    for (int i = 0; i < 300; ++i)
        r.Fail(true, 1.0f, 1.0f);
    Check(r.Failures() == 255u, "saturate: the failure count caps at 255");
    Check(r.RetryMs() == 30000u, "saturate: the escalated cooldown holds at the cap");
    Check(!r.CanTry(), "saturate: cannot retry at the cap");
    Check(r.Deferred(), "saturate: deferred at the cap");
}

void TestResetClearsEverything()
{
    RetryPolicy r;
    r.Fail(true, 0.0f, 0.0f);
    r.Fail(true, 100.0f, 0.0f);
    r.Fail(true, 200.0f, 0.0f);
    r.Tick(1000);
    Check(r.Failures() == 3u, "reset: failures are recorded");
    Check(!r.CanTry(), "reset: not ready before the reset");
    Check(r.Deferred(), "reset: deferred before the reset");
    Check(r.Excludes(0.0f, 0.0f), "reset: the endpoint is remembered before the reset");

    r.Reset();
    Check(r.Failures() == 0u, "reset: the failure count is cleared");
    Check(r.RetryMs() == 0u, "reset: the cooldown is cleared");
    Check(r.CanTry(), "reset: ready after the reset");
    Check(!r.Deferred(), "reset: not deferred after the reset");
    Check(!r.Excludes(0.0f, 0.0f), "reset: the first endpoint is cleared");
    Check(!r.Excludes(100.0f, 0.0f), "reset: the second endpoint is cleared");
    Check(!r.Excludes(200.0f, 0.0f), "reset: the third endpoint is cleared");
}

void TestProgressFeedsRetryEscalation()
{
    Progress progress;
    RetryPolicy retry;

    Check(retry.CanTry(), "feed: ready before any failure");

    // Attempt 1: a stalled retreat feeds the first failure.
    progress.Reset();
    progress.Begin(0.0f, 0.0f);
    Check(progress.Update(5000, 0.0f, 0.0f, true) == Outcome::Stalled,
          "feed: attempt 1 stalls");
    retry.Fail(true, 100.0f, 100.0f);
    Check(retry.Failures() == 1u, "feed: the first failure is recorded");
    Check(retry.RetryMs() == 5000u, "feed: the first failure sets the base cooldown");
    Check(!retry.CanTry(), "feed: cannot retry during the base cooldown");
    Check(!retry.Deferred(), "feed: the first failure is not deferred");

    // Attempt 2 waits for admission and uses a different destination.
    retry.Tick(5000);
    Check(retry.CanTry(), "feed: attempt 2 admitted after cooldown");
    Check(retry.Excludes(100.0f, 100.0f), "feed: first failed endpoint remains excluded");
    Check(!retry.Excludes(200.0f, 100.0f), "feed: alternate endpoint is available");
    progress.Reset();
    progress.Begin(0.0f, 0.0f);
    Check(progress.Update(5000, 0.0f, 0.0f, true) == Outcome::Stalled,
          "feed: attempt 2 stalls");
    retry.Fail(true, 200.0f, 100.0f);
    Check(retry.Failures() == 2u, "feed: the second failure is recorded");
    Check(retry.RetryMs() == 5000u, "feed: the second failure stays on the base cooldown");
    Check(!retry.CanTry(), "feed: cannot retry during the base cooldown");
    Check(!retry.Deferred(), "feed: still not deferred at two failures");

    // Attempt 3 has no usable alternate and escalates without starting movement.
    retry.Tick(5000);
    Check(retry.CanTry(), "feed: attempt 3 admitted after cooldown");
    Check(retry.Excludes(200.0f, 100.0f), "feed: second failed endpoint remains excluded");
    Check(!progress.Active(), "feed: no movement is active for unavailable attempt");
    retry.Fail(false, 0.0f, 0.0f);
    Check(retry.Failures() == 3u, "feed: the third failure is recorded");
    Check(retry.RetryMs() == 30000u, "feed: the third failure escalates the cooldown");
    Check(retry.Deferred(), "feed: the third failure defers retries");
    Check(!retry.CanTry(), "feed: still cannot retry while escalated");
}

void TestLeaderLossThreatenedFallsThrough()
{
    Check(EvaluateLeaderLoss(0u, true) == LeaderLoss::FallThrough,
          "leader-loss: threatened member falls through at zero wait");
    Check(EvaluateLeaderLoss(59999u, true) == LeaderLoss::FallThrough,
          "leader-loss: threatened member falls through just before the deadline");
    Check(EvaluateLeaderLoss(60000u, true) == LeaderLoss::FallThrough,
          "leader-loss: threatened member falls through at the deadline");
    Check(EvaluateLeaderLoss(std::numeric_limits<std::uint32_t>::max(), true) ==
              LeaderLoss::FallThrough,
          "leader-loss: threatened member falls through at saturated wait");
}

void TestLeaderLossWaitAndDisband()
{
    Check(EvaluateLeaderLoss(0u, false) == LeaderLoss::Wait,
          "leader-loss: unthreatened member waits at zero");
    Check(EvaluateLeaderLoss(59999u, false) == LeaderLoss::Wait,
          "leader-loss: unthreatened member waits just before the deadline");
    Check(EvaluateLeaderLoss(60000u, false) == LeaderLoss::Disband,
          "leader-loss: unthreatened member disbands at the deadline");
    Check(EvaluateLeaderLoss(std::numeric_limits<std::uint32_t>::max(), false) ==
              LeaderLoss::Disband,
          "leader-loss: unthreatened member disbands at saturated wait");
}
void TestFollowStallDeadlineBounds()
{
    using Companion::CitizenRecovery::kFollowStallDeadlineMs;
    using Companion::CitizenRecovery::kLeaderLostDeadlineMs;
    Check(kFollowStallDeadlineMs != 0u,
          "follow-stall: deadline is nonzero");
    Check(kFollowStallDeadlineMs > kLeaderLostDeadlineMs,
          "follow-stall: a reachable-but-unfollowable leader gets a "
          "longer grace than a dead leader");
    Check(kFollowStallDeadlineMs < 600000u,
          "follow-stall: deadline stays within ten minutes");
}

void TestFollowProgressDistinguishesMovementFromStall()
{
    FollowProgress moving;
    moving.Begin(0.0f, 0.0f);
    float x = 0.0f;
    for (std::uint32_t i = 0; i <
         Companion::CitizenRecovery::kFollowStallDeadlineMs /
             Companion::CitizenRecovery::kFollowProgressSampleMs; ++i)
    {
        x += 3.0f;
        Check(!moving.Update(Companion::CitizenRecovery::kFollowProgressSampleMs,
                             x, 0.0f),
              "follow-progress: steady movement beyond the deadline is not stalled");
    }
    Check(moving.Active(), "follow-progress: moving tracker remains active");

    FollowProgress blocked;
    blocked.Begin(0.0f, 0.0f);
    std::uint32_t const sampleMs = Companion::CitizenRecovery::kFollowProgressSampleMs;
    std::uint32_t const samples = Companion::CitizenRecovery::kFollowStallDeadlineMs / sampleMs;
    for (std::uint32_t i = 1; i < samples; ++i)
        Check(!blocked.Update(sampleMs, 0.0f, 0.0f),
              "follow-progress: blocked follower waits for the bounded deadline");
    Check(blocked.Update(sampleMs, 0.0f, 0.0f),
          "follow-progress: blocked follower stalls at the deadline");
}

void TestRecoveryPausesVoluntaryGroupDuty()
{
    Check(ShouldPauseGroupDuty(false, false),
          "group-recovery: unready member pauses follow and assistance");
    Check(!ShouldPauseGroupDuty(false, true),
          "group-recovery: self-defense remains available while unready");
    Check(!ShouldPauseGroupDuty(true, false),
          "group-recovery: ready member can perform group duty");
}
void TestLoginRelocationUnsafePosition()
{
    RememberedDangerCell active{0, 0, 3u, true, 30u};
    Check(IsUnsafeForRememberedCell(10.0f, 10.0f, active),
          "login-guard: position inside the remembered cell is unsafe");
    Check(IsUnsafeForRememberedCell(160.0f, 20.0f, active),
          "login-guard: position at the active buffer edge is unsafe");
    Check(FirstUnsafeRememberedCell(10.0f, 10.0f, {active}) == 0u,
          "login-guard: a single unsafe cell reports its own index");
    RememberedDangerCell far{10, 10, 3u, true, 30u};
    Check(FirstUnsafeRememberedCell(10.0f, 10.0f, {far, active}) == 1u,
          "login-guard: the unsafe cell is found among several rows");
    Check(FirstUnsafeRememberedCell(10.0f, 10.0f, {active, far}) == 0u,
          "login-guard: the first unsafe row in order is reported");
}

void TestLoginRelocationSafeAnchor()
{
    RememberedDangerCell active{0, 0, 3u, true, 30u};
    Check(!IsUnsafeForRememberedCell(165.0f, 20.0f, active),
          "login-guard: a same-zone anchor beyond the buffer is safe");
    Check(FirstUnsafeRememberedCell(165.0f, 20.0f, {active}) == std::nullopt,
          "login-guard: a safe anchor does not re-arm relocation");
    Check(FirstUnsafeRememberedCell(165.0f, 20.0f, {}) == std::nullopt,
          "login-guard: no active rows never re-arm relocation");

    RememberedDangerCell stale{0, 0, 2u, false, 30u};
    Check(!IsUnsafeForRememberedCell(10.0f, 10.0f, stale),
          "login-guard: an inactive low-death row is not a danger cell");
    RememberedDangerCell recent{0, 0, 3u, false, 3599u};
    Check(IsUnsafeForRememberedCell(110.0f, 20.0f, recent),
          "login-guard: a recent repeated cell keeps its 80-yard buffer");
    Check(!IsUnsafeForRememberedCell(125.0f, 20.0f, recent),
          "login-guard: a point beyond the 80-yard buffer is safe");
    RememberedDangerCell old{0, 0, 3u, false, 3600u};
    Check(IsUnsafeForRememberedCell(80.0f, 20.0f, old),
          "login-guard: an older repeated cell keeps its 40-yard buffer");
    Check(!IsUnsafeForRememberedCell(80.01f, 20.0f, old),
          "login-guard: a point beyond the 40-yard buffer is safe");
}

void TestLoginRelocationBufferBoundary()
{
    // IsOutsideDangerCell is strictly greater-than, so the exact buffer
    // edge stays unsafe and just beyond it is safe, at every tier.
    RememberedDangerCell active{0, 0, 3u, true, 30u};
    Check(IsUnsafeForRememberedCell(160.0f, 20.0f, active),
          "login-guard: exactly at the 120-yard edge is unsafe");
    Check(!IsUnsafeForRememberedCell(160.01f, 20.0f, active),
          "login-guard: just beyond the 120-yard edge is safe");
    Check(IsUnsafeForRememberedCell(112.0f, 136.0f, active),
          "login-guard: a diagonal exactly 120 yards from the corner is unsafe");
    Check(!IsUnsafeForRememberedCell(113.0f, 136.0f, active),
          "login-guard: a diagonal just beyond 120 yards is safe");
    RememberedDangerCell recent{0, 0, 3u, false, 3599u};
    Check(IsUnsafeForRememberedCell(120.0f, 20.0f, recent),
          "login-guard: exactly at the 80-yard edge is unsafe");
    Check(!IsUnsafeForRememberedCell(120.01f, 20.0f, recent),
          "login-guard: just beyond the 80-yard edge is safe");
    RememberedDangerCell old{0, 0, 3u, false, 3600u};
    Check(IsUnsafeForRememberedCell(80.0f, 20.0f, old),
          "login-guard: exactly at the 40-yard edge is unsafe");
    Check(!IsUnsafeForRememberedCell(80.01f, 20.0f, old),
          "login-guard: just beyond the 40-yard edge is safe");
}

void TestLoginRelocationInvalidInputs()
{
    float const nan = std::nanf("");
    float const inf = std::numeric_limits<float>::infinity();
    RememberedDangerCell active{0, 0, 3u, true, 30u};
    Check(IsUnsafeForRememberedCell(nan, 0.0f, active),
          "login-guard: a NaN login position is unsafe");
    Check(IsUnsafeForRememberedCell(inf, 0.0f, active),
          "login-guard: an inf login position is unsafe");
    Check(IsUnsafeForRememberedCell(0.0f, nan, active),
          "login-guard: a NaN y login position is unsafe");
    Check(FirstUnsafeRememberedCell(nan, 0.0f, {active}) == 0u,
          "login-guard: an untrusted position still reports the unsafe cell");
    RememberedDangerCell stale{0, 0, 2u, false, 30u};
    Check(!IsUnsafeForRememberedCell(nan, 0.0f, stale),
          "login-guard: an inactive row never reports an unsafe cell");
}
} // namespace

int main()
{
    TestDangerCellGeometry();
    TestDangerMemoryEvidenceAndExpiry();
    TestRetreatHealthThreshold();
    TestNoStart();
    TestResetAndRestart();
    TestNoMovementCombatClearedIsNotSuccess();
    TestFiveYardEscape();
    TestMovementWithoutCombatClearance();
    TestTwoYardThreshold();
    TestFiveSecondStalledCheck();
    TestTimeoutEvenWithMovement();
    TestSuccessPriorityOverTimeout();
    TestSuccessPriorityOverStall();
    TestSuccessiveProgressCheckpoints();
    TestHugeDiffOverflow();
    TestNonfiniteCoords();
    TestFailureCounting();
    TestCooldownBoundaryAndHugeDiff();
    TestFurtherFailuresRemainDeferred();
    TestEndpointExclusion();
    TestEndpointExpiry();
    TestDuplicateRefresh();
    TestMemoryBoundedEviction();
    TestInvalidCoordinates();
    TestFailureSaturation();
    TestResetClearsEverything();
    TestProgressFeedsRetryEscalation();
    TestLeaderLossThreatenedFallsThrough();
    TestLeaderLossWaitAndDisband();
    TestFollowStallDeadlineBounds();
    TestFollowProgressDistinguishesMovementFromStall();
    TestRecoveryPausesVoluntaryGroupDuty();
    TestLoginRelocationUnsafePosition();
    TestLoginRelocationSafeAnchor();
    TestLoginRelocationBufferBoundary();
    TestLoginRelocationInvalidInputs();

    if (g_failures != 0)
    {
        std::printf("value tests: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("value tests: ALL OK\n");
    return 0;
}
