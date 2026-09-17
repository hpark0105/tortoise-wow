// PORT-025 (KAP-558): value-level coverage for the bag-pressure and
// junk-classification contract (Companion/Inventory.h). Standalone: no
// engine, no Docker. Compiled and run by
// docker/test_companion_inventory_value.py.
#include "Companion/Inventory.h"

#include <cstdio>
#include <type_traits>

namespace CI = Companion::Inventory;

static int g_failures = 0;
#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            std::printf("FAIL %d: %s\n", __LINE__, #cond); \
            ++g_failures; \
        } \
    } while (0)

static_assert(std::is_trivially_copyable<CI::ItemInfo>::value &&
              std::is_standard_layout<CI::ItemInfo>::value,
              "item info must be a value type");

static CI::ItemInfo Junk(uint32_t quality, uint32_t cls)
{
    CI::ItemInfo i;
    i.quality = quality;
    i.itemClass = cls;
    i.sellPrice = 1;
    i.bonding = 0;
    return i;
}

static void TestConstants()
{
    CHECK(CI::kInventoryObservationVersion == 1);
    CHECK(CI::kPressureFreeSlots == 2);
    CHECK(CI::kVendorSearchRadiusYd == 75.0f);
    CHECK(CI::kVendorScanPaceMs == 5000);
    CHECK(CI::kVendorFailReportMs == 30000);
    CHECK(CI::kMaxSalesPerTick == 12);
}

static void TestPressure()
{
    // The declared threshold: at or below 2 free slots is pressure.
    CHECK(CI::PressureActive(0, 0));
    CHECK(CI::PressureActive(1, 0));
    CHECK(CI::PressureActive(2, 0));
    CHECK(!CI::PressureActive(3, 0));
    CHECK(!CI::PressureActive(16, 0));
    // A stored-loot event is pressure even with free slots left.
    CHECK(CI::PressureActive(5, 1));
    CHECK(CI::PressureActive(16, 8));
}

static void TestClassify()
{
    // Declared junk matrix: consumables up to common; poor-only
    // weapon/armor/trade goods/generic.
    CHECK(CI::Classify(Junk(0, 0)) == CI::Verdict::Sellable); // poor food
    CHECK(CI::Classify(Junk(1, 0)) == CI::Verdict::Sellable); // common potion
    CHECK(CI::Classify(Junk(0, 2)) == CI::Verdict::Sellable); // poor weapon
    CHECK(CI::Classify(Junk(0, 4)) == CI::Verdict::Sellable); // poor armor
    CHECK(CI::Classify(Junk(0, 7)) == CI::Verdict::Sellable); // poor trade goods
    CHECK(CI::Classify(Junk(0, 8)) == CI::Verdict::Sellable); // poor generic
    // Quality gate.
    CHECK(CI::Classify(Junk(2, 0)) == CI::Verdict::Protected); // rare potion
    CHECK(CI::Classify(Junk(1, 2)) == CI::Verdict::Protected); // common weapon
    CHECK(CI::Classify(Junk(1, 4)) == CI::Verdict::Protected); // common armor
    // Undeclared classes are protected at any quality.
    CHECK(CI::Classify(Junk(0, 1)) == CI::Verdict::Protected); // container
    CHECK(CI::Classify(Junk(0, 3)) == CI::Verdict::Protected); // gem
    CHECK(CI::Classify(Junk(0, 5)) == CI::Verdict::Protected); // reagent
    CHECK(CI::Classify(Junk(0, 6)) == CI::Verdict::Protected); // projectile
    CHECK(CI::Classify(Junk(0, 9)) == CI::Verdict::Protected); // recipe
    CHECK(CI::Classify(Junk(0, 10)) == CI::Verdict::Protected); // money
    CHECK(CI::Classify(Junk(0, 11)) == CI::Verdict::Protected); // quiver
    CHECK(CI::Classify(Junk(0, 12)) == CI::Verdict::Protected); // quest
    CHECK(CI::Classify(Junk(0, 13)) == CI::Verdict::Protected); // key
    CHECK(CI::Classify(Junk(0, 14)) == CI::Verdict::Protected); // permanent
    CHECK(CI::Classify(Junk(0, 15)) == CI::Verdict::Protected); // junk class
    CHECK(CI::Classify(Junk(0, 99)) == CI::Verdict::Protected); // unknown
    // The protection gates each beat the matrix on their own.
    CI::ItemInfo i = Junk(0, 2);
    i.isEquipped = true;
    CHECK(CI::Classify(i) == CI::Verdict::Protected);
    i = Junk(0, 2);
    i.isBag = true;
    CHECK(CI::Classify(i) == CI::Verdict::Protected);
    i = Junk(1, 0);
    i.isQuest = true;
    CHECK(CI::Classify(i) == CI::Verdict::Protected);
    i = Junk(0, 2);
    i.isKeyOrCurrency = true;
    CHECK(CI::Classify(i) == CI::Verdict::Protected);
    i = Junk(0, 2);
    i.isUnique = true;
    CHECK(CI::Classify(i) == CI::Verdict::Protected);
    i = Junk(0, 2);
    i.hasEnchant = true;
    CHECK(CI::Classify(i) == CI::Verdict::Protected);
    i = Junk(0, 2);
    i.upgradeOverEquipped = true; // a strict upgrade stays protected
    CHECK(CI::Classify(i) == CI::Verdict::Protected);
    i = Junk(0, 2);
    i.sellPrice = 0;
    CHECK(CI::Classify(i) == CI::Verdict::Protected);
    i = Junk(0, 2);
    i.bonding = 1;
    CHECK(CI::Classify(i) == CI::Verdict::Protected);
}

int main()
{
    TestConstants();
    TestPressure();
    TestClassify();
    if (g_failures != 0)
    {
        std::printf("value tests: %d FAILURES\n", g_failures);
        return 1;
    }
    std::printf("value tests: ALL OK\n");
    return 0;
}
