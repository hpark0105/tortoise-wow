"""Actual SQL failures must never be reported as successful provisioning."""
import json
import unittest
import uuid
import test_bot_provision as p


class BotProvisionCommitFailureTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            p.command(['docker', 'image', 'inspect', p.IMAGE])
        except RuntimeError:
            raise unittest.SkipTest(f"{p.IMAGE} image not present; build it first")
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
INSERT INTO bot_provision_state (char_guid,account_id,character_name,phase)
VALUES (500090,1000500090,'Commitorphan',2);
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
        self.assertIn("native character 'Commitfresh' saved but roster publication failed", self.fresh_logs)
        self.assertNotIn("created native character 'Commitfresh'", self.fresh_logs)

    def test_fresh_retry_completes_orphan_without_replacing_identity(self):
        self.assertEqual(self.fresh_retry['guid'], self.fresh['guid'])
        self.assertEqual(self.fresh_retry['account'], self.fresh['account'])
        self.assertEqual(self.fresh_retry['bindings'], [(self.fresh['account'], 2)])
        self.assertEqual(self.fresh_retry['roster'], 1)
        self.assertIn('completed (native-ready character)', self.fresh_retry_logs)

    def test_orphan_sql_failure_preserves_state_and_reports_failure(self):
        self.assertEqual(self.orphan, self.before)
        self.assertIn("native-ready orphan completion failed for 'Commitorphan'", self.orphan_logs)
        self.assertNotIn('completed (native-ready character)', self.orphan_logs)

    def test_retry_after_sql_failure_completes_original_identity(self):
        self.assertEqual(self.retry['guid'], self.before['guid'])
        self.assertEqual(self.retry['account'], self.before['account'])
        self.assertEqual(self.retry['bindings'], [(self.before['account'], 2)])
        self.assertEqual(self.retry['roster'], 1)
        self.assertIn('completed (native-ready character)', self.retry_logs)


class BotNativeSaveFailureTests(unittest.TestCase):
    """A partial native save is cleaned and retried under one reserved identity."""

    @classmethod
    def setUpClass(cls):
        try:
            p.command(['docker', 'image', 'inspect', p.IMAGE])
        except RuntimeError:
            raise unittest.SkipTest(f"{p.IMAGE} image not present; build it first")
        cls.project = 'tortoise-bot-prov-' + uuid.uuid4().hex[:12]
        cls.evidence = p.ROOT / 'local' / cls.project
        cls.evidence.mkdir()
        cls.base = None
        cls.addClassCleanup(cls.cleanup_lab)
        cls.base, cls.env = p.boot_lab(
            cls.project, cls.evidence, cls.world('Nativefail'),
            "CREATE TRIGGER review_reject_native BEFORE INSERT ON character_action "
            "FOR EACH ROW SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='review injected native save failure';")
        p.command(['docker', 'compose'] + cls.base + ['up', '-d', '--no-deps', 'world'], env=cls.env)
        cls.failure_logs = p.wait_for(cls.base, cls.env,
                                      lambda logs: 'World server is up and running!' in logs)
        marker = p.db_exec(cls.base, cls.env,
                           "SELECT char_guid,account_id,phase FROM bot_provision_state "
                           "WHERE character_name='Nativefail'").strip()
        cls.marker_before = tuple(int(value) for value in marker.split('\t'))
        failed_guid = cls.marker_before[0]
        cls.failure_counts = (
            p.db_int(cls.base, cls.env, f"SELECT COUNT(*) FROM characters WHERE guid={failed_guid}"),
            p.db_int(cls.base, cls.env, f"SELECT COUNT(*) FROM bot_ownership WHERE char_guid={failed_guid}"),
            p.db_int(cls.base, cls.env, f"SELECT COUNT(*) FROM playerbot WHERE char_guid={failed_guid}"),
        )
        cls.partial_items = p.db_int(cls.base, cls.env,
                                     f"SELECT COUNT(*) FROM item_instance WHERE owner_guid={failed_guid}")
        p.db_exec(cls.base, cls.env, 'DROP TRIGGER review_reject_native;')
        cls.retry_logs = p.start_world_again(cls.base, cls.env, cls.evidence,
                                             cls.world('Nativefail'), 'native-save-retry',
                                             'World server is up and running!')
        cls.after = p.snapshot(cls.base, cls.env, 'Nativefail')

    @staticmethod
    def world(name):
        env = p.world_env_for(name)
        env.update(PLAYERBOT_ENABLE='0', PLAYERBOT_MIN_BOTS='0', PLAYERBOT_MAX_BOTS='0')
        return env

    @classmethod
    def cleanup_lab(cls):
        if cls.base is None:
            return
        ids = p.command(['docker', 'compose'] + cls.base + ['ps', '-a', '-q'], env=cls.env).splitlines()
        for obj in json.loads(p.command(['docker', 'inspect', *ids])) if ids else []:
            assert obj['Config']['Labels']['com.docker.compose.project'] == cls.project
            assert not obj['HostConfig'].get('PortBindings')
            assert all(not mount.get('Name', '').startswith('tortoise-local_') for mount in obj['Mounts'])
        p.teardown_lab(cls.base, cls.env, cls.project, cls.evidence)

    def test_native_save_failure_retains_partial_character_without_publication(self):
        guid, account, phase = self.marker_before
        self.assertEqual(phase, 1)
        self.assertEqual(self.failure_counts, (1, 0, 0))
        self.assertGreater(self.partial_items, 0, "failure should exercise cleanup after partial item persistence")
        self.assertIn("native action save failed for 'Nativefail'", self.failure_logs)
        self.assertNotIn("created native character 'Nativefail'", self.failure_logs)

    def test_retry_reuses_reserved_identity_and_publishes_once(self):
        guid, account, _ = self.marker_before
        self.assertEqual(self.after['guid'], guid)
        self.assertEqual(self.after['account'], account)
        self.assertEqual(self.after['bindings'], [(account, 2)])
        self.assertEqual(self.after['roster'], 1)
        self.assertEqual(p.db_int(self.base, self.env,
                                  f"SELECT COUNT(*) FROM character_inventory WHERE guid={guid}"),
                         int(p.db_exec(self.base, self.env,
                                       "SELECT COUNT(*) FROM playercreateinfo_item WHERE race=1 AND class=1",
                                       database="tw_world")))
        self.assertEqual(p.db_int(self.base, self.env,
                                  f"SELECT phase FROM bot_provision_state WHERE char_guid={guid}"), 2)
        self.assertIn("created native character 'Nativefail'", self.retry_logs)


if __name__ == '__main__':
    unittest.main()
