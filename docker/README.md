# Personal Docker server

This setup follows the root README: Ubuntu 22.04, the bundled SQL schema and
base data, then the world server's ordered migrations. Optional custom features
belong under `modules/`; the image builds discovered modules statically.
Gameplay rates come from the upstream config. Character transfers and player
bots are disabled initially.

The world service accepts these optional `.env` settings for the playable
companion implementation. Defaults keep bots disabled:

```dotenv
PLAYERBOT_ENABLE=1
PLAYERBOT_MIN_BOTS=1
PLAYERBOT_MAX_BOTS=1
PLAYERBOT_PROVISION=Companion
PLAYERBOT_DEBUG=1
PLAYERBOT_QUEST_ID=0
PLAYER_SAVE_INTERVAL=60000
```

`PLAYERBOT_PROVISION` is a new bot character name. It must differ from an
existing player character name. Keep `PLAYERBOT_QUEST_ID=0` for ordinary play;
quest 456 is the single bounded automated quest used by the MVP lab.
The build explicitly enables `-DALLOW_TURTLE_ADDONS=ON` for Turtle client addon
support, as required for this installation.

## First start on Windows

Start Docker Desktop with Linux containers, then run from the repository root:

```powershell
./docker/setup.ps1
docker compose build world
docker compose up -d db
docker compose exec -T db bash /ops/check-db.sh
```

Wait for `docker compose ps` to show the database as healthy before checking it.
The first database boot imports the schema and every `sql/base/*.sql` file.
The image also includes the matching map/vmap/mmap extraction tools.
Two compiler jobs are used by default; `BUILD_JOBS` in `.env` controls this.

The world server needs data extracted from the exact client specified in the
root README: Turtle 1.18.1 build 7272 with the April 12, 2026 hotfixes. Do not
assume that a successor server's client has identical DBCs. A candidate archive
must be verified against `tools/dbc_verification/dbc_verifier.py`.
That script runs in the directory containing the DBC files and prints mismatch
and missing-file messages; **its exit code alone does not indicate a match**.

After obtaining and unpacking a candidate client, run:

```powershell
./docker/extract-client.ps1 -ClientPath 'C:\Games\Turtle-1.18.1' -VerifyOnly
./docker/extract-client.ps1 -ClientPath 'C:\Games\Turtle-1.18.1'
```

The first command extracts only DBCs and fails if any DBC differs from the
repository manifest. The second repeats that check, then extracts maps, collision
geometry and navigation meshes. Full navigation generation can take hours.
The client directory is mounted read-only; generated data goes to the repository's
ignored `data/` directory. The non-DBC `hashes.txt` entry in the upstream verifier
is excluded from the DBC comparison.
Verification-only runs use a fresh ignored `local/dbc-check-*` folder. Full
extraction refuses to overwrite an existing extraction, preventing files from
different client versions being mixed. Move old extraction folders aside before
replacing them; keep the character database volume in place.

The resulting local directories are:

```text
data/dbc/
data/maps/
data/vmaps/
data/mmaps/
```

The world entrypoint refuses to start if any directory is empty or missing,
or if a DBC differs from the repository manifest. The DBC hashes are checked
on every start.
Keep collision and pathfinding data enabled for gameplay correctness.
The full game client runs on Windows; only the server runs in Docker.

Once client data is ready:

```powershell
docker compose up -d
docker compose logs --tail 50 world
docker compose attach world
```

In the server console, create your account using `account create NAME PASSWORD`.
Use a dedicated local game password. Detach with **Ctrl+P, Ctrl+Q**; Ctrl+C can
stop the server. Do not enable GM privileges for your ordinary play account
if you want normal gameplay behavior.

In a separate local copy of the matching client, set `realmlist.wtf` to:

```text
set realmlist 127.0.0.1
```

Start the client executable directly so a successor-server launcher does not
replace the server address or modify this client copy.
Login listens on localhost port 3724; the game server uses localhost port 8085.
The database port is not published. These settings are for this PC only.

## Persistence and shutdown

Accounts, characters, inventory, quests, and world state are stored in the
`tortoise-local_database` Docker volume. Separate volumes hold server logs,
honor files and character dumps. Rebuilding images or using `docker compose down`
does not delete these volumes. **Do not use `docker compose down -v`, volume
pruning, or Docker Desktop's data reset on your play installation.**

Log out before stopping. Use `docker compose stop` for an orderly shutdown;
the server gets up to two minutes to exit. The upstream periodic character-save
interval is 60 seconds, so abrupt host failures can still lose recent progress.

The generated `.env` contains database credentials and is ignored by Git.
Keep it with your private backups. Re-running setup preserves it; changing its
passwords does not change passwords in an already initialized database.
Container configs are generated from upstream templates on each start. Change
the overrides in `docker/server.py`, then rebuild, rather than editing a
generated config inside a running container.

## Backups and updates

```powershell
./docker/backup.ps1
```

This stops any running game/login services, creates a SQL dump and SHA-256 file
under `local/backups/`, then restarts only the services that were running.
The dump includes all four game databases, triggers, routines, and events.
It uses a global read lock because the repository mixes MyISAM and InnoDB.
Copy successful backups off this PC; a Docker volume is persistence, not a backup.

Test the latest backup without touching your play database:

```powershell
./docker/test-restore.ps1
```

This checks the backup checksum, imports it into a uniquely named disposable
Compose database, checks the schema and baseline world content, and removes that
test project's volume afterward. It does not prove in-game character behavior.

Before pulling upstream updates, back up and record `git rev-parse HEAD`.
Then rebuild and start the services. World startup applies SQL migrations;
reverting an image alone does not reverse a database migration.
Restore testing should use a separate Compose project and fresh database volume,
never overwrite your only character database. Restore the dump there using the
MariaDB client, inspect the character and inventory records, and test login
before switching your play installation to it.

## Validation

The local/CI smoke checks cover compilation, database bootstrap, and persistence
across database-container recreation. They do not contain the game client or
prove gameplay correctness. The first in-game acceptance check is to create a
character, earn XP, loot/equip an item, accept/complete a quest, log out, restart
the containers, and verify those records and your position after logging back in.
Then check combat, death/recovery, vendors, travel, and one dungeon before bots.

See [bot feasibility](bots.md) for the proposed LLM work and remaining decisions.
See [content repair status](content-status.md) for repaired startup warnings,
remaining gameplay gaps, and the opt-in disposable content integration check.
