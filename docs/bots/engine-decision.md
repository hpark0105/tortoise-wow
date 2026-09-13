# Engine decision: smallest viable bot-engine integration boundary (KAP-545)

Date: 2026-09-11. Head decision recorded for TW-002 after the TW-001
(KAP-544) diagnosis. Inputs: [feasibility.md](feasibility.md) (bundled-engine
blockers, candidate inspection), the TW-001 diagnosis section, and direct
source reads at personal-server baseline `8a7f25a` (worktree) and the pinned
candidate `deada5f33018d2bbddf21d07601e89c5469a5dcd` (clean checkout).
Read-only; no candidate code was imported.

## Context

The living-world epic needs a bot execution engine. Two candidates exist:

1. **Bundled engine** (`src/game/PlayerBots/`: `PlayerBotAI.{h,cpp}`,
   `PlayerBotMgr.{h,cpp}`) - compiles in the personal core today, driven by
   `WorldSession::GetBot()` (`src/game/WorldSession.h:541`, `PlayerBotEntry`
   with `ai`). Known source blockers (feasibility.md, verified): save
   rejection, synthetic session account IDs on load, initialization/null
   ordering, lifecycle counts, population boundaries, weak default behavior.
2. **Pinned candidate** (Shyalya playerbots integration; retirement announced
   in its README; AGPL v3) - richer strategy/tactics code, but requires host
   integration, and its architecture-test configuration is broken at the pin
   (TW-001: single-point marker mismatch; all architecture tests unexecuted;
   server link unverified).

## Decision

**D1 - Keep the working personal core.** The candidate branch is not
imported, whole or by wholesale module copy (5 representative core-file
diffs: +2,610/-414 lines; whole-file branch differences, feasibility.md).

**D2 - Evaluate the candidate module as a separately-built target before any
core merge.** Reuse of the candidate `mod-playerbots` tree is conditional:
it must build, and its targeted regressions must pass, in the isolated probe
(`docker/bot-candidate.Dockerfile` pattern) before any of it enters
personal-server source. Each ported fragment needs a provenance audit (AGPL)
and its regression test.

**D3 - The bundled engine stays for the small controlled experiment**, gated
by TW-008/TW-009 (initialization/null safety, lifecycle safety). Its save
path remains closed by the existing guard (`Player::SaveToDB`,
`src/game/Objects/Player.cpp:18211`, "Pas de sauvegarde des bots") until the
TW-004 ownership contract is approved.

**D4 - The ownership contract precedes save changes in either engine.** The
candidate's save posture (synthetic sessions; load account-check bypass
removed at candidate `Player.cpp:16831`) writes session account IDs into
character ownership without persistent ownership metadata. The TW-004
contract and its save enablement (TW-007) are ported atomically; the bundled
`!GetBot()` load bypass (`Player.cpp:16604`) is likewise not removed on its
own.

**D5 - Map-owned AI scheduling is a separate review item (AC2).** The
candidate's scheduling pair is a core concurrency change, not module
plumbing. It is separated here from the module import and requires its own
review before adoption (Required host hooks, H2). The smallest viable
boundary executes bot AI on the existing owner context (phase A) without it.

## Smallest viable boundary

### Selected reuse (candidate; conditional on the D2 gate)

| Item | Candidate evidence | Condition |
|---|---|---|
| Strategy/action/tactics source tree | `modules/mod-playerbots/src/playerbot/**` (travel, retry cache, BG tactics, class strategies) | Builds behind the build boundary; targeted regressions pass in the probe; per-fragment AGPL provenance audit before port |
| Architecture-test pattern | `tests/architecture/CMakeLists.txt:11-25` (`extract_native_fragment`), `ForkIntegrationTest.cpp` | Harness template for ported fragments only; the broken contract is repaired test-side (TW-001 uncertainty), never by weakening assertions |
| Taxi/travel policy + retry-cache fragments | `TravelNode.cpp`, `strategy/Engine.cpp` fragments (55 of 56 extraction contracts verified intact at the pin, TW-001 audit) | Fragment-level port with its regression test, not whole files |

### Required host hooks

| # | Hook | Candidate evidence | Personal baseline | Boundary rule |
|---|---|---|---|---|
| H1 | Bot session entry | Synthetic-session holder in candidate `PlayerBotMgr.cpp` (feasibility.md) | Present: `WorldSession.h:541` (`PlayerBotEntry` + `ai`) | Reuse the existing entry; no new session identity in phase A |
| H2 | AI update dispatch | `PlayerScript::OnAIUpdate` (candidate `ScriptObjects.h:157-178`); `PlayerbotPlayerScript::IsAIUpdateDue/OnAIUpdate` (`PlayerbotScripts.cpp:296,312`); `Map::UpdatePlayerAI` (`Map.h:357`, `Map.cpp:1396`, call sites `1600,1627`) | Absent: no `OnAIUpdate`/`UpdatePlayerAI` in our `ScriptObjects.h`, `World.h`, `Map.h` | **Separate review (D5).** Phase A drives AI from the existing owner context without importing the map-owned scheduler |
| H3 | Startup/population entry | `World::InitPlayerbotsAtStartup` (`World.h:972`, `World.cpp:2540`; `World.cpp:2437`) | Absent | Add a bounded init point gated on game-data readiness; enforce tiny cohort limits |
| H4 | Save/ownership contract | Load bypass removed (candidate `Player.cpp:16831`); saves under session account IDs | Guard at `Player.cpp:18211`; load bypass at `Player.cpp:16604` | TW-004 contract first; contract and save changes ported atomically (D4) |
| H5 | Build boundary | `BUILD_PLAYERBOTS` (candidate `CMakeLists.txt:76-91`); `mod-playerbots.cmake` (Boost 1.70 thread/filesystem/system; `botpch.h` force-include of `cmangos-compat-shim.h` on every module TU; defs `CMANGOS MANGOSBOT_ZERO ENABLE_PLAYERBOTS`; three include roots; conf generation) | Probe: `docker/bot-candidate.Dockerfile` | The candidate evaluates as its own build target; the shim/PCH is the compatibility boundary and must reach every module TU |

### Excluded changes

- Whole-branch import and its core/content deltas (D1): the `World`, `Map`,
  `Player` header/source and `WorldSession` branch diffs stay out of scope.
- Map-owned AI scheduling and any pool/concurrency rework (D5): separate
  review, not part of the module import.
- Auction/mail automation (`modules/mod-playerbots/src/ahbot/**`): prohibited
  by the PRD constraints.
- Candidate runtime defaults (4,000-bot min/max, auto-creation/autologin,
  auction automation): never run on the personal realm.
- `mod-dungeon-clear` and the candidate build's other module dependencies.
- Eluna and optional modules (probe sets `BUILD_ELUNA=OFF`, `MODULES=disabled`).

### Unresolved runtime proof (required before the D2 gate passes)

1. Candidate server link: unverified (TW-001: no `tortoise-bot-candidate`
   image, no link/export record) - a clean `server`-stage build is required.
2. Candidate architecture tests: unexecuted (TW-001: configure broken; the
   four targeted regressions were never built) - the `checks` stage must
   pass after the test-side repair.
3. No live-bot experiment, save/persistence proof or telemetry exists for
   either engine (TW-003 through TW-011 produce them).
4. Bundled-engine source hazards (initialization/null ordering, lifecycle
   counts, population boundaries) are unfixed in-tree (TW-008/TW-009/TW-012).

## Consequences

- Phase A (no core scheduling change) keeps the personal core diff small and
  reviewable: ownership contract, save enablement, bounded init, and
  owner-context AI drive.
- Phase B (after the D5 review) may adopt the candidate scheduling pair only
  with the concurrency evidence that review requires.
- If the D2 gate fails (build or regressions), the bundled engine is the only
  path for this wave, and the candidate is demoted to a code reference.
