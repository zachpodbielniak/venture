import pathlib, tempfile, subprocess, socket, ssl, time, http.client, json, shutil, hashlib

if not __debug__:
    raise RuntimeError(
        "Run without Python optimization: assertions verify the demonstration"
    )
root = pathlib.Path.cwd()
state = pathlib.Path(tempfile.mkdtemp(prefix="venture-http-demo-"))
processes = []
logs = []
output = []


def freeport():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def start(name, tls=False):
    directory = state / name
    directory.mkdir()
    port = freeport()
    config = directory / "config.yaml"
    text = f"server:\n  bind_address: 127.0.0.1\n  port: {port}\n  max_request_size_mb: 1\n  max_buffered_request_mb: 1\n  max_connections: 2\n  request_timeout: 2\nsecurity:\n  require_auth: false\ndatabase:\n  uri: sqlite://{directory}/records.db\n"
    if tls:
        text = text.replace(
            "  request_timeout: 2",
            f"  request_timeout: 2\n  tls_certificate: {root}/tests/fixtures/federation/tls-cert.pem\n  tls_private_key: {root}/tests/fixtures/federation/tls-key.pem",
        )
    config.write_text(text)
    log = open(directory / "server.log", "w")
    logs.append(log)
    process = subprocess.Popen(
        [
            str(root / "build/debug/venture"),
            "-c",
            str(config),
            "--state-dir",
            str(directory),
            "--no-ai",
            "--no-plugins",
            "--no-automation",
        ],
        stdout=log,
        stderr=subprocess.STDOUT,
    )
    processes.append(process)
    server = (port, tls)
    for _ in range(200):
        if process.poll() is not None:
            raise RuntimeError("server exited")
        try:
            if request(server, "GET", "/api/v1/health")[0] == 200:
                return server
        except (OSError, http.client.HTTPException):
            time.sleep(0.1)
    raise RuntimeError("startup timeout")


trust = ssl.create_default_context(
    cafile=str(root / "tests/fixtures/federation/tls-cert.pem")
)


def connect(server):
    sock = socket.create_connection(("127.0.0.1", server[0]), timeout=5)
    return trust.wrap_socket(sock, server_hostname="localhost") if server[1] else sock


def request(server, method, path, body=None):
    connection = (
        http.client.HTTPSConnection("127.0.0.1", server[0], context=trust, timeout=5)
        if server[1]
        else http.client.HTTPConnection("127.0.0.1", server[0], timeout=5)
    )
    connection.request(
        method,
        path,
        body=body,
        headers={"Content-Type": "application/json", "Connection": "close"},
    )
    response = connection.getresponse()
    result = (response.status, response.read())
    connection.close()
    return result


def raw(server, payload):
    sock = connect(server)
    try:
        sock.sendall(payload)
        response = http.client.HTTPResponse(sock)
        response.begin()
        status = response.status
        response.read()
        return status
    finally:
        sock.close()


def note(label, value):
    output.append(label + " => " + str(value) + "\n")


try:
    artifact = hashlib.sha256((root / "build/debug/venture").read_bytes()).hexdigest()
    note("Artifact SHA256", artifact)
    a = start("workspace-a")
    b = start("workspace-b")
    tls = start("tls-workspace", True)
    assert (
        request(
            a, "POST", "/api/v1/organization", b'{"name":"Ordinary HTTP","active":true}'
        )[0]
        == 201
    )
    before = json.loads(request(a, "GET", "/api/v1/organization")[1])["total"]
    note("Ordinary generic organization create", 201)
    header = b"POST /api/v1/organization HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\nContent-Length: 1048577\r\n\r\n"
    for server, label in [(a, "HTTP"), (tls, "TLS with verified fixture CA")]:
        status = raw(server, header)
        assert status == 413
        note(label + " declared oversized body without payload", status)
    chunk = b"x" * 1048577
    payload = (
        b"POST /api/v1/organization HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n100001\r\n"
        + chunk
        + b"\r\n0\r\n\r\n"
    )
    status = raw(a, payload)
    assert status == 413
    note("Chunked overflow without Content-Length", status)
    status = raw(
        a,
        b"POST /api/v1/organization HTTP/1.1\r\nHost: localhost\r\nContent-Length: -1\r\nConnection: close\r\n\r\n",
    )
    assert status == 400
    note("Malformed negative Content-Length", status)
    slow = b"POST /api/v1/organization HTTP/1.1\r\nHost: localhost\r\nContent-Length: 10\r\n\r\nx"
    started = time.monotonic()
    status = raw(a, slow)
    assert status == 408 and time.monotonic() - started < 4
    note("Incomplete body, absolute receive deadline", status)
    held = [connect(a), connect(a)]
    time.sleep(0.1)
    try:
        rejected = raw(
            a,
            b"GET /api/v1/health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n",
        )
        assert rejected == 503
        note("A: two idle connections occupy both slots", rejected)
    except (ConnectionResetError, BrokenPipeError, http.client.RemoteDisconnected):
        note("A: two idle connections occupy both slots", "excess connection closed")
    assert request(b, "GET", "/api/v1/health")[0] == 200
    note("B remains independently available while A slots are full", 200)
    for sock in held:
        sock.close()
    time.sleep(0.1)
    held = connect(a)
    partial = (
        b"POST /api/v1/organization HTTP/1.1\r\nHost: localhost\r\nContent-Length: 700000\r\nConnection: close\r\n\r\n"
        + b"x" * 600000
    )
    held.sendall(partial)
    time.sleep(0.1)
    status = raw(a, partial)
    assert status == 503
    note("Aggregate receive budget, individually valid body sizes", status)
    held.close()
    time.sleep(0.1)
    assert request(a, "GET", "/api/v1/health")[0] == 200
    note("A admission recovered after abort releases body/connection budget", 200)
    stalled = socket.create_connection(("127.0.0.1", tls[0]), timeout=5)
    started = time.monotonic()
    assert stalled.recv(1) == b"" and time.monotonic() - started < 4
    stalled.close()
    note(
        "Raw TCP stalled before TLS ClientHello",
        "closed within configured receive bound",
    )
    assert request(tls, "GET", "/api/v1/health")[0] == 200
    note("Verified TLS remains available after stalled handshake", 200)
    after = json.loads(request(a, "GET", "/api/v1/organization")[1])["total"]
    assert before == after
    note("Rejected generic writes changed organization count", 0)
    invalid = state / "invalid"
    invalid.mkdir()
    config = invalid / "config.yaml"
    config.write_text(
        f"server:\n  max_request_size_mb: 0\ndatabase:\n  uri: sqlite://{invalid}/must-not-exist.db\n"
    )
    result = subprocess.run(
        [
            str(root / "build/debug/venture"),
            "-c",
            str(config),
            "--state-dir",
            str(invalid),
            "--no-ai",
            "--no-plugins",
            "--no-automation",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=10,
    )
    assert result.returncode and not (invalid / "must-not-exist.db").exists()
    note("Invalid size fails before opening the database", result.stdout.strip())
    for setting in ["http_requests_per_minute", "http_burst", "http_concurrency"]:
        invalid = state / setting
        invalid.mkdir()
        config = invalid / "config.yaml"
        config.write_text(
            f"hosted:\n  enabled: true\n  workspace_id: 0381d47a-cbf7-43a5-89e8-f540cae344da\n  origin: https://startup.example.test\n  {setting}: 0\ndatabase:\n  uri: sqlite://{invalid}/must-not-exist.db\n"
        )
        result = subprocess.run(
            [
                str(root / "build/debug/venture"),
                "-c",
                str(config),
                "--state-dir",
                str(invalid),
                "--no-ai",
                "--no-plugins",
                "--no-automation",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=10,
        )
        assert result.returncode and not (invalid / "must-not-exist.db").exists()
        note(
            "Invalid hosted " + setting + " fails before database creation",
            result.stdout.strip(),
        )
    assert (
        hashlib.sha256((root / "build/debug/venture").read_bytes()).hexdigest()
        == artifact
    )
    for process in processes:
        process.terminate()
    for process in processes:
        process.wait(timeout=10)
    for log in logs:
        log.close()
    document = root / "docs/http-limits-demonstration.org"
    text = document.read_text()
    start = text.index("#+begin_example\n") + len("#+begin_example\n")
    end = text.index("#+end_example", start)
    document.write_text(text[:start] + "".join(output) + text[end:])
    shutil.rmtree(state)
    print("Built-server HTTP/TLS limits demonstration passed")
except:
    for process in processes:
        if process.poll() is None:
            process.terminate()
    for process in processes:
        process.wait(timeout=10)
    for log in logs:
        log.close()
    print("Failure evidence", state)
    raise
