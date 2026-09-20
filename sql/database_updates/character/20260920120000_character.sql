-- KAP-558 review (PORT-034): persisted mirror provenance for companion
-- quest turn-in. The mirror turn-in gate no longer trusts "the owner also
-- holds this quest" alone: the companion's log can carry rows the mirror
-- path never accepted (fixture/DB surgery, the declared single-quest path),
-- and such rows must never be rewarded as mirrors.
--
-- A row here means the companion accepted the quest through the mirror
-- path (PlayerBotAI::MirrorOwnerQuestStep) - or, for legacy in-flight
-- mirrors, the one-shot login backfill (PlayerBot.MirrorMarkerBackfill)
-- recorded it. Set on mirror accept; cleared on reward. The turn-in gate
-- requires BOTH this marker and a live owner-log entry.
-- Idempotent: CREATE TABLE IF NOT EXISTS.
CREATE TABLE IF NOT EXISTS `bot_mirror_quest` (
  `char_guid` bigint(20) unsigned NOT NULL COMMENT 'Character GUID of the companion bot',
  `quest_id` int(10) unsigned NOT NULL COMMENT 'Quest accepted through the mirror path',
  `mirrored_at` int(10) unsigned NOT NULL DEFAULT 0 COMMENT 'UNIX seconds of the (re)accept/backfill',
  PRIMARY KEY (`char_guid`,`quest_id`) USING BTREE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb3 COLLATE=utf8mb3_general_ci ROW_FORMAT=DYNAMIC;
