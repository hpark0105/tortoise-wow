// PORT-012 (KAP-558): value-level coverage for the value-only combat module
// (Companion/Combat.h). Standalone: no engine, no database, no Docker.
// Compiled and run by docker/test_companion_combat_value.py.
#include "Companion/Combat.h"

#include <cstdio>
#include <string>
#include <vector>

namespace CC = Companion::Combat;

static int g_failures = 0;
#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            std::printf("FAIL %d: %s\n", __LINE__, #cond); \
            ++g_failures; \
        } \
    } while (0)

static CC::Request Req(CC::Source source, float maxDistance = 35.0f)
{
    CC::Request r;
    r.target = 1234;
    r.source = source;
    r.generation = 7;
    r.maxDistance = maxDistance;
    return r;
}

static CC::TargetSnapshot BaseSnap()
{
    CC::TargetSnapshot s;
    s.exists = true;
    s.alive = true;
    s.inWorld = true;
    s.canAttack = true;
    s.friendly = false;
    s.inLos = true;
    s.distance = 5.0f;
    return s;
}

static void TestVerifyAssist()
{
    CC::Request r = Req(CC::Source::Assist);
    CC::TargetSnapshot s = BaseSnap();
    CHECK(CC::Verify(r, s).legal);
    CHECK(CC::Verify(r, s).reject == CC::Reject::None);

    s = BaseSnap(); s.exists = false;
    CHECK(CC::Verify(r, s).reject == CC::Reject::Missing);
    s = BaseSnap(); s.alive = false;
    CHECK(CC::Verify(r, s).reject == CC::Reject::Dead);
    s = BaseSnap(); s.inWorld = false;
    CHECK(CC::Verify(r, s).reject == CC::Reject::NotInWorld);
    s = BaseSnap(); s.canAttack = false;
    CHECK(CC::Verify(r, s).reject == CC::Reject::CannotAttack);
    s = BaseSnap(); s.friendly = true;
    CHECK(CC::Verify(r, s).reject == CC::Reject::Friendly);
    s = BaseSnap(); s.inLos = false;
    CHECK(CC::Verify(r, s).reject == CC::Reject::NoLos);
    s = BaseSnap(); s.distance = 35.1f;
    CHECK(CC::Verify(r, s).reject == CC::Reject::OutOfRange);
    s = BaseSnap(); s.distance = 35.0f; // at the limit is legal
    CHECK(CC::Verify(r, s).legal);
}

static void TestVerifyContinueCombat()
{
    CC::Request r = Req(CC::Source::ContinueCombat);
    CC::TargetSnapshot s = BaseSnap();
    s.isVictim = true;
    CHECK(CC::Verify(r, s).legal);

    // No LOS rule: an already-live fight stays legal without sight.
    s = BaseSnap(); s.isVictim = true; s.inLos = false;
    CHECK(CC::Verify(r, s).legal);

    s = BaseSnap(); s.exists = false;
    CHECK(CC::Verify(r, s).reject == CC::Reject::Missing);
    s = BaseSnap(); s.alive = false;
    CHECK(CC::Verify(r, s).reject == CC::Reject::Dead);
    s = BaseSnap(); s.inWorld = false;
    CHECK(CC::Verify(r, s).reject == CC::Reject::NotInWorld);
    s = BaseSnap(); s.canAttack = false; s.isVictim = true;
    CHECK(CC::Verify(r, s).reject == CC::Reject::CannotAttack);
    s = BaseSnap(); s.friendly = true; s.isVictim = true;
    CHECK(CC::Verify(r, s).reject == CC::Reject::Friendly);
    s = BaseSnap(); s.isVictim = true; s.distance = 40.0f;
    CHECK(CC::Verify(r, s).reject == CC::Reject::OutOfRange);
    s = BaseSnap(); // neither victim nor held
    CHECK(CC::Verify(r, s).reject == CC::Reject::NotCurrentTarget);
    s = BaseSnap(); s.isHeld = true; // held alone is enough
    CHECK(CC::Verify(r, s).legal);
    s = BaseSnap(); s.isVictim = false; s.isHeld = false;
    CHECK(CC::Verify(r, s).reject == CC::Reject::NotCurrentTarget);
}

static void TestVerifyDefend()
{
    CC::Request r = Req(CC::Source::Defend);
    CC::TargetSnapshot s = BaseSnap();
    CHECK(CC::Verify(r, s).legal); // canAttack path

    // Targetable with live attack evidence.
    s = BaseSnap(); s.canAttack = false; s.targetable = true; s.victimIsProtected = true;
    CHECK(CC::Verify(r, s).legal);
    s = BaseSnap(); s.canAttack = false; s.targetable = true; s.attackingMe = true;
    CHECK(CC::Verify(r, s).legal);
    s = BaseSnap(); s.canAttack = false; s.targetable = true; s.attackingOwner = true;
    CHECK(CC::Verify(r, s).legal);

    // No evidence: bystanders are not defend targets.
    s = BaseSnap(); s.canAttack = false; s.targetable = true;
    CHECK(CC::Verify(r, s).reject == CC::Reject::NotDefendTarget);
    s = BaseSnap(); s.canAttack = false;
    CHECK(CC::Verify(r, s).reject == CC::Reject::NotDefendTarget);
    s = BaseSnap(); s.friendly = true;
    CHECK(CC::Verify(r, s).reject == CC::Reject::NotDefendTarget);

    s = BaseSnap(); s.exists = false;
    CHECK(CC::Verify(r, s).reject == CC::Reject::Missing);
    s = BaseSnap(); s.alive = false;
    CHECK(CC::Verify(r, s).reject == CC::Reject::Dead);
    s = BaseSnap(); s.inWorld = false;
    CHECK(CC::Verify(r, s).reject == CC::Reject::NotInWorld);
    s = BaseSnap(); s.inLos = false;
    CHECK(CC::Verify(r, s).reject == CC::Reject::NoLos);
    s = BaseSnap(); s.distance = 36.0f;
    CHECK(CC::Verify(r, s).reject == CC::Reject::OutOfRange);
}

static void TestDefendTargetLegal()
{
    CC::TargetSnapshot s; // all false: no evidence
    CHECK(!CC::DefendTargetLegal(s));
    s.canAttack = true;
    CHECK(CC::DefendTargetLegal(s));
    s = CC::TargetSnapshot(); s.friendly = true; s.canAttack = true;
    CHECK(!CC::DefendTargetLegal(s)); // friendly wins first
    s = CC::TargetSnapshot(); s.canAttack = false; s.targetable = true;
    CHECK(!CC::DefendTargetLegal(s));
    s.victimIsProtected = true;
    CHECK(CC::DefendTargetLegal(s));
    s = CC::TargetSnapshot(); s.canAttack = false; s.targetable = true; s.attackingOwner = true;
    CHECK(CC::DefendTargetLegal(s));
}

static void TestLeash()
{
    CC::Leash leash;
    CHECK(!leash.Armed());
    CHECK(leash.Tick(false, 100) == CC::Leash::Step::Armed);
    CHECK(leash.Armed());
    CHECK(leash.remainingMs == CC::Leash::kArmedMs);
    CHECK(leash.Tick(false, 1000) == CC::Leash::Step::Continue);
    CHECK(leash.remainingMs == CC::Leash::kArmedMs - 1000);
    CHECK(leash.Tick(true, 100) == CC::Leash::Step::Disarmed); // reach disarms
    CHECK(!leash.Armed());
    CHECK(leash.Tick(false, 100) == CC::Leash::Step::Armed); // fresh budget
    CHECK(leash.Tick(false, CC::Leash::kArmedMs) == CC::Leash::Step::Expired);
    CHECK(!leash.Armed());
    CC::Leash fresh;
    fresh.Disarm();
    CHECK(fresh.Tick(false, 1) == CC::Leash::Step::Armed);
    CHECK(fresh.Tick(false, fresh.remainingMs + 1) == CC::Leash::Step::Expired);
}

static void TestTargetSlots()
{
    using T = CC::TargetSlots::Transition;
    CC::TargetSlots t;
    CHECK(t.Empty());
    CHECK(t.AcquireLive(0) == T::None); // zero guard
    CHECK(t.live == 0);
    CHECK(t.AcquireLive(5) == T::LiveAcquired);
    CHECK(t.AcquireLive(5) == T::None); // idempotent
    CHECK(t.OnLiveResolved(true, true) == T::None);
    CHECK(t.OnLiveResolved(true, false) == T::LiveToCorpse);
    CHECK(t.live == 0 && t.corpse == 5);

    // Re-embodied corpse returns to the live slot.
    CHECK(t.OnCorpseResolved(true, true) == T::CorpseToLive);
    CHECK(t.live == 5 && t.corpse == 0);
    CHECK(t.OnCorpseResolved(true, false) == T::None); // corpse empty
    CHECK(t.OnLiveResolved(false, false) == T::LiveCleared);
    CHECK(t.Empty());

    // Release boundaries: live release keeps the corpse and vice versa.
    CC::TargetSlots both;
    both.AcquireLive(9);
    both.OnLiveResolved(true, false);
    both.AcquireLive(10);
    CHECK(both.live == 10 && both.corpse == 9);
    CHECK(both.ReleaseLive() == T::LiveCleared);
    CHECK(both.live == 0 && both.corpse == 9);
    CHECK(both.ReleaseCorpse() == T::CorpseCleared);
    CHECK(both.Empty());
    CHECK(both.ReleaseLive() == T::None);
    CHECK(both.ReleaseCorpse() == T::None);
    CHECK(both.ClearAll() == T::None);
    CC::TargetSlots full;
    full.AcquireLive(1);
    full.OnLiveResolved(true, false);
    full.AcquireLive(2);
    CHECK(full.ClearAll() == T::AllCleared);
    CHECK(full.Empty());
    // Vanished corpse clears only the corpse slot.
    CC::TargetSlots ghost;
    ghost.AcquireLive(1);
    ghost.OnLiveResolved(true, false);
    ghost.AcquireLive(2);
    CHECK(ghost.OnCorpseResolved(false, false) == T::CorpseCleared);
    CHECK(ghost.live == 2 && ghost.corpse == 0);
}

static CC::AbilityProfile Prof(uint32_t id, bool known = true)
{
    CC::AbilityProfile p;
    p.id = id;
    p.known = known;
    return p;
}

static void TestSelectExecutable()
{
    CC::AbilityQuery q;
    q.distance = 5.0f;
    q.meleeReach = true;

    CC::AbilityProfile table[4];
    table[0] = Prof(100);
    table[1] = Prof(200);
    table[2] = Prof(300);
    table[3] = Prof(400);
    CC::AbilityDecision d = CC::SelectExecutable(table, q);
    CHECK(d.selected == 100); // table order first wins
    CHECK(d.anyKnown);
    CHECK(d.firstBlock == CC::Block::None);

    // Unknown profiles are skipped entirely.
    CC::AbilityProfile unknowns[2] = {Prof(10, false), Prof(20, false)};
    d = CC::SelectExecutable(unknowns, q);
    CHECK(d.selected == 0);
    CHECK(!d.anyKnown);
    CHECK(d.firstBlock == CC::Block::None);

    // One blocker per block; the blocked profile is never selected and the
    // first blocker is recorded.
    CC::AbilityProfile single[1];
    single[0] = Prof(1); single[0].obsoleteRank = true;
    CHECK(CC::SelectExecutable(single, q).firstBlock == CC::Block::ObsoleteRank);
    single[0] = Prof(1); single[0].passive = true;
    CHECK(CC::SelectExecutable(single, q).firstBlock == CC::Block::Passive);
    single[0] = Prof(1); single[0].onNextSwing = true;
    CC::AbilityDecision ons = CC::SelectExecutable(single, q);
    CHECK(ons.selected == 0); // explicitly unsupported is never cast
    CHECK(ons.firstBlock == CC::Block::OnNextSwing);
    single[0] = Prof(1); single[0].powerType = 0; single[0].powerCost = 50;
    q.power[0] = 49;
    CHECK(CC::SelectExecutable(single, q).firstBlock == CC::Block::Unaffordable);
    q.power[0] = 50;
    CHECK(CC::SelectExecutable(single, q).selected == 1);
    // Power index beyond the table is treated as costless.
    single[0] = Prof(1); single[0].powerType = CC::AbilityQuery::kPowerSlots; single[0].powerCost = 1;
    CHECK(CC::SelectExecutable(single, q).selected == 1);
    single[0] = Prof(1); single[0].onCooldown = true;
    CHECK(CC::SelectExecutable(single, q).firstBlock == CC::Block::Cooldown);
    single[0] = Prof(1); single[0] = CC::AbilityProfile(); single[0].id = 1; single[0].known = true;
    single[0].stanceOk = false;
    CHECK(CC::SelectExecutable(single, q).firstBlock == CC::Block::StanceForm);
    single[0] = Prof(1); single[0].reagentOk = false;
    CHECK(CC::SelectExecutable(single, q).firstBlock == CC::Block::Reagent);
    single[0] = Prof(1); single[0].targetOk = false;
    CHECK(CC::SelectExecutable(single, q).firstBlock == CC::Block::TargetInappropriate);
    single[0] = Prof(1); single[0].minRange = 6.0f;
    CHECK(CC::SelectExecutable(single, q).firstBlock == CC::Block::OutOfRange);
    single[0] = Prof(1); single[0].maxRange = 4.0f;
    CHECK(CC::SelectExecutable(single, q).firstBlock == CC::Block::OutOfRange);
    single[0] = Prof(1); single[0].meleeOnly = true;
    q.meleeReach = false;
    CHECK(CC::SelectExecutable(single, q).firstBlock == CC::Block::OutOfRange);
    q.meleeReach = true;
    CHECK(CC::SelectExecutable(single, q).selected == 1);
    // Zero ranges impose no bound.
    single[0] = Prof(1); single[0].minRange = 0.0f; single[0].maxRange = 0.0f;
    CHECK(CC::SelectExecutable(single, q).selected == 1);

    // Blocked-then-clean: first blocker wins, clean cast still selectable.
    CC::AbilityProfile mixed[2];
    mixed[0] = Prof(1); mixed[0].onCooldown = true;
    mixed[1] = Prof(2);
    d = CC::SelectExecutable(mixed, q);
    CHECK(d.selected == 2);
    CHECK(d.firstBlock == CC::Block::Cooldown);
    CHECK(d.anyKnown);
}


static void TestRotationPlanBaseline()
{
    CC::AbilityQuery q;
    q.distance = 5.0f;
    q.meleeReach = true;

    std::vector<CC::AbilityProfile> profiles;
    profiles.push_back(Prof(100));
    profiles.push_back(Prof(200));
    profiles.push_back(Prof(300));

    // Baseline plan (identity) must produce the same result as no plan.
    CC::AbilityDecision d0 = CC::SelectExecutable(profiles, q);
    CC::RotationPlan baseline = CC::RotationPlan::Baseline(3);
    CC::AbilityDecision d1 = CC::SelectExecutable(profiles, q, baseline);
    CHECK(d0.selected == d1.selected);
    CHECK(d0.anyKnown == d1.anyKnown);
    CHECK(d0.firstBlock == d1.firstBlock);
    CHECK(d1.selected == 100); // first in table order
}

static void TestRotationPlanReordered()
{
    CC::AbilityQuery q;
    q.distance = 5.0f;
    q.meleeReach = true;

    std::vector<CC::AbilityProfile> profiles;
    profiles.push_back(Prof(100));
    profiles.push_back(Prof(200));
    profiles.push_back(Prof(300));

    // Plan that reverses the order: scan index 2 first.
    CC::RotationPlan reversed{2, 1, 0};
    CHECK(reversed.CompatibleWith(3));
    CC::AbilityDecision d = CC::SelectExecutable(profiles, q, reversed);
    CHECK(d.selected == 300); // profile at index 2 is scanned first

    // Plan that puts the second profile first.
    CC::RotationPlan secondFirst{1, 0, 2};
    CHECK(secondFirst.CompatibleWith(3));
    d = CC::SelectExecutable(profiles, q, secondFirst);
    CHECK(d.selected == 200); // profile at index 1 is scanned first
}

static void TestRotationPlanNullFallback()
{
    CC::AbilityQuery q;
    q.distance = 5.0f;
    q.meleeReach = true;

    std::vector<CC::AbilityProfile> profiles;
    profiles.push_back(Prof(100));
    profiles.push_back(Prof(200));

    // Default-constructed (empty) plan: incompatible, falls back to table order.
    CC::RotationPlan null_plan;
    CHECK(!null_plan.CompatibleWith(2));
    CC::AbilityDecision d = CC::SelectExecutable(profiles, q, null_plan);
    CHECK(d.selected == 100); // table order first

    // Wrong-size plan: incompatible, falls back.
    CC::RotationPlan wrongSize{0, 1, 2}; // 3 indices for 2 profiles
    CHECK(!wrongSize.CompatibleWith(2));
    d = CC::SelectExecutable(profiles, q, wrongSize);
    CHECK(d.selected == 100);
}

static void TestRotationPlanIncompatible()
{
    CC::AbilityQuery q;
    q.distance = 5.0f;
    q.meleeReach = true;

    std::vector<CC::AbilityProfile> profiles;
    profiles.push_back(Prof(100));
    profiles.push_back(Prof(200));
    profiles.push_back(Prof(300));

    // Duplicate index: not a permutation, incompatible.
    CC::RotationPlan dup{0, 0, 1};
    CHECK(!dup.CompatibleWith(3));
    CC::AbilityDecision d = CC::SelectExecutable(profiles, q, dup);
    CHECK(d.selected == 100); // falls back to table order

    // Out-of-range index: incompatible.
    CC::RotationPlan oob{0, 1, 3}; // index 3 is out of range for 3 profiles
    CHECK(!oob.CompatibleWith(3));
    d = CC::SelectExecutable(profiles, q, oob);
    CHECK(d.selected == 100);

    // Negative index: incompatible.
    CC::RotationPlan neg{0, -1, 1};
    CHECK(!neg.CompatibleWith(3));
    d = CC::SelectExecutable(profiles, q, neg);
    CHECK(d.selected == 100);

    // Oversized input must fail closed; it cannot be silently truncated into
    // an acceptable plan for an eight-profile catalog.
    CC::RotationPlan oversized{0, 1, 2, 3, 4, 5, 6, 7, 0};
    CHECK(!oversized.CompatibleWith(8));
}

static void TestRotationPlanNoLearning()
{
    // The plan holds indices only: it cannot name spell IDs, targets,
    // coordinates, or conditions. Verify the struct exposes no such fields
    // by checking that a reordered plan with the same profiles produces
    // the same set of selectable IDs (only order changes).
    CC::AbilityQuery q;
    q.distance = 5.0f;
    q.meleeReach = true;

    std::vector<CC::AbilityProfile> profiles;
    profiles.push_back(Prof(100));
    profiles.push_back(Prof(200));
    profiles.push_back(Prof(300));
    profiles.push_back(Prof(400));

    // With all profiles unblocked, every permutation selects some profile
    // from the same set; the selected ID is always one of the profile IDs.
    CC::RotationPlan p{3, 1, 0, 2};
    CHECK(p.CompatibleWith(4));
    CC::AbilityDecision d = CC::SelectExecutable(profiles, q, p);
    bool found = false;
    for (auto const& prof : profiles)
        if (prof.id == d.selected)
            found = true;
    CHECK(found);
    CHECK(d.selected == 400); // index 3 scanned first
}

static void TestRotationPlanCurrentClassBaselines()
{
    CC::AbilityQuery q;
    q.distance = 5.0f;
    q.meleeReach = true;

    // Pin the identity plan to the live warrior and mage action-table ordering
    // in PlayerBotAI::SelectOffensiveSpell.
    std::vector<CC::AbilityProfile> warrior;
    warrior.push_back(Prof(100));  // Charge
    warrior.push_back(Prof(7372)); // Hamstring
    warrior.push_back(Prof(772));  // Rend
    warrior.push_back(Prof(78));   // Heroic Strike
    CHECK(CC::SelectExecutable(warrior, q).selected == 100);
    CHECK(CC::SelectExecutable(warrior, q, CC::RotationPlan::Baseline(4)).selected == 100);

    std::vector<CC::AbilityProfile> mage;
    mage.push_back(Prof(116));  // Frostbolt
    mage.push_back(Prof(133));  // Fireball
    mage.push_back(Prof(2136)); // Fire Blast
    mage.push_back(Prof(122));  // Frost Nova
    mage.push_back(Prof(1449)); // Arcane Explosion
    CHECK(CC::SelectExecutable(mage, q).selected == 116);
    CHECK(CC::SelectExecutable(mage, q, CC::RotationPlan::Baseline(5)).selected == 116);
}

static void TestCastReport()
{
    static_assert(CC::kCastOk == 0xFF, "SPELL_CAST_OK must stay 0xFF");
    CC::CastReport ok = CC::ReportCast(77, CC::kCastOk, CC::CastReject::None);
    CHECK(ok.outcome == CC::CastOutcome::Accepted);
    CHECK(ok.spellId == 77);
    CHECK(ok.reject == CC::CastReject::None);
    CHECK(!ok.meleeFallback);

    CC::CastReport rej = CC::ReportCast(77, 3, CC::CastReject::InsufficientPower);
    CHECK(rej.outcome == CC::CastOutcome::Rejected);
    CHECK(rej.reject == CC::CastReject::InsufficientPower);
    CHECK(rej.meleeFallback);
    CHECK(rej.rawResult == 3);

    CC::CastReport none = CC::ReportNoEligible();
    CHECK(none.outcome == CC::CastOutcome::NoEligibleAbility);
    CHECK(none.spellId == 0);
    CHECK(none.meleeFallback);
    CHECK(none.reject == CC::CastReject::None);

    CHECK(std::string(CC::CastRejectName(CC::CastReject::RangeLos)) == "range-los");
    CHECK(std::string(CC::RejectName(CC::Reject::NotCurrentTarget)) == "not-current-target");
    CHECK(std::string(CC::BlockName(CC::Block::OnNextSwing)) == "on-next-swing");
}

static void TestVerifyDamage()
{
    // PORT-016: the damage source shares the baseline legality and
    // adds the pull-discipline gates (crowd-control preservation and
    // the established tank target) before the shared LOS/range tail.
    CC::Request r = Req(CC::Source::Damage);

    CC::TargetSnapshot s = BaseSnap();
    s.establishedTarget = true;
    s.targetUnderCC = false;
    CHECK(CC::Verify(r, s).legal);
    CHECK(CC::Verify(r, s).reject == CC::Reject::None);

    s = BaseSnap(); s.establishedTarget = true; s.targetUnderCC = false;
    s.canAttack = false;
    CHECK(!CC::Verify(r, s).legal);
    CHECK(CC::Verify(r, s).reject == CC::Reject::CannotAttack);

    s = BaseSnap(); s.establishedTarget = true; s.targetUnderCC = false;
    s.friendly = true;
    CHECK(CC::Verify(r, s).reject == CC::Reject::Friendly);

    s = BaseSnap(); s.establishedTarget = true; s.targetUnderCC = true;
    CHECK(CC::Verify(r, s).reject == CC::Reject::TargetUnderCC);

    s = BaseSnap(); s.establishedTarget = false; s.targetUnderCC = false;
    CHECK(CC::Verify(r, s).reject == CC::Reject::NotEstablishedTarget);

    s = BaseSnap(); s.establishedTarget = true; s.targetUnderCC = false;
    s.inLos = false;
    CHECK(CC::Verify(r, s).reject == CC::Reject::NoLos);

    s = BaseSnap(); s.establishedTarget = true; s.targetUnderCC = false;
    s.distance = 100.0f;
    CHECK(CC::Verify(r, s).reject == CC::Reject::OutOfRange);

    CHECK(std::string(CC::RejectName(CC::Reject::NotEstablishedTarget)) == "not-established-target");
    CHECK(std::string(CC::RejectName(CC::Reject::TargetUnderCC)) == "target-under-cc");
}

static void TestOffenseRoute()
{
    // BL-001B: the routing matrix. The Tank route is the Assist +
    // declared-tank row only; every other source (or an Assist without
    // the gate) uses the ordinary rotation selector.
    CHECK(CC::RouteOffense(CC::Source::Assist, true) == CC::OffenseRoute::Tank);
    CHECK(CC::RouteOffense(CC::Source::Assist, false) == CC::OffenseRoute::Rotation);
    CHECK(CC::RouteOffense(CC::Source::ContinueCombat, true) == CC::OffenseRoute::Rotation);
    CHECK(CC::RouteOffense(CC::Source::ContinueCombat, false) == CC::OffenseRoute::Rotation);
    CHECK(CC::RouteOffense(CC::Source::Defend, true) == CC::OffenseRoute::Rotation);
    CHECK(CC::RouteOffense(CC::Source::Defend, false) == CC::OffenseRoute::Rotation);
    CHECK(CC::RouteOffense(CC::Source::Damage, true) == CC::OffenseRoute::Rotation);
    CHECK(CC::RouteOffense(CC::Source::Damage, false) == CC::OffenseRoute::Rotation);

    // Stable name/value for applicability diagnostics.
    CHECK(std::string(CC::OffenseRouteName(CC::OffenseRoute::Tank)) == "tank");
    CHECK(std::string(CC::OffenseRouteName(CC::OffenseRoute::Rotation)) == "rotation");
}

static void TestWarriorPlaybookAdapter()
{
    uint32_t catalog[] = {355, 772, 676, 78};
    uint32_t candidate[] = {355, 772, 78, 676};
    auto p = CC::WarriorPlaybookAdapter::Adapt(candidate, 4, 7, 7, catalog, 4);
    CHECK(p.CompatibleWith(4));
    CHECK(p.order[0] == 0 && p.order[1] == 1 && p.order[2] == 3 && p.order[3] == 2);
    uint32_t duplicate[] = {355, 772, 772, 78};
    auto fallback = CC::WarriorPlaybookAdapter::Adapt(duplicate, 4, 7, 7, catalog, 4);
    CHECK(fallback.count == 4 && fallback.order[0] == 0 && fallback.order[1] == 1 && fallback.order[2] == 2 && fallback.order[3] == 3);
    auto stale = CC::WarriorPlaybookAdapter::Adapt(candidate, 4, 6, 7, catalog, 4);
    CHECK(stale.order[0] == 0 && stale.order[1] == 1 && stale.order[2] == 2 && stale.order[3] == 3);
}

int main()
{
    TestVerifyAssist();
    TestVerifyDamage();
    TestVerifyContinueCombat();
    TestVerifyDefend();
    TestDefendTargetLegal();
    TestLeash();
    TestTargetSlots();
    TestSelectExecutable();
    TestRotationPlanBaseline();
    TestRotationPlanReordered();
    TestRotationPlanNullFallback();
    TestRotationPlanIncompatible();
    TestRotationPlanNoLearning();
    TestRotationPlanCurrentClassBaselines();
    TestCastReport();
    TestOffenseRoute();
    TestWarriorPlaybookAdapter();
    if (g_failures != 0)
    {
        std::printf("value tests: %d FAILURES\n", g_failures);
        return 1;
    }
    std::printf("value tests: ALL OK\n");
    return 0;
}
