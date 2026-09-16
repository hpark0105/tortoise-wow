// PORT-016 (KAP-558): value-level coverage for the declared damage
// policy (Companion/Damage.h). Standalone: no engine, no Docker.
// Compiled and run by docker/test_companion_damage_value.py.
#include "Companion/Damage.h"

#include <cstdio>
#include <type_traits>

namespace CD = Companion::Damage;

static int g_failures = 0;
#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            std::printf("FAIL %d: %s\n", __LINE__, #cond); \
            ++g_failures; \
        } \
    } while (0)

// Compile-time pin: the contract is a bounded value type only.
static_assert(std::is_trivially_copyable<CD::Observation>::value &&
              std::is_standard_layout<CD::Observation>::value,
              "damage observation must be a value type");

static CD::Observation Base()
{
    CD::Observation o;
    o.damageGuid = 41;
    o.tankGuid = 11;
    o.targetGuid = 33;
    o.targetHasThreatList = true;
    o.canAttack = true;
    o.inLos = true;
    o.ownerAvailable = true;
    o.distance = 5.0f;
    return o;
}

static void TestDeclaredMatrix()
{
    CHECK(CD::kDeclaredDamageClass == 4);         // CLASS_ROGUE
    CHECK(CD::kDeclaredDamageMinLevel == 1);      // 1752/2098 baseLevel
    CHECK(CD::kDeclaredDamageEviscerate == 2098);
    CHECK(CD::kDeclaredDamageSinisterStrike == 1752);
    CHECK(CD::kDeclaredDamageGarrote == 703);
    CHECK(!CD::kDamageOnNextSwing);               // declared: none
    CHECK(CD::kSwitchMarginNum == 11 && CD::kSwitchMarginDen == 10);
    CHECK(CD::kDamageVersion == 2);
    // 35 yd engagement cap (the shared executor enforces the same).
    CHECK(CD::kEngageDistance == 35.0f);
}

static void TestIncompleteEvidence()
{
    CD::Observation o = Base();
    o.tankGuid = 0; // no declared tank in the party
    CHECK(!CD::PullEstablished(o));
    CHECK(CD::Select(o).action == CD::Action::None);
    o = Base();
    o.targetGuid = 0;
    CHECK(CD::Select(o).action == CD::Action::None);
    o = Base();
    o.targetHasThreatList = false;
    CHECK(!CD::PullEstablished(o));
    CHECK(CD::Select(o).action == CD::Action::None);
    // No threat list: even a tank-victim state never engages.
    o.tankIsVictim = true;
    CHECK(CD::Select(o).action == CD::Action::None);
}

static void TestTankVictimEstablishesPull()
{
    // The victim state alone establishes the pull: no threat comparison
    // is consulted while the tank is the victim.
    CD::Observation o = Base();
    o.tankIsVictim = true;
    o.tankThreat = 0;
    o.victimThreat = 99999;
    CHECK(CD::PullEstablished(o));
    CHECK(CD::Select(o).action == CD::Action::Damage);
}

static void TestSwitchMarginBoundary()
{
    // 1.1x victim-switch margin (integer 11/10, the Tank.h convention):
    // at or above it the tank's pull is established, one step below it
    // the damage companion keeps waiting.
    CD::Observation o = Base();
    o.tankIsVictim = false;
    o.tankThreat = 110;
    o.victimThreat = 100; // 110*10 == 100*11: exactly the margin
    CHECK(CD::PullEstablished(o));
    CHECK(CD::Select(o).action == CD::Action::Damage);
    o.tankThreat = 109; // 109*10 < 100*11: below the margin
    CHECK(!CD::PullEstablished(o));
    CHECK(CD::Select(o).action == CD::Action::None);
    o.tankThreat = 111;
    CHECK(CD::PullEstablished(o));
    CHECK(CD::Select(o).action == CD::Action::Damage);
}

static void TestNoVictimThreatAlone()
{
    // No victim paired yet (a fresh pull): the tank's any stored threat
    // establishes the pull (nothing else can steal it); zero tank
    // threat never does.
    CD::Observation o = Base();
    o.tankIsVictim = false;
    o.tankThreat = 5;
    o.victimThreat = 0;
    CHECK(CD::PullEstablished(o));
    CHECK(CD::Select(o).action == CD::Action::Damage);
    o.tankThreat = 0;
    CHECK(!CD::PullEstablished(o));
    CHECK(CD::Select(o).action == CD::Action::None);
}

static void TestCrowdControlPreservation()
{
    // A controlled target is never engaged: the established pull alone
    // is not enough while a controlling aura holds. Once the CC clears,
    // the same facts engage.
    CD::Observation o = Base();
    o.tankIsVictim = true;
    o.targetUnderCC = true;
    CHECK(CD::PullEstablished(o));
    CHECK(CD::Select(o).action == CD::Action::None);
    o.targetUnderCC = false;
    CHECK(CD::Select(o).action == CD::Action::Damage);
}

static void TestSuppressionGates()
{
    CD::Observation o = Base();
    o.tankIsVictim = true;
    o.held = true;
    CHECK(CD::Select(o).action == CD::Action::None);
    o = Base();
    o.tankIsVictim = true;
    o.ownerAvailable = false;
    CHECK(CD::Select(o).action == CD::Action::None);
}

static void TestReachGate()
{
    CD::Observation o = Base();
    o.tankIsVictim = true;
    o.distance = 35.0f; // the cap is inclusive
    CHECK(CD::Select(o).action == CD::Action::Damage);
    o.distance = 35.01f;
    CHECK(CD::Select(o).action == CD::Action::None);
    o = Base();
    o.tankIsVictim = true;
    o.canAttack = false;
    CHECK(CD::Select(o).action == CD::Action::None);
    o = Base();
    o.tankIsVictim = true;
    o.inLos = false;
    CHECK(CD::Select(o).action == CD::Action::None);
}

static void TestDefaultObservation()
{
    CD::Observation o;
    CHECK(o.version == CD::kDamageVersion);
    CHECK(o.damageGuid == 0 && o.tankGuid == 0 && o.targetGuid == 0);
    CHECK(o.tankThreat == 0 && o.victimThreat == 0 && o.distance == 0.0f);
    CHECK(!o.targetHasThreatList && !o.tankIsVictim && !o.targetUnderCC);
    CHECK(!o.canAttack && !o.inLos && !o.held && !o.ownerAvailable);
    CHECK(!CD::PullEstablished(o));
    CHECK(CD::Select(o).action == CD::Action::None);
}

int main()
{
    TestDeclaredMatrix();
    TestIncompleteEvidence();
    TestTankVictimEstablishesPull();
    TestSwitchMarginBoundary();
    TestNoVictimThreatAlone();
    TestCrowdControlPreservation();
    TestSuppressionGates();
    TestReachGate();
    TestDefaultObservation();
    if (g_failures != 0)
    {
        std::printf("value tests: %d FAILURES\n", g_failures);
        return 1;
    }
    std::printf("value tests: ALL OK\n");
    return 0;
}
