# Living-world population path

Goal: make 50+ persistent, distinct bots feel active in the world while a
player uses only a small, chosen party. Fifty bots is a capacity target, not
a validated setting or a recommendation to add fifty to one party.

## First slice: party choice and off-duty presence

`.botinit <name>` sets up one owned companion; `.botinit` fills only free
normal-party slots and skips the rest of a large roster. `.botdismiss <name>`
releases a companion from the party without logging it out. While its owner is
online, an ungrouped owned bot
wanders locally around its release point, within a bounded home area, and
does not auto-acquire enemies. It pauses that wandering while the owner is
offline. This is presence, not yet independent questing or combat. The
release point and off-duty state are session-scoped, not a persistent job.

## Opt-in owned roster activation

`PLAYERBOT_OWNED_WORLD_TARGET` (default `0`) sets a global count of owned
companions to bring online while their human owner is in-world. Loading and
online companions, including manually recruited ones, count toward it. The
target is clamped to the actual owned roster and a defensive ceiling of 200;
it does not create characters or change party membership. At most one login
is queued per `PLAYERBOT_OWNED_WORLD_PACE_MS` (default 5000 ms, minimum 1000
ms). A failed identity receives a one-minute retry delay while other roster
members can proceed. Changing the target requires a world restart; it is an
activation target, not a runtime despawn command. Existing online bots are
not evicted when their owner disconnects, but their off-duty wandering pauses.

This makes a named 50-bot owned roster *eligible* to come online gradually.
A disposable 50-of-51 session smoke test passed in a small safe area, with no
party created. Its steady-state process sample was about 1.06 GiB and 28% CPU
on the test host; most five-second processing windows had a 7–9 ms p95 against
a roughly 50 ms tick interval. This does not establish capacity across zones,
combat, pathfinding, or a loaded local model. Keep the personal setting at
zero until you deliberately opt into a staged in-game pilot.

## Per-bot local-model world intent (opt-in)

`PLAYERBOT_WORLD_INTENT_ENABLE=1` lets an online, ungrouped owned bot ask the
existing local model adapter for one quiet activity: `roam` or `rest`.
Requests carry only its letters-only name, persisted personality profile,
and previous choice. The adapter accepts only a strict JSON intent and
returns one allowlisted word. It cannot specify coordinates, targets, spells,
quests, chat, or server commands. Model failure leaves deterministic roaming
in control. The world thread only submits and polls; a shared worker owns
network I/O. Player-addressed party conversation has priority over background
world intents.

Per-bot requests are at least 10 minutes apart by default, with a separate
global minimum of 10 seconds between accepted requests. The corresponding
variables are `PLAYERBOT_WORLD_INTENT_INTERVAL_MS` (minimum 60 seconds) and
`PLAYERBOT_WORLD_INTENT_GLOBAL_PACE_MS` (minimum 5 seconds). The model is
shared; this does not start one model process per bot. The same local adapter
URL configured by `PLAYERBOT_CONVERSATION_SERVICE_URL` serves the new
`/world-intent` endpoint. Defaults keep world intents disabled.

A disposable two-bot run through the actual local 27B model accepted both
closed intents (344 ms and 219 ms adapter rounds, six completion tokens each).
A stepped ten-bot run accepted ten of ten, with zero adapter fallbacks,
233–405 ms rounds, and no party. These are small, uncontended observations,
not a 50-bot model-load result or a live-player conversation test. The adapter
logs only intent, closed fallback reason, latency and token counts; it does not
log character names or prompts.
Run the opt-in real-model fixture with `BOT_OWNED_WORLD_INTENT_REAL=1` and
`python -m unittest discover -s docker -p test_bot_owned_world.py -v`.
It reuses the model already listening at `127.0.0.1:8090`; it does not swap or
launch a second GPU model. The real-model fixture needs that local server and
its normal API-key file available on the host.
Set `BOT_OWNED_WORLD_BOTS=10` and `BOT_OWNED_WORLD_TARGET=10` for the
ten-bot step; leave those unset for the default two-of-four check.
The host adapter's `REAL_PLANNER_WORLD_INTENT_TIMEOUT_MS` defaults to 1500
and is capped at 3000, separate from the party planner budget. Keep the
default for the first pilot; a timeout still falls back to local roaming.
The smaller `park-llama` preset is not automatically selected: that launcher
swaps the single managed GPU model, which would interrupt other 27B work.

## Four-citizen same-zone pilot

Four separately owned Alliance citizens can be brought online near a live
Alliance player with `PLAYERBOT_ZONE_WORLD_TARGET=4`. They are not members of
that player's owned companion roster. A normal invite temporarily recruits
one: party follow and defense take priority, and leaving the party returns
them to independent activity. Placement and recruit/defend have disposable-
world tests; the operator has also confirmed recruitment in game.

While independent, a citizen first looks for an ordinary, untapped,
level-appropriate creature within 25 yards. Otherwise it looks up to 110
yards away for a suitable same-zone hunting ground within 180 yards of its
spawn home and walks toward it. If none is available, it patrols a walkable
same-zone point. Low health pauses pulls, corpse recovery and loot retain
priority, and an unreachable hunting destination expires after 30 seconds
with a brief retry exclusion. This is local deterministic behavior, not an
LLM-directed quest or a zone-wide travel network. Disposable-world fixtures
cover the distant-target and combat paths; live movement and killing still
require the in-game pilot.

## Scaling constraints in the current implementation

- The ambient population controller counts only unowned bots; raising its
  min/max does not populate the owned roster. The opt-in owned target counts
  only already-provisioned, owner-bound identities.
- The party planner still forms one shared round for an owner-led party.
  Off-duty world intent uses a separate closed request kind on the shared
  conversation transport, not a party planner round. It chooses only
  roam/rest; independent quest and combat planning remain unimplemented.
- Every online bot is a full player session with world updates, movement,
  AI, inventory, and persistence costs. Actual capacity needs a stepped
  disposable-world load test before changing the personal server target.

## Next slices

1. Extend the local hunting-ground pilot to named places, rest, vendor, and
   limited quest work, with explicit home, death, logout, and persistence
   rules. Validate each addition in a disposable world and in game.
2. Extend the bounded roam/rest world-intent path to reviewed noncombat jobs
   and compact durable memory. Keep combat, movement, legality, and
   persistence deterministic. A stalled model must not stall the world tick.
3. Extend the opt-in activation controller into a full population manager:
   reviewed durable identities, safe zone/job assignments, deliberate
   logout/downscale rules, and priority near players. Stage load at 4, 10,
   25, then 50 bots in a disposable world while recording server tick
   latency, memory, CPU, database load, pathfinding, and LLM queue latency.

The 50-bot controlled-area session test is a capacity smoke test, not proof
of world-wide pathfinding, useful autonomous jobs, or local-model throughput
at that population. Re-run the disposable population fixture with
`BOT_OWNED_WORLD_BOTS=51` and `BOT_OWNED_WORLD_TARGET=50` for that gate, and
`BOT_OWNED_WORLD_INTENT=1` at the default small roster for the adapter round.
