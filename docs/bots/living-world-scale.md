# Living-world population path

Goal: make 50+ persistent, distinct bots feel active in the world while a
player uses only a small, chosen party. Fifty bots is a capacity target, not
a validated setting or a recommendation to add fifty to one party.

## First slice: party choice and off-duty presence

`.botinit <name>` sets up one owned companion; `.botinit` fills only free
normal-party slots and skips the rest of a large roster. `.botdismiss <name>`
releases a companion from the party without logging it out. While its owner is
online, an ungrouped owned bot travels and hunts level-appropriate, unclaimed
enemies within a bounded home area, using the same activity loop as a zone
citizen. It pauses that activity while the owner is offline. This is not
independent questing or LLM-directed combat. The release point and off-duty
state are session-scoped, not a persistent job.

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

`PLAYERBOT_WORLD_INTENT_ENABLE=1` lets an online, ungrouped persistent bot ask the
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

## Shared persistent-bot activity

Owned companions with an online owner and independent zone citizens now use
the same bounded off-duty travel-and-hunt loop. A party follow, hold, assist,
combat, recovery, or loot job takes priority. On leaving a party, the bot
re-anchors its activity around the release point. An owned bot with its owner
offline retains the prior idle behavior; this does not log it in or recruit it.
Unowned legacy ambient bots retain their separate behavior.

A recruited citizen can follow, defend, join the party planner round and be
considered for party presence cues while its exact recruiter lease is valid.
The citizen returns to solo activity when the lease ends. A recruiter can use
`.botlearn start <name>` on that citizen while it is in their party; with an
active profile, completed solo encounters can also be recorded under the
citizen's own character. `.botlearn start all` still targets permanently owned
companions only. Learning remains observe-only: no recorded summary changes
spells, targeting, quest choice, or combat policy. The optional shared local
model chooses only `roam` or `rest`, never a target or skill.

## Four-citizen same-zone pilot

Four independent Alliance citizens can be brought online across a live
Alliance player's zone with `PLAYERBOT_ZONE_WORLD_TARGET=4`. Candidate areas
come from that zone's creature spawns and receive walkability and spacing
checks; the nearby-radius setting is only a fallback. They are not members of
that player's owned companion roster. A normal invite temporarily recruits
one: party follow and defense take priority, and leaving the party returns
them to independent activity. Placement and recruit/defend have disposable-
world tests; the operator has also confirmed recruitment in game.

While independent, a citizen first looks for an ordinary, untapped,
level-appropriate creature within 25 yards. Otherwise it looks up to 110
yards away for a suitable same-zone hunting ground within 240 yards of its
spawn home and walks toward it. If none is available, it patrols a walkable
same-zone point. Low health pauses pulls, corpse recovery and loot retain
priority, and an unreachable hunting destination expires after 45 seconds
with a brief retry exclusion. This is local deterministic behavior, not an
LLM-directed quest or a zone-wide travel network. Disposable-world fixtures
cover the distant-target and combat paths; live movement and killing still
require the in-game pilot.

The outdoor route pilot now remembers up to 12 successfully reached points
per bot and the legs walked between them. It sometimes reuses a learned leg
instead of choosing a fresh patrol point. The graph is session-scoped and
cleared when the bot changes zone or is released from a party; it is not a
persisted map or T-imothy's dungeon route recorder. A stalled travel leg
gets one path reissue, then one small walkable same-zone nudge, then is
abandoned with a destination cooldown. No recovery step teleports the bot.
These stages still need a density and obstruction pilot before use on the
live 50-citizen world.

## Durable, data-driven citizen progression

An independent citizen records its current level band, zone visit, and any
in-progress progression destination in the character database. On a later
login it resumes only a compatible same-map destination; combat, party work,
loot, death, rest, and a failed walk leg retain priority. Arrival records the
new zone, while ordinary level updates do not inflate visit counts.

Progression now uses each zone's observed ordinary-creature level distribution
instead of its average alone. A citizen stays while at least 15% of profiled
creatures fall in its solo-combat band (`level - 3` through its own level).
Once that share drops below the threshold, it selects the nearest same-map,
faction-compatible zone whose observed spawns meet the threshold. The rule is
level-generic rather than a table of destinations for levels 15, 20, 45, or 50;
those choices follow from the citizen's current map and actual creature data.
At level 60 it reports `level-cap` and does not pretend there is another
leveling zone. Travel uses short path-found legs, never teleportation, and
names the selected destination in local chat. This is not a hand-authored
road graph: it does not yet know road names, inns, or guaranteed cross-map
transit. `PLAYERBOT_ZONE_WORLD_ZONE_TARGETS` provides exact per-zone counts for
bounded pilots; for example, four each in Elwynn Forest and Dun Morogh can be
set with `0:12:4;0:1:4` while the total citizen target is eight.

Citizens also persist reciprocal shared-kill acquaintances. At most one
participant speaks per shared event and uses the other citizen's name, which
keeps the nearby social cue visible without chat floods.

The optional local-model server can be started with the separate
`qwen3.8-27b-concurrent` preset for four concurrent request slots. It omits
MTP because the underlying server requires MTP to run at one slot. The normal
MTP preset remains the preferred single-request configuration. Use the
included concurrent-server smoke script before enabling it for a larger live
population; it measures success and latency without logging prompts or keys.

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
   Persisting or sharing learned routes needs a versioned destination/route
   schema, cross-bot validation, and invalidation when terrain or paths change;
   the current 12-node graph deliberately does not survive a restart.
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
