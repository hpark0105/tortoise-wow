# BL-006 evidence card — bounded planner/reflection prompt contracts

## Objective

Implement and test the pure local-side prompt/schema boundary for tactical planning and reflection. This card must not connect runtime combat, persistence, or a live model.

## Allowed files

- `docker/personality-service/real_planner.py`
- `docker/personality-service/test_learning_prompts.py` (new)
- this card

## Requirements

- Add strict, deterministic builders for bounded tactical-planner and reflection requests.
- Include only capability fingerprint, accepted playbook/version, at most three sanitized lessons, and bounded encounter summaries.
- Reject/omit fabricated evidence, raw chat, database credentials, raw spell IDs, targets, coordinates, executable conditions, and unknown fields.
- Enforce request/response size limits and explicit schema/version markers.
- Keep existing v1 personality behavior unchanged.
- Expose latency/contention metadata as values only; do not add a second model or direct DB access.

## Validation

Run `python docker/personality-service/test_learning_prompts.py -v` and `git diff --check`. Do not edit C++ or start the realm.

## Evidence supplied

PRD BL-006 requires strict schemas, fake inference, fabricated-evidence rejection, latency/contention measurement, and no direct DB authority. Current `real_planner.py` is the existing v1 prompt adapter and must remain backward compatible.

## Uncertainties

This card does not implement candidate evaluation, assignment, promotion, or runtime Bram control; those belong to BL-007 and later.
