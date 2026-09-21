#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Synthetic attribution and independent email-consent journey against a DEBUG server."""
import argparse
import base64
from datetime import datetime, timezone
import hashlib
import hmac
import importlib.util
import json
import os
from pathlib import Path
import secrets
import shutil
import tempfile
import time

spec = importlib.util.spec_from_file_location("accounting_drill", Path(__file__).with_name("venture-accounting-server-drill.py"))
accounting = importlib.util.module_from_spec(spec)
spec.loader.exec_module(accounting)

SITE_ORIGIN = "https://synthetic-site.example.test"
POLICY = "synthetic-email-v1"
STATEMENT = "I agree to receive synthetic demonstration marketing email."


def moment():
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def attribution(client, model="first"):
    return client.request(f"/api/v1/reports/attribution?period=all&organization_id=1&currency=USD&model={model}&details=true")


def row(report, measure):
    found = [item for item in report["rows"] if item["measure"] == measure and item["source"] == "newsletter"]
    assert len(found) == 1, (measure, found)
    return found[0]


def financial_report(client, invoice, payment, campaign):
    events = client.rows("invoice_event", invoice_id=invoice["id"], kind="issue")
    allocations = client.rows("payment_allocation", payment_id=payment["id"])
    assert len(events) == len(allocations) == 1
    for model in ("first", "last"):
        report = attribution(client, model)
        for measure, expected in (("won_deal_value", 50000), ("invoiced_net", 20000), ("cash_receipts", 7500)):
            result = row(report, measure)
            assert accounting.cents(result["amount"]) == expected, (measure, result)
            assert result["campaign_id"] == campaign["id"]
            assert result["source_records"], result
        assert row(report, "customers")["count"] == 1
        metrics = {metric["key"]: metric["value"] for metric in report["metrics"]}
        assert metrics["cac_customer_difference"] == 0 and metrics["cac_new_customers"] == 1
        for name in ("cac", "campaigns"):
            original = client.request(f"/api/v1/reports/{name}?period=all&organization_id=1&currency=USD")
            for metric in original["metrics"]:
                assert metrics[name + "_" + metric["key"]] == metric["value"]
        receivables = client.request("/api/v1/reports/receivables?period=all&organization_id=1&currency=USD")
        balances = {metric["key"]: metric["value"] for metric in receivables["metrics"]}
        assert accounting.cents(balances["outstanding"]) == 12500
        assert row(report, "invoiced_net")["source_records"] == f'invoice_event:{events[0]["id"]}'
        assert row(report, "cash_receipts")["source_records"] == f'sale:{allocations[0]["sale_id"]}'
    return report


def journey(server):
    client = server.start()
    anonymous = accounting.Client(server.origin)
    secret = secrets.token_hex(32)
    settings = client.request("/organizations/1/settings/attribution", {
        "operation": "configure", "connection_id": "0", "version": "0",
        "tenant_id": "synthetic_tenant", "environment": "test", "signing_secret": secret,
    }, form=True, text=True)
    assert "Signing settings saved" in settings and secret not in settings
    connections = client.rows("integration_connection", provider="lightsite_capture")
    assert len(connections) == 1 and "sealed_settings" not in connections[0]
    connection = connections[0]
    campaign = client.create("campaign", name="Synthetic newsletter", channel="email", spend="100 USD", revenue="999 USD", started_at=moment())
    form = client.create("lead_form", name="Synthetic inquiry", active=True, honeypot="company_fax")
    site = client.create("attribution_site", name="Synthetic site", origin=SITE_ORIGIN,
                         external_site_id="synthetic_site", external_tenant_id="synthetic_tenant",
                         lead_form_id=form["id"], connection_id=connection["id"], active=True,
                         consent_policy="synthetic-analytics-v1", marketing_policy=POLICY,
                         marketing_statement=STATEMENT, campaign_map=json.dumps({"newsletter": campaign["id"]}))
    prefix = f'/attribution/{site["uuid"]}'
    request_headers = {"Origin": SITE_ORIGIN, "Content-Type": "text/plain"}
    anonymous.request(prefix + "/grant", {"policy": "synthetic-analytics-v1"}, headers={"Origin": "https://neighbor.example.test"}, expected=(404,))
    assert client.rows("attribution_visitor") == []
    permission = anonymous.request(prefix + "/grant", {"policy": "synthetic-analytics-v1"}, headers=request_headers)
    token = permission["token"]
    assert client.rows("marketing_consent") == []
    observation = {"token": token, "event_id": "synthetic-observation", "fields": {
        "page": SITE_ORIGIN + "/services?discard=email@example.test", "utm_source": "newsletter",
        "utm_medium": "email", "utm_campaign": "newsletter",
    }}
    anonymous.request(prefix + "/observe", observation, headers=request_headers)
    touches = client.rows("attribution_touch")
    assert len(touches) == 1 and "path" not in touches[0] and "visitor_id" not in touches[0]
    print("PASS exact Origin rejects neighbor without a visitor; analytics grant creates no email permission; one scoped touch retained", flush=True)

    payload = {"version": 1, "submission_id": "synthetic-signed-first", "visitor_token": token,
               "tenant_id": "synthetic_tenant", "site_id": "synthetic_site", "origin": SITE_ORIGIN,
               "fields": {"name": "Synthetic Visitor", "email": "synthetic@example.test", "message": "Please discuss the synthetic service."}}
    body = json.dumps(payload, separators=(",", ":"), sort_keys=True).encode()
    timestamp = str(int(time.time()))
    signature = hmac.new(secret.encode(), timestamp.encode() + b"\n" + body, hashlib.sha256).hexdigest()
    signed_headers = {"X-Venture-Timestamp": timestamp, "X-Venture-Signature": signature}
    hook = f'/hooks/lightsite/{site["uuid"]}/{connection["id"]}'
    anonymous.request(hook, body, headers={**signed_headers, "X-Venture-Signature": "0" * 64}, expected=(403,))
    assert client.rows("attribution_submission") == [] and client.rows("lead") == []
    anonymous.request(hook, body, headers=signed_headers)
    submissions = client.rows("attribution_submission")
    assert len(submissions) == 1
    submission = submissions[0]
    assert submission["connection_id"] == connection["id"] and submission["first_touch_id"] == touches[0]["id"]
    assert submission["marketing_consent_id"] == 0 and client.rows("marketing_consent") == []
    lead_id = submission["lead_id"]
    client.request(f"/api/v1/lead/{lead_id}", {"status": "qualified"}, method="PATCH")
    converted = client.request(f"/api/v1/leads/{lead_id}/convert", {"deal": True})
    company_id, deal_id = converted["converted_company_id"], converted["converted_deal_id"]
    assert company_id > 0 and deal_id > 0
    client.request(f"/api/v1/deal/{deal_id}", {"value": "500 USD"}, method="PATCH")
    won = [stage for stage in client.rows("pipeline_stage") if stage["kind"] == "won"]
    assert len(won) == 1
    client.request(f"/api/v1/deals/{deal_id}/move", {"stage_id": won[0]["id"], "note": "Synthetic accepted proposal"})
    today = datetime.now(timezone.utc).date().isoformat()
    invoice = client.create("invoice", number="ATTR-SYNTHETIC-1", company_id=company_id, issued_at=today, due_at=today)
    client.create("invoice_line", invoice_id=invoice["id"], description="Synthetic acquired work", quantity=1, unit_price="200 USD")
    client.request(f'/api/v1/invoice/{invoice["id"]}', {"status": "sent"}, method="PATCH")
    payment = client.create("payment", customer_id=company_id, invoice_id=invoice["id"], method="synthetic-bank-evidence", amount="75 USD", date=today)
    allocations = client.rows("payment_allocation")
    assert len(allocations) == 1 and accounting.cents(allocations[0]["amount"]) == 7500
    financial_report(client, invoice, payment, campaign)
    print(f'PASS signed submission{submission["id"]} -> lead{lead_id} -> company{company_id}/deal{deal_id} -> invoice{invoice["id"]}/payment{payment["id"]}; both models show won50000 invoiced20000 cash7500 cents, not declared campaign revenue99900', flush=True)

    email_payload = {**payload, "submission_id": "synthetic-explicit-email", "marketing": {
        "granted": True, "policy": POLICY, "statement": STATEMENT, "occurred_at": moment()}}
    anonymous.request(prefix + "/capture", email_payload, headers=request_headers)
    consents = client.rows("marketing_consent")
    assert len(consents) == 1 and consents[0]["email"] == "synthetic@example.test"
    consent = consents[0]
    assert len(client.rows("lead")) == 1 and len(client.rows("attribution_submission")) == 2
    anonymous.request(prefix + "/withdraw", {"token": token}, headers=request_headers)
    retained = client.request(f'/api/v1/marketing_consent/{consent["id"]}')
    assert retained == consent, "Analytics withdrawal must not silently alter independent email evidence"
    client.action("marketing_consent", consent["id"], "withdraw")
    assert len(client.rows("suppression")) == 1
    financial_report(client, invoice, payment, campaign)
    print(f'PASS explicit email choice created consent{consent["id"]}; analytics withdrawal preserved email evidence; separate email withdrawal created suppression and retained financial attribution', flush=True)

    identities = {name: [item["id"] for item in client.rows(name)] for name in (
        "lead", "company", "deal", "invoice", "payment", "payment_allocation", "attribution_submission", "attribution_binding", "marketing_consent", "suppression")}
    email_evidence = {name: client.rows(name) for name in ("marketing_consent", "suppression")}
    server.stop()
    client = server.start()
    anonymous = accounting.Client(server.origin)
    anonymous.request(hook, body, headers=signed_headers)
    assert identities == {name: [item["id"] for item in client.rows(name)] for name in identities}
    assert email_evidence == {name: client.rows(name) for name in email_evidence}
    financial_report(client, invoice, payment, campaign)
    print("PASS exact original timestamp/body/HMAC retry after restart retained every business, submission and consent ID; no re-consent or duplicate receipt", flush=True)


def main():
    if not __debug__:
        raise SystemExit("Run without Python -O: acceptance assertions are mandatory")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    os.umask(0o077)
    root = Path(tempfile.mkdtemp(prefix="venture-attribution-server-drill-"))
    fingerprint = hashlib.sha256(binary.read_bytes()).hexdigest()
    print("Artifact SHA256: " + fingerprint, flush=True)
    print("Private fixture: " + str(root), flush=True)
    server = accounting.Server(binary, root / "workspace", environment={"VENTURE_INTEGRATION_KEY": base64.b64encode(secrets.token_bytes(32)).decode()})
    succeeded = False
    try:
        journey(server)
        assert hashlib.sha256(binary.read_bytes()).hexdigest() == fingerprint, "Artifact changed during walkthrough"
        print("PASS unchanged artifact SHA256: " + fingerprint, flush=True)
        succeeded = True
    finally:
        server.stop()
        if succeeded:
            shutil.rmtree(root)
            print("PASS owned private fixture removed; no external provider or Lightsite provisioning was invoked", flush=True)
        else:
            print("FAILED: private diagnostic fixture retained at " + str(root), flush=True)


if __name__ == "__main__":
    main()
