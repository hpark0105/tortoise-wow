-- TW-BOTS-002: bounded shared hints from verified citizen zone migrations.
-- A policy/content version mismatch starts a fresh count for that edge.
CREATE TABLE IF NOT EXISTS `bot_citizen_migration_hint` (
  `map_id` SMALLINT UNSIGNED NOT NULL,
  `source_zone` MEDIUMINT UNSIGNED NOT NULL,
  `destination_zone` MEDIUMINT UNSIGNED NOT NULL,
  `source_version` TINYINT UNSIGNED NOT NULL DEFAULT 1,
  `success_count` TINYINT UNSIGNED NOT NULL DEFAULT 1,
  `last_success` INT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`map_id`, `source_zone`, `destination_zone`),
  KEY `idx_citizen_migration_hint_recent` (`source_version`, `last_success`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

-- Preserve the trip origin across a restart, including restarts after zone entry.
ALTER TABLE `bot_citizen_journal`
  ADD COLUMN `progression_source_zone` MEDIUMINT UNSIGNED NOT NULL DEFAULT 0
    AFTER `target_zone`;
