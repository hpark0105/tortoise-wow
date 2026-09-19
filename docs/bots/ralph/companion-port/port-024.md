# PORT-024: Progress companion equipment from earned loot

- Depends on: PORT-023
- Status: implemented (PORT-024); passes=false pending acceptance
- Tracking: KAP-543 / Phase 2 equipment progression
- Shared contract: [execution and review](README.md)

## Objective

Keep automatic class-skill learning up to the companion's level, but stop free
equipment refreshes for persistent companions. A companion improves its gear
only from items it legitimately receives through the accepted loot policy. A
deterministic authoritative evaluator decides whether an item is legal and a
strict upgrade; the model never selects or equips an item.

## Allowed edit candidates

- `PlayerBotAI.cpp/.h` gating between `AutoLearnSpellsForLevel` and
  `AutoEquipForLevel`
- Deterministic equipment capability/comparison code under `Companion/`
- Narrow integration with the existing normal corpse-loot and inventory paths
- One disposable loot/equip/persistence fixture under `docker/`
- No model or prompt change is required to equip gear

## Acceptance

`AutoLearnSpellsForLevel` remains authoritative on login and level-up, so every
eligible class ability up to the companion's level is learned without trainer
travel or cost. Once a bot enters persistent companion progression,
`AutoEquipForLevel` cannot replace its equipment on login, level-up, party leave
or rejoin. Ambient world bots retain their existing automatic equipment policy.

Before dispatch, the evidence card fixes one loot-distribution policy; until
then no automatic Need roll against the owner is authorized. For an item the
companion legitimately receives, C++ validates class, level, armor/weapon
proficiency, equip/unique restrictions, slot and current equipment. A strict
upgrade is equipped through normal inventory APIs, replaced gear is retained in
bags when possible, and equipment/inventory persist across restart.

## Failure cases

Do not duplicate, mint, transform, silently delete or equip an item the companion
did not receive. Unusable items, sidegrades, downgrades, broken unique rules or
ambiguous comparisons leave equipment unchanged and preserve the item when normal
inventory rules allow. A full bag raises a bounded inventory-pressure state for
PORT-025; it never deletes an item to make room. Leaving the party cannot re-enable
free gear. The model cannot supply item IDs, stat weights or equip commands.

## Validation

Compile; automatic skill learning at login and level-up; no free companion gear
refresh at the same boundaries; unchanged ambient-bot auto-equip behavior;
legitimate loot receipt, usable upgrade, unusable item, downgrade, full bags,
replacement retention, party leave/rejoin and restart persistence. Record exact
item-instance and equipped-slot state; prove the full-bag path raises pressure
without deletion; rerun loot/regroup and combat regressions.

## Evidence

### Implementation

- `AutoEquipForLevel` is gated for owned companions at the single entry
  that covers login, level-up, the UpdateAI level-change fallback and
  party leave/rejoin (a rejoin is a relogin); the free level-based
  refresh stays authoritative for ambient world bots.
- `Companion/Equipment.h`: value-only policy. `Score() =
  effectiveLevel*1000 + quality*10 + itemLevel` with
  `effectiveLevel = requiredLevel else max(1, itemLevel*2/3) else
  playerLevel`; `Compare` is strict `>`, so sidegrades and downgrades
  are retained in the bag.
- `PlayerBotAI::EvaluateReceivedEquipment` (called from
  `CorpseLootStep` after `AutoStoreLoot`, owned companions only): an
  entry whose saved count did not grow raises the bounded
  `_inventoryPressure` state (capped at 8, consumed by PORT-025) -
  nothing is ever deleted to make room; an entry whose count grew is
  evaluated for up to the delta of its bag instances (bounded,
  idempotent).
- `PlayerBotAI::EvaluateReceivedInstance`: the authoritative
  `CanEquipItem(..., swap=true)` decides legality and slot (class,
  level, skill/proficiency, unique rules, live-combat state); only a
  strict upgrade moves, via `SwapItem`, the received instance into the
  equipment slot - a 1:1 exchange where the displaced gear returns to
  the bag slot the new item came from, so full bags can never block an
  upgrade. Engine position encoding: `CanEquipItem` packs `dest` as
  `(INVENTORY_SLOT_BAG_0 << 8) | slot` with
  `INVENTORY_SLOT_BAG_0 = 255`, so the policy unpacks the low byte
  before comparing against `EQUIPMENT_SLOT_END`. The first revision
  compared the packed value and silently rejected every legal upgrade
  (Upgrade lab signature: items stored and counted by `GetItemCount`,
  never evaluated); found and fixed before acceptance.
- The model never supplies item IDs, scores or equip commands.

### Validation

- Value suite: `docker/test_companion_equipment_value.{cpp,py}` -
  scoring and strict-comparison pins (1008 -> 6020, 15335 -> 9034,
  3267 -> 3015, 7298 -> 8033) - green.
- Runtime fixture: `docker/test_bot_equipment.py`, three disposable
  Compose labs (isolated projects, personal state asserted
  before/after), all on image
  `sha256:355cdaae94b89b922a4a98b7a6948bf8bbd8446acaef71d52ffabc9288fb0e11`;
  17/17 green:
  - Lab A (Upgrade, 6/6): one kill of the seeded Kobold Vermin drops
    15335 + 3267 + 7298 at 100%; log sequence
    `equipment keep item:3267 new:3015 eq:6020`,
    `equipment skip illegal item:7298 err:10` (rogue-only item,
    warrior companion),
    `equipment upgraded slot:15 item:15335 new:9034 eq:6020`; DB
    state shows exactly one equipped item (MAINHAND 15335), the
    seeded baseline instance retained in a bag slot, and the
    downgrade + illegal items in the bag; clean stop + world restart
    -> saved inventory/equipment byte-identical and no re-evaluation
    markers in the new boot generation.
  - Lab B (Pressure, 8/8): bag0 filled with 16 x 20 filler stacks;
    the looted 15335 is unstoreable ->
    `equipment pressure stored:0`, no mint, no equip, no deletion
    (filler total 320 intact, captured before lab teardown);
    companion regroups within 2.0 s of the pressure event.
  - Lab C (Ambient, 3/3): an ambient bot (owner_account_id NULL)
    keeps the free `AutoEquipForLevel` refresh on login.
- Fixture hardening from failed runs: seeded item rows must fill all
  15 `item_instance` columns (`charges` non-NULL - `Item::LoadFromDB`
  tokenizes field 4 and aborts on NULL; durability clamps to the
  proto max and 0 is a broken weapon); `boot_lab` failure self-heal
  (force-down with dummy envs, compose requires the
  `${BOT_LAB_ROOT_PASSWORD:?}` interpolation); DB count assertions
  captured in `setUpClass` before the lab is torn down, not in the
  test methods after.
