"""BL-009/010 deterministic qualification summary; live evidence remains explicit."""
def qualify(disabled, observed, trial, real_model=False):
    return {"disabled": disabled, "observed": observed, "trial": trial,
            "real_model_evidence": bool(real_model),
            "status": "qualified" if real_model and trial else "pending-real-world-evidence"}
