# BL-001A worker evidence card v1

- Objective: extract the smallest pure, reusable rotation-plan selection seam from
  the existing offense selector while preserving its exact baseline decision. Do
  not enable LLM learning or change live behavior.
- Repository/branch/commit: `tortoise-wow`, `feature/kap-558-port-phase2`,
  `c5781974b444f6ad692b48a516b0994d8cf59bc5`.
- Retrieval: `tortoise-wow` was fresh before dispatch; source citations:
  `PlayerBotAI.cpp:1297-1416`, `PlayerBotAI.cpp:1621-1740`, and
  `Companion/Combat.h:325-444`. Re-read current source and verify hashes first.
- Verified SHA-256: `PlayerBotAI.cpp`
  `4CE3B770C07627EBDFA828CF3FA659BB1142E20E445B166D9A519E1821F2FAA5`;
  `Companion/Combat.h`
  `0523691FFD564BA3DB7EA51A1476EE693D4CCC72C276C8535B5AC53D55495474`;
  `Companion/Policy.h`
  `EEDC44AEA59969C9AB5E7AC05D6385FCF4A37226CBB341FAA3984DC920AE4320`;
  `PlayerBotAI.h`
  `1660C58E567937820323A89F9F69B134AD0EAB51862BD038B7CEDB0B23AADB45`.
- Allowed reads: the four files above; `docker/test_companion_combat_value.py` and
  `.cpp`; `docs/prd/bram-party-learning.prd`; this evidence card.
- Allowed edits: `src/game/PlayerBots/Companion/Combat.h`,
  `src/game/PlayerBots/PlayerBotAI.cpp`, and the two named combat value tests only.
- Required implementation: in `Combat.h`, add a bounded value-only plan carrying
  an ordered list of profile indices, a baseline constructor for `N` profiles, a
  compatibility check requiring an exact non-duplicate permutation of `0..N-1`,
  and a selector overload that uses a compatible plan or falls back to existing
  table order. Preserve the original `SelectExecutable(profiles, query)` API as
  the baseline wrapper. In `PlayerBotAI.cpp`, create/use only the baseline plan
  for the existing populated profile vector before selection. Do not add class
  enums, a new header, spell metadata, or non-baseline runtime plans in this card.
- Requirements: preserve every baseline class-action table's order, rank lookup,
  on-next-swing exclusion, capability checks, and ordinary-attack fallback. The
  baseline/null/incompatible plan must produce the same selected ability as the
  present selector. The plan holds indices only: it cannot name raw spell IDs,
  target/coordinates, or arbitrary conditions. Do not apply a non-baseline plan
  in runtime behavior yet.
- Performance refinement: `RotationPlan`, baseline creation, compatibility checks,
  and plan-based selection must allocate no heap memory. Use a fixed maximum of
  eight profile indices (the PRD's ability cap) and scan profiles directly in plan
  order. Factor shared eligibility classification instead of copying/reordering a
  profile vector every companion offense tick.
- Expected output: a minimal patch plus tests demonstrating baseline order,
  reordered pure selection, null/incompatible fallback, duplicate/out-of-range
  rejection, and no executable learning behavior. State any architecture decision
  that would require scope expansion.
- Validation: run the focused value test(s), `python -m py_compile
  docker/server.py`, and `git diff --check`. Do not build Docker or start services.
- Stop boundary: need to edit files outside scope; cannot preserve exact baseline;
  discovered runtime behavior change; access to secrets/live character data;
  unavailable or occupied local model.

You are a bounded local worker. Do not use retrieval or Jira; launch any nested
agent/model; read `.env`, private character data, backups or raw logs; stage,
commit, push, deploy, transition issues, edit acceptance flags, or claim final
acceptance. Return changed paths, hash verification, test results and uncertainty.

Safety attestation by head: supplied evidence is current, bounded and contains no
secrets; this is the sole editing worker; its edits/tests are authorized and do not
touch the personal realm.
