"""PORT-017 (KAP-558): deterministic fake party-planner service.

Reference encoder/validator for the planner protocol v1. The byte
layout must stay byte-identical with
src/game/PlayerBots/Companion/PlannerProtocol.h (the golden vectors
in both test suites pin it). This module performs no I/O, no model
call and no networking: it maps (request bytes, scenario) to an exact
outcome, which is what makes every required response class
reproducible.

Scenarios:
  success     valid response, one step per bot, correct echo
  delay       same bytes as success plus a transport delay marker
  timeout     no payload (the transport deadline passes first)
  malformed   truncated payload (last 4 bytes dropped)
  oversized   payload padded beyond the 4096 byte ceiling
  unsupported protocol_version + 1 (unknown version)
  stale       capture time 2000 ms in the past (age budget 1000 ms)
"""
import struct

MAGIC = 0x31504C43          # "CLP1"
PROTOCOL_VERSION = 1
OBSERVATION_VERSION = 2     # Companion::kObservationVersion (PORT-016)

ENVELOPE = 40
REQUEST_BODY = 44
STEP = 32
REQUEST_BYTES = ENVELOPE + REQUEST_BODY     # 84, fixed
MAX_STEPS = 8
MAX_PARTY_BOTS = 4
MAX_PAYLOAD_BYTES = 4096
MAX_RESPONSE_AGE_MS = 1000
MAX_STEP_LIFETIME_MS = 5000
STEP_TTL_MS = 1000           # step lifetime used by the golden vectors
MAX_PREFERENCE = 0xFFFF

# Closed action vocabulary (must mirror Companion::Planner::Action)
ACTION_NONE = 0
ACTION_HOLD = 1
ACTION_FOLLOW = 2
ACTION_ASSIST = 3
ACTION_DEFEND = 4
ACTION_LOOT = 5
ACTION_REGROUP = 6
ACTION_PREFERENCE = 7
ACTION_COUNT = 8
REQUIRES_TARGET = frozenset([ACTION_ASSIST, ACTION_LOOT])

REJECT_OK = "ok"
REJECT_BAD_MAGIC = "bad-magic"
REJECT_UNKNOWN_VERSION = "unknown-version"
REJECT_SIZE_MISMATCH = "size-mismatch"
REJECT_OVERSIZED = "oversized"
REJECT_TOO_MANY_STEPS = "too-many-steps"
REJECT_BAD_ID = "bad-id"
REJECT_BAD_RESERVED = "bad-reserved"
REJECT_UNKNOWN_ACTION = "unknown-action"
REJECT_MISSING_TARGET = "missing-target"
REJECT_FORBIDDEN_TARGET = "forbidden-target"
REJECT_OUT_OF_RANGE = "out-of-range"
REJECT_EXPIRED = "expired"
REJECT_STALE = "stale"
REJECT_MISMATCHED = "mismatched"

REJECT_NAMES = {
    0: REJECT_OK, 1: REJECT_BAD_MAGIC, 2: REJECT_UNKNOWN_VERSION,
    3: REJECT_SIZE_MISMATCH, 4: REJECT_OVERSIZED, 5: REJECT_TOO_MANY_STEPS,
    6: REJECT_BAD_ID, 7: REJECT_BAD_RESERVED, 8: REJECT_UNKNOWN_ACTION,
    9: REJECT_MISSING_TARGET, 10: REJECT_FORBIDDEN_TARGET,
    11: REJECT_OUT_OF_RANGE, 12: REJECT_EXPIRED, 13: REJECT_STALE,
    14: REJECT_MISMATCHED,
}


# ---------------------------------------------------------------------------
# Reference encoder (little-endian, fixed width)
# ---------------------------------------------------------------------------
def encode_envelope(magic, protocol_version, request_id, owner_guid,
                    observation_version, capture_time_ms, step_count,
                    total_size):
    return struct.pack("<IIQIIQII", magic, protocol_version, request_id,
                       owner_guid, observation_version, capture_time_ms,
                       step_count, total_size)


def decode_envelope(buf):
    (magic, protocol_version, request_id, owner_guid, observation_version,
     capture_time_ms, step_count, total_size) = struct.unpack_from(
         "<IIQIIQII", buf, 0)
    return {
        "magic": magic, "protocol_version": protocol_version,
        "request_id": request_id, "owner_guid": owner_guid,
        "observation_version": observation_version,
        "capture_time_ms": capture_time_ms, "step_count": step_count,
        "total_size": total_size,
    }


def encode_step(bot_guid, login_generation, order_generation, action,
                target_guid, expires_at_ms, preference):
    return struct.pack("<III4sIQI", bot_guid, login_generation,
                       order_generation, bytes([action, 0, 0, 0]), target_guid,
                       expires_at_ms, preference)


def decode_step(buf, off):
    (bot_guid, login_generation, order_generation, action_bytes,
     target_guid, expires_at_ms, preference) = struct.unpack_from(
         "<III4sIQI", buf, off)
    return {
        "bot_guid": bot_guid, "login_generation": login_generation,
        "order_generation": order_generation,
        "action": action_bytes[0], "reserved": action_bytes[1:],
        "target_guid": target_guid, "expires_at_ms": expires_at_ms,
        "preference": preference,
    }


def encode_request(generation, flags, bots):
    """bots: list of (bot_guid, cls); zero-filled to MAX_PARTY_BOTS."""
    body = bytearray()
    body += struct.pack("<IB3sI", generation, flags, b"\x00\x00\x00",
                        len(bots))
    for i in range(MAX_PARTY_BOTS):
        if i < len(bots):
            guid, cls = bots[i]
            body += struct.pack("<IB3s", guid, cls, b"\x00\x00\x00")
        else:
            body += b"\x00" * 8
    assert len(body) == REQUEST_BODY
    env = encode_envelope(MAGIC, PROTOCOL_VERSION, 0, 0,
                          OBSERVATION_VERSION, 0, 0, REQUEST_BYTES)
    return bytes(env + body)


def decode_request_body(buf):
    generation, flags, reserved, bot_count = struct.unpack_from(
        "<IB3sI", buf, 0)
    bots = []
    for i in range(MAX_PARTY_BOTS):
        off = 12 + 8 * i
        guid, cls, resv = struct.unpack_from("<IB3s", buf, off)
        bots.append({"bot_guid": guid, "cls": cls, "reserved": resv})
    return {"generation": generation, "flags": flags,
            "reserved": reserved, "bot_count": bot_count, "bots": bots}


def build_response(request, steps, capture_offset_ms=2000):
    """Echo the request envelope and append steps.

    steps: list of dicts with bot_guid, login_generation,
    order_generation, action, target_guid, preference.
    capture_offset_ms: response capture time relative to the request
    capture (deterministic; the age budget is relative to arrival).
    """
    req = decode_envelope(request)
    body = b"".join(encode_step(
        s["bot_guid"], s["login_generation"], s["order_generation"],
        s["action"], s["target_guid"],
        req["capture_time_ms"] + capture_offset_ms + STEP_TTL_MS,
        s["preference"])
        for s in steps)
    total = ENVELOPE + STEP * len(steps)
    env = encode_envelope(MAGIC, PROTOCOL_VERSION, req["request_id"],
                          req["owner_guid"], req["observation_version"],
                          req["capture_time_ms"] + capture_offset_ms,
                          len(steps), total)
    return bytes(env + body)


# ---------------------------------------------------------------------------
# Reference validator (mirrors the C++ Verdict logic exactly)
# ---------------------------------------------------------------------------
def validate_envelope(env, received_bytes, expected_bytes, is_request,
                      now_ms):
    if received_bytes > MAX_PAYLOAD_BYTES:
        return REJECT_OVERSIZED
    if received_bytes != expected_bytes:
        return REJECT_SIZE_MISMATCH
    if env["magic"] != MAGIC:
        return REJECT_BAD_MAGIC
    if (env["protocol_version"] != PROTOCOL_VERSION or
            env["observation_version"] != OBSERVATION_VERSION):
        return REJECT_UNKNOWN_VERSION
    if (env["request_id"] == 0 or env["owner_guid"] == 0 or
            env["capture_time_ms"] == 0):
        return REJECT_BAD_ID
    if is_request:
        if env["step_count"] != 0:
            return REJECT_TOO_MANY_STEPS
    else:
        if env["step_count"] == 0 or env["step_count"] > MAX_STEPS:
            return REJECT_TOO_MANY_STEPS
    if env["total_size"] != received_bytes:
        return REJECT_SIZE_MISMATCH
    if (not is_request and now_ms >= env["capture_time_ms"]
            and now_ms - env["capture_time_ms"] > MAX_RESPONSE_AGE_MS):
        return REJECT_STALE
    return REJECT_OK


def validate_request_body(body):
    if any(body["reserved"]):
        return REJECT_BAD_RESERVED
    if body["bot_count"] == 0 or body["bot_count"] > MAX_PARTY_BOTS:
        return REJECT_TOO_MANY_STEPS
    for i, b in enumerate(body["bots"]):
        used = i < body["bot_count"]
        if used and b["bot_guid"] == 0:
            return REJECT_BAD_ID
        if not used and (b["bot_guid"] != 0 or b["cls"] != 0 or
                         any(b["reserved"])):
            return REJECT_BAD_RESERVED
        if used and any(b["reserved"]):
            return REJECT_BAD_RESERVED
    return REJECT_OK


def validate_step(step, env, now_ms):
    if step["bot_guid"] == 0:
        return REJECT_BAD_ID
    if any(step["reserved"]):
        return REJECT_BAD_RESERVED
    if step["action"] >= ACTION_COUNT:
        return REJECT_UNKNOWN_ACTION
    if step["action"] in REQUIRES_TARGET and step["target_guid"] == 0:
        return REJECT_MISSING_TARGET
    if step["action"] not in REQUIRES_TARGET and step["target_guid"] != 0:
        return REJECT_FORBIDDEN_TARGET
    if step["preference"] > MAX_PREFERENCE:
        return REJECT_OUT_OF_RANGE
    if now_ms >= step["expires_at_ms"]:
        return REJECT_EXPIRED
    if (step["expires_at_ms"] > env["capture_time_ms"] and
            step["expires_at_ms"] - env["capture_time_ms"] >
            MAX_STEP_LIFETIME_MS):
        return REJECT_EXPIRED
    return REJECT_OK


def validate_request(payload, now_ms=0):
    if len(payload) < ENVELOPE:
        return REJECT_SIZE_MISMATCH
    env = decode_envelope(payload)
    v = validate_envelope(env, len(payload), REQUEST_BYTES, True, now_ms)
    if v != REJECT_OK:
        return v
    return validate_request_body(decode_request_body(payload[ENVELOPE:]))


def validate_response(request, response, now_ms):
    req = decode_envelope(request)
    res = decode_envelope(response)
    expected = ENVELOPE + STEP * res["step_count"]
    v = validate_envelope(res, len(response), expected, False, now_ms)
    if v != REJECT_OK:
        return v
    if (res["request_id"] != req["request_id"] or
            res["owner_guid"] != req["owner_guid"]):
        return REJECT_MISMATCHED
    for i in range(res["step_count"]):
        v = validate_step(decode_step(response, ENVELOPE + STEP * i), res,
                          now_ms)
        if v != REJECT_OK:
            return v
    return REJECT_OK


# ---------------------------------------------------------------------------
# Deterministic scenario outcomes
# ---------------------------------------------------------------------------
GOLDEN_NOW_MS = 1002000  # 2000 ms after the golden capture time

def golden_request():
    return {
        "protocol_version": PROTOCOL_VERSION,
        "request_id": 0x1122334455667788,
        "owner_guid": 610001,
        "observation_version": OBSERVATION_VERSION,
        "capture_time_ms": 1000000,
        "step_count": 0,
        "total_size": REQUEST_BYTES,
        "generation": 7,
        "flags": 0b010,
        "bots": [(610002, 1), (610003, 4)],
    }


def golden_response(request=None):
    request = request or make_request(golden_request())
    return build_response(request, [
        {"bot_guid": 610002, "login_generation": 1, "order_generation": 7,
         "action": ACTION_HOLD, "target_guid": 0, "preference": 0},
        {"bot_guid": 610003, "login_generation": 1, "order_generation": 7,
         "action": ACTION_ASSIST, "target_guid": 2500040, "preference": 0},
    ])


def make_request(spec):
    env = encode_envelope(
        MAGIC, spec["protocol_version"], spec["request_id"],
        spec["owner_guid"], spec["observation_version"],
        spec["capture_time_ms"], spec["step_count"], spec["total_size"])
    body = bytearray()
    body += struct.pack("<IB3sI", spec["generation"], spec["flags"],
                        b"\x00\x00\x00", len(spec["bots"]))
    for i in range(MAX_PARTY_BOTS):
        if i < len(spec["bots"]):
            guid, cls = spec["bots"][i]
            body += struct.pack("<IB3s", guid, cls, b"\x00\x00\x00")
        else:
            body += b"\x00" * 8
    return bytes(env + body)


def respond(request, scenario, now_ms=GOLDEN_NOW_MS):
    """Deterministic outcome for (request, scenario).

    Returns (payload_or_None, meta). meta carries transport markers
    (delay_ms) and, for timeout, the deadline that passed.
    """
    if scenario == "success":
        return golden_response(request), {"delay_ms": 120}
    if scenario == "delay":
        # Same payload, a slower transport: still inside the 500 ms P95
        # budget, so the response is legal on arrival.
        return golden_response(request), {"delay_ms": 400}
    if scenario == "timeout":
        return None, {"deadline_ms": 5000, "delay_ms": 6000}
    if scenario == "malformed":
        payload = bytearray(golden_response(request))
        del payload[-4:]  # truncate: size no longer matches
        return bytes(payload), {"delay_ms": 120}
    if scenario == "oversized":
        payload = golden_response(request) + b"\x00" * (MAX_PAYLOAD_BYTES + 128)
        return payload, {"delay_ms": 120}
    if scenario == "unsupported":
        req = decode_envelope(request)
        body = b"".join(encode_step(
            s["bot_guid"], s["login_generation"], s["order_generation"],
            s["action"], s["target_guid"],
            req["capture_time_ms"] + 2000 + STEP_TTL_MS, s["preference"])
            for s in [
                {"bot_guid": 610002, "login_generation": 1,
                 "order_generation": 7, "action": ACTION_HOLD,
                 "target_guid": 0, "preference": 0}])
        total = ENVELOPE + STEP
        env = encode_envelope(MAGIC, PROTOCOL_VERSION + 1, req["request_id"],
                              req["owner_guid"], req["observation_version"],
                              req["capture_time_ms"] + 2000, 1, total)
        return bytes(env + body), {"delay_ms": 120}
    if scenario == "stale":
        req = decode_envelope(request)
        # The service answered a capture 2000 ms old: past the 1000 ms age
        # budget, so the caller must discard it and keep the deterministic
        # policies.
        body = encode_step(610002, 1, 7, ACTION_HOLD, 0,
                           req["capture_time_ms"] + STEP_TTL_MS, 0)
        total = ENVELOPE + STEP
        env = encode_envelope(MAGIC, PROTOCOL_VERSION, req["request_id"],
                              req["owner_guid"], req["observation_version"],
                              req["capture_time_ms"], 1, total)
        return bytes(env + body), {"delay_ms": 120}
    raise ValueError("unknown scenario: %s" % scenario)


SCENARIOS = ("success", "delay", "timeout", "malformed", "oversized",
             "unsupported", "stale", "chase", "express")


# ---------------------------------------------------------------------------
# Golden vectors (byte-identical to the C++ value test)
# ---------------------------------------------------------------------------
if __name__ == "__main__":
    req = make_request(golden_request())
    res = golden_response(req)
    print("golden_request_hex=" + req.hex())
    print("golden_response_hex=" + res.hex())
    print("request_bytes=%d response_bytes=%d" % (len(req), len(res)))
