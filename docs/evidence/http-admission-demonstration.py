#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Demonstrate request backpressure with two private synthetic server processes."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import secrets
import socket
import subprocess
import tempfile
import time
import urllib.error
import urllib.request
import uuid


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, response, code, message, headers, url):
        return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    root = Path(tempfile.mkdtemp(prefix="venture-admission-demo-"))
    root.chmod(0o700)
    environment = {k: v for k, v in os.environ.items() if not k.startswith("VENTURE_")}
    environment["VENTURE_SESSION_SECRET"] = secrets.token_hex(32)
    opener = urllib.request.build_opener(NoRedirect())
    processes = []
    logs = []
    bases = []

    def get(index, path):
        try:
            response = opener.open(bases[index] + path, timeout=5)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            return response.status, response.headers, response.read()

    try:
        print(
            "Artifact SHA256:",
            hashlib.sha256(binary.read_bytes()).hexdigest(),
            flush=True,
        )
        for name in ("limited", "neighbor"):
            state = root / name
            state.mkdir(mode=0o700)
            with socket.socket() as bound:
                bound.bind(("127.0.0.1", 0))
                port = bound.getsockname()[1]
            base = f"http://127.0.0.1:{port}"
            bases.append(base)
            config = state / "config.yaml"
            config.write_text(
                f"hosted:\n  enabled: true\n  workspace_id: {uuid.uuid4()}\n"
                f"  origin: {base}\n  http_requests_per_minute: 1\n"
                "  http_burst: 2\n  http_concurrency: 1\n"
                "server:\n  bind_address: 127.0.0.1\n"
            )
            config.chmod(0o600)
            command = [
                str(binary),
                "--config",
                str(config),
                "--database",
                "sqlite://" + str(state / "workspace.db"),
                "--state-dir",
                str(state),
                "--port",
                str(port),
                "--no-ai",
                "--no-automation",
                "--no-plugins",
            ]
            subprocess.run(
                command + ["--migrate"],
                env=environment,
                cwd=state,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=60,
                check=True,
            )
            log = (state / "server.log").open("wb")
            logs.append(log)
            child = subprocess.Popen(
                command,
                env=environment,
                cwd=state,
                stdout=log,
                stderr=subprocess.STDOUT,
            )
            processes.append(child)
            deadline = time.monotonic() + 60
            while True:
                if child.poll() is not None:
                    raise RuntimeError(
                        "Synthetic server stopped; inspect retained private logs"
                    )
                try:
                    assert get(len(bases) - 1, "/api/v1/health")[0] == 200
                    break
                except (OSError, AssertionError):
                    if time.monotonic() >= deadline:
                        raise RuntimeError("Readiness deadline exceeded")
                    time.sleep(0.05)
        for expected in (200, 200, 429):
            status, headers, body = get(0, "/login")
            assert status == expected, (status, expected)
            if status == 429:
                assert headers["Retry-After"] == "60"
                assert headers["Cache-Control"] == "no-store"
                assert b"request limit" in body
        print(
            "PASS limited workspace: two dynamic requests admitted; third returns 429, Retry-After 60 and no-store",
            flush=True,
        )
        assert get(1, "/login")[0] == 200
        print(
            "PASS separate neighbor process retains its own admission budget",
            flush=True,
        )
        status, _, body = get(0, "/api/v1/health")
        assert status == 200 and isinstance(json.loads(body), dict)
        print("PASS exhausted workspace still serves public build health", flush=True)
        print(
            "LIMIT: this is a two-process admission demonstration, not capacity, storage, job fairness or hosted-launch qualification",
            flush=True,
        )
    finally:
        for child in processes:
            child.terminate()
        for child in processes:
            try:
                child.wait(timeout=10)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait(timeout=10)
        for log in logs:
            log.close()
        print("Private evidence:", root, flush=True)


if __name__ == "__main__":
    main()
