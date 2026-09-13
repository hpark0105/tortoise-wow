# AzerothCore reference review

Reviewed 2026-09-13 for `feature/kap-543-bot-living-world`.

## Decision

Do not switch this server to AzerothCore and do not install an AzerothCore
module directly. AzerothCore targets the 3.3.5a client, while this server and
its data target Turtle 1.18.1. Core hooks, packet layouts, spells, talents,
quests, schemas and module loaders differ. Use the projects below as bounded
design and test references, then implement against current Tortoise source and
prove behavior in disposable Turtle labs.

The old `azerothcore/mod-playerbots` repository is explicitly marked not ready
to use as a module. The actively maintained implementation is
`mod-playerbots/mod-playerbots` in a separate GitHub organization, and it
requires that project's custom AzerothCore fork. It is therefore a reference,
not a dependency candidate for this branch.

## Ranked references

| Project | Value here | What to reuse | What not to copy |
|---|---|---|---|
| [mod-playerbots/mod-playerbots](https://github.com/mod-playerbots/mod-playerbots) | High | Strategy/trigger/action separation, explicit bot roles, owner commands, random-versus-alt bot distinction, paced activation, active-alone rotation, force-active rules and overload feedback | Whole module, WotLK spell/quest tables, packet/core glue, automatic gear/progression shortcuts |
| [azerothcore/AzerothGhost](https://github.com/azerothcore/AzerothGhost) | High for scale testing | Orchestrator/node separation, spawn-rate limits, scripted scenarios, structured validation logs and distributed load reporting | Its 3.3.5a protocol client, AzerothCore auth schema and direct account provisioning |
| [azerothcore/azerothcore-wotlk](https://github.com/azerothcore/azerothcore-wotlk) | Medium | CI discipline, database update organization, configuration documentation and narrow subsystem boundaries | Core replacement or WotLK gameplay behavior |
| [AzerothCore getting started](https://www.azerothcore.org/wiki/getting-started) | Medium | Docker as a reproducible local setup, database-first content work, and event/action/target behavior as a data-driven pattern | Connecting AzerothCore-only tools directly to the Turtle schema |
| [azerothcore/mod-autobalance](https://github.com/azerothcore/mod-autobalance) | Medium, later | Measured instance scaling, per-map diagnostics and config reload patterns | Using stat scaling to hide weak companion tactics or broken content |
| [azerothcore/mod-ah-bot](https://github.com/azerothcore/mod-ah-bot) | Medium, optional | Seller/buyer separation, quotas, bounded market maintenance and a dedicated non-played identity | Enabling autonomous auctions before inventory disposal and economy limits are designed |
| [azerothcore/mod-solocraft](https://github.com/azerothcore/mod-solocraft) | Low, optional | A fallback comparison for solo/small-group dungeon tuning | Hidden player buffs in companion gameplay acceptance tests |
| [azerothcore/Keira3](https://github.com/azerothcore/Keira3) | Low for runtime; useful UX reference | Generated, reviewable SQL and visual editing concepts for quests/creatures/event-action behavior | Direct database connection: its editors assume AzerothCore tables |
| [azerothcore/mod-solo-lfg](https://github.com/azerothcore/mod-solo-lfg) | Low | Small-party queue acceptance ideas | WotLK Dungeon Finder code, which the 1.18.1 client does not provide |
| [azerothcore/mod-playerbots](https://github.com/azerothcore/mod-playerbots) | Reject as implementation source | Historical provenance only | The repository says it is not ready as a module and contains an old core fork |

## Concrete design inputs

### Companion controller

Adopt the maintained playerbots project's conceptual split: observations fire
triggers, strategies nominate actions, and an engine selects a legal action by
priority. Keep the first Tortoise version much smaller:

1. Server state creates typed observations; chat or planner text never becomes
   an executable action.
2. A deterministic role policy chooses from allowlisted actions.
3. The world thread revalidates ownership, party epoch, goal sequence, target,
   spell, range and resource state immediately before execution.
4. Stop/stay increments the goal sequence and invalidates queued work.

Use one shared party coordinator. Tank, healer and damage policies consume the
same party snapshot; they do not run independent planners that can disagree
about the pull.

### Ambient population

The maintained playerbots configuration distinguishes persistent identities
from active work, rotates an active-alone subset, forces bots active for group
or combat conditions, paces new bots per interval, and can reduce optional
activity as update time rises. These are directly relevant to POP-01 through
POP-03, with one product correction: this project must preserve a measured
distant-world activity floor instead of pausing every bot away from the human.

### Load validation

Keep the existing in-process ten-bot cohort as the truth for gameplay and
persistence. Borrow AzerothGhost's scenario-runner shape for higher scale:
scenario seed, requested and achieved count, paced activation, duration,
structured actions/errors, server metrics and a clear degraded result. A later
spike may determine whether a small Turtle 1.18.1 protocol load client is worth
building. It must never be counted as a gameplay bot until its behavior and
persistence paths are equivalent to an in-server bot session.

### World content and economy

Use event/action/target tables where content-specific behavior can remain data
driven, and C++ for reusable player actions and coordination. An auction bot is
a later atmosphere feature. AutoBalance or SoloCraft-style scaling is a
separate product decision after one normal dungeon exposes actual difficulty;
neither is part of the companion correctness gate.

## Provenance and maintenance rules

- Record the exact upstream commit and license before copying any code.
- Prefer reimplementation from documented behavior and tests over importing
  WotLK-dependent files.
- Any copied GPL code must retain its required notices and remain compatible
  with this repository's license.
- Port one behavior at a time and prove it with Turtle data, spells and normal
  game paths.
- Keep upstream references out of the runtime dependency graph.

## Resulting execution order

The machine queue in `ralph/prd.json` and the cards under
`ralph/post-mvp/` break the useful ideas into bounded work. The next dependency-
ready implementation is CMP-010: real group membership and a persistent
companion roster. Role tactics follow only after that boundary is proven.
