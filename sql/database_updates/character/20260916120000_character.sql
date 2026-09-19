-- PORT-020 (KAP-558): persist the minimal versioned personality state
-- for owned companions: schema version, declared profile id, assignment
-- timestamp. No prompts, no raw chat text, no learning history (those
-- remain Phase 4). Profile ids: 0 = none (baseline), 1 = reckless,
-- 2 = cautious (see src/game/PlayerBots/Companion/Personality.h).
CREATE TABLE IF NOT EXISTS `bot_personality` (
  `char_guid` INT UNSIGNED NOT NULL,
  `schema_version` TINYINT UNSIGNED NOT NULL DEFAULT 1,
  `profile_id` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `assigned_at` INT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`char_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;
