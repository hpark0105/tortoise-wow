-- Mark the citizen gear policy already applied so one policy revision can
-- refresh existing citizens once without rerolling their gear on every login.
ALTER TABLE `bot_citizen_journal`
  ADD COLUMN `gear_roll_policy_version` TINYINT UNSIGNED NOT NULL DEFAULT 0 AFTER `gear_roll_level`;
