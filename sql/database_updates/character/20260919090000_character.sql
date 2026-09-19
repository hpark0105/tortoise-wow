-- 2026-09-19 personal runtime migration (KAP-558 / PORT-034 deployment).
-- PORT-034 provisions a companion cohort (Bram, Rowan, Elowen, Clem) from
-- one PLAYERBOT_PROVISION list, and companion commands (.botfollow /
-- .botrecruit / .botdismiss / .botassist) address bots by name. The
-- original warrior companion (guid 2, reserved bot account 1000000000,
-- provisioned bare-name as "Companion") is renamed to "Bram" so the
-- roster reads as a named party instead of a generic singleton.
-- Provisioning resolves its idempotency key by character name
-- (ProvisionPersistentBot: SELECT ... FROM characters WHERE name = ...
-- plus the bot_provision_state marker), so BOTH rows are renamed in the
-- same migration; afterwards provisioning "Bram,1,1,0" finds guid 2 and
-- the marker (race 1 / class 1 / gender 0) is an exact match, making the
-- provision a no-op instead of creating a second warrior.
-- Scope is strictly the reserved bot-account rows; no player data touched.
UPDATE characters
SET name = 'Bram'
WHERE guid = 2 AND name = 'Companion' AND account = 1000000000;

UPDATE bot_provision_state
SET character_name = 'Bram'
WHERE character_name = 'Companion' AND account_id = 1000000000;
