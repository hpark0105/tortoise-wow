# BOTLEARN-START-ALL worker evidence card v1

## Objective

Implement the bounded shortcut `.botlearn start all`. It must enable learning
for every persistent companion whose `ownerAccountId` matches the issuing
player's account, using one serialized asynchronous learning-store operation.

## Repository and provenance

- Repository: `C:\Users\hpark\WebstormProjects\tortoise-wow`
- Branch: `feature/kap-558-port-phase2`
- Commit: `c5781974b444f6ad692b48a516b0994d8cf59bc5`
- `src/game/Commands/Commands.cpp`: `99513E989D27E9F6BB9293D167D2CE8B014830E1BB423DDB9E9949F498D4A1D5`
- `src/game/PlayerBots/PlayerBotMgr.cpp`: `9B161452828DCD8C8734609463D1D69F172186E56BC0A3B2B0A8D5ACFF3ACCAF`
- `src/game/PlayerBots/PlayerBotMgr.h`: `3F4ABE637EEC4214BF82F017F767F53D77384800B8E1A6725AB546C8EF96125B`
- `src/game/PlayerBots/Companion/LearningStore.h`: `B35A44FF1CA10CBC399B6C7CA58243EC66F9B6F8E4C73E7A3F46A779DC4C8875`

Retrieval was healthy/fresh and identified `HandleBotLearnCommand`,
`PlayerBotMgr::BotLearn`, and the Store's bounded maintenance queue. Current
source was opened and hashes verified by the hosted head.

## Required behavior

- `.botlearn start all` is accepted case-insensitively for the target token.
- It operates only on persistent registered companions owned by the issuer's
  account. Never affect unowned bots or another account's companions.
- Use one SQL statement queued through `Companion::Learning::Store`; do not
  loop through per-bot `QueueControl`, because its maintenance capacity is four
  and a larger roster would be partially applied.
- Preserve idempotent start semantics: insert missing profiles in Observe mode;
  change Disabled profiles to Observe; leave all other modes unchanged.
- Other actions with `all` must fail closed with a clear message. Individual
  `.botlearn <action> <botname>` behavior must remain unchanged.
- Report an honest owned-companion candidate count and that the batch was
  queued, not completed.
- Add a focused static/value test under `docker/`.

Suggested SQL shape: `INSERT ... SELECT` from `bot_ownership` joined to
`playerbot`, filtered by numeric `owner_account_id`, with the same
`ON DUPLICATE KEY UPDATE` semantics as the existing start template. Keep SQL
construction inside `LearningStore.h` and serialize via `EnqueueMaintenance`.

## Allowed edits

- `src/game/Commands/Commands.cpp`
- `src/game/PlayerBots/PlayerBotMgr.cpp`
- `src/game/PlayerBots/PlayerBotMgr.h`
- `src/game/PlayerBots/Companion/LearningStore.h`
- one new focused `docker/test_botlearn_start_all_value.py`

Do not edit any other tracked source. Do not commit, deploy, alter Jira, or
touch `.idea/`.

## Validation

- Run the focused Python test.
- Run `git diff --check` on allowed files.
- Report changed files, commands/results, assumptions, and uncertainties.
- Full `docker compose build world`, diff acceptance, deployment, runtime and
  retrieval validation remain owned by hosted Codex.

## Safety attestations

- No credentials, environment maps, personal character data, backups, or raw
  logs are included.
- The worker has no architecture, release, Jira, deployment, or final
  acceptance authority.
