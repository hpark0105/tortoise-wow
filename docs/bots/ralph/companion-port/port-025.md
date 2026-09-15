# PORT-025: Report bag pressure and sell protected junk at a vendor

- Depends on: PORT-024
- Status: pending hosted dispatch; passes=false
- Tracking: KAP-543 / Phase 2 inventory progression
- Shared contract: [execution and review](README.md)

## Objective

React to authoritative bag pressure with a bounded personality-consistent message
and one safe cleanup path: sell explicitly approved junk to a reachable current
Turtle vendor through normal APIs. Static model context explains vendor concepts;
it never identifies the NPC, items, route or transaction.

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
