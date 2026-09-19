# PORT-034: companion cohort provisioning and level-appropriate skills

- Status: validated: Lab D 11/11 (port035d-lab6), cohort 17/17 (port034-cohort11); awaiting operator review
- Phase: 3, implementation follow-up to the PORT-033 draft
- Depends on: PORT-033 design, Phase 2 mirror/turn-in repairs (commit b7221dc)
- Retrieval: sync not performed (embedding paused per operator instruction); direct-read fallback only

## Scope

Provision a bounded companion cohort from one startup declaration:

```dotenv
PLAYERBOT_PROVISION=Bram,1,1,0;Rowan,1,3,0;Elowen,10,8,1;Clem,3,5,0
```

Each spec is `Name,race,class,gender` (four fields, `;`-separated). Roster:

| Name   | Race      | Class   | Gender |
|--------|-----------|---------|--------|
| Bram   | Human     | Warrior | Male   |
| Rowan  | Human     | Hunter  | Male   |
| Elowen | High Elf  | Mage    | Female |
| Clem   | Dwarf     | Priest  | Male   |

Bram is the existing warrior companion (guid 2, reserved bot account
1000000000) renamed from "Companion" by
`sql/database_updates/character/20260919090000_character.sql`. Both the
`characters` row and the `bot_provision_state` marker are renamed because
provisioning resolves its idempotency key by character name
(`ProvisionPersistentBot` selects by name plus marker); afterwards
provisioning `Bram,1,1,0` finds guid 2 and an exact race/class/gender
match, making it a no-op instead of creating a second warrior. The
remaining three are created natively at level 1 on reserved bot accounts.
Scope is strictly the reserved bot-account rows; no player data touched.

## Provisioning behavior

- The world parses the list at startup; missing names are created as
  native characters, roster rows published, and a `bot_provision_state`
  marker written (provision version 2, `character_name` column).
- Idempotent: restarting with the same list finds the existing character
  and marker and logs "already provisioned; idempotent no-op". An
  identity mismatch fails closed (no second character).
- Provisioning leaves `bot_ownership.owner_account_id` NULL. Provisioning
  declares identity; the operator binds ownership (below).

## Ownership binding rule

Verified in `PlayerBotMgr.cpp` (BotHold ~line 1693; quarantine ~line 426):

- Party commands (`.botrecruit` / `.botfollow` / `.bothold` /
  `.botdismiss` / `.botassist`) require
  `bot_ownership.owner_account_id` to equal the issuer session's account.
  A NULL owner is rejected ("hold rejected unowned bot").
- Roster rows with no `bot_ownership` row at all are quarantined: logged
  and skipped, the character's bot AI never runs.
- Labs bind to their lab account (1000 * owner character guid); the Lab D
  owner self-binds so its `bothold` succeeds and it stays pinned at spawn.
  The personal deployment binds the cohort to operator account 4.

## Level-appropriate skills (per class, dynamic)

- `PlayerBotAI::AutoLearnSpellsForLevel`
  (src/game/PlayerBots/PlayerBotAI.cpp:1209) runs at login and at
  level-up. It walks `tw_world.skill_line_ability` rows matching the
  character's class/race mask, filters by `spellLevel <= level` and
  `req_skill_value <=` current skill value
  (`GetSkillMaxForLevel() = level * 5`,
  src/game/Objects/Object.h:1124), and persists the learned set to
  `tw_char.character_spell (guid, spell, active, disabled)`.
- Nothing is hard-coded per bot: every provisioned companion, in any
  class or race, goes through the same engine path, so the cohort bots
  and any future bot get their class's legal ability set at their level.
  Personality profiles only affect planner suggestions and chase
  distance, never the learned set.
- Data finding (this fork's 1.12 data): the learned set grows with
  level. Class-masked `skill_line_ability` rows are gated by
  `spell_template.spellLevel`, observed spread 0-60 (e.g. Heroic Strike
  and Fireball 1, Charge and Frostbolt 4, Hamstring 32); the
  `req_skill_value` gates only require the skill line to exist (the
  learner raises it, PORT-028). A level-1 companion starts with the
  level-1 subset (Fireball, Smite, Auto Shot plus weapon/armor
  proficiencies) and gains higher spells at each level gate as it
  levels.
- Cohort lab skill assertions are empirical, with no hard-coded spell ids
  (fork spell numbering is data-specific): each companion persists at
  least 3 learned spells after login; the cohort spell sets are pairwise
  distinct (their class/race mask filters differ); the post-run set is a
  superset of the pre-run set per companion, except that a talent rank may
  be replaced by a rank of the same ability at a spellLevel >= (the engine
  unlearns the other ranks of a talent when a rank is learned; root cause 9).

## Root causes fixed during lab validation

1. Boar ranged drain (Lab D flakiness): the neutralization UPDATE covered
   only melee columns, but ranged damage lives in separate
   `ranged_dmg_min/max` columns; the boar kept shooting at 30 yd. Zeroed
   in the fixture, and the post-seed self-assert
   `_assert_fixture_templates` (docker/test_bot_quest_coop.py) queries the
   touched `creature_template` rows right after boot so a future drift
   fails in seconds instead of at the 15-minute mark.
2. Finisher hostility (run #2 denial): `FactionTemplateEntry::IsHostileTo`
   falls back to mask comparison (`hostile_mask & our_mask`) when the
   enemy/friend lists are empty; the faction-14 (Monster) template has
   hostile_mask 1 and the player template our_mask 3, so the finisher was
   hostile at 0 yd and `CanInteractWithNPC` rejected it. The fixture gives
   the finisher faction 1 (like the giver); a real quest finisher is
   friendly.
3. Owner wander drift (run #3 stall): the Lab D owner's `bot_ownership`
   row had `owner_account_id` NULL, so its `bothold` was rejected and the
   owner's own bot AI random-walked away from spawn (wander radius 1,
   unbounded). The companion followed the owner beyond
   `kOwnerFollowChaseDist` (25 yd, PlayerBotAI.h:193) and left the boar
   pack beyond `kBotAssistSearchRange` (30 yd, PlayerBotMgr.cpp:1738);
   quest credit stalled at 7 sabers / 1 boar. Fix: the Lab D owner
   self-binds (`owner_account_id` = its own lab account) so the hold
   succeeds and the designed geometry holds (all pack mobs within 26.5 yd
   of spawn).
4. Build fix: `GetDistanceTo(anchor)` -> `GetDistance(anchor)` in the
   mirror turn-in skip log (PlayerBotAI.cpp).
5. Idempotent quest-script turn-in: repeated turn-in attempts after
   reward no longer error (PlayerBotMgr.cpp).
6. Unbound cohort owner (cohort run #5, ERROR): the phase-2 owner had
   no `bot_ownership` row, so its 0 s `bothold` was rejected (party
   commands require the issuer account as owner) and the unowned
   character's legacy random walk drifted it off spawn before the
   10 s recruits; the mirror setup never started. Fix: the phase-2
   seed self-binds the owner to its lab account (same pattern as Lab
   D, root cause 3), and `test_phase1_ownership_and_roster` asserts
   the owner self-bind row (5 rows total).
7. Fixture format bug (cohort run #6, test side): the phase-2 seed
   SQL in `test_bot_companion_cohort.py` passed a value count that did
   not match its format specifiers, so the run died at 49 s during
   fixture preparation. Fixed to the 9-value tuple; test-only change,
   no rebuild.
8. Turn-in walk armed but never issued (cohort run #7, 13/17): every
   quest failure was Elowen's - quest row stuck COMPLETE+unrewarded
   (`1:0:7:4`), no mirror turn-in, level 1. Root cause:
   `MotionMaster::empty()` is effectively always false. `MotionMaster`
   is a `std::stack<MovementGenerator*>` (`using Impl::empty`,
   MotionMaster.h:83/100); `Initialize()` does `Clear(false, true)` and
   then `push(&si_idleMovement)`, so the static idle generator sits at
   the stack bottom forever (`DirectExpire`/`DirectClean` stop popping
   at size 1) and true stack-emptiness never means "no active motion".
   Both turn-in walk re-issue paths - the arm path (PlayerBotAI.cpp
   ~3309) and the follow walk block (~4515) - gated the `MovePoint` on
   `empty()` alone: the walk armed (5 s-throttled arm log repeated 9
   times) but was never issued, and Elowen stood idle at her follow
   side offset 36+ s, 23.4 yd from the finisher. Two apparent
   successes in the same run were coincidences: Clem's retry assist
   was already walking her to a boar 2.7 yd from the finisher and
   Rowan was already within 5 yd. The follow paths themselves re-issue
   on `moved`/`age` conditions (not `empty()` alone), which is why
   nothing else surfaced. The recovery corpse walk (~4058) carried the
   same latent bug and was fixed in the same change. Fix:
   `PlayerBotAI::MotionIdle()` =
   `GetMotionMaster()->empty() ||
   GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE` (the same
   idiom as PlayerAI.cpp:231), gating the arm path, the follow walk
   block, and the recovery walk. Run #8: all three companions
   mirror-accepted and mirror-turned in, 15/17.
9. Talent rank replacement (cohort runs #9-#10, test false positive):
   raw-set monotonicity failed for Elowen only, lost=[11115]
   gained=[11368] - both "Critical Mass" (High Elf Mage racial
   ability), spellLevel 1 and 2. Traced with a temporary [SpellTrace]
   instrumentation (added, then removed; evidence
   local/tortoise-bot-cohort-72c2ddfceeeb-*/phase2.log): in this
   fork's data (Talent.dbc, not the empty SQL talent table) Critical
   Mass is a talent family (ranks 11115/11367/11368). When the bot's
   AutoLearnSpellsForLevel learns a rank through AddSpell, the
   engine's talent block in AddSpell (src/game/Objects/Player.cpp
   4396-4412) unlearns the other ranks of the same talent. The level-2
   AutoLearn pass (OnLevelUp, from the mirror turn-in XP) learns rank
   2 (11368; rank 1 is already held and skipped by HasSpell) and the
   engine unlearns 11115 - the character keeps the ability at its
   highest learned rank, the engine's intended one-rank-per-talent
   semantics. Not a spell loss; the raw-set monotonicity assumption
   was wrong for talent ranks. Fix (test only): rank-aware
   monotonicity - a lost spell is acceptable only when a gained spell
   with the same name at spellLevel >= replaces it (name/spellLevel
   queried from the lab world DB; still no hard-coded spell ids); any
   other shrinkage still fails. Cohort run #11: 17/17.

## Debug instrumentation

`[CoopQuest] mirror turnin skip GUID:%u quest:%u anchor:%u interact:%u
dist:%.1f gates:%x` logs all 13 `CanInteractWithNPC` gates as a bitmask:
0x1 in-world/!taxi, 0x2 !CAN_NOT_REACT, 0x4 QUESTGIVER flag, 0x8 alive,
0x10 !invisible, 0x20 !charmer, 0x40 !IsHostileTo, 0x80 !IsInCombat,
0x100 !NOT_SELECTABLE, 0x200 within 5 yd. Run #2 logged gates 0x3bf
(only 0x40 failing), which isolated the hostility root cause in one run.

## Labs

- Lab D: `cd docker; python -m unittest -v
  test_bot_quest_coop.BotQuestCoopFinisherTests` - 11 tests; mirror mode
  with the giver/finisher split (questrelation vs involvedrelation),
  party loss and rejoin, kill credit through the pack, mirror turn-in at
  the finisher, restart persistence of quest rows.
- Cohort: `cd docker; python -m unittest -v test_bot_companion_cohort` -
  four companions provisioned from one list (Bram plus the quest cohort
  Rowan/Elowen/Clem); idempotent re-provision after a world restart;
  per-companion mirror accept (anchor 2079) and mirror turn-in (anchor
  6911); per-class skill learning (>= 3 spells each, pairwise-distinct
  sets, rank-aware monotone post-set: a talent rank may be replaced by
  a same-ability rank at spellLevel >= (root cause 9)); companions reach
  level 2; world healthy; personal state untouched.

Both labs run a disposable Compose project with synthetic characters; no
personal containers or personal volumes are touched.

## Personal deployment plan (one maintenance window)

1. `./docker/backup.ps1`.
2. Apply `sql/database_updates/character/20260919090000_character.sql` to
   live tw_char (verify guid 2 = Bram and the marker rename), then record
   the uppercase-hex SHA-1 of the file in `tw_char.migrations`
   (Name `20260919090000_character`, mirroring init-db-updates.sh).
3. `.env`: set `PLAYERBOT_PROVISION=Bram,1,1,0;Rowan,1,3,0;Elowen,10,8,1;Clem,3,5,0`
   (replaces the bare `Companion`; keep MIN/MAX 1/1, MIRROR 1, COOP 0).
4. `UPDATE tw_char.bot_ownership SET owner_account_id=4 WHERE
   char_guid IN (<the three new cohort guids>)` (Bram guid 2 is already
   bound to account 4).
5. `docker compose stop world; docker compose up -d world`; boot check.
   The operator character is kicked briefly during the restart.

Ambient population MIN/MAX stays 1/1: the cohort is owned companions
brought online through `.botrecruit`, not ambient population growth.

## Open notes

- The lab's quest cohort is Rowan/Elowen/Clem: Bram is provisioned (and
  idempotency-checked) but not test-logged in, so his spell-learning
  path is exercised on first `.botrecruit` in the personal deployment.
- Elowen is High Elf, not Night Elf: this fork's playercreateinfo
  (the table GetPlayerInfo validates against) has no race-4 (Night
  Elf) row for class 8 (Mage); race 4 valid classes here are
  {1,3,4,5,11}. The provisioner correctly rejected 4,8,1; 10,8,1
  (High Elf Mage, Alliance) is the nearest valid combination and
  keeps race diversity in the roster.
- Fork data quirks that touch the offense action matrix but not this
  requirement: warrior Heroic Strike is marked superseded_by 284
  (Shield Block) in skill_line_ability, and the hunter Arcane Shot
  slot maps to entry 142 (New Magic Missile Test). Learned sets are
  otherwise complete at each level gate, and in-game usage (Charge,
  Rend, class abilities) confirms usable skills per level.

