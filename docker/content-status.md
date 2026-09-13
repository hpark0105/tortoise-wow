# Content repairs and remaining validation

## Repaired in the personal server

- Dialogue validation now uses `broadcast_text` for positive IDs and the loaded `script_texts` store for negative IDs. Optional zero slots remain ignored; mandatory zero first slots and genuinely absent texts are still reported. The 31 previously reported legacy references all exist.
- `npc_teslinah` now registers the existing quest-accept handler. Its quest 80261 is deprecated and currently has no quest-giver bindings; this restores the code registration, not availability of that quest.
- Migration `20260911060000_world.sql` restores 109 rows for reference-loot groups 30171, 150112 and 30559, copied exactly from the base SQL at c99b445. The 20260511053220 table replacement omitted these groups, and 20260813194016 reintroduced their callers. Existing rows are preserved. Snowball and four other creature templates regain these loot branches.
- Literal `script_name='0'` placeholders become empty strings in creature/gameobject templates. Meaningful missing script names are retained.

## Validation

Build the candidate image, then run the opt-in integration check with your existing extracted assets:

```powershell
docker build --build-arg BUILD_JOBS=4 -t tortoise-local:content-fixes .
python docker/check_content_startup.py --image tortoise-local:content-fixes
```

The check creates a uniquely named disposable database/world project without publishing world ports. It verifies startup, script registration, exact loot rows and item/condition dependencies, then injects six isolated dialogue fixtures covering valid/missing positive and negative IDs, a mandatory zero slot and mixed optional slots. Cleanup removes only that disposable project's volumes. Logs and results stay under ignored `local/`. It requires game assets and does not run in public CI.

These checks establish startup/data correctness; they do not substitute for observing actual boss drops or quest interactions in the client.

## Remaining work

Seventeen distinct missing script names remain, including custom portals, Wrath's optional hook, special items and NPC interactions. Do not register empty handlers to suppress those warnings.

Custom portals require a dedicated change with click-path and teleport tests. The 13 templates are generic objects; the client-use handler rejects that type before ordinary script dispatch. Ten template names have matching `areatrigger_teleport` definitions, but entry 112917 is reused at several unrelated coordinates, including map 815. Verify per-placement destinations/access behavior. Scarlet Citadel and the Caverns of Time placeholders lack a confirmed matching destination.

An unmerged [upstream portal proposal #414](https://github.com/tortoise-wow/tortoise-wow/pull/414) describes mappings and a clickable-type change, but had no live click/teleport validation. Treat it as a lead, not a tested fix. Preserve native phase, level, condition, corpse/ghost, raid-combat, instance and teleport checks when implementing support. Do not infer that base Wrath damage is broken solely from a missing script hook.
