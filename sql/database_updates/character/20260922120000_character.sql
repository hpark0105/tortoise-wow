-- BL-003 (KAP-558): ordered character-database learning schema.
-- Idempotent: CREATE TABLE IF NOT EXISTS. Rerunning adds no tables and
-- changes nothing. No foreign keys to the character schema (indexed
-- char_guid columns + explicit cleanup only). No migration mutates
-- gameplay state.
--
-- Mode vocabulary (bot_learning_profile.mode):
--   0 = disabled (default; no rows in encounter until enrolled)
--   1 = observe
--   2 = shadow
--   3 = trial
--   4 = paused
--
-- end_reason vocabulary (bot_learning_encounter.end_reason):
--   0 = none
--   1 = target-death
--   2 = companion-death
--   3 = target-changed
--   4 = target-invalid
--   5 = leash-expired
--   6 = owner-override
--   7 = interrupted: written by the startup stale repair (see
--        Companion/LearningStore.h, kStaleInterruptedSql) for a row that
--        a previous process captured but never completed.
--
-- Retention contract (executed by Companion/LearningStore.h, see
-- kEncounterRetentionSql / kPlaybookRetentionSql):
--   * encounter rows: at most 500 per character and at most 30 days;
--     characters with a pending candidate (state = 0) are not pruned at
--     all, so their cited evidence is never deleted (pruning is paused
--     for that character);
--   * playbook versions: at most the 20 newest historical (state = 1)
--     versions per character; active versions and versions cited as
--     parent_version by another version of the same character survive.
CREATE TABLE IF NOT EXISTS `bot_learning_profile` (
  `char_guid` BIGINT(20) UNSIGNED NOT NULL,
  `mode` TINYINT(3) UNSIGNED NOT NULL DEFAULT 0
    COMMENT '0=disabled 1=observe 2=shadow 3=trial 4=paused',
  `schema_version` INT UNSIGNED NOT NULL DEFAULT 1,
  `active_playbook_version` INT UNSIGNED NOT NULL DEFAULT 0
    COMMENT '0 = baseline',
  `expected_version` INT UNSIGNED NOT NULL DEFAULT 0
    COMMENT 'CAS target version for compare-and-swap activation',
  `created_at` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'UNIX seconds',
  `updated_at` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'UNIX seconds',
  PRIMARY KEY (`char_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb3 COLLATE=utf8mb3_general_ci ROW_FORMAT=DYNAMIC;

CREATE TABLE IF NOT EXISTS `bot_learning_playbook` (
  `id` BIGINT(20) UNSIGNED NOT NULL AUTO_INCREMENT,
  `char_guid` BIGINT(20) UNSIGNED NOT NULL,
  `version` INT UNSIGNED NOT NULL,
  `parent_version` INT UNSIGNED NOT NULL DEFAULT 0
    COMMENT '0 = baseline (no parent)',
  `capability_fingerprint` BLOB NOT NULL
    COMMENT 'bounded capability/context fingerprint (serialized)',
  `settings_payload` BLOB NOT NULL
    COMMENT 'bounded settings (priority plan, serialized)',
  `state` TINYINT(3) UNSIGNED NOT NULL DEFAULT 0
    COMMENT '0=active 1=historical 2=rolled_back',
  `provenance` VARCHAR(256) NOT NULL DEFAULT ''
    COMMENT 'origin: enrollment/evaluator/rollback',
  `created_at` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'UNIX seconds',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_playbook_char_version` (`char_guid`, `version`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb3 COLLATE=utf8mb3_general_ci ROW_FORMAT=DYNAMIC;

CREATE TABLE IF NOT EXISTS `bot_learning_encounter` (
  `id` BIGINT(20) UNSIGNED NOT NULL AUTO_INCREMENT,
  `delivery_nonce` BIGINT(20) UNSIGNED NOT NULL
    COMMENT 'process/session nonce (unique per server process, never zero)',
  `delivery_seq` INT UNSIGNED NOT NULL
    COMMENT 'per-store monotonic sequence (1-based)',
  `char_guid` BIGINT(20) UNSIGNED NOT NULL,
  `target_guid` BIGINT(20) UNSIGNED NOT NULL DEFAULT 0
    COMMENT 'full 64-bit creature GUID of the active target (never truncated)',
  `session_id` BIGINT(20) UNSIGNED NOT NULL DEFAULT 0
    COMMENT 'world session identifier (opaque; 0 in BL-003, no account data)',
  `sequence` INT UNSIGNED NOT NULL DEFAULT 0
    COMMENT 'per-session encounter sequence (1-based)',
  `policy_version` INT UNSIGNED NOT NULL DEFAULT 0,
  `playbook_version` INT UNSIGNED NOT NULL DEFAULT 0,
  `source` TINYINT(3) UNSIGNED NOT NULL DEFAULT 0,
  `route` TINYINT(3) UNSIGNED NOT NULL DEFAULT 0,
  `duration_ms` INT UNSIGNED NOT NULL DEFAULT 0,
  `effective_damage` INT UNSIGNED NOT NULL DEFAULT 0,
  `periodic_damage` INT UNSIGNED NOT NULL DEFAULT 0,
  `damage_taken` INT UNSIGNED NOT NULL DEFAULT 0,
  `deaths` TINYINT(3) UNSIGNED NOT NULL DEFAULT 0,
  `owner_overrides` TINYINT(3) UNSIGNED NOT NULL DEFAULT 0,
  `decisions` INT UNSIGNED NOT NULL DEFAULT 0,
  `casts_accepted` INT UNSIGNED NOT NULL DEFAULT 0,
  `casts_rejected` INT UNSIGNED NOT NULL DEFAULT 0,
  `casts_no_eligible` INT UNSIGNED NOT NULL DEFAULT 0,
  `event_count` INT UNSIGNED NOT NULL DEFAULT 0,
  `end_reason` TINYINT(3) UNSIGNED NOT NULL DEFAULT 0
    COMMENT '0..6 = BL-002 end reason, 7 = interrupted (startup stale repair)',
  `is_complete` TINYINT(1) NOT NULL DEFAULT 0,
  `is_overflow` TINYINT(1) NOT NULL DEFAULT 0,
  `is_efficacy_eligible` TINYINT(1) NOT NULL DEFAULT 0,
  `captured_at` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'UNIX seconds at capture',
  `completed_at` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'UNIX seconds at DB insert; 0 = captured, never completed',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_encounter_delivery` (`delivery_nonce`, `delivery_seq`, `char_guid`),
  KEY `idx_encounter_char_captured` (`char_guid`, `captured_at`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb3 COLLATE=utf8mb3_general_ci ROW_FORMAT=DYNAMIC;

CREATE TABLE IF NOT EXISTS `bot_learning_candidate` (
  `id` BIGINT(20) UNSIGNED NOT NULL AUTO_INCREMENT,
  `char_guid` BIGINT(20) UNSIGNED NOT NULL,
  `proposal_payload` BLOB NOT NULL COMMENT 'bounded change proposal (serialized)',
  `evidence_payload` BLOB NOT NULL COMMENT 'cited encounter IDs (serialized)',
  `state` TINYINT(3) UNSIGNED NOT NULL DEFAULT 0
    COMMENT '0=pending 1=accepted 2=rejected 3=expired',
  `evaluator_version` INT UNSIGNED NOT NULL DEFAULT 0,
  `created_at` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'UNIX seconds',
  `updated_at` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'UNIX seconds',
  PRIMARY KEY (`id`),
  KEY `idx_candidate_char_state` (`char_guid`, `state`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb3 COLLATE=utf8mb3_general_ci ROW_FORMAT=DYNAMIC;

CREATE TABLE IF NOT EXISTS `bot_learning_audit` (
  `id` BIGINT(20) UNSIGNED NOT NULL AUTO_INCREMENT,
  `char_guid` BIGINT(20) UNSIGNED NOT NULL,
  `event` TINYINT(3) UNSIGNED NOT NULL
    COMMENT '0=enroll 1=pause 2=resume 3=override 4=promotion 5=rollback',
  `actor` VARCHAR(64) NOT NULL DEFAULT ''
    COMMENT 'who triggered: owner/system/evaluator',
  `reason` VARCHAR(256) NOT NULL DEFAULT '',
  `version_ref` INT UNSIGNED NOT NULL DEFAULT 0
    COMMENT 'playbook version involved',
  `created_at` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'UNIX seconds',
  PRIMARY KEY (`id`),
  KEY `idx_audit_char` (`char_guid`, `created_at`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb3 COLLATE=utf8mb3_general_ci ROW_FORMAT=DYNAMIC;
