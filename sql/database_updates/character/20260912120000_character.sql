-- Native persistent-bot provisioning state.
-- This marker makes MyISAM partial writes distinguishable from unrelated or
-- legacy characters. Only a marker owned by the configured bot identity may
-- be resumed automatically.
CREATE TABLE IF NOT EXISTS `bot_provision_state` (
  `char_guid` bigint(20) unsigned NOT NULL,
  `account_id` int(10) unsigned NOT NULL,
  `character_name` varchar(12) NOT NULL,
  `phase` tinyint(3) unsigned NOT NULL DEFAULT 1 COMMENT '1 = native save started, 2 = native state ready',
  `updated_at` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (`char_guid`) USING BTREE,
  UNIQUE KEY `uq_bot_provision_account` (`account_id`),
  UNIQUE KEY `uq_bot_provision_name` (`character_name`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb3 COLLATE=utf8mb3_general_ci ROW_FORMAT=DYNAMIC;
