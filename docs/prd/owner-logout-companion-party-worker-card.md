# Owner logout companion-party cleanup — worker card v1

- Card ID/version: TW-OWNER-PARTY-LOGOUT-v1
- Objective: add bounded cleanup so normal owner logout disbands a party made
  only of that owner and companions owned by that account, preventing bot-led
  stale parties on the next login.
- Jira reference: none.
- Repository and current branch/commit: `tortoise-wow`,
  `feature/kap-558-port-phase2`, `c5781974b444f6ad692b48a516b0994d8cf59bc5`.
- Allowed read files/functions: `src/game/WorldSession.cpp::LogoutPlayer`,
  `src/game/PlayerBots/PlayerBotMgr.h`,
  `src/game/PlayerBots/PlayerBotMgr.cpp::{FindBotByGuid,BotRecruit,BotDismiss}`,
  `src/game/Group/Group.h` member-list/disband declarations, and one focused
  test file under `docker/`.
- Allowed edit files: `src/game/WorldSession.cpp`,
  `src/game/PlayerBots/PlayerBotMgr.h`, `src/game/PlayerBots/PlayerBotMgr.cpp`,
  and one focused test under `docker/`. Do not edit other files.
- Verified evidence: `WorldSession.cpp` hash
  `f087d393f6abc25ccfed8ef9736bdd8be129eb95` removes a normally logging-out
  non-raid player at lines 768-778; `PlayerBotMgr.cpp` hash
  `65af79ba6db9e336eab34ffb8600d1dd37b14831` documents at lines 2406-2422
  that owner logout transfers leadership and can persist a bot-only group;
  `PlayerBotMgr.h` hash `ec873beaffb7908b6d5fac21cf84d8d7393e9471`;
  `Group.h` hash `5f7e9b73ec98ecd85d5cb356ecb53e7bdea7d289` exposes member slots and `Disband`.
- Retrieval: `tortoise-wow` healthy and fresh at base commit; bounded search
  identified `LogoutPlayer` and stale-group recovery as the relevant seams.
- Requirements: before the normal non-raid logout removal, disband only when
  every other group member is a registered companion owned by the logging-out
  account. Do not disband raids, battleground groups, solo/no-group state,
  groups containing another human, unregistered bots, or companions owned by
  another account. Preserve the normal logout path when the predicate fails.
  Avoid invalidating a group pointer and then using it.
- Dependencies: existing ownership binding in `PlayerBotEntry::ownerAccountId`;
  existing `Group::Disband`; no database or schema change.
- Validation: focused value/static test, then report whether full
  `docker compose build world` is required. Do not start or restart the realm.
- Expected output: bounded patch, tests actually run, changed paths, hashes,
  and uncertainty.
- Stop boundary: any need to change generic group semantics, persistence,
  database schema, or affect mixed-human groups.

You are a bounded local worker, not the head. Do not call retrieval or Jira,
launch park-agent/park-head or nested models, choose architecture, deploy,
publish, transition issues, edit acceptance flags or declare final acceptance.
Do not read secrets, environment maps, private game records, raw logs, backups
or transcripts. Preserve every unrelated worktree change.

Safety attestation by the head: the evidence is bounded, current, contains no
prohibited material, the edit/test scope is authorized, and no other local
editing worker is active.
