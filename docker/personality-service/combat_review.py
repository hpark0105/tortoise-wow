"""Bounded, advisory post-fight conversation for a recruited party citizen.

Only six nonnegative recorder counters cross this boundary. The model gets no
character identity, location, target, account data, or authority to act.
"""
import re

import converse


_EVIDENCE = re.compile(
    r"duration_ms=(\d{1,9}) damage=(\d{1,9}) taken=(\d{1,9}) "
    r"casts_ok=(\d{1,9}) casts_rejected=(\d{1,9}) decisions=(\d{1,9})"
    r"(?: memory=(none|caution|timing|steady))?\Z")


def build_messages(profile, text):
    """Return a small model prompt, or None for malformed/oversize evidence."""
    if not isinstance(text, str) or len(text) > converse.MAX_TEXT:
        return None
    match = _EVIDENCE.fullmatch(text)
    if not match:
        return None
    numeric = match.groups()[:6]
    memory = match.groups()[6] or "none"
    values = tuple(int(value) for value in numeric)
    if values[0] > 3600000 or any(value > 100000000 for value in values[1:]):
        return None
    system = (
        "You are a companion in a World of Warcraft player party. A fight "
        "just ended. Reflect in character on your own performance from the "
        "six counters provided; speak to your party in one natural short "
        "sentence under 120 characters. Make one modest observation or "
        "intention for the next fight. Do not claim to know the enemy, "
        "abilities, quest, location, or cause of damage. Do not give a "
        "command or gameplay instruction. The counters are imperfect "
        "observations, not proof of success or failure. Output only one "
        "JSON object: {\"reply\": \"<your text>\"}.")
    user = ("Persona: %s\nFight counters: %s\nRecent party memory: %s\n"
            "Answer with the JSON object only. /no_think") % (
                converse.persona_for(profile), text, memory)
    return [{"role": "system", "content": system},
            {"role": "user", "content": user}]
