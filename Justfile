# VENTURE — local instance task runner.
# Run `just`        to list grouped recipes.
# Run `just c`      to fuzzy-select one interactively.
# Run `just <name>` to invoke directly.
#
# This drives a local instance out of the build tree: the binaries make
# produced, a SQLite database under build/, and nothing containerised.
# `make compose-up` is the other shape, and the two are deliberately
# separate -- this one is for reading the database with sqlite3 while the
# server is still running.
#
# make builds. just runs. Nothing here compiles anything except by calling
# make, so there is one build system and it is the one that knows how.

set shell := ["bash", "-euo", "pipefail", "-c"]
set positional-arguments

# ──────────────────────────────────────────────────────────────────────
# Variables (override with: just port=9000 <recipe>)
# ──────────────────────────────────────────────────────────────────────

# debug carries the assertions and the symbols, which is what you want
# locally. `just build_type=release ...` for the other one.
build_type := "debug"
outdir     := justfile_directory() + "/build/" + build_type

# 8748, not 8747.
#
# The default in the configuration is 8747, and that is also where a
# containerised instance from compose.yaml listens. Colliding with it would
# mean this recipe either fails to bind or -- worse -- silently talks to the
# container while you think you are testing the build tree.
port     := "8748"
state    := justfile_directory() + "/build/run"
database := "sqlite://" + state + "/venture.db"
log      := state + "/server.log"
pidfile  := state + "/venture.pid"
base_url := "http://127.0.0.1:" + port

# Everything the server needs to find its own plugins, venture types and pod
# modules in the build tree rather than an installed copy. Mirrors what
# `make run` sets, so the two behave the same.
run_env := \
    "VENTURE_PLUGIN_PATH=" + outdir + "/plugins " + \
    "VENTURE_VENTURE_TYPE_PATH=" + justfile_directory() + "/data/venture-types " + \
    "VENTURE_POD_MODULE_PATH=" + outdir + "/pod-modules:" + justfile_directory() + "/deps/podomation/build/" + build_type + "/modules"

venture    := outdir + "/venture"
venturectl := outdir + "/venturectl"

# ──────────────────────────────────────────────────────────────────────
# Meta
# ──────────────────────────────────────────────────────────────────────

# List grouped recipes.
[group('meta')]
default:
    @just --list --unsorted --list-heading $'VENTURE local instance\n\n'

# Fuzzy-select a recipe (requires fzf).
[group('meta')]
c:
    @just --choose

# Print the resolved paths, when something is not where you expect.
[group('meta')]
where:
    @echo "binary    {{ venture }}"
    @echo "cli       {{ venturectl }}"
    @echo "state     {{ state }}"
    @echo "database  {{ database }}"
    @echo "log       {{ log }}"
    @echo "url       {{ base_url }}"

# ──────────────────────────────────────────────────────────────────────
# Running
# ──────────────────────────────────────────────────────────────────────

# Build (if needed) and start the server in the background.
[group('run')]
start: build
    #!/usr/bin/env bash
    set -euo pipefail

    # Checked inline rather than through a recipe: `just _running` inside
    # an `if` still reports a failed recipe when it returns non-zero.
    if [ -f '{{ pidfile }}' ] && kill -0 "$(cat '{{ pidfile }}')" 2>/dev/null; then
        echo "Already running on {{ port }} (pid $(cat '{{ pidfile }}'))."
        exit 0
    fi

    mkdir -p '{{ state }}'

    # A stable secret, kept in the state directory.
    #
    # Without one the server generates a random secret at startup, and
    # every restart logs you out -- which during a session of restarting
    # the server after each change is most of what you would be doing.
    if [ ! -f '{{ state }}/session-secret' ]; then
        umask 077
        openssl rand -hex 32 > '{{ state }}/session-secret'
    fi

    VENTURE_SESSION_SECRET="$(cat '{{ state }}/session-secret')" \
    {{ run_env }} \
    nohup '{{ venture }}' \
        --database '{{ database }}' \
        --state-dir '{{ state }}' \
        --port '{{ port }}' >> '{{ log }}' 2>&1 &

    echo $! > '{{ pidfile }}'

    for _ in $(seq 1 60); do
        if curl -fs -o /dev/null '{{ base_url }}/api/v1/health' 2>/dev/null; then
            echo "Listening on {{ base_url }}"
            just _first_run_password
            exit 0
        fi
        sleep 0.25
    done

    echo "It did not come up. Last of the log:" >&2
    tail -20 '{{ log }}' >&2
    exit 1

# Stop it.
[group('run')]
stop:
    #!/usr/bin/env bash
    set -euo pipefail

    if [ ! -f '{{ pidfile }}' ]; then
        echo "Not running."
        exit 0
    fi

    pid="$(cat '{{ pidfile }}')"

    # Checked against the recorded pid rather than pkill'd by name: a
    # pattern broad enough to catch this server is broad enough to catch
    # the installed one, or somebody else's.
    if kill -0 "$pid" 2>/dev/null; then
        kill "$pid"
        for _ in $(seq 1 40); do
            kill -0 "$pid" 2>/dev/null || break
            sleep 0.25
        done
        kill -0 "$pid" 2>/dev/null && kill -9 "$pid" || true
        echo "Stopped $pid."
    else
        echo "Stale pidfile; it was not running."
    fi

    rm -f '{{ pidfile }}'

# Stop, then start.
[group('run')]
restart: stop start

# Is it up, and what is it?
[group('run')]
status:
    #!/usr/bin/env bash
    set -euo pipefail

    if [ -f '{{ pidfile }}' ] && kill -0 "$(cat '{{ pidfile }}')" 2>/dev/null; then
        echo "running   pid $(cat '{{ pidfile }}') on {{ base_url }}"
        curl -fsS '{{ base_url }}/api/v1/health'
    else
        echo "stopped"
    fi

# Follow the log.
[group('run')]
logs lines="40":
    @tail -n {{ lines }} -f '{{ log }}'

# Open the UI in your browser.
[group('run')]
open:
    @xdg-open '{{ base_url }}' >/dev/null 2>&1 &

# Build the binaries, plugins and pod modules this instance needs.
[group('run')]
build:
    @{{ if build_type == "debug" { "DEBUG=1" } else { "" } }} make --no-print-directory venture venturectl plugins pod-modules

# ──────────────────────────────────────────────────────────────────────
# Credentials and the CLI
# ──────────────────────────────────────────────────────────────────────

# It is shown once, when the database is created. If this finds nothing the
# account has already been set up -- `just reset` starts over.

# Print the owner password from the first-run log.
[group('cli')]
password:
    @grep -oP 'Password: \K\S+' '{{ log }}' | tail -1 || \
        echo "No first-run password in the log. Already set up, or try: just reset" >&2

# Needs the owner password, so it only works before you have changed it.
# After that, mint one from Settings and write it to build/run/token.

# Mint an owner API token and keep it in the state directory.
[group('cli')]
token:
    #!/usr/bin/env bash
    set -euo pipefail

    if ! { [ -f '{{ pidfile }}' ] && kill -0 "$(cat '{{ pidfile }}')" 2>/dev/null; }; then
        echo "Not running. Try: just start" >&2
        exit 1
    fi

    if [ -s '{{ state }}/token' ]; then
        cat '{{ state }}/token'
        exit 0
    fi

    password="$(just password)"
    jar="$(mktemp)"
    trap 'rm -f "$jar"' EXIT

    curl -fsS -c "$jar" -o /dev/null -X POST \
        --data-urlencode "username=owner" \
        --data-urlencode "password=$password" \
        '{{ base_url }}/login'

    umask 077
    curl -fsS -b "$jar" -X POST -H 'Content-Type: application/json' \
        -d '{"name":"just","role":"owner"}' \
        '{{ base_url }}/api/v1/tokens' \
        | grep -oP '"token"\s*:\s*"\K[^"]+' > '{{ state }}/token'

    cat '{{ state }}/token'

# Run venturectl against this instance: `just ctl list ticket`
[group('cli')]
ctl *args:
    @VENTURE_SERVER='{{ base_url }}' VENTURE_TOKEN="$(just token)" '{{ venturectl }}' "$@"

# Open the database in sqlite3. The reason this instance is not a container.
[group('cli')]
sql *args:
    @sqlite3 '{{ state }}/venture.db' "$@"

# ──────────────────────────────────────────────────────────────────────
# The database
# ──────────────────────────────────────────────────────────────────────

# Apply pending migrations without starting the server.
[group('db')]
migrate: build
    @mkdir -p '{{ state }}'
    @{{ run_env }} '{{ venture }}' --database '{{ database }}' --state-dir '{{ state }}' --migrate

# Everything under the state directory goes, including the session secret
# and the minted token -- both belong to the database that is being thrown
# away, and keeping either would mean the next start looked healthy while
# authenticating against nothing.

# Delete the database and start over. Stops the server first.
[group('db')]
reset:
    #!/usr/bin/env bash
    set -euo pipefail

    just stop
    rm -rf '{{ state }}'
    echo "Gone. Next start creates a fresh owner account."

# What tables exist, and how many rows in each.
[group('db')]
tables:
    #!/usr/bin/env bash
    set -euo pipefail

    for t in $(sqlite3 '{{ state }}/venture.db' \
        "SELECT name FROM sqlite_master WHERE type='table' ORDER BY name"); do
        printf '%-24s %s\n' "$t" \
            "$(sqlite3 '{{ state }}/venture.db' "SELECT count(*) FROM \"$t\"")"
    done

# ──────────────────────────────────────────────────────────────────────
# Checks
# ──────────────────────────────────────────────────────────────────────

# The whole test suite.
[group('check')]
test:
    @DEBUG=1 make --no-print-directory test

# One test binary: `just test-one test-forge`
[group('check')]
test-one name:
    @DEBUG=1 make --no-print-directory test-one T={{ name }}

# SQLite widens nothing and PostgreSQL widens aggregates, so a suite that
# passes on one proves little about the other.

# Run the suite against PostgreSQL instead of SQLite.
[group('check')]
test-pg uri="postgres://postgres@127.0.0.1/venture_suite_tmp":
    @VENTURE_TEST_DB='{{ uri }}' DEBUG=1 make --no-print-directory test

# ──────────────────────────────────────────────────────────────────────
# Internals (leading underscore: not meant to be called directly)
# ──────────────────────────────────────────────────────────────────────

[private]
_first_run_password:
    @grep -oP 'Password: \K\S+' '{{ log }}' 2>/dev/null | tail -1 \
        | sed 's/^/  owner password: /' || true
