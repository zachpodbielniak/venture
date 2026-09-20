#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Local OCR and capture review (#123) against a supplied DEBUG server.

Usage: ocr-demonstration.py <path-to-venture> <scratch-root>

Builds nothing. Starts the supplied binary three times against one private
state directory and one SQLite database: with OCR off, with OCR explicitly
enabled but its binary absent, and with OCR enabled against a fake engine
script that emits deterministic text -- the same technique tests/test-ocr.c
uses (a shell script that answers --version and records its argv). Image and
PDF fixtures are generated with python cairo exactly as tests/test-ocr.c
generates them with the C cairo API; the driver refuses to run without it
rather than inventing a different fixture.

No Tesseract, no language pack and no network are used or required.
"""
import hashlib
import http.cookiejar
import json
import os
import pathlib
import re
import secrets
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid

try:
    import cairo
except ImportError:  # explicit refusal, never a silent skip
    raise SystemExit("python cairo is required to generate the PNG/PDF fixtures "
                     "that tests/test-ocr.c generates with the C cairo API")

FAILURES = []
STEP = [0]


def step(title):
    STEP[0] += 1
    print("\n--- %d. %s" % (STEP[0], title), flush=True)


def check(condition, message):
    print(("    OK   " if condition else "    FAIL ") + message, flush=True)
    if not condition:
        FAILURES.append("%d. %s" % (STEP[0], message))


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

    def raw(self, path, values=None, method=None, form=False, content_type=None, quiet=False):
        if isinstance(values, bytes):
            body = values
        elif values is None:
            body = None
        elif form:
            body = urllib.parse.urlencode(values).encode()
        else:
            body = json.dumps(values).encode()
        shown = "" if values is None else (
            " <%d bytes>" % len(body) if isinstance(values, bytes) else " " + json.dumps(values))
        if not quiet:
            print("    %s %s%s" % (method or ("POST" if body is not None else "GET"), path, shown), flush=True)
        request = urllib.request.Request(self.origin + path, body, method=method)
        if body is not None:
            request.add_header("Content-Type", content_type or (
                "application/x-www-form-urlencoded" if form else "application/json"))
            request.add_header("Origin", self.origin)
        try:
            response = self.opener.open(request, timeout=60)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            status, content = response.status, response.read(32 * 1024 * 1024)
        if not quiet:
            print("    HTTP %d" % status, flush=True)
        try:
            parsed = json.loads(content) if content else None
        except ValueError:
            parsed = None
        if isinstance(parsed, dict):
            parsed = parsed.get("data", parsed)
        return status, parsed, content

    def ok(self, path, values=None, method=None, form=False, content_type=None, quiet=False):
        status, parsed, content = self.raw(path, values, method, form, content_type, quiet)
        assert 200 <= status < 300, "%s: HTTP %d: %s" % (path, status, content[:800].decode(errors="replace"))
        return parsed

    def refuse(self, path, values=None, expected=(400, 409, 422)):
        status, parsed, content = self.raw(path, values)
        message = (parsed or {}).get("message") if isinstance(parsed, dict) else None
        print("    message: " + json.dumps(message), flush=True)
        check(status in expected, "refused with HTTP %d (expected one of %s)" % (status, expected))
        return status, message

    def login(self, username, password):
        print("    POST /login username=%s password=<synthetic, not printed>" % username, flush=True)
        request = urllib.request.Request(
            self.origin + "/login",
            urllib.parse.urlencode({"username": username, "password": password}).encode())
        request.add_header("Content-Type", "application/x-www-form-urlencoded")
        request.add_header("Origin", self.origin)
        try:
            response = self.opener.open(request, timeout=30)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            status = response.status
            response.read(4096)
        print("    HTTP %d" % status, flush=True)
        assert any(cookie.name == "venture_session" for cookie in self.cookies), "login issued no session"

    def upload(self, filename, data, mime):
        boundary = uuid.uuid4().hex
        marker = ("--" + boundary).encode()
        body = b"".join([
            marker, b"\r\nContent-Disposition: form-data; name=\"organization_id\"\r\n\r\n1\r\n",
            marker, b"\r\nContent-Disposition: form-data; name=\"file\"; filename=\"",
            filename.encode(), b"\"\r\nContent-Type: ", mime.encode(), b"\r\n\r\n", data,
            b"\r\n", marker, b"--\r\n"])
        print("    POST /ui/chat/upload multipart filename=%s type=%s bytes=%d" % (filename, mime, len(data)), flush=True)
        status, parsed, content = self.raw(
            "/ui/chat/upload", body,
            content_type="multipart/form-data; boundary=" + boundary, quiet=True)
        print("    HTTP %d %s" % (status, json.dumps(parsed)), flush=True)
        assert status == 201, content[:500]
        return parsed["id"]

    def record(self, type_name, identity):
        return self.ok("/api/v1/%s/%s" % (type_name, identity), quiet=True)

    def rows(self, type_name, **filters):
        query = urllib.parse.urlencode(dict({"organization_id": 1, "limit": 1000}, **filters))
        return self.ok("/api/v1/%s?%s" % (type_name, query), quiet=True)["records"]

    def action(self, type_name, identity, name, **values):
        return self.ok("/api/v1/%s/%s/actions/%s" % (type_name, identity, name), values)


class Server:
    def __init__(self, binary, root, extra):
        self.binary, self.root, self.extra = binary, root, extra
        self.process = None

    def config(self):
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            self.port = listener.getsockname()[1]
        self.origin = "http://127.0.0.1:%d" % self.port
        path = self.root / ("config-%d-%d.yaml" % (STEP[0], self.port))
        path.write_text(
            "server:\n  bind_address: 127.0.0.1\n  port: %d\n"
            "security:\n  require_auth: true\n"
            "database:\n  uri: %s\n"
            "modules:\n  finance:\n    enabled: true\n  capture:\n    enabled: true\n%s"
            % (self.port, json.dumps("sqlite://" + str(self.root / "workspace.db")), self.extra))
        path.chmod(0o600)
        return path

    def popen(self):
        path = self.config()
        print("    config: " + json.dumps(path.read_text()), flush=True)
        environment = dict((k, v) for k, v in os.environ.items() if not k.startswith("VENTURE_"))
        environment.update(VENTURE_SESSION_SECRET=secrets.token_hex(32),
                           XDG_CONFIG_HOME=str(self.root / "config-home"),
                           XDG_DATA_HOME=str(self.root / "data-home"))
        self.log = (self.root / "server.log").open("ab")
        self.process = subprocess.Popen(
            ["nice", "-n", "19", str(self.binary), "--config", str(path),
             "--state-dir", str(self.root / "state"), "--no-plugins", "--no-automation"],
            cwd=str(self.binary.parents[2]), env=environment,
            stdout=self.log, stderr=subprocess.STDOUT)
        return self.process

    def start(self, password=None):
        self.popen()
        client = Client(self.origin)
        deadline = time.monotonic() + 120
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise RuntimeError("Server exited during startup; see " + str(self.root / "server.log"))
            try:
                client.ok("/api/v1/health", quiet=True)
                break
            except (OSError, ValueError):
                time.sleep(0.1)
        else:
            raise RuntimeError("Server startup exceeded 120 seconds")
        print("    GET /api/v1/health -> HTTP 200 on " + self.origin, flush=True)
        if password is None:
            password = re.findall(r"^\s*Password:\s*(\S+)\s*$",
                                  (self.root / "server.log").read_text(errors="replace"), re.M)[0]
        client.login("owner", password)
        self.client, self.password = client, password
        return client

    def stop(self):
        if self.process is not None and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=15)
        if getattr(self, "log", None):
            self.log.close()


def write_png(path, width, height, text=None):
    surface = cairo.ImageSurface(cairo.FORMAT_RGB24, width, height)
    if text is not None:
        context = cairo.Context(surface)
        context.set_source_rgb(1, 1, 1)
        context.paint()
        context.set_source_rgb(0, 0, 0)
        context.set_font_size(12)
        context.move_to(4, 24)
        context.show_text(text)
    surface.write_to_png(str(path))
    return path.read_bytes()


def write_mixed_pdf(path):
    """Page 1 carries a real text layer; page 2 is a scanned-style raster."""
    surface = cairo.PDFSurface(str(path), 200, 100)
    context = cairo.Context(surface)
    context.move_to(10, 30)
    context.show_text("Native invoice 100")
    context.show_page()
    context.set_source_rgb(0, 0, 0)
    context.rectangle(10, 10, 20, 20)
    context.fill()
    context.show_page()
    surface.finish()
    return path.read_bytes()


def write_pages_pdf(path, pages):
    surface = cairo.PDFSurface(str(path), 50, 50)
    context = cairo.Context(surface)
    for _ in range(pages):
        context.show_page()
    surface.finish()
    return path.read_bytes()


def attachment_of(root, filename):
    directory = root / "state" / "attachments"
    matches = [p for p in directory.iterdir() if p.name.endswith("_" + filename)]
    assert len(matches) == 1, (filename, [p.name for p in matches])
    return matches[0]


def job_line(job):
    return ("    job id=%s state=%s pages=%s completed_pages=%s attempts=%s language=%s "
            "engine=%s source_hash=%s extracted_at=%s reviewed=%s error=%s" % (
                job["id"], job["state"], job["pages"], job["completed_pages"], job["attempts"],
                json.dumps(job["language"]), json.dumps(job["engine"]), json.dumps(job["source_hash"]),
                json.dumps(job["extracted_at"]), job["reviewed"], json.dumps(job["error"])))


def show(job):
    print(job_line(job), flush=True)
    print("    job text: " + json.dumps(job["text"]), flush=True)
    return job


def batch_line(batch):
    return ("    batch id=%s state=%s cursor=%s total=%s failed=%s current_job_id=%s "
            "capture_ids=%s error=%s" % (
                batch["id"], batch["state"], batch["cursor"], batch["total"], batch["failed"],
                batch["current_job_id"], batch["capture_ids"], json.dumps(batch["error"])))


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    binary = pathlib.Path(sys.argv[1]).resolve()
    root = pathlib.Path(sys.argv[2]).resolve()
    root.mkdir(parents=True, exist_ok=True)
    root.chmod(0o700)
    fixtures = root / "fixtures"
    fixtures.mkdir(exist_ok=True)

    print("Artifact: " + str(binary), flush=True)
    print("Artifact SHA-256: " + hashlib.sha256(binary.read_bytes()).hexdigest(), flush=True)
    print("Scratch root: " + str(root), flush=True)
    print("python cairo: " + cairo.version, flush=True)

    # The fake engine: answers --version with a stable provenance string,
    # records its real argv, fails on demand, and otherwise emits fixed text.
    engine = root / "fake-tesseract"
    arguments = root / "engine-argv"
    failure_switch = root / "engine-should-fail"
    engine.write_text(
        "#!/bin/sh\n"
        "if [ \"$1\" = --version ]; then printf 'fixture OCR 1\\n'; exit 0; fi\n"
        "printf '%%s\\n' \"$@\" > %s\n"
        "if [ -f %s ]; then printf 'fixture engine refused\\n' >&2; exit 3; fi\n"
        "printf 'SYNTHETIC RECEIPT TOTAL 12.50 USD\\n'\n"
        % (str(arguments), str(failure_switch)))
    engine.chmod(0o700)
    print("Fake engine script: " + str(engine), flush=True)
    print(json.dumps(engine.read_text()), flush=True)

    receipt = write_png(fixtures / "receipt.png", 40, 40, "RECEIPT")
    mixed = write_mixed_pdf(fixtures / "mixed.pdf")

    disabled = Server(binary, root, "")
    missing = Server(binary, root,
                     "ocr:\n  enabled: true\n  executable: /nonexistent/venture-demo-tesseract\n  language: eng\n")
    enabled = Server(binary, root,
                     "ocr:\n  enabled: true\n  executable: %s\n  language: eng\n" % json.dumps(str(engine)))

    try:
        # ---------------------------------------------------------------- A
        step("OCR disabled (default): ordinary capture and native document extraction continue")
        client = disabled.start()
        password = disabled.password
        document = client.upload("receipt.png", receipt, "image/png")
        record = client.record("document", document)
        check(record["mime_type"] == "image/png" and not record["extracted_text"],
              "uploaded original is stored as an ordinary document (id=%s, mime=%s, extracted_text=%s)"
              % (document, record["mime_type"], json.dumps(record["extracted_text"])))
        note = client.upload("supplier-note.txt",
                             b"Supplier note: native text layer, no OCR involved\n", "text/plain")
        native = client.record("document", note)
        check((native["extracted_text"] or "").startswith("Supplier note"),
              "native (non-OCR) document text extraction still runs: " + json.dumps(native["extracted_text"]))
        item = client.ok("/api/v1/capture", {"kind": "receipt", "title": "Ordinary capture with OCR off",
                                             "source": "upload", "document_id": document,
                                             "vendor": "Synthetic vendor", "amount": "12.50 USD"})
        check(item["status"] == "inbox",
              "capture inbox ingest works with OCR off (capture_item id=%s status=%s)" % (item["id"], item["status"]))
        expense = client.ok("/api/v1/capture/%s/convert" % item["id"], {"as": "expense"})
        check(expense["type"] == "expense",
              "the ordinary capture -> expense conversion path is untouched (expense id=%s amount=%s)"
              % (expense["id"], json.dumps(expense["amount"])))

        step("OCR disabled: ocr_extract reports a configuration error, and OCR records are hidden")
        status, message = client.refuse("/api/v1/document/%s/actions/ocr_extract" % document, {"language": "eng"})
        check(message == "module: The OCR module is disabled",
              "ocr_extract names the disabled module rather than succeeding through some other path")
        status, parsed, _ = client.raw("/api/v1/ocr_job?organization_id=1")
        print("    message: " + json.dumps(parsed.get("message")), flush=True)
        check(status == 404 and "modules.ocr.enabled" in (parsed.get("message") or ""),
              "ocr_job records are hidden and the answer names the switch to turn on")
        baseline = dict((name, len(client.rows(name))) for name in ("expense", "ledger_entry"))
        print("    financial row counts after ordinary capture: " + json.dumps(baseline), flush=True)
        disabled.stop()

        # ---------------------------------------------------------------- B
        step("OCR explicitly enabled without its binary: a clear configuration error, no silent success")
        process = missing.popen()
        code = process.wait(timeout=120)
        missing.log.close()
        log = (root / "server.log").read_text(errors="replace")
        tail = [line for line in log.splitlines() if "OCR" in line][-3:]
        print("    exit code: %d" % code, flush=True)
        for line in tail:
            print("    log: " + line, flush=True)
        check(code != 0, "the server refuses to start rather than running with a broken OCR configuration")
        check(any("OCR enabled but Tesseract is not installed or executable" in line for line in tail),
              "the refusal names the missing binary")
        try:
            urllib.request.urlopen(missing.origin + "/api/v1/health", timeout=2).close()
            listening = True
        except Exception:
            listening = False
        check(not listening, "nothing is listening on the refused server's port")

        # ---------------------------------------------------------------- C
        step("OCR enabled with the fake engine: extraction with provenance on an image")
        client = enabled.start(password)
        job = show(client.action("document", document, "ocr_extract", language="eng"))
        check(job["state"] == "pending" and job["pages"] == 1, "queued job starts pending with a known page count")
        check(re.match(r"^[0-9a-f]{64}$", job["source_hash"] or "") is not None,
              "provenance: source hash is a SHA-256 of the original bytes")
        check(job["source_hash"] == hashlib.sha256(receipt).hexdigest(),
              "provenance: the stored hash equals SHA-256 of the fixture we uploaded")
        check((job["engine"] or "").startswith("fixture OCR 1"),
              "provenance: engine/version recorded from the engine itself")
        check(job["language"] == "eng", "provenance: language recorded")
        first = job["id"]
        job = show(client.action("ocr_job", first, "step"))
        check(job["state"] == "succeeded" and job["completed_pages"] == 1, "one page completed; state succeeded")
        check(job["extracted_at"] is not None, "provenance: extraction time recorded")
        check("SYNTHETIC RECEIPT TOTAL 12.50 USD" in (job["text"] or ""),
              "the fake engine's deterministic text was captured")
        print("    engine argv: " + json.dumps(arguments.read_text()), flush=True)
        check("\nstdout\n-l\neng\n" in arguments.read_text(),
              "the engine was executed with an argument vector, not a shell string")
        record = client.record("document", document)
        print("    document extracted_text: " + json.dumps(record["extracted_text"]), flush=True)
        check("SYNTHETIC RECEIPT TOTAL 12.50 USD" in (record["extracted_text"] or ""),
              "extracted text lands on the document for review")

        step("Repeated extraction skips an unchanged input unless it is forced")
        again = show(client.action("document", document, "ocr_extract", language="eng"))
        check(again["id"] == first, "unchanged source and language reuse the existing successful job (no re-run)")
        forced = show(client.action("document", document, "ocr_extract", language="eng", force=True))
        check(forced["id"] != first, "force=true creates new output instead of reusing it")
        forced = show(client.action("ocr_job", forced["id"], "step"))
        check(forced["state"] == "succeeded", "the forced job completes")

        step("A reviewed correction is never silently overwritten")
        reviewed = show(client.action("document", document, "ocr_review",
                                      job_id=forced["id"], text="Reviewed receipt total 12.60 USD"))
        check(reviewed["reviewed"] is True, "the job is marked explicitly reviewed")
        record = client.record("document", document)
        print("    document extracted_text: " + json.dumps(record["extracted_text"]), flush=True)
        check(record["extracted_text"] == "Reviewed receipt total 12.60 USD", "the correction is on the document")
        after = show(client.action("document", document, "ocr_extract", language="eng", force=True))
        after = show(client.action("ocr_job", after["id"], "step"))
        record = client.record("document", document)
        print("    document extracted_text: " + json.dumps(record["extracted_text"]), flush=True)
        check(record["extracted_text"] == "Reviewed receipt total 12.60 USD",
              "a later forced extraction does not overwrite the reviewed correction")
        check("SYNTHETIC RECEIPT TOTAL 12.50 USD" in (after["text"] or "") and after["reviewed"] is False,
              "the new output is retained on its job, waiting for explicit review")

        step("A mixed PDF: the existing text layer is used as-is, the scanned page is rasterised")
        pdf_document = client.upload("mixed.pdf", mixed, "application/pdf")
        if arguments.exists():
            arguments.unlink()
        job = show(client.action("document", pdf_document, "ocr_extract", language="eng"))
        check(job["pages"] == 2, "both PDF pages are counted before any work is done")
        job = show(client.action("ocr_job", job["id"], "step"))
        check(job["state"] == "running" and job["completed_pages"] == 1,
              "progress: page 1 of 2 committed, job still running")
        check(not arguments.exists(),
              "page 1 has a text layer, so the OCR engine was never executed for it (no duplicate extraction)")
        check("Native invoice 100" in (job["text"] or ""), "page 1 contributed its native text layer")
        job = show(client.action("ocr_job", job["id"], "step"))
        check(job["state"] == "succeeded" and job["completed_pages"] == 2, "page 2 completes the job")
        check(arguments.exists(), "the scanned page 2 was rasterised and sent to the engine")
        if arguments.exists():
            print("    engine argv: " + json.dumps(arguments.read_text()), flush=True)
        text = job["text"] or ""
        check("Native invoice 100" in text and "SYNTHETIC RECEIPT TOTAL 12.50 USD" in text,
              "the document text carries the native page and the OCR'd page")
        check(text.count("Native invoice 100") == 1, "the native text layer is not appended twice")

        step("Per-document failure, retry and cancel")
        retry_document = client.upload("retry.png", write_png(fixtures / "retry.png", 40, 40, "RETRY"), "image/png")
        failure_switch.write_text("fail\n")
        job = show(client.action("document", retry_document, "ocr_extract", language="eng"))
        check(job["state"] == "pending", "queued pending")
        job = show(client.action("ocr_job", job["id"], "step"))
        check(job["state"] == "failed" and bool(job["error"]),
              "an engine failure is persisted as a failed job with a useful error")
        status, message = client.refuse("/api/v1/ocr_job/%s/actions/step" % job["id"], {})
        check(message == "Retry a failed or cancelled job before stepping it",
              "a failed job must be retried explicitly before it steps again")
        job = show(client.action("ocr_job", job["id"], "retry"))
        check(job["state"] == "pending" and job["error"] is None,
              "retry returns the job to pending and clears the error")
        job = show(client.action("ocr_job", job["id"], "cancel"))
        check(job["state"] == "cancelled", "cancel takes effect between page steps")
        job = show(client.action("ocr_job", job["id"], "retry"))
        failure_switch.unlink()
        job = show(client.action("ocr_job", job["id"], "step"))
        check(job["state"] == "succeeded", "after the engine recovers, the retried job succeeds")

        step("A batch where one bad document fails without losing the rest")
        good_one = client.upload("batch-good-1.png", write_png(fixtures / "g1.png", 40, 40, "ONE"), "image/png")
        bad = client.upload("batch-bad.png", write_png(fixtures / "bad.png", 40, 40, "BAD"), "image/png")
        good_two = client.upload("batch-good-2.png", write_png(fixtures / "g2.png", 40, 40, "TWO"), "image/png")
        cursor = max(row["id"] for row in client.rows("capture_item"))
        for title, identity in (("Batch good one", good_one), ("Batch bad", bad), ("Batch good two", good_two)):
            created = client.ok("/api/v1/capture", {"kind": "receipt", "title": title, "source": "upload",
                                                    "document_id": identity, "amount": "1.00 USD"})
            print("    capture_item id=%s title=%s status=%s" % (created["id"], json.dumps(title), created["status"]),
                  flush=True)
        queued = [row for row in client.rows("ocr_job") if row["document_id"] in (good_one, bad, good_two)]
        for row in queued:
            print(job_line(row), flush=True)
        check(len(queued) == 3 and all(row["state"] == "pending" for row in queued),
              "filing a capture row queues a pending extraction job per document")
        missing_path = attachment_of(root, "batch-bad.png")
        missing_path.unlink()
        print("    removed the middle document's original: " + str(missing_path), flush=True)
        batch = client.action("capture_item", 0, "ocr_extract_all", organization_id=1, after_id=cursor, limit=25)
        print(batch_line(batch), flush=True)
        check(batch["total"] == 3 and batch["state"] == "pending", "the inbox selection is frozen at 3 rows")
        for _ in range(3):
            batch = client.action("ocr_batch", batch["id"], "step")
            print(batch_line(batch), flush=True)
        check(batch["state"] == "succeeded" and batch["cursor"] == 3 and batch["failed"] == 1,
              "the batch completed: 3 documents attempted, exactly 1 failed")
        for identity, label in ((good_one, "first"), (good_two, "third")):
            record = client.record("document", identity)
            print("    %s document extracted_text: %s" % (label, json.dumps(record["extracted_text"])), flush=True)
            check("SYNTHETIC RECEIPT TOTAL 12.50 USD" in (record["extracted_text"] or ""),
                  "the %s document still got its text; the bad one did not lose it" % label)
        bad_jobs = sorted([row for row in client.rows("ocr_job") if row["document_id"] == bad],
                          key=lambda row: row["id"])
        for row in bad_jobs:
            print(job_line(row), flush=True)
        check(bad_jobs[-1]["state"] == "failed" and bool(bad_jobs[-1]["error"]),
              "the bad document keeps its own failed job carrying its own error")
        check(bad_jobs[0]["state"] == "pending",
              "the pending job filing already queued for it is retained, not rewritten")

        step("A batch can be cancelled and resumed")
        batch = client.action("capture_item", 0, "ocr_extract_all", organization_id=1, after_id=cursor, limit=25)
        print(batch_line(batch), flush=True)
        batch = client.action("ocr_batch", batch["id"], "step")
        print(batch_line(batch), flush=True)
        batch = client.action("ocr_batch", batch["id"], "cancel")
        print(batch_line(batch), flush=True)
        check(batch["state"] == "cancelled", "the batch is cancelled mid-run")
        guard = 0
        while batch["state"] != "succeeded" and guard < 6:
            guard += 1
            batch = client.action("ocr_batch", batch["id"], "step")
            print(batch_line(batch), flush=True)
        check(batch["state"] == "succeeded" and batch["cursor"] == batch["total"],
              "stepping a cancelled batch resumes it from its durable cursor")

        step("Bounds are refused with useful errors")
        bounds = client.upload("bounds.png", write_png(fixtures / "bounds.png", 40, 40, "BOUNDS"), "image/png")
        path = attachment_of(root, "bounds.png")
        write_png(fixtures / "wide.png", 4097, 1)
        path.write_bytes((fixtures / "wide.png").read_bytes())
        print("    replaced the original with a 4097x1 PNG (raster bound is 4096 per dimension)", flush=True)
        job = show(client.action("document", bounds, "ocr_extract", language="eng"))
        check(job["state"] == "failed" and "raster bound" in (job["error"] or ""),
              "the oversized raster is refused with a named bound")
        path.write_bytes(b"x" * (20 * 1024 * 1024 + 1))
        print("    replaced the original with 20 MiB + 1 byte (file bound is 20 MiB)", flush=True)
        job = show(client.action("document", bounds, "ocr_extract", language="eng"))
        check(job["state"] == "failed" and "allowed size" in (job["error"] or ""),
              "the oversized original is refused with a named bound")
        pages_document = client.upload("pages.pdf", write_pages_pdf(fixtures / "one.pdf", 1), "application/pdf")
        pages_path = attachment_of(root, "pages.pdf")
        pages_path.write_bytes(write_pages_pdf(fixtures / "fifty-one.pdf", 51))
        print("    replaced the original with a 51-page PDF (page bound is 50)", flush=True)
        job = show(client.action("document", pages_document, "ocr_extract", language="eng"))
        check(job["state"] == "failed" and "at most 50 pages" in (job["error"] or ""),
              "the over-long PDF is refused with a named bound")
        client.refuse("/api/v1/document/%s/actions/ocr_extract" % document,
                      {"language": "eng; touch /tmp/venture-ocr-should-not-exist"})
        check(not pathlib.Path("/tmp/venture-ocr-should-not-exist").exists(),
              "a shell-looking language is refused and nothing was executed")
        status, message = client.refuse("/api/v1/capture_item/0/actions/ocr_extract_all",
                                        {"organization_id": 1, "limit": 26})
        check(status in (400, 409, 422), "a batch larger than the documented 25-row bound is refused")

        step("OCR posts no books")
        final = dict((name, len(client.rows(name))) for name in ("expense", "ledger_entry"))
        print("    financial row counts after all OCR work: " + json.dumps(final), flush=True)
        check(final == baseline, "no expense and no ledger entry was created by any OCR action")
        jobs = client.rows("ocr_job")
        states = sorted(set(row["state"] for row in jobs))
        print("    ocr_job states observed across %d durable jobs: %s" % (len(jobs), json.dumps(states)), flush=True)
        check({"succeeded", "failed"} <= set(states),
              "per-document job state is a durable record, including failures")
        enabled.stop()
    finally:
        for server in (disabled, missing, enabled):
            try:
                server.stop()
            except Exception:
                pass

    print("\nLIMITATIONS:", flush=True)
    for line in [
        "No real Tesseract, no language pack and no real scanned receipt: the engine is a shell",
        "  script that answers --version and prints fixed text, exactly as tests/test-ocr.c does.",
        "  Recognition quality is therefore not demonstrated; only the contract around the engine is.",
        "  (tests/test-ocr.c's /ocr/real-engine case covers a real binary when one is provisioned.)",
        "All documents, capture rows and money are synthetic, in one organization, on SQLite, with a",
        "  single owner; no PostgreSQL, no multi-tenant isolation and no concurrency are exercised.",
        "The vision/AI fallback path is not exercised: this shows that ordinary capture and native",
        "  document text extraction continue with OCR off and that ocr_extract refuses, not that an",
        "  AI provider answered. No provider is configured and no network call is made.",
        "The file-size, page-count and raster bounds are triggered by rewriting the stored original",
        "  on disk, which is how tests/test-ocr.c triggers them; the upload route has its own limit.",
        "Restart resumability, the 30 s process deadline, active cancellation of a running engine,",
        "  temporary-artifact cleanup and staged-approval staleness are covered by tests/test-ocr.c",
        "  and are not re-proved here.",
    ]:
        print("  " + line, flush=True)

    if FAILURES:
        print("\nFAILED CHECKS:", flush=True)
        for line in FAILURES:
            print("  " + line, flush=True)
        return 1
    print("\nAll checks passed.", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
