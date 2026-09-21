#!/usr/bin/python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Run shipped operator commands with actual GnuPG and owned synthetic archives.

Requires rootless Podman for its read-only platform preflight; creates no
containers, database, volume or network. Retains private synthetic evidence.
"""
import base64
import hashlib
import json
import os
from pathlib import Path
import secrets
import subprocess
import tarfile
import tempfile
import uuid

root = Path(__file__).resolve().parents[2]
tool = root / "tools/venture-tenantctl"
backup = root / "tools/venture-tenant-backup"
state = Path(tempfile.mkdtemp(prefix="venture-125-retention-demo-"))
os.umask(0o077)
for name in ("platform", "platform/studio", "platform/studio/control", "archives", "payload", "source", "gnupg"):
    (state / name).mkdir(mode=0o700)
workspace, host, archive_id = (str(uuid.uuid4()) for _ in range(3))
registration = {"schema": 1, "host_id": host, "tenant_id": "studio", "workspace_id": workspace,
                "app_container": "a" * 64, "db_container": "b" * 64, "app_image": "sha256:" + "c" * 64,
                "database": "venture", "db_role": "venture", "postgres_major": 18, "phase": "ready"}
(state / "platform/host.json").write_text(json.dumps({"schema": 1, "host_id": host}))
(state / "platform/studio/manifest.json").write_text(json.dumps(registration))
key = state / "key"
key.write_text(base64.b64encode(secrets.token_bytes(48)).decode() + "\n")
(state / "source/synthetic.txt").write_text("Synthetic recovery payload, no business data\n")
for name in ("state", "config"):
    with tarfile.open(state / f"payload/{name}.tar", "w") as bundle:
        bundle.add(state / "source/synthetic.txt", arcname="synthetic.txt")
(state / "payload/database.dump").write_bytes(b"Synthetic database fixture only\n")
manifest = {"schema": 1, "archive_id": archive_id, "workspace_id": workspace, "tenant_id": "studio",
            "app_image": registration["app_image"], "postgres_major": 18, "created_at": "2020-01-01T00:00:00Z"}
for name, suffix in (("database", "dump"), ("state", "tar"), ("config", "tar")):
    manifest[name + "_sha256"] = hashlib.sha256((state / f"payload/{name}.{suffix}").read_bytes()).hexdigest()
(state / "payload/manifest.json").write_text(json.dumps(manifest))
with tarfile.open(state / "payload.tar", "w") as bundle:
    for path in (state / "payload").iterdir():
        bundle.add(path, arcname=path.name)
env = os.environ.copy()
for name in ("VENTURE_TENANT_MAINTENANCE_FD", "VENTURE_TENANT_ARCHIVE_KIND"):
    env.pop(name, None)
env["GNUPGHOME"] = str(state / "gnupg")
primary, replica, unregistered = (state / "archives" / name for name in ("primary.gpg", "replica.gpg", "unregistered.gpg"))
with key.open("rb") as passphrase:
    subprocess.run(["gpg", "--batch", "--yes", "--no-tty", "--pinentry-mode", "loopback", "--no-symkey-cache",
                    "--passphrase-fd", str(passphrase.fileno()), "--symmetric", "--force-mdc", "--cipher-algo", "AES256",
                    "--output", str(primary), str(state / "payload.tar")], env=env,
                   pass_fds=(passphrase.fileno(),), stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True, timeout=30)
replica.write_bytes(primary.read_bytes())
unregistered.write_bytes(primary.read_bytes())


def command(operation, *arguments, refused=False):
    result = subprocess.run([str(tool), "--root", str(state / "platform"), operation, "studio", *arguments],
                            env=env, capture_output=True, text=True, timeout=60)
    assert result.returncode == (2 if refused else 0), result.stderr
    return result


print("Operator SHA256: " + hashlib.sha256(tool.read_bytes()).hexdigest())
print("Backup SHA256: " + hashlib.sha256(backup.read_bytes()).hexdigest())
for path in (primary, replica):
    command("backup-enroll", "--archive", str(path), "--key-file", str(key), "--reason", "Explicit synthetic local enrollment")
catalog = json.loads(command("backup-list").stdout)
assert len(catalog["entries"]) == 2
assert {entry["archive_id"] for entry in catalog["entries"].values()} == {archive_id}
print("PASS two authenticated local copies have distinct catalog IDs and one matching workspace/archive UUID")
plan = json.loads(command("retention-plan", "--reason", "Review synthetic expiry").stdout)
assert plan["days"] == 30 and len(plan["candidates"]) == 2
assert primary.exists() and replica.exists() and unregistered.exists()
print("PASS default 30-day plan names exactly the two enrolled copies and deletes nothing")
command("hold", "on", "--reason", "Synthetic legal hold")
result = command("retention-execute", "--plan", plan["plan_id"], "--reason", "Held deletion must fail", refused=True)
assert "on hold" in result.stderr and primary.exists() and replica.exists()
print("PASS a subsequently applied hold refuses execution and preserves both copies")
command("hold", "off", "--reason", "Release synthetic hold")
command("retention-execute", "--plan", plan["plan_id"], "--reason", "Approve reviewed synthetic expiry")
assert not primary.exists() and not replica.exists() and unregistered.exists()
catalog = json.loads(command("backup-list").stdout)
assert all(entry["state"] == "deleted" for entry in catalog["entries"].values())
assert catalog["journal"] is None
print("PASS explicit execution deletes only enrolled copies, persists two tombstones and leaves the unregistered file")
result = subprocess.run([str(backup), "--root", str(state / "platform"), "--tenant", "studio", "--key-file", str(key),
                         "verify", str(unregistered)], env=env, capture_output=True, text=True, timeout=60)
assert result.returncode == 2 and "retired by this workspace" in result.stderr
print("PASS authenticated unregistered copy of the retired archive is refused by the original workspace ledger")
print("PASS no container/database/volume/network was created; all archive contents and deletion targets were owned synthetic fixtures")
print("Evidence directory: " + str(state))
