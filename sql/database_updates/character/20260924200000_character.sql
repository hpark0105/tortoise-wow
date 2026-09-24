-- SOCIAL-001: persistent bot-to-bot acquaintances are separate from the
-- player's client-visible friend list. Rows are directional so each bot can
-- later develop its own social memory and event count.
CREATE TABLE IF NOT EXISTS `bot_social_acquaintance` (
  `char_guid` INT UNSIGNED NOT NULL,
  `acquaintance_guid` INT UNSIGNED NOT NULL,
  `shared_events` INT UNSIGNED NOT NULL DEFAULT 0,
  `first_seen` INT UNSIGNED NOT NULL DEFAULT 0,
  `last_seen` INT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`char_guid`, `acquaintance_guid`),
  KEY `idx_bot_social_last_seen` (`last_seen`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;
