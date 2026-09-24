"""Closed, low-frequency off-duty bot decision contract.

The shared local model chooses only `roam` or `rest`. The world owns all
movement, combat, legality, and fallback behavior; model output cannot be a
chat/GM command, coordinate, target, item, or spell.
"""
import json
import re


REQUEST = re.compile(r"^([A-Za-z]{2,12})\|(roam|rest)$")
INTENTS = frozenset(("roam", "rest"))
PROFILES = frozenset(("none", "reckless", "cautious"))


def build_messages(profile, request):
    if profile not in PROFILES:
        return None
    match = REQUEST.fullmatch(request)
    if not match:
        return None
    name, last_intent = match.groups()
    return [
        {"role": "system", "content": (
            "You choose one quiet off-duty activity for a named character "
            "in a fantasy game world. Reply with exactly one JSON object: "
            '{"intent":"roam"} or {"intent":"rest"}. '
            "Roam means a short safe walk near the current home point; "
            "rest means stay there briefly. You cannot choose combat, travel "
            "destinations, quests, dialogue, tools, or commands. No prose.")},
        {"role": "user", "content": (
            "Character: %s. Disposition: %s. Previous choice: %s. "
            "Choose the next quiet activity." % (name, profile, last_intent))},
    ]


def parse_intent(text):
    try:
        doc = json.loads(text)
    except (TypeError, ValueError):
        return None
    if type(doc) is not dict or set(doc) != {"intent"}:
        return None
    intent = doc["intent"]
    return intent if type(intent) is str and intent in INTENTS else None
