# Ten-bot disposable cohort (TW-013 / KAP-556)

Status: passed 2026-09-13.

```gherkin
Scenario: Sustain a paced ten-bot cohort
Given ten valid persistent identities and an exact target of ten
When the isolated world runs for 30 minutes
Then ten distinct sessions remain online, ownership stays intact, and resource
and processing telemetry is retained
```

`python docker/bot_cohort_ten.py --seconds 1800 --interval 10` completed in
1,800.8 seconds in port-free project `tortoise-bot-cohort-aa39194d7092`.
The harness recorded 10 unique logins, 10 online DB rows, 10 ownership rows,
1,548 wander actions, no combat or loot actions in the intentionally empty
spawn box, and no crash. The project and both volumes were removed after the
report was written.

Across 153 Docker samples, the world used 36.08% mean CPU, 50.32% p95, and
62.09% max. World memory moved from 1.065 GiB to 1.267 GiB. The database used
0.78% mean CPU, 2.70% p95, and 8.08% max; memory moved from 270.3 MiB to
272.2 MiB. The telemetry sink retained 359 windows. Processing p95 had a
16 ms median and 67 ms maximum; p99 reached the 250 ms histogram overflow
bucket in intermittent windows. The tail spikes are a scaling signal to track
at larger cohorts, while this ten-bot gate remained responsive and stable.

Raw evidence is gitignored at
`local/tortoise-bot-cohort-aa39194d7092-20260913T131059Z`.
