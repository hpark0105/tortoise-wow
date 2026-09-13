# KAP-543 API reconciliation

Subsequent authorized publication: KAP-558 was created as one Story for the
companion-port README wave, with Gherkin scenarios and parent KAP-543 submitted.
Readback confirmed To Do and the description; parent is omitted by the connector.
The no-write statements below describe the preceding review, not this publication.

Source: live Atlassian read_jira_issue responses for
[KAP-543](https://parkenstein.atlassian.net/browse/KAP-543) and 14 known mapped
issues. Epic updated timestamp: 2026-09-13T10:05:46.182-0500.
This review made no Jira writes, comments, creations or status transitions.

## Verified issue mappings

| Local story | Jira | Live status |
|---|---|---|
| TW-001 | [KAP-544](https://parkenstein.atlassian.net/browse/KAP-544) | Done |
| TW-002 | [KAP-545](https://parkenstein.atlassian.net/browse/KAP-545) | Done |
| TW-003 | [KAP-546](https://parkenstein.atlassian.net/browse/KAP-546) | Done |
| TW-004 | [KAP-547](https://parkenstein.atlassian.net/browse/KAP-547) | Done |
| TW-005 | [KAP-548](https://parkenstein.atlassian.net/browse/KAP-548) | Done |
| TW-006 | [KAP-549](https://parkenstein.atlassian.net/browse/KAP-549) | Done |
| TW-007 | [KAP-550](https://parkenstein.atlassian.net/browse/KAP-550) | Done |
| TW-008 | [KAP-551](https://parkenstein.atlassian.net/browse/KAP-551) | Done |
| TW-009 | [KAP-552](https://parkenstein.atlassian.net/browse/KAP-552) | Done |
| TW-010 | [KAP-553](https://parkenstein.atlassian.net/browse/KAP-553) | Done |
| TW-011 | [KAP-554](https://parkenstein.atlassian.net/browse/KAP-554) | In Progress |
| TW-012 | [KAP-555](https://parkenstein.atlassian.net/browse/KAP-555) | In Progress |
| TW-013 | [KAP-556](https://parkenstein.atlassian.net/browse/KAP-556) | In Progress |
| TW-014 | [KAP-557](https://parkenstein.atlassian.net/browse/KAP-557) | In Progress |

MVP-001 additionally maps to KAP-550, MVP-002 to KAP-552, and MVP-008
to KAP-554 in prd.json. These are local subdivisions, not separate verified
Jira children. Local passes=true values were preserved; this review did not
rerun or independently reaccept their gameplay evidence.

## Epic scope and next milestone

The epic remains In Progress and requires 500+ persistent identities, meaningful
activity in human-free regions, 2-5 personal companions (human plus four active),
normal saved progression, and deterministic play with unavailable inference.
The PORT queue is a smaller intermediate milestone under that outcome. It does
not replace the epic's multi-companion, role, regional or capacity acceptance.

The epic description's explicitly dated September 11 baseline is historical:
it names personal-server, bots disabled, save/lifecycle blockers and an unfinished
candidate build. Current local work uses feature/kap-543-bot-living-world and
contains subsequent persistence/follow/cohort evidence and unfinished party edits.
Do not use that historical paragraph to reopen completed implementation blindly.

KAP-554 through KAP-557 are still In Progress despite local passes=true.
Keep Jira delivery status separate from local lab evidence. KAP-557 expressly
includes local in-game follow/range/stop/relog checks; source-only review cannot
close it. Follow the applicable hosted QA pipeline before any future transition.

## New work mapping

- PORT-001: new maintained Playerbots provenance spike; extends the historical
  TW-001/TW-002 engine research, rather than reopening their old candidate scope.
- CMP-010: existing local party/roster implementation; separate from KAP-557's
  narrow follow/stop behavior. Review dirty work before dispatch.
- PORT-002: bench persistence repair extends lifecycle/population behavior covered
  by KAP-555; do not fold it into that already-tested historical slice silently.
- PORT-003 through PORT-009: new useful-companion behaviors, mapped locally to the
  KAP-543 outcome. Explicit scopes and upstream sources are in the story cards.
- PORT-010: hosted gameplay acceptance, followed by CMP-011 roll-up.
- CMP-012/013/014, POP-020/021 and LLM-010 remain later local backlog.

No Jira keys are assigned to these new cards. Their existing 'Not created'
metadata means not created by this workflow; it is not a verified absence of
equivalent issues elsewhere in Jira.

## API limitations and safe publication follow-up

Both parent search and project/label search failed with:
Cannot read properties of undefined (reading 'map').
Individual reads worked, but their projected fields omit parent and comments.
Therefore full child enumeration, actual parent links, recent comments and
duplicate detection remain unverified. Description references to the epic are
not proof of the parent field.

Before publishing the local backlog, obtain a working child enumeration, reuse
any matching stories, create only missing stories under verified parent KAP-543,
and write returned Jira keys back into prd.json and cards. Do not create a
duplicate CMP-011 implementation; it is a roll-up. The present request was handled
as an API review and local accounting, not authority to post comments or change
delivery states.

## Governance and retrieval

Hosted Codex retains dispatch, source verification, commit/diff review and final
acceptance. No worker acceptance flags were changed here. No local delegation
was needed for this bounded metadata reconciliation; the launcher execution-policy
block from the earlier review also remains unresolved. Retrieval stays paused
by the operator; no embedding or derived-write counts are claimed.
