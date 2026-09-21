#!/usr/bin/env python3
"""Exercise populated released-to-candidate SQLite or disposable PostgreSQL upgrades."""

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
import urllib.parse
import uuid


class RefuseRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, response, code, message, headers, url):
        return None


def request(origin, path, values=None, method=None):
    body = None if values is None else json.dumps(values).encode()
    req = urllib.request.Request(origin + path, data=body, method=method)
    if body is not None:
        req.add_header("Content-Type", "application/json")
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), RefuseRedirect())
    with opener.open(req, timeout=10) as response:
        content = response.read(32 * 1024 * 1024 + 1)
        if len(content) > 32 * 1024 * 1024:
            raise RuntimeError("HTTP fixture response exceeds bounded limit")
        result = json.loads(content)
    return result.get("data", result)


class Server:
    def __init__(self, binary, directory, database=None):
        self.database = database
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
            + json.dumps(self.database.uri if self.database else "sqlite://" + str(self.directory / "workspace.db"))
            + "\n"
        )
        config.chmod(0o600)
        environment = {
            k: v for k, v in os.environ.items() if not k.startswith(("VENTURE_", "PG"))
        }
        environment.update(XDG_CONFIG_HOME=str(self.directory / "config-home"), XDG_DATA_HOME=str(self.directory / "data-home"))
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


class PostgreSQL:
    """Only this invocation's fresh databases are copied, inspected or removed."""
    def __init__(self, service_uri):
        parsed = urllib.parse.urlsplit(service_uri)
        if parsed.scheme not in ("postgres", "postgresql") or not parsed.hostname or not parsed.username or not parsed.path.strip("/") or parsed.query or parsed.fragment:
            raise ValueError("Use an explicit disposable PostgreSQL service URI without query options")
        self.environment = {key: value for key, value in os.environ.items() if not key.startswith("PG")}
        self.environment.update(PGHOST=parsed.hostname, PGPORT=str(parsed.port or 5432), PGUSER=urllib.parse.unquote(parsed.username),
                                PGPASSWORD=urllib.parse.unquote(parsed.password or ""), PGDATABASE=parsed.path.lstrip("/"),
                                PGCONNECT_TIMEOUT="5", PGPASSFILE="/dev/null", PGSERVICEFILE="/dev/null", PGOPTIONS="-c statement_timeout=120000 -c lock_timeout=10000")
        self.service_uri = service_uri
        self.name = "venture_upgrade_" + uuid.uuid4().hex
        self.uri = urllib.parse.urlunsplit((parsed.scheme, parsed.netloc, "/" + self.name, "", ""))
        self.owned = False

    def sql(self, sql, administrative=False):
        environment = dict(self.environment)
        if not administrative:
            if not self.owned:
                raise RuntimeError("Refusing access to an unowned fixture database")
            environment["PGDATABASE"] = self.name
        result = subprocess.run(["psql", "-X", "-w", "-q", "-A", "-t", "--set", "ON_ERROR_STOP=1", "--command", sql],
                                env=environment, capture_output=True, text=True, timeout=130)
        if result.returncode:
            raise RuntimeError("Private PostgreSQL fixture command failed: " + result.stderr.strip())
        return result.stdout.strip()

    def create(self, source=None):
        if self.owned:
            raise RuntimeError("Fixture database already exists")
        template = "template0"
        if source is not None:
            if not source.owned or source.service_uri != self.service_uri:
                raise RuntimeError("Only this invocation's owned stopped baseline may be cloned")
            template = source.name
        self.sql("CREATE DATABASE " + quote(self.name) + " TEMPLATE " + quote(template), administrative=True)
        self.owned = True

    def remove(self):
        if self.owned:
            self.sql("DROP DATABASE " + quote(self.name), administrative=True)
            self.owned = False

    def snapshot(self, columns=None):
        if columns is None:
            columns = {}
            for table in TABLES:
                rows = json.loads(self.sql("SELECT COALESCE(json_agg(column_name ORDER BY ordinal_position),'[]') FROM information_schema.columns WHERE table_schema='public' AND table_name='" + table + "'"))
                if not rows:
                    raise RuntimeError("Expected baseline table absent: " + table)
                columns[table] = rows
        rows = {table: json.loads(self.sql("SELECT COALESCE(json_agg(row_to_json(t)),'[]') FROM (SELECT " +
                                        ",".join(map(quote, fields)) + " FROM " + quote(table) + " ORDER BY id) t"))
                for table, fields in columns.items()}
        history = json.loads(self.sql("SELECT COALESCE(json_agg(row_to_json(t)),'[]') FROM (SELECT version,name,checksum FROM schema_migrations ORDER BY version) t"))
        return columns, rows, history


TABLES = ("companies", "accounts", "journals", "journal_lines", "ledger_entries", "invoices", "invoice_lines",
          "payments", "payment_allocations", "invoice_events", "forges", "forge_repos")


def financials(server):
    reports = {}
    for name in ("income_statement", "balance_sheet", "account_balances"):
        report = request(server.origin, f"/api/v1/reports/{name}?period=2026-01&organization_id=1&currency=USD")
        values = {}
        for row in report["rows"]:
            money = row["closing" if name == "account_balances" else "current"]
            if money["currency"] != "USD" or money["exponent"] != 2 or type(money["amount"]) is not int:
                raise RuntimeError("Financial report did not return integer USD cents")
            values[row["key"]] = money["amount"]
        reports[name] = values
    expected = {"account_balances": {"UP-CASH": 12345, "UP-EQUITY": -12345, "1000": 4000, "1100": 6500, "2100": -500, "4000": -10000},
                "income_statement": {"income": 10000, "net_income": 10000}, "balance_sheet": {"difference": 0}}
    for name, values in expected.items():
        for key, amount in values.items():
            if reports[name].get(key) != amount:
                raise RuntimeError(f"Independent financial expectation failed: {name}.{key}={reports[name].get(key)} expected {amount} cents")
    return reports


def snapshot(path, columns=None):
    with sqlite3.connect(path) as db:
        if columns is None:
            columns = {}
            for table in TABLES:
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
    parser.add_argument("--postgres-uri-env", metavar="NAME", help="Explicit environment variable containing a disposable PostgreSQL service URI; requires CREATEDB and psql")
    args = parser.parse_args()
    old_binary = args.baseline_binary.resolve(strict=True)
    new_binary = args.candidate_binary.resolve(strict=True)
    os.umask(0o077)
    service_uri = os.environ.get(args.postgres_uri_env) if args.postgres_uri_env else None
    if args.postgres_uri_env and not service_uri:
        parser.error("The explicitly selected PostgreSQL URI environment variable is empty")
    old_database = PostgreSQL(service_uri) if service_uri else None
    new_database = PostgreSQL(service_uri) if service_uri else None
    fingerprints = {"baseline": hashlib.sha256(old_binary.read_bytes()).hexdigest(), "candidate": hashlib.sha256(new_binary.read_bytes()).hexdigest()}
    root = Path(tempfile.mkdtemp(prefix="venture-upgrade-drill-"))
    old = root / "released"
    new = root / "candidate"
    old.mkdir(mode=0o700)
    try:
        if old_database:
            old_database.create()
        with Server(old_binary, old, old_database) as server:
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
            invoice = server.create("invoice", number="UP-AR", company_id=company["id"], issued_at="2026-01-11")
            server.create("invoice_line", invoice_id=invoice["id"], description="Upgrade sale", quantity=1, unit_price="100 USD", tax_percent=5)
            request(server.origin, f"/api/v1/invoice/{invoice['id']}", {"status": "sent"}, method="PATCH")
            server.create("payment", customer_id=company["id"], invoice_id=invoice["id"], amount="40 USD", method="transfer", date="2026-01-15")
            before_financials = financials(server)
            forge = server.create(
                "forge",
                name="Upgrade forge",
                base_url="https://git.example.test",
                active=True,
            )
            server.create("forge_repo", name="fixture/project", forge_id=forge["id"])
        columns, before, history = old_database.snapshot() if old_database else snapshot(old / "workspace.db")
        shutil.copytree(old, new)
        if new_database:
            new_database.create(old_database)
        with Server(new_binary, new, new_database) as server:
            retained = request(server.origin, "/api/v1/company/" + str(company["id"]))
            after_financials = financials(server)
            if before_financials != after_financials:
                raise RuntimeError("Complete financial reports changed across upgrade")
            if retained["name"] != "Upgrade customer":
                raise RuntimeError("Company identity changed")
        _, after, new_history = new_database.snapshot(columns) if new_database else snapshot(new / "workspace.db", columns)
        if before != after:
            raise RuntimeError("Retained financial/business row values changed")
        if new_history[: len(history)] != history or len(new_history) <= len(history):
            raise RuntimeError(
                "Migration history is not a strict append-only extension"
            )
        if fingerprints != {"baseline": hashlib.sha256(old_binary.read_bytes()).hexdigest(), "candidate": hashlib.sha256(new_binary.read_bytes()).hexdigest()}:
            raise RuntimeError("An executable changed during qualification")
        if new_database:
            new_database.remove()
            old_database.remove()
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
            "financial_reports_equal": True,
            "partial_receipt": "invoice10500 receipt4000 AR6500 tax500 income10000 USD cents",
            "backend": "PostgreSQL" if service_uri else "SQLite",
            "disposable_databases_removed": bool(service_uri),
            "scope": "Released-history upgrade; authentication disabled in isolated fixture; not hosted capacity or full replacement acceptance",
        }
        print(json.dumps(result, indent=2))
    except BaseException:
        print("Upgrade failed; private evidence retained at " + str(root), flush=True)
        for database in (old_database, new_database):
            if database and database.owned:
                print("Retained owned fixture database: " + database.name, flush=True)
        raise


if __name__ == "__main__":
    main()
