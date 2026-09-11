"""Opt-in integration check using local extracted assets and disposable services.

Run: python docker/check_content_startup.py --image tortoise-local:content-fixes
No personal database, account, or client archive is copied into the test project.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess
import time
import uuid


ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', default='tortoise-local:content-fixes')
    args = parser.parse_args()
    project = 'tortoise-content-check-' + uuid.uuid4().hex[:8]
    evidence = ROOT / 'local' / project
    evidence.mkdir(parents=True)
    override = evidence / 'compose.yaml'
    override.write_text('services:\n  world:\n    image: ' + json.dumps(args.image)
                        + '\n    ports: !reset []\n', encoding='utf-8')
    base = ['docker', 'compose', '-f', str(ROOT / 'compose.yaml'), '-f', str(override), '-p', project]

    def run(*command, script=None):
        result = subprocess.run(base + list(command), cwd=ROOT,
                                input=script.encode() if script else None,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if result.returncode:
            raise RuntimeError('Compose command failed: ' + ' '.join(command)
                               + '\n' + result.stderr.decode(errors='replace')[-1200:])
        return result.stdout.decode(errors='replace')

    def sql(query):
        # SQL text goes through stdin; passwords remain inside the database container.
        return run('exec', '-T', 'db', 'bash', script=
                   'set -e\nexport MYSQL_PWD="$MARIADB_ROOT_PASSWORD"\n'
                   "mariadb --user=root --batch --skip-column-names tw_world <<'CONTENT_SQL'\n"
                   + query + '\nCONTENT_SQL\n').strip()

    def ready(label):
        deadline = time.monotonic() + 240
        while time.monotonic() < deadline:
            logs = run('logs', '--no-color', 'world')
            (evidence / (label + '.log')).write_text(logs, encoding='utf-8')
            if 'World server is up and running!' in logs:
                return logs
            if 'world' not in run('ps', '--services', '--status', 'running').splitlines():
                raise RuntimeError('Test world exited; inspect ' + str(evidence))
            time.sleep(2)
        raise RuntimeError('Test startup timed out; inspect ' + str(evidence))

    try:
        run('up', '-d', '--wait', '--wait-timeout', '900', 'db')
        print('Disposable database ready.', flush=True)
        run('up', '-d', '--no-deps', 'world')
        logs = ready('baseline')
        for warning in ('out of range broadcast text id',
                        'Table `broadcast_text` is missing text id 429',
                        'Script not found: npc_teslinah.', 'Script not found: 0.'):
            assert warning not in logs, warning
        assert not re.search(r"reference_loot_template.*entry (30559|30171|150112) .*not exist", logs)

        # Compare all restored loot columns against the repository's original definitions.
        pattern = r'\((30171|150112|30559),\s*(\d+),\s*(-?\d+(?:\.\d+)?),\s*(\d+),\s*(-?\d+),\s*(\d+),\s*(\d+)\)'
        expected = re.findall(pattern, (ROOT / 'sql/base/tw_world_reference_loot_template.sql').read_text())
        actual = [line.split('\t') for line in sql('SELECT entry,item,ChanceOrQuestChance,groupid,mincountOrRef,maxcount,condition_id FROM reference_loot_template WHERE entry IN (30171,150112,30559)').splitlines()]
        normalize = lambda rows: sorted(tuple(float(value) for value in row) for row in rows)
        assert len(expected) == 109 and normalize(actual) == normalize(expected), 'Restored loot differs from base definitions'
        assert sql('SELECT COUNT(*) FROM reference_loot_template r LEFT JOIN item_template i ON i.entry=r.item WHERE r.entry IN (30171,150112,30559) AND i.entry IS NULL') == '0'
        assert sql('SELECT COUNT(*) FROM reference_loot_template r LEFT JOIN conditions c ON c.condition_entry=r.condition_id WHERE r.entry IN (30171,150112,30559) AND r.condition_id<>0 AND c.condition_entry IS NULL') == '0'
        assert sql("SELECT COUNT(*) FROM creature_template WHERE script_name='0'") == '0'
        assert sql("SELECT COUNT(*) FROM gameobject_template WHERE script_name='0'") == '0'
        print('PASS: baseline startup, registration, placeholder cleanup and 109 exact loot rows.', flush=True)

        run('stop', 'world')
        good_positive = int(sql('SELECT MIN(entry) FROM broadcast_text WHERE entry>0'))
        good_negative = -1999883
        bad_positive, bad_negative = 2000000000, -19900000
        assert sql(f'SELECT COUNT(*) FROM script_texts WHERE entry={good_negative}') == '1'
        assert sql(f'SELECT COUNT(*) FROM broadcast_text WHERE entry={bad_positive}') == '0'
        assert sql(f'SELECT COUNT(*) FROM script_texts WHERE entry={bad_negative}') == '0'
        assert sql('SELECT COUNT(*) FROM creature_ai_scripts WHERE id BETWEEN 900001 AND 900006') == '0'
        sql(f'''INSERT INTO creature_ai_scripts (id,command,dataint,dataint2,comments) VALUES
          (900001,0,{good_positive},0,'positive fixture'),
          (900002,0,{good_negative},0,'negative fixture'),
          (900003,0,{bad_positive},0,'missing positive fixture'),
          (900004,0,{bad_negative},0,'missing negative fixture'),
          (900005,0,0,0,'mandatory zero fixture'),
          (900006,0,{good_positive},{good_negative},'mixed optional fixture');''')
        run('up', '-d', '--no-deps', '--force-recreate', 'world')
        logs = ready('fixtures')
        for script_id in (900001, 900002, 900006):
            assert not re.search(r'(?:missing text|invalid talk|out of range).*script id ' + str(script_id), logs)
        assert f'Table `broadcast_text` is missing text id {bad_positive}, used in database script id 900003.' in logs
        assert f'Table `script_texts` is missing text id {bad_negative}, used in database script id 900004.' in logs
        assert 'invalid talk text id (dataint = 0) in SCRIPT_COMMAND_TALK for script id 900005' in logs
        print('PASS: valid/missing positive and negative IDs, mandatory zero, and mixed optional slots.', flush=True)
        (evidence / 'result.json').write_text(json.dumps({'passed': True, 'image': args.image, 'loot_rows': len(actual), 'dialogue_fixtures': 6}, indent=2) + '\n')
    finally:
        # Only the explicitly generated disposable project can reach this cleanup.
        assert project.startswith('tortoise-content-check-')
        run('down', '--volumes')
    print('Evidence: ' + str(evidence), flush=True)


if __name__ == '__main__':
    main()
