-- BL-009 (KAP-558): bounded evidence-backed combat lessons.
-- Lessons are observations only; they never directly authorize gameplay.
CREATE TABLE IF NOT EXISTS `bot_learning_lesson` (
  `id` BIGINT(20) UNSIGNED NOT NULL AUTO_INCREMENT,
  `char_guid` BIGINT(20) UNSIGNED NOT NULL,
  `lesson_type` TINYINT(3) UNSIGNED NOT NULL DEFAULT 0 COMMENT '1=caution 2=timing 3=steady',
  `confidence` TINYINT(3) UNSIGNED NOT NULL DEFAULT 0 COMMENT '0..3',
  `encounter_sequence` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'per-session evidence reference',
  `duration_ms` INT UNSIGNED NOT NULL DEFAULT 0,
  `effective_damage` INT UNSIGNED NOT NULL DEFAULT 0,
  `damage_taken` INT UNSIGNED NOT NULL DEFAULT 0,
  `decisions` INT UNSIGNED NOT NULL DEFAULT 0,
  `evidence_count` INT UNSIGNED NOT NULL DEFAULT 1,
  `applied` TINYINT(1) NOT NULL DEFAULT 0,
  `created_at` INT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`id`),
  KEY `idx_learning_lesson_char_created` (`char_guid`, `created_at`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb3 COLLATE=utf8mb3_general_ci ROW_FORMAT=DYNAMIC;
