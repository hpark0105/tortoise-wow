# PORT-025: Report bag pressure and sell protected junk at a vendor

- Depends on: PORT-024
- Status: implemented (PORT-025); passes=false pending acceptance
- Tracking: KAP-543 / Phase 2 inventory progression
- Shared contract: [execution and review](README.md)

## Objective

React to authoritative bag pressure with a bounded personality-consistent message
and one safe cleanup path: sell explicitly approved junk to a reachable current
Turtle vendor through normal APIs. Static model context explains vendor concepts;
it never identifies the NPC, items, route or transaction.

## Fixed policy (declared before implementation)

The values this card fixes before implementation; the implementation must
match them and the value tests pin them.

- Scope: owned companions only (owner account non-zero); ambient bots never
  run the cleanup path.
- Trigger: authoritative free item slots <= 2, or the bounded PORT-024
  stored-loot pressure counter is non-zero.
- Status: exactly one personality line per pressure episode, never per tick;
  the per-profile lines are fixed in the personality catalog and the None
  profile has a plain line.
- Vendor discovery: same-map 75 yd grid scan; re-scan paced at 5000 ms while
  pressure persists and no vendor is selected; only a live, in-world,
  non-evading creature with the vendor service flag is eligible; nearest
  wins, ties by lower GUID.
- No-vendor failure: report on the first failed scan, then at most once per
  30000 ms, with the per-profile personality line; nothing is sold or
  deleted; the companion waits for owner guidance.
- Junk matrix (fail-closed; an item is sellable only when every gate holds):
  - never sold: equipped items, bags, quest items (quest class, start quest,
    or log-required), keys or currencies, unique non-consumables, any
    instance enchantment, a strict upgrade over the gear worn in the slot it
    would fill, sell price 0, any bonding.
  - sellable: CONSUMABLE up to common (quality <= 1); WEAPON, ARMOR,
    TRADE_GOODS and GENERIC at poor quality only (quality == 0).
  - any undeclared class or unknown field: protected.
- Sale: handler-shaped through the normal vendor APIs - the authoritative
  interaction check (service flag, life, hostility, combat state, reputation,
  5 yd), re-resolution by GUID, owner/bank/loot/non-empty-bag guards,
  full-stack sale at SellPrice, buyback slot, authoritative money log. At
  most 12 sales per tick.
- Exhaustion: when every remaining item is protected, report once, stop the
  vendor travel for the episode, and resume the prior valid owner goal.
- Preemption: Hold freezes the whole path (no scan, no sale); combat pauses
  scans; recovery/death and stale generations preempt; a stale or
  unreachable vendor is dropped with a reason and re-scanned at pace.
- Priority: Hold > Assist > ContinueCombat > Defend > Damage > Vendor > Loot
  > Follow (the Vendor candidate sits between Damage and Loot).
- Versioning: inventory observation version 1; Observation v3 adds
  vendorTarget and bagPressure; directive v2 appends the Vendor action to
  the end of the action enum (index 8).

## Allowed edit candidates

- Inventory-pressure observation and protected/sellable classification under
  `src/game/PlayerBots/Companion/`
- Narrow authoritative vendor discovery, bounded approach and sale execution
- Bounded expression/status integration through the accepted personality channel
- One disposable bag-pressure/vendor/money fixture under `docker/`
- `PlayerBotAI.cpp/.h` dispatch only as required

## Acceptance

At the declared free-slot threshold, the companion emits one bounded status line
such as a personality-specific “my bags are full” message and does not spam each
tick. C++ classifies items before any travel or sale. Equipped items, quest items,
keys/currencies, currently selected upgrades, explicitly retained items and every
unrecognized/ambiguous item are protected by default.

The deterministic resolver selects only a current live vendor with the required
NPC service flags and supported same-map bounded reachability. Hold, combat,
recovery, owner orders and party loss preempt travel/selling. At interaction range,
the companion sells only the declared low-risk junk set through normal vendor
handling, receives authoritative money, frees the expected slots and resumes its
prior valid owner goal. Inventory and money persist across restart.

The model may phrase the status or prefer a validated cleanup opportunity, but it
cannot supply vendor or item IDs, coordinates, routes, prices or a sell command.
If no supported vendor is known or reachable, the companion reports that bounded
failure and waits for owner guidance without deleting or selling anything.

## Failure cases

Do not sell equipped, quest, upgrade, rare/valuable, unique, bound-but-useful or
unknown items merely to meet a slot target. Do not teleport, use model-invented
vendor knowledge, scan the entire world per tick, sell from outside interaction
range, bypass normal prices, or continue after a stale generation or owner Hold.

## Validation

Compile; pressure threshold and notification-rate tests; protected, sellable and
ambiguous item matrices; no-vendor, wrong-vendor, unreachable-vendor, combat/Hold,
party-loss and stale-generation cancellation; normal sale money/inventory deltas;
resume-goal and restart persistence; loot/equipment/quest regressions; bounded
client evidence for full-bag reporting, vendor travel and sale.

## Evidence

- 2026-09-17, branch `feature/kap-558-port-phase2`, baseline `192624c`
  (PORT-024), image `tortoise-local:dev` (image id recorded per lab under
  `local/`, evidence retained there).
- Value suite 9/9 (`test_companion_inventory_value` plus the existing
  personality/protocol/policy/fake-contract suites), 5.6 s.
- Compose labs (`docker/test_bot_vendor.py`, isolated `tortoise-bot-vend-*`
  projects, personal containers asserted untouched): 25/25 OK in 479.9 s,
  sanitized log `local/port025-labs4.log`.
  - Lab A (Sale + restart, 9/9): pressure line once with profile, vendor
    found at dist:20.4, exactly 11 jerky stacks sold at `money:20` each
    (220 c total), q0 sword / quest item / q2 sword kept, pressure cleared
    at free:13, restart round-trips inventory and money unchanged, zero
    `[Inventory]` activity in the new boot generation.
  - Lab B (Hold, 7/7): hold before the first possible sale tick freezes
    scan and sale alike; the newer follow releases it and all 11 stacks
    sell.
  - Lab C (No-vendor, 6/6): first failed scan reports, pacing holds under
    the 30 s window, nothing sold or deleted, bag untouched (14 items).
  - Lab D (Ambient gate, 3/3): an unowned bot with a full bag inside
    vendor radius keeps the whole cleanup path silent.
- Fixture defect found and fixed while bringing the labs green: the
  seeded `item_instance.charges` must carry exactly
  `MAX_ITEM_PROTO_SPELLS` (5) tokens. `Item::LoadFromDB` applies charges
  only when the token count matches exactly, and `Item::SaveToDB` always
  writes 5. A 4-token seed leaves the in-memory charges at 0 and the
  vendor handler's charge-price multiplier then computes `0 / -1 = -0.0`,
  zeroing the sale price of every stack (observed as 11x `money:0` with
  money unchanged). The game code matches the authoritative
  `HandleSellItemOpcode` path; only the fixture seed was corrected.