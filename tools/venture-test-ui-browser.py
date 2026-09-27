#!/usr/bin/env python3
"""Opt-in layout regression against an authenticated, populated DEBUG demo.

Copyright (C) 2026 Zach Podbielniak
SPDX-License-Identifier: AGPL-3.0-or-later
"""

import argparse
import os
import sys
from typing import TYPE_CHECKING, Literal

# Imported for real only once there is something to drive, so --help and
# --license work on a machine without the opt-in browser dependency.
if TYPE_CHECKING:
    from playwright.sync_api import Browser, Page


LICENSE_TEXT: str = """venture-test-ui-browser.py -- ticket-board layout regression for VENTURE
Copyright (C) 2026 Zach Podbielniak

This program is free software: you can redistribute it and/or modify it under
the terms of the GNU Affero General Public License as published by the Free
Software Foundation, either version 3 of the License, or (at your option) any
later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>."""

EPILOG: str = """environment:
  VENTURE_TEST_UI_URL       base URL of a disposable DEBUG demo
  VENTURE_TEST_UI_USERNAME  a user on that demo
  VENTURE_TEST_UI_PASSWORD  that user's password

examples:
  # a throwaway environment with the pinned Playwright and its Chromium
  python3 -m venv /tmp/venture-ui && /tmp/venture-ui/bin/pip install -r tools/requirements-ui-browser.txt
  /tmp/venture-ui/bin/playwright install chromium

  # against `make demo` on 8749; its owner password is printed once, into
  # build/demo/server.log, so read it from there rather than typing it
  VENTURE_TEST_UI_URL=http://127.0.0.1:8749 VENTURE_TEST_UI_USERNAME=owner \\
      VENTURE_TEST_UI_PASSWORD="$(sed -n 's/^ *Password: *//p' build/demo/server.log | head -n 1)" \\
      /tmp/venture-ui/bin/python tools/venture-test-ui-browser.py"""

LOOKS: tuple[str, ...] = ("industrial", "classic")
WIDTHS: tuple[int, ...] = (390, 1440)
Motion = Literal["reduce", "no-preference"]
MOTIONS: tuple[Motion, ...] = ("reduce", "no-preference")


def parse_args() -> argparse.Namespace:
    """Build the command line: only --help and --license, the rest is env."""
    parser: argparse.ArgumentParser = argparse.ArgumentParser(
        description="Opt-in layout regression against an authenticated, populated DEBUG demo.",
        epilog=EPILOG,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--license", action="store_true",
                        help="print the license (AGPLv3) and exit")
    return parser.parse_args()


def require_env(name: str) -> str:
    """Read a required variable, failing with a sentence rather than a KeyError."""
    value: str | None = os.environ.get(name)
    if not value:
        print(f"venture-test-ui-browser: {name} is not set (see --help)",
              file=sys.stderr)
        sys.exit(2)
    return value


def sign_in(page: "Page", base: str, username: str, password: str) -> None:
    """Log in through the real form, so the session cookie is the server's own."""
    page.goto(base + "/login")
    page.locator('[name="username"]').fill(username)
    page.locator('[name="password"]').fill(password)
    page.locator('button[type="submit"]').click()
    page.wait_for_url(lambda url: "/login" not in url)


def check_board(page: "Page", base: str, look: str, motion: str, width: int) -> None:
    """Assert the board scrolls inside its own region at one look/width/motion."""
    page.set_viewport_size({"width": width, "height": 1000})
    response = page.goto(base + "/tickets")
    assert response is not None and response.status == 200
    assert page.locator(".ticket-card").count() > 0, "Seed demo tickets first"

    # Animations previously supplied accidental containing blocks.
    # Reduced motion exposed absolutely positioned hidden words in
    # off-screen columns, widening the entire page.
    page.wait_for_timeout(800)
    size: dict[str, int] = page.evaluate("""() => ({
        viewport: innerWidth,
        page: document.documentElement.scrollWidth,
        board: document.querySelector('.board').clientWidth,
        columns: document.querySelector('.board').scrollWidth
    })""")
    assert size["page"] <= size["viewport"] + 1, (look, motion, width, size)
    assert size["columns"] > size["board"], "Demo must exercise scrolling columns"
    print(f"PASS ticket board: {look}, {motion}, {width}px")


def main() -> int:
    """Walk every look, width and motion preference; it only reads pages."""
    args: argparse.Namespace = parse_args()
    if args.license:
        print(LICENSE_TEXT)
        return 0

    base: str = require_env("VENTURE_TEST_UI_URL").rstrip("/")
    username: str = require_env("VENTURE_TEST_UI_USERNAME")
    password: str = require_env("VENTURE_TEST_UI_PASSWORD")

    from playwright.sync_api import sync_playwright

    with sync_playwright() as playwright:
        browser: "Browser" = playwright.chromium.launch(headless=True)
        for motion in MOTIONS:
            context = browser.new_context(reduced_motion=motion)
            page: "Page" = context.new_page()
            sign_in(page, base, username, password)
            for look in LOOKS:
                context.add_cookies([{
                    "name": "venture_look", "value": look, "url": base,
                }])
                for width in WIDTHS:
                    check_board(page, base, look, motion, width)
            context.close()
        browser.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
