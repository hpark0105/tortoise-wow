#!/bin/bash
set -euo pipefail
export MYSQL_PWD="$MARIADB_ROOT_PASSWORD"
query() { mariadb --user=root --batch --skip-column-names -e "$1"; }
case "${1:-check}" in
    check)
        [[ "$(query "SELECT COUNT(*) FROM tw_world.creature_template")" -gt 0 ]]
        [[ "$(query "SELECT COUNT(*) FROM tw_world.quest_template")" -gt 0 ]]
        # World startup rewrites realmbuilds; realmd advertises its compiled Turtle build.
        [[ "$(query "SELECT COUNT(*) FROM tw_logon.realmlist WHERE id=1 AND address='127.0.0.1' AND port=8085")" == 1 ]]
        query 'SELECT COUNT(*) AS characters_present FROM tw_char.characters' >/dev/null
        echo 'Database bootstrap and local realm checks passed.'
        ;;
    mark)
        query 'CREATE TABLE IF NOT EXISTS tw_logs.tortoise_docker_probe (id INT PRIMARY KEY); INSERT IGNORE INTO tw_logs.tortoise_docker_probe VALUES (1);'
        ;;
    verify)
        [[ "$(query 'SELECT COUNT(*) FROM tw_logs.tortoise_docker_probe WHERE id=1')" == 1 ]]
        query 'DROP TABLE tw_logs.tortoise_docker_probe'
        echo 'Database persistence check passed.'
        ;;
    *) echo 'Expected check, mark, or verify' >&2; exit 2 ;;
esac
