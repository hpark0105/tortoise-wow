-- LIVING-WORLD-003: the current deterministic citizen activity survives a
-- relog as an observable intent. It never authorizes movement by itself.
ALTER TABLE `bot_citizen_journal`
  ADD COLUMN `current_job` TINYINT UNSIGNED NOT NULL DEFAULT 0 AFTER `intent`;
