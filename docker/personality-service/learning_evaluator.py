"""BL-007 pure, fail-closed candidate evaluator; no DB or engine authority."""
from dataclasses import dataclass

@dataclass(frozen=True)
class Outcome:
    eligible: bool
    unprotected_fraction: float
    recovery_latency_ms: float
    deaths: int = 0

def evaluate(baseline, candidate, minimum_samples=3):
    if len(baseline) < minimum_samples or len(candidate) < minimum_samples:
        return "inconclusive"
    if any(not x.eligible for x in baseline + candidate):
        return "inconclusive"
    # Lower is better; require a bounded practical improvement and no death regression.
    b = sum(x.unprotected_fraction for x in baseline) / len(baseline)
    c = sum(x.unprotected_fraction for x in candidate) / len(candidate)
    bd = sum(x.deaths for x in baseline); cd = sum(x.deaths for x in candidate)
    if cd > bd:
        return "regression"
    if b > 0 and (b - c) / b >= 0.10 and c < b:
        return "improvement"
    return "no_effect"

def assign_arm(encounter_index, candidate_id, eligible=True):
    if not eligible or not candidate_id:
        return "baseline"
    return "candidate" if encounter_index % 2 == 0 else "baseline"
