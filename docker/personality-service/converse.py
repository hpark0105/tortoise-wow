"""PORT-022 (KAP-558): bounded companion-conversation adapter (value level).

Maps one addressed player message to a bounded model prompt for the
companion's persisted personality, and maps the model's JSON back to one
sanitized text reply. No I/O here: model_client owns the model transport,
real_planner_server owns the sockets, and fake_planner_server is the
deterministic reference.

Safety invariants:
- The prompt carries only the companion's declared persona and the player's
  sanitized text. Never GUIDs, coordinates, item ids, or any live world
  observation.
- The model may only answer one JSON object {"reply": "..."} of plain
  conversational text. Unprovided game facts must be stated as unknown,
  never invented.
- Every reply is sanitized to bounded text: <= MAX_REPLY, printable ASCII,
  no control characters, no leading dot (it must never be reinterpreted as
  a command).
- Provider failure (offline, busy, timeout, malformed) is no reply, never a
  fabricated one; the world says nothing.
"""
import json

MAX_REPLY = 120
MAX_TEXT = 200  # mirror ConversationTransport kMaxTextBytes

# Static persona text per declared profile (none|reckless|cautious). The
# model sees only this and the player's text; the prompt never lives in C++.
PERSONAS = {
    "none": (
        "You are a quiet companion in a World of Warcraft (Turtle WoW 1.18) "
        "player party. You speak plainly and briefly, in a calm, neutral "
        "tone."),
    "reckless": (
        "You are a reckless, eager young companion in a World of Warcraft "
        "(Turtle WoW 1.18) player party. You are enthusiastic and a little "
        "overconfident, you jump in first, and you talk forward. You are "
        "still learning, so you are occasionally wrong about the fine "
        "details."),
    "cautious": (
        "You are a cautious, careful companion in a World of Warcraft "
        "(Turtle WoW 1.18) player party. You keep your composure, you think "
        "before you act, you prefer safety, and you talk covered. You are "
        "honest about what you are not sure of."),
}

CONVERSE_SYSTEM_PROMPT = (
    "You are one companion in a World of Warcraft (Turtle WoW 1.18) player "
    "party. The player has addressed you by name in party chat. Reply as "
    "yourself, in character, with one short conversational sentence or two. "
    "Keep it plain text under 120 characters. You know only your own "
    "personality and what the player just said; you know NOTHING about the "
    "game world, items, other players, or the quest log, and you must never "
    "invent such facts - if asked about them, say you are not sure. Never "
    "give orders, commands, or gameplay instructions, and never start your "
    "reply with a dot. Respond with ONE JSON object and nothing else - no "
    "prose, no markdown. Schema: {\"reply\": \"<your text>\"}."
)


def persona_for(profile_name):
    """Static persona blurb; unknown/empty names are the neutral one."""
    return PERSONAS.get(profile_name or "none", PERSONAS["none"])


def build_converse_messages(profile_name, text):
    """[system, user] for one conversation. The user message carries only
    the persona and the player's sanitized text."""
    user = ("Persona:\n%s\n\nThe player says: %s"
            "\nAnswer with the JSON object only. /no_think") % (
                persona_for(profile_name), (text or "").strip())
    return [
        {"role": "system", "content": CONVERSE_SYSTEM_PROMPT},
        {"role": "user", "content": user},
    ]


def sanitize_reply(text, max_len=MAX_REPLY):
    """Bounded conversational text, or '' (no reply) when empty.

    Keeps printable ASCII only (drops control characters and newlines),
    trims, removes a leading dot (a reply must never read as a command),
    and truncates to max_len."""
    if not isinstance(text, str):
        return ""
    out = []
    for ch in text:
        o = ord(ch)
        if 0x20 <= o <= 0x7e:
            out.append(ch)
    s = "".join(out).strip()
    while s.startswith("."):
        s = s[1:]
    s = s.strip()
    if len(s) > max_len:
        s = s[:max_len].rstrip()
    return s


def parse_converse_text(text, max_len=MAX_REPLY):
    """Strict schema -> sanitized reply string, or '' (no reply).

    Tolerates surrounding prose: the outermost {...} slice is parsed. The
    document must be exactly {"reply": "<string>"} and the sanitized text
    must be non-empty."""
    if not isinstance(text, str):
        return ""
    start = text.find("{")
    end = text.rfind("}")
    if start < 0 or end <= start:
        return ""
    try:
        doc = json.loads(text[start:end + 1])
    except ValueError:
        return ""
    if not isinstance(doc, dict) or set(doc.keys()) != {"reply"}:
        return ""
    reply = doc["reply"]
    if not isinstance(reply, str):
        return ""
    return sanitize_reply(reply, max_len)