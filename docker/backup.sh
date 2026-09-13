#!/bin/bash
set -euo pipefail
export MYSQL_PWD="$MARIADB_ROOT_PASSWORD"
stamp=$(date -u +%Y%m%dT%H%M%SZ)
temporary="/backups/tortoise-${stamp}.sql.partial"
destination="/backups/tortoise-${stamp}.sql"
# Global read lock covers both MyISAM and InnoDB tables. Do not substitute
# --single-transaction: this repository contains many MyISAM tables.
mariadb-dump --user=root --lock-all-tables --routines --events --triggers \
    --databases tw_logon tw_char tw_world tw_logs > "$temporary"
mv "$temporary" "$destination"
sha256sum "$destination" > "$destination.sha256"
echo "Backup saved: $destination"
