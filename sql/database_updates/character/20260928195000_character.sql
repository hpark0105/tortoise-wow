-- Track the last level at which an autonomous citizen rolled its initial or
-- level-up equipment set, so relogging cannot reroll the same level.
ALTER TABLE `bot_citizen_journal`
  ADD COLUMN `gear_roll_level` TINYINT UNSIGNED NOT NULL DEFAULT 0 AFTER `current_job`;
