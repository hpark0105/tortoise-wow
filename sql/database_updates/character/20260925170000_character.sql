-- LIVING-ROUTE-001: a compact, durable journal for autonomous citizens.
-- Values are server-selected map/zone/anchor state; no model output or
-- client command is ever persisted as a movement instruction.
CREATE TABLE IF NOT EXISTS `bot_citizen_journal` (
  `char_guid` INT UNSIGNED NOT NULL,
  `intent` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `progression_band` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `last_level` TINYINT UNSIGNED NOT NULL DEFAULT 1,
  `current_map` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `current_zone` MEDIUMINT UNSIGNED NOT NULL DEFAULT 0,
  `target_map` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `target_zone` MEDIUMINT UNSIGNED NOT NULL DEFAULT 0,
  `target_x` FLOAT NOT NULL DEFAULT 0,
  `target_y` FLOAT NOT NULL DEFAULT 0,
  `target_z` FLOAT NOT NULL DEFAULT 0,
  `visited_zones` INT UNSIGNED NOT NULL DEFAULT 0,
  `updated_at` INT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`char_guid`),
  KEY `idx_citizen_journal_target` (`target_map`, `target_zone`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

CREATE TABLE IF NOT EXISTS `bot_citizen_zone_visit` (
  `char_guid` INT UNSIGNED NOT NULL,
  `map_id` SMALLINT UNSIGNED NOT NULL,
  `zone_id` MEDIUMINT UNSIGNED NOT NULL,
  `first_seen` INT UNSIGNED NOT NULL DEFAULT 0,
  `last_seen` INT UNSIGNED NOT NULL DEFAULT 0,
  `visits` INT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`char_guid`, `map_id`, `zone_id`),
  KEY `idx_citizen_visit_recent` (`char_guid`, `last_seen`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;
