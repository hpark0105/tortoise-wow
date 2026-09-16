// PORT-015 (KAP-558): value-level coverage for the declared healer triage
// policy (Companion/Healer.h). Standalone: no engine, no Docker. Compiled
// and run by docker/test_companion_healer_value.py.
#include "Companion/Healer.h"

#include <cstdio>
#include <type_traits>

namespace CH = Companion::Healer;

static int g_failures = 0;
#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            std::printf("FAIL %d: %s\n", __LINE__, #cond); \
            ++g_failures; \
        } \
    } while (0)

// Compile-time pins: the contract is a bounded value type only.
static_assert(std::is_trivially_copyable<CH::Observation>::value &&
              std::is_standard_layout<CH::Observation>::value,
              "healer observation must be a value type");
static_assert(std::is_trivially_copyable<CH::Slot>::value &&
              std::is_standard_layout<CH::Slot>::value,
              "healer slot must be a value type");
static_assert(CH::kMaxSlots == 5, "a 1.12 party is five");

static void TestDeclaredMatrix()
{
    CHECK(CH::kDeclaredHealerClass == 11);   // CLASS_DRUID
    CHECK(CH::kDeclaredHealerMinLevel == 12); // Regrowth 8936 baseLevel
    CHECK(CH::kDeclaredHealerHeal == 8936);   // the declared heal
    CHECK(CH::kDeclaredHealCost == 96);       // 8936 manaCost
    CHECK(CH::kDeclaredHealRangeYd == 40);    // rangeIndex 5: 0-40 yd
    CHECK(CH::kDeclaredHealMinPerMille == 700); // heal below 70%
    CHECK(CH::kDeclaredManaReserve == 32);
    CHECK(CH::kHealerVersion == 1);
}

static CH::Observation Base()
{
    CH::Observation o;
    o.canCast = true;
    o.mana = 1000;
    o.maxMana = 1000;
    // slot 0: self, healthy
    o.slots[0].guid = 11;
    o.slots[0].hp = 400;
    o.slots[0].maxHp = 400;
    o.slots[0].alive = true;
    o.slots[0].inLos = true;
    o.slots[0].distance = 0.0f;
    o.slots[0].isSelf = true;
    // slot 1: owner, healthy
    o.slots[1].guid = 22;
    o.slots[1].hp = 1500;
    o.slots[1].maxHp = 1500;
    o.slots[1].alive = true;
    o.slots[1].inLos = true;
    o.slots[1].distance = 5.0f;
    o.slots[1].isOwner = true;
    return o;
}

static void TestNothingInjured()
{
    CH::Observation o = Base();
    CHECK(CH::Select(o).action == CH::Action::None);
    CHECK(CH::Select(o).target == 0);
}

static void TestThresholdBoundary()
{
    // 70% per-mille boundary: at or above 70% no heal, one point below it
    // the slot is injured.
    CH::Observation o = Base();
    o.slots[1].hp = (1500 * 700) / 1000; // exactly 70%
    CHECK(!CH::Injured(o.slots[1]));
    CHECK(CH::Select(o).action == CH::Action::None);
    o.slots[1].hp = (1500 * 700) / 1000 - 1; // one below
    CHECK(CH::Injured(o.slots[1]));
    CHECK(CH::Select(o).action == CH::Action::PartyHeal);
    CHECK(CH::Select(o).target == 22);
    // maxHp 0 (no evidence) is never injured even at hp 0.
    o.slots[1].maxHp = 0;
    CHECK(!CH::Injured(o.slots[1]));
    // a dead slot is never injured.
    o.slots[1].maxHp = 1500;
    o.slots[1].alive = false;
    CHECK(!CH::Injured(o.slots[1]));
    CHECK(CH::Select(o).action == CH::Action::None);
}

static void TestOwnerPriorityOverSelf()
{
    CH::Observation o = Base();
    o.slots[0].hp = 100;  // self very injured
    o.slots[1].hp = 900;  // owner injured (below 70% of 1500)
    CHECK(CH::Select(o).action == CH::Action::PartyHeal);
    CHECK(CH::Select(o).target == 22);
}

static void TestSelfWhenOwnerHealthy()
{
    CH::Observation o = Base();
    o.slots[0].hp = 100; // self injured, owner healthy
    CHECK(CH::Select(o).action == CH::Action::SelfHeal);
    CHECK(CH::Select(o).target == 0);
}

static void TestOtherMostInjuredFirst()
{
    CH::Observation o = Base();
    // two other party members, both injured: the lower health fraction
    // wins.
    o.slots[2].guid = 33;
    o.slots[2].hp = 300;
    o.slots[2].maxHp = 1000; // 30%
    o.slots[2].alive = true;
    o.slots[2].inLos = true;
    o.slots[2].distance = 10.0f;
    o.slots[3].guid = 44;
    o.slots[3].hp = 500;
    o.slots[3].maxHp = 1000; // 50%
    o.slots[3].alive = true;
    o.slots[3].inLos = true;
    o.slots[3].distance = 12.0f;
    CHECK(CH::Select(o).action == CH::Action::PartyHeal);
    CHECK(CH::Select(o).target == 33);
    // swap the injury fractions: the selection follows, not the slot.
    o.slots[2].hp = 500;
    o.slots[3].hp = 300;
    CHECK(CH::Select(o).target == 44);
}

static void TestOtherTieByLowerGuid()
{
    CH::Observation o = Base();
    o.slots[2].guid = 55;
    o.slots[2].hp = 400;
    o.slots[2].maxHp = 1000;
    o.slots[2].alive = true;
    o.slots[2].inLos = true;
    o.slots[2].distance = 9.0f;
    o.slots[3].guid = 44;
    o.slots[3].hp = 400;
    o.slots[3].maxHp = 1000;
    o.slots[3].alive = true;
    o.slots[3].inLos = true;
    o.slots[3].distance = 9.0f;
    CHECK(CH::Select(o).action == CH::Action::PartyHeal);
    CHECK(CH::Select(o).target == 44); // exact tie: lower GUID
}

static void TestOutOfRangeIneligible()
{
    CH::Observation o = Base();
    o.slots[1].hp = 500;                    // injured owner
    o.slots[1].distance = 39.0f;
    CHECK(CH::Select(o).target == 22);      // inside the 40 yd radius
    o.slots[1].distance = 40.0f;
    CHECK(CH::Select(o).action == CH::Action::None); // strictly inside only
    // no line of sight: ineligible even in range.
    o.slots[1].distance = 5.0f;
    o.slots[1].inLos = false;
    CHECK(CH::Select(o).action == CH::Action::None);
    // an out-of-range injured owner does not leak to another candidate:
    // the bounded outcome is None, and the behavior path (follow) is what
    // regains range.
    o.slots[1].inLos = true;
    o.slots[1].distance = 60.0f;
    o.slots[0].hp = 100; // self injured
    CHECK(CH::Select(o).action == CH::Action::SelfHeal);
}

static void TestManaReserve()
{
    CH::Observation o = Base();
    o.slots[1].hp = 500;
    // exactly cost + reserve: affordable.
    o.mana = 96 + 32;
    CHECK(CH::Affordable(o.mana, o.spellCost, o.manaReserve));
    CHECK(CH::Select(o).target == 22);
    // one point below: the bounded no-cast outcome, never a delayed retry.
    o.mana = 96 + 32 - 1;
    CHECK(!CH::Affordable(o.mana, o.spellCost, o.manaReserve));
    CHECK(CH::Select(o).action == CH::Action::None);
    // zero mana.
    o.mana = 0;
    CHECK(CH::Select(o).action == CH::Action::None);
}

static void TestCanCastGate()
{
    CH::Observation o = Base();
    o.slots[1].hp = 500;
    o.canCast = false; // casting/channeling, cooldown, or not learned
    CHECK(CH::Select(o).action == CH::Action::None);
}

static void TestEmptyAndDefault()
{
    CH::Observation o; // default: no evidence at all
    CHECK(o.version == CH::kHealerVersion);
    CHECK(o.canCast == false);
    CHECK(CH::Select(o).action == CH::Action::None);
    CH::Slot s;
    CHECK(!CH::Injured(s));
    CHECK(!CH::Castable(s, CH::kDeclaredHealRangeYd));
}

int main()
{
    TestDeclaredMatrix();
    TestNothingInjured();
    TestThresholdBoundary();
    TestOwnerPriorityOverSelf();
    TestSelfWhenOwnerHealthy();
    TestOtherMostInjuredFirst();
    TestOtherTieByLowerGuid();
    TestOutOfRangeIneligible();
    TestManaReserve();
    TestCanCastGate();
    TestEmptyAndDefault();
    if (g_failures != 0)
    {
        std::printf("value tests: %d FAILURES\n", g_failures);
        return 1;
    }
    std::printf("value tests: ALL OK\n");
    return 0;
}
