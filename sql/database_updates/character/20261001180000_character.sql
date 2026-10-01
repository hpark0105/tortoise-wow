-- LIVING-ROUTE-003: bounded, per-citizen cooldown for a progression anchor
-- that failed navmesh validation or stalled during movement. A single row in
-- the existing journal keeps the penalty bounded and restart-safe.
ALTER TABLE `bot_citizen_journal`
  ADD COLUMN `route_failure_map` SMALLINT UNSIGNED NOT NULL DEFAULT 0 AFTER `updated_at`,
  ADD COLUMN `route_failure_zone` MEDIUMINT UNSIGNED NOT NULL DEFAULT 0 AFTER `route_failure_map`,
  ADD COLUMN `route_failure_x` FLOAT NOT NULL DEFAULT 0 AFTER `route_failure_zone`,
  ADD COLUMN `route_failure_y` FLOAT NOT NULL DEFAULT 0 AFTER `route_failure_x`,
  ADD COLUMN `route_failure_z` FLOAT NOT NULL DEFAULT 0 AFTER `route_failure_y`,
  ADD COLUMN `route_failure_until` INT UNSIGNED NOT NULL DEFAULT 0 AFTER `route_failure_z`,
  ADD COLUMN `route_failure_reason` TINYINT UNSIGNED NOT NULL DEFAULT 0 AFTER `route_failure_until`,
  ADD COLUMN `route_search_cursor` INT UNSIGNED NOT NULL DEFAULT 0 AFTER `route_failure_reason`;
