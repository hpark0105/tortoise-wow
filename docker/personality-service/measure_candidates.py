"""PORT-021 (KAP-558): candidate model measurement harness.

Operator tool, run OUTSIDE any park-head session (shared-model
concurrency: exactly one local inference model at a time). For each
candidate GGUF it starts the shared llama-server on the standard port,
runs the same bounded schema workload the real planner uses (a fixed
party request through build_messages -> chat -> parse_model_text),
and records: file name, sha256 digest, context size, latency
distribution, peak VRAM, schema-valid rate and fallback behavior.
Availability is never approval: selection requires this evidence.

Usage (PowerShell, park-llama checkout next to this repo):
  python docker/personality-service/measure_candidates.py --out local/model-measure.json
  python ... --models "qwen3-4b-instruct-2507-q8_0.gguf" --rounds 10
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fake_planner as fp
import model_client as mc
import real_planner as rp

REPO_ROOT = os.path.abspath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", ".."))
PARK_ROOT = os.path.abspath(os.path.join(REPO_ROOT, "..", "park-llama"))

DEFAULT_MODELS = [
    "qwen3-4b-instruct-2507-q8_0.gguf",
    "Qwen3.8-4B-Q8_0.gguf",
    "Ministral-3-8B-Instruct-2512-Q8_0.gguf",
]


def die(msg):
    print("MEASURE-ABORT: %s" % msg, flush=True)
    sys.exit(2)


def port_free(port):
    import socket
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        try:
            s.bind(("127.0.0.1", port))
            return True
        except OSError:
            return False


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def key_file():
    override = os.environ.get("PARK_LLAMA_API_KEY_FILE", "")
    if override:
        return override
    local = os.environ.get("LOCALAPPDATA", "")
    return os.path.join(local, "park-llama", "llama-api.key")


def start_server(exe, model_path, port, rounds):
    cmd = [exe, "-m", model_path, "-ngl", "99", "-c", "8192",
           "-fa", "on", "--cache-type-k", "q8_0", "--cache-type-v", "q8_0",
           "--port", str(port), "--api-key-file", key_file(),
           "--cors-origins", "localhost", "--no-cors-credentials",
           "--temp", "0.0", "--top-p", "0.95", "--jinja",
           "--no-mmap"]
    print("MEASURE: starting %s" % " ".join(
        c if len(c) < 60 else c[:57] + "..." for c in cmd), flush=True)
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    return proc


def wait_ready(base_url, key, deadline_s):
    end = time.monotonic() + deadline_s
    last = ""
    while time.monotonic() < end:
        try:
            with urllib.request.urlopen(base_url + "/models", timeout=3) as r:
                doc = json.loads(r.read().decode())
            if doc.get("data"):
                return doc["data"][0]["id"]
        except (urllib.error.URLError, OSError, ValueError) as e:
            last = str(e)
        time.sleep(2)
    die("server not ready within %d s (last: %s)" % (deadline_s, last))


def stop_server(proc):
    if proc.poll() is None:
        try:
            subprocess.run(["taskkill", "/PID", str(proc.pid), "/T", "/F"],
                           capture_output=True, timeout=15)
        except Exception:
            proc.kill()
    try:
        proc.wait(timeout=15)
    except Exception:
        pass


class VramSampler(threading.Thread):
    def __init__(self):
        super().__init__(daemon=True)
        self.peak = None
        self.available = shutil.which("nvidia-smi") is not None
        self._proc = None
        self._stop = threading.Event()

    def run(self):
        if not self.available:
            return
        self._proc = subprocess.Popen(
            ["nvidia-smi", "--query-gpu=memory.used",
             "--format=csv,noheader,nounits", "-l", "1"],
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            text=True)
        for line in self._proc.stdout:
            if self._stop.is_set():
                break
            m = re.search(r"(\d+)", line)
            if m:
                v = int(m.group(1))
                if self.peak is None or v > self.peak:
                    self.peak = v

    def stop(self):
        self._stop.set()
        if self._proc is not None:
            try:
                subprocess.run(["taskkill", "/PID", str(self._proc.pid),
                                "/T", "/F"], capture_output=True, timeout=10)
            except Exception:
                pass
            self._proc.wait(timeout=10)
        self.join(timeout=5)


def synthetic_request():
    return fp.make_request({
        "protocol_version": fp.PROTOCOL_VERSION,
        "request_id": 0x4D45415355524530,
        "owner_guid": 610001,
        "observation_version": fp.OBSERVATION_VERSION,
        "capture_time_ms": 1000000,
        "step_count": 0,
        "total_size": fp.REQUEST_BYTES,
        "generation": 7,
        "flags": 0b010,
        "bots": [(610002, 1), (610003, 8)],
    })


def measure_model(model_path, port, rounds, primer):
    base_url = "http://127.0.0.1:%d/v1" % port
    key = mc.read_api_key(key_file())
    exe = os.path.join(PARK_ROOT, "bin", "llama-server.exe")
    if not os.path.exists(exe):
        die("llama-server.exe not found at %s" % exe)
    rec = {
        "file": os.path.basename(model_path),
        "sha256": sha256_file(model_path),
        "size_bytes": os.path.getsize(model_path),
        "port": port,
        "rounds": rounds,
    }
    if not port_free(port):
        die("port %d is busy: stop the current model (including any "
            "park-head session) before measuring" % port)
    sampler = VramSampler()
    sampler.start()
    rec["vram_available"] = sampler.available
    proc = start_server(exe, model_path, port, rounds)
    try:
        rec["model_id"] = wait_ready(base_url, key, 300)
        # Warmup: first two calls absorb graph capture / cache effects.
        kwargs = {"chat_template_kwargs":
                  {"enable_thinking": False}}
        for _ in range(2):
            try:
                mc.chat(base_url, key, rec["model_id"],
                        rp.build_messages(synthetic_request(), primer[0]),
                        timeout_s=60.0, max_tokens=64, extra=kwargs)
            except mc.ModelError as e:
                die("warmup failed: %s %s" % (e.reason, e.detail))
        latencies = []
        prompt_tokens = 0
        completion_tokens = 0
        valid = 0
        outcomes = []
        for i in range(rounds):
            t0 = time.monotonic()
            try:
                text, meta = mc.chat(base_url, key, rec["model_id"],
                                     rp.build_messages(
                                         synthetic_request(), primer[0]),
                                     timeout_s=60.0, max_tokens=64,
                                     extra=kwargs)
                steps = rp.parse_model_text(text, 2)
                ok = steps is not None
                valid += 1 if ok else 0
                outcomes.append("valid" if ok else "fallback")
                prompt_tokens += meta["prompt_tokens"]
                completion_tokens += meta["completion_tokens"]
                latencies.append(meta["latency_ms"])
                print("MEASURE: round %d %s latency_ms:%d" % (
                    i + 1, "valid" if ok else "FALLBACK",
                    meta["latency_ms"]), flush=True)
            except mc.ModelError as e:
                latencies.append(int((time.monotonic() - t0) * 1000))
                outcomes.append("error:%s" % e.reason)
                print("MEASURE: round %d ERROR %s" % (i + 1, e.reason),
                      flush=True)
        latencies.sort()
        rec.update({
            "latency_ms": {
                "min": latencies[0],
                "p50": latencies[len(latencies) // 2],
                "p95": latencies[max(0, int(len(latencies) * 0.95) - 1)],
                "max": latencies[-1],
            },
            "context_prompt_tokens_avg": prompt_tokens // max(1, rounds),
            "context_completion_tokens_avg": completion_tokens // max(1, rounds),
            "schema_valid_rate": round(valid / max(1, rounds), 3),
            "outcomes": outcomes,
        })
    finally:
        stop_server(proc)
        sampler.stop()
        rec["vram_peak_mib"] = sampler.peak
    # Under the transport's 5 s round deadline: a p95 above ~4500 ms
    # means real-world rounds will time out (deterministic fallback).
    rec["fits_5s_round_deadline"] = (
        rec["latency_ms"]["p95"] < 4500 if rec.get("latency_ms") else False)
    return rec


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--models", nargs="*", default=DEFAULT_MODELS)
    ap.add_argument("--rounds", type=int, default=10)
    ap.add_argument("--port", type=int, default=8090)
    ap.add_argument("--out", default="")
    args = ap.parse_args()
    primer = rp.load_primer()
    records = []
    for name in args.models:
        path = os.path.join(PARK_ROOT, "models", name)
        if not os.path.exists(path):
            die("model not found: %s" % path)
        rec = measure_model(path, args.port, args.rounds, primer)
        records.append(rec)
        print("MEASURE: %s valid:%s p50:%dms" % (
            rec["file"], rec["schema_valid_rate"],
            rec["latency_ms"]["p50"]), flush=True)
    ts = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
    out = args.out or os.path.join(REPO_ROOT, "local",
                                   "model-measure-%s.json" % ts)
    with open(out, "w", encoding="utf-8") as f:
        json.dump({"measured_at_utc": ts,
                   "primer": {"file": rp.PRIMER_NAME, "sha256": primer[1]},
                   "models": records}, f, indent=2)
    print("MEASURE: written %s" % out, flush=True)


if __name__ == "__main__":
    main()
