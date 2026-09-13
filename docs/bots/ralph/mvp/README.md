# Playable companion MVP

Reviewed 2026-09-12 on `feature/kap-543-bot-living-world`. This folder splits the remaining MVP work into assignments small enough for one fresh local-model session. `../prd.json` is the machine-readable queue.

## Corrected assessment

The local-model summary accurately describes current provisioning: only the name is configurable and fresh bots are Human male Warriors with zeroed appearance fields and the native Human Warrior spawn/homebind. It also correctly identifies R5, R4 and TW-011 as open gates.

The claim that TW-011 is ?the only remaining integration proof? is too optimistic. Current default AI calls `SelectNearestTarget`, which the core defines as the nearest hostile unit. The local summary?s ?anything? wording is too broad, although attackable hostile players can still qualify. The AI has no demonstrated normal corpse-loot or quest interaction. TW-011 requires legitimately earned XP, an item and supported quest progress, so those capabilities and their focused proofs must precede restart/restore.

A foundation persistence gate ends at MVP-008. A playable companion MVP additionally requires owner-safe target selection (MVP-003) and TW-014 follow/stop. One fixed Human Warrior is acceptable for this first demo. Race/class/appearance configuration is a later story before multi-role companions.

## Current status (2026-09-13)

MVP-001 through MVP-009 pass (see ../progress.txt and ../prd.json), including TW-014/KAP-557 owner-only follow/stop. The playable companion MVP (KAP-543) is complete pending the operator's in-game validation.

## Execution order

1. MVP-001 save-boundary rejection.
2. MVP-002 stale-completion injection and rejection.
3. MVP-003 owner-safe targeting (source/build gate plus disposable two-faction lifecycle proof).
4. MVP-004 normal combat XP.
5. MVP-005 normal corpse loot.
6. MVP-006 one declared supported quest.
7. MVP-007 restart comparison.
8. MVP-008 isolated restore comparison; this completes TW-011/KAP-554.
9. TW-014/KAP-557 deterministic owner-only follow/stop; this completes the playable companion MVP.

TW-012, TW-013 and NEXT-001 (configured native bot identity) now pass. The 50/100/250/500 population stages and LLM party planning remain later epic milestones.

## Worker rule

Select exactly one dependency-ready item whose `passes` is false. Read only its card and named current files, verify hashes supplied by the head, and return bounded findings or a diff plus validation evidence. A worker does not change Jira or mark acceptance. Retrieval sync remains operator-paused; the active head must declare direct-read fallback until it resumes.
