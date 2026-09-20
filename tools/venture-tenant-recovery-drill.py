#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Synthetic, destructive-to-own-fixtures-only physical recovery qualification.

Requires a prebuilt DEBUG Venture image and a PostgreSQL 18 image. The fixture
owns newly named containers/networks/volumes, removes only those on exit and
retains a private evidence directory. Never point it at an existing tenant.
"""

import argparse
import base64
import hashlib
import http.cookiejar
import json
import os
import pathlib
import secrets
import socket
import subprocess
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--image", required=True, help="Existing DEBUG application image")
parser.add_argument(
    "--application-revision",
    required=True,
    help="Source revision built into that image",
)
parser.add_argument(
    "--venture-binary",
    default="/usr/bin/venture",
    help="Application path inside the image",
)
parser.add_argument("--postgres-image", default="docker.io/library/postgres:18.4")
args = parser.parse_args()
root = pathlib.Path(tempfile.mkdtemp(prefix="venture-tenant-recovery-drill-"))
os.chmod(root, 0o700)
image = args.image
tool = str(pathlib.Path(__file__).resolve().with_name("venture-tenant-backup"))
owned = []
networks = []
volumes = []


def run(args, data=None, timeout=180):
    p = subprocess.run(
        args,
        input=data,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=timeout,
    )
    if p.returncode:
        raise RuntimeError(
            "Command failed: "
            + args[0]
            + " "
            + args[1]
            + "; "
            + p.stderr.decode()[-1600:]
        )
    return p.stdout


image_id = (
    run(["podman", "image", "inspect", image, "--format", "{{.Id}}"]).decode().strip()
)
if (
    run(["podman", "info", "--format", "{{.Host.Security.Rootless}}"]).strip()
    != b"true"
):
    raise RuntimeError("This fixture requires rootless Podman")
if not image_id.startswith("sha256:"):
    image_id = "sha256:" + image_id


def private(path, text):
    path.write_text(text)
    os.chmod(path, 0o600)


def host(name):
    p = root / name
    p.mkdir(mode=0o700)
    private(p / "host.json", json.dumps({"schema": 1, "host_id": str(uuid.uuid4())}))
    return p


source = host("source")
target = host("target")


def provision(parent, name, start=True, workspace_id=None):
    host_id = json.loads((parent / "host.json").read_text())["host_id"]
    d = parent / name
    d.mkdir(mode=0o700)
    for part in ("control", "state", "config"):
        (d / part).mkdir(mode=0o700)
    suffix = secrets.token_hex(4)
    network = "v125-recovery-" + suffix
    networks.append(network)
    run(["podman", "network", "create", network])
    labels = [
        "--label",
        "io.venture.host=" + host_id,
        "--label",
        "io.venture.tenant=" + name,
    ]
    dbpw = secrets.token_hex(24)
    adminpw = secrets.token_hex(24)
    private(d / "control" / "postgres.env", "POSTGRES_PASSWORD=" + adminpw + "\n")
    volume = "v125-recovery-data-" + suffix
    volumes.append(volume)
    run(["podman", "volume", "create", volume])
    db = (
        run(
            [
                "podman",
                "run",
                "-d",
                "--name",
                "v125-recovery-db-" + suffix,
                *labels,
                "--label",
                "io.venture.role=database",
                "--network",
                network,
                "--network-alias",
                "database",
                "--env-file",
                str(d / "control" / "postgres.env"),
                "--volume",
                volume + ":/var/lib/postgresql",
                "--memory",
                "384m",
                "--cpus",
                "1",
                args.postgres_image,
                "-c",
                "max_connections=30",
                "-c",
                "shared_buffers=32MB",
            ]
        )
        .decode()
        .strip()
    )
    owned.append(db)
    for _ in range(100):
        p = subprocess.run(
            [
                "podman",
                "exec",
                "--user",
                "postgres",
                db,
                "pg_isready",
                "-h",
                "127.0.0.1",
                "-U",
                "postgres",
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        if p.returncode == 0:
            break
        time.sleep(0.2)
    else:
        raise RuntimeError("PostgreSQL readiness timeout")
    run(
        [
            "podman",
            "exec",
            "-i",
            "--user",
            "postgres",
            db,
            "psql",
            "-X",
            "--set",
            "ON_ERROR_STOP=1",
            "-U",
            "postgres",
            "-d",
            "postgres",
        ],
        (
            "CREATE ROLE venture LOGIN PASSWORD '"
            + dbpw
            + "' NOSUPERUSER NOCREATEDB NOCREATEROLE NOREPLICATION NOBYPASSRLS;\nCREATE DATABASE venture OWNER venture;\n"
        ).encode(),
    )
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
    private(
        d / "control" / "runtime.env",
        "VENTURE_DATABASE_URI=postgresql://venture@database/venture\nVENTURE_DB_PASSWORD="
        + dbpw
        + "\nVENTURE_STATE_DIR=/var/lib/venture\nVENTURE_SERVER_BIND_ADDRESS=0.0.0.0\nVENTURE_SERVER_PORT=8747\n",
    )
    if start:
        private(
            d / "config" / "identity.env",
            "VENTURE_INTEGRATION_KEY="
            + base64.b64encode(secrets.token_bytes(32)).decode()
            + "\nVENTURE_SESSION_SECRET="
            + secrets.token_hex(32)
            + "\n",
        )
        private(d / "config" / "config.yaml", "bankfeed:\n  enabled: true\n")
    # A recovery target has empty application config until restore. Runtime DB
    # transport credentials remain in control and are never copied from source.
    app_args = [
        "podman",
        "create",
        "--name",
        "v125-recovery-app-" + suffix,
        *labels,
        "--label",
        "io.venture.role=application",
        "--network",
        network,
        "--user",
        "0:0",
        "--cap-drop",
        "ALL",
        "--security-opt",
        "no-new-privileges",
        "--read-only",
        "--tmpfs",
        "/tmp:rw,nosuid,size=128m",
        "--memory",
        "512m",
        "--cpus",
        "1",
        "--env-file",
        str(d / "control" / "runtime.env"),
        "--volume",
        str(d / "state") + ":/var/lib/venture:Z",
        "--volume",
        str(d / "config") + ":/etc/venture:ro,Z",
        "--publish",
        "127.0.0.1:" + str(port) + ":8747",
        "--entrypoint",
        "/bin/sh",
        image,
        "-c",
        'set -a; . /etc/venture/identity.env; set +a; exec "$1" --config /etc/venture/config.yaml --state-dir /var/lib/venture --no-ai --no-plugins --no-automation --owner-password Synthetic-Recovery-Owner-125',
        "venture-recovery-bootstrap",
        args.venture_binary,
    ]
    app = run(app_args).decode().strip()
    owned.append(app)
    manifest = {
        "schema": 1,
        "host_id": host_id,
        "tenant_id": name,
        "workspace_id": workspace_id or str(uuid.uuid4()),
        "app_container": app,
        "db_container": db,
        "app_image": image_id,
        "database": "venture",
        "db_role": "venture",
        "postgres_major": 18,
    }
    private(d / "manifest.json", json.dumps(manifest))
    obj = {
        "dir": d,
        "app": app,
        "db": db,
        "url": "http://127.0.0.1:" + str(port),
        "port": port,
        "workspace_id": manifest["workspace_id"],
        "jar": http.cookiejar.CookieJar(),
    }
    obj["http"] = urllib.request.build_opener(
        urllib.request.HTTPCookieProcessor(obj["jar"])
    )
    if start:
        start_app(obj)
    return obj


def request(obj, path, values=None, form=False):
    data = (
        None
        if values is None
        else (
            urllib.parse.urlencode(values).encode()
            if form
            else json.dumps(values).encode()
        )
    )
    req = urllib.request.Request(
        obj["url"] + path,
        data=data,
        headers={
            "Content-Type": (
                "application/x-www-form-urlencoded" if form else "application/json"
            )
        },
    )
    try:
        with obj["http"].open(req, timeout=15) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()


def start_app(obj):
    run(["podman", "start", obj["app"]])
    for _ in range(1500):
        try:
            with urllib.request.urlopen(obj["url"] + "/api/v1/health", timeout=2) as r:
                if r.status == 200:
                    return
        except (OSError, urllib.error.URLError):
            pass
        time.sleep(0.2)
    logs = subprocess.run(
        ["podman", "logs", obj["app"]], capture_output=True, text=True
    ).stderr
    (obj["dir"] / "control" / "startup.log").write_text(logs)
    raise RuntimeError(
        "Venture startup failed; see private startup.log in " + str(obj["dir"])
    )


def login(obj):
    status, body = request(
        obj,
        "/login",
        {"username": "owner", "password": "Synthetic-Recovery-Owner-125"},
        True,
    )
    assert status == 200, (status, body[:300])


def create(obj, kind, values):
    status, body = request(obj, "/api/v1/" + kind, {"organization_id": 1, **values})
    assert status == 201, (kind, status, body[:600])
    v = json.loads(body)
    return v.get("data", v)


def sql(obj, text):
    return run(
        [
            "podman",
            "exec",
            "-i",
            "--user",
            "postgres",
            obj["db"],
            "psql",
            "-X",
            "-qAt",
            "--set",
            "ON_ERROR_STOP=1",
            "-U",
            "postgres",
            "-d",
            "venture",
        ],
        text.encode(),
    )


def fingerprint(obj):
    tables = (
        sql(
            obj,
            "SELECT tablename FROM pg_tables WHERE schemaname='public' ORDER BY tablename;\n",
        )
        .decode()
        .splitlines()
    )
    counts = {}
    digest = hashlib.sha256()
    statements = []
    for table in tables:
        statements.append(
            "SELECT 'TABLE:"
            + table
            + "'; SELECT row_to_json(t)::text FROM public.\""
            + table
            + '" t ORDER BY row_to_json(t)::text;'
        )
    rows = sql(obj, "\n".join(statements) + "\n")
    current = None
    for row in rows.splitlines(keepends=True):
        if row.startswith(b"TABLE:"):
            current = row[6:].decode().strip()
            counts[current] = 0
            digest.update(current.encode() + b"\0")
        else:
            counts[current] += 1
            digest.update(row)
    return counts, digest.hexdigest()


def backup_cmd(parent, op, archive):
    return run(
        [
            tool,
            "--root",
            str(parent),
            "--tenant",
            "studio",
            "--key-file",
            str(root / "recovery-key"),
            op,
            str(archive),
        ],
        timeout=300,
    )


stop = threading.Event()
neighbor_results = []


def watch_neighbor(obj):
    while not stop.is_set():
        begin = time.monotonic()
        try:
            with urllib.request.urlopen(obj["url"] + "/api/v1/health", timeout=3) as r:
                neighbor_results.append((r.status, time.monotonic() - begin))
        except Exception:
            neighbor_results.append((0, time.monotonic() - begin))
        stop.wait(0.25)


try:
    a = provision(source, "studio")
    b = provision(source, "neighbor")
    login(a)
    login(b)
    company = create(a, "company", {"name": "Recovery customer", "is_customer": True})
    neighbor = create(b, "company", {"name": "Neighbor customer", "is_customer": True})
    status, body = request(b, "/api/v1/company/" + str(neighbor["id"]))
    assert b"Neighbor customer" in body and b"Recovery customer" not in body
    cookie = "; ".join(c.name + "=" + c.value for c in a["jar"])
    req = urllib.request.Request(
        b["url"] + "/api/v1/company", headers={"Cookie": cookie}
    )
    try:
        urllib.request.urlopen(req)
        raise AssertionError("Foreign session accepted")
    except urllib.error.HTTPError as e:
        assert e.code == 401
    cash = create(
        a,
        "account",
        {"code": "REC-CASH", "name": "Recovery cash", "kind": "asset", "active": True},
    )
    equity = create(
        a,
        "account",
        {
            "code": "REC-EQUITY",
            "name": "Recovery equity",
            "kind": "equity",
            "active": True,
        },
    )
    journal = {
        "organization_id": 1,
        "source_type": "organization",
        "source_id": 1,
        "currency": "USD",
        "occurred_at": "2026-01-10",
        "memo": "Recovery opening balance",
        "lines": [
            {"account_id": cash["id"], "side": "debit", "amount": "123.45 USD"},
            {"account_id": equity["id"], "side": "credit", "amount": "123.45 USD"},
        ],
    }
    status, body = request(
        a, "/api/v1/journal/0/actions/create_and_post", {"journal": journal}
    )
    assert status == 200, (status, body[:800])
    bank = create(
        a,
        "bank_account",
        {"name": "Recovery bank", "currency": "USD", "account_id": cash["id"]},
    )
    connection = create(
        a,
        "bank_connection",
        {
            "name": "Recovery provider binding",
            "provider": "teller",
            "provider_account_id": "acc_recovery",
            "bank_account_id": bank["id"],
        },
    )
    status, body = request(
        a,
        "/bankfeed/" + str(connection["id"]) + "/settings",
        {
            "operation": "configure",
            "binding_id": 0,
            "version": 0,
            "access_token": "synthetic-recovery-token",
            "environment": "sandbox",
        },
        True,
    )
    assert status == 200, (status, body[:300])
    assert b"synthetic-recovery-token" not in body
    # Upload through the real application path; the restored document must
    # resolve to the same attachment bytes, not merely to descriptive metadata.
    attachment = b"Recovery receipt: USD 123.45\n"
    boundary = "venture-recovery-" + secrets.token_hex(12)
    payload = (
        (
            "--"
            + boundary
            + '\r\nContent-Disposition: form-data; name="file"; filename="recovery-receipt.txt"\r\nContent-Type: text/plain\r\n\r\n'
        ).encode()
        + attachment
        + ("\r\n--" + boundary + "--\r\n").encode()
    )
    upload = urllib.request.Request(
        a["url"] + "/ui/chat/upload",
        data=payload,
        headers={"Content-Type": "multipart/form-data; boundary=" + boundary},
    )
    with a["http"].open(upload, timeout=15) as response:
        document = json.load(response)
    status, body = request(a, "/api/v1/document/" + str(document["id"]))
    assert status == 200, (status, body[:300])
    metadata = json.loads(body)
    metadata = metadata.get("data", metadata)
    attachment_path = pathlib.PurePosixPath(metadata["path"]).relative_to(
        "/var/lib/venture"
    )
    assert metadata["hash"] == hashlib.sha256(attachment).hexdigest()
    assert (a["dir"] / "state" / attachment_path).read_bytes() == attachment
    status, body = request(
        a,
        "/api/v1/capture",
        {
            "kind": "receipt",
            "title": "Recovery receipt",
            "document_id": document["id"],
            "amount": "123.45 USD",
            "notes": "Restore preserves the source reference",
        },
    )
    assert status == 201, (status, body[:300])
    private(root / "recovery-key", secrets.token_hex(32) + "\n")
    run(["podman", "stop", "--time", "10", a["app"]])
    before = fingerprint(a)
    identity = (a["dir"] / "config" / "identity.env").read_bytes()
    neighbor_before = fingerprint(b)
    observer = threading.Thread(target=watch_neighbor, args=(b,), daemon=True)
    observer.start()
    started = time.monotonic()
    archive = root / "studio.gpg"
    backup_cmd(source, "create", archive)
    backup_time = time.monotonic() - started
    verified = json.loads(backup_cmd(source, "verify", archive))
    assert verified["tenant_id"] == "studio"
    restored = provision(target, "studio", False, a["workspace_id"])
    started = time.monotonic()
    backup_cmd(target, "restore", archive)
    restore_time = time.monotonic() - started
    after = fingerprint(restored)
    assert before == after, (before[1], after[1])
    assert (restored["dir"] / "config" / "identity.env").read_bytes() == identity
    assert (restored["dir"] / "state" / attachment_path).read_bytes() == attachment
    start_app(restored)
    login(restored)
    status, body = request(restored, "/api/v1/company/" + str(company["id"]))
    assert status == 200 and b"Recovery customer" in body
    status, body = request(restored, "/bankfeed/" + str(connection["id"]) + "/settings")
    assert (
        status == 200
        and b"Unconfigured" not in body
        and b"synthetic-recovery-token" not in body
    )
    neighbor_after = fingerprint(b)
    assert (
        neighbor_before == neighbor_after
    ), "Neighbor business rows changed during restore"
    stop.set()
    observer.join(5)
    assert neighbor_results and all(
        code == 200 for code, _ in neighbor_results
    ), neighbor_results
    print(
        json.dumps(
            {
                "application_head": args.application_revision,
                "application_image": image_id,
                "postgres_major": 18,
                "tables_reconciled": len(before[0]),
                "rows_reconciled": sum(before[0].values()),
                "all_row_sha256_equal": before[1] == after[1],
                "attachment_and_identity_equal": True,
                "neighbor_health_checks": len(neighbor_results),
                "neighbor_failures": 0,
                "neighbor_rows_unchanged": True,
                "backup_seconds": round(backup_time, 3),
                "restore_seconds": round(restore_time, 3),
                "archive_bytes": archive.stat().st_size,
                "financial_posting": "USD123.45 debit and credit",
                "live_provider_contacted": False,
            },
            indent=2,
        ),
        flush=True,
    )
    print("Evidence directory: " + str(root), flush=True)
finally:
    stop.set()
    for container in reversed(owned):
        try:
            logged = subprocess.run(["podman", "logs", container], capture_output=True)
            logs = logged.stdout + logged.stderr
            (root / (container[:12] + ".log")).write_bytes(logs)
            run(["podman", "rm", "-f", container])
        except Exception as e:
            print("Owned fixture cleanup: " + str(e), flush=True)
    for network in networks:
        try:
            run(["podman", "network", "rm", network])
        except Exception as e:
            print("Owned network cleanup: " + str(e), flush=True)
    for volume in volumes:
        try:
            run(["podman", "volume", "rm", volume])
        except Exception as e:
            print("Owned volume cleanup: " + str(e), flush=True)
    print("Retained private evidence directory: " + str(root), flush=True)
