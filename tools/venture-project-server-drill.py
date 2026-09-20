#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Authenticated synthetic CRM-to-project acceptance against a supplied DEBUG server."""
import argparse
from datetime import date, datetime, timedelta, timezone
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import sys

sys.dont_write_bytecode = True

spec = importlib.util.spec_from_file_location("accounting_drill", Path(__file__).with_name("venture-accounting-server-drill.py"))
accounting = importlib.util.module_from_spec(spec)
spec.loader.exec_module(accounting)
Server, cents = accounting.Server, accounting.cents
TODAY = datetime.now(timezone.utc).date().isoformat()


def report(client, name):
    return client.request(f"/api/v1/reports/{name}?organization_id=1&currency=USD")["rows"]


def margin(client, project, expected):
    rows = [row for row in report(client, "project_margin") if int(row["project_id"]) == project["id"]]
    assert len(rows) == 1
    row = rows[0]
    for key, amount in expected.items():
        assert cents(row[key]) == amount, (key, row[key], amount)
    assert int(row["unknown_cost"]) == 0 and int(row["undated"]) == 0
    billing = [r for r in client.rows("project_billing") if r["project_id"] == project["id"]]
    assert sum(cents(r["amount"]) for r in billing) == cents(row["billed"])
    expected_sources = {f"{kind}:{r['id']}" for kind in ("project_time", "project_cost", "project_billing")
                        for r in client.rows(kind) if r["project_id"] == project["id"]}
    actual_sources = set(row["sources"].split(", ")) if row["sources"] else set()
    assert expected_sources <= actual_sources, (expected_sources, actual_sources)
    for token in actual_sources:
        kind, identity = token.split(":")
        assert kind in {"project_time", "project_cost", "project_billing", "project_deliverable"}
        assert client.request(f"/api/v1/{kind}/{identity}")["project_id"] == project["id"]
    for allocation in billing:
        invoice = client.request(f"/api/v1/invoice/{allocation['invoice_id']}")
        assert invoice["company_id"] == project["customer_id"]
        source = client.request(f"/api/v1/{allocation['source_type']}/{allocation['source_id']}")
        if allocation["source_type"] == "progress_billing":
            assert source["quote_id"] in {scope["quote_id"] for scope in scopes(client, project)}
            assert source["invoice_id"] == invoice["id"]
        else:
            assert source["project_id"] == project["id"]
    return row


def prospect(client, name):
    lead = client.create("lead", name=name + " Buyer", company_name=name + " Company", email=name.lower().replace(" ", "-") + "@example.invalid", status="qualified")
    converted = client.request(f"/api/v1/leads/{lead['id']}/convert", {"deal": True})
    assert converted["converted_company_id"] and converted["converted_contact_id"] and converted["converted_deal_id"]
    client.request(f"/api/v1/leads/{lead['id']}/convert", {"deal": True}, expected=(409,))
    assert len(client.rows("deal")) == 1
    deal = client.request(f"/api/v1/deal/{converted['converted_deal_id']}", {"value": "1000 USD"}, method="PATCH")
    won = next(row for row in client.rows("pipeline_stage") if row["pipeline_id"] == deal["pipeline_id"] and row["kind"] == "won")
    deal = client.request(f"/api/v1/deals/{deal['id']}/move", {"stage_id": won["id"], "note": "Synthetic customer accepted commercial terms"})
    return deal, converted["converted_company_id"]


def accepted_quote(client, customer, number, amount="1000 USD", mode="progress", deal=0):
    quote = client.create("quote", number=number, company_id=customer, currency="USD", billing_mode=mode, deal_id=deal)
    client.create("quote_line", quote_id=quote["id"], description="Synthetic agreed delivery", quantity=1, unit_price=amount)
    client.request(f"/api/v1/quotes/{quote['id']}/send", {})
    client.request(f"/api/v1/quotes/{quote['id']}/accept", {"accepted_by": "Synthetic customer authorized staff record"})
    return client.request(f"/api/v1/quote/{quote['id']}")


def plan(client, project, scope, key, amount=None):
    params = {"key": key, "title": "Deliver " + key, "scope_id": scope["id"], "due": TODAY}
    if amount:
        params["amount"] = amount
    delivery = client.action("client_project", project["id"], "plan_work", **params)
    assert client.action("client_project", project["id"], "plan_work", **params)["id"] == delivery["id"]
    client.request(f"/api/v1/project_deliverable/{delivery['id']}/actions/accept", {"evidence": "Premature"}, expected=(400, 409, 422))
    client.request(f"/api/v1/ticket/{delivery['ticket_id']}", {"status": "done"}, method="PATCH")
    return client.action("project_deliverable", delivery["id"], "accept", evidence="Synthetic customer accepted completed work")


def labor(client, project, minutes=60):
    rate = client.create("project_rate", project_id=project["id"], role="Engineer", billing_rate="150 USD", cost_rate="80 USD")
    entry = client.create("project_time", project_id=project["id"], rate_id=rate["id"], minutes=minutes, occurred_at=TODAY)
    entry = client.action("project_time", entry["id"], "approve")
    assert cents(entry["actual_cost"]) == minutes * 8000 // 60
    client.request(f"/api/v1/project_rate/{rate['id']}", {"cost_rate": "999 USD"}, method="PATCH")
    assert cents(client.request(f"/api/v1/project_time/{entry['id']}")["actual_cost"]) == minutes * 8000 // 60
    return entry


def receive(client, customer, invoice, amount):
    before = client.rows("payment")
    client.request("/api/v1/payment", {"organization_id": 1, "customer_id": customer, "invoice_id": invoice["id"],
                   "method": "transfer", "amount": "1 USD", "date": (date.fromisoformat(TODAY) - timedelta(days=1)).isoformat()},
                   expected=(400, 409, 422))
    assert client.rows("payment") == before
    paid_at = datetime.now(timezone.utc).isoformat() if client.precise_receipts else TODAY
    return client.create("payment", customer_id=customer, invoice_id=invoice["id"], method="transfer", amount=amount, date=paid_at)


def scopes(client, project):
    return [row for row in client.rows("project_scope") if row["project_id"] == project["id"]]


def time_materials(client):
    deal, customer = prospect(client, "Time engagement")
    project = client.action("deal", deal["id"], "handoff", name="Time engagement", owner="owner", scope="Approved engineering and pass-through materials")
    assert project["billing_kind"] == "time"
    plan(client, project, scopes(client, project)[0], "engineering")
    labor(client, project, 90)
    client.create("project_cost", project_id=project["id"], amount="25 USD", billable=True, occurred_at=TODAY)
    client.create("project_cost", project_id=project["id"], amount="10 USD", billable=False, occurred_at=TODAY)
    margin(client, project, {"budget": 100000, "billed": 0, "unbilled": 25000, "actual_cost": 15500, "profit": -15500})
    invoice = client.action("client_project", project["id"], "bill", date=TODAY)
    margin(client, project, {"billed": 25000, "unbilled": 0, "actual_cost": 15500, "profit": 9500})
    client.request(f"/api/v1/client_project/{project['id']}/actions/bill", {"date": TODAY}, expected=(400, 409, 422))
    receive(client, customer, invoice, "100 USD")
    assert client.request(f"/api/v1/invoice/{invoice['id']}")["workflow_state"] == "partially_paid"
    receive(client, customer, invoice, "150 USD")
    labor(client, project, 60)
    second = client.action("client_project", project["id"], "bill", date=TODAY)
    assert second["id"] != invoice["id"]
    receive(client, customer, second, "150 USD")
    margin(client, project, {"billed": 40000, "unbilled": 0, "actual_cost": 23500, "profit": 16500})
    client.action("client_project", project["id"], "manage", owner="owner", status="completed", reason="Accepted delivery and all approved work invoiced")
    print("PASS CRM conversion/won deal -> owned scope/ticket/acceptance -> two T&M invoices; billed40000 cost23500 profit16500 cents; frozen cost, duplicate refusal and source links", flush=True)


def fixed(client, full=False):
    deal, customer = prospect(client, "Fixed full" if full else "Fixed progress")
    quote = accepted_quote(client, customer, "FULL-1" if full else "FIXED-1", mode="full" if full else "progress", deal=deal["id"])
    project = client.action("quote", quote["id"], "handoff", name="Fixed engagement", owner="owner", scope="Contracted launch")
    assert project["billing_kind"] == "fixed"
    original_scope = scopes(client, project)[0]
    labor(client, project)
    client.create("project_cost", project_id=project["id"], amount="25 USD", billable=True, occurred_at=TODAY)
    client.request(f"/api/v1/client_project/{project['id']}/actions/bill", {"date": TODAY}, expected=(400, 409, 422))
    if full:
        plan(client, project, original_scope, "full-delivery", "1000 USD")
        invoices = client.rows("invoice")
        assert len(invoices) == 1
        receive(client, customer, invoices[0], "1000 USD")
        margin(client, project, {"billed": 100000, "unbilled": 0, "actual_cost": 10500, "profit": 89500})
        print("PASS full fixed quote invoice retained at handoff; no duplicate labor billing; billed100000 cost10500 profit89500 cents", flush=True)
        return
    first = plan(client, project, original_scope, "design", "400 USD")
    margin(client, project, {"billed": 0, "unbilled": 40000, "actual_cost": 10500, "profit": -10500})
    invoice = client.action("project_deliverable", first["id"], "bill")
    assert client.action("project_deliverable", first["id"], "bill")["id"] == invoice["id"]
    receive(client, customer, invoice, "200 USD")
    assert client.request(f"/api/v1/invoice/{invoice['id']}")["workflow_state"] == "partially_paid"
    receive(client, customer, invoice, "200 USD")
    second = plan(client, project, original_scope, "launch", "600 USD")
    direct = client.action("quote", quote["id"], "progress_invoice", percent=60)
    linked = client.action("project_deliverable", second["id"], "link_invoice", invoice_id=direct["id"])
    assert linked["id"] == direct["id"]
    receive(client, customer, direct, "600 USD")
    change = accepted_quote(client, customer, "CHANGE-1", "200 USD")
    client.action("client_project", project["id"], "change_scope", quote_id=change["id"], scope="Accepted reporting addition")
    client.action("client_project", project["id"], "change_scope", quote_id=change["id"], scope="Accepted reporting addition")
    agreements = scopes(client, project)
    assert len(agreements) == 2
    changed = next(row for row in agreements if row["quote_id"] == change["id"])
    delivery = plan(client, project, changed, "reporting", "200 USD")
    extra = client.action("project_deliverable", delivery["id"], "bill")
    receive(client, customer, extra, "200 USD")
    margin(client, project, {"budget": 120000, "billed": 120000, "unbilled": 0, "actual_cost": 10500, "profit": 109500})
    client.request(f"/api/v1/quote/{quote['id']}/actions/progress_invoice", {"percent": 1}, expected=(400, 409, 422))
    client.action("client_project", project["id"], "manage", owner="owner", status="completed", reason="Both agreements accepted and invoiced once")
    print("PASS fixed400/600 progress + changed scope200; invoice replay/linking and cap refusal; budget120000 billed120000 cost10500 profit109500 cents", flush=True)


def accounting_controls(client, cash, receivable, income, liability=None):
    balances = {row["key"]: cents(row["closing"]) for row in report(client, "account_balances")}
    assert balances["1000"] == cash and balances["1100"] == receivable and balances["4000"] == -income, balances
    if liability is not None:
        assert balances["2200"] == -liability
    sheet = {row["key"]: cents(row["current"]) for row in report(client, "balance_sheet")}
    assert sheet["difference"] == 0


def retainer(client):
    deal, customer = prospect(client, "Retainer engagement")
    project = client.action("deal", deal["id"], "handoff", name="Retained engagement", owner="owner", scope="Separate earned retainer; not an invoice prepayment")
    liability = next(row for row in client.rows("account") if row["code"] == "2200")
    held = client.action("company", customer, "collect_retainer", amount="250 USD", liability_account_id=liability["id"])
    assert cents(held["remaining"]) == 25000
    page = client.request(f"/e/client_project/{project['id']}", text=True)
    assert 'action="/links"' in page and 'data-record-pick' in page and 'value="customer_retainer"' in page
    choices = client.request("/ui/records/search?type=customer_retainer&q=")
    assert any(int(item["id"]) == held["id"] for item in choices["items"])
    client.request("/links", {"source_type": "client_project", "source_id": project["id"], "target_type": "customer_retainer",
                   "target_id": held["id"], "kind": "references", "note": "Explicit earned-retainer agreement link"},
                   form=True, expected=(302, 303))
    links = client.rows("record_link")
    assert len(links) == 1 and links[0]["source_id"] == project["id"] and links[0]["target_id"] == held["id"]
    assert f'/e/customer_retainer/{held["id"]}' in client.request(f"/e/client_project/{project['id']}", text=True)
    accounting_controls(client, 25000, 0, 0, 25000)
    released = client.action("customer_retainer", held["id"], "release", amount="100 USD")
    assert cents(released["remaining"]) == 15000
    accounting_controls(client, 25000, 0, 10000, 15000)
    client.request(f"/api/v1/customer_retainer/{held['id']}/actions/release", {"amount": "151 USD"}, expected=(400, 409, 422))
    assert cents(client.request(f"/api/v1/customer_retainer/{held['id']}")["remaining"]) == 15000
    released = client.action("customer_retainer", held["id"], "release", amount="150 USD")
    assert cents(released["remaining"]) == 0
    client.request(f"/api/v1/customer_retainer/{held['id']}/actions/release", {"amount": "1 USD"}, expected=(400, 409, 422))
    accounting_controls(client, 25000, 0, 25000, 0)
    assert not client.rows("invoice") and not client.rows("payment")
    margin(client, project, {"billed": 0, "unbilled": 0, "actual_cost": 0, "profit": 0})
    print("PASS existing authenticated link form/picker; retainer cash25000 -> liability25000 -> releases10000+15000 revenue25000 cents; over-release refused; no invoice/receipt or project billed allocation invented", flush=True)


def main():
    if not __debug__:
        raise SystemExit("Run without Python -O: acceptance assertions are mandatory")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--precise-receipts", action="store_true", help="Diagnostic timestamp workaround; ordinary date-only receipts remain the acceptance default")
    parser.add_argument("--scenario", choices=("time", "fixed", "full", "retainer", "all"), default="all")
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    os.umask(0o077)
    root = Path(tempfile.mkdtemp(prefix="venture-project-server-drill-"))
    fingerprint = hashlib.sha256(binary.read_bytes()).hexdigest()
    print("Artifact SHA256: " + fingerprint, flush=True)
    print("Evidence directory: " + str(root), flush=True)
    print("Receipt mode: " + ("precise UTC timestamps (diagnostic workaround)" if args.precise_receipts else "ordinary date-only"), flush=True)
    for name, scenario in (("time", time_materials), ("fixed", fixed), ("full", lambda client: fixed(client, True)), ("retainer", retainer)):
        if args.scenario not in ("all", name):
            continue
        server = Server(binary, root / name)
        try:
            client = server.start()
            client.precise_receipts = args.precise_receipts
            scenario(client)
            if name != "retainer":
                accounting_controls(client, {"time": 40000, "fixed": 120000, "full": 100000}[name], 0,
                                    {"time": 40000, "fixed": 120000, "full": 100000}[name])
                assert all(row["workflow_state"] == "paid" for row in client.rows("invoice"))
                print("PASS collected cash equals invoices and project billed allocations; AR zero and balance sheet ties", flush=True)
            before = report(client, "project_margin")
            accounts_before = report(client, "account_balances")
            retained_types = ("client_project", "project_scope", "project_deliverable", "project_time", "project_cost",
                              "project_billing", "customer_retainer", "record_link", "invoice", "payment")
            retained = {kind: client.rows(kind) for kind in retained_types}
            server.stop()
            client = server.start()
            assert report(client, "project_margin") == before
            assert report(client, "account_balances") == accounts_before
            assert {kind: client.rows(kind) for kind in retained_types} == retained
            assert hashlib.sha256(binary.read_bytes()).hexdigest() == fingerprint, "Artifact changed during walkthrough"
            print("PASS project profitability, account balances and retained source records survive authenticated server restart", flush=True)
        finally:
            server.stop()


if __name__ == "__main__":
    main()
