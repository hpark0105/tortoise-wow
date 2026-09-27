-- LIVING-WORLD-002: validated, successful same-zone route legs shared by
-- autonomous citizens. Coordinates are server-observed arrival points only.
CREATE TABLE IF NOT EXISTS `bot_citizen_route_edge` (
  `map_id` SMALLINT UNSIGNED NOT NULL,
  `zone_id` MEDIUMINT UNSIGNED NOT NULL,
  `from_x` INT NOT NULL,
  `from_y` INT NOT NULL,
  `to_x` INT NOT NULL,
  `to_y` INT NOT NULL,
  `successes` INT UNSIGNED NOT NULL DEFAULT 1,
  `last_seen` INT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`map_id`, `zone_id`, `from_x`, `from_y`, `to_x`, `to_y`),
  KEY `idx_citizen_route_recent` (`map_id`, `zone_id`, `last_seen`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;
