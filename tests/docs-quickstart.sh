#!/usr/bin/env bash
#
# docs-quickstart.sh - Runs the quickstart, for real, and checks each step
#
# Copyright (C) 2026 Zach Podbielniak
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# docs/quickstart.org is a linear path from a built tree to a running
# VENTURE with the demo organisation, an issued invoice, an imported bank
# statement and a closed period. This script is what keeps it honest: it
# pulls every shell block named `qs-*` out of the document, runs them in
# the order they appear, in one shell, and asserts the result of each. A
# block that is in the document but not run here, or run here but not
# asserted, fails the run. tests/test-docs.c drives it under `make test`.
#
# The build step is the one block it does not run -- the suite is built
# by the time this executes -- so it checks that block's products exist
# instead. Everything else is executed exactly as written.
#
# Environment:
#   PORT                where the throwaway instance listens (default 8749)
#   VENTURE_DEMO_STATE  its state directory (default build/demo)
#   BUILD_TYPE          debug (default) or release

set -euo pipefail

root="$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${root}"

doc="docs/quickstart.org"
work="$(mktemp -d "${TMPDIR:-/tmp}/venture-quickstart-run-XXXXXX")"

export PORT="${PORT:-8749}"
export VENTURE_DEMO_STATE="${VENTURE_DEMO_STATE:-${root}/build/demo}"
export BUILD_TYPE="${BUILD_TYPE:-debug}"

say () { printf '%s\n' "$*"; }
fail () { printf 'not ok %s\n' "$*" >&2; exit 1; }

cleanup () {
    local code=$?
    # Whatever happened, the instance this run started must not outlive it.
    tools/venture-demo.sh --stop > /dev/null 2>&1 || true
    rm -rf "${work}"
    exit "${code}"
}
trap cleanup EXIT INT TERM

json () {
    # json FILE-OR-STDIN EXPR: evaluate a python expression against parsed JSON as `j`.
    python3 -c 'import json, sys; j = json.load(sys.stdin); print(eval(sys.argv[1]))' "$1"
}

# ──────────────────────────────────────────────────────────────────────────
# Extract the blocks
# ──────────────────────────────────────────────────────────────────────────

[[ -f "${doc}" ]] || fail "${doc} is missing"

# Every `#+name: NAME` immediately followed by a `#+begin_src sh` block
# becomes ${work}/NN-NAME.sh, numbered in order of appearance.
awk -v dir="${work}" '
    /^#\+name:[ \t]*/ { name = $2; next }
    /^#\+begin_src[ \t]+(sh|bash)/ {
        if (name != "") {
            n++
            file = sprintf("%s/%02d-%s.sh", dir, n, name)
            capturing = 1
        }
        next
    }
    /^#\+end_src/ { capturing = 0; name = ""; next }
    capturing { print > file }
' "${doc}"

blocks=("${work}"/*.sh)
[[ ${#blocks[@]} -gt 0 && -f "${blocks[0]}" ]] || fail "no named shell blocks in ${doc}"

# ──────────────────────────────────────────────────────────────────────────
# The assertions, one per block
# ──────────────────────────────────────────────────────────────────────────

assert_build_native () {
    # Not run: `make DEBUG=1` is what built the tree this is running in.
    # Its products are what the rest of the path needs.
    [[ -x "build/${BUILD_TYPE}/venture" ]] || fail "build-native: build/${BUILD_TYPE}/venture is not built"
    [[ -x "build/${BUILD_TYPE}/venturectl" ]] || fail "build-native: build/${BUILD_TYPE}/venturectl is not built"
}

assert_qs_env () {
    [[ "${VENTURE_SERVER}" == "http://127.0.0.1:${PORT}" ]] || fail "qs-env: VENTURE_SERVER is ${VENTURE_SERVER}"
    command -v venturectl > /dev/null || fail "qs-env: venturectl is not on PATH"
    [[ "$(command -v venturectl)" == "${root}/build/${BUILD_TYPE}/venturectl" ]] \
        || fail "qs-env: PATH found $(command -v venturectl), not the build tree"
}

assert_qs_seed () {
    [[ -n "${VENTURE_TOKEN:-}" ]] || fail "qs-seed: VENTURE_TOKEN is empty"
    [[ "$(venturectl -f json health | json 'j["status"]')" == "ok" ]] || fail "qs-seed: the server is not healthy"
    # The demo organisation is there: ventures, companies, an issued invoice.
    [[ "$(venturectl -f json list venture | json 'len(j["records"])')" -ge 2 ]] || fail "qs-seed: no ventures seeded"
    [[ "$(venturectl -f json list company | json 'len(j["records"])')" -ge 2 ]] || fail "qs-seed: no companies seeded"
    [[ "$(venturectl -f json list invoice status__eq=sent | json 'len(j["records"])')" -ge 1 ]] || fail "qs-seed: no issued invoice seeded"
    [[ "$(venturectl -f json list fiscal_period state__eq=closed | json 'len(j["records"])')" -ge 1 ]] || fail "qs-seed: no closed period seeded"
}

assert_qs_home () {
    local cards
    cards="$(curl -sf -H "Authorization: Bearer ${VENTURE_TOKEN}" \
        "${VENTURE_SERVER}/api/v1/headline?period=this_month")" || fail "qs-home: /api/v1/headline did not answer"
    # The five questions are always there; a module may add a card (billing
    # adds recurring revenue), so the check is a subset, not an equality.
    [[ "$(printf '%s' "${cards}" | json 'isinstance(j, list) and len(j) >= 5')" == "True" ]] || fail "qs-home: expected an array of at least five cards: ${cards}"
    [[ "$(printf '%s' "${cards}" | json '{"cac", "churn", "ltv_cac", "pnl", "support"} <= {c["key"] for c in j}')" == "True" ]] \
        || fail "qs-home: the five question cards are not all there: ${cards}"
    [[ "$(printf '%s' "${cards}" | json 'all(c["value"] for c in j)')" == "True" ]] || fail "qs-home: a card has no value: ${cards}"
    # The home page itself is what a browser gets, behind the sign-in.
    [[ "$(curl -s -o /dev/null -w '%{http_code}' "${VENTURE_SERVER}/")" == "302" ]] || fail "qs-home: / did not redirect to sign in"
}

assert_qs_invoice () {
    [[ -n "${INVOICE:-}" ]] || fail "qs-invoice: INVOICE is empty"
    local invoice
    invoice="$(venturectl -f json get invoice "${INVOICE}")"
    [[ "$(printf '%s' "${invoice}" | json 'j["status"]')" == "sent" ]] || fail "qs-invoice: invoice ${INVOICE} is not issued: ${invoice}"
    [[ "$(printf '%s' "${invoice}" | json 'j["number"]')" == "QS-1" ]] || fail "qs-invoice: wrong number"
    [[ "$(printf '%s' "${invoice}" | json 'j["paid_at"]')" == "None" ]] || fail "qs-invoice: nothing has paid it yet: ${invoice}"
    # Its one line is what the total is made of.
    local lines
    lines="$(venturectl -f json list invoice_line "invoice_id__eq=${INVOICE}")"
    [[ "$(printf '%s' "${lines}" | json 'len(j["records"])')" -eq 1 ]] || fail "qs-invoice: expected one line: ${lines}"
    [[ "$(printf '%s' "${lines}" | json 'j["records"][0]["unit_price"]["formatted"]')" == "250.00 USD" ]] || fail "qs-invoice: wrong line price: ${lines}"
}

assert_qs_bank () {
    [[ -n "${STATEMENT:-}" ]] || fail "qs-bank: STATEMENT is empty"
    local statement lines invoice
    statement="$(venturectl -f json get bank_statement "${STATEMENT}")"
    [[ "$(printf '%s' "${statement}" | json 'j["closing_balance"]["formatted"]')" == "1250.00 USD" ]] \
        || fail "qs-bank: statement not imported as written: ${statement}"
    lines="$(venturectl -f json list bank_transaction "statement_id__eq=${STATEMENT}")"
    [[ "$(printf '%s' "${lines}" | json 'len(j["records"])')" -eq 1 ]] || fail "qs-bank: expected one bank line"
    [[ "$(printf '%s' "${lines}" | json 'j["records"][0]["state"]')" == "matched" ]] || fail "qs-bank: the line was not matched: ${lines}"
    # The receipt created from the line settled the invoice.
    invoice="$(venturectl -f json get invoice "${INVOICE}")"
    [[ "$(printf '%s' "${invoice}" | json 'j["status"]')" == "paid" ]] || fail "qs-bank: invoice ${INVOICE} is not paid: ${invoice}"
    [[ "$(printf '%s' "${invoice}" | json 'bool(j["paid_at"])')" == "True" ]] || fail "qs-bank: no paid_at stamp: ${invoice}"
}

assert_qs_close () {
    [[ -n "${PERIOD:-}" ]] || fail "qs-close: PERIOD is empty"
    local period
    period="$(venturectl -f json get fiscal_period "${PERIOD}")"
    [[ "$(printf '%s' "${period}" | json 'j["state"]')" == "closed" ]] || fail "qs-close: period ${PERIOD} is not closed: ${period}"
    [[ "$(printf '%s' "${period}" | json 'bool(j.get("closed_at"))')" == "True" ]] || fail "qs-close: no close stamp"
}

assert_qs_stop () {
    if curl -s --max-time 2 -o /dev/null "${VENTURE_SERVER}/api/v1/health"; then
        fail "qs-stop: the instance is still answering"
    fi
    [[ ! -d "${VENTURE_DEMO_STATE}" ]] || fail "qs-stop: ${VENTURE_DEMO_STATE} was not removed"
}

# ──────────────────────────────────────────────────────────────────────────
# Run them, in order, in this shell
# ──────────────────────────────────────────────────────────────────────────

for block in "${blocks[@]}"; do
    name="$(basename "${block}" .sh)"
    name="${name#[0-9][0-9]-}"
    assertion="assert_${name//-/_}"

    declare -F "${assertion}" > /dev/null || fail "${name}: the quickstart has a block this script does not assert"

    case "${name}" in
        build-*)
            say "skip ${name} (already built; products checked)"
            ;;
        qs-*)
            # shellcheck disable=SC1090
            if ! source "${block}" > "${work}/${name}.out" 2>&1; then
                cat "${work}/${name}.out" >&2
                fail "${name}: a command in the block failed"
            fi
            ;;
        *)
            fail "${name}: blocks must be named build-* or qs-*"
            ;;
    esac

    "${assertion}"
    say "ok ${name}"
done

say "quickstart: every step ran and held"
