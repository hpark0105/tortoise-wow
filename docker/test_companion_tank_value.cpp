// PORT-014 (KAP-558): value-level coverage for the declared tank threat
// policy (Companion/Tank.h). Standalone: no engine, no Docker. Compiled
// and run by docker/test_companion_tank_value.py.
#include "Companion/Tank.h"

#include <cstdio>
#include <type_traits>

namespace CT = Companion::Tank;

static int g_failures = 0;
#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            std::printf("FAIL %d: %s\n", __LINE__, #cond); \
            ++g_failures; \
        } \
    } while (0)

// Compile-time pins: the contract is a bounded value type only.
static_assert(std::is_trivially_copyable<CT::Observation>::value &&
              std::is_standard_layout<CT::Observation>::value,
              "tank observation must be a value type");

static CT::Observation Base()
{
    CT::Observation o;
    o.tankGuid = 11;
    o.ownerGuid = 22;
    o.targetGuid = 33;
    o.targetHasThreatList = true;
    o.tauntUsable = true;
    return o;
}

static void TestDeclaredMatrix()
{
    CHECK(CT::kDeclaredTankClass == 1);       // CLASS_WARRIOR
    CHECK(CT::kDeclaredTankMinLevel == 10);   // Taunt 355 baseLevel
    CHECK(CT::kDeclaredTankTaunt == 355);     // the only true taunt
    CHECK(CT::kDeclaredTankPullCap == 2);     // bounded two-target pull
    CHECK(!CT::kTauntOnNextSwing);            // declared: no on-next-swing
    CHECK(CT::kSwitchMarginNum == 11 && CT::kSwitchMarginDen == 10);
    CHECK(CT::kTankVersion == 1);
}

static void TestIncompleteEvidence()
{
    CT::Observation o = Base();
    o.targetGuid = 0;
    CHECK(CT::SelectAction(o).action == CT::Action::NormalAttack);
    o = Base();
    o.tankGuid = 0;
    CHECK(CT::SelectAction(o).action == CT::Action::NormalAttack);
    o = Base();
    o.targetHasThreatList = false;
    CHECK(CT::SelectAction(o).action == CT::Action::NormalAttack);
    // No threat list: even an owner-victim state never taunts.
    o.victimGuid = o.ownerGuid;
    o.ownerThreat = 500;
    CHECK(CT::SelectAction(o).action == CT::Action::NormalAttack);
}

static void TestTankVictimNeverTaunts()
{
    // EffectTaunt is a no-op rejection while the tank is the victim; the
    // policy must not even attempt it.
    CT::Observation o = Base();
    o.victimGuid = o.tankGuid;
    o.tankThreat = 100;
    o.ownerThreat = 500; // owner far above
    CHECK(CT::OwnerHoldsThreat(o));
    CHECK(CT::SelectAction(o).action == CT::Action::NormalAttack);
}

static void TestNoOwnerThreat()
{
    CT::Observation o = Base();
    o.tankThreat = 100;
    o.ownerThreat = 0;
    o.victimGuid = o.tankGuid; // tank holds it, owner has none
    CHECK(!CT::OwnerHoldsThreat(o));
    CHECK(CT::SelectAction(o).action == CT::Action::NormalAttack);
}

static void TestOwnerVictimTaunts()
{
    CT::Observation o = Base();
    o.victimGuid = o.ownerGuid; // the protected member is the victim
    o.tankThreat = 500;         // even with more stored threat than the owner
    o.ownerThreat = 1;
    CHECK(CT::OwnerHoldsThreat(o));
    CHECK(CT::SelectAction(o).action == CT::Action::Taunt);
}

static void TestSwitchMarginBoundary()
{
    // 1.1x victim-switch margin: at or above it the owner holds threat,
    // one step below it the tank still holds.
    CT::Observation o = Base();
    o.victimGuid = 0; // no victim paired yet: the margin is the evidence
    o.tankThreat = 100;
    o.ownerThreat = 110; // 110*10 == 100*11: exactly the margin
    CHECK(CT::OwnerHoldsThreat(o));
    CHECK(CT::SelectAction(o).action == CT::Action::Taunt);
    o.ownerThreat = 109; // 109*10 < 100*11: below the margin
    CHECK(!CT::OwnerHoldsThreat(o));
    CHECK(CT::SelectAction(o).action == CT::Action::NormalAttack);
    o.ownerThreat = 111;
    CHECK(CT::SelectAction(o).action == CT::Action::Taunt);
}

static void TestZeroTankThreat()
{
    // The tank owns nothing yet (fresh pull, or released from a hold): the
    // owner's any threat is above the margin and the taunt re-anchors.
    CT::Observation o = Base();
    o.tankThreat = 0;
    o.ownerThreat = 5;
    o.victimGuid = o.ownerGuid;
    CHECK(CT::SelectAction(o).action == CT::Action::Taunt);
    o.victimGuid = 0; // owner attacking but no victim paired yet
    CHECK(CT::OwnerHoldsThreat(o));
    CHECK(CT::SelectAction(o).action == CT::Action::Taunt);
}

static void TestUsabilityGate()
{
    // A not-usable taunt (cooldown, range, not learned - the adapter's
    // tauntUsable fact) degrades to the ordinary attack immediately: the
    // decision is the same evaluation, never a delayed retry.
    CT::Observation o = Base();
    o.victimGuid = o.ownerGuid;
    o.ownerThreat = 500;
    o.tauntUsable = false;
    CHECK(CT::OwnerHoldsThreat(o));
    CHECK(CT::SelectAction(o).action == CT::Action::NormalAttack);
}

static void TestDefaultObservation()
{
    CT::Observation o;
    CHECK(o.version == CT::kTankVersion);
    CHECK(o.tankGuid == 0 && o.ownerGuid == 0 && o.targetGuid == 0);
    CHECK(o.victimGuid == 0 && o.tankThreat == 0 && o.ownerThreat == 0);
    CHECK(!o.targetHasThreatList && !o.tauntUsable);
    CHECK(CT::SelectAction(o).action == CT::Action::NormalAttack);
}

int main()
{
    TestDeclaredMatrix();
    TestIncompleteEvidence();
    TestTankVictimNeverTaunts();
    TestNoOwnerThreat();
    TestOwnerVictimTaunts();
    TestSwitchMarginBoundary();
    TestZeroTankThreat();
    TestUsabilityGate();
    TestDefaultObservation();
    if (g_failures != 0)
    {
        std::printf("value tests: %d FAILURES\n", g_failures);
        return 1;
    }
    std::printf("value tests: ALL OK\n");
    return 0;
}
