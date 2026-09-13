# Bot persistence evidence

This file records accepted, reproducible evidence for the playable companion
MVP. Evidence comes from disposable, port-free Docker projects; the personal
server database is not used.

## MVP-004: normal combat XP

- Bot: `Combatxp`, character GUID `500101`
- Opponent: creature template `6` (`Kobold Vermin`, level 1-2)
- Path: default PlayerBotAI hostile target selection, melee combat, core kill
  reward, `Player::RewardSinglePlayerAtKill`, then `Player::GiveXP`
- Before: XP `0`
- After: XP `54`
- Validation: `python docker/test_bot_combat_xp.py -v`, 1/1 passed in 121.231s
- Safety: no XP SQL update, GM command, published port, or personal volume

