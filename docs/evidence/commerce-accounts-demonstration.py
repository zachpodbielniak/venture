#!/usr/bin/env python3
"""Demonstrate the DEBUG artifact in a private hosted workspace; no provider calls.

Requires Playwright Chromium. Retains private evidence for review. Synthetic
credentials are submitted through password fields/stdin and never printed.
"""
import argparse
import base64
import hashlib
import json
import os
import pathlib
import secrets
import socket
import subprocess
import tempfile
import time
import urllib.request

from playwright.sync_api import sync_playwright

root = pathlib.Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description="Commerce account demonstration")
parser.add_argument("--binary", type=pathlib.Path, default=root / "build/debug/venture",
                    help="DEBUG venture executable to demonstrate; venturectl is taken from the same directory")
options = parser.parse_args()
binary = options.binary.resolve(strict=True)
controller = (binary.parent / "venturectl").resolve(strict=True)
state = pathlib.Path(tempfile.mkdtemp(prefix="venture-125-commerce-demo-"))
state.chmod(0o700)
with socket.socket() as listener:
    listener.bind(("127.0.0.1", 0))
    port = listener.getsockname()[1]
base = f"http://127.0.0.1:{port}"
config = state / "config.yaml"
config.write_text("hosted:\n  enabled: true\n  workspace_id: 277cccd4-bdae-43c7-a77a-10c341d70e2e\n  origin: " + base + "\ncommerce:\n  enabled: true\n")
config.chmod(0o600)
env = os.environ.copy()
env.pop("VENTURE_TOKEN", None)
env["VENTURE_SESSION_SECRET"] = secrets.token_hex(32)
env["VENTURE_INTEGRATION_KEY"] = base64.b64encode(secrets.token_bytes(32)).decode()
env["VENTURE_COMMERCE_SHOPIFY_TOKEN"] = "ignored-ambient-fixture-secret"
env["VENTURE_COMMERCE_SHOPIFY_SHOP"] = "ignored.myshopify.com"
password = secrets.token_urlsafe(24)
token = secrets.token_urlsafe(32)
rotated = secrets.token_urlsafe(32)
args = [str(binary), "--config", str(config), "--database", "sqlite://" + str(state / "venture.db"), "--state-dir", str(state), "--port", str(port), "--no-ai", "--no-plugins", "--no-automation"]

def command(extra, credential=None):
    result = subprocess.run(args + extra, input=credential, capture_output=True, text=True, env=env, cwd=root, timeout=120)
    if result.returncode:
        raise RuntimeError(result.stderr)

print("Artifact SHA256: " + hashlib.sha256(binary.read_bytes()).hexdigest(), flush=True)
command(["--migrate"])
command(["--tenant-admin", "administrator", "--tenant-password-file", "-", "--tenant-reason", "Synthetic commerce demonstration"], password + "\n")
log = open(state / "server.log", "w")
server = subprocess.Popen(args, env=env, cwd=root, stdout=log, stderr=subprocess.STDOUT)
try:
    for _ in range(300):
        if server.poll() is not None:
            raise RuntimeError("Server stopped: " + str(state))
        try:
            urllib.request.urlopen(base + "/api/v1/health", timeout=1).close()
            break
        except Exception:
            time.sleep(0.1)
    else:
        raise RuntimeError("Readiness timeout")
    with sync_playwright() as playwright:
        browser = playwright.chromium.launch(headless=True)
        context = browser.new_context(viewport={"width": 1280, "height": 900}, record_video_dir=str(state / "video"))
        page = context.new_page()
        page.goto(base + "/login")
        page.locator("[name=username]").fill("administrator")
        page.locator("[name=password]").fill(password)
        page.locator("button[type=submit]").click()
        page.wait_for_load_state()
        path = base + "/organizations/1/settings/commerce"
        page.goto(path)
        assert "No Shopify account" in page.inner_text("body")
        print("PASS tenant administrator opens commerce settings; ambient Shopify credentials do not configure an account", flush=True)
        page.locator("[name=shop]").fill("fixture.myshopify.com")
        page.locator("[name=access_token]").fill(token)
        page.get_by_role("button", name="Connect or rotate").click()
        page.wait_for_load_state()
        assert "Configured Shopify account: fixture.myshopify.com" in page.inner_text("body")
        assert token not in page.content()
        connection = int(page.locator("[name=connection_id]").input_value())
        version = int(page.locator("[name=version]").input_value())
        page.wait_for_timeout(700)
        print("PASS explicit organization account configured; credential is absent from returned HTML", flush=True)
        page.locator("[name=shop]").fill("fixture.myshopify.com")
        page.locator("[name=access_token]").fill(rotated)
        page.get_by_role("button", name="Connect or rotate").click()
        page.wait_for_load_state()
        assert int(page.locator("[name=version]").input_value()) == version + 1
        assert rotated not in page.content()
        response = context.request.post(path, form={"operation": "configure", "connection_id": str(connection), "version": str(version), "shop": "fixture.myshopify.com", "access_token": token})
        assert response.status == 400, response.status
        print("PASS rotation increments the binding revision; a stale settings form is refused", flush=True)

        def payload(response):
            assert response.ok, (response.status, response.text())
            value = response.json()
            value = value.get("data", value) if isinstance(value, dict) else value
            return value.get("records", value) if isinstance(value, dict) else value

        customer = payload(context.request.post(base + "/api/v1/company", data={"organization_id": 1, "name": "Synthetic legacy commerce customer", "external_id": "shopify:customer:55"}))
        invoice = payload(context.request.post(base + "/api/v1/invoices/compose", data={"organization_id": 1, "company_id": customer["id"], "external_id": "shopify:9001", "send": False, "lines": [{"description": "Synthetic legacy service", "quantity": 1, "unit_price": "25 USD"}]}))
        invoice_version = invoice["version"]
        page.goto(base + f"/e/integration_connection/{connection}")
        form = page.locator('form[data-record-action="adopt_commerce_invoice"]')
        form.scroll_into_view_if_needed()
        form.locator('[name="invoice_id"]').select_option(str(invoice["id"]))
        form.locator('[name="reason"]').fill("Reviewed original synthetic shop order evidence")
        page.wait_for_timeout(700)
        form.get_by_role("button", name="Adopt legacy commerce invoice").click()
        page.wait_for_timeout(1000)
        links = payload(context.request.get(base + "/api/v1/commerce_import_link"))
        assert len(links) == 2, links
        retained = payload(context.request.get(base + f'/api/v1/invoice/{invoice["id"]}'))
        assert retained["version"] == invoice_version
        print("PASS generated adoption action records invoice and customer identity; retained invoice revision is unchanged", flush=True)
        page.goto(base + "/e/commerce_import_link")
        page.wait_for_timeout(900)
        page.screenshot(path=str(state / "identity.png"), full_page=True)
        page.goto(path)
        page.get_by_role("button", name="Disconnect", exact=True).click()
        page.wait_for_load_state()
        assert "No Shopify account" in page.inner_text("body")
        response = context.request.post(base + "/api/v1/commerce/import", data={"organization_id": 1, "connector": "shopify"})
        assert response.status >= 400
        assert len(payload(context.request.get(base + "/api/v1/commerce_import_link"))) == 2
        print("PASS disconnect refuses import despite ambient credentials and preserves historical identity", flush=True)
        assert context.request.get(base + "/settings").status == 403
        print("PASS tenant administrator remains excluded from platform settings", flush=True)
        session = state / "cli-session.json"
        cookie = next(item for item in context.cookies() if item["name"] == "venture_session")
        session.write_text(json.dumps({"origin": base, "cookie": cookie["name"] + "=" + cookie["value"]}))
        session.chmod(0o600)
        cli = [str(controller), "--server", base, "--session-file", str(session)]
        described = subprocess.run(cli + ["describe", "integration_connection"], capture_output=True, text=True, env=env, cwd=root, timeout=20)
        assert described.returncode == 0 and "account_id" in described.stdout, described.stderr
        schema = payload(context.request.get(base + "/api/v1/schema/integration_connection"))
        assert any(action["name"] == "adopt_commerce_invoice" for action in schema["actions"])
        imported = subprocess.run(cli + ["commerce", "import", '{"organization_id":1,"connector":"shopify"}'], capture_output=True, text=True, env=env, cwd=root, timeout=20)
        assert imported.returncode != 0
        print("PASS venturectl describes account fields; API schema offers adoption; explicitly scoped CLI import refuses the disconnected account", flush=True)
        page.screenshot(path=str(state / "final.png"), full_page=True)
        page.wait_for_timeout(900)
        context.close()
        browser.close()
    print("PASS demonstration completed without contacting Shopify; provider test/import semantics are covered by injected transport tests", flush=True)
    print("Evidence directory: " + str(state), flush=True)
finally:
    server.terminate()
    try:
        server.wait(timeout=10)
    except subprocess.TimeoutExpired:
        server.kill()
        server.wait()
    log.close()
