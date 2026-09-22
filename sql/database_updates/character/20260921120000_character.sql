-- 20260921120000_character.sql
-- KAP-558 / PORT-033 cohort data repair (park-head local-Qwen, 2026-09-21).
--
-- Symptom: the provisioned companions Rowan (guid 5), Elowen (guid 6) and
-- Clem (guid 7) loaded with "partially filled" gear. Their upgrade items
-- existed in item_instance but had no matching character_inventory row, so
-- the instances were invisible (neither equipped nor stored in a bag).
-- Bram (guid 2) was unaffected.
--
-- Root cause: a non-atomic (mixed MyISAM/InnoDB) inventory save dropped the
-- character_inventory placement rows while the item_instance rows survived,
-- most plausibly interrupted by the world crash on bot login (Item::LoadFromDB
-- null charges; since fixed in code and data).
--
-- Repair: best-of-slot re-placement. AutoEquipForLevel() early-returns for
-- owned companions (PlayerBotAI.cpp:1980), so a backpack-only re-placement
-- would NOT fill the paper doll. For each bot we place the best (highest
-- item_level) item of each equipment slot into the equipment slot and the
-- remaining spares into the backpack. Verified against the authoritative
-- InventoryType->slot map (Player::FindEquipSlot, Player.cpp:10211) and the
-- EquipmentSlots enum (Player.h:585). For all three of these bots each
-- currently-equipped item is already the best of its slot, so this repair is
-- pure guarded INSERTs: it adds the missing placement rows and never modifies
-- or deletes an existing row.
--
-- Idempotent: every insert is guarded so it applies only while the target item
-- is still unplaced AND the target slot is free. A re-run, or a fresh database
-- with no such bots, is a no-op.
--
-- Applied live on the personal server 2026-09-21 against tortoise-local_database
-- after ./docker/backup.ps1; SHA-1 recorded in tw_char.migrations.

-- =====================================================================
-- Rowan (guid 5, hunter)
--   paper doll (new):  5 waist 502, 6 legs 503, 7 feet 504, 8 wrists 505,
--                      9 hands 506, 15 mainhand 507
--   backpack (new):    28 442, 29 441, 30 445, 31 447
--   kept (already best): 3 body 443, 4 chest 501, 17 ranged 508, 19 quiver 446
-- =====================================================================
INSERT INTO character_inventory (guid, bag, slot, item, item_template)
SELECT 5, 0, 5, 502, 4690 FROM DUAL
WHERE NOT EXISTS (SELECT 1 FROM character_inventory ci WHERE ci.item = 502)
  AND NOT EXISTS (SELECT 1 FROM character_inventory ci WHERE ci.guid = 5 AND ci.bag = 0 AND ci.slot = 5);
INSERT INTO character_inventory (guid, bag, slot, item, item_template)
SELECT 5, 0, 6, 503, 5617 FROM DUAL
WHERE NOT EXISTS (SELECT 1 FROM character_inventory ci WHERE ci.item = 503)
  AND NOT EXISTS (SELECT 1 FROM character_inventory ci WHERE ci.guid = 5 AND ci.bag = 0 AND ci.slot = 6);
INSERT INTO character_inventory (guid, bag, slot, item, item_template)
SELECT 5, 0, 7, 504, 4942 FROM DUAL
WHERE NOT EXISTS (SELECT 1 FROM character_inventory ci WHERE ci.item = 504)
  AND NOT EXISTS (SELECT 1 FROM character_inventory ci WHERE ci.guid = 5 AND ci.bag = 0 AND ci.slot = 7);
INSERT INTO character_inventory (guid, bag, slot, item, item_template)
SELECT 5, 0, 8, 505, 4973 FROM DUAL
WHERE NOT EXISTS (SELECT 1 FROM character_inventory ci WHERE ci.item = 505)
  AND NOT EXISTS (SELECT 1 FROM character_inventory ci WHERE ci.guid = 5 AND ci.bag = 0 AND ci.slot = 8);
INSERT INTO character_inventory (guid, bag, slot, item, item_template)
SELECT 5, 0, 9, 506, 4239 FROM DUAL
WHERE NOT EXISTS (SELECT 1 FROM character_inventory ci WHERE ci.item = 506)
  AND NOT EXISTS (SELECT 1 FROM character_inventory ci WHERE ci.guid = 5 AND ci.bag = 0 AND ci.slot = 9);
INSERT INTO character_inventory (guid, bag, slot, item, item_template)
SELECT 5, 0, 15, 507, 5459 FROM DUAL
WHERE NOT EXISTS (SELECT 1 FROM character_inventory ci WHERE ci.item = 507)
  AND NOT EXISTS (SELECT 1 FROM character_inventory ci WHERE ci.guid = 5 AND ci.bag = 0 AND ci.slot = 15);
INSERT INTO character_inventory (guid, bag, slot, item, item_template)
SELECT 5, 0, 28, 442, 147 FROM DUAL
WHERE NOT EXISTS (SELECT 1 FROM character_inventory ci WHERE ci.item = 442)
  AND NOT EXISTS (SELECT 1 FROM character_inventory ci WHERE ci.guid = 5 AND ci.bag = 0 AND ci.slot = 28);
INSERT INTO character_inventory (guid, bag, slot, item, item_template)
SELECT 5, 0, 29, 441, 129 FROM DUAL
WHERE NOT EXISTS (SELECT 1 FROM character_inventory ci WHERE ci.item = 441)
  AND NOT EXISTS (SELECT 1 FROM character_inventory ci WHERE ci.guid = 5 AND ci.bag = 0 AND ci.slot = 29);
INSERT INTO character_inventory (guid, bag, slot, item, item_template)
SELECT 5, 0, 30, 445, 2092 FROM DUAL
WHERE NOT EXISTS (SELECT 1 FROM character_inventory ci WHERE ci.item = 445)
  AND NOT EXISTS (SELECT 1 FROM character_inventory ci WHERE ci.guid = 5 AND ci.bag = 0 AND ci.slot = 30);
INSERT INTO character_inventory (guid, bag, slot, item, item_template)
SELECT 5, 0, 31, 447, 2504 FROM DUAL
WHERE NOT EXISTS (SELECT 1 FROM character_inventory ci WHERE ci.item = 447)
  AND NOT EXISTS (SELECT 1 FROM character_inventory ci WHERE ci.guid = 5 AND ci.bag = 0 AND ci.slot = 31);

-- =====================================================================
-- Elowen (guid 6, high elf mage)
--   paper doll (new):  5 waist 510, 6 legs 511, 7 feet 512, 8 wrists 513,
--                      9 hands 514
--   backpack (new):    28 456, 29 454, 30 457, 31 450
--   kept (already best): 3 body 452, 4 chest 509, 14 back 515, 15 mainhand 516
-- =====================================================================
INSERT INTO character_inventory (guid, bag, slot, item, item_template)
SELECT 6, 0, 5, 510, 3606 FROM DUAL
WHERE NOT EXISTS (SELECT 1 FROM character_inventory ci WHERE ci.item = 510)
  AND NOT EXISTS (SELECT 1 FROM character_inventory ci WHERE ci.guid = 6 AND ci.bag = 0 AND ci.slot = 5);
INSERT INTO character_inventory (guid, bag, slot, item, item_template)
SELECT 6, 0, 6, 511, 1499 FROM DUAL
WHERE NOT EXISTS (SELECT 1 FROM character_inventory ci WHERE ci.item = 511)
  AND NOT EXISTS (SELECT 1 FROM character_inventory ci WHERE ci.guid = 6 AND ci.bag = 0 AND ci.slot = 6);
INSERT INTO character_inventory (guid, bag, slot, item, item_template)
SELECT 6, 0, 7, 512, 2569 FROM DUAL
WHERE NOT EXISTS (SELECT 1 FROM character_inventory ci WHERE ci.item = 512)
  AND NOT EXISTS (SELECT 1 FROM character_inventory ci WHERE ci.guid = 6 AND ci.bag = 0 AND ci.slot = 7);
INSERT INTO character_inventory (guid, bag, slot, item, item_template)
SELECT 6, 0, 8, 513, 3375 FROM DUAL
WHERE NOT EXISTS (SELECT 1 FROM character_inventory ci WHERE ci.item = 513)
  AND NOT EXISTS (SELECT 1 FROM character_inventory ci WHERE ci.guid = 6 AND ci.bag = 0 AND ci.slot = 8);
INSERT INTO character_inventory (guid, bag, slot, item, item_template)
SELECT 6, 0, 9, 514, 4307 FROM DUAL
WHERE NOT EXISTS (SELECT 1 FROM character_inventory ci WHERE ci.item = 514)
  AND NOT EXISTS (SELECT 1 FROM character_inventory ci WHERE ci.guid = 6 AND ci.bag = 0 AND ci.slot = 9);
INSERT INTO character_inventory (guid, bag, slot, item, item_template)
SELECT 6, 0, 28, 456, 20893 FROM DUAL
WHERE NOT EXISTS (SELECT 1 FROM character_inventory ci WHERE ci.item = 456)
  AND NOT EXISTS (SELECT 1 FROM character_inventory ci WHERE ci.guid = 6 AND ci.bag = 0 AND ci.slot = 28);
INSERT INTO character_inventory (guid, bag, slot, item, item_template)
SELECT 6, 0, 29, 454, 1395 FROM DUAL
WHERE NOT EXISTS (SELECT 1 FROM character_inventory ci WHERE ci.item = 454)
  AND NOT EXISTS (SELECT 1 FROM character_inventory ci WHERE ci.guid = 6 AND ci.bag = 0 AND ci.slot = 29);
INSERT INTO character_inventory (guid, bag, slot, item, item_template)
SELECT 6, 0, 30, 457, 55 FROM DUAL
WHERE NOT EXISTS (SELECT 1 FROM character_inventory ci WHERE ci.item = 457)
  AND NOT EXISTS (SELECT 1 FROM character_inventory ci WHERE ci.guid = 6 AND ci.bag = 0 AND ci.slot = 30);
INSERT INTO character_inventory (guid, bag, slot, item, item_template)
SELECT 6, 0, 31, 450, 26 FROM DUAL
WHERE NOT EXISTS (SELECT 1 FROM character_inventory ci WHERE ci.item = 450)
  AND NOT EXISTS (SELECT 1 FROM character_inventory ci WHERE ci.guid = 6 AND ci.bag = 0 AND ci.slot = 31);

-- =====================================================================
-- Clem (guid 7, dwarf priest)
--   paper doll (new):  5 waist 518, 6 legs 519, 7 feet 520, 8 wrists 521,
--                      9 hands 522
--   backpack (new):    28 464, 29 460, 30 459, 31 458
--   kept (already best): 3 body 461, 4 chest 517, 14 back 523, 15 mainhand 524
-- =====================================================================
INSERT INTO character_inventory (guid, bag, slot, item, item_template)
SELECT 7, 0, 5, 518, 4663 FROM DUAL
WHERE NOT EXISTS (SELECT 1 FROM character_inventory ci WHERE ci.item = 518)
  AND NOT EXISTS (SELECT 1 FROM character_inventory ci WHERE ci.guid = 7 AND ci.bag = 0 AND ci.slot = 5);
INSERT INTO character_inventory (guid, bag, slot, item, item_template)
SELECT 7, 0, 6, 519, 794 FROM DUAL
WHERE NOT EXISTS (SELECT 1 FROM character_inventory ci WHERE ci.item = 519)
  AND NOT EXISTS (SELECT 1 FROM character_inventory ci WHERE ci.guid = 7 AND ci.bag = 0 AND ci.slot = 6);
INSERT INTO character_inventory (guid, bag, slot, item, item_template)
SELECT 7, 0, 7, 520, 7351 FROM DUAL
WHERE NOT EXISTS (SELECT 1 FROM character_inventory ci WHERE ci.item = 520)
  AND NOT EXISTS (SELECT 1 FROM character_inventory ci WHERE ci.guid = 7 AND ci.bag = 0 AND ci.slot = 7);
INSERT INTO character_inventory (guid, bag, slot, item, item_template)
SELECT 7, 0, 8, 521, 7350 FROM DUAL
WHERE NOT EXISTS (SELECT 1 FROM character_inventory ci WHERE ci.item = 521)
  AND NOT EXISTS (SELECT 1 FROM character_inventory ci WHERE ci.guid = 7 AND ci.bag = 0 AND ci.slot = 8);
INSERT INTO character_inventory (guid, bag, slot, item, item_template)
SELECT 7, 0, 9, 522, 2960 FROM DUAL
WHERE NOT EXISTS (SELECT 1 FROM character_inventory ci WHERE ci.item = 522)
  AND NOT EXISTS (SELECT 1 FROM character_inventory ci WHERE ci.guid = 7 AND ci.bag = 0 AND ci.slot = 9);
INSERT INTO character_inventory (guid, bag, slot, item, item_template)
SELECT 7, 0, 28, 464, 6098 FROM DUAL
WHERE NOT EXISTS (SELECT 1 FROM character_inventory ci WHERE ci.item = 464)
  AND NOT EXISTS (SELECT 1 FROM character_inventory ci WHERE ci.guid = 7 AND ci.bag = 0 AND ci.slot = 28);
INSERT INTO character_inventory (guid, bag, slot, item, item_template)
SELECT 7, 0, 29, 460, 52 FROM DUAL
WHERE NOT EXISTS (SELECT 1 FROM character_inventory ci WHERE ci.item = 460)
  AND NOT EXISTS (SELECT 1 FROM character_inventory ci WHERE ci.guid = 7 AND ci.bag = 0 AND ci.slot = 29);
INSERT INTO character_inventory (guid, bag, slot, item, item_template)
SELECT 7, 0, 30, 459, 51 FROM DUAL
WHERE NOT EXISTS (SELECT 1 FROM character_inventory ci WHERE ci.item = 459)
  AND NOT EXISTS (SELECT 1 FROM character_inventory ci WHERE ci.guid = 7 AND ci.bag = 0 AND ci.slot = 30);
INSERT INTO character_inventory (guid, bag, slot, item, item_template)
SELECT 7, 0, 31, 458, 36 FROM DUAL
WHERE NOT EXISTS (SELECT 1 FROM character_inventory ci WHERE ci.item = 458)
  AND NOT EXISTS (SELECT 1 FROM character_inventory ci WHERE ci.guid = 7 AND ci.bag = 0 AND ci.slot = 31);
