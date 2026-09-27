#!/usr/bin/env python3
"""Opt-in layout regression against an authenticated, populated DEBUG demo.

Copyright (C) 2026 Zach Podbielniak
SPDX-License-Identifier: AGPL-3.0-or-later
"""

import os

from playwright.sync_api import sync_playwright


def main():
    """Keep board scrolling inside its region with or without animations."""
    base = os.environ["VENTURE_TEST_UI_URL"].rstrip("/")
    username = os.environ["VENTURE_TEST_UI_USERNAME"]
    password = os.environ["VENTURE_TEST_UI_PASSWORD"]
    with sync_playwright() as playwright:
        browser = playwright.chromium.launch(headless=True)
        for motion in ("reduce", "no-preference"):
            context = browser.new_context(reduced_motion=motion)
            page = context.new_page()
            page.goto(base + "/login")
            page.locator('[name="username"]').fill(username)
            page.locator('[name="password"]').fill(password)
            page.locator('button[type="submit"]').click()
            page.wait_for_url(lambda url: "/login" not in url)
            for look in ("industrial", "classic"):
                context.add_cookies([{
                    "name": "venture_look", "value": look, "url": base,
                }])
                for width in (390, 1440):
                    page.set_viewport_size({"width": width, "height": 1000})
                    response = page.goto(base + "/tickets")
                    assert response.status == 200
                    assert page.locator(".ticket-card").count() > 0, "Seed demo tickets first"
                    # Animations previously supplied accidental containing blocks.
                    # Reduced motion exposed absolutely positioned hidden words
                    # in off-screen columns, widening the entire page.
                    page.wait_for_timeout(800)
                    size = page.evaluate("""() => ({
                        viewport: innerWidth,
                        page: document.documentElement.scrollWidth,
                        board: document.querySelector('.board').clientWidth,
                        columns: document.querySelector('.board').scrollWidth
                    })""")
                    assert size["page"] <= size["viewport"] + 1, (look, motion, width, size)
                    assert size["columns"] > size["board"], "Demo must exercise scrolling columns"
                    print(f"PASS ticket board: {look}, {motion}, {width}px")
            context.close()
        browser.close()


if __name__ == "__main__":
    main()
