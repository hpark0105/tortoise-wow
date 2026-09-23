// BL-002 (KAP-558): value-level coverage for the observe-only encounter
// recorder (Companion/Encounter.h). Standalone: no engine, no database,
// no Docker. Compiled and run by docker/test_companion_encounter_value.py.
#include "Companion/Encounter.h"

#include <cstdio>
#include <string>

namespace CE = Companion::Encounter;

static_assert(sizeof(CE::Event) <= 16, "event must stay a small fixed value");
static_assert(sizeof(CE::Recorder) <= 4096, "recorder must stay bounded (no heap)");

static int g_failures = 0;
#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            std::printf("FAIL %d: %s\n", __LINE__, #cond); \
            ++g_failures; \
        } \
    } while (0)

static void TestStateBoundaries()
{
    CE::Recorder r;
    CHECK(r.GetState() == CE::State::Idle);
    CHECK(!r.End(CE::EndReason::TargetInvalid)); // End while idle: no-op
    CHECK(r.GetEventCount() == 0);

    // Begin refused for a zero target.
    CHECK(!r.Begin(0, 0, 0));
    CHECK(r.GetState() == CE::State::Idle);

    // Begin from idle.
    CHECK(r.Begin(100, 1, 0));
    CHECK(r.GetState() == CE::State::Active);
    CHECK(r.GetTargetGuid() == 100);
    CHECK(r.GetEventCount() == 1);
    CHECK(r.EventAt(0)->kind == static_cast<uint8_t>(CE::EventKind::Begin));
    CHECK(r.EventAt(0)->elapsedMs == 0);
    CHECK(r.ActiveFor(100));
    CHECK(!r.ActiveFor(200));

    // Begin refused while active (no implicit replacement).
    CHECK(!r.Begin(200, 1, 0));
    CHECK(r.GetTargetGuid() == 100);
    CHECK(r.GetEventCount() == 1);

    // Tick accumulates monotonically while active only.
    r.Tick(1000);
    r.Tick(500);
    CHECK(r.GetElapsedMs() == 1500);

    CHECK(r.End(CE::EndReason::TargetDeath));
    CHECK(r.GetState() == CE::State::Complete);
    CHECK(r.GetEventCount() == 2);
    CHECK(r.EventAt(1)->kind == static_cast<uint8_t>(CE::EventKind::End));
    CHECK(r.EventAt(1)->endReason == static_cast<uint8_t>(CE::EndReason::TargetDeath));
    // Idempotent end: the summary and the buffer are untouched.
    CHECK(!r.End(CE::EndReason::OwnerOverride));
    CHECK(r.GetEventCount() == 2);

    // Tick and records after complete are no-ops.
    r.Tick(999);
    r.RecordDecision(10, false);
    r.RecordDamageDealt(100, 5, 0, false, false);
    r.RecordDamageTaken(5, 0, false, false);
    r.RecordOwnerOverride();
    CHECK(r.GetElapsedMs() == 1500);
    CHECK(r.GetEventCount() == 2);

    // Begin after complete starts fresh.
    CHECK(r.Begin(200, 2, 1));
    CHECK(r.GetEventCount() == 1);
    CHECK(r.GetElapsedMs() == 0);
    CE::Recorder::Summary const s2 = r.GetSummary();
    CHECK(s2.targetGuid == 200);
    CHECK(s2.source == 2);
    CHECK(s2.route == 1);
    CHECK(!s2.complete); // active: not yet terminal
    CHECK(!s2.efficacyEligible);
    CHECK(s2.overflow == false);
}

static void TestDamageAttribution()
{
    CE::Recorder r;
    CHECK(r.Begin(100, 1, 0));
    // Unrelated-target damage is ignored entirely (no event, no metric).
    r.RecordDamageDealt(200, 50, 772, false, false);
    CHECK(r.GetSummary().effectiveDamage == 0);
    CHECK(r.GetEventCount() == 1);
    // Matching direct damage counts.
    r.RecordDamageDealt(100, 50, 772, false, false);
    r.RecordDamageDealt(100, 30, 1715, false, false);
    // Periodic damage is attributed and distinguished.
    r.RecordDamageDealt(100, 20, 772, true, false);
    CE::Recorder::Summary const s = r.GetSummary();
    CHECK(s.effectiveDamage == 100);
    CHECK(s.periodicDamage == 20);
    // Zero (blocked) damage is ignored.
    r.RecordDamageDealt(100, 0, 0, false, false);
    CHECK(r.GetEventCount() == 4); // Begin + 3 damage events
    // Event fields: amount, spell id, flags.
    CE::Event const* e = r.EventAt(1);
    CHECK(e->kind == static_cast<uint8_t>(CE::EventKind::DamageDealt));
    CHECK(e->damage == 50 && e->spellId == 772 && (e->flags & CE::EF_PERIODIC) == 0);
    e = r.EventAt(3);
    CHECK(e->flags == CE::EF_PERIODIC);
    CHECK(e->damage == 20);
    // Damage taken while active (direct and periodic).
    r.RecordDamageTaken(40, 0, false, false);
    r.RecordDamageTaken(5, 0, true, false);
    CHECK(r.GetSummary().damageTaken == 45);
    CHECK(r.EventAt(5)->flags == CE::EF_PERIODIC);
    // Facts after the encounter ends are ignored.
    CHECK(r.End(CE::EndReason::TargetDeath));
    r.RecordDamageTaken(99, 0, false, false);
    r.RecordDamageDealt(100, 99, 0, false, false);
    CHECK(r.GetSummary().damageTaken == 45);
    CHECK(r.GetSummary().effectiveDamage == 100);
}

static void TestTargetDeath()
{
    CE::Recorder r;
    CHECK(r.Begin(100, 1, 0));
    r.Tick(1234);
    r.RecordDamageDealt(100, 40, 772, false, true);
    CHECK(r.GetState() == CE::State::Complete);
    CE::Recorder::Summary const s = r.GetSummary();
    CHECK(s.endReason == CE::EndReason::TargetDeath);
    CHECK(s.deaths == 1);
    CHECK(s.effectiveDamage == 40);
    CHECK(s.durationMs == 1234);
    CHECK(s.complete);
    CHECK(!s.overflow);
    CHECK(s.efficacyEligible);
    // Event trail: Begin, DamageDealt(died), Death(target), End(target-death).
    CHECK(r.GetEventCount() == 4);
    CHECK(r.EventAt(1)->flags == CE::EF_TARGET_DIED);
    CHECK(r.EventAt(2)->kind == static_cast<uint8_t>(CE::EventKind::Death));
    CHECK(r.EventAt(2)->flags == CE::EF_TARGET_DIED);
    CHECK(r.EventAt(3)->kind == static_cast<uint8_t>(CE::EventKind::End));
    // Late facts cannot change the completed summary.
    r.RecordDamageDealt(100, 10, 0, false, false);
    r.RecordOwnerOverride();
    r.RecordDecision(5, false);
    CHECK(r.GetSummary().endReason == CE::EndReason::TargetDeath);
    CHECK(r.GetSummary().ownerOverrides == 0);
    CHECK(r.GetSummary().decisions == 0);
    CHECK(r.GetEventCount() == 4);
}

static void TestCompanionDeath()
{
    CE::Recorder r;
    CHECK(r.Begin(100, 1, 0));
    r.RecordDamageDealt(100, 10, 0, false, false);
    r.RecordDamageTaken(25, 0, false, true);
    CHECK(r.GetState() == CE::State::Complete);
    CE::Recorder::Summary const s = r.GetSummary();
    CHECK(s.endReason == CE::EndReason::CompanionDeath);
    CHECK(s.deaths == 1);
    CHECK(s.damageTaken == 25);
    CHECK(s.effectiveDamage == 10);
    CHECK(s.complete);
    CHECK(!s.efficacyEligible); // safety outcome: reported, never efficacy
    CHECK(r.EventAt(r.GetEventCount() - 2)->kind == static_cast<uint8_t>(CE::EventKind::Death));
    CHECK(r.EventAt(r.GetEventCount() - 2)->flags == CE::EF_COMPANION_DIED);

    // Damage taken is always the companion's own: an unrelated kill in the
    // same instant does not stop it, and the death still completes it.
    CE::Recorder r2;
    CHECK(r2.Begin(100, 1, 0));
    r2.RecordDamageDealt(200, 5, 0, false, false); // unrelated: ignored
    r2.RecordDamageTaken(3, 0, false, true);
    CHECK(r2.GetSummary().endReason == CE::EndReason::CompanionDeath);
    CHECK(r2.GetSummary().damageTaken == 3);
    CHECK(r2.GetSummary().effectiveDamage == 0);
}

static void TestOwnerOverride()
{
    CE::Recorder r;
    r.RecordOwnerOverride(); // idle: no-op
    CHECK(r.GetEventCount() == 0);
    CHECK(r.GetState() == CE::State::Idle);
    CHECK(r.Begin(100, 1, 0));
    r.RecordDecision(100, true);
    r.RecordOwnerOverride();
    CHECK(r.GetState() == CE::State::Complete);
    CE::Recorder::Summary const s = r.GetSummary();
    CHECK(s.endReason == CE::EndReason::OwnerOverride);
    CHECK(s.ownerOverrides == 1);
    CHECK(s.decisions == 1);
    CHECK(s.complete);
    CHECK(!s.efficacyEligible); // owner override: safety-reported, excluded
    CHECK(r.EventAt(1)->kind == static_cast<uint8_t>(CE::EventKind::Decision));
    CHECK(r.EventAt(1)->flags == CE::EF_ROUTE_TANK);
    CHECK(r.EventAt(2)->kind == static_cast<uint8_t>(CE::EventKind::OwnerOverride));
    CHECK(r.EventAt(3)->kind == static_cast<uint8_t>(CE::EventKind::End));
    CHECK(r.EventAt(3)->endReason == static_cast<uint8_t>(CE::EndReason::OwnerOverride));
    // A second override (e.g. a stop right after a hold) is a no-op.
    r.RecordOwnerOverride();
    CHECK(r.GetSummary().ownerOverrides == 1);
    CHECK(r.GetEventCount() == 4);
}

static void TestNewTarget()
{
    CE::Recorder r;
    CHECK(r.Begin(100, 1, 0));
    r.Tick(1000);
    r.RecordDamageDealt(100, 10, 0, false, false);
    // Adapter path: close as target-changed, then begin the new one.
    CHECK(r.End(CE::EndReason::TargetChanged));
    CE::Recorder::Summary const prior = r.GetSummary();
    CHECK(prior.endReason == CE::EndReason::TargetChanged);
    CHECK(prior.targetGuid == 100);
    CHECK(prior.effectiveDamage == 10);
    CHECK(prior.durationMs == 1000);
    CHECK(!prior.efficacyEligible);
    CHECK(r.Begin(200, 1, 0));
    // The recorder keeps only the current encounter.
    CE::Recorder::Summary const s = r.GetSummary();
    CHECK(s.targetGuid == 200);
    CHECK(s.durationMs == 0);
    CHECK(s.effectiveDamage == 0);
    CHECK(r.GetEventCount() == 1);
    CHECK(r.EventAt(0)->kind == static_cast<uint8_t>(CE::EventKind::Begin));
    // Same target continues (the adapter's Begin is refused, the tick runs).
    r.Tick(500);
    r.RecordDamageDealt(200, 7, 0, false, false);
    CE::Recorder::Summary const s2 = r.GetSummary();
    CHECK(s2.targetGuid == 200);
    CHECK(s2.durationMs == 500);
    CHECK(s2.effectiveDamage == 7);
    // Tick advances duration only; the card bounds event kinds (no per-tick event).
    CHECK(r.GetEventCount() == 2);
}

static void TestDecisionAndCast()
{
    CE::Recorder r;
    CHECK(r.Begin(100, 1, 0));
    r.RecordDecision(1715, false);
    r.RecordCastResult(1715, CE::CastOutcome::Accepted);
    r.RecordDecision(0, false); // no eligible ability: fallback attack
    r.RecordCastResult(0, CE::CastOutcome::NoEligibleAbility);
    r.RecordDecision(355, true); // tank route decision
    r.RecordCastResult(355, CE::CastOutcome::Rejected);
    CE::Recorder::Summary const s = r.GetSummary();
    CHECK(s.decisions == 3);
    CHECK(s.castsAccepted == 1);
    CHECK(s.castsRejected == 1);
    CHECK(s.castsNoEligible == 1);
    CHECK(r.EventAt(1)->flags == 0); // rotation route
    // Event order: 0 Begin, 1 Decision, 2 Cast, 3 Decision, 4 Cast, 5 Decision (tank), 6 Cast.
    CHECK(r.EventAt(5)->flags == CE::EF_ROUTE_TANK);
    CHECK(r.EventAt(2)->castOutcome == static_cast<uint8_t>(CE::CastOutcome::Accepted));
    CHECK(r.EventAt(6)->castOutcome == static_cast<uint8_t>(CE::CastOutcome::Rejected));
    // Records while idle are ignored.
    CE::Recorder idle;
    idle.RecordDecision(1, false);
    idle.RecordCastResult(1, CE::CastOutcome::Accepted);
    CHECK(idle.GetEventCount() == 0);
    CHECK(idle.GetSummary().decisions == 0);
}

static void TestCapOverflow()
{
    CE::Recorder r;
    CHECK(r.Begin(100, 1, 0));
    // Fill the fixed buffer: Begin (1) + 127 decisions = 128 events.
    for (uint32_t i = 0; i < CE::Recorder::kMaxEvents - 1; ++i)
        r.RecordDecision(1, false);
    CHECK(r.GetEventCount() == CE::Recorder::kMaxEvents);
    CHECK(!r.Overflowed());
    // The next event fails closed: dropped, overflow latched.
    r.RecordDecision(1, false);
    CHECK(r.Overflowed());
    CHECK(r.GetEventCount() == CE::Recorder::kMaxEvents);
    // Metrics stay accurate even after the event buffer is exhausted.
    r.Tick(100);
    r.RecordDamageDealt(100, 25, 772, false, false);
    // End: available but incomplete; the End event is dropped (full).
    CHECK(r.End(CE::EndReason::TargetDeath));
    CE::Recorder::Summary const s = r.GetSummary();
    CHECK(s.overflow);
    CHECK(!s.complete);
    CHECK(!s.efficacyEligible);
    CHECK(s.endReason == CE::EndReason::TargetDeath);
    CHECK(s.effectiveDamage == 25);
    CHECK(s.decisions == 128); // all decisions counted, last event dropped
    CHECK(s.eventCount == CE::Recorder::kMaxEvents);
    // A fresh begin after overflow resets the latch and the buffer.
    CHECK(r.Begin(300, 1, 0));
    CHECK(!r.Overflowed());
    CHECK(r.GetEventCount() == 1);

    // Death path with a full buffer: the terminal transition still lands.
    CE::Recorder r2;
    CHECK(r2.Begin(100, 1, 0));
    for (uint32_t i = 0; i < CE::Recorder::kMaxEvents; ++i)
        r2.RecordDamageDealt(100, 1, 0, false, false); // last push dropped
    CHECK(r2.Overflowed());
    CHECK(r2.GetSummary().effectiveDamage == 128);
    r2.RecordDamageDealt(100, 1, 0, false, true); // killing hit: events dropped
    CHECK(r2.GetState() == CE::State::Complete);
    CE::Recorder::Summary const s2 = r2.GetSummary();
    CHECK(s2.endReason == CE::EndReason::TargetDeath);
    CHECK(s2.deaths == 1);
    CHECK(s2.effectiveDamage == 129);
    CHECK(s2.overflow);
    CHECK(!s2.efficacyEligible); // overflowed: fail-closed
}

static void TestZeroDuration()
{
    CE::Recorder r;
    CHECK(r.Begin(100, 1, 0));
    r.RecordDecision(10, false);
    CHECK(r.End(CE::EndReason::TargetDeath));
    CE::Recorder::Summary const s = r.GetSummary();
    CHECK(s.durationMs == 0);
    CHECK(s.effectiveDamage == 0);
    CHECK(s.damageTaken == 0);
    CHECK(s.deaths == 0);
    CHECK(s.decisions == 1);
    CHECK(s.complete);
    CHECK(s.efficacyEligible); // mechanical gate; zero baselines are the
                               // evaluator's inconclusive case, not this
                               // recorder's
    // Duration is the exact sum of the per-call ticks.
    CE::Recorder r2;
    CHECK(r2.Begin(100, 1, 0));
    r2.Tick(250);
    r2.Tick(0);
    r2.Tick(750);
    CHECK(r2.GetElapsedMs() == 1000);
    CHECK(r2.End(CE::EndReason::LeashExpired));
    CHECK(r2.GetSummary().durationMs == 1000);
    CHECK(!r2.GetSummary().efficacyEligible);
}

static void TestDuplicateSummaryEnd()
{
    CE::Recorder r;
    CHECK(r.Begin(100, 1, 0));
    r.Tick(1000);
    r.RecordDamageDealt(100, 12, 772, false, false);
    CHECK(r.End(CE::EndReason::TargetDeath));
    CE::Recorder::Summary const a = r.GetSummary();
    // Duplicate ends and duplicate summary reads: nothing changes.
    CHECK(!r.End(CE::EndReason::TargetDeath));
    CHECK(!r.End(CE::EndReason::OwnerOverride));
    CE::Recorder::Summary const b = r.GetSummary();
    CHECK(a.targetGuid == b.targetGuid);
    CHECK(a.durationMs == b.durationMs);
    CHECK(a.effectiveDamage == b.effectiveDamage);
    CHECK(a.periodicDamage == b.periodicDamage);
    CHECK(a.damageTaken == b.damageTaken);
    CHECK(a.deaths == b.deaths);
    CHECK(a.ownerOverrides == b.ownerOverrides);
    CHECK(a.decisions == b.decisions);
    CHECK(a.castsAccepted == b.castsAccepted);
    CHECK(a.castsRejected == b.castsRejected);
    CHECK(a.castsNoEligible == b.castsNoEligible);
    CHECK(a.eventCount == b.eventCount);
    CHECK(a.endReason == b.endReason);
    CHECK(a.complete == b.complete);
    CHECK(a.overflow == b.overflow);
    CHECK(a.efficacyEligible == b.efficacyEligible);
    CHECK(r.GetEventCount() == a.eventCount);
}

static void TestEfficacyGate()
{
    CE::Recorder r;
    auto finish = [&r](CE::EndReason reason) {
        CHECK(r.Begin(1, 0, 0));
        CHECK(r.End(reason));
        return r.GetSummary();
    };
    CHECK(finish(CE::EndReason::TargetDeath).efficacyEligible);
    CHECK(!finish(CE::EndReason::CompanionDeath).efficacyEligible);
    CHECK(!finish(CE::EndReason::TargetChanged).efficacyEligible);
    CHECK(!finish(CE::EndReason::TargetInvalid).efficacyEligible);
    CHECK(!finish(CE::EndReason::LeashExpired).efficacyEligible);
    CHECK(!finish(CE::EndReason::OwnerOverride).efficacyEligible);
    // Interrupted encounters still report their safety metrics.
    CE::Recorder s;
    CHECK(s.Begin(100, 1, 0));
    s.RecordDamageDealt(100, 15, 0, false, false);
    s.RecordDamageTaken(4, 0, false, false);
    CHECK(s.End(CE::EndReason::LeashExpired));
    CE::Recorder::Summary const g = s.GetSummary();
    CHECK(g.effectiveDamage == 15);
    CHECK(g.damageTaken == 4);
    CHECK(g.complete);
    CHECK(!g.efficacyEligible);
}

static void TestNames()
{
    CHECK(std::string(CE::StateName(CE::State::Active)) == "active");
    CHECK(std::string(CE::EventKindName(CE::EventKind::DamageTaken)) == "damage-taken");
    CHECK(std::string(CE::EndReasonName(CE::EndReason::LeashExpired)) == "leash-expired");
    CHECK(std::string(CE::CastOutcomeName(CE::CastOutcome::NoEligibleAbility)) == "no-eligible-ability");
}

int main()
{
    TestStateBoundaries();
    TestDamageAttribution();
    TestTargetDeath();
    TestCompanionDeath();
    TestOwnerOverride();
    TestNewTarget();
    TestDecisionAndCast();
    TestCapOverflow();
    TestZeroDuration();
    TestDuplicateSummaryEnd();
    TestEfficacyGate();
    TestNames();
    if (g_failures != 0)
    {
        std::printf("value tests: %d FAILURES\n", g_failures);
        return 1;
    }
    std::printf("value tests: ALL OK\n");
    return 0;
}