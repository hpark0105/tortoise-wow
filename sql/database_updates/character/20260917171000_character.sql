-- 2026-09-17 personal runtime migration (KAP-558 / PORT-026 deployment).
-- ObjectMgr::BackupCharacterInventory (src/game/ObjectMgr.cpp) truncates and mirrors
-- character_inventory into character_inventory_copy on honor maintenance, and
-- mangosd.conf.dist.in ships BackupCharacterInventory = 1, but no migration ever
-- created the mirror table. With the fork's fail-closed DatabaseMysql handler the
-- resulting ER_NO_SUCH_TABLE aborts world startup. LIKE keeps the mirror exactly
-- in step with character_inventory so INSERT ... SELECT * keeps matching.
CREATE TABLE IF NOT EXISTS character_inventory_copy LIKE character_inventory;
