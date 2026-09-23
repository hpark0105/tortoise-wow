# BL-008 runtime integration evidence card v1

## Objective

Audit and, only if safely bounded, implement the owner-only `.botlearn` command registration and state transition seam using the existing companion ownership and learning profile APIs.

## Repository and provenance

- Repository: `tortoise-wow`
- Branch: `feature/kap-558-port-phase2`
- Indexed/base commit: `c5781974b444f6ad692b48a516b0994d8cf59bc5`
- Current worktree contains uncommitted BL-001A through BL-007 work. Preserve it.

## Allowed files

- `src/game/Chat/Chat.cpp`
- `src/game/Chat/Chat.h`
- `src/game/PlayerBots/PlayerBotMgr.cpp`
- `src/game/PlayerBots/PlayerBotMgr.h`
- focused new/updated command value test under `docker/`
- this card

Do not edit SQL, learning persistence, combat, planner, PRD, or handoff files. Do not commit.

## Required behavior

- Register one `SEC_PLAYER` command family: `.botlearn start|status|pause|resume|explain|rollback Bram`.
- Resolve the companion through the established ownership path; intruders and unowned targets fail closed.
- Hold/orders continue to win. Pause and rollback must immediately select baseline and invalidate outstanding tactical work.
- Status/explain must say when evidence is insufficient; never claim improvement without accepted evidence.
- If current APIs cannot support a subcommand safely, report the exact missing seam and do not invent direct SQL or broad state.

## Validation

Run the narrow command/value test if added, `git diff --check`, and report whether a world build is required. No realm startup, DB mutation, or personal data.

## Safety attestations

Do not use retrieval, secrets, raw logs, personal character data, destructive Git commands, or nested model sessions. Do not restore or overwrite unrelated worktree files.
