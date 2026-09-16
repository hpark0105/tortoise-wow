"""PORT-021 (KAP-558): bounded OpenAI-compatible chat client for the
park-llama local model service.

Stdlib only (urllib). No SDK, no retry, no streaming: one request per
planner round with a hard timeout. The API key is read from the file
the park-llama launcher owns (default:
%LOCALAPPDATA%\\park-llama\\llama-api.key, override
PARK_LLAMA_API_KEY_FILE) and is never logged or returned.
"""
import json
import os
import socket
import time
import urllib.error
import urllib.request

DEFAULT_BASE_URL = "http://127.0.0.1:8090/v1"
KEY_FILE_NAME = "llama-api.key"


class ModelError(Exception):
    """Bounded failure reason; the adapter maps every reason to the
    deterministic Hold fallback. detail carries no key material."""

    def __init__(self, reason, detail=""):
        super().__init__(reason)
        self.reason = reason  # no-key-file|connect-fail|timeout|http-error|bad-json|no-model
        self.detail = detail


def default_key_file():
    override = os.environ.get("PARK_LLAMA_API_KEY_FILE", "")
    if override:
        return override
    local = os.environ.get("LOCALAPPDATA", "")
    if not local:
        raise ModelError("no-key-file", "LOCALAPPDATA unavailable")
    return os.path.join(local, "park-llama", KEY_FILE_NAME)


def read_api_key(key_file=None):
    path = key_file or default_key_file()
    try:
        with open(path, "r", encoding="utf-8") as f:
            key = f.read().strip()
    except OSError as e:
        raise ModelError("no-key-file",
                         "cannot read key file (%s)" % e.errno)
    if not key:
        raise ModelError("no-key-file", "empty key file")
    return key


def _authed_get(url, key, timeout_s):
    req = urllib.request.Request(url, method="GET")
    req.add_header("Authorization", "Bearer " + key)
    try:
        with urllib.request.urlopen(req, timeout=timeout_s) as r:
            return r.read()
    except urllib.error.HTTPError as e:
        raise ModelError("http-error", "status %d" % e.code)
    except (socket.timeout, TimeoutError):
        raise ModelError("timeout", "models probe timed out")
    except (urllib.error.URLError, OSError) as e:
        raise ModelError("connect-fail", str(getattr(e, "reason", e)))


def fetch_model_id(base_url, key, timeout_s=5.0):
    """The single loaded model id (llama-server reports the file path)."""
    raw = _authed_get(base_url.rstrip("/") + "/models", key, timeout_s)
    try:
        doc = json.loads(raw.decode("utf-8"))
    except ValueError:
        raise ModelError("bad-json", "non-JSON /v1/models")
    for entry in doc.get("data", []):
        mid = entry.get("id", "")
        if mid:
            return mid
    raise ModelError("no-model", "empty /v1/models")


def chat(base_url, key, model, messages, timeout_s, max_tokens=64,
         temperature=0.0, extra=None):
    """One bounded completion. Returns (text, meta) or raises ModelError.
    meta: latency_ms, prompt_tokens, completion_tokens (0 when absent).
    extra: optional provider-specific payload keys (e.g.
    chat_template_kwargs for thinking-mode templates); ignored by
    servers that do not know them."""
    payload = {
        "model": model,
        "messages": messages,
        "max_tokens": int(max_tokens),
        "temperature": temperature,
        "stream": False,
    }
    if extra:
        payload.update(extra)
    body = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(base_url.rstrip("/") + "/chat/completions",
                                 data=body, method="POST")
    req.add_header("Content-Type", "application/json")
    req.add_header("Authorization", "Bearer " + key)
    start = time.monotonic()
    try:
        with urllib.request.urlopen(req, timeout=timeout_s) as r:
            raw = r.read()
    except urllib.error.HTTPError as e:
        raise ModelError("http-error", "status %d" % e.code)
    except (socket.timeout, TimeoutError):
        raise ModelError("timeout", "chat timed out after %d ms"
                         % int(timeout_s * 1000))
    except (urllib.error.URLError, OSError) as e:
        raise ModelError("connect-fail", str(getattr(e, "reason", e)))
    latency_ms = int((time.monotonic() - start) * 1000)
    try:
        doc = json.loads(raw.decode("utf-8"))
    except ValueError:
        raise ModelError("bad-json", "non-JSON chat response")
    try:
        text = doc["choices"][0]["message"]["content"]
    except (KeyError, IndexError, TypeError):
        raise ModelError("bad-json", "no choices[0].message.content")
    usage = doc.get("usage") or {}
    return text, {
        "latency_ms": latency_ms,
        "prompt_tokens": int(usage.get("prompt_tokens", 0) or 0),
        "completion_tokens": int(usage.get("completion_tokens", 0) or 0),
    }
