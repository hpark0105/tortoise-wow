# LLM-directed bots: feasibility review

The repository already implements `PlayerBotMgr` and `PlayerBotAI` under
`src/game/PlayerBots`. Population settings are `PlayerBot.MinBots` and
`PlayerBot.MaxBots`. The manager loads an existing character roster from the
character database's `playerbot` table; raising a limit does not create that
many characters automatically. Its startup also adjusts limits against roster
size, so an exact target count needs testing and potentially a small correction.

The default AI handles teleport acknowledgments, nearby combat, random wandering,
spell learning, and equipment selection. The reviewed update loop does not
establish reliable group roles, healing, quest completion, looting, or recovery
after death. It returns immediately when dead. Its `_abilityTimer` also lacks
constructor initialization in the reviewed header. These are reasons to validate
and improve ordinary bot behavior before attaching an LLM.

## Proposed approach

Start with four persistent party companions (tank, healer, two damage dealers)
as a proposed test size, subject to the player's preferred class and bot count.
Keep normal server AI responsible for movement, combat timing and spell legality.
A module can provide chat/personality, while any missing bot execution hooks may
need a narrowly scoped core change. `ScriptObjects.h` exposes world-update,
player-update, chat, login, death, and quest-completion hooks, which are useful
integration points. Bot ownership and execution hooks still need checking before
claiming that the full feature can live entirely in a module.

An asynchronous service would receive compact, bounded game-state snapshots and
return validated goals such as follow, assist, defend, rest, or travel to a known
quest location. Apply accepted actions on the world thread. Never wait for model
inference on that thread, and do not let model output run SQL, shell commands, GM
commands or arbitrary spells. Use deadlines, bounded queues and a global request
budget; keep ordinary AI running if the model is unavailable. Ignore responses
for stale sessions, changed targets or expired actions.

Store each bot's roster identity and optional conversation/goal memory locally.
Make the population configurable separately from LLM concurrency. One shared
model can serve multiple bots; more bots do not imply one model process each.
Measure inference latency and server tick time before promising a maximum count.
The existing shared local model may be occupied by coding sessions, so its
availability and resource budget need to be explicit.

## References reviewed September 10, 2026

- [bigr00/mod-llm-playerbots](https://github.com/bigr00/mod-llm-playerbots)
  is an AzerothCore fork that separates ordinary combat from LLM social/agenda
  decisions. It is an architectural reference, not verified compatible code.
- [Merrymak3r/wow-llm-personas](https://github.com/Merrymak3r/wow-llm-personas)
  adds local-model personalities and memory to CMaNGOS/playerbots. Personality
  integration alone does not establish autonomous quest/dungeon competence here.

No external bot module has been installed and no LLM integration is implemented.
Next decisions: companion party versus autonomous population, initial count,
player class/role, and local model versus hosted provider.
