#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Call logging and follow-up (#122) against a supplied DEBUG venture binary.

Builds nothing. Starts the given artifact on an OS-chosen loopback port with a
private config, state directory and SQLite file, bootstraps the first-run owner,
then drives the documented log_call surfaces over HTTP and venturectl exactly as
an operator would. Every request, status and relevant response field is printed
verbatim; each expectation is asserted and any mismatch exits non-zero.

Usage: calls-demonstration.py BINARY SCRATCH_ROOT
"""
import hashlib
import http.cookiejar
import json
import os
from pathlib import Path
import re
import secrets
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

if not __debug__:
    raise RuntimeError("Run without Python optimization: assertions carry the demonstration")

STEP = [0]


def step(text):
    STEP[0] += 1
    print("\n[%02d] %s" % (STEP[0], text), flush=True)


def show(text):
    print("     " + text, flush=True)


class RefuseRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, response, code, message, headers, url):
        return None


class Client:
    def __init__(self, origin):
        self.origin = origin
        self.cookies = http.cookiejar.CookieJar()
        self.opener = urllib.request.build_opener(
            urllib.request.ProxyHandler({}),
            urllib.request.HTTPCookieProcessor(self.cookies),
            RefuseRedirect())

    def call(self, method, path, values=None, form=False, quiet=False):
        """Return (status, decoded body text). Never raises on HTTP status."""
        body = None
        if values is not None:
            body = urllib.parse.urlencode(values).encode() if form else json.dumps(values).encode()
        request = urllib.request.Request(self.origin + path, body, method=method)
        if body is not None:
            request.add_header("Content-Type",
                               "application/x-www-form-urlencoded" if form else "application/json")
            request.add_header("Origin", self.origin)
        try:
            response = self.opener.open(request, timeout=30)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            status, content = response.status, response.read(8 * 1024 * 1024)
        if not quiet:
            shown = "" if values is None else " " + json.dumps(values, sort_keys=True)
            show("%s %s%s" % (method, path, shown))
            show("  -> HTTP %d" % status)
        return status, content.decode("utf-8", errors="replace")

    def json(self, method, path, values=None, expect=(200, 201), quiet=False):
        status, text = self.call(method, path, values, quiet=quiet)
        assert status in expect, "%s %s: HTTP %d: %s" % (method, path, status, text[:800])
        if not text:
            return None
        result = json.loads(text)
        return result.get("data", result) if isinstance(result, dict) else result

    def get(self, path, **kwargs):
        return self.json("GET", path, **kwargs)

    def post(self, path, values, **kwargs):
        return self.json("POST", path, values, **kwargs)

    def refuse(self, method, path, values, expect, slug):
        status, text = self.call(method, path, values)
        payload = json.loads(text)
        show("  -> error=%r message=%r" % (payload["error"], payload["message"]))
        assert status == expect, "expected HTTP %d, got %d: %s" % (expect, status, text[:800])
        assert payload["error"] == slug, payload["error"]
        return payload

    def records(self, name, **filters):
        query = urllib.parse.urlencode({"organization_id": 1, "limit": 10000, **filters})
        return self.get("/api/v1/%s?%s" % (name, query), quiet=True)["records"]

    def page(self, path):
        status, text = self.call("GET", path)
        assert status == 200, "GET %s: HTTP %d" % (path, status)
        return text


def start(binary, root):
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        port = listener.getsockname()[1]
    origin = "http://127.0.0.1:%d" % port
    config = root / "config.yaml"
    config.write_text(
        "server:\n  bind_address: 127.0.0.1\n  port: " + str(port) +
        "\nsecurity:\n  require_auth: true\ndatabase:\n  uri: " +
        json.dumps("sqlite://" + str(root / "workspace.db")) + "\n")
    config.chmod(0o600)
    environment = {k: v for k, v in os.environ.items() if not k.startswith("VENTURE_")}
    environment.update(VENTURE_SESSION_SECRET=secrets.token_hex(32),
                       XDG_CONFIG_HOME=str(root / "config-home"),
                       XDG_DATA_HOME=str(root / "data-home"))
    log_path = root / "server.log"
    log = log_path.open("ab")
    process = subprocess.Popen(
        [str(binary), "--config", str(config), "--state-dir", str(root / "state"),
         "--no-ai", "--no-plugins", "--no-automation"],
        cwd=binary.parents[2], env=environment, stdout=log, stderr=subprocess.STDOUT)
    client = Client(origin)
    deadline = time.monotonic() + 90
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError("Server exited during startup; see " + str(log_path))
        try:
            status, _ = client.call("GET", "/api/v1/health", quiet=True)
        except OSError:
            status = 0
        if status == 200:
            break
        time.sleep(0.1)
    else:
        raise RuntimeError("Server did not answer /api/v1/health within 90s")
    return process, log, origin, client, log_path


# Occurrence times are in the past relative to the run; follow-ups are ahead of
# their call, as the service requires.
CALL_A = {
    "subject": "Renewal discovery call",
    "owner": "owner",
    "call_direction": "outbound",
    "call_duration": 900,
    "call_occurred_at": "2026-09-03T14:05:00Z",
    "call_outcome": "reached",
    "outcome": "Agreed to send a revised estimate before the renewal date.",
    "call_followup_due_at": "2026-09-25T15:00:00Z",
    "call_followup_owner": "owner",
}
CALL_B = {
    "subject": "Inbound support escalation",
    "call_direction": "inbound",
    "call_duration": 480,
    "call_occurred_at": "2026-09-08T09:30:00Z",
    "call_outcome": "voicemail",
    "outcome": "Left a voicemail confirming the escalation was received.",
    "call_external_source": "fixture-voice-v1",
    "call_external_id": "prov-call-8821",
}
CALL_C = {
    "subject": "Inbound enquiry from the website",
    "owner": "dana.reyes",
    "call_direction": "inbound",
    "call_duration": 300,
    "call_outcome": "callback",
    "call_occurred_at": "2026-09-10T11:00:00Z",
    "outcome": "Asked to be called back on Friday.",
}
SECRET_TOKEN = "s3cr3t-do-not-log"
TRANSCRIPT = "<script>alert('untrusted')</script> Caller asked for the annual price."
CALL_D = {
    "subject": "Pricing callback",
    "owner": "owner",
    "call_direction": "outbound",
    "call_duration": 60,
    "call_occurred_at": "2026-09-12T16:20:00Z",
    "call_outcome": "reached",
    "outcome": "Quoted the annual price and recorded the caller's transcript.",
    "call_recording_url": "https://recordings.example.invalid/calls/8821",
    "call_transcript": TRANSCRIPT,
}


def churn(client):
    report = client.get("/api/v1/reports/customer_churn?period=2026-09&organization_id=1", quiet=True)
    return next(m for m in report["metrics"] if m["key"] == "activity_churn")


def counts(client):
    activities = client.records("activity")
    interactions = client.records("interaction")
    return (len(activities),
            len([a for a in activities if a["kind"] == "call"]),
            len([a for a in activities if a["kind"] == "followup"]),
            len(interactions))


def run(binary, root, origin, client, log_path):
    step("Bootstrap: the first-run owner password is printed once into the private server log")
    password = re.findall(r"^\s*Password:\s*(\S+)\s*$", log_path.read_text(), re.M)
    assert password, "First-run owner password not found in the private server log"
    status, _ = client.call("GET", "/api/v1/company?organization_id=1")
    assert status == 401
    show("  -> unauthenticated record access is refused before login")
    show('POST /login {"username": "owner", "password": "<private, never printed>"}')
    status, _ = client.call("POST", "/login", {"username": "owner", "password": password[0]},
                            form=True, quiet=True)
    show("  -> HTTP %d" % status)
    assert status in (302, 303), status
    assert any(c.name == "venture_session" for c in client.cookies)
    show("  -> session cookie issued for owner")

    step("Create the CRM subjects: a company, its contact and a separate lead")
    company = client.post("/api/v1/company", {"organization_id": 1, "name": "Northwind Freight"})
    contact = client.post("/api/v1/contact", {"organization_id": 1, "name": "Priya Raman",
                                              "company_id": company["id"],
                                              "email": "priya.raman@example.invalid"})
    lead = client.post("/api/v1/lead", {"organization_id": 1, "name": "Website enquiry",
                                        "company_name": "Halden Logistics",
                                        "email": "enquiry@example.invalid"})
    show("  -> company id=%d contact id=%d lead id=%d" % (company["id"], contact["id"], lead["id"]))
    baseline_churn = churn(client)
    show("  -> baseline customer_churn activity_churn = %r" % baseline_churn["formatted"])
    before = counts(client)
    show("  -> rows before any call: activities=%d calls=%d followups=%d interactions=%d" % before)

    step("A negative duration is refused, and the refused request writes nothing")
    client.refuse("POST", "/api/v1/company/%d/actions/log_call" % company["id"],
                  dict(CALL_A, call_duration=-1), 422, "validation")
    assert counts(client) == before, "A refused log_call left rows behind"
    show("  -> rows unchanged: activities=%d calls=%d followups=%d interactions=%d" % before)

    step("Outbound call on a company: one action writes the completed call, its timeline "
         "history and one planned follow-up")
    call_a = client.post("/api/v1/company/%d/actions/log_call" % company["id"], CALL_A)
    for key in ("id", "kind", "status", "owner", "company_id", "call_direction", "call_duration",
                "call_occurred_at", "call_outcome", "outcome", "completed_at",
                "call_external_source", "call_external_id", "call_interaction_id",
                "call_followup_id"):
        show("  %s = %r" % (key, call_a[key]))
    assert call_a["kind"] == "call" and call_a["status"] == "done"
    assert call_a["call_direction"] == "outbound" and call_a["call_duration"] == 900
    assert call_a["call_outcome"] == "reached"
    assert call_a["outcome"] == CALL_A["outcome"], "the free-text outcome must survive beside the structured one"
    assert call_a["company_id"] == company["id"]
    assert call_a["call_external_source"] == "manual-v1", call_a["call_external_source"]
    assert call_a["call_interaction_id"] > 0 and call_a["call_followup_id"] > 0
    assert "call_external_key" not in call_a and "call_request_hash" not in call_a
    show("  -> call_external_key / call_request_hash are SENSITIVE and absent from the API record")

    history = client.get("/api/v1/interaction/%d" % call_a["call_interaction_id"])
    show("  interaction kind=%r outbound=%r company_id=%r occurred_at=%r subject=%r"
         % (history["kind"], history["outbound"], history["company_id"],
            history["occurred_at"], history["subject"]))
    assert history["kind"] == "call" and history["outbound"] is True
    assert history["company_id"] == company["id"]
    assert history["occurred_at"].startswith("2026-09-03T14:05:00")

    followup = client.get("/api/v1/activity/%d" % call_a["call_followup_id"])
    show("  followup kind=%r status=%r owner=%r due_at=%r remind_at=%r subject=%r"
         % (followup["kind"], followup["status"], followup["owner"], followup["due_at"],
            followup["remind_at"], followup["subject"]))
    assert followup["kind"] == "followup" and followup["status"] == "planned"
    assert followup["owner"] == "owner"
    assert followup["due_at"].startswith("2026-09-25T15:00:00")
    assert followup["remind_at"].startswith("2026-09-25T15:00:00"), "reminder defaults to due"
    assert followup["company_id"] == company["id"]
    assert not followup.get("call_transcript") and not followup.get("call_recording_url")

    after_a = counts(client)
    show("  -> rows now: activities=%d calls=%d followups=%d interactions=%d" % after_a)
    assert after_a == (before[0] + 2, before[1] + 1, before[2] + 1, before[3] + 1)

    step("Replay: the identical request derives the same manual-v1 identity and creates "
         "no second call, interaction or follow-up")
    replay = client.post("/api/v1/company/%d/actions/log_call" % company["id"], CALL_A)
    show("  -> returned activity id=%d (original %d), call_interaction_id=%d, call_followup_id=%d"
         % (replay["id"], call_a["id"], replay["call_interaction_id"], replay["call_followup_id"]))
    assert replay["id"] == call_a["id"]
    assert replay["call_interaction_id"] == call_a["call_interaction_id"]
    assert replay["call_followup_id"] == call_a["call_followup_id"]
    assert counts(client) == after_a, "the replay created rows"
    show("  -> rows unchanged: activities=%d calls=%d followups=%d interactions=%d" % after_a)

    step("Inbound call completing a planned call activity on a contact, under an explicit "
         "external identity")
    planned = client.post("/api/v1/activity", {"organization_id": 1, "kind": "call",
                                               "subject": "Planned check-in",
                                               "owner": "owner",
                                               "contact_id": contact["id"],
                                               "company_id": company["id"],
                                               "due_at": "2026-09-08T09:00:00Z"})
    show("  planned activity id=%d status=%r kind=%r owner=%r"
         % (planned["id"], planned["status"], planned["kind"], planned["owner"]))
    assert planned["status"] == "planned"
    call_b = client.post("/api/v1/activity/%d/actions/log_call" % planned["id"], CALL_B)
    for key in ("id", "status", "owner", "contact_id", "call_direction", "call_duration",
                "call_occurred_at", "call_outcome", "due_at", "completed_at",
                "call_external_source", "call_external_id", "call_interaction_id",
                "call_followup_id"):
        show("  %s = %r" % (key, call_b[key]))
    assert call_b["id"] == planned["id"], "the planned activity itself is completed"
    assert call_b["status"] == "done" and call_b["call_direction"] == "inbound"
    assert call_b["owner"] == "owner", "owner defaults to the existing activity owner"
    assert call_b["call_occurred_at"].startswith("2026-09-08T09:30:00")
    assert call_b["due_at"].startswith("2026-09-08T09:00:00"), "planned dates stay separate from occurrence"
    assert call_b["call_external_source"] == "fixture-voice-v1"
    assert call_b["call_external_id"] == "prov-call-8821"
    assert call_b["call_followup_id"] == 0, "no follow-up was requested for this call"
    history_b = client.get("/api/v1/interaction/%d" % call_b["call_interaction_id"])
    show("  interaction kind=%r outbound=%r contact_id=%r occurred_at=%r"
         % (history_b["kind"], history_b["outbound"], history_b["contact_id"],
            history_b["occurred_at"]))
    assert history_b["outbound"] is False and history_b["contact_id"] == contact["id"]
    after_b = counts(client)
    show("  -> rows now: activities=%d calls=%d followups=%d interactions=%d" % after_b)
    replay_b = client.post("/api/v1/activity/%d/actions/log_call" % planned["id"], CALL_B)
    show("  -> provider replay returned activity id=%d (original %d), interaction %d"
         % (replay_b["id"], call_b["id"], replay_b["call_interaction_id"]))
    assert replay_b["id"] == call_b["id"]
    assert replay_b["call_interaction_id"] == call_b["call_interaction_id"]
    assert counts(client) == after_b, "the provider replay created rows"
    show("  -> rows unchanged: activities=%d calls=%d followups=%d interactions=%d" % after_b)

    step("Reusing that provider identity with different content is refused as a conflict")
    client.refuse("POST", "/api/v1/activity/%d/actions/log_call" % planned["id"],
                  dict(CALL_B, outcome="Rewritten history", call_duration=30),
                  409, "conflict")
    assert counts(client) == after_b
    show("  -> rows unchanged: activities=%d calls=%d followups=%d interactions=%d" % after_b)

    step("Lead-only call through venturectl, with no invented contact")
    session = root / "cli-session.json"
    cookie = next(c for c in client.cookies if c.name == "venture_session")
    session.write_text(json.dumps({"origin": origin, "cookie": cookie.name + "=" + cookie.value}))
    session.chmod(0o600)
    arguments = [str(binary.with_name("venturectl")), "--server", origin,
                 "--session-file", str(session), "act", "lead", str(lead["id"]), "log_call"] + \
                ["%s=%s" % (k, v) for k, v in CALL_C.items()]
    show("$ venturectl --server " + origin + " --session-file <private> act lead " +
         str(lead["id"]) + " log_call " + " ".join("%s=%s" % (k, v) for k, v in CALL_C.items()))
    completed = subprocess.run(arguments, capture_output=True, text=True, timeout=60,
                               cwd=binary.parents[2])
    show("  -> exit %d" % completed.returncode)
    assert completed.returncode == 0, completed.stdout + completed.stderr
    call_c = next(a for a in client.records("activity")
                  if a["kind"] == "call" and a["lead_id"] == lead["id"])
    for key in ("id", "owner", "lead_id", "contact_id", "company_id", "call_direction",
                "call_outcome", "call_external_source", "call_interaction_id"):
        show("  %s = %r" % (key, call_c[key]))
    assert call_c["lead_id"] == lead["id"]
    assert call_c["contact_id"] == 0 and call_c["company_id"] == 0
    assert call_c["owner"] == "dana.reyes"
    assert call_c["call_external_source"] == "manual-v1"
    timeline = client.get("/api/v1/activity/lead/%d" % lead["id"])
    for event in timeline:
        show("  lead timeline event: " + json.dumps(event, sort_keys=True))
    calls_in_timeline = [e for e in timeline if e["kind"] == "call"]
    assert len(calls_in_timeline) == 1, timeline
    assert calls_in_timeline[0]["activity_id"] == call_c["id"]
    assert calls_in_timeline[0]["actor"] == "dana.reyes"

    after_c = counts(client)
    show("  -> rows now: activities=%d calls=%d followups=%d interactions=%d" % after_c)

    step("Recording addresses: credential-bearing URLs are refused and the refusal repeats "
         "no submitted credential")
    for url in ("https://svc:%s@recordings.example.invalid/calls/8821" % SECRET_TOKEN,
                "https://recordings.example.invalid/calls/8821?token=%s" % SECRET_TOKEN,
                "https://recordings.example.invalid/calls/8821#%s" % SECRET_TOKEN):
        show("  submitted call_recording_url = %r" % url)
        payload = client.refuse("POST", "/api/v1/company/%d/actions/log_call" % company["id"],
                                dict(CALL_D, call_recording_url=url), 422, "validation")
        assert SECRET_TOKEN not in json.dumps(payload), "the refusal echoed the credential"
        show("  -> the refusal body contains no part of the submitted credential")
    assert counts(client) == after_c, "refusals must not have created rows"
    show("  -> rows unchanged: activities=%d calls=%d followups=%d interactions=%d" % after_c)

    step("A stable HTTPS recording address and a supplied transcript are stored, and the "
         "transcript is escaped where it is rendered")
    call_d = client.post("/api/v1/company/%d/actions/log_call" % company["id"], CALL_D)
    show("  call_recording_url = %r" % call_d["call_recording_url"])
    show("  call_transcript = %r" % call_d["call_transcript"])
    assert call_d["call_recording_url"] == CALL_D["call_recording_url"]
    assert call_d["call_transcript"] == TRANSCRIPT
    page = client.page("/e/activity/%d" % call_d["id"])
    assert "&lt;script&gt;alert(" in page, "the transcript was not escaped"
    assert "<script>alert('untrusted')" not in page, "the transcript rendered as live markup"
    show("  -> /e/activity/%d renders the transcript escaped, not as markup" % call_d["id"])
    where = page.index("&lt;script&gt;")
    show("  page excerpt: ..." + page[where - 60:where + 130].replace("\n", " ") + "...")
    company_timeline = client.get("/api/v1/activity/company/%d" % company["id"])
    rendered = json.dumps(company_timeline)
    assert CALL_D["call_recording_url"] not in rendered
    assert "<script>" not in rendered
    assert "Caller asked for the annual price" not in rendered
    show("  -> the company timeline summaries carry the title and narrative outcome only: "
         "no recording address and no transcript")
    for event in [e for e in company_timeline if e["kind"] == "call"]:
        show("  company timeline event: " + json.dumps(event, sort_keys=True))

    step("The calls report counts each call once, by the owner frozen at logging, over the "
         "actual occurrence period")
    final = counts(client)
    show("  rows in the organization: activities=%d calls=%d followups=%d interactions=%d" % final)
    report = client.get("/api/v1/reports/calls?period=2026-09&organization_id=1")
    show("  report title: " + report["title"])
    show("  metrics: " + json.dumps([{m["key"]: m["value"]} for m in report["metrics"]]))
    for row in report["rows"]:
        show("  row: " + json.dumps({k: v for k, v in row.items()
                                     if not k.endswith("_formatted")}, sort_keys=True))
    total = next(m for m in report["metrics"] if m["key"] == "calls")["value"]
    assert total == 4, total
    assert final[1] == 4 and final[2] == 1 and final[3] == 4
    rows = {row["owner"]: row for row in report["rows"]}
    assert set(rows) == {"owner", "dana.reyes"}, rows
    assert rows["owner"]["calls"] == 3 and rows["owner"]["inbound"] == 1 and rows["owner"]["outbound"] == 2
    assert rows["owner"]["seconds"] == 900 + 480 + 60
    assert rows["owner"]["reached"] == 2 and rows["owner"]["voicemail"] == 1
    assert rows["dana.reyes"]["calls"] == 1 and rows["dana.reyes"]["inbound"] == 1
    assert rows["dana.reyes"]["callback"] == 1 and rows["dana.reyes"]["seconds"] == 300
    show("  -> 4 call activities, 4 linked interactions and 1 follow-up produce 4 counted "
         "calls, not 9")
    empty = client.get("/api/v1/reports/calls?period=2026-08&organization_id=1")
    show("  August period metrics: " +
         json.dumps([{m["key"]: m["value"]} for m in empty["metrics"]]))
    assert next(m for m in empty["metrics"] if m["key"] == "calls")["value"] == 0
    show("  -> the period is taken from the actual occurrence, inclusive start, exclusive end")

    step("The activity_churn headline is a cash-receipt churn metric and logging calls does "
         "not move it")
    final_churn = churn(client)
    show("  activity_churn before the calls: %r (value %r)"
         % (baseline_churn["formatted"], baseline_churn["value"]))
    show("  activity_churn after  the calls: %r (value %r)"
         % (final_churn["formatted"], final_churn["value"]))
    assert final_churn["value"] == baseline_churn["value"]
    assert final_churn["formatted"] == baseline_churn["formatted"]
    for kind in ("payment", "payment_allocation", "refund", "invoice"):
        rows = client.records(kind)
        show("  %s rows in the organization after four logged calls: %d" % (kind, len(rows)))
        assert len(rows) == 0, rows
    show("  -> logging a call created no cash receipt, so the accounting definition of "
         "activity_churn is untouched")
    show("  -> this fixture has no paying customer, so activity_churn has no denominator "
         "and reads n/a on both sides; the calls report is the call-activity evidence")

    step("Call evidence is service-owned: generic CRUD cannot manufacture or rewrite it")
    client.refuse("PATCH", "/api/v1/activity/%d" % call_a["id"],
                  {"organization_id": 1, "call_duration": 5, "version": call_a["version"]},
                  422, "validation")
    client.refuse("POST", "/api/v1/activity",
                  {"organization_id": 1, "kind": "call", "subject": "Manufactured",
                   "status": "done", "call_direction": "inbound", "call_duration": 10,
                   "call_occurred_at": "2026-09-02T10:00:00Z", "call_outcome": "reached"},
                  422, "validation")
    assert counts(client) == final
    show("  -> rows unchanged: activities=%d calls=%d followups=%d interactions=%d" % final)

    print("\nALL EXPECTATIONS MET", flush=True)
    print("""
LIMITATIONS: this demonstration proves only what the transcript above shows.
  - No telephony, voice provider, webhook or recording download exists or was
    contacted; the external identity seam is exercised with a synthetic
    "fixture-voice-v1" namespace, and no recording address was ever fetched.
  - All companies, contacts, leads, calls, transcripts and credentials are
    synthetic and were created inside a private throwaway SQLite database that
    is deleted with the scratch root.
  - One organization, one signed-in owner. Cross-organization isolation,
    read-only role refusal, staged confirmation/approval and the reminder sweep
    are covered by test-activities, not by this transcript.
  - SQLite only. PostgreSQL and migration 000490 are covered by the suite when
    VENTURE_TEST_ACCOUNTING_POSTGRES_URI / VENTURE_TEST_MIGRATION_POSTGRES_URI
    are set; neither ran here.
  - activity_churn read n/a both before and after, because this fixture has no
    paying customer and therefore no customer-months denominator. What is shown
    is that four logged calls created no payment, allocation, refund or invoice
    row, so the cash-receipt definition of the metric is untouched. The
    arithmetic of activity_churn on a populated ledger is demonstrated by the
    headline reports, not here.
  - Transaction atomicity is shown from the outside only: refused requests left
    no activity, interaction or follow-up rows. The internal rollback after a
    partial write is covered by test-activities' call-followup-rollback case.
""", flush=True)


def main():
    binary = Path(sys.argv[1]).resolve()
    root = Path(sys.argv[2]).resolve()
    root.mkdir(parents=True, exist_ok=True)
    root.chmod(0o700)
    print("Artifact: " + str(binary), flush=True)
    print("Artifact SHA-256: " + hashlib.sha256(binary.read_bytes()).hexdigest(), flush=True)
    process, log, origin, client, log_path = start(binary, root)
    try:
        run(binary, root, origin, client, log_path)
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=10)
        log.close()


if __name__ == "__main__":
    main()
