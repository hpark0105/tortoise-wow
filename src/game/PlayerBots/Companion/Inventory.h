// PORT-025 (KAP-558): value-only bag-pressure observation and the
// protected/sellable junk classification for owned companions.
//
// The engine adapter (PlayerBotAI) fills ItemInfo from the item
// prototype and instance, calls the authoritative CanEquipItem for
// the upgrade-over-equipped verdict, and runs the sale through the
// same inventory APIs the vendor packet handler uses. This header
// owns only the classification and the pressure trigger: pure
// functions of values, no engine pointers, no I/O.
//
// The declared policy is fixed by the owning card
// (docs/bots/ralph/companion-port/port-025.md) before
// implementation: the class/quality matrix in Classify is the last
// gate, and every item that fails one declared gate is Protected.
// Ambiguity never resolves to a sale.
#ifndef TORTOISE_COMPANION_INVENTORY_H
#define TORTOISE_COMPANION_INVENTORY_H
#include <cstdint>
#include <type_traits>

namespace Companion
{
namespace Inventory
{

inline constexpr uint32_t kInventoryObservationVersion = 1;

// Declared free-slot threshold: bag pressure is active while the
// authoritative free item-slot count is at or below this, or while
// at least one loot event could not be stored (the bounded PORT-024
// stored-loot pressure counter is non-zero).
inline constexpr uint8_t kPressureFreeSlots = 2;

// Declared vendor discovery bounds: same-map grid scan, the nearest
// legal vendor wins; re-scan pace while no vendor is selected and
// pressure remains.
inline constexpr float kVendorSearchRadiusYd = 75.0f;
inline constexpr uint32_t kVendorScanPaceMs = 5000;
// Declared no-vendor failure report: one report on the first failed
// scan, then at most one per this window.
inline constexpr uint32_t kVendorFailReportMs = 30000;
// Bounded sale pace: at most this many sales per tick. One pass of
// the bag evaluates every slot once; the pace only caps writes.
inline constexpr uint8_t kMaxSalesPerTick = 12;

inline bool PressureActive(uint8_t freeSlots, uint8_t storedLootPressure)
{
    return freeSlots <= kPressureFreeSlots || storedLootPressure > 0;
}

// The prototype/instance fields the classification depends on.
// Filled by the engine adapter each sale pass; the adapter fills
// upgradeOverEquipped through the authoritative CanEquipItem before
// consulting the policy, so the policy stays a pure function of
// values.
struct ItemInfo
{
    uint32_t version = kInventoryObservationVersion;
    uint32_t quality = 0;     // 0 poor .. 5 legendary
    uint32_t itemClass = 0;   // ItemClass (ItemPrototype.h)
    uint32_t bonding = 0;     // ItemBondingType; 0 = NO_BIND
    uint32_t sellPrice = 0;   // 0 = the vendor buys nothing
    bool isEquipped = false;
    bool isBag = false;
    bool isQuest = false;          // quest class, start quest, or log-required
    bool isKeyOrCurrency = false;  // key or money class
    bool isUnique = false;         // MaxCount==1 && !Stackable (non-consumable)
    bool hasEnchant = false;       // any instance enchantment
    bool upgradeOverEquipped = false; // legal equipment strictly better than the slot's gear
};

static_assert(std::is_trivially_copyable<ItemInfo>::value &&
              std::is_standard_layout<ItemInfo>::value,
              "item info must stay a bounded value type with no retained pointers");

enum class Verdict
{
    Sellable,  // declared low-risk junk: the vendor may buy it
    Protected  // everything else, including every unrecognized case
};

// The declared low-risk junk set. An item is sellable only when
// every gate holds; the class/quality matrix is the last gate and
// every undeclared class is protected:
//   - CONSUMABLE (0) up to common (q <= 1): food/potion filler
//   - WEAPON (2), ARMOR (4), TRADE_GOODS (7) and GENERIC (8) of
//     poor quality only (q == 0)
// Equipment the companion may still want (a strict upgrade over
// what is worn in the slot it would fill) stays protected even when
// it matches the matrix.
inline Verdict Classify(ItemInfo const& i)
{
    if (i.isEquipped || i.isBag || i.isQuest || i.isKeyOrCurrency ||
        i.isUnique || i.hasEnchant || i.upgradeOverEquipped)
        return Verdict::Protected;
    if (i.sellPrice == 0 || i.bonding != 0)
        return Verdict::Protected;
    bool inMatrix = false;
    switch (i.itemClass)
    {
        case 0: // ITEM_CLASS_CONSUMABLE
            inMatrix = i.quality <= 1;
            break;
        case 2: // ITEM_CLASS_WEAPON
        case 4: // ITEM_CLASS_ARMOR
        case 7: // ITEM_CLASS_TRADE_GOODS
        case 8: // ITEM_CLASS_GENERIC
            inMatrix = (i.quality == 0);
            break;
        default:
            inMatrix = false; // undeclared class: protected
            break;
    }
    return inMatrix ? Verdict::Sellable : Verdict::Protected;
}

} // namespace Inventory
} // namespace Companion

#endif
