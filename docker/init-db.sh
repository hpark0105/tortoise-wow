#!/bin/bash
# MariaDB may source a non-executable .sh file. Isolate shell options and env.
(
set -euo pipefail

# Official MariaDB entrypoint executes this only for a new data directory.
# Refuse unsafe interpolation; setup.ps1 generates hexadecimal passwords.
[[ "${DB_PASSWORD:-}" =~ ^[a-fA-F0-9]{32,}$ ]] || {
    echo 'DB_PASSWORD must contain at least 32 hexadecimal characters.' >&2
    exit 1
}
export MYSQL_PWD="$MARIADB_ROOT_PASSWORD"
mariadb --user=root < /bootstrap/sql/create_databases.sql
for migration in /bootstrap/sql/base/*.sql; do
    echo "Importing $(basename "$migration")"
    mariadb --user=root tw_world < "$migration"
done
mariadb --user=root <<SQL
CREATE USER 'tortoise'@'%' IDENTIFIED BY '${DB_PASSWORD}';
GRANT ALL PRIVILEGES ON tw_logon.* TO 'tortoise'@'%';
GRANT ALL PRIVILEGES ON tw_char.* TO 'tortoise'@'%';
GRANT ALL PRIVILEGES ON tw_world.* TO 'tortoise'@'%';
GRANT ALL PRIVILEGES ON tw_logs.* TO 'tortoise'@'%';
INSERT INTO tw_logon.realmlist
    (id, name, address, port, icon, realmflags, timezone, realmbuilds)
VALUES (1, 'Tortoise Local', '127.0.0.1', 8085, 0, 2, 1, '7272');
SQL
unset MYSQL_PWD
)
