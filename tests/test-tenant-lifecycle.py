#!/usr/bin/python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Platform ownership tests; container execution is replaced, file locks are real."""

import copy
import contextlib
import io
import importlib.machinery
import importlib.util
import json
import hashlib
import os
from pathlib import Path
import subprocess
import signal
import base64
import sys
import unittest
from unittest import mock
import uuid

signal.alarm(30)
sys.dont_write_bytecode = True
ROOT = Path(sys.argv.pop(1))
SCRIPT = Path(__file__).resolve().parents[1] / "tools" / "venture-tenantctl"
loader = importlib.machinery.SourceFileLoader("tenantctl", str(SCRIPT))
spec = importlib.util.spec_from_loader(loader.name, loader)
tool = importlib.util.module_from_spec(spec)
loader.exec_module(tool)


class Lifecycle(unittest.TestCase):
    def setUp(self):
        self.root = ROOT / str(uuid.uuid4())
        self.root.mkdir(mode=0o700)
        self.host = str(uuid.uuid4())
        tool.write_private(
            self.root / "host.json", json.dumps({"schema": 1, "host_id": self.host})
        )
        self.calls = []
        self.rows = {}
        self.patch = mock.patch.object(tool, "run", side_effect=self.run_container)
        self.patch.start()
        self.addCleanup(self.patch.stop)
        self.platform = tool.Platform(self.root)
        self.tenant = self.platform.tenant("studio")
        self.tenant.directory.mkdir(mode=0o700)
        for child in ("control", "state", "config"):
            (self.tenant.directory / child).mkdir(mode=0o700)
        self.manifest = {
            "schema": 1,
            "host_id": self.host,
            "tenant_id": "studio",
            "workspace_id": str(uuid.uuid4()),
            "origin": "https://studio.example",
            "phase": "ready",
            "app_container": "a" * 64,
            "db_container": "b" * 64,
            "app_image": "sha256:" + "c" * 64,
            "db_image": "sha256:" + "d" * 64,
            "network": "studio-network",
            "volume": "studio-volume",
            "app_memory": "512m",
            "app_cpus": 1,
            "port": 8748,
            "binary": "/usr/bin/venture",
        }
        tool.write_private(
            self.tenant.directory / "manifest.json", json.dumps(self.manifest)
        )
        tool.write_private(
            self.tenant.control / "runtime.env",
            "VENTURE_DATABASE_URI=postgresql://venture@database/venture\n",
        )
        for role, identity, image in (
            ("application", "a", "c"),
            ("database", "b", "d"),
        ):
            mounts = [
                {
                    "Source": str(self.tenant.directory / "state"),
                    "Destination": "/var/lib/venture",
                    "RW": True,
                },
                {
                    "Source": str(self.tenant.directory / "config"),
                    "Destination": "/etc/venture",
                    "RW": False,
                },
            ]
            if role == "database":
                mounts = [
                    {
                        "Name": "studio-volume",
                        "Destination": "/var/lib/postgresql",
                        "RW": True,
                    }
                ]
            self.rows[identity * 64] = {
                "Id": identity * 64,
                "Image": image * 64,
                "Config": {
                    "Labels": {
                        "io.venture.host": self.host,
                        "io.venture.tenant": "studio",
                        "io.venture.role": role,
                    }
                },
                "State": {"Running": role == "database"},
                "Mounts": mounts,
                "NetworkSettings": {"Networks": {"studio-network": {}}},
            }

    def run_container(self, argv, *unused, **options):
        self.calls.append(argv)
        if argv[:2] == ["podman", "info"]:
            return b"true\n"
        if argv[:3] == ["podman", "container", "exists"]:
            return 0 if argv[3] in self.rows else 1
        if argv[:2] == ["podman", "inspect"]:
            return json.dumps([copy.deepcopy(self.rows[argv[2]])]).encode()
        if argv[:2] == ["podman", "stop"]:
            self.rows[argv[-1]]["State"]["Running"] = False
            return b""
        raise AssertionError("Unexpected container operation")

    def test_registration_refuses_duplicate_identity_or_origin(self):
        other = self.platform.tenant("neighbor")
        proposed = {
            **self.manifest,
            "tenant_id": "neighbor",
            "origin": "https://neighbor.example",
        }
        with self.assertRaises(tool.Refused):
            self.platform.reserve(other, proposed)
        proposed["workspace_id"] = str(uuid.uuid4())
        proposed["origin"] = self.manifest["origin"]
        with self.assertRaises(tool.Refused):
            self.platform.reserve(other, proposed)
        self.assertFalse(other.directory.exists())
        proposed["origin"] = "https://neighbor.example"
        self.platform.reserve(other, proposed)
        self.assertEqual(
            tool.read_json(other.directory / "manifest.json")["workspace_id"],
            proposed["workspace_id"],
        )

    def test_abandoned_maintenance_refuses_new_operations(self):
        tool.write_private(
            self.tenant.control / "maintenance-container", "e" * 64 + "\n"
        )
        with self.assertRaises(tool.Refused), self.tenant.locked():
            self.fail("abandoned maintenance permitted another operation")
        self.rows["e" * 64] = copy.deepcopy(self.rows["a" * 64])
        self.rows["e" * 64]["Id"] = "e" * 64
        self.rows["e" * 64]["Config"]["Labels"]["io.venture.role"] = "maintenance"
        self.rows["e" * 64]["Config"]["Labels"]["io.venture.tenant"] = "neighbor"
        with self.tenant.locked(allow_abandoned=True), self.assertRaises(tool.Refused):
            self.tenant.recover_maintenance("Investigate abandoned operation")
        self.assertFalse(any(call[:2] == ["podman", "rm"] for call in self.calls))
        del self.rows["e" * 64]
        with self.tenant.locked(allow_abandoned=True):
            self.tenant.recover_maintenance("Verify completed container cleanup")
        self.assertFalse((self.tenant.control / "maintenance-container").exists())

    def test_restore_quarantines_before_reopening_access(self):
        path = self.tenant.directory / "config" / "identity.env"
        master = base64.b64encode(b"x" * 32).decode()
        tool.write_private(
            path,
            "VENTURE_INTEGRATION_KEY="
            + master
            + "\nVENTURE_SESSION_SECRET="
            + "a" * 64
            + "\n",
        )
        tool.write_private(
            self.tenant.control / "last-restore.json",
            json.dumps({"workspace_id": self.manifest["workspace_id"]}),
        )
        with self.tenant.locked(), mock.patch.object(
            self.tenant,
            "identity_status",
            side_effect=[{"state": "active"}, {"state": "suspended"}],
        ), mock.patch.object(self.tenant, "offline") as offline:
            self.tenant.manifest["phase"] = "restore-target"
            self.tenant.activate_restored("Validate restored access")
            offline.assert_called_once_with(
                [
                    "--tenant-revoke-credentials",
                    "--tenant-reason",
                    "Validate restored access",
                ]
            )
            self.assertEqual(self.tenant.manifest["phase"], "recovery-review")
            with self.assertRaises(tool.Refused):
                self.tenant.set_state("active", "Cannot skip administrator recovery")
        values = dict(line.split("=", 1) for line in path.read_text().splitlines())
        self.assertEqual(values["VENTURE_INTEGRATION_KEY"], master)
        self.assertNotEqual(values["VENTURE_SESSION_SECRET"], "a" * 64)

    def test_failed_restore_quarantine_stays_unstartable(self):
        tool.write_private(
            self.tenant.directory / "config" / "identity.env",
            "VENTURE_INTEGRATION_KEY="
            + base64.b64encode(b"x" * 32).decode()
            + "\nVENTURE_SESSION_SECRET="
            + "a" * 64
            + "\n",
        )
        tool.write_private(
            self.tenant.control / "last-restore.json",
            json.dumps({"workspace_id": self.manifest["workspace_id"]}),
        )
        with self.tenant.locked(), mock.patch.object(
            self.tenant, "identity_status", return_value={"state": "active"}
        ), mock.patch.object(
            self.tenant, "offline", side_effect=tool.Refused("Interrupted quarantine")
        ):
            self.tenant.manifest["phase"] = "restore-target"
            with self.assertRaises(tool.Refused):
                self.tenant.activate_restored("Validate restored access")
            self.assertEqual(self.tenant.manifest["phase"], "quarantining")
            with self.assertRaises(tool.Refused):
                self.tenant.start("Must not reopen stale authority")
        self.assertEqual(
            tool.read_json(self.tenant.directory / "manifest.json")["phase"],
            "quarantining",
        )

    def test_password_input_refuses_fifo_before_reading(self):
        path = self.root / "password-fifo"
        os.mkfifo(path, 0o600)
        with self.assertRaises(tool.Refused):
            tool.password_material(path)

    def test_other_tenant_cannot_be_stopped(self):
        # Stale or forged registration must not stop a neighbor, even if the
        # container ID exists and its state/image otherwise look correct.
        self.rows["a" * 64]["Config"]["Labels"]["io.venture.tenant"] = "neighbor"
        with self.tenant.locked(), self.assertRaises(tool.Refused):
            self.tenant.stop("maintenance")
        self.assertFalse(any(call[:2] == ["podman", "stop"] for call in self.calls))

    def test_database_volume_and_network_are_bound(self):
        with self.tenant.locked():
            self.rows["b" * 64]["Mounts"][0]["Name"] = "neighbor-volume"
            with self.assertRaises(tool.Refused):
                self.tenant.postgres("SELECT 1")
            self.rows["b" * 64]["Mounts"][0]["Name"] = "studio-volume"
            self.rows["a" * 64]["NetworkSettings"]["Networks"]["neighbor-network"] = {}
            with self.assertRaises(tool.Refused):
                self.tenant.container("application")
        self.assertFalse(any(call[:2] == ["podman", "exec"] for call in self.calls))

    def test_wrong_mount_and_image_refuse(self):
        with self.tenant.locked():
            self.rows["a" * 64]["Mounts"][1]["RW"] = True
            with self.assertRaises(tool.Refused):
                self.tenant.container("application")
            self.rows["a" * 64]["Mounts"][1]["RW"] = False
            self.rows["a" * 64]["Image"] = "f" * 64
            with self.assertRaises(tool.Refused):
                self.tenant.container("application")

    def test_lock_conflict_and_links_refuse(self):
        with self.tenant.locked():
            other = self.platform.tenant("studio")
            with self.assertRaises(tool.Refused), other.locked():
                self.fail("second maintenance acquired lock")
        path = self.tenant.control / "maintenance.lock"
        path.unlink()
        os.symlink(self.root / "host.json", path)
        with self.assertRaises(OSError), self.tenant.locked():
            self.fail("symlink lock accepted")

    def test_public_or_hardlinked_manifest_refused(self):
        path = self.tenant.directory / "manifest.json"
        path.chmod(0o644)
        with self.assertRaises(tool.Refused), self.tenant.locked():
            self.fail("public manifest accepted")
        path.chmod(0o600)
        os.link(path, self.root / "manifest-copy")
        with self.assertRaises(tool.Refused), self.tenant.locked():
            self.fail("aliased manifest accepted")

    def test_stop_is_idempotent_and_audited(self):
        self.rows["a" * 64]["State"]["Running"] = True
        with self.tenant.locked():
            self.tenant.stop("planned service")
            self.tenant.stop("already stopped")
        self.assertEqual(sum(call[:2] == ["podman", "stop"] for call in self.calls), 1)
        events = [
            json.loads(line)
            for line in (self.tenant.control / "operations.jsonl")
            .read_text()
            .splitlines()
        ]
        self.assertEqual(len(events), 4)
        self.assertEqual(events[1]["outcome"], "complete")
        self.assertEqual(events[1]["previous"], events[0]["sha256"])
        self.assertTrue(
            all(
                event["workspace_id"] == self.manifest["workspace_id"]
                for event in events
            )
        )

    def test_foreign_persisted_identity_refused(self):
        with self.tenant.locked(), mock.patch.object(
            self.tenant,
            "offline",
            return_value=json.dumps(
                {
                    "workspace_id": str(uuid.uuid4()),
                    "origin": self.manifest["origin"],
                    "state": "active",
                }
            ).encode(),
        ):
            with self.assertRaises(tool.Refused):
                self.tenant.identity_status()

    def test_failed_offline_command_removes_only_its_container(self):
        tool.write_private(
            self.tenant.control / "runtime.env", "VENTURE_DB_PASSWORD=never-log-this\n"
        )
        original = self.run_container

        def controlled(argv, *values, **options):
            if argv[:2] == ["podman", "create"]:
                self.calls.append(argv)
                self.assertIn("io.venture.role=maintenance", argv)
                self.assertIn("sha256:" + "f" * 64, argv)
                return ("e" * 64).encode()
            if argv[:2] == ["podman", "start"]:
                self.calls.append(argv)
                raise tool.CommandFailure(
                    b"Synthetic failure never-log-this postgresql://operator:uri-secret@database/venture"
                )
            if argv[:2] == ["podman", "rm"]:
                self.calls.append(argv)
                return b""
            return original(argv, *values, **options)

        self.patch.stop()
        with mock.patch.object(
            tool, "run", side_effect=controlled
        ), self.tenant.locked():
            with self.assertRaises(tool.Refused):
                self.tenant.offline(["--migrate"], image="sha256:" + "f" * 64)
        self.assertEqual(self.calls[-1], ["podman", "rm", "--force", "e" * 64])
        self.assertFalse((self.tenant.control / "maintenance-container").exists())
        diagnostic = (self.tenant.control / "last-maintenance-error.txt").read_text()
        self.assertNotIn("never-log-this", diagnostic)
        self.assertNotIn("uri-secret", diagnostic)
        self.assertIn("[redacted]", diagnostic)
        self.assertEqual(
            tool.read_json(self.tenant.directory / "manifest.json")["app_container"],
            "a" * 64,
        )

    def test_failed_upgrade_never_restarts_old_binary_on_changed_schema(self):
        with self.tenant.locked(), mock.patch.object(
            self.platform, "image", return_value="sha256:" + "f" * 64
        ), mock.patch.object(
            self.tenant, "identity_status", return_value={"state": "active"}
        ), mock.patch.object(
            self.tenant, "probe_image"
        ), mock.patch.object(
            self.tenant, "archive"
        ) as archive, mock.patch.object(
            self.tenant,
            "offline",
            side_effect=tool.Refused("Synthetic migration failure"),
        ):
            with self.assertRaises(tool.Refused):
                self.tenant.upgrade(
                    "candidate",
                    self.root / "rollback.gpg",
                    self.root / "key",
                    "upgrade fixture",
                )
            archive.assert_called_once()
        saved = tool.read_json(self.tenant.directory / "manifest.json")
        self.assertEqual(saved["phase"], "upgrade-failed")
        self.assertEqual(saved["previous_image"], self.manifest["app_image"])
        self.assertFalse(any(call[:2] == ["podman", "start"] for call in self.calls))

    def test_empty_restore_target_status_never_starts_maintenance(self):
        # An empty recovery target has no application config or identity yet;
        # inventory must still report its registration without starting a binary.
        self.manifest["phase"] = "restore-target"
        tool.write_private(
            self.tenant.directory / "manifest.json", json.dumps(self.manifest)
        )
        output = io.StringIO()
        with mock.patch.object(
            sys, "argv", [str(SCRIPT), "--root", str(self.root), "status", "studio"]
        ), contextlib.redirect_stdout(output):
            tool.main()
        result = json.loads(output.getvalue())
        self.assertEqual(result["phase"], "restore-target")
        self.assertNotIn("persisted", result)
        self.assertFalse(any(call[:2] == ["podman", "create"] for call in self.calls))

    def test_help_and_initialization_have_no_container_side_effect(self):
        result = subprocess.run(
            [str(SCRIPT), "--help"], capture_output=True, text=True, timeout=10
        )
        self.assertEqual(result.returncode, 0)
        initialized = self.root / "fresh"
        tool.initialize(initialized)
        self.assertEqual(tool.read_json(initialized / "host.json")["schema"], 1)
        with self.assertRaises(tool.Refused):
            tool.initialize(initialized)
        self.assertEqual(len(self.calls), 1)


    def test_backup_catalog_retention(self):
        # A plan never authorizes deleting a different inode or an unregistered file.
        archive_dir = self.root.parent / (self.root.name + "-archives")
        archive_dir.mkdir(mode=0o700)
        archive = archive_dir / "snapshot.gpg"
        tool.write_private(archive, "synthetic encrypted fixture")
        tool.write_private(archive_dir / "unregistered.gpg", "must remain")
        manifest = {"workspace_id": self.manifest["workspace_id"], "tenant_id": "studio",
                    "archive_id": str(uuid.uuid4()), "created_at": "2020-01-01T00:00:00Z",
                    "encrypted_sha256": hashlib.sha256(archive.read_bytes()).hexdigest()}
        with self.tenant.locked():
            catalog = tool.BackupCatalog(self.tenant)
            entry = catalog.register(archive, manifest, "export", "Synthetic fixture")
            plan = catalog.plan(30, "Review expiry")
            self.assertEqual(plan["candidates"], [entry["copy_id"]])
            self.assertTrue(archive.exists())
            catalog.execute(plan["plan_id"], "Expire reviewed fixture")
            self.assertFalse(archive.exists())
            self.assertTrue((archive_dir / "unregistered.gpg").exists())
            with self.assertRaises(tool.Refused):
                catalog.check_restore(manifest, entry["sha256"])
            self.assertEqual(catalog.data["entries"][entry["copy_id"]]["state"], "deleted")

    def test_backup_catalog_hold_and_replacement(self):
        archive_dir = self.root.parent / (self.root.name + "-archives")
        archive_dir.mkdir(mode=0o700)
        archive = archive_dir / "snapshot.gpg"
        tool.write_private(archive, "synthetic encrypted fixture")
        manifest = {"workspace_id": self.manifest["workspace_id"], "tenant_id": "studio",
                    "archive_id": str(uuid.uuid4()), "created_at": "2020-01-01T00:00:00Z",
                    "encrypted_sha256": hashlib.sha256(archive.read_bytes()).hexdigest()}
        with self.tenant.locked():
            catalog = tool.BackupCatalog(self.tenant)
            catalog.register(archive, manifest, "export", "Synthetic fixture")
            plan = catalog.plan(30, "Review expiry")
            self.tenant.manifest["hold"] = True
            self.tenant.save()
            with self.assertRaises(tool.Refused):
                catalog.execute(plan["plan_id"], "Held fixture")
            self.tenant.manifest["hold"] = False
            self.tenant.save()
            tool.write_private(archive, "replaced unrelated inode")
            with self.assertRaises(tool.Refused):
                catalog.execute(plan["plan_id"], "Replaced fixture")
            self.assertEqual(archive.read_text(), "replaced unrelated inode")


    def catalog_fixture(self):
        directory = self.root.parent / (self.root.name + "-archives")
        directory.mkdir(mode=0o700)
        archive = directory / "snapshot.gpg"
        tool.write_private(archive, "synthetic encrypted fixture")
        manifest = {"workspace_id": self.manifest["workspace_id"], "tenant_id": "studio",
                    "archive_id": str(uuid.uuid4()), "created_at": "2020-01-01T00:00:00Z",
                    "encrypted_sha256": hashlib.sha256(archive.read_bytes()).hexdigest()}
        return directory, archive, manifest

    def test_retention_crash_recovery_preserves_survivors(self):
        directory, archive, manifest = self.catalog_fixture()
        survivor = directory / "survivor.gpg"
        tool.write_private(survivor, "second encrypted fixture")
        with self.tenant.locked():
            catalog = tool.BackupCatalog(self.tenant)
            first = catalog.register(archive, manifest, "export", "Fixture")
            catalog.register(survivor, {**manifest, "archive_id": str(uuid.uuid4()), "encrypted_sha256": hashlib.sha256(survivor.read_bytes()).hexdigest()}, "upgrade", "Fixture")
            plan = catalog.plan(30, "Fixture plan")
            # Force the first candidate to be the archive whose crash is simulated.
            catalog.data["plan"]["candidates"].sort(key=lambda value: value != first["copy_id"])
            catalog.save()
            catalog.data["plan"]["revision"] = catalog.data["revision"]
            tool.write_private(catalog.path, json.dumps(catalog.data))
            unlink = tool.os.unlink
            def interrupted(path, **kwargs):
                unlink(path, **kwargs)
                raise OSError("Simulated crash after unlink before catalog publication")
            with mock.patch.object(tool.os, "unlink", side_effect=interrupted):
                with self.assertRaises(OSError):
                    catalog.execute(plan["plan_id"], "Fixture expiry")
            restarted = tool.BackupCatalog(self.tenant)
            self.assertIsNotNone(restarted.data["journal"])
            with self.assertRaises(tool.Refused):
                restarted.plan(30, "Must recover first")
            self.tenant.manifest["hold"] = True
            self.tenant.save()
            restarted.recover("Reconcile interruption under hold")
            self.assertFalse(archive.exists())
            self.assertTrue(survivor.exists())
            self.assertIsNone(tool.BackupCatalog(self.tenant).data["journal"])
            self.assertEqual(restarted.data["entries"][first["copy_id"]]["deletion_outcome"],
                             "missing-after-durable-intent")

    def test_retention_parent_replacement_and_links(self):
        directory, archive, manifest = self.catalog_fixture()
        with self.tenant.locked():
            catalog = tool.BackupCatalog(self.tenant)
            catalog.register(archive, manifest, "export", "Fixture")
            plan = catalog.plan(30, "Fixture plan")
            old = directory.with_name(directory.name + "-retained")
            directory.rename(old)
            directory.mkdir(mode=0o700)
            tool.write_private(archive, "synthetic encrypted fixture")
            with self.assertRaises(tool.Refused):
                catalog.execute(plan["plan_id"], "Refuse replaced parent")
            archive.unlink()
            archive.symlink_to(old / archive.name)
            with self.assertRaises(OSError):
                catalog.plan(30, "Refuse symlink")
            archive.unlink()
            os.link(old / archive.name, archive)
            with self.assertRaises(tool.Refused):
                catalog.plan(30, "Refuse hard link")
            self.assertTrue((old / archive.name).exists())

    def test_retention_replica_identity_and_stale_plan(self):
        directory, archive, manifest = self.catalog_fixture()
        with self.tenant.locked():
            catalog = tool.BackupCatalog(self.tenant)
            first = catalog.register(archive, manifest, "export", "Fixture")
            plan = catalog.plan(30, "Fixture plan")
            replica = directory / "replica.gpg"
            tool.write_private(replica, archive.read_bytes())
            second = catalog.register(replica, manifest, "replica", "Explicit local replica")
            self.assertNotEqual(first["copy_id"], second["copy_id"])
            self.assertEqual(first["archive_id"], second["archive_id"])
            with self.assertRaises(tool.Refused):
                catalog.execute(plan["plan_id"], "Stale plan")
            altered = directory / "altered.gpg"
            tool.write_private(altered, "different encrypted bytes")
            with self.assertRaises(tool.Refused):
                catalog.register(altered, manifest, "replica", "UUID content mismatch")
            with self.assertRaises(tool.Refused):
                catalog.register(altered, {**manifest, "workspace_id": str(uuid.uuid4())}, "replica", "Wrong workspace")
            with self.assertRaises((tool.Refused, ValueError)):
                catalog.register(altered, {**manifest, "archive_id": ""}, "replica", "Legacy needs re-export")
            current = catalog.plan(30, "Review both copies")
            catalog.execute(current["plan_id"], "Expire explicitly registered copies")
            self.assertFalse(archive.exists())
            self.assertFalse(replica.exists())
            self.assertTrue(altered.exists())
            with self.assertRaises(tool.Refused):
                catalog.register(altered, manifest, "replica", "Retired archive cannot re-enter")

    def test_retention_offboarding_hold_and_clock(self):
        _, archive, manifest = self.catalog_fixture()
        with self.tenant.locked():
            catalog = tool.BackupCatalog(self.tenant)
            catalog.register(archive, manifest, "export", "Fixture")
            plan = catalog.plan(30, "Fixture plan", now=1700000000)
            with self.assertRaises(tool.Refused):
                catalog.execute(plan["plan_id"], "Clock rollback", now=1699999999)
            self.tenant.manifest.update(phase="offboarded", offboarded_at=1700000000)
            self.tenant.save()
            with self.assertRaises(tool.Refused):
                catalog.execute(plan["plan_id"], "New offboard retention", now=1700000001)
            self.assertEqual(catalog.plan(30, "Wait offboarding period", now=1700000001)["candidates"], [])
            self.tenant.manifest["hold"] = True
            self.tenant.save()
            self.assertEqual(catalog.plan(30, "Held indefinitely", now=1800000000)["candidates"], [])
            self.assertTrue(archive.exists())

    def test_retention_lock_and_corrupt_catalog(self):
        with self.tenant.locked():
            result = subprocess.run([str(SCRIPT), "--root", str(self.root), "retention-plan", "studio", "--reason", "Competing operation"],
                                    capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 2)
            self.assertIn(b"already in progress", result.stderr)
            tool.write_private(self.tenant.control / "backup-catalog.json", "not json")
            with self.assertRaises(ValueError):
                tool.BackupCatalog(self.tenant)

    def test_retention_recovery_refuses_uncertain_replacement(self):
        _, archive, manifest = self.catalog_fixture()
        with self.tenant.locked():
            catalog = tool.BackupCatalog(self.tenant)
            catalog.register(archive, manifest, "export", "Fixture")
            plan = catalog.plan(30, "Fixture plan")
            with mock.patch.object(tool.os, "unlink", side_effect=OSError("Simulated filesystem refusal")):
                with self.assertRaises(OSError):
                    catalog.execute(plan["plan_id"], "Fixture expiry")
            tool.write_private(archive, "replacement must never be deleted")
            restarted = tool.BackupCatalog(self.tenant)
            with self.assertRaises(tool.Refused):
                restarted.recover("Uncertain replacement")
            self.assertIsNotNone(tool.BackupCatalog(self.tenant).data["journal"])
            self.assertEqual(archive.read_text(), "replacement must never be deleted")


    def test_retention_requires_durable_intent_before_unlink(self):
        _, archive, manifest = self.catalog_fixture()
        with self.tenant.locked():
            catalog = tool.BackupCatalog(self.tenant)
            catalog.register(archive, manifest, "export", "Fixture")
            plan = catalog.plan(30, "Fixture plan")
            with mock.patch.object(catalog, "save", side_effect=OSError("Catalog publication failed")):
                with self.assertRaises(OSError):
                    catalog.execute(plan["plan_id"], "Must journal first")
            self.assertTrue(archive.exists())
            self.assertIsNone(tool.BackupCatalog(self.tenant).data["journal"])


    def test_enrollment_binds_authenticated_encrypted_bytes(self):
        _, archive, manifest = self.catalog_fixture()
        with self.tenant.locked():
            catalog = tool.BackupCatalog(self.tenant)
            with self.assertRaises(tool.Refused):
                catalog.register(archive, {**manifest, "encrypted_sha256": "0" * 64}, "replica", "Stale verification snapshot")
            self.assertEqual(catalog.data["entries"], {})
            self.assertTrue(archive.exists())


    def test_registered_catalog_cannot_disappear_silently(self):
        _, archive, manifest = self.catalog_fixture()
        with self.tenant.locked():
            catalog = tool.BackupCatalog(self.tenant)
            catalog.register(archive, manifest, "export", "Fixture")
            self.assertTrue(tool.read_json(self.tenant.directory / "manifest.json")["backup_catalog_required"])
            catalog.path.unlink()
            with self.assertRaises(tool.Refused):
                tool.BackupCatalog(self.tenant)
            self.assertTrue(archive.exists())


    def test_backup_child_registration_survives_parent_manifest_save(self):
        # Upgrade writes its phase after the backup child initializes catalog
        # authority. A stale parent manifest must not erase that requirement.
        with self.tenant.locked():
            def backup_child(*args, **kwargs):
                current = tool.read_json(self.tenant.directory / "manifest.json")
                current["backup_catalog_required"] = True
                tool.write_private(self.tenant.directory / "manifest.json", json.dumps(current))
                return b"{}"
            with mock.patch.object(tool, "run", side_effect=backup_child):
                self.tenant.archive("verify", self.root / "fixture.gpg", self.root / "fixture-key")
            self.tenant.manifest["phase"] = "upgrading"
            self.tenant.save()
            self.assertTrue(tool.read_json(self.tenant.directory / "manifest.json").get("backup_catalog_required"))


unittest.main()
