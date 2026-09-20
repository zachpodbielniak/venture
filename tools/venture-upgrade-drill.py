#!/usr/bin/env python3
"""Exercise released-to-candidate SQLite migration using isolated real servers."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import sqlite3
import subprocess
import tempfile
import time
import urllib.error
import urllib.request


def request(origin, path, values=None):
    body = None if values is None else json.dumps(values).encode()
    req = urllib.request.Request(origin + path, data=body)
    if body is not None:
        req.add_header("Content-Type", "application/json")
    with urllib.request.urlopen(req, timeout=10) as response:
        result = json.load(response)
    return result.get("data", result)


class Server:
    def __init__(self, binary, directory):
        self.binary = binary
        self.directory = directory
        self.process = None
        self.log = None

    def __enter__(self):
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            port = listener.getsockname()[1]
        self.origin = f"http://127.0.0.1:{port}"
        config = self.directory / "config.yaml"
        config.write_text(
            "server:\n  bind_address: 127.0.0.1\n  port: "
            + str(port)
            + "\nsecurity:\n  require_auth: false\n"
            + "database:\n  uri: "
            + json.dumps("sqlite://" + str(self.directory / "workspace.db"))
            + "\n"
        )
        config.chmod(0o600)
        environment = {
            k: v for k, v in os.environ.items() if not k.startswith("VENTURE_")
        }
        self.log = (self.directory / "server.log").open("wb")
        self.process = subprocess.Popen(
            [
                str(self.binary),
                "--config",
                str(config),
                "--state-dir",
                str(self.directory / "state"),
                "--no-ai",
                "--no-automation",
                "--no-plugins",
            ],
            env=environment,
            cwd=self.directory,
            stdout=self.log,
            stderr=subprocess.STDOUT,
        )
        deadline = time.monotonic() + 60
        try:
            while time.monotonic() < deadline:
                if self.process.poll() is not None:
                    raise RuntimeError(
                        "Server exited; inspect retained private server.log"
                    )
                try:
                    request(self.origin, "/api/v1/health")
                    return self
                except (OSError, urllib.error.HTTPError, json.JSONDecodeError):
                    time.sleep(0.1)
            raise RuntimeError("Server readiness exceeded 60 seconds")
        except BaseException:
            self.__exit__(None, None, None)
            raise

    def __exit__(self, *_):
        if self.process is not None and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=10)
        if self.log:
            self.log.close()

    def create(self, type_name, **values):
        return request(
            self.origin, "/api/v1/" + type_name, {"organization_id": 1, **values}
        )


def quote(identifier):
    return '"' + identifier.replace('"', '""') + '"'


def snapshot(path, columns=None):
    with sqlite3.connect(path) as db:
        if columns is None:
            columns = {}
            for table in (
                "companies",
                "accounts",
                "journals",
                "journal_lines",
                "forges",
                "forge_repos",
            ):
                fields = [
                    row[1]
                    for row in db.execute("PRAGMA table_info(" + quote(table) + ")")
                ]
                if not fields:
                    raise RuntimeError("Expected baseline table is absent: " + table)
                columns[table] = fields
        rows = {
            table: list(
                db.execute(
                    "SELECT "
                    + ",".join(map(quote, fields))
                    + " FROM "
                    + quote(table)
                    + " ORDER BY id"
                )
            )
            for table, fields in columns.items()
        }
        history = list(
            db.execute(
                "SELECT version, name, checksum FROM schema_migrations ORDER BY version"
            )
        )
    return columns, rows, history


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-binary", type=Path, required=True)
    parser.add_argument("--candidate-binary", type=Path, required=True)
    args = parser.parse_args()
    old_binary = args.baseline_binary.resolve(strict=True)
    new_binary = args.candidate_binary.resolve(strict=True)
    root = Path(tempfile.mkdtemp(prefix="venture-upgrade-drill-"))
    old = root / "released"
    new = root / "candidate"
    old.mkdir(mode=0o700)
    try:
        with Server(old_binary, old) as server:
            company = server.create(
                "company", name="Upgrade customer", is_customer=True
            )
            cash = server.create(
                "account",
                code="UP-CASH",
                name="Upgrade cash",
                kind="asset",
                active=True,
            )
            equity = server.create(
                "account",
                code="UP-EQUITY",
                name="Upgrade equity",
                kind="equity",
                active=True,
            )
            journal = {
                "organization_id": 1,
                "source_type": "organization",
                "source_id": 1,
                "currency": "USD",
                "occurred_at": "2026-01-10",
                "memo": "Upgrade opening balance",
                "lines": [
                    {"account_id": cash["id"], "side": "debit", "amount": "123.45 USD"},
                    {
                        "account_id": equity["id"],
                        "side": "credit",
                        "amount": "123.45 USD",
                    },
                ],
            }
            request(
                server.origin,
                "/api/v1/journal/0/actions/create_and_post",
                {"journal": journal},
            )
            forge = server.create(
                "forge",
                name="Upgrade forge",
                base_url="https://git.example.test",
                active=True,
            )
            server.create("forge_repo", name="fixture/project", forge_id=forge["id"])
        columns, before, history = snapshot(old / "workspace.db")
        shutil.copytree(old, new)
        with Server(new_binary, new) as server:
            retained = request(server.origin, "/api/v1/company/" + str(company["id"]))
            if retained["name"] != "Upgrade customer":
                raise RuntimeError("Company identity changed")
        _, after, new_history = snapshot(new / "workspace.db", columns)
        if before != after:
            raise RuntimeError("Retained financial/business row values changed")
        if new_history[: len(history)] != history or len(new_history) <= len(history):
            raise RuntimeError(
                "Migration history is not a strict append-only extension"
            )
        result = {
            "baseline_binary_sha256": hashlib.sha256(
                old_binary.read_bytes()
            ).hexdigest(),
            "candidate_binary_sha256": hashlib.sha256(
                new_binary.read_bytes()
            ).hexdigest(),
            "baseline_migrations": len(history),
            "candidate_migrations": len(new_history),
            "tables_reconciled": len(before),
            "rows_reconciled": sum(map(len, before.values())),
            "retained_rows_equal": True,
            "posted_journal": "USD 123.45 debit and credit",
            "fixture": str(root),
            "scope": "SQLite released-history upgrade; not hosted capacity or full replacement acceptance",
        }
        print(json.dumps(result, indent=2))
    except BaseException:
        print("Upgrade failed; private evidence retained at " + str(root), flush=True)
        raise


if __name__ == "__main__":
    main()
