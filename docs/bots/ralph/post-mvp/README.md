# Living-world post-MVP queue

Current dispatch is the [chunked companion port queue](../companion-port/README.md).
CMP-011 is now a hosted roll-up after PORT-010, not an editing assignment.
The order below is historical; use prd.json dependencies and priorities.

These cards continue KAP-543 after the one-companion persistence MVP. Select
the lowest-priority dependency-ready item with `passes:false` in `../prd.json`.
Each card is one bounded implementation or validation session.

The upstream assessment is in `../../azerothcore-reference-review.md`.
AzerothCore code is reference material only; this branch remains a Turtle
1.18.1 server and every behavior must pass against its current source and data.

Order:

1. CMP-010 real party membership and persistent roster state.
2. CMP-011 deterministic assist/defend/attack commands.
3. CMP-012 tank threat policy.
4. CMP-013 healer triage policy.
5. CMP-014 damage assist and pull discipline.
6. POP-020 independent regional director and observability.
7. POP-021 50-active qualification, followed by later 100/250/500 cards.
8. LLM-010 bounded party-planner transport after deterministic roles work.

Economy automation and dungeon stat scaling remain optional backlog decisions.
