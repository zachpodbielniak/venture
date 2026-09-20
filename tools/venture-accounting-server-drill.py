#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Authenticated synthetic Accounting walkthrough against a supplied DEBUG server."""
import argparse
import copy
import hashlib
import http.cookiejar
import json
import os
from pathlib import Path
import re
import secrets
import socket
import subprocess
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request


class RefuseRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, response, code, message, headers, url):
        return None


class Client:
    def __init__(self, origin):
        self.origin = origin
        self.cookies = http.cookiejar.CookieJar()
        self.opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), urllib.request.HTTPCookieProcessor(self.cookies), RefuseRedirect())

    def request(self, path, values=None, method=None, form=False, expected=None, text=False, headers=None):
        body = values if isinstance(values, bytes) else (None if values is None else (urllib.parse.urlencode(values).encode() if form else json.dumps(values).encode()))
        request = urllib.request.Request(self.origin + path, body, method=method)
        if body is not None:
            request.add_header("Content-Type", "application/x-www-form-urlencoded" if form else "application/json")
            request.add_header("Origin", self.origin)
        for name, value in (headers or {}).items():
            request.add_header(name, value)
        try:
            response = self.opener.open(request, timeout=20)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            status, content = response.status, response.read(32 * 1024 * 1024 + 1)
        assert len(content) <= 32 * 1024 * 1024, "HTTP response exceeds bounded fixture limit"
        if expected is not None:
            assert status in expected, f"{method or ('POST' if body is not None else 'GET')} {path}: HTTP {status}: {content[:1200].decode(errors='replace')}"
        else:
            assert 200 <= status < 300, f"{path}: HTTP {status}: {content[:1200].decode(errors='replace')}"
        if status in (302, 303):
            return None
        if text:
            return content.decode("utf-8")
        result = json.loads(content) if content else None
        return result.get("data", result) if isinstance(result, dict) else result

    def login(self, username, password):
        self.request("/login", {"username": username, "password": password}, form=True, expected=(302, 303))
        assert any(cookie.name == "venture_session" for cookie in self.cookies), "Login did not issue a session"

    def create(self, type_name, org=1, **values):
        return self.request("/api/v1/" + type_name, {"organization_id": org, **values})

    def rows(self, name, org=1, **filters):
        query = urllib.parse.urlencode({"organization_id": org, "limit": 10000, **filters})
        return self.request("/api/v1/" + name + "?" + query)["records"]

    def action(self, type_name, identity, action, **values):
        return self.request(f"/api/v1/{type_name}/{identity}/actions/{action}", values)

    def report(self, name, org=1):
        return self.request(f"/api/v1/reports/{name}?period=2026-01&organization_id={org}&currency=USD")


class Server:
    def __init__(self, binary, directory, environment=None):
        self.binary, self.directory = binary, directory
        self.environment = dict(environment or {})
        self.directory.mkdir(mode=0o700)
        self.password = None
        self.process = None
        self.secret = secrets.token_hex(32)

    def start(self):
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            port = listener.getsockname()[1]
        self.origin = f"http://127.0.0.1:{port}"
        config = self.directory / "config.yaml"
        config.write_text("server:\n  bind_address: 127.0.0.1\n  port: " + str(port) +
                          "\nsecurity:\n  require_auth: true\ndatabase:\n  uri: " +
                          json.dumps("sqlite://" + str(self.directory / "workspace.db")) + "\n")
        config.chmod(0o600)
        environment = {key: value for key, value in os.environ.items() if not key.startswith("VENTURE_")}
        environment.update(VENTURE_SESSION_SECRET=self.secret, XDG_CONFIG_HOME=str(self.directory / "config-home"),
                           XDG_DATA_HOME=str(self.directory / "data-home"))
        environment.update(self.environment)
        self.log = (self.directory / "server.log").open("ab")
        self.process = subprocess.Popen([str(self.binary), "--config", str(config), "--state-dir", str(self.directory / "state"),
                                         "--no-ai", "--no-plugins", "--no-automation"],
                                        cwd=self.binary.parents[2], env=environment, stdout=self.log, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 90
        self.client = Client(self.origin)
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise RuntimeError("Server exited; inspect retained private server.log")
            try:
                self.client.request("/api/v1/health")
                break
            except (OSError, ValueError):
                time.sleep(0.1)
        else:
            raise RuntimeError("Server startup exceeded 90 seconds")
        self.client.request("/api/v1/company", expected=(401,))
        if self.password is None:
            matches = re.findall(r"^\s*Password:\s*(\S+)\s*$", (self.directory / "server.log").read_text(), re.M)
            assert matches, "First-run owner password was not found in private server log"
            self.password = matches[0]
        self.client.login("owner", self.password)
        return self.client

    def stop(self):
        if self.process is not None and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=10)
        if getattr(self, "log", None):
            self.log.close()


def cents(value):
    assert isinstance(value, dict) and value["currency"] == "USD" and value["exponent"] == 2, value
    assert isinstance(value["amount"], int), value
    return value["amount"]


def totals(client, org=1):
    result = {}
    for name in ("income_statement", "balance_sheet", "account_balances"):
        result[name] = {row["key"]: cents(row["closing" if name == "account_balances" else "current"])
                        for row in client.report(name, org)["rows"]}
    return result


def assert_books(client, expected, org=1):
    actual = totals(client, org)
    for report, rows in expected.items():
        for key, amount in rows.items():
            assert actual[report].get(key) == amount, f"{report}.{key}: {actual[report].get(key)} != independently expected {amount} cents"
    return actual


OPENING = {"source": "quickbooks", "cutoff": "2026-01-01", "currency": "USD",
           "customers": [{"source_id": "accept-customer", "name": "Opening customer"}],
           "vendors": [{"source_id": "accept-vendor", "name": "Opening supplier"}],
           "open_ar": [{"source_id": "accept-invoice", "customer_source_id": "accept-customer", "number": "OPEN-AR",
                        "date": "2025-12-15", "net": "100 USD", "tax": "5 USD"}],
           "open_ap": [{"source_id": "accept-bill", "vendor_source_id": "accept-vendor", "number": "OPEN-AP",
                        "date": "2025-12-16", "lines": [{"description": "Prior period materials", "amount": "50", "tax": "0"}]}],
           "bank_balances": [{"source_id": "accept-bank", "name": "Opening checking", "account_code": "1000", "amount": "500 USD"}],
           "trial_balance": [{"account_code": "1000", "debit": "500 USD"}, {"account_code": "1100", "debit": "105 USD"},
                             {"account_code": "2000", "credit": "50 USD"}, {"account_code": "3000", "credit": "555 USD"}]}
TRADING_EXPECTED = {"income_statement": {"income": 10000, "expenses": 5000, "net_income": 5000},
                    "balance_sheet": {"assets": 5500, "difference": 0},
                    "account_balances": {"1000": -1000, "1100": 6500, "2000": 0, "2100": -500, "4000": -10000}}


def opening(client):
    batch = client.request("/api/v1/accounting_cutovers/preview", OPENING)
    client.action("accounting_cutover", batch["id"], "import")
    batch = client.action("accounting_cutover", batch["id"], "reconcile")
    assert "Trial balance ties" in batch["reconciliation_report"]
    assert_books(client, {"account_balances": {"1000": 50000, "1100": 10500, "2000": -5000,
                                               "3000": -55500, "3900": 0, "4000": 0, "2100": 0}})
    again = client.request("/api/v1/accounting_cutovers/preview", OPENING)
    client.action("accounting_cutover", again["id"], "import")
    invoices, bills = client.rows("invoice"), client.rows("vendor_bill")
    assert len(invoices) == len(bills) == 1
    client.create("payment", customer_id=invoices[0]["company_id"], invoice_id=invoices[0]["id"], method="transfer", amount="105 USD", date="2026-01-05")
    client.create("bill_payment", vendor_id=bills[0]["company_id"], bill_id=bills[0]["id"], method="transfer", amount="50 USD", date="2026-01-06")
    expected = {"income_statement": {"net_income": 0}, "balance_sheet": {"difference": 0},
                "account_balances": {"1000": 55500, "1100": 0, "2000": 0, "4000": 0, "2100": 0}}
    assert_books(client, expected)
    print("PASS opening import reconciles/replays once; settled cash55500 AR0 AP0 January profit0 and tax0 cents", flush=True)
    return expected


def trading(client):
    customer = client.create("company", name="Synthetic trading customer")
    vendor = client.create("company", name="Synthetic trading supplier", kind="supplier")
    year = client.create("fiscal_year", name="FY2026", start_at="2026-01-01", end_at="2027-01-01", period_length="monthly")
    periods = [row for row in client.rows("fiscal_period") if row["fiscal_year_id"] == year["id"] and row["start_at"].startswith("2026-01-01")]
    assert len(periods) == 1, "Fiscal-year setup must generate exactly one January period"
    period = periods[0]
    invoice = client.create("invoice", number="ACC-1", company_id=customer["id"], issued_at="2026-01-10")
    client.create("invoice_line", invoice_id=invoice["id"], description="Synthetic work", quantity=1, tax_percent=5, unit_price="100 USD")
    client.request(f'/api/v1/invoice/{invoice["id"]}', {"status": "sent"}, method="PATCH")
    receipt = client.create("payment", customer_id=customer["id"], invoice_id=invoice["id"], method="transfer", amount="40 USD", date="2026-01-20")
    bill = client.create("vendor_bill", number="B-1", company_id=vendor["id"], status="draft", currency="USD", bill_date="2026-01-12")
    client.create("vendor_bill_line", bill_id=bill["id"], description="Synthetic parts", quantity="1", unit_price="50 USD")
    client.create("vendor_bill_event", bill_id=bill["id"], vendor_id=vendor["id"], kind="approve", state="approved", date="2026-01-12")
    payment = client.create("bill_payment", vendor_id=vendor["id"], bill_id=bill["id"], method="transfer", amount="50 USD", date="2026-01-22")
    assert_books(client, TRADING_EXPECTED)
    accounts = {row["code"]: row["id"] for row in client.rows("account")}
    bank = client.create("bank_account", name="Synthetic checking", account_id=accounts["1000"], currency="USD", date_column="Date",
                         amount_column="Amount", description_column="Memo", reference_column="Ref", external_id_column="ID", date_format="%Y-%m-%d", sign_convention="normal")
    statement = client.request(f'/api/v1/bank_accounts/{bank["id"]}/import',
                               {"period_start": "2026-01-01", "period_end": "2026-01-31", "opening_balance": "0 USD", "closing_balance": "-10 USD",
                                "format": "csv", "data": "Date,Amount,Memo,Ref,ID\n2026-01-20,40,Customer receipt,ACC-1,accept-receipt\n2026-01-22,-50,Supplier payment,B-1,accept-payment\n"})
    client.request(f'/api/v1/bank_statements/{statement["id"]}/reconcile', {}, expected=(400, 409, 422))
    for transaction in client.rows("bank_transaction"):
        source = receipt if transaction["external_id"] == "accept-receipt" else payment
        kind = "payment" if transaction["external_id"] == "accept-receipt" else "bill_payment"
        client.request(f'/api/v1/bank_transactions/{transaction["id"]}/match', {"parts": [{"type": kind, "id": source["id"]}]})
    reconciled = client.request(f'/api/v1/bank_statements/{statement["id"]}/reconcile', {})
    assert cents(reconciled["difference"]) == 0
    print("PASS trading invoice10500 receipt4000 bill5000 payment5000; cash-1000 AR6500 tax500 profit5000 cents; explicit bank matching reconciles zero", flush=True)
    return period, accounts, customer


def close_month(client, period, accounts, customer):
    password = secrets.token_urlsafe(24)
    client.request("/users", {"username": "synthetic-reviewer", "password": password, "role": "owner"}, form=True, expected=(302, 303))
    reviewer = Client(client.origin)
    reviewer.login("synthetic-reviewer", password)
    workspace = client.action("fiscal_period", period["id"], "open_close", currency="USD")
    identity = workspace["id"]
    workspace = client.action("close_workspace", identity, "run_checks")
    assert workspace["subledger_tied"]
    tasks = [row for row in client.rows("close_task") if row["workspace_id"] == identity]
    assert tasks
    for task in tasks:
        client.action("close_task", task["id"], "complete", notes="Synthetic source totals and bank matches independently checked")
    client.action("close_workspace", identity, "sign", role="preparer")
    client.request(f"/api/v1/close_workspace/{identity}/actions/sign", {"role": "reviewer"}, expected=(400, 403, 409, 422))
    reviewer.action("close_workspace", identity, "sign", role="reviewer")
    client.action("close_workspace", identity, "complete")
    assert "trial_balance" in client.request(f"/api/v1/close/{identity}/pack")
    prior_payments = client.rows("payment")
    client.request("/api/v1/payment", {"organization_id": 1, "customer_id": customer["id"], "method": "transfer", "amount": "1 USD", "date": "2026-01-30"}, expected=(409, 422))
    assert_books(client, TRADING_EXPECTED)
    assert client.rows("payment") == prior_payments
    reviewer.action("close_workspace", identity, "reopen")
    before = totals(client)["account_balances"]["6900"]
    client.request("/api/v1/journals/post", {"organization_id": 1, "source_type": "organization", "source_id": 1,
                    "occurred_at": "2026-01-31", "currency": "USD", "memo": "Reclassify five dollars of supplier expense",
                    "lines": [{"account_id": accounts["6400"], "side": "debit", "amount": "5 USD"},
                              {"account_id": accounts["6900"], "side": "credit", "amount": "5 USD"}]})
    balances = assert_books(client, TRADING_EXPECTED)["account_balances"]
    assert balances["6400"] == 500 and balances["6900"] == before - 500
    client.action("close_workspace", identity, "run_checks")
    client.request(f"/api/v1/close_workspace/{identity}/actions/complete", {}, expected=(400, 409, 422))
    client.action("close_workspace", identity, "sign", role="preparer")
    reviewer.action("close_workspace", identity, "sign", role="reviewer")
    client.action("close_workspace", identity, "complete")
    print("PASS distinct authenticated preparer/reviewer; self-review refused; closed-date receipt refused; balanced correction and fresh signatures preserve profit5000 cents", flush=True)


def roundtrip(source, destination, expected):
    original = totals(source)
    exported = source.request("/api/v1/accounting_backups/export", {})
    archive = json.loads(exported["payload"])
    assert archive["version"] == 4
    (destination.directory.parent / (destination.directory.name + "-source.json")).write_text(json.dumps(archive, indent=2))
    target = destination.start()
    organization = target.create("organization", name="Independent restore destination", default_currency="USD")
    org = organization["id"]
    placeholder = target.request("/api/v1/accounting_backups/export", {"organization_id": org}, form=True)
    target.action("accounting_backup", placeholder["id"], "restore", payload=exported["payload"])
    manifests = [row for row in target.rows("accounting_backup", org) if row["state"] == "restored"]
    assert len(manifests) == 1
    mapping = json.loads(manifests[0]["payload"])["identities"]
    identities = {(item["type"], item["source_id"]): item["id"] for item in mapping}
    assert len(identities) == len(archive["records"]) == len(mapping)
    assert len({(item["type"], item["id"]) for item in mapping}) == len(mapping)
    restored = target.request("/api/v1/accounting_backups/export", {"organization_id": org}, form=True)
    restored_archive = json.loads(restored["payload"])
    (destination.directory.parent / (destination.directory.name + "-restored.json")).write_text(json.dumps(restored_archive, indent=2))
    assert len(restored_archive["records"]) == len(archive["records"])
    restored_rows = {(entry["type"], entry["record"]["id"]): entry["record"] for entry in restored_archive["records"]}
    schemas = {}
    references = 0
    uuid_mapping = {entry["record"]["uuid"]: restored_rows[(entry["type"], identities[(entry["type"], entry["record"]["id"])])]["uuid"]
                    for entry in archive["records"]}
    for entry in archive["records"]:
        kind, old = entry["type"], copy.deepcopy(entry["record"])
        current = copy.deepcopy(restored_rows[(kind, identities[(kind, old["id"])])])
        assert current["organization_id"] == org
        if kind not in schemas:
            schemas[kind] = source.request("/api/v1/schema/" + kind)
        fields = schemas[kind]["fields"]
        for field in fields:
            key = field["name"].replace("-", "_")
            reference = field.get("references")
            for stem in ("source", "record", "subject", "correction"):
                if key == stem + "_id" and isinstance(old.get(key), int) and old.get(stem + "_type"):
                    reference = old[stem + "_type"]
            if reference and old.get(key):
                value = old[key]
                if reference == "organization" and value == 1:
                    old[key] = org
                else:
                    assert (reference, value) in identities, (kind, key, reference, value)
                    old[key] = identities[(reference, value)]
                references += 1
        if kind == "ledger_entry" and old.get("transaction_id"):
            old["transaction_id"] = uuid_mapping[old["transaction_id"]]
        if kind == "journal" and old.get("posting_key"):
            old["posting_key"] = str(org) + ":" + old["posting_key"].removeprefix("1:")
        if kind == "bank_transaction" and old.get("external_key"):
            old["external_key"] = f"{org}:{old['bank_account_id']}:{old['external_id']}"
        for key in {"id", "uuid", "organization_id", "display_name"}:
            old.pop(key, None)
            current.pop(key, None)
        assert old == current, (kind, {key: (old.get(key), current.get(key)) for key in old.keys() | current.keys() if old.get(key) != current.get(key)})
    assert references > 0
    print(f"PASS all historical fields and {references} remapped references match independently", flush=True)
    assert_books(target, expected, org)
    assert totals(target, org) == original
    print(f"PASS independent server restored {len(mapping)} unique record identities; every statement/account total agrees in cents", flush=True)
    destination.stop()
    target = destination.start()
    assert totals(target, org) == original
    after_restart = target.request("/api/v1/accounting_backups/export", {"organization_id": org}, form=True)
    assert json.loads(after_restart["payload"])["records"] == restored_archive["records"]
    print("PASS restored server restart preserves all historical fields/references and statement/account totals", flush=True)


def main():
    if not __debug__:
        raise SystemExit("Run without Python -O: acceptance assertions are mandatory")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--scenario", choices=("opening", "trading", "all"), default="all")
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    root = Path(tempfile.mkdtemp(prefix="venture-accounting-server-drill-"))
    os.umask(0o077)
    fingerprint = hashlib.sha256(binary.read_bytes()).hexdigest()
    print("Artifact SHA256: " + fingerprint, flush=True)
    print("Evidence directory: " + str(root), flush=True)
    for name in ("opening", "trading"):
        if args.scenario not in ("all", name):
            continue
        server = Server(binary, root / name)
        try:
            client = server.start()
            if name == "opening":
                expected = opening(client)
            else:
                close_month(client, *trading(client))
                expected = TRADING_EXPECTED
            source_totals = totals(client)
            server.stop()
            client = server.start()
            assert totals(client) == source_totals
            print("PASS source restart preserves all statement/account totals", flush=True)
            destination = Server(binary, root / (name + "-restore"))
            try:
                roundtrip(client, destination, expected)
                assert hashlib.sha256(binary.read_bytes()).hexdigest() == fingerprint, "Artifact changed during walkthrough"
            finally:
                destination.stop()
        finally:
            server.stop()


if __name__ == "__main__":
    main()
