# Bot feasibility: first engineering pass

Date: 2026-09-11. Personal source baseline: `8a7f25a`. This is evidence for
BOT-001/BOT-002 in [the PRD](../prd/living-world.prd), not approval to enable bots
on the personal realm or a claim that P0's gameplay gates have passed.

## Reproduce the measurements

```powershell
python docker/bot_baseline.py --seconds 120
python docker/bot_baseline.py --isolated --seconds 120
python -m unittest discover -s docker -p 'test_*.py'
```

The first command only reads the existing world's bot configuration, aggregate
database counts, image identities and Docker resource samples. It does not attach
to the game console, change configuration or restart services. The second boots a
fresh database/world under a random `tortoise-bot-lab-<id>` project, using generated
test credentials, the existing tested image and read-only extracted assets. It
publishes no ports and mounts no personal database or backup. Cleanup verifies
project ownership and mounts before removing only the generated lab's resources.
Evidence stays under ignored `local/`. Existing CI discovers the new safety tests;
the client-dependent lab is an opt-in local check.

Use idle results only to establish a starting point. Re-run matched gameplay routes
with a human and then bot cohorts; record concurrent client, model and indexing
workloads. Docker CPU 100% represents one logical CPU, not the entire desktop.
Samples are snapshots, not continuous peak detection.

## Live idle measurement

Two minutes beginning 2026-09-11 20:45:48 UTC, 18 samples per container:

| Service | Mean CPU | Sample p95/max CPU | Memory first / last |
|---|---:|---:|---|
| World | 9.49% | 12.19% | 1.163 / 1.163 GiB |
| Database | 0.20% | 3.27% | 158.4 / 158.0 MiB |

Bot enable/min/max were zero, roster rows were zero, and database online-character
rows were zero at both endpoints. Online flags are not an authoritative active
session metric. Docker reported 32 logical CPUs and 32,264,798,208 bytes of memory.
No candidate compilation or bot lab ran during this measurement. Other host work
was not continuously instrumented, so this is not a controlled whole-desktop test.

Observed world image ID:
`sha256:34763fe924181dc22ff7f84377a77e7bc7dbdf69b5e69c13fce2e0cd3fc64014`.
Local report: `local/tortoise-local-20260911T204548Z/result.json`.
The source commit is recorded separately; it is not proof that a binary contains
every current source change.

Tick processing p95/p99, interaction latency, FPS and active-bot capacity were not
measured. `WorldRunnable.cpp` targets 50 ms between iterations; `SetLastDiff`
records elapsed time between starts, including sleep. Do not report that value as
world-processing cost. `PerformanceMonitor` has separate tick/sleep timers but
its inspected reporting does not provide the required per-tick percentile series.
BOT-001 still needs bounded processing-time instrumentation and matched routes.

The fresh port-free lab also reached world-ready and completed 60 seconds of
sampling (nine samples). Its world averaged 34.04% CPU with memory growing from
1.033 to 1.110 GiB; this startup/warm-up observation is not interchangeable with
the settled live baseline. Database average CPU was 0.72%, with memory around
659 MiB after import. Bot settings and roster/online endpoints were zero.
Report: `local/tortoise-bot-lab-3926c9a24659-20260911T204801Z/result.json`.
The lab's containers and volumes were removed; the personal services remained
running. This proves bootstrap/isolation, not persisted bot gameplay.

## Bundled engine blockers

| Finding | Current source evidence | Required proof before use |
|---|---|---|
| Bot saves are explicitly rejected | `src/game/Objects/Player.cpp`, `Player::SaveToDB`, returns false for `GetSession()->GetBot()` | A reviewed persistent-bot policy; earned XP, items, quests and position survive logout/restart/isolated restore |
| Removing the save guard alone is unsafe | `PlayerBotMgr::Load` assigns generated session account IDs; `SaveToDB` writes `GetSession()->GetAccountId()` into the character row | Stable persisted ownership separate from temporary session identity; human and existing account ownership never overwritten |
| Unsafe initialization/entry ordering | `PlayerBotAI.h` omits `_abilityTimer` initialization; `UpdateAI` dereferences `me` before its null check | Null/unloaded/teleport tests and deterministic first ability evaluation |
| Lifecycle counts need validation | `OnSessionLoaded` returns true after calling `LoginPlayer`; manager changes loading/online counts; `AddBot` has no existing-state rejection at its entry | Repeated add, failed login, stale session, disconnect and retry tests with actual session counts |
| Exact population is not guaranteed | `Load` changes min/max at roster boundaries; `AddOrRemoveBot` removes when chosen count equals current count | Empty/single/full roster and exact-target boundary tests; explicit loading versus active counts |
| Default behavior falls short of companions | `UpdateAI` returns when dead and otherwise uses offensive targeting/wandering; login/level hooks auto-learn and auto-equip | Role tactics, recovery, supported quests and earned-gear policy before adding LLM planning |

These are source findings. No new bot crash, saved-character corruption or bot
gameplay failure was injected into the personal realm. The save guard is an
existing intentional boundary, not something to delete without tracing ownership.
Custom spawned bots also need explicit provisioning; a generated runtime `Player`
is not proof of a durable character.

## Candidate inspection and build probe

Candidate: [Shyalya playerbots integration at the pinned revision](https://github.com/Shyalya/tortoise-wow/tree/deada5f33018d2bbddf21d07601e89c5469a5dcd),
`deada5f33018d2bbddf21d07601e89c5469a5dcd`, in the separate ignored checkout
`local/bot-engine-candidate`. Its README announces retirement during September
2026. Root license files in both trees identify AGPL v3; retain notices and audit
vendored file provenance before selecting a port. No candidate code was imported
into the personal core.

Actual bot code lives under `modules/mod-playerbots/`; the candidate's quick-start
still references an older `src/modules/PlayerBots` layout. Current build wiring
requires `BUILD_PLAYERBOTS=ON`, Boost thread/filesystem/system and the host hooks.
The probe explicitly preserves `ALLOW_TURTLE_ADDONS=ON`, disables optional Eluna
and unrelated modules, and enables the playerbot module through the candidate's
existing CMake switch. LTO is off for the compatibility compile; this is not a
production performance build.

```powershell
git -C local/bot-engine-candidate rev-parse HEAD
docker build --progress plain -f docker/bot-candidate.Dockerfile --target checks -t tortoise-bot-candidate:checks local/bot-engine-candidate
docker build --progress plain -f docker/bot-candidate.Dockerfile --target server -t tortoise-bot-candidate:build local/bot-engine-candidate
```

Verify the checkout matches the pinned revision before running. The probe has no
runtime entrypoint and starts no realm. The `checks` stage builds four existing
candidate regression executables: creation lifecycle, movement dispatch, retry
handling and travel-route policy. They exercise bounded production fragments with
stand-in services; a pass does not prove live database or gameplay compatibility.
The separate `server` stage must link `mangosd` before reporting a server build pass.

Server CMake configuration passed with the probe flags. The existing architecture
test configuration failed before any selected test executed:
`Cannot find native regression start: ForkBannerCandidates.inc` at
`tests/architecture/CMakeLists.txt:163`. That extraction expects the comment
`// Ground-level static rays` in `BattleGroundTactics.cpp`; the pinned source no
longer contains it. This is a concrete test/source mismatch, not a failed gameplay
scenario. Evidence: `local/bot-candidate-build.log`. Server and test stages are
independent so a server compile can still be assessed without concealing this
failure. The four candidate tests are **not passed**.

Evidence for reuse value includes real follow/formation, quest action, character
creation/save and class-strategy code. Evidence against copying just the module
includes new module slots on `Player`, bot hook registration/packet dispatch and
map-owned AI scheduling absent from our baseline. Five representative core-file
diffs contain 2,610 added and 414 removed lines (World, Map, Player header/source,
WorldSession). These are whole-file branch differences, not an estimate that every
line is required for bots. Unrelated core/content changes must be separated.

Candidate default templates enable bot auto-creation/autologin with a 4,000-bot
minimum/maximum and enable auction automation. Never run those defaults on our
realm. Any later runtime experiment must explicitly disable auction automation,
set a tiny cohort, use disposable databases and review progression/relocation
settings against the PRD.

## Decision and next gate

Keep the working personal core. Treat selective reuse from the candidate as a
promising option requiring a dependency map and tests; do not adopt its entire
branch or assume its module can compile unchanged against our host interfaces.
The bundled engine remains useful for a small controlled experiment, but its
current persistence policy rules it out as a ready-made durable companion system.

The first dependency map is:

| Boundary | Candidate evidence | Port constraint |
|---|---|---|
| Build/compatibility | `mod-playerbots.cmake`, `botpch.h`, compatibility stubs | Inventory shim contracts and Boost dependencies; a same-named API is not proof of equivalent behavior |
| Player ownership/lifetime | `BotSlots.h`, `HostHooks.cpp`, `PlayerbotScripts.cpp` | Define creation, release-to-human and logout cleanup; our baseline has no candidate module-slot accessors |
| AI scheduling | `PlayerScript::OnAIUpdate`, `Map::UpdatePlayerAI` | Our hook is absent; place initial execution on the existing owner context without importing unrelated scheduler/concurrency changes |
| Startup/population | `World::InitPlayerbotsAtStartup`, `PlayerbotWorldScript::OnStartup` | Wait for required game data; enforce ten-bot test limits and bounded creation/login work |
| Sessions/actions | `PlayerbotHolder` in `PlayerbotMgr.cpp`, packet hooks | Preserve real account ownership, unique session/lifecycle handling and normal action validation; verify hook order and callback lifetime |
| Saving | Candidate `Player::SaveToDB` removes the legacy bot guard and its holder creates sessions using the login holder's account ID | Port the ownership contract together with saving; the removed guard alone is insufficient |

For a bundled-engine experiment, the proposed persistence contract is: explicit
bot-owned metadata referencing a real account and character GUID; initially one
dedicated account per persistent bot to fit the existing session map; ephemeral
bots retain their current no-save behavior. Before login/save, verify that account,
character and bot ownership agree. A generation number distinguishes stale async
login completions from a later request. Human-owned characters cannot be silently
converted or reassigned. Provisioning must resume partial creation idempotently,
and companions must disable automatic replacement of earned gear. Tests must
exercise interrupted provisioning, repeated login, level/loot/quest saving,
restart, restore and human-record preservation. This contract remains a design
proposal, not an implemented schema or relaxation of the current save guard.

Next work is an explicit persistent-bot identity/save contract, the smallest host
hook dependency map for candidate reuse, and a ten-identity disposable experiment.
That experiment must demonstrate normal saves and basic follow/combat/recovery
before 50+ activity or LLM integration. Choose and record a known-working quest
route and dungeon separately; the content warning backlog still applies.

Retrieval synchronization remained deferred while the operator worked on embedding
containers. Current source was checked directly. A fresh bounded local audit was
attempted, but the shared-model mutex rejected it because another session was
active; the head performed this review. No local-worker result, fresh embedding
counts or token savings are claimed.
## TW-001 diagnosis: candidate test-configuration mismatch (KAP-544)

Date: 2026-09-11 (head pass during the retrieval-sync window). Read-only source
comparison against the pinned checkout; no build was run for this diagnosis and
no candidate code was imported.

### Verified pin

`deada5f33018d2bbddf21d07601e89c5469a5dcd` ("Merge remote-tracking branch
'penqle/main' into playerbots-integration-gh", 2026-09-11), worktree clean.
The local checkout is shallow (parent objects absent), so the exact commit that
removed the marker is not identifiable locally; reproduce with a full clone plus
`git log -S "Ground-level static rays" -- modules/mod-playerbots/src/playerbot/strategy/actions/BattleGroundTactics.cpp`.

### Missing contract

- Contract: `extract_native_fragment` (`tests/architecture/CMakeLists.txt:11-25`)
  requires literal anchors in
  `modules/mod-playerbots/src/playerbot/strategy/actions/BattleGroundTactics.cpp`:
  start `        // Ground-level static rays`, end `        closePlayers =`
  (invocation at `tests/architecture/CMakeLists.txt:163-164`). The extracted
  `ForkBannerCandidates.inc` is compiled into `ForkIntegrationTest`
  (`tests/architecture/ForkIntegrationTest.cpp:90`).
- At the pin: the start marker exists nowhere in the checkout (the only reference
  is the CMake invocation). The end marker still exists
  (`BattleGroundTactics.cpp:4517,4527`).
- Full audit of all 56 `extract_native_fragment` contracts under CMake semantics
  (end searched after truncation at the start): 55 OK, 1 broken
  (`ForkBannerCandidates.inc`). The inline trainer-handler extraction markers are
  present. The mismatch is single-point; no latent anchor breakage follows it.
  Audit script: `local/scratch-marker-audit2.py` (ignored, rerunnable).

### Build status from evidence

| Stage | Evidence | State |
|---|---|---|
| configure (server CMake) | prior run recorded in `local/bot-candidate-build.log` | pass |
| checks (architecture tests) | `bot-candidate-build.log:837-868`, docker build exit 1 at `bot-candidate.Dockerfile:26-31` | failed at CMake configure: `Cannot find native regression start: ForkBannerCandidates.inc` |
| server (mangosd link) | completed build log reports mangosd link failure: prebuilt gSOAP references `__isoc23_strtol`, `__isoc23_strtoul` and `strlcpy`, unavailable in the Ubuntu 22.04 build environment | failed compatibility gate; no candidate image; gameplay not assessed |

### Unexecuted versus failed

- Failed: exactly one thing - the CMake configure of `tests/architecture`.
- Unexecuted (not failed): every architecture test, including the four targeted
  candidate regressions (`BotCreationLifecycleTest`, `BotMovementDispatchTest`,
  `BotRetryIntegrationTest`, `TravelRoutePolicyTest`); configure aborted before
  any test binary was built. "The four candidate tests are not passed" remains
  true; the precise statement is "never executed".

### Repair-semantics uncertainty (AC2; recorded, not resolved by weakening)

`ForkIntegrationTest` pins banner-candidate selection: from the context value
`nearest game objects no los`, keep guids whose `GetGameObject` is non-null and
within `INTERACTION_DISTANCE` (5.0f); expected `{1,2}` from ids `{1,2,3,4,99}`
against objects `{1:(map1,1.5), 2:(map1,5.0), 3:(map1,5.1), 4:(map2,1.0)}`
(`ForkIntegrationTest.cpp:80-100`). The refactored AB/IC block at the pin
(`BattleGroundTactics.cpp:4456-4520`) preserves the core selection semantics but
now also contains banner-entry classification (`vFlagIds`), diagnostic counters
and throttled `sLog` output that the test's stand-ins do not model. Re-anchoring
the extraction to the new block would (a) fail to compile against the stand-ins
and (b) silently widen what the regression test asserts. A correct repair
requires extending the test harness to the current block (or restoring the
marker with a narrow fragment boundary) - a test-side change to what is pinned,
to be owned by the TW-002 boundary decision. No assertion was deleted or
weakened to obtain a pass.
