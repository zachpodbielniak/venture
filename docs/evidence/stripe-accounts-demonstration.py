#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Stripe account boundary, hosted invoice links and ACH return guards on a built DEBUG artifact.

Usage: stripe-accounts-demonstration.py BINARY SCRATCH_ROOT

Starts the supplied `venture` binary on a free loopback port with a private
config and state directory, bootstraps the first-run owner, and drives the
Stripe surfaces over HTTP exactly as an operator would. No provider is
contacted: the only outbound HTTPS attempt the server can make (Stripe account
verification) is pinned to a local sink through https_proxy, and that sink
counts the attempts. Synthetic credentials are fixture strings; no live
merchant account, bank account or Stripe key is used.

Every request, status and the relevant response field is printed. Any mismatch
raises and the process exits non-zero.
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
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

STEP = [0]


def free_port():
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


class Sink:
    """A local listener standing in for every outbound provider destination."""

    def __init__(self):
        self.socket = socket.socket()
        self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.socket.bind(("127.0.0.1", 0))
        self.socket.listen(16)
        self.port = self.socket.getsockname()[1]
        self.attempts = []
        self.running = True
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()

    def serve(self):
        while self.running:
            try:
                connection, _ = self.socket.accept()
            except OSError:
                return
            try:
                connection.settimeout(2)
                try:
                    first = connection.recv(256).decode("utf-8", "replace").splitlines()
                    self.attempts.append(first[0] if first else "")
                except OSError:
                    self.attempts.append("")
            finally:
                connection.close()

    def stop(self):
        self.running = False
        try:
            self.socket.close()
        except OSError:
            pass


class RefuseRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, response, code, message, headers, url):
        return None


class Client:
    def __init__(self, origin, label):
        self.origin, self.label = origin, label
        self.cookies = http.cookiejar.CookieJar()
        self.opener = urllib.request.build_opener(
            urllib.request.ProxyHandler({}),
            urllib.request.HTTPCookieProcessor(self.cookies),
            RefuseRedirect())
        self.last_headers = {}
        self.last_text = ""

    def request(self, path, values=None, method=None, form=False, expected=None,
                anonymous=False, note=None):
        body = None
        if values is not None:
            body = urllib.parse.urlencode(values).encode() if form else json.dumps(values).encode()
        request = urllib.request.Request(self.origin + path, body, method=method)
        if body is not None:
            request.add_header("Content-Type",
                               "application/x-www-form-urlencoded" if form else "application/json")
            request.add_header("Origin", self.origin)
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), RefuseRedirect()) \
            if anonymous else self.opener
        try:
            response = opener.open(request, timeout=30)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            status = response.status
            content = response.read(4 * 1024 * 1024)
            headers = dict(response.headers.items())
        STEP[0] += 1
        verb = method or ("POST" if body is not None else "GET")
        who = "anonymous" if anonymous else self.label
        print("[%02d] %s %s  (%s) -> HTTP %d" % (STEP[0], verb, path, who, status), flush=True)
        if note:
            print("     " + note, flush=True)
        text = content.decode("utf-8", "replace")
        self.last_headers = headers
        self.last_text = text
        if expected is not None:
            assert status in expected, "expected %s, got %d: %s" % (expected, status, text[:600])
        try:
            parsed = json.loads(text) if text else None
        except ValueError:
            return None
        if isinstance(parsed, dict):
            return parsed.get("data", parsed)
        return parsed

    def login(self, username, password):
        self.request("/login", {"username": username, "password": password},
                     form=True, expected=(302, 303))
        assert any(cookie.name == "venture_session" for cookie in self.cookies), "no session issued"

    def create(self, entity, **values):
        values.setdefault("organization_id", 1)
        return self.request("/api/v1/" + entity, values, expected=(200, 201))

    def rows(self, entity, org=1):
        query = urllib.parse.urlencode({"organization_id": org, "limit": 1000})
        return self.request("/api/v1/%s?%s" % (entity, query), expected=(200,))["records"]


def alert(html):
    match = re.search(r'<p role="alert">(.*?)</p>', html, re.S)
    return re.sub(r"\s+", " ", match.group(1)).strip() if match else ""


# Every key below is synthetic and belongs to this fixture. They are assembled
# from parts so that no literal Stripe-key-shaped token is stored under docs/:
# test-stripe.c's test_no_keys greps src, data and docs for exactly that shape,
# and that guard must keep working.
TEST_SECRET_KEY = "sk_" + "test_synthetic_fixture_only"
LIVE_SECRET_KEY = "sk_" + "live_synthetic_fixture_only"
AMBIENT_SECRET_KEY = "sk_" + "test_ambient_installation_fixture"


def shows(text, needle):
    assert needle in text, "missing %r in: %s" % (needle, text[:800])


def absent(text, needle):
    assert needle not in text, "unexpectedly present: %r" % (needle,)


def connect_form(**overrides):
    values = {"operation": "configure", "version": "0", "connection_id": "0",
              "secret_key": TEST_SECRET_KEY,
              "publishable_key": "pk_test_synthetic_fixture_only",
              "webhook_secret": "whsec_synthetic_fixture_only",
              "api_version": "2024-06-20",
              "success_url": "https://invoices.example.test/paid",
              "cancel_url": "https://invoices.example.test/cancel",
              "environment": "test", "ach_enabled": "on"}
    values.update(overrides)
    return values


def main():
    binary = pathlib.Path(sys.argv[1]).resolve()
    root = pathlib.Path(sys.argv[2]).resolve()
    root.mkdir(parents=True, exist_ok=True)
    root.chmod(0o700)

    print("Artifact: " + str(binary), flush=True)
    print("Artifact SHA256: " + hashlib.sha256(binary.read_bytes()).hexdigest(), flush=True)

    sink = Sink()
    port = free_port()
    origin = "http://127.0.0.1:%d" % port
    config = root / "config.yaml"
    config.write_text(
        "server:\n  bind_address: 127.0.0.1\n  port: " + str(port) +
        "\nsecurity:\n  require_auth: true\ndatabase:\n  uri: " +
        json.dumps("sqlite://" + str(root / "workspace.db")) +
        "\nstripe:\n  enabled: true\n")
    config.chmod(0o600)

    environment = {key: value for key, value in os.environ.items() if not key.startswith("VENTURE_")}
    environment.update(
        VENTURE_SESSION_SECRET=secrets.token_hex(32),
        VENTURE_INTEGRATION_KEY="MDEyMzQ1Njc4OTAxMjM0NTY3ODkwMTIzNDU2Nzg5MDE=",
        XDG_CONFIG_HOME=str(root / "config-home"), XDG_DATA_HOME=str(root / "data-home"),
        # Ambient installation credentials that production must never adopt.
        VENTURE_STRIPE_SECRET_KEY=AMBIENT_SECRET_KEY,
        VENTURE_STRIPE_PUBLISHABLE_KEY="pk_test_ambient_installation_fixture",
        VENTURE_STRIPE_WEBHOOK_SECRET="whsec_ambient_installation_fixture",
        VENTURE_STRIPE_API_VERSION="2024-06-20",
        VENTURE_STRIPE_SUCCESS_URL="https://ambient.example.test/paid",
        VENTURE_STRIPE_CANCEL_URL="https://ambient.example.test/cancel",
        # Every outbound destination resolves to the local sink.
        https_proxy="http://127.0.0.1:%d" % sink.port,
        HTTPS_PROXY="http://127.0.0.1:%d" % sink.port,
        http_proxy="http://127.0.0.1:%d" % sink.port,
        HTTP_PROXY="http://127.0.0.1:%d" % sink.port,
        no_proxy="127.0.0.1,localhost")
    arguments = [str(binary), "--config", str(config), "--state-dir", str(root / "state"),
                 "--no-ai", "--no-plugins", "--no-automation"]
    log_path = root / "server.log"
    log = log_path.open("ab")
    process = subprocess.Popen(arguments, cwd=str(binary.parents[2]), env=environment,
                               stdout=log, stderr=subprocess.STDOUT)
    try:
        owner = Client(origin, "owner")
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise RuntimeError("server exited; see " + str(log_path))
            try:
                urllib.request.build_opener(urllib.request.ProxyHandler({})).open(
                    origin + "/api/v1/health", timeout=2).close()
                break
            except (OSError, urllib.error.URLError, ValueError):
                time.sleep(0.1)
        else:
            raise RuntimeError("server did not become ready")
        matches = re.findall(r"^\s*Password:\s*(\S+)\s*$", log_path.read_text(errors="replace"), re.M)
        assert matches, "first-run owner password not found in the private server log"
        owner.login("owner", matches[0])

        print("\n== A. One Stripe account per organization: the unconnected binding ==", flush=True)
        owner.request("/api/v1/health", expected=(200,))
        owner.request("/organizations/1/settings/stripe", expected=(200,))
        html = owner.last_text
        shows(html, "No Stripe account is connected to this organization.")
        shows(html, "Credentials are write-only.")
        absent(html, "Webhook path:")
        print("     page: no connected account; no webhook endpoint is published yet", flush=True)
        second = owner.request("/api/v1/organization",
                               {"name": "Second synthetic business", "slug": "second-stripe"},
                               expected=(200, 201))
        assert second["id"] != 1, second
        owner.request("/organizations/%d/settings/stripe" % second["id"], expected=(200,))
        shows(owner.last_text, "No Stripe account is connected to this organization.")
        print("     organization %d has its own independent settings page" % second["id"], flush=True)
        print("PASS each organization carries its own Stripe binding; none is shared or inherited", flush=True)

        print("\n== B. Connect refusals decided before any provider request ==", flush=True)
        before = len(sink.attempts)
        cases = [
            ({"environment": "sandbox"}, "Stripe environment must be test or live"),
            ({"secret_key": LIVE_SECRET_KEY}, "Stripe keys must match the selected environment"),
            ({"success_url": "http://invoices.example.test/paid"}, "Stripe return URLs require HTTPS without embedded credentials"),
            ({"cancel_url": "https://user:secret@invoices.example.test/cancel"}, "Stripe return URLs require HTTPS without embedded credentials"),
            ({"webhook_secret": ""}, "Stripe settings require bounded nonempty token and URL values"),
        ]
        for overrides, message in cases:
            owner.request("/organizations/1/settings/stripe", connect_form(**overrides),
                          form=True, expected=(400,))
            body = owner.last_text
            assert alert(body) == message, (alert(body), message)
            print("     refusal: " + message, flush=True)
            absent(body, TEST_SECRET_KEY)
            absent(body, "whsec_synthetic_fixture_only")
        assert len(sink.attempts) == before, sink.attempts
        print("PASS environment, key/environment agreement, URL and bounds rules refuse locally; "
              "outbound provider attempts still %d; no credential is echoed" % before, flush=True)

        print("\n== C. The account is verified with the supplied key before anything is stored ==", flush=True)
        owner.request("/organizations/1/settings/stripe", connect_form(), form=True, expected=(400,))
        body = owner.last_text
        assert alert(body) == "Stripe account verification failed; check credentials and connectivity", alert(body)
        shows(body, "No Stripe account is connected to this organization.")
        assert len(sink.attempts) > before, "no outbound verification attempt was made"
        print("     outbound attempts captured by the local sink: %r" % (sink.attempts[before:],), flush=True)
        connections = owner.rows("integration_connection")
        assert not [row for row in connections if row.get("provider") == "stripe"], connections
        print("PASS a well-formed but unverifiable account stores no binding: "
              "integration_connection rows for stripe = 0", flush=True)

        print("\n== D. Organization A's binding is invisible to organization B ==", flush=True)
        outsider_password = secrets.token_urlsafe(24)
        owner.request("/users", {"username": "second-business-admin", "password": outsider_password,
                                 "role": "editor"}, form=True, expected=(302, 303))
        user = next(row for row in owner.rows("user") if row["username"] == "second-business-admin")
        owner.request("/api/v1/organization_membership",
                      {"organization_id": second["id"], "user_id": user["id"], "role": "admin",
                       "active": True}, expected=(200, 201))
        outsider = Client(origin, "second-business-admin")
        outsider.login("second-business-admin", outsider_password)
        outsider.request("/organizations/%d/settings/stripe" % second["id"], expected=(200,))
        shows(outsider.last_text, "No Stripe account is connected to this organization.")
        outsider.request("/organizations/1/settings/stripe", expected=(403,))
        shows(outsider.last_text, "Organization integration administration is required")
        outsider.request("/organizations/1/settings/stripe", connect_form(), form=True, expected=(403,))
        shows(outsider.last_text, "Organization integration administration is required")
        print("PASS an administrator of the second business can neither read nor rotate the first "
              "business's Stripe settings", flush=True)

        print("\n== E. Provider identity alone grants no authority ==", flush=True)
        owner.request("/api/v1/integration_connection",
                      {"organization_id": 1, "provider": "stripe", "account_id": "acct_forged",
                       "environment": "live", "active": True}, expected=(400, 403, 409, 422))
        shows(owner.last_text, "Integration bindings are changed only through organization settings")
        for record in ("stripe_checkout", "stripe_event", "stripe_payment_link"):
            owner.request("/api/v1/" + record, {"organization_id": 1, "connection_id": 1},
                          expected=(400, 403, 409, 422))
            shows(owner.last_text, "Stripe evidence must be written through VentureStripeService")
            print("     %s: refused to a generic writer" % record, flush=True)
        print("PASS neither a Stripe binding nor Stripe evidence can be forged through the generic API",
              flush=True)

        print("\n== F. Webhook routing is per connection, with no ambient fallback ==", flush=True)
        event = {"id": "evt_synthetic_fixture", "object": "event", "type": "checkout.session.completed",
                 "livemode": False,
                 "data": {"object": {"id": "cs_synthetic", "amount_total": 10000, "currency": "usd",
                                     "payment_status": "paid", "payment_intent": "pi_synthetic"}}}
        owner.request("/webhooks/stripe/1", event, anonymous=True, expected=(503,),
                      note="event routed to a connection that does not exist")
        owner.request("/webhooks/stripe/999999", event, anonymous=True, expected=(503,))
        owner.request("/webhooks/stripe", event, anonymous=True, expected=(503,),
                      note="the unbound fixture route refuses in production configuration")
        owner.request("/webhooks/stripe/not-a-connection", event, anonymous=True, expected=(503,),
                      note="a non-numeric binding resolves no connection either")
        print("     ambient VENTURE_STRIPE_* credentials are set in the server environment and "
              "settle nothing", flush=True)
        print("PASS /webhooks/stripe/:connection_id only resolves a configured connection; "
              "an unbound or unknown endpoint cannot post a receipt", flush=True)

        print("\n== G. Hosted payment actions refuse without that organization's account ==", flush=True)
        customer = owner.create("company", name="Synthetic ACH customer")
        invoice = owner.create("invoice", number="ACH-DEMO-1", company_id=customer["id"],
                               issued_at="2026-01-10")
        owner.create("invoice_line", invoice_id=invoice["id"], description="Synthetic delivered work",
                     quantity=1, unit_price="100 USD")
        owner.request("/api/v1/invoice/%d" % invoice["id"], {"status": "sent"}, method="PATCH",
                      expected=(200,))
        owner.request("/api/v1/invoices/%d/checkout" % invoice["id"], {},
                      expected=(400, 403, 409, 422, 500),
                      note="VENTURE_ERROR_CONFIG is mapped to 500 by venture_web_error_response()")
        shows(owner.last_text, "Organization integration is unconfigured or ambiguous")
        owner.request("/api/v1/invoice/%d/actions/payment_link" % invoice["id"], {},
                      expected=(400, 403, 409, 422, 500))
        shows(owner.last_text, "Organization integration is unconfigured or ambiguous")
        assert owner.rows("stripe_payment_link") == [], "a link was created without an account"
        print("PASS ACH Checkout and the hosted invoice link both require this organization's "
              "verified account; no link record exists", flush=True)

        print("\n== H. The public /pay resolver exposes nothing it was not given ==", flush=True)
        capability = "%s-%s-4%s-a%s-%s.%s" % (
            secrets.token_hex(4), secrets.token_hex(2), secrets.token_hex(2)[1:],
            secrets.token_hex(2)[1:], secrets.token_hex(6), secrets.token_hex(32))
        assert len(capability) == 101 and capability[36] == ".", capability
        owner.request("/pay/" + capability, anonymous=True, expected=(404,))
        shows(owner.last_text, "Payment link is unavailable")
        headers = owner.last_headers
        assert headers.get("Cache-Control") == "no-store", headers
        assert headers.get("Referrer-Policy") == "no-referrer", headers
        absent(owner.last_text, "ACH-DEMO-1")
        absent(owner.last_text, "Synthetic ACH customer")
        print("     unavailable page: no invoice number, no customer, Cache-Control=no-store, "
              "Referrer-Policy=no-referrer", flush=True)
        owner.request("/pay/" + capability, {"invoice_id": invoice["id"]}, form=True,
                      anonymous=True, expected=(404,),
                      note="a submitted invoice selector cannot choose an invoice")
        shows(owner.last_text, "Payment link is unavailable")
        owner.request("/pay/invalid", anonymous=True, expected=(302, 303, 401, 403, 404))
        absent(owner.last_text, "ACH-DEMO-1")
        state = owner.request("/api/v1/invoice/%d" % invoice["id"], expected=(200,))
        assert state["workflow_state"] in ("sent", "overdue"), state["workflow_state"]
        print("     invoice workflow_state remains " + state["workflow_state"], flush=True)
        print("PASS a missing or tampered capability returns one unavailable page, discloses no "
              "invoice identity and honours no invoice selector", flush=True)

        print("\n== I. A return after settlement meets the existing refund guards ==", flush=True)
        owner.create("payment", customer_id=customer["id"], invoice_id=invoice["id"],
                     method="transfer", amount="100 USD", date="2026-01-20")
        paid = owner.request("/api/v1/invoice/%d" % invoice["id"], expected=(200,))
        assert paid["workflow_state"] == "paid", paid["workflow_state"]
        allocation = owner.rows("payment_allocation")[0]
        print("     receipt applied: invoice workflow_state=%s, allocation id=%d"
              % (paid["workflow_state"], allocation["id"]), flush=True)
        year = owner.create("fiscal_year", name="FY2026", start_at="2026-01-01",
                            end_at="2027-01-01", period_length="monthly")
        january = next(row for row in owner.rows("fiscal_period")
                       if row["fiscal_year_id"] == year["id"]
                       and row["start_at"].startswith("2026-01-01"))
        owner.request("/api/v1/fiscal_period/%d" % january["id"], {"state": "closed"},
                      method="PATCH", expected=(200,))
        print("     fiscal period %s closed" % january["name"], flush=True)
        owner.request("/api/v1/refund",
                      {"organization_id": 1, "customer_id": customer["id"],
                       "allocation_id": allocation["id"], "date": "2026-01-28", "amount": "30 USD",
                       "reference": "Synthetic ACH return in a closed month"},
                      expected=(400, 403, 409, 422),
                      note="a return dated inside the closed month")
        shows(owner.last_text, "is closed for organization 1")
        assert owner.rows("refund") == [], "a closed-period refund was written"
        print("PASS a return cannot post into the closed month; refund rows = 0", flush=True)

        owner.create("accounting_approval_rule", action="pay", require_second_actor=True)
        reviewer_password = secrets.token_urlsafe(24)
        owner.request("/users", {"username": "synthetic-reviewer", "password": reviewer_password,
                                 "role": "owner"}, form=True, expected=(302, 303))
        reviewer = Client(origin, "synthetic-reviewer")
        reviewer.login("synthetic-reviewer", reviewer_password)
        chargeback = {"organization_id": 1, "customer_id": customer["id"],
                      "allocation_id": allocation["id"], "date": "2026-02-05", "amount": "30 USD",
                      "reference": "Synthetic ACH return, open month"}
        owner.request("/api/v1/refund", dict(chargeback), expected=(400, 403, 409, 422),
                      note="the proposer applying their own return")
        shows(owner.last_text, "The whole business operation requires a second account; repeat the exact command to approve")
        assert owner.rows("refund") == [], "a self-approved refund was written"
        pending = [row for row in owner.rows("accounting_approval") if row["state"] == "pending"]
        assert len(pending) == 1, pending
        print("     pending approval: action=%s record_type=%s proposer=%s"
              % (pending[0]["action"], pending[0]["record_type"], pending[0]["proposer"]), flush=True)
        reviewer.request("/api/v1/refund", dict(chargeback), expected=(200, 201),
                         note="the second actor applying the identical proposal")
        refunds = owner.rows("refund")
        assert len(refunds) == 1, refunds
        applied = [row for row in owner.rows("accounting_approval") if row["state"] == "applied"]
        assert len(applied) == 1, applied
        reopened = owner.request("/api/v1/invoice/%d" % invoice["id"], expected=(200,))
        print("     refund %s applied by %s; invoice workflow_state=%s"
              % (refunds[0]["amount"], applied[0]["approver"], reopened["workflow_state"]), flush=True)
        assert reopened["workflow_state"] == "partially_paid", reopened["workflow_state"]
        print("PASS separation of duties is enforced on the return: the proposer is refused, a "
              "distinct authenticated actor applies the identical proposal once", flush=True)

        print("\nAll assertions passed.", flush=True)
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=10)
        log.close()
        sink.stop()

    print("""
LIMITATIONS: what this transcript does NOT prove.
- No Stripe was contacted. The only outbound attempt the server made was
  captured by the local sink shown above, so no live account, bank account,
  mandate or settlement timing is exercised here.
- Venture's synthetic Stripe provider is a C StripeTransport passed to
  venture_stripe_service_new()/venture_stripe_settings_configure(); there is no
  configuration key or environment variable that injects it into a running
  server, and Venture does not expose stripe-glib's test_base_url. A running
  artifact therefore cannot hold a verified Stripe connection offline, so
  everything that needs one is shown only by the DEBUG test artifact:
  * an event signed for organization A's connection delivered to another
    endpoint, a wrong signing secret, wrong livemode, wrong account, colliding
    provider IDs across two organizations and identical local invoice numbers:
    build/debug/tests/test-stripe -p /stripe/binding-rotation,
    -p /stripe/binding-organizations, -p /stripe/binding-settings-ui,
    -p /stripe/binding-replacement, -p /stripe/binding-authorization,
    -p /stripe/binding-migration, -p /stripe/account-binding-metadata.
  * disconnect retaining historical credentials so an old connection's callbacks
    still verify while its replacement cannot borrow them:
    -p /stripe/binding-replacement.
  * rotation accepting the new signing secret and refusing the old one:
    -p /stripe/binding-rotation.
  * the hosted link lifecycle with a live capability - resolving a real token,
    the outstanding amount without the invoice number or customer, expiry,
    replacement, revocation, already-paid, deleted and changed invoices, and a
    pending bank payment that creates no second collection:
    -p /stripe/payment-link/basic, /authorization, /changed, /replacement,
    /uncertain, /deleted, /finance, /api, /paid, /processing, /form.
  * the ACH return itself arriving as a signed charge.dispute event and passing
    through the refund/dispute guards, including the closed-period and
    separation-of-duties replays:
    -p /stripe/ach/return-lost, -p /stripe/ach/return-won,
    -p /stripe/ach/return-closed-period, -p /stripe/ach/return-sod,
    -p /stripe/ach/sod, -p /stripe/ach/closed-period.
- Sections H and I demonstrate the public resolver and the refund, period and
  approval guards the Stripe return path reuses, driven through the ordinary
  HTTP API rather than through a signed provider event.
- SQLite only, one process, synthetic organizations, customers and money.""",
          flush=True)


if __name__ == "__main__":
    main()
