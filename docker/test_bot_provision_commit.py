"""Actual SQL failures must never be reported as successful provisioning."""
import json
import unittest
import uuid
import test_bot_provision as p


class BotProvisionCommitFailureTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        p.command(['docker', 'image', 'inspect', p.IMAGE])
        cls.project = 'tortoise-bot-prov-' + uuid.uuid4().hex[:12]
        cls.evidence = p.ROOT / 'local' / cls.project
        cls.evidence.mkdir()
        cls.base = None
        cls.addClassCleanup(cls.cleanup_lab)
        seed = """
INSERT INTO characters (guid,account,name,race,class,gender,level,money,
position_x,position_y,position_z,map,orientation,zone,health,
power1,power2,power3,power4,power5)
VALUES (500090,1000500090,'Commitorphan',1,1,0,10,100000,
-8949.95,-132.493,83.5312,0,0,12,26,0,0,0,0,0);
CREATE TRIGGER review_reject_binding BEFORE INSERT ON bot_ownership
FOR EACH ROW SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='review injected binding failure';
"""
        cls.base, cls.env = p.boot_lab(cls.project, cls.evidence, cls.world('Commitfresh'), seed)
        cls.before = p.snapshot(cls.base, cls.env, 'Commitorphan')
        cls.fresh_logs = p.start_world_again(cls.base, cls.env, cls.evidence,
                                            cls.world('Commitfresh'), 'fresh-failure',
                                            'World server is up and running!')
        cls.fresh = p.snapshot(cls.base, cls.env, 'Commitfresh')
        cls.orphan_logs = p.start_world_again(cls.base, cls.env, cls.evidence,
                                             cls.world('Commitorphan'), 'orphan-failure',
                                             'World server is up and running!')
        cls.orphan = p.snapshot(cls.base, cls.env, 'Commitorphan')
        cls.engines = p.db_exec(cls.base, cls.env,
            "SELECT TABLE_NAME, ENGINE FROM information_schema.TABLES "
            "WHERE TABLE_SCHEMA='tw_char' AND TABLE_NAME IN ('characters','playerbot','bot_ownership')")
        (cls.evidence / 'table-engines.txt').write_text(cls.engines, encoding='utf-8')
        p.db_exec(cls.base, cls.env, 'DROP TRIGGER review_reject_binding;')
        cls.fresh_retry_logs = p.start_world_again(cls.base, cls.env, cls.evidence,
                                                  cls.world('Commitfresh'), 'fresh-retry',
                                                  'World server is up and running!')
        cls.fresh_retry = p.snapshot(cls.base, cls.env, 'Commitfresh')
        cls.retry_logs = p.start_world_again(cls.base, cls.env, cls.evidence,
                                            cls.world('Commitorphan'), 'retry',
                                            'World server is up and running!')
        cls.retry = p.snapshot(cls.base, cls.env, 'Commitorphan')

    @staticmethod
    def world(name):
        env = p.world_env_for(name)
        env.update(PLAYERBOT_ENABLE='0', PLAYERBOT_MIN_BOTS='0', PLAYERBOT_MAX_BOTS='0')
        return env

    @classmethod
    def cleanup_lab(cls):
        if cls.base is None:
            return
        # Verification errors must prevent deletion, not degrade to a warning.
        ids = p.command(['docker', 'compose'] + cls.base + ['ps', '-a', '-q'], env=cls.env).splitlines()
        for obj in json.loads(p.command(['docker', 'inspect', *ids])) if ids else []:
            assert obj['Config']['Labels']['com.docker.compose.project'] == cls.project
            assert not obj['HostConfig'].get('PortBindings')
            assert all(not m.get('Name', '').startswith('tortoise-local_') for m in obj['Mounts'])
        p.teardown_lab(cls.base, cls.env, cls.project, cls.evidence)

    def test_fresh_sql_failure_leaves_only_resumable_orphan_and_reports_failure(self):
        # The shipped characters table is MyISAM. C6 permits an unbound orphan;
        # transaction syntax cannot promise rollback for a nontransactional table.
        self.assertIsNotNone(self.fresh)
        self.assertEqual(self.fresh['bindings'], [])
        self.assertEqual(self.fresh['roster'], 0)
        self.assertIn("fresh creation transaction failed for 'Commitfresh'", self.fresh_logs)
        self.assertNotIn("created character 'Commitfresh'", self.fresh_logs)

    def test_fresh_retry_completes_orphan_without_replacing_identity(self):
        self.assertEqual(self.fresh_retry['guid'], self.fresh['guid'])
        self.assertEqual(self.fresh_retry['account'], self.fresh['account'])
        self.assertEqual(self.fresh_retry['bindings'], [(self.fresh['account'], 2)])
        self.assertEqual(self.fresh_retry['roster'], 1)
        self.assertIn('completed (existing character)', self.fresh_retry_logs)

    def test_orphan_sql_failure_preserves_state_and_reports_failure(self):
        self.assertEqual(self.orphan, self.before)
        self.assertIn("orphan completion transaction failed for 'Commitorphan'", self.orphan_logs)
        self.assertNotIn('completed (existing character)', self.orphan_logs)

    def test_retry_after_sql_failure_completes_original_identity(self):
        self.assertEqual(self.retry['guid'], self.before['guid'])
        self.assertEqual(self.retry['account'], self.before['account'])
        self.assertEqual(self.retry['bindings'], [(self.before['account'], 2)])
        self.assertEqual(self.retry['roster'], 1)
        self.assertIn('completed (existing character)', self.retry_logs)


if __name__ == '__main__':
    unittest.main()
