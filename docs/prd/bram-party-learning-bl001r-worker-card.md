# BL-001R worker evidence card v1

Date: 2026-09-22
Repository: `C:\Users\hpark\WebstormProjects\tortoise-wow`
Branch: `feature/kap-558-port-phase2`
Baseline commit: `c5781974b444f6ad692b48a516b0994d8cf59bc5` with accepted, uncommitted BL-001A/B changes

## Objective

Perform a read-only viability audit for exactly one current-level Bram rotation
candidate. Produce a bounded experiment specification; do not implement it.

Candidate under audit:

- baseline catalog priority: Charge, Hamstring, Rend, Heroic Strike;
- candidate priority: Charge, Rend, Hamstring, Heroic Strike;
- the only delta is swapping Rend ahead of Hamstring;
- hypothesis: when both target effects are absent and Charge is unavailable or
  already consumed, applying Rend first begins hostile damage-over-time earlier,
  improving effective hostile damage per active combat second without changing
  targets, pulls, legal abilities, resource rules, emergency behavior, or the
  later opportunity to apply Hamstring.

Sanitized current capability fingerprint supplied by the hosted head from a
read-only live query: level-9 warrior; relevant learned ranks are Charge 100,
Heroic Strike 284, Rend 772, Hamstring 1715. Do not access any database or
private character/account data. The current chain resolver starts from catalog
bases 100, 7372, 772, and 78 and selects the highest learned rank. Heroic Strike
is currently blocked by the accepted on-next-swing exclusion.

## Allowed reads

- this card
- `docs/prd/bram-party-learning.prd`
- `docs/prd/bram-party-learning-handoff.md`
- `src/game/PlayerBots/PlayerBotAI.cpp`
- `src/game/PlayerBots/Companion/Combat.h`
- `docker/test_companion_combat_value.cpp`

## Edit permission

None. Do not edit any file. Do not list directories or inspect Git metadata.
Do not commit, stage, deploy, start services, access secrets/private data, call
retrieval, or launch another model session.

## Verified provenance

- Retrieval healthy/fresh after accepted BL-001B sync.
- `PlayerBotAI.cpp` SHA-256: `55C0706EF177BD47715F20B599D7DF0962053A7F4A69AF2868BFA78F1948A77B`
- `Combat.h` SHA-256: `44A9B6176F1FED8402065D210593AFD40F9BD7E3A4DE148EB797B74CBE48ED8A`
- value test SHA-256: `4C5F3A114CF7F98FAD28F5909FCF79C34D5BB7B3D24ACF2C376580118EECBDAE`

## Required report

Return:

1. VIABLE, NOT VIABLE, or INCONCLUSIVE with a source-grounded reason.
2. Exact applicability: known-action/effect conditions and engagement routes.
3. One primary metric and bounded secondary/safety metrics with explicit units.
4. A deterministic synthetic fixture and a later disposable-world fixture that
   can expose a legal choice difference between baseline and candidate.
5. Exclusions and confounders, including short fights, pre-existing auras,
   Charge range, rage availability, blocked Heroic Strike, owner overrides,
   deaths, and capability changes.
6. The capability fingerprint fields that invalidate comparison.
7. What BL-002 must record so BL-005/BL-007 can evaluate this candidate.
8. Explicit uncertainty; do not claim measured improvement.

Do not propose a second candidate or expand scope.
