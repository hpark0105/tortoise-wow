-- TW-014 (KAP-557): optional human-owner binding for a companion bot.
-- account_id remains the bot's own reserved session identity (contract
-- C2: one session per account, reserved range >= 1e9); owner_account_id
-- is the human account allowed to command this companion via
-- .botfollow/.botstop. NULL = unowned (legacy roster bot); commands are
-- rejected for unowned bots. A human may own several companions, so the
-- column is deliberately not unique.
-- Idempotent: MariaDB ADD COLUMN IF NOT EXISTS.
ALTER TABLE `bot_ownership`
  ADD COLUMN IF NOT EXISTS `owner_account_id` INT(10) UNSIGNED NULL DEFAULT NULL
  COMMENT 'Human account allowed to command this companion (NULL = unowned)';
