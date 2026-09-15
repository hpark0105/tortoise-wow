# PORT-024: Progress companion equipment from earned loot

- Depends on: PORT-023
- Status: pending hosted dispatch; passes=false
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
