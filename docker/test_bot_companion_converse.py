"""PORT-022 (KAP-558): bounded conversational party chat - runtime proof.

One disposable Compose project, one world generation, one host-side fake
planner service (which also serves /converse), and a deterministic
follow-script that drives the .botpartymsg lab command. The fake adapter
selects its failure class from a keyword in the message text (a lab-only
convention), so one world generation covers every response class with no
racy scenario switching:

P1 greeting      addressed message -> one bounded reply (profile flows)
P2 random q      a second reply for a different question (distinct input)
P3 unaddressed   first token is not the bot -> no reply
P4 slow          CNVSLOW: 5 s adapter (past the 4 s round deadline) -> no reply
P5 malformed     CNVMALFORM: adapter rejects (HTTP 500) -> no reply
P6 empty         CNVEMPTY: adapter answers empty (HTTP 200) -> no reply
P7 leave         CNVSLOW in flight, party disbands -> reply dropped

Only P1 and P2 ever Say a reply, so the world log carries exactly two
reply lines for the whole run. The final follow-script chat event proving
the world reached the last timestamp shows the world thread never waited on
the (up to 5 s) model rounds, which all ran on the transport worker.

An optional real-model round runs when PORT022_MODEL_URL is set: the fake
is swapped for the real adapter on the same port and one /converse probe is
recorded to the evidence card (the world-through-real-model round is the
operator's in-game verification step).
"""
import json
import os
import socket
import subprocess
import sys
import time
import unittest
import urllib.request
import uuid
from datetime import datetime, timezone

import test_bot_provision as p

ROOT = os.path.dirname(os.path.abspath(__file__))
SERVER = os.path.join(ROOT, "personality-service", "fake_planner_server.py")
REAL_SERVER = os.path.join(ROOT, "personality-service", "real_planner_server.py")

OWNER_GUID = 500150
COMP_GUID = 500151
OWNER_ACC = 1000500150
COMP_ACC = 1000500151
PERSONAL_CONTAINERS = ("tortoise-local-db-1", "tortoise-local-realmd-1",
                       "tortoise-local-world-1")
COMP_NAME = "Cnvcomp"

SCRIPT = ";".join([
    "3000:%d:botfollow %s" % (OWNER_GUID, COMP_NAME),
    "8000:%d:botrecruit %s" % (OWNER_GUID, COMP_NAME),
    "15000:%d:botpartymsg %s hi there friend" % (OWNER_GUID, COMP_NAME),       # P1
    "40000:%d:botpartymsg %s how is the weather out" % (OWNER_GUID, COMP_NAME),  # P2
    "65000:%d:botpartymsg somebody hello" % OWNER_GUID,                        # P3
    "90000:%d:botpartymsg %s CNVSLOW take your time" % (OWNER_GUID, COMP_NAME), # P4
    "120000:%d:botpartymsg %s CNVMALFORM something broken" % (OWNER_GUID, COMP_NAME),  # P5
    "150000:%d:botpartymsg %s CNVEMPTY nothing here" % (OWNER_GUID, COMP_NAME),  # P6
    "180000:%d:botpartymsg %s CNVSLOW im about to leave" % (OWNER_GUID, COMP_NAME),  # P7
    "181500:%d:botdismiss %s" % (OWNER_GUID, COMP_NAME),                          # P7 leave
])


def _seed_sql():
    return """
INSERT INTO tw_char.characters
 (guid,account,name,race,class,gender,level,money,position_x,position_y,position_z,map,
  orientation,zone,health,power1,power2,power3,power4,power5,xp,xp_gain)
VALUES (%d,%d,'Cnvowner',1,1,0,10,100000,-8949.95,-120.493,83.5312,0,
 0,12,100,0,0,0,0,0,0,1),
 (%d,%d,'Cnvcomp',1,1,0,10,100000,-8949.95,-152.493,83.5312,0,
 0,12,100,0,0,0,0,0,0,1);
INSERT INTO tw_char.playerbot (char_guid,chance,ai)
 VALUES (%d,100,'Default'),(%d,100,'Default');
INSERT INTO tw_char.bot_ownership
 (char_guid,account_id,bot_type,provision_version,owner_account_id)
 VALUES (%d,%d,1,2,NULL),
        (%d,%d,1,2,%d);
DELETE FROM tw_world.creature WHERE map=0
 AND position_x BETWEEN -8990 AND -8910 AND position_y BETWEEN -170 AND -110;
""" % (OWNER_GUID, OWNER_ACC, COMP_GUID, COMP_ACC,
       OWNER_GUID, COMP_GUID, OWNER_GUID, OWNER_ACC,
       COMP_GUID, COMP_ACC, OWNER_ACC)


def _free_port(lo, hi):
    for port in range(lo, hi + 1):
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            try:
                s.bind(("127.0.0.1", port))
                return port
            except OSError:
                continue
    raise RuntimeError("no free port in %d..%d" % (lo, hi))


class BotCompanionConverseTests(unittest.TestCase):
    @staticmethod
    def _personal_state():
        try:
            out = p.command(["docker", "inspect", "--format",
                             "{{.Name}}|{{.State.StartedAt}}|{{.State.Running}}",
                             *PERSONAL_CONTAINERS], timeout=60)
        except RuntimeError:
            return {}
        state = {}
        for line in out.splitlines():
            parts = line.split("|")
            if len(parts) == 3:
                state[parts[0].lstrip("/")] = (parts[1], parts[2])
        return state

    def test_bounded_party_chat_rounds_and_failure_classes(self):
        p.IMAGE = os.environ.get("PORT022_LAB_IMAGE", "tortoise-local:dev")
        try:
            p.command(["docker", "image", "inspect", "--format", "{{.Id}}", p.IMAGE])
        except RuntimeError:
            self.skipTest("Build the candidate image first")
        before = self._personal_state()
        port = _free_port(18441, 18469)
        project = "tortoise-bot-converse-" + uuid.uuid4().hex[:12]
        evidence = p.ROOT / "local" / (project + "-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ"))
        evidence.mkdir(parents=True)
        adapter_log = evidence / "fake-converse.log"
        adapter_proc = None
        adapter_out = None
        base = env = None
        all_logs = []
        try:
            adapter_env = dict(os.environ)
            adapter_env.update({"FAKE_PLANNER_PORT": str(port)})
            adapter_out = open(adapter_log, "a", encoding="utf-8")
            adapter_proc = subprocess.Popen(
                [sys.executable, SERVER, str(port)],
                stdout=adapter_out, stderr=subprocess.STDOUT,
                env=adapter_env)

            world = p.world_env_for("")
            world.update(
                PLAYERBOT_MIN_BOTS="0", PLAYERBOT_MAX_BOTS="0",
                PLAYERBOT_REFRESH="600000", PLAYERBOT_UPDATE_MS="1000",
                PLAYERBOT_DEBUG="1",
                PLAYERBOT_TEST_LOGIN=str(OWNER_GUID) + "," + str(COMP_GUID),
                PLAYERBOT_QUEST_ID="0", PLAYERBOT_FOLLOW_SCRIPT=SCRIPT,
                PLAYERBOT_PERSONALITY_PROFILE="reckless",
                PLAYERBOT_PLANNER_SERVICE_URL="",
                PLAYERBOT_CONVERSATION_SERVICE_URL="http://hostbridge:%d" % port,
                PLAYER_SAVE_INTERVAL="5000")
            base, env = p.boot_lab(project, evidence, world, _seed_sql())
            compose_path = evidence / "compose.json"
            cfg = json.loads(compose_path.read_text(encoding="utf-8"))
            cfg["services"]["world"]["extra_hosts"] = ["hostbridge:host-gateway"]
            compose_path.write_text(json.dumps(cfg, indent=2), encoding="utf-8")

            def adapter_text():
                return adapter_log.read_text(encoding="utf-8", errors="replace")

            def expect(pred, deadline, label=""):
                try:
                    logs = p.wait_for(base, env, pred, deadline=deadline)
                except RuntimeError:
                    try:
                        fail_logs = p.command(
                            ["docker", "compose"] + base + ["logs", "world"],
                            env=env, timeout=60)
                    except RuntimeError:
                        fail_logs = ""
                    all_logs.append(fail_logs)
                    (evidence / "world.log").write_text(
                        "\n".join(all_logs), encoding="utf-8")
                    raise RuntimeError("world did not reach: %s" % label)
                all_logs.append(logs)
                return logs

            def n_reply(logs):
                return logs.count("[Conversation] reply GUID:%d profile:reckless" % COMP_GUID)

            def n_fail(logs):
                return logs.count("[Conversation] fail bot:%d (no reply)" % COMP_GUID)

            try:
                p.command(["docker", "compose"] + base + ["up", "-d", "--no-deps", "world"], env=env)
                # The world booted and the conversation transport started
                # against the adapter.
                expect(lambda t: "[Conversation] transport started url:http://hostbridge:%d" % port in t,
                       deadline=400, label="transport started")

                # P1: one bounded reply for the greeting (profile flows).
                logs = expect(lambda t: n_reply(t) >= 1, deadline=60, label="P1 reply")
                self.assertIn("POST /converse profile:reckless", adapter_text())

                # P2: a second reply for a distinct question.
                logs = expect(lambda t: n_reply(t) >= 2, deadline=60, label="P2 reply")
                # The adapter saw two distinct inbound messages (different lengths).
                lens = [ln for ln in adapter_text().splitlines() if "POST /converse profile:reckless" in ln]
                self.assertGreaterEqual(len(lens), 2)
                self.assertNotEqual(lens[0], lens[1], "P1 and P2 must be distinct messages")

                # P3: unaddressed (first token is not the bot) -> no reply.
                logs = expect(lambda t: ("conversation unaddressed issuer:%d first:somebody" % OWNER_GUID) in t,
                              deadline=60, label="P3 unaddressed")
                self.assertEqual(n_reply(logs), 2)

                # P4: slow adapter (past the round deadline) -> no reply, one fail.
                logs = expect(lambda t: n_fail(t) >= 1, deadline=60, label="P4 slow fail")
                self.assertEqual(n_reply(logs), 2)

                # P5: malformed (adapter rejects) -> no reply, one more fail.
                logs = expect(lambda t: n_fail(t) >= 2, deadline=60, label="P5 malformed fail")
                self.assertEqual(n_reply(logs), 2)

                # P6: empty (HTTP 200, no body) -> no reply, one more fail.
                logs = expect(lambda t: n_fail(t) >= 3, deadline=60, label="P6 empty fail")
                self.assertEqual(n_reply(logs), 2)

                # P7: slow round in flight while the party disbands -> the
                # reply is dropped; the world still dispatches the dismiss on
                # its own clock (the worker, not the world, ran the 5 s round).
                logs = expect(lambda t: ("[PlayerBot][FollowScript] chat at 181500 issuer:%d text:botdismiss %s" % (OWNER_GUID, COMP_NAME)) in t,
                              deadline=90, label="P7 dismiss dispatched")
                time.sleep(6)  # let the in-flight slow round time out / drop
                logs = p.command(["docker", "compose"] + base + ["logs", "--no-color", "world"], env=env, timeout=60)
                all_logs.append(logs)
                self.assertEqual(n_reply(logs), 2, "the leave must drop the in-flight reply")

                (evidence / "world.log").write_text("\n".join(all_logs), encoding="utf-8")
                self.assertNotIn("[CRASH]", "\n".join(all_logs))

                # Optional real-model round (host-side probe on the same port).
                live_url = os.environ.get("PORT022_MODEL_URL", "").strip()
                live = None
                if live_url:
                    live = self._real_probe(evidence, port, live_url, adapter_log)
            finally:
                if adapter_proc is not None and adapter_proc.poll() is None:
                    adapter_proc.kill()
                if adapter_out is not None:
                    adapter_out.close()
                if not (evidence / "world.log").exists() and all_logs:
                    (evidence / "world.log").write_text("\n".join(all_logs), encoding="utf-8")
        finally:
            if base is not None:
                try:
                    p.teardown_lab(base, env, project, evidence)
                except Exception:
                    p.force_down(base, env)
            if adapter_proc is not None and adapter_proc.poll() is None:
                adapter_proc.kill()
            if adapter_out is not None:
                adapter_out.close()
        self.assertEqual(before, self._personal_state())

    def _real_probe(self, evidence, port, live_url, adapter_log):
        """Swap the fake for the real adapter on the same port and drive one
        /converse round against the real local model. Recorded, not gated:
        the world-through-real-model round is the operator's in-game step."""
        live = {"url": live_url, "outcome": None, "model": None}
        real_proc = None
        real_out = None
        try:
            # Reuse the operator-local park-llama key file (host path, never
            # logged); fall back to the hermetic fixture key if unset.
            key = os.environ.get("PARK_LLAMA_API_KEY_FILE")
            if not key:
                key = os.path.join(os.environ["LOCALAPPDATA"], "park-llama", "llama-api.key")
            real_env = dict(os.environ)
            real_env.update(
                REAL_PLANNER_PORT=str(port),
                REAL_PLANNER_MODEL_URL=live_url,
                REAL_PLANNER_KEY_FILE=key,
                REAL_PLANNER_TIMEOUT_MS="25000",
                REAL_PLANNER_TICK_MS="1000",
                REAL_PLANNER_MAX_TOKENS="128")
            real_out = open(adapter_log, "a", encoding="utf-8")
            real_proc = subprocess.Popen(
                [sys.executable, REAL_SERVER],
                stdout=real_out, stderr=subprocess.STDOUT, env=real_env)
            up = time.monotonic() + 30
            while time.monotonic() < up:
                try:
                    with urllib.request.urlopen("http://127.0.0.1:%d/health" % port, timeout=2) as r:
                        if r.status == 200:
                            break
                except Exception:
                    time.sleep(0.3)
            req = urllib.request.Request(
                "http://127.0.0.1:%d/converse?profile=reckless" % port,
                data=b"Cnvcomp hello there", method="POST")
            with urllib.request.urlopen(req, timeout=40) as r:
                body = r.read().decode("utf-8", "replace")
                live["outcome"] = "status:%d reply:%r" % (r.status, body)
                live["reached"] = r.status == 200 and len(body) > 0
        except Exception as exc:
            live["outcome"] = "probe failed: %s" % exc
        finally:
            if real_proc is not None and real_proc.poll() is None:
                real_proc.kill()
            if real_out is not None:
                real_out.close()
        (evidence / "real-converse-probe.json").write_text(
            json.dumps(live, indent=2), encoding="utf-8")
        return live


if __name__ == "__main__":
    unittest.main()