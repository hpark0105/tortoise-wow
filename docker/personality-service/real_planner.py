"""PORT-021 (KAP-558): real-model planner adapter (value level).

Maps a planner protocol v1 request to a bounded model prompt, maps the
model's JSON back to protocol steps, and falls back to deterministic
Hold steps when the provider is unavailable or unparseable. No I/O
here: model_client owns the model transport, real_planner_server owns
the sockets. fake_planner remains the byte reference for encoding.

Safety invariants:
- The prompt carries only the request's value fields (bot ordinals,
  class names, flags, order generation) plus the static versioned
  primer. Never GUIDs, coordinates, item ids, names or any live world
  observation.
- The model may only express the closed action vocabulary WITHOUT
  targets (none, hold, follow, defend, regroup, preference). Assist and
  loot require a resolvable world target, which the request does not
  carry and the provider can therefore never name.
- Every response has >= 1 step; provider failure is a Hold step, never
  an empty or malformed payload.
- Capture policy: the response claims capture = request capture +
  offset, offset = clamp(max(2000, round_ms) + tick_ms, <= 15000).
  The claimed capture lands at or just past the next expected world
  tick fetch, inside the 1000 ms response-age budget, for any round
  that completes inside the transport's 5000 ms deadline. The step
  lifetime stays 1000 ms (the fake_planner reference value). The
  world applies the bounded Preference vocabulary at fetch time, so
  the plan age at application is at most tick + round; see
  planner-protocol.md (real-adapter capture policy).
"""
import hashlib
import json
import time
import os

import fake_planner as fp

PRIMER_VERSION = 1
PRIMER_NAME = "primer-v1.txt"
PRIMER_MAX_BYTES = 4096
# Snapshot digest of context/primer-v1.txt (value test pins it).
PRIMER_SHA256 = "9d0691ba1eb0e2a805888d1f55ca5df59455202b716e2447045d3755aeb2e16a"

CAPTURE_MIN_MS = 2000
CAPTURE_MAX_MS = 15000
STEP_TTL_MS = fp.STEP_TTL_MS  # 1000; must stay <= fp.STEP_TTL_MS semantics

# Closed action vocabulary the provider may express. Assist (3) and
# Loot (5) are deliberately absent: they require a resolvable world
# target GUID, which the protocol request does not carry.
ACTIONS = {
    "none": fp.ACTION_NONE,
    "hold": fp.ACTION_HOLD,
    "follow": fp.ACTION_FOLLOW,
    "defend": fp.ACTION_DEFEND,
    "regroup": fp.ACTION_REGROUP,
    "preference": fp.ACTION_PREFERENCE,
}
# id -> (PrefId, value_min, value_max); mirrors Personality.h sets.
PREF_IDS = {
    "follow_chase": (1, 0, 2),
    "expression": (2, 0, 1),
}
# WoW class bytes (1..11) -> bounded display names.
CLASS_NAMES = {
    1: "Warrior", 2: "Paladin", 3: "Hunter", 4: "Rogue", 5: "Priest",
    6: "Death Knight", 7: "Shaman", 8: "Mage", 9: "Warlock",
    10: "Monk", 11: "Druid",
}

SYSTEM_PROMPT = (
    "You are the behavior planner for one companion in a World of "
    "Warcraft (Turtle WoW 1.18) player party. You see only the static "
    "primer and the current party state; you never see items, "
    "coordinates, targets or other players' names.\n"
    "Respond with ONE JSON object and nothing else - no prose, no "
    "markdown, no commentary. Schema: "
    '{"steps":[{"bot":<int 1..N>,"action":"<none|hold|follow|defend|'
    'regroup|preference>"}]} . A step may add "preference":'
    '{"id":"follow_chase","value":<0-2>} or {"id":"expression","'
    'value":<0-1>}, and only with action "preference". At least one '
    "step, at most one step per bot. Never invent actions, targets, "
    "item names, numbers outside these ranges, or text. If the "
    "companion is held, propose only hold or none. Prefer keeping the "
    "companion with the party: follow when the leader is available "
    "and the companion is not held; hold when held or when unsure."
)


class PrimerError(Exception):
    pass


def load_primer(path=None):
    """(text, sha256hex) of the versioned primer; fails closed when the
    file is missing, oversized, or not version 1."""
    path = path or os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "context", PRIMER_NAME)
    try:
        with open(path, "rb") as f:
            raw = f.read()
    except OSError:
        raise PrimerError("primer missing: %s" % os.path.basename(path))
    if len(raw) > PRIMER_MAX_BYTES:
        raise PrimerError("primer oversized: %d bytes" % len(raw))
    # Git may check this text file out as CRLF on Windows. Pin the logical
    # versioned primer, not the host's newline convention.
    raw = raw.replace(b"\r\n", b"\n")
    text = raw.decode("utf-8")
    first = text.splitlines()[0] if text else ""
    if ("primer" not in first.lower() or
            ("version %d" % PRIMER_VERSION) not in first):
        raise PrimerError("primer version line missing")
    return text, hashlib.sha256(raw).hexdigest()


def class_name(cls):
    return CLASS_NAMES.get(cls, "Class%d" % cls)


def build_messages(request, primer_text):
    """[system, user] for one request byte payload. The user message is
    the entire world-facing context: primer + value fields only."""
    env = fp.decode_envelope(request)
    body = fp.decode_request_body(request[fp.ENVELOPE:])
    bots = []
    for i in range(body["bot_count"]):
        b = body["bots"][i]
        bots.append("  %d: %s" % (i + 1, class_name(b["cls"])))
    held = 1 if body["flags"] & 0x01 else 0
    following = 1 if body["flags"] & 0x02 else 0
    owner_available = 1 if body["flags"] & 0x04 else 0
    user = (
        "Context primer (version %d):\n%s\n"
        "Party state:\n"
        "leader: player\n"
        "bots:\n%s\n"
        "flags: held=%d following=%d owner_available=%d\n"
        "order_generation: %d\n"
        # /no_think is the Qwen3 thinking-mode switch; other models
        # ignore the token and the strict parser slices the JSON.
        "Answer with the JSON object only. /no_think"
    ) % (PRIMER_VERSION, primer_text, "\n".join(bots), held, following,
         owner_available, body["generation"])
    return [
        {"role": "system", "content": SYSTEM_PROMPT},
        {"role": "user", "content": user},
    ]


def _is_int(v):
    return isinstance(v, int) and not isinstance(v, bool)


def parse_model_text(text, bot_count):
    """Strict schema -> step list, or None (any violation).

    Tolerates surrounding prose: the outermost {...} slice is parsed.
    Returns dicts: bot (1-based ordinal), action (wire code),
    preference (packed (id<<8)|value, 0 unless preference).
    """
    if not isinstance(text, str):
        return None
    start = text.find("{")
    end = text.rfind("}")
    if start < 0 or end <= start:
        return None
    try:
        doc = json.loads(text[start:end + 1])
    except ValueError:
        return None
    if not isinstance(doc, dict) or set(doc.keys()) != {"steps"}:
        return None
    steps = doc["steps"]
    if not isinstance(steps, list) or not (1 <= len(steps) <= fp.MAX_STEPS):
        return None
    seen = set()
    out = []
    for step in steps:
        if (not isinstance(step, dict) or
                not set(step.keys()) <= {"bot", "action", "preference"}):
            return None
        if not _is_int(step.get("bot")) or not (1 <= step["bot"] <= bot_count):
            return None
        if step["bot"] in seen:
            return None
        seen.add(step["bot"])
        action = step.get("action")
        if not isinstance(action, str) or action not in ACTIONS:
            return None
        pref = 0
        if "preference" in step:
            if action != "preference":
                return None
            p = step["preference"]
            if not isinstance(p, dict) or set(p.keys()) != {"id", "value"}:
                return None
            pid = p.get("id")
            if not isinstance(pid, str) or pid not in PREF_IDS:
                return None
            value = p.get("value")
            pref_id, vmin, vmax = PREF_IDS[pid]
            if not _is_int(value) or not (vmin <= value <= vmax):
                return None
            pref = (pref_id << 8) | value
        out.append({"bot": step["bot"], "action": ACTIONS[action],
                    "preference": pref})
    return out


def fallback_steps(body):
    """Deterministic Hold for every bot in the request."""
    return [
        {"bot": i + 1, "action": fp.ACTION_HOLD, "preference": 0}
        for i in range(body["bot_count"])
    ]


def capture_offset_ms(round_ms, tick_ms):
    return min(CAPTURE_MAX_MS,
               max(CAPTURE_MIN_MS, int(round_ms)) + int(tick_ms))


def build_wire_response(request, steps, round_ms, tick_ms):
    """Protocol bytes for the parsed (or fallback) steps. login
    generation echoes the v1 constant (the request cannot carry per-bot
    login generations; protocol-v2 candidate) and order generation
    echoes the request, the world-side freshness field."""
    body = fp.decode_request_body(request[fp.ENVELOPE:])
    guid_by_ordinal = {i + 1: b["bot_guid"]
                       for i, b in enumerate(body["bots"][:body["bot_count"]])}
    wire = [
        {"bot_guid": guid_by_ordinal[s["bot"]],
         "login_generation": 1,
         "order_generation": body["generation"],
         "action": s["action"],
         "target_guid": 0,
         "preference": s["preference"]}
        for s in steps
    ]
    if not wire or any(w["bot_guid"] == 0 for w in wire):
        raise ValueError("step without a request bot")
    return fp.build_response(
        request, wire,
        capture_offset_ms=capture_offset_ms(round_ms, tick_ms))


# BL-006: bounded learning prompt contracts. These helpers are deliberately
# sidecar-only and have no database or engine authority.
LEARNING_SCHEMA_VERSION = 2
LEARNING_MAX_BYTES = 16 * 1024
LEARNING_MAX_LESSONS = 3
LEARNING_MAX_SUMMARIES = 5

def _clean_text(value, limit=160):
    if not isinstance(value, str) or len(value) > limit:
        raise ValueError("invalid bounded text")
    if any(x in value.lower() for x in ("spellid", "target_guid", "coordinate", "password", "token")):
        raise ValueError("unsafe evidence")
    return value

def build_learning_request(capability_fingerprint, playbook_version, lessons=(), summaries=(), request_id=""):
    if not isinstance(capability_fingerprint, str) or not capability_fingerprint or len(capability_fingerprint) > 128:
        raise ValueError("invalid capability fingerprint")
    if not isinstance(playbook_version, int) or playbook_version < 0:
        raise ValueError("invalid playbook version")
    if not isinstance(request_id, str) or len(request_id) > 96:
        raise ValueError("invalid request id")
    lessons = list(lessons); summaries = list(summaries)
    if len(lessons) > LEARNING_MAX_LESSONS or len(summaries) > LEARNING_MAX_SUMMARIES:
        raise ValueError("evidence limit")
    safe_lessons = []
    for lesson in lessons:
        if not isinstance(lesson, dict) or set(lesson) - {"id", "text", "evidence_count"}:
            raise ValueError("invalid lesson")
        if not isinstance(lesson.get("id"), str) or not isinstance(lesson.get("evidence_count"), int) or lesson["evidence_count"] < 1:
            raise ValueError("fabricated lesson")
        safe_lessons.append({"id": _clean_text(lesson["id"], 64), "text": _clean_text(lesson.get("text", "")), "evidence_count": lesson["evidence_count"]})
    safe_summaries = []
    for summary in summaries:
        if not isinstance(summary, dict) or set(summary) - {"id", "outcome", "duration_ms"}:
            raise ValueError("invalid summary")
        if not isinstance(summary.get("id"), str) or not isinstance(summary.get("duration_ms"), int) or summary["duration_ms"] < 0:
            raise ValueError("fabricated summary")
        safe_summaries.append({"id": _clean_text(summary["id"], 64), "outcome": _clean_text(summary.get("outcome", "")), "duration_ms": summary["duration_ms"]})
    doc = {"schema_version": LEARNING_SCHEMA_VERSION, "request_id": request_id, "capability_fingerprint": capability_fingerprint, "playbook_version": playbook_version, "lessons": safe_lessons, "summaries": safe_summaries}
    encoded = json.dumps(doc, separators=(",", ":"), sort_keys=True).encode("utf-8")
    if len(encoded) > LEARNING_MAX_BYTES:
        raise ValueError("request too large")
    return doc

def inference_metadata(start_ms, end_ms, contended=False):
    return {"latency_ms": max(0, int(end_ms) - int(start_ms)), "contended": bool(contended)}
