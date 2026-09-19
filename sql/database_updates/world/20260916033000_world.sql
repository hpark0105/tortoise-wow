-- ==============================================
-- FILE: spell_mod_taunt_stances_normalize.sql
-- GENERATED: 20260916033000
-- ==============================================
-- PORT-014 (KAP-558) data fix: Taunt (355) retains the un-normalized
-- classic warrior stance mask (Stances = 131072). This build enforces
-- Stances against shapeshift form only (SpellEntry::
-- GetErrorAtShapeshiftedCast), so a neutral-stance warrior cannot cast
-- it (cast result 86, SPELL_FAILED_ONLY_SHAPESHIFT). The rest of the
-- pinned spell set is normalized to 0 (e.g., Whirlwind 974, Taunt
-- 29060, Curse of Agony 980), so normalize Taunt 355 through the
-- standard spell_mod override (field Stances = 0 is explicit). This
-- also unblocks the declared companion tank matrix
-- (src/game/PlayerBots/Companion/Tank.h), which requires a castable
-- Taunt for the level-10 warrior build.
INSERT INTO `spell_mod` (`Id`, `Stances`, `Comment`)
VALUES (355, 0, 'Normalize Taunt stance mask so neutral-stance warriors can cast (PORT-014 KAP-558)');
