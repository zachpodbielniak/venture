#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Exercise the built DEBUG server with a private DB and a loopback AI fixture.

Requires Python Playwright and its Chromium browser. No production AI service
or existing database is contacted. --evidence DIRECTORY retains a browser
recording and sanitized transcript; without it the owned temporary tree is
removed. Build first with make DEBUG=1. This is an opt-in demonstration.
"""
import argparse
import base64
import http.server
import json
import os
import pathlib
import secrets
import shutil
import socket
import subprocess
import tempfile
import threading
import time
import urllib.request

from playwright.sync_api import sync_playwright


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--evidence", type=pathlib.Path)
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parent.parent
    state = pathlib.Path(tempfile.mkdtemp(prefix="venture-ai-settings-"))
    os.chmod(state, 0o700)
    transcript = []
    requests = []

    def record(message):
        print(message, flush=True)
        transcript.append(message)

    class Provider(http.server.BaseHTTPRequestHandler):
        def log_message(self, *unused):
            pass

        def do_POST(self):
            body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            key = self.headers.get("Authorization")
            assert key in {"Bearer fixture-own-first", "Bearer fixture-own-second", "Bearer fixture-platform"}
            assert self.path == "/v1/chat/completions"
            requests.append((key, body))
            response = {"id": "synthetic", "model": "fixture-model", "choices": [{"index": 0, "message": {"role": "assistant", "content": "Scoped fixture answer"}, "finish_reason": "stop"}], "usage": {"prompt_tokens": 7, "completion_tokens": 3}}
            payload = json.dumps(response).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

    provider = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Provider)
    provider_thread = threading.Thread(target=provider.serve_forever, daemon=True)
    provider_thread.start()
    provider_base = f"http://127.0.0.1:{provider.server_port}"
    config = state / "config.yaml"
    config.write_text(f"ai:\n  enabled: true\n  allow_loopback: true\n  allowed_base_urls:\n    - {provider_base}\n  provider_deadline_seconds: 3\n")
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    base = f"http://127.0.0.1:{port}"
    env = os.environ.copy()
    env["VENTURE_INTEGRATION_KEY"] = base64.b64encode(secrets.token_bytes(32)).decode()
    env["VENTURE_SESSION_SECRET"] = secrets.token_hex(32)
    for name in ("OPENAI_API_KEY", "ANTHROPIC_API_KEY", "GEMINI_API_KEY", "XAI_API_KEY", "GROK_API_KEY"):
        env.pop(name, None)
    log = (state / "server.log").open("w")
    server = subprocess.Popen([str(root / "build/debug/venture"), "--config", str(config), "--database", "sqlite://" + str(state / "venture.db"), "--state-dir", str(state), "--port", str(port), "--owner-password", "Synthetic-AI-Demo-125", "--no-plugins", "--no-automation"], cwd=root, env=env, stdout=log, stderr=subprocess.STDOUT)
    success = False
    try:
        for _ in range(300):
            if server.poll() is not None:
                raise RuntimeError("Built server stopped; inspect the private fixture log")
            try:
                urllib.request.urlopen(base + "/api/v1/health", timeout=1).close()
                break
            except OSError:
                time.sleep(0.1)
        else:
            raise RuntimeError("Built server readiness timeout")
        with sync_playwright() as playwright:
            browser = playwright.chromium.launch(headless=True)
            context = browser.new_context(viewport={"width": 1280, "height": 960}, record_video_dir=str(state / "video"))
            page = context.new_page()
            page.goto(base + "/login")
            page.locator("input[name=username]").fill("owner")
            page.locator("input[name=password]").fill("Synthetic-AI-Demo-125")
            page.locator("button[type=submit]").click()
            page.wait_for_url(lambda url: "/login" not in url)

            def response_data(response):
                assert response.ok, (response.status, response.text())
                value = response.json()
                return value.get("data", value) if isinstance(value, dict) else value

            def create(kind, values):
                return response_data(context.request.post(base + "/api/v1/" + kind, data=values))["id"]

            second = create("organization", {"name": "Second synthetic business", "slug": "second-ai-fixture", "active": True})
            secret_company = create("company", {"organization_id": second, "name": "ForeignCompanyMustNotEnterPrompt"})
            for org, key in ((1, "fixture-own-first"), (second, "fixture-own-second")):
                page.goto(base + f"/organizations/{org}/settings/ai")
                form = page.locator("form").filter(has=page.locator('input[name=purpose][value=chat]'))
                form.locator("[name=provider]").select_option("openai")
                form.locator("[name=model]").fill("fixture-model")
                form.locator("[name=base_url]").fill(provider_base)
                form.locator("[name=api_key]").fill(key)
                form.get_by_role("button", name="Connect or rotate own provider", exact=True).click()
                page.wait_for_load_state()
                assert "Effective source: organization" in page.locator("body").inner_text()
                assert key not in page.content()
                page.reload()
                assert all(value == "" for value in page.locator("input[name=api_key]").evaluate_all("els => els.map(e => e.value)"))
                page.get_by_role("heading", name="chat", exact=True).scroll_into_view_if_needed()
                page.wait_for_timeout(1200)
            record("Built DEBUG server: two organizations configured independent write-only chat credentials; reload showed blank secret fields.")
            before = len(requests)
            reply = context.request.post(base + "/ui/chat", form={"organization_id": "1", "message": "Summarize this record", "context": f"/e/company/{secret_company}"})
            assert reply.ok and "Scoped fixture answer" in reply.text(), (reply.status, reply.text())
            assert len(requests) == before + 1 and requests[-1][0] == "Bearer fixture-own-first"
            assert "ForeignCompanyMustNotEnterPrompt" not in json.dumps(requests[-1][1])
            record("A first-organization chat used its own provider; another organization's record context never entered the provider request.")
            upload = response_data(context.request.post(base + "/ui/chat/upload", multipart={"organization_id": str(second), "file": {"name": "private.txt", "mimeType": "text/plain", "buffer": b"ForeignAttachmentMustNotEnterPrompt"}}))
            before = len(requests)
            refused = context.request.post(base + "/ui/chat", form={"organization_id": "1", "message": "Read this attachment", "attachments": str(upload["id"])})
            assert not refused.ok and len(requests) == before
            record("A cross-organization attachment was refused before any provider request.")
            page.goto(base + "/settings/ai/platform")
            form = page.locator("form").filter(has=page.locator('input[name=offer_id][value="0"]'))
            form.locator("[name=title]").fill("Synthetic platform offer")
            form.locator("[name=billing_organization_id]").select_option(str(second))
            form.locator("[name=provider]").select_option("openai")
            form.locator("[name=model]").fill("fixture-model")
            form.locator("[name=base_url]").fill(provider_base)
            form.locator("[name=api_key]").fill("fixture-platform")
            form.get_by_role("button", name="Create or rotate platform credentials").click()
            page.wait_for_load_state()
            offer_id = int(page.get_by_role("link", name="Eligibility and disable actions").first.get_attribute("href").rsplit("/", 1)[1])
            offer = response_data(context.request.get(base + f"/api/v1/ai_platform_offer/{offer_id}"))
            grant = response_data(context.request.post(base + f'/api/v1/ai_platform_offer/{offer["id"]}/actions/grant', data={"organization_id": 1, "enabled": True, "monthly_requests": 20, "concurrency_limit": 1, "grant_version": 0}))
            page.goto(base + "/organizations/1/settings/ai")
            form = page.locator("form").filter(has=page.locator('input[name=purpose][value=chat]'))
            form.locator("[name=grant_id]").select_option(str(grant["id"]))
            form.get_by_role("button", name="Select platform grant", exact=True).click()
            page.wait_for_load_state()
            assert "Effective source: platform" in page.locator("body").inner_text()
            page.get_by_role("heading", name="chat", exact=True).scroll_into_view_if_needed()
            page.wait_for_timeout(1200)
            form = page.locator("form").filter(has=page.locator('input[name=purpose][value=chat]'))
            form.get_by_role("button", name="Test selected provider", exact=True).click()
            page.wait_for_load_state()
            assert "answered the synthetic connection test" in page.locator("body").inner_text()
            assert requests[-1][0] == "Bearer fixture-platform"
            page.get_by_role("heading", name="chat", exact=True).scroll_into_view_if_needed()
            page.wait_for_timeout(1200)
            record("Platform offer creation did not select it; an explicit metadata grant and recipient selection enabled its synthetic connection test.")
            response_data(context.request.post(base + f'/api/v1/ai_platform_offer/{offer["id"]}/actions/grant', data={"organization_id": 1, "enabled": False, "monthly_requests": 20, "concurrency_limit": 1, "grant_version": grant["version"]}))
            page.reload()
            before = len(requests)
            form = page.locator("form").filter(has=page.locator('input[name=purpose][value=chat]'))
            form.get_by_role("button", name="Test selected provider", exact=True).click()
            page.wait_for_load_state()
            assert "unavailable" in page.locator("body").inner_text() and len(requests) == before
            page.get_by_role("heading", name="chat", exact=True).scroll_into_view_if_needed()
            page.wait_for_timeout(1200)
            record("Revoking the platform grant made the selected source unavailable without trying the organization's previous key.")
            page.screenshot(path=str(state / "final.png"), full_page=False)
            context.close()
            browser.close()
        record("Demonstration complete. All users, credentials, records and HTTP responses were synthetic; no paid provider or existing database was used.")
        success = True
    finally:
        server.terminate()
        try:
            server.wait(timeout=15)
        except subprocess.TimeoutExpired:
            server.kill()
            server.wait()
        log.close()
        provider.shutdown()
        provider.server_close()
        provider_thread.join(timeout=5)
        if args.evidence:
            args.evidence.mkdir(parents=True, exist_ok=True)
            (args.evidence / "transcript.txt").write_text("\n".join(transcript) + "\n")
            if (state / "video").exists():
                shutil.copytree(state / "video", args.evidence / "video", dirs_exist_ok=True)
            if (state / "final.png").exists():
                shutil.copy2(state / "final.png", args.evidence / "final.png")
        if success:
            shutil.rmtree(state)
        else:
            print(f"Failed fixture retained at {state}")


if __name__ == "__main__":
    main()
