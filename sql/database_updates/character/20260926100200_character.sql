-- Durable, evidence-driven local danger memory for independent citizens.
-- Coordinates are coarse cells, never player-controlled locations.
CREATE TABLE IF NOT EXISTS `bot_citizen_danger_memory` (
  `char_guid` INT UNSIGNED NOT NULL,
  `map_id` SMALLINT UNSIGNED NOT NULL,
  `zone_id` INT UNSIGNED NOT NULL,
  `cell_x` INT NOT NULL,
  `cell_y` INT NOT NULL,
  `deaths` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `safe_until` INT UNSIGNED NOT NULL DEFAULT 0,
  `updated_at` INT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`char_guid`, `map_id`, `zone_id`, `cell_x`, `cell_y`),
  KEY `bot_citizen_danger_active` (`char_guid`, `safe_until`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
