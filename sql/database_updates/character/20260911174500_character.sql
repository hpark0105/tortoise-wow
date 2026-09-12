-- TW-005 (KAP-548): persistent bot ownership metadata.
-- Contract: docs/bots/persistence-contract.md (C1, C6).
-- Reserved synthetic bot account range: 1000000000..1000009999999 (far above any
-- personal account id). A roster character is only bound here when its current
-- characters.account already sits in that reserved range; characters owned by a
-- real (below-range) account are never touched and are reported by the
-- validation query in docker/test_bot_ownership_migration.py (AC2).
-- This file is idempotent: rerunning it adds no rows and changes nothing.
CREATE TABLE IF NOT EXISTS `bot_ownership` (
  `char_guid` bigint(20) unsigned NOT NULL COMMENT 'Character GUID of the persistent bot',
  `account_id` int(10) unsigned NOT NULL COMMENT 'Reserved synthetic account bound to this character',
  `bot_type` tinyint(3) unsigned NOT NULL DEFAULT 1 COMMENT '1 = persistent roster bot',
  `provision_version` int(10) unsigned NOT NULL DEFAULT 1 COMMENT '1 = migration-seeded, 2 = runtime provisioned',
  `provisioned_at` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`char_guid`) USING BTREE,
  UNIQUE KEY `uq_bot_ownership_account` (`account_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb3 COLLATE=utf8mb3_general_ci ROW_FORMAT=DYNAMIC;

INSERT INTO `bot_ownership` (`char_guid`, `account_id`, `bot_type`, `provision_version`)
SELECT c.`guid`, c.`account`, 1, 1
FROM `playerbot` p
JOIN `characters` c ON c.`guid` = p.`char_guid`
WHERE c.`account` >= 1000000000
  AND NOT EXISTS (SELECT 1 FROM `bot_ownership` b WHERE b.`char_guid` = c.`guid`);