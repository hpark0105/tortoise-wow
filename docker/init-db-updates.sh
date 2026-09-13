#!/bin/bash
# Apply ordered database updates to a fresh data directory (initdb.d hook).
#
# 10-tortoise.sh imports the sql/base dumps. This hook then applies the
# ordered sql/database_updates files (glob order matches
# Database.AutoUpdate.SortByName=1) and records each file's SHA-1 in the
# per-database `migrations` table, mirroring src/shared/Database/
# AutoUpdater.cpp. mangosd's DB auto-updater then finds nothing pending at
# world start, so fixture seeds applied by the test harness after db boot
# survive world load instead of being clobbered by replayed updates.
(
set -euo pipefail

export MYSQL_PWD="$MARIADB_ROOT_PASSWORD"

declare -A DB_FOR_FOLDER=( [auth]=tw_logon [character]=tw_char [world]=tw_world )

create_migrations_table() {
    # Mirror AutoUpdater.cpp: tolerate the legacy table shipped in the base
    # dump (no Module column) by adding it, as the C++ code does at runtime.
    local db="$1"
    mariadb --user=root "$db" <<'SQL'
CREATE TABLE IF NOT EXISTS `migrations` (
    `Id` INT(10) UNSIGNED NOT NULL AUTO_INCREMENT,
    `Name` VARCHAR(255) NOT NULL DEFAULT '0' COLLATE 'utf8_general_ci',
    `Module` VARCHAR(255) NOT NULL DEFAULT '' COLLATE 'utf8_general_ci',
    `Hash` VARCHAR(128) NOT NULL DEFAULT '0' COLLATE 'utf8_general_ci',
    `AppliedAt` DATETIME NOT NULL,
    PRIMARY KEY(`Id`) USING BTREE
)
COLLATE = 'utf8_general_ci'
ENGINE = InnoDB;
SQL
    if [[ -z "$(mariadb --user=root -N -B "$db" -e "SHOW COLUMNS FROM \`migrations\` LIKE 'Module'")" ]]; then
        mariadb --user=root "$db" -e "ALTER TABLE \`migrations\` ADD COLUMN \`Module\` VARCHAR(255) NOT NULL DEFAULT '' COLLATE 'utf8_general_ci' AFTER \`Name\`"
    fi
}

for folder in auth character world; do
    db="${DB_FOR_FOLDER[$folder]}"
    dir="/bootstrap/sql/database_updates/$folder"
    [[ -d "$dir" ]] || continue
    create_migrations_table "$db"
    shopt -s nullglob
    for file in "$dir"/*.sql; do
        name="$(basename "$file" .sql)"
        # AutoUpdater's ByteArrayToHexStr is uppercase; key lookup is case-sensitive.
        hash="$(sha1sum "$file" | cut -d' ' -f1 | tr 'a-f' 'A-F')"
        recorded="$(mariadb --user=root -N -B "$db" -e "SELECT COUNT(*) FROM \`migrations\` WHERE \`Hash\` = '"$hash"'")"
        if [[ "$recorded" -gt 0 ]]; then
            continue
        fi
        echo "Applying database update $db/$name"
        mariadb --user=root "$db" < "$file"
        mariadb --user=root "$db" -e "INSERT INTO \`migrations\` (\`Name\`, \`Module\`, \`Hash\`, \`AppliedAt\`) VALUES ('"$name"', '', '"$hash"', NOW())"
    done
done

unset MYSQL_PWD
)
