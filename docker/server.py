"""Render container-specific settings over the repository's config templates."""
import os
from pathlib import Path
import re
import sys

from verify_dbc import load_hashes, verify


def render(template, overrides):
    remaining = dict(overrides)
    output = []
    for line in template.splitlines():
        match = re.match(r"^\s*([\w.]+)\s*=", line)
        if match and match[1] in overrides:
            key = match[1]
            output.append(f"{key} = {overrides[key]}")
            remaining.pop(key, None)
        else:
            output.append(line)
    output.extend(f"{key} = {value}" for key, value in remaining.items())
    return "\n".join(output) + "\n"


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "world"
    if mode not in ("world", "realm"):
        sys.exit("Expected world or realm")
    password = os.environ.get("DB_PASSWORD", "")
    if not re.fullmatch(r"[a-fA-F0-9]{32,}", password):
        sys.exit("DB_PASSWORD must contain at least 32 hexadecimal characters; run docker/setup.ps1")
    for directory in ("logs", "honor", "pdump", "patches"):
        Path("/state", directory).mkdir(exist_ok=True)
    values = {
        "LogsDir": '"/state/logs"',
        "BindIP": '"0.0.0.0"',
        "WaitAtStartupError": "0",
    }
    connection = lambda database: f'"db;3306;tortoise;{password};{database}"'
    if mode == "world":
        missing = [name for name in ("dbc", "maps", "vmaps", "mmaps")
                   if not Path("/data", name).is_dir() or not any(Path("/data", name).iterdir())]
        if missing:
            sys.exit("Missing extracted client data: " + ", ".join(missing)
                     + ". See docker/README.md. Character data has not been reset.")
        errors = verify(Path("/data/dbc"), load_hashes(
            Path("/opt/tortoise/dbc_verification/dbc_verifier.py")))
        if errors:
            sys.exit("Client DBC verification failed:\n" + "\n".join(errors)
                     + "\nUse data matching the repository manifest. See docker/README.md.")
        executable = "mangosd"
        values.update({
            "RealmID": "1", "WorldServerPort": "8085",
            "DataDir": '"/data"', "HonorDir": '"/state/honor"',
            "PDumpDir": '"/state/pdump"', "HttpApi.Enable": "0",
            "Database.AutoUpdate.Path": '"/opt/tortoise/sql/database_updates"',
            "Database.AutoUpdate.SortByName": "1",
            "PlayerBot.Enable": "0", "PlayerBot.MinBots": "0", "PlayerBot.MaxBots": "0",
        })
        for kind, database in (("Login", "tw_logon"), ("World", "tw_world"),
                               ("Character", "tw_char"), ("Logs", "tw_logs")):
            values[f"{kind}Database.Info"] = connection(database)
    else:
        executable = "realmd"
        values.update({"LoginDatabaseInfo": connection("tw_logon"),
                       "RealmServerPort": "3724", "PatchesDir": '"/state/patches"'})
    template = Path(f"/opt/tortoise/etc/{executable}.conf.dist").read_text()
    config = Path(f"/state/{executable}.conf")
    os.umask(0o077)
    config.write_text(render(template, values))
    os.execv(f"/opt/tortoise/bin/{executable}", [executable, "-c", str(config)])


if __name__ == "__main__":
    main()
