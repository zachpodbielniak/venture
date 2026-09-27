#!/usr/bin/env bash
#
# demo-foreground.sh - `make demo` must hold the port it announces
#
# Copyright (C) 2026 Zach Podbielniak
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# venture-demo.sh used to background the server inside a subshell. `wait`
# only reaps children of the current shell, so it returned at once with
# status 127, the script exited, and the EXIT trap killed the instance.
# The banner named a port nothing was listening on.
#
# What breaks if this regresses: `make demo` returns as soon as the seed
# finishes, and http://127.0.0.1:<port> refuses the connection.

set -euo pipefail

root="$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
demo="${root}/tools/venture-demo.sh"

fail () {
    printf 'not ok %s\n' "$*" >&2
    exit 1
}

[[ -f "${demo}" ]] || fail "missing ${demo}"

stub="$(mktemp -d "${TMPDIR:-/tmp}/venture-demo-foreground-XXXXXX")"

finish () {
    local code=$?

    if [[ -n "${server_pid:-}" ]] && kill -0 "${server_pid}" 2>/dev/null
    then
        kill "${server_pid}" 2>/dev/null || true
        wait "${server_pid}" 2>/dev/null || true
    fi

    if [[ -f "${stub}/launched" ]]
    then
        local launched

        launched="$(cat "${stub}/launched")"
        if [[ -n "${launched}" ]] && kill -0 "${launched}" 2>/dev/null
        then
            kill "${launched}" 2>/dev/null || true
        fi
    fi

    rm -rf "${stub}"
    exit "${code}"
}

mkdir -p "${stub}/bin"
cat > "${stub}/bin/venture" <<'STUB'
#!/usr/bin/env bash
# A stand-in server. It records who spawned it and stays until killed.
printf '%s\n' "$$" > "${VENTURE_DEMO_STUB_DIR}/child-pid"
printf '%s\n' "${PPID}" > "${VENTURE_DEMO_STUB_DIR}/child-ppid"
while true
do
    sleep 1
done
STUB
chmod +x "${stub}/bin/venture"

export VENTURE_DEMO_STUB_DIR="${stub}"

# shellcheck disable=SC1090
source "${demo}"

# Installed after the source: the demo script defines its own cleanup,
# and a trap set earlier would call that and tear the stub down on success.
trap finish EXIT INT TERM

state="${stub}/state"
# Read by start_server in the sourced demo script. ShellCheck cannot see
# that use across the source.
# shellcheck disable=SC2034
outdir="${stub}/bin"
# shellcheck disable=SC2034
base_url="http://127.0.0.1:9"
# shellcheck disable=SC2034
port=9

start_server

waited=0
while [[ ! -s "${stub}/child-ppid" && ${waited} -lt 50 ]]
do
    sleep 0.05
    waited=$((waited + 1))
done

[[ -s "${stub}/child-ppid" ]] || fail "the stub server never wrote its parent pid"
[[ "$(cat "${stub}/child-ppid")" == "$$" ]] \
    || fail "server parent is $(cat "${stub}/child-ppid"), not this script ($$)"
[[ "$(cat "${stub}/child-pid")" == "${server_pid}" ]] \
    || fail "pid file tracks ${server_pid}, stub is $(cat "${stub}/child-pid")"
[[ -f "${state}/venture.pid" ]] || fail "no pid file under the state directory"
[[ "$(cat "${state}/venture.pid")" == "${server_pid}" ]] \
    || fail "state pid file does not match the server"

# `wait` has to block. A pid that is not our child returns 127 at once
# and the EXIT trap would then tear the instance down.
started_ms="$(date +%s%3N)"
(
    sleep 0.4
    kill "${server_pid}" 2>/dev/null || true
) &
killer=$!
status=0
wait "${server_pid}" 2>/dev/null || status=$?
elapsed_ms="$(( $(date +%s%3N) - started_ms ))"
wait "${killer}" 2>/dev/null || true

[[ ${status} -ne 127 ]] || fail "wait returned 127; the server is not a child"
[[ ${elapsed_ms} -ge 300 ]] || fail "wait returned after ${elapsed_ms}ms"
if kill -0 "${server_pid}" 2>/dev/null
then
    fail "wait returned while the server was still running"
fi
server_pid=""

# Detach exits the script on purpose. The server has to outlive that,
# because --stop is a later process and the only thing that should kill it.
cat > "${stub}/launch.sh" <<LAUNCH
#!/usr/bin/env bash
set -euo pipefail
export VENTURE_DEMO_STUB_DIR="${stub}"
# shellcheck disable=SC1090
source "${demo}"
state="${stub}/detach-state"
outdir="${stub}/bin"
base_url="http://127.0.0.1:9"
port=9
start_server
printf '%s\n' "\${server_pid}" > "${stub}/launched"
exit 0
LAUNCH
chmod +x "${stub}/launch.sh"
"${stub}/launch.sh"

launched="$(cat "${stub}/launched")"
[[ -n "${launched}" ]] || fail "detach launch did not record a pid"
kill -0 "${launched}" 2>/dev/null \
    || fail "the server died when the launching script exited"

printf 'ok demo stays a child and outlives the script\n'
