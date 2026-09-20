#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Exercise local platform operations using new, disposable physical workspaces.

Requires qualified local DEBUG application and PostgreSQL images. Creates its own
private roots and removes only containers, networks and volumes registered there.
Private archives, credentials and logs remain as evidence. No existing workspace
is accepted as an argument. This is an orchestration drill, not a capacity test.
"""

import argparse
import json
import os
from pathlib import Path
import secrets
import socket
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--image", required=True)
parser.add_argument("--upgrade-image", required=True)
parser.add_argument("--application-revision", required=True)
parser.add_argument("--postgres-image", default="docker.io/library/postgres:18.4")
parser.add_argument("--binary", default="/usr/bin/venture")
args = parser.parse_args()
tool = Path(__file__).resolve().with_name("venture-tenantctl")
root = Path(tempfile.mkdtemp(prefix="venture-tenant-lifecycle-drill-"))
root.chmod(0o700)
owned = []
evidence = []


def run(argv, allowed=(0,)):
    result = subprocess.run(argv, capture_output=True, text=True, timeout=1200)
    if result.returncode not in allowed:
        # Container diagnostics may contain connection details. Preserve them
        # privately instead of copying arbitrary command stderr into a transcript.
        private(root / "failure.txt", result.stderr)
        raise RuntimeError("Operation failed; inspect private failure.txt")
    return result


def private(path, value):
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(descriptor, "w") as output:
        output.write(value)


def passed(message):
    evidence.append("PASS: " + message)
    print(evidence[-1], flush=True)


def operation(host, name, *arguments, allowed=(0,)):
    return run([str(tool), "--root", str(host), name, "studio", *arguments], allowed)


def manifest(host):
    return json.loads((host / "studio" / "manifest.json").read_text())


def identity(host):
    return dict(
        line.split("=", 1)
        for line in (host / "studio/config/identity.env").read_text().splitlines()
    )


def provision(host, *extra):
    run([str(tool), "--root", str(host), "init"])
    owned.append(host)
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        port = listener.getsockname()[1]
    operation(
        host,
        "provision",
        "--origin",
        "https://studio.example",
        "--image",
        args.image,
        "--postgres-image",
        args.postgres_image,
        "--port",
        str(port),
        "--binary",
        args.binary,
        *extra,
        "--reason",
        "Synthetic lifecycle drill",
    )


def cleanup(host):
    path = host / "studio/manifest.json"
    if not path.exists():
        return
    row = manifest(host)
    for key in ("app_container", "db_container"):
        if not row.get(key):
            continue
        inspected = json.loads(run(["podman", "inspect", row[key]]).stdout)[0]
        labels = inspected["Config"]["Labels"]
        if (
            inspected["Id"] != row[key]
            or labels.get("io.venture.host") != row["host_id"]
            or labels.get("io.venture.tenant") != row["tenant_id"]
        ):
            raise RuntimeError("Refusing cleanup of a foreign container")
        run(["podman", "rm", "--force", row[key]])
    for kind, key in (("volume", "volume"), ("network", "network")):
        if row.get(key):
            inspected = json.loads(run(["podman", kind, "inspect", row[key]]).stdout)[0]
            labels = inspected.get("Labels") or inspected.get("labels") or {}
            if (
                labels.get("io.venture.host") != row["host_id"]
                or labels.get("io.venture.tenant") != row["tenant_id"]
            ):
                raise RuntimeError("Refusing cleanup of a foreign resource")
            run(["podman", kind, "rm", row[key]])


try:
    source, destination = root / "source", root / "destination"
    initial, recovered, backup_key = (
        root / "initial-password",
        root / "recovered-password",
        root / "backup-key",
    )
    for path in (initial, recovered, backup_key):
        private(path, secrets.token_urlsafe(32) + "\n")
    provision(source, "--password-file", str(initial))
    original = manifest(source)
    assert original["phase"] == "ready"
    operation(source, "start", "--reason", "Synthetic source startup")
    operation(source, "stop", "--reason", "Synthetic consistent snapshot")
    archive = root / "snapshot.gpg"
    operation(
        source,
        "export",
        "--archive",
        str(archive),
        "--key-file",
        str(backup_key),
        "--reason",
        "Synthetic complete export",
    )
    passed(
        "provision, bootstrap, healthy startup and encrypted stopped-workspace export"
    )
    provision(destination, "--empty", "--workspace-id", original["workspace_id"])
    target = manifest(destination)
    empty_status = json.loads(operation(destination, "status").stdout)
    assert empty_status["phase"] == "restore-target" and "persisted" not in empty_status
    assert (
        target["network"] != original["network"]
        and target["volume"] != original["volume"]
    )
    operation(
        destination,
        "restore",
        "--archive",
        str(archive),
        "--key-file",
        str(backup_key),
        "--reason",
        "Synthetic independent restore",
    )
    operation(
        destination, "activate-restored", "--reason", "Synthetic credential quarantine"
    )
    status = json.loads(operation(destination, "status").stdout)
    assert status["persisted"]["state"] == "suspended"
    assert manifest(destination)["phase"] == "recovery-review"
    assert (
        identity(destination)["VENTURE_INTEGRATION_KEY"]
        == identity(source)["VENTURE_INTEGRATION_KEY"]
    )
    assert (
        identity(destination)["VENTURE_SESSION_SECRET"]
        != identity(source)["VENTURE_SESSION_SECRET"]
    )
    refused = operation(
        destination,
        "state",
        "active",
        "--reason",
        "Must require administrator recovery",
        allowed=(2,),
    )
    assert "Recover a named administrator" in refused.stderr
    passed(
        "physical restore preserves workspace/encryption identity, rotates session secret and requires recovery before activation"
    )
    operation(
        destination,
        "recover-admin",
        "--username",
        "workspace-admin",
        "--password-file",
        str(recovered),
        "--reason",
        "Synthetic explicit administrator recovery",
    )
    assert (
        json.loads(operation(destination, "status").stdout)["persisted"]["state"]
        == "suspended"
    )
    operation(destination, "start", "--reason", "Synthetic suspended review startup")
    operation(destination, "stop", "--reason", "Synthetic reviewed reactivation")
    operation(
        destination,
        "state",
        "active",
        "--reason",
        "Synthetic explicit reactivation after review",
    )
    passed(
        "fresh administrator recovery leaves suspension intact; suspended server starts; explicit reactivation clears review phase"
    )
    operation(
        destination,
        "upgrade",
        "--image",
        args.upgrade_image,
        "--archive",
        str(root / "before-upgrade.gpg"),
        "--key-file",
        str(backup_key),
        "--reason",
        "Synthetic bounded upgrade",
    )
    upgraded = manifest(destination)
    assert (
        upgraded["phase"] == "ready"
        and upgraded["previous_image"] == original["app_image"]
    )
    assert upgraded["app_image"] != original["app_image"]
    operation(destination, "start", "--reason", "Synthetic upgraded startup")
    passed(
        "pinned candidate upgrade takes locked snapshot, migrates offline and starts healthy"
    )
    operation(destination, "hold", "on", "--reason", "Synthetic hold")
    operation(destination, "offboard", "--reason", "Synthetic retained offboarding")
    assert (
        manifest(destination)["phase"] == "offboarded" and manifest(destination)["hold"]
    )
    refused = operation(
        destination, "start", "--reason", "Offboarded restart must refuse", allowed=(2,)
    )
    assert "not qualified" in refused.stderr and archive.exists()
    passed("held offboarding retains data and archives and refuses restart")
finally:
    for host in reversed(owned):
        cleanup(host)
    private(
        root / "evidence.json",
        json.dumps(
            {
                "revision": args.application_revision,
                "image": args.image,
                "upgrade_image": args.upgrade_image,
                "checks": evidence,
            },
            indent=2,
        )
        + "\n",
    )
    print("Private evidence: " + str(root), flush=True)
