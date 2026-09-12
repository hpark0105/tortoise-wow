# Native persistent-bot initialization - 2026-09-12

Status: implemented and validated on a disposable review image. Not deployed to
the personal realm and not yet an earned-gameplay persistence claim.

## Result

Fresh persistent bots now use the server's native `Player::Create` path with a
temporary SEC_PLAYER bot session. The native path supplies the configured
starting level and money, race/class location, stats, taxi state, default
race/class spells, and starting inventory. `MasterPlayer::Create` supplies the
native action buttons. The bot then saves directly and synchronously, receives
its native homebind, and is published to `bot_ownership`, `playerbot`, and the
player cache only after native state is ready.

Default race/class spells are intentionally dependent data in this core:
`LearnDefaultSpells` derives them from `playercreateinfo_spell` on creation and
login, and `_SaveSpells` does not duplicate dependent spells in
`character_spell`. The equivalence test verifies this behavior rather than
requiring rows that normal native creation does not write. The initial
`characters.zone` is likewise `0`; login derives the cached zone. Homebind uses
the configured `playercreateinfo` zone and coordinates.

Fresh character GUIDs now come from `GeneratePlayerLowGuid`; account identities
remain in the stable synthetic range required by C1/C5. Existing fully bound
version-2 fixture bots are left unchanged. An unbound legacy/raw character has
no native provision marker and fails closed instead of being silently upgraded.

## Resumability across mixed storage engines

Migration `20260912120000_character.sql` adds the InnoDB
`bot_provision_state` table. Phase 1 reserves name/account/GUID before native
saving; phase 2 means native character, action, inventory, and homebind state is
ready for ownership/roster publication.

`characters`, `playerbot`, and `character_action` are MyISAM while item and
marker/ownership tables include InnoDB. Cross-table atomic rollback is not
claimed. Recovery follows these bounded states:

- A phase-1 marker with no character consumes a safe current core GUID on
  restart, moving the marker if the old unused GUID is below the generator
  frontier.
- A phase-1 marker plus a matching reserved character, with no binding or
  roster, identifies this provisioner's partial native save. The server uses
  the core permanent-delete path, verifies the character is gone, and retries
  the same materialized GUID/account. Any ownership/roster state makes cleanup
  fail closed.
- A phase-2 character without ownership/roster resumes publication in place.
  Binding-first failure is retryable; a later run adds the missing roster row.
- Marker/character identity disagreement, an unmarked orphan, a real-account
  owner, or an unsupported binding version is rejected without adoption.

## Validation

- Final Docker build passed using Ubuntu 22.04, `BUILD_JOBS=2`, and
  `-DALLOW_TURTLE_ADDONS=ON`; image `tortoise-local:native-review` only.
- Native equivalence/idempotency: 5/5 passed in 66.877 seconds. Direct database
  comparisons covered starting character fields, health, derived-spell
  semantics, action buttons, item templates/counts, homebind, phase-2 marker,
  one binding/roster, and restart no-op.
- Combined provisioning suite on the implementation before the final cleanup
  strengthening: 22/22 passed in 444.779 seconds. It covers R1/R2/R3, fail-closed bindings,
  native equivalence, idempotency, native-ready resume, and binding failure.
- Final partial-native recovery: 2/2 passed in 62.260 seconds. A trigger rejected
  action persistence after character/inventory writes. Before retry there was a
  phase-1 character and inventory with zero ownership and roster. Retry cleaned
  only that marker-matched synthetic identity, reused its GUID/account, and
  produced one native inventory, binding, roster, and phase-2 marker.
- Exact-worktree rerun after final review: the five native
  equivalence/idempotency cases plus both partial-save recovery cases passed
  7/7 in 128.962 seconds against `tortoise-local:native-review`.
- Fast gates: baseline 5/5, telemetry configuration 4/4, DBC helpers 2/2;
  Python compilation, Compose configuration, and `git diff --check` passed.
- All disposable test projects and volumes were removed after identity and
  no-published-port checks. The personal server and `tortoise-local:dev` were
  untouched.

The interrupted full retry that injected failure at `character_inventory`
produced a safe retry but intentionally failed a test assertion expecting item
rows; InnoDB rolled the item rows back. The later action-persistence injection
is the accepted stronger test because it proves cleanup after persisted items.

## Review limits and next gate

Retrieval's database was healthy but its embedding service was unhealthy and
the index remained incomplete, so bounded direct current-source reads were the
declared fallback. No sync or embedding/derived-write counts are claimed.

Fresh serialized `park-agent` review attempts were stopped because the worker
read paths outside the explicit evidence-card allowlist and, during earlier
attempts, observed source changing after its hashes were captured. No worker
report is accepted or inherited. Hosted source inspection, builds, diffs, and
runtime labs are the evidence above.

This closes the finding-4 native-initialization gate for fresh provisioning.
It does not prove earned state across restart/restore, in-game combat/quest
correctness, companion control, or population capacity. Next is R6's bounded
nonblocking telemetry sink, then R5 save-boundary failure coverage, R4 stale
completion coverage, and TW-011 earned-state restart/restore.
