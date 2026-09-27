#!/usr/bin/env bash
#
# venture-demo.sh - A throwaway VENTURE instance with enough data to show it off
#
# Copyright (C) 2026 Zach Podbielniak
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Builds nothing: `make demo` does that and then calls this. What this does
# is start a server against a state directory it owns and can delete, seed a
# small but complete business into it -- two ventures, a year of sales and
# expenses, invoices, a pipeline, a support desk with service levels and a
# sprint, a release that went out and an incident that followed -- plus a
# second organization that trades inside a game world in its own gold, and
# tell you how to sign in.
#
# The database is thrown away and rebuilt on every run. That is the point:
# the demo is the same demo every time, and nothing you do to it matters.

set -euo pipefail

readonly VERSION="1.0.0"

# 8749, not 8747 or 8748.
#
# 8747 is the configuration default and where a containerised instance from
# compose.yaml listens; 8748 is where `just start` puts the build tree. A
# demo that quietly bound to either would be a demo that either refused to
# start or, far worse, seeded example data into something real.
readonly DEFAULT_PORT=8749

# CDPATH='' on both, and cd -- to end the options.
#
# A `cd` that resolves through CDPATH prints the directory it landed in,
# which inside a command substitution becomes part of the answer: with
# CDPATH set -- and it is set in any shell configured to jump around by
# name -- this came back as two lines, and every path built from it was
# nonsense. It cost an afternoon somewhere, so it is spelled out here.
root="$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
port="${PORT:-${DEFAULT_PORT}}"
build_type="${BUILD_TYPE:-debug}"
outdir=""
state=""
# Where the instance lives. build/demo unless --state or VENTURE_DEMO_STATE
# says otherwise -- the test suite runs the quickstart against a private
# directory so it can never stop or delete a demo somebody is looking at.
state_override="${VENTURE_DEMO_STATE:-}"
detach="false"
action="start"

server_pid=""
home_org=""
token=""
password=""
base_url=""

# ──────────────────────────────────────────────────────────────────────────
# Saying things
# ──────────────────────────────────────────────────────────────────────────

if [[ -t 1 ]]
then
    readonly BOLD=$'\e[1m' DIM=$'\e[2m' RED=$'\e[31m' GREEN=$'\e[32m' OFF=$'\e[0m'
else
    readonly BOLD='' DIM='' RED='' GREEN='' OFF=''
fi

say () {
    printf '%s\n' "$*"
}

# Progress goes to stderr, deliberately.
#
# Several of the seeding functions below hand an id back on stdout, and a
# progress line printed there becomes part of the answer: the first run of
# this tried to create a company called "==>". Anything that is not a
# return value belongs on stderr.
step () {
    printf '%s==>%s %s\n' "${GREEN}" "${OFF}" "$*" >&2
}

die () {
    printf '%sdemo:%s %s\n' "${RED}" "${OFF}" "$*" >&2
    exit 1
}

usage () {
    cat <<'USAGE'
venture-demo.sh - a throwaway VENTURE instance with example data

Usage:
  tools/venture-demo.sh [options]

Options:
  -p, --port PORT     Listen on PORT (default 8749)
  -d, --detach        Seed it, print how to reach it, and leave it running
  -s, --stop          Stop a detached demo and remove its data
      --state DIR     Keep the instance under DIR instead of build/demo
  -h, --help          This
      --license       Licence and copyright

Environment:
  PORT                Same as --port
  VENTURE_DEMO_STATE  Same as --state
  BUILD_TYPE          debug (default) or release -- which build tree to run

Examples:
  make demo                       # build, seed, run in the foreground
  tools/venture-demo.sh           # the same, without building first
  tools/venture-demo.sh -d        # leave it running in the background
  tools/venture-demo.sh --stop    # stop that one and delete its data
  tools/venture-demo.sh -p 9100   # somewhere else

What it seeds:
  Two ventures and a year of trade, posted through to a general ledger.
  Receivables and payables with their credits and refunds. A bank
  statement, imported and matched. A fiscal year with its first quarter
  closed. Fixed assets and deferrals. Leads, activities, quotes and a
  sales pipeline the deals have actually been moved through.
  Subscriptions, a dunning ladder and a follow-up sequence. A support
  desk, a sprint, a release and the incident that followed.

  Stripe and federation stay off: one needs an account, the other a
  second server.

Everything lives under build/demo and is deleted and rebuilt on every run.
USAGE
}

licence () {
    cat <<'LICENCE'
venture-demo.sh, part of VENTURE.
Copyright (C) 2026 Zach Podbielniak

This program is free software: you can redistribute it and/or modify it
under the terms of the GNU Affero General Public License as published by
the Free Software Foundation, either version 3 of the License, or (at your
option) any later version. It is distributed WITHOUT ANY WARRANTY; without
even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
PURPOSE. See <https://www.gnu.org/licenses/> for the full text.
LICENCE
}

# ──────────────────────────────────────────────────────────────────────────
# Arguments
# ──────────────────────────────────────────────────────────────────────────

parse_arguments () {
    while [[ $# -gt 0 ]]
    do
        case "$1" in
            -p|--port)
                [[ $# -ge 2 ]] || die "--port needs a number"
                port="$2"
                shift 2
                ;;
            -d|--detach)
                detach="true"
                shift
                ;;
            -s|--stop)
                action="stop"
                shift
                ;;
            --state)
                [[ $# -ge 2 ]] || die "--state needs a directory"
                state_override="$2"
                shift 2
                ;;
            -h|--help)
                usage
                exit 0
                ;;
            --license|--licence)
                licence
                exit 0
                ;;
            --version)
                say "venture-demo.sh ${VERSION}"
                exit 0
                ;;
            *)
                die "unknown option \"$1\". Try --help."
                ;;
        esac
    done

    [[ "${port}" =~ ^[0-9]+$ ]] || die "\"${port}\" is not a port number"
    [[ "${port}" -ge 1 && "${port}" -le 65535 ]] || die "port ${port} is out of range"
}

# ──────────────────────────────────────────────────────────────────────────
# The environment this needs
# ──────────────────────────────────────────────────────────────────────────

resolve_paths () {
    outdir="${root}/build/${build_type}"
    state="${state_override:-${root}/build/demo}"
    base_url="http://127.0.0.1:${port}"
}

require_tools () {
    local missing=()
    local tool

    for tool in curl python3
    do
        command -v "${tool}" > /dev/null 2>&1 || missing+=("${tool}")
    done

    if [[ ${#missing[@]} -gt 0 ]]
    then
        die "this needs ${missing[*]}, which ${#missing[@]} of are not on PATH"
    fi
}

require_binaries () {
    local binary

    for binary in venture venturectl
    do
        if [[ ! -x "${outdir}/${binary}" ]]
        then
            die "${outdir}/${binary} is not there. Run \`make DEBUG=1\` first, or \`make demo\`, which does."
        fi
    done
}

# Whether anything at all is listening on the port.
port_is_taken () {
    curl --silent --show-error --max-time 2 --output /dev/null \
         "${base_url}/api/v1/health" 2>/dev/null
}

# A demo we started earlier, still running.
previous_pid () {
    local pidfile="${state}/venture.pid"
    local pid

    [[ -f "${pidfile}" ]] || return 1
    pid="$(cat "${pidfile}" 2>/dev/null || true)"
    [[ -n "${pid}" ]] || return 1
    kill -0 "${pid}" 2>/dev/null || return 1

    printf '%s' "${pid}"
}

stop_previous () {
    local pid

    if pid="$(previous_pid)"
    then
        step "Stopping the demo already running as pid ${pid}"
        kill "${pid}" 2>/dev/null || true

        local waited=0

        while kill -0 "${pid}" 2>/dev/null && [[ ${waited} -lt 50 ]]
        do
            sleep 0.1
            waited=$((waited + 1))
        done

        kill -9 "${pid}" 2>/dev/null || true
    fi
}

# ──────────────────────────────────────────────────────────────────────────
# Starting and stopping
# ──────────────────────────────────────────────────────────────────────────

stop_server () {
    if [[ -n "${server_pid}" ]] && kill -0 "${server_pid}" 2>/dev/null
    then
        kill "${server_pid}" 2>/dev/null || true
        wait "${server_pid}" 2>/dev/null || true
    fi
}

# Called on any exit while the demo is being built. Once it is up and we
# have decided to detach, the trap is cleared so the server survives.
cleanup () {
    local code=$?

    if [[ ${code} -ne 0 ]]
    then
        printf '\n%sThe demo did not come up.%s' "${RED}" "${OFF}" >&2

        if [[ -s "${state}/server.log" ]]
        then
            printf ' The last of %s:\n\n' "${state}/server.log" >&2
            tail -n 15 "${state}/server.log" >&2
            printf '\n' >&2
        else
            printf '\n' >&2
        fi
    fi

    stop_server
    exit "${code}"
}

start_server () {
    step "Starting a fresh instance on ${base_url}"

    rm -rf "${state}"
    mkdir -p "${state}"

    # Somewhere for a harness session to work. Without a root configured
    # a session may only use a checkout it clones itself, which makes the
    # page look broken the moment anyone tries it.
    #
    # Two roots, for the two things a person does here. The scratch one
    # inside build/demo goes away with the rest of the demo. This
    # checkout is the second, because a harness demonstrated against an
    # empty directory demonstrates nothing -- the point is to point an
    # agent at real code and watch it work.
    #
    # That does mean a session can edit this working tree. It is a
    # checkout under version control, which is the protection; commit or
    # stash before turning an agent loose on it. Override the pair with
    # VENTURE_FORGE_WORKSPACE_ROOTS to keep the demo away from it.
    mkdir -p "${state}/workspace"

    # The server has to be a child of this script.
    #
    # `wait` only reaps children. Backgrounding venture *inside* a
    # subshell records a pid this shell did not spawn: the subshell
    # returns, the server is reparented, and `wait` comes back at once
    # with status 127. This script then exits, the EXIT trap treats that
    # as the demo ending, and it kills the server it just announced. The
    # port in the banner is dead before anyone can open it.
    #
    # Background the subshell itself and exec the server into it, so the
    # pid we wait on stays the server. The working directory is the
    # checkout because the server resolves a few paths from there. Every
    # path passed in is absolute; the cd is for what it opens itself.
    (
        CDPATH='' cd -- "${root}"
        export VENTURE_PLUGIN_PATH="${outdir}/plugins"
        export VENTURE_VENTURE_TYPE_PATH="${root}/data/venture-types"
        export VENTURE_POD_MODULE_PATH="${outdir}/pod-modules:${root}/deps/podomation/build/${build_type}/modules"
        export VENTURE_UI_THEME="${VENTURE_UI_THEME:-mocha}"
        export VENTURE_SESSION_SECRET="venture-demo-secret"
        export VENTURE_FORGE_RUNS_ENABLED="true"
        export VENTURE_FORGE_WORKSPACE_ROOTS="${VENTURE_FORGE_WORKSPACE_ROOTS:-${state}/workspace,${root}}"
        export VENTURE_DOCS_SITE_DIR="${VENTURE_DOCS_SITE_DIR:-${root}/build/docs-site}"
        exec "${outdir}/venture" \
            --database "sqlite://${state}/venture.db" \
            --state-dir "${state}" \
            --port "${port}"
    ) > "${state}/server.log" 2>&1 &

    server_pid=$!
    printf '%s\n' "${server_pid}" > "${state}/venture.pid"
}

wait_for_health () {
    local waited=0

    while [[ ${waited} -lt 200 ]]
    do
        if ! kill -0 "${server_pid}" 2>/dev/null
        then
            die "the server exited while starting up"
        fi

        if curl --silent --max-time 2 --output /dev/null --fail \
                "${base_url}/api/v1/health" 2>/dev/null
        then
            return 0
        fi

        sleep 0.1
        waited=$((waited + 1))
    done

    die "the server did not answer on ${base_url} within 20 seconds"
}

# The owner password is printed once, at first start, and never again.
read_owner_password () {
    password="$(sed -n 's/^ *Password: *//p' "${state}/server.log" | head -n 1)"

    [[ -n "${password}" ]] \
        || die "could not find the owner password in ${state}/server.log"
}

# A token, so the seeding below is ordinary API traffic rather than a
# special path into the database.
mint_token () {
    local jar="${state}/cookies.txt"
    local body

    curl --silent --show-error --max-time 10 --cookie-jar "${jar}" \
         --output /dev/null \
         --data-urlencode "username=owner" \
         --data-urlencode "password=${password}" \
         "${base_url}/login" \
        || die "could not sign in as owner"

    body="$(curl --silent --show-error --max-time 10 --cookie "${jar}" \
                 --header 'Content-Type: application/json' \
                 --data '{"name":"demo-seed","role":"owner"}' \
                 "${base_url}/api/v1/tokens")" \
        || die "could not mint an API token"

    token="$(printf '%s' "${body}" | python3 -c \
        'import json,sys; print(json.load(sys.stdin).get("token",""))' 2>/dev/null || true)"

    [[ -n "${token}" ]] || die "the server did not return a token: ${body}"

    rm -f "${jar}"

    # Kept, so the quickstart can pick it up with one line rather than
    # minting a second one by hand. It is a full owner token to a throwaway
    # instance, in a directory that is deleted on the next run.
    (
        umask 077
        printf '%s\n' "${token}" > "${state}/token"
    )
}

# ──────────────────────────────────────────────────────────────────────────
# Seeding
#
# Every record goes in through venturectl, which goes in through the REST
# API, which is the same door a person or an agent uses. Nothing here
# reaches past the server into the database, so a demo that seeds is also a
# demo that the API can build.
# ──────────────────────────────────────────────────────────────────────────

ctl () {
    "${outdir}/venturectl" --server "${base_url}" --token "${token}" "$@"
}

# Creates one record and prints its id. Dies naming what it was making,
# because "venturectl: exit 4" fifty lines into a seed is not a diagnosis.
make_record () {
    local type="$1"
    shift

    local output
    local id

    if ! output="$(ctl --format json create "${type}" "$@" 2>&1)"
    then
        die "could not create the ${type} (${1:-}): ${output}"
    fi

    id="$(printf '%s' "${output}" | python3 -c \
        'import json,sys
try:
    print(json.load(sys.stdin).get("id", ""))
except Exception:
    pass' 2>/dev/null || true)"

    [[ -n "${id}" ]] || die "the server did not return an id for the ${type}"

    printf '%s' "${id}"
}

# The same, for a record whose id nothing later needs.
add () {
    make_record "$@" > /dev/null
}

# A date N days from today, as the API wants it.
day () {
    date -u -d "$1 days" +%Y-%m-%d
}

# The first of the month N months back, for spreading a year of trade.
month_start () {
    date -u -d "$(date -u +%Y-%m-01) -$1 months" +%Y-%m-%d
}

# A timestamp for the financial services, which refuse an event dated
# before the document it settles or after the present moment. A bill
# approved during this run can therefore only be paid now.
now () {
    # The services compare against sub-second timestamps: an event
    # stamped at 12:00:01.8 is not preceded by "12:00:01", which is what
    # truncating to the second would send. Waiting a second first means
    # the truncated value is always strictly after the event and still
    # not in the future.
    sleep 1
    date -u +%Y-%m-%dT%H:%M:%SZ
}

# The default organisation's id: the press and the studio's entity.
#
# Every lookup below that reads the books is scoped to it. The demo also
# seeds a second organisation, whose sales post to a chart of its own, and
# an unscoped "the account with code 1000" is then whichever of the two
# the list happens to put first.
default_organization () {
    ctl --format json list organization is_default=true 2>/dev/null | python3 -c \
        'import json, sys
try:
    rows = json.load(sys.stdin).get("records", [])
    print(rows[0]["id"] if rows else "")
except Exception:
    pass' 2>/dev/null || true
}

# The id of an account in the chart every organisation starts with, by
# code. Seeding looks accounts up rather than creating them so that the
# demo's books are the ones a new install already has.
account_id () {
    local code="$1"

    ctl --format json list account "code=${code}" "organization_id=${home_org}" 2>/dev/null | python3 -c \
        'import json, sys
try:
    rows = json.load(sys.stdin).get("records", [])
    print(rows[0]["id"] if rows else "")
except Exception:
    pass' 2>/dev/null || true
}

seed_business () {
    step "Two ventures, their products, and a year of trade"

    local press
    local studio
    local book
    local app
    local i

    press="$(make_record venture name="Wrenmouth Press" slug=wrenmouth \
        venture_type=books status=active priority=high \
        description="A small press. Six titles a year, sold direct and through two shops." \
        started_at="$(day -700)")"
    studio="$(make_record venture name="Harrow Studio" slug=harrow \
        venture_type=newsletter status=active priority=high \
        description="One product, sold as a subscription. This is the one with the tickets." \
        started_at="$(day -420)")"

    book="$(make_record product venture_id="${press}" name="The Long Field" \
        sku=WP-014 category=Fiction format=Paperback list_price=14.99 cost=4.20 \
        active=true)"
    app="$(make_record product venture_id="${studio}" name="Harrow Studio, yearly" \
        sku=HS-Y category=Software list_price=180.00 cost=0.00 active=true)"

    add product venture_id="${press}" name="The Long Field, hardback" sku=WP-014H \
        category=Fiction format=Hardback list_price=24.99 cost=8.90 active=true
    add product venture_id="${studio}" name="Harrow Studio, monthly" sku=HS-M \
        category=Software list_price=18.00 cost=0.00 active=true

    # A year of trade, heavier lately, so the monthly report has a shape
    # rather than a flat line.
    for i in 11 10 9 8 7 6 5 4 3 2 1 0
    do
        local when
        local books
        local seats

        when="$(month_start "${i}")"
        books=$(( 18 + (11 - i) * 4 ))
        seats=$(( 3 + (11 - i) ))

        add sale venture_id="${press}" product_id="${book}" occurred_at="${when}" \
            quantity="${books}" gross="$(( books * 1499 / 100 )).00" \
            fees="$(( books * 2 )).00" channel=direct country=GB
        add sale venture_id="${studio}" product_id="${app}" occurred_at="${when}" \
            quantity="${seats}" gross="$(( seats * 180 )).00" \
            fees="$(( seats * 5 )).00" channel=web country=GB

        add expense venture_id="${studio}" description="Hosting" vendor="Hosting Co" \
            amount=42.00 occurred_at="${when}" category=software deductibility=full \
            payment_method=card
    done

    add expense venture_id="${press}" description="Print run, 500 copies" \
        vendor="Marlow Print" amount=1850.00 occurred_at="$(month_start 6)" \
        category=inventory deductibility=full payment_method=transfer
    add expense venture_id="${press}" description="Cover illustration" \
        vendor="J. Attwell" amount=600.00 occurred_at="$(month_start 7)" \
        category=contractor deductibility=full payment_method=transfer
    add expense venture_id="${studio}" description="Laptop" vendor="Refurb Direct" \
        amount=1100.00 occurred_at="$(month_start 4)" category=equipment \
        deductibility=full business_use_percent=80 payment_method=card

    printf '%s %s %s' "${press}" "${studio}" "${book}"
}

seed_relations () {
    local press="$1"
    local studio="$2"

    step "Companies, contacts, a pipeline and two invoices"

    local shop
    local agency
    local buyer

    shop="$(make_record company name="Bellhaven Books" kind=customer \
        industry=Retail email=orders@bellhaven.example venture_id="${press}" \
        source=referral active=true)"
    agency="$(make_record company name="Cold Harbour Agency" kind=customer \
        industry=Marketing email=accounts@coldharbour.example \
        venture_id="${studio}" source=web active=true)"
    add company name="Marlow Print" kind=supplier industry=Printing \
        venture_id="${press}" active=true

    # A tax rate to pick on an invoice line, and a customer who pays none:
    # choosing the library on the invoice sheet fills in its exemption.
    add tax_code code=NY-SALES name="New York sales tax" jurisdiction=US-NY \
        rate_numerator=8875 rate_denominator=100000 active=true
    add company name="Harbour Library Trust" kind=customer industry=Nonprofit \
        email=accounts@harbourlibrary.example venture_id="${press}" active=true \
        tax_exempt=true tax_exempt_reason="Non-profit (501(c)(3))" \
        tax_exemption_number=EX-4471

    buyer="$(make_record contact name="Ruth Ellery" email=ruth@bellhaven.example \
        company_id="${shop}" role="Buyer" venture_id="${press}" source=referral)"
    add contact name="Sam Okonjo" email=sam@coldharbour.example \
        company_id="${agency}" role="Operations" venture_id="${studio}" source=web

    add deal name="Bellhaven, spring order" company_id="${shop}" \
        contact_id="${buyer}" venture_id="${press}" stage=proposal value=2400.00 \
        probability=60 expected_close_at="$(day 21)"
    add deal name="Cold Harbour, ten seats" company_id="${agency}" \
        venture_id="${studio}" stage=negotiation value=1800.00 probability=75 \
        expected_close_at="$(day 9)"
    add deal name="Thorne & Sons" venture_id="${press}" stage=qualified \
        value=900.00 probability=40 expected_close_at="$(day 45)"
    add deal name="Ardley Trust" venture_id="${studio}" stage=won value=3600.00 \
        probability=100 closed_at="$(day -30)"

    add interaction contact_id="${buyer}" company_id="${shop}" kind=call \
        subject="Spring order" occurred_at="$(day -3)" outbound=true \
        body="Wants 160 paperbacks for March. Sending a quote."

    local paid
    local owing

    # An invoice is drafted, given its lines, and only then issued. Its
    # financial state is derived from what has been allocated against it,
    # so "paid" is not a word to be written here -- the receipt in
    # seed_receivables is what makes it true.
    paid="$(make_record invoice number=INV-0041 status=draft company_id="${shop}" \
        contact_id="${buyer}" venture_id="${press}" issued_at="$(day -52)" \
        due_at="$(day -22)" terms="30 days")"
    owing="$(make_record invoice number=INV-0042 status=draft company_id="${agency}" \
        venture_id="${studio}" issued_at="$(day -48)" due_at="$(day -18)" \
        terms="30 days")"

    add invoice_line invoice_id="${paid}" description="Paperbacks, 120" \
        quantity=120 unit_price=9.50 position=1
    add invoice_line invoice_id="${paid}" description="Carriage" quantity=1 \
        unit_price=45.00 position=2
    add invoice_line invoice_id="${owing}" description="Ten seats, one year" \
        quantity=10 unit_price=180.00 position=1

    ctl update invoice "${paid}" status=sent > /dev/null \
        || die "could not issue INV-0041"
    ctl update invoice "${owing}" status=sent > /dev/null \
        || die "could not issue INV-0042"

    add campaign name="Spring catalogue" venture_id="${press}" status=running \
        channel=email started_at="$(day -20)" budget=400.00 spend=280.00 \
        revenue=1900.00 impressions=4200 clicks=310 conversions=41
    add idea title="A subscription box for the press" status=researching \
        priority=normal opportunity=7 confidence=5 effort=6 \
        estimated_revenue=12000.00 \
        description="Four titles a year, posted. Would smooth the cash flow."
    printf '%s %s %s %s %s' "${shop}" "${agency}" "${buyer}" "${paid}" "${owing}"
}

# ──────────────────────────────────────────────────────────────────────────
# The books
#
# Every organisation starts with a chart of accounts and nothing to say
# about it. What the ledger needs before it can draw a trial balance is a
# posting profile: the map from what a business does -- a sale, a fee, a
# refund -- to the account it lands in. With one in place the services
# post as documents move, so the general ledger below is written by the
# trade rather than typed in beside it.
# ──────────────────────────────────────────────────────────────────────────

seed_books () {
    step "A chart of accounts, a posting profile and an opening balance"

    local org
    local journal

    # Four accounts the default chart does not carry, because they only
    # matter once there are assets to depreciate and rent paid in advance.
    # 1300 and 6800 are not free: the default chart seeds them as
    # recoverable tax and bad debt, so these sit beside them instead.
    add account code=1400 name="Prepaid expenses" kind=asset active=true
    add account code=1500 name="Equipment" kind=asset active=true
    add account code=1590 name="Accumulated depreciation" kind=asset active=true
    add account code=6850 name="Depreciation" kind=expense active=true

    # The posting profile is deliberately not written here. The
    # autojournal service builds one the first time it posts anything,
    # creating the accounts it needs and the full category map along with
    # it. A hand-written profile that omits a field -- the category map,
    # say -- is found by the service, used, and fails every expense, so
    # the right move is to let the service own it.

    # A journal always names the document it came from. An entry that is
    # nobody's invoice -- the money the owner put in to start -- names the
    # legal entity itself, which is the only source a manual adjustment
    # legitimately has.
    org="${home_org}"

    [[ -n "${org}" ]] || die "the server has no organisation to post against"

    journal="$(make_record journal memo="Owner's opening capital" state=draft \
        occurred_at="$(month_start 11)" currency=USD \
        source_type=organization source_id="${org}")"

    add journal_line journal_id="${journal}" account_id="$(account_id 1000)" \
        side=debit amount=5000.00 memo="Opening float"
    add journal_line journal_id="${journal}" account_id="$(account_id 3000)" \
        side=credit amount=5000.00 memo="Capital introduced"

    ctl journal post "${journal}" > /dev/null \
        || die "could not post the opening journal"
}

# Anything the services did not post as it happened -- a document written
# before the profile existed, or one whose rule has since been added --
# is caught here, at the end of seeding.
seed_ledger_catchup () {
    step "Posting whatever the trade left unposted"

    ctl post backfill > /dev/null \
        || die "could not backfill the general ledger"
}

# A fiscal calendar, so the period reports have boundaries to respect and
# the closing controls have something to close.
seed_periods () {
    step "A fiscal year, and the first quarter closed"

    local periods
    local period

    # The periods are generated from the year, so this one record is the
    # whole calendar.
    add fiscal_year name="FY$(date -u +%Y)" start_at="$(date -u +%Y)-01-01" \
        end_at="$(( $(date -u +%Y) + 1 ))-01-01" state=open period_length=monthly

    # Closing a period freezes the reports as they stood and refuses any
    # later posting dated inside it, which is why this runs after every
    # document has been written and posted. The first quarter is closed
    # and the rest left open, so the demo has both.
    periods="$(ctl --format json list fiscal_period limit=12 "organization_id=${home_org}" 2>/dev/null \
        | python3 -c \
        'import json, sys
try:
    rows = json.load(sys.stdin).get("records", [])
    rows.sort(key=lambda r: r["start_at"])
    print(" ".join(str(r["id"]) for r in rows[:3]))
except Exception:
    pass' 2>/dev/null || true)"

    for period in ${periods}
    do
        ctl update fiscal_period "${period}" state=closed > /dev/null \
            || die "could not close fiscal period ${period}"
    done
}

# ──────────────────────────────────────────────────────────────────────────
# Money in, money out, and the bank
#
# An invoice is not marked paid; it becomes paid because a receipt was
# allocated to it. A bill is not marked settled; it is approved and then
# paid. Both services refuse to have their derived fields written
# directly, so the seeding below moves the documents the way an operator
# would and lets the states follow.
# ──────────────────────────────────────────────────────────────────────────

seed_receivables () {
    local shop="$1"
    local agency="$2"
    local paid="$3"

    step "A receipt, a credit note and a refund"

    local receipt
    local credit

    # The receipt that settles INV-0041. The allocation, the cash posting
    # and the invoice's paid state all follow from this one record.
    receipt="$(make_record payment customer_id="${shop}" date="$(day -19)" \
        amount=1185.00 method=transfer invoice_id="${paid}" \
        reference=BACS-4471)"

    # A credit note raised for the agency, part of which is handed back
    # in cash. The rest stays on the account, so the receivables report
    # has both an open credit and a refund against one.
    credit="$(make_record customer_credit customer_id="${agency}" \
        date="$(day -14)" amount=90.00 kind=credit_note \
        reference="Two seats, short month")"

    add refund customer_id="${agency}" credit_id="${credit}" \
        date="$(day -11)" amount=40.00 reference=REF-0001

    # The bank feed needs the receipt this created in order to match
    # against it.
    printf '%s' "${receipt}"
}

seed_payables () {
    local press="$1"
    local studio="$2"

    step "Supplier bills, approved and part paid"

    local printer
    local host
    local bill
    local small
    local credit

    printer="$(make_record company name="Marlow Print" kind=supplier \
        industry=Printing venture_id="${press}" active=true \
        email=accounts@marlowprint.example)"
    host="$(make_record company name="Hosting Co" kind=supplier \
        industry=Software venture_id="${studio}" active=true \
        email=billing@hosting.example)"

    bill="$(make_record vendor_bill company_id="${printer}" number=MP-3312 \
        bill_date="$(day -40)" due_date="$(day -10)" currency=USD status=draft \
        venture_id="${press}" memo="Print run, 500 copies")"
    add vendor_bill_line bill_id="${bill}" description="Print run, 500 copies" \
        quantity=1 unit_price=1850.00 account_id="$(account_id 5000)" position=1

    small="$(make_record vendor_bill company_id="${host}" number=HC-9080 \
        bill_date="$(day -12)" due_date="$(day 18)" currency=USD status=draft \
        venture_id="${studio}" memo="Hosting, this month")"
    add vendor_bill_line bill_id="${small}" description="Hosting" quantity=1 \
        unit_price=42.00 account_id="$(account_id 6300)" position=1

    ctl bill approve "${bill}" > /dev/null || die "could not approve bill MP-3312"
    ctl bill approve "${small}" > /dev/null || die "could not approve bill HC-9080"

    # Part of the printer's bill is paid, which leaves the rest on the
    # aging report where a demo can see it. A payables event cannot be
    # dated before the approval it follows, and the approval is now.
    ctl bill pay "${bill}" amount=1000.00 date="$(now)" method=transfer \
        reference=BACS-77 > /dev/null || die "could not pay bill MP-3312"

    # The printer over-charged for carriage on an earlier job and owes it
    # back; an unspent vendor credit is the other half of payables.
    credit="$(make_record vendor_credit vendor_id="${printer}" \
        date="$(day -20)" amount=65.00 kind=credit_note \
        reference="Carriage overcharged, job MP-3280")"

    add bill_refund vendor_id="${printer}" credit_id="${credit}" \
        date="$(day -15)" amount=25.00 reference=VREF-0001
}

# An imported transaction's id, by the reference its statement row
# carried. The ids are the service's to hand out, so they are read back
# rather than assumed.
transaction_by_reference () {
    local statement="$1"
    local reference="$2"
    local id

    id="$(ctl --format json list bank_transaction "statement_id=${statement}" \
        2>/dev/null | python3 -c \
        'import json, sys
try:
    rows = json.load(sys.stdin).get("records", [])
    wanted = sys.argv[1]
    print(next(str(r["id"]) for r in rows if r.get("reference") == wanted))
except Exception:
    pass' "${reference}" 2>/dev/null || true)"

    [[ -n "${id}" ]] || die "no imported transaction references ${reference}"

    printf '%s' "${id}"
}

seed_banking () {
    local receipt="$1"

    step "A bank feed, matched against what the books already say"

    local bank
    local statement
    local feed

    bank="$(make_record bank_account name="Current account" \
        account_id="$(account_id 1000)" currency=USD \
        date_column=Date amount_column=Amount description_column=Description \
        reference_column=Reference external_id_column=FitID \
        date_format="%Y-%m-%d" sign_convention=normal)"

    # A statement is imported, never written. The rows below are the same
    # money the receipt and the hosting expense already record, so the
    # matcher has something true to find: equal amounts, within three
    # days, exactly one candidate each. The bank charge matches nothing,
    # which is the interesting row -- a demo should show the one that
    # still needs a person.
    feed="${state}/bank-feed.json"

    python3 -c 'import json, sys
rows = [
    (sys.argv[1], "1185.00", "BELLHAVEN BOOKS", "BACS-4471", "FIT-8801"),
    (sys.argv[2], "-42.00", "HOSTING CO DD", "DD-0091", "FIT-8802"),
    (sys.argv[3], "-18.00", "ACCOUNT FEE", "FEE-03", "FIT-8803"),
]
data = "Date,Amount,Description,Reference,FitID\n" + \
    "\n".join(",".join(row) for row in rows)
print(json.dumps({
    "format": "csv", "data": data,
    "period_start": sys.argv[4], "period_end": sys.argv[5],
    "opening_balance": "1000.00", "closing_balance": "2125.00",
}))' "$(day -19)" "$(month_start 0)" "$(day -8)" "$(day -35)" "$(day -5)" \
        > "${feed}" || die "could not write the bank feed"

    statement="$(ctl --format json bank import "${bank}" "@${feed}" 2>/dev/null \
        | python3 -c \
        'import json, sys
try:
    print(json.load(sys.stdin).get("id", ""))
except Exception:
    pass' 2>/dev/null || true)"

    [[ -n "${statement}" ]] || die "could not import the bank statement"

    ctl bank auto "${statement}" > /dev/null \
        || die "could not match the statement"

    # The hosting charge matches by itself. The customer receipt does
    # not, and correctly so: allocating a receipt also writes a derived
    # cash-revenue sale of the same amount on the same day, so the
    # matcher sees two candidates and declines to guess. Confirming it
    # is exactly the judgement a person is there to make.
    printf '{"parts":[{"type":"payment","id":%s}]}' "${receipt}" \
        > "${state}/bank-match.json"
    ctl bank match "$(transaction_by_reference "${statement}" BACS-4471)" \
        "@${state}/bank-match.json" > /dev/null \
        || die "could not confirm the receipt against the bank feed"

    # The account fee answers to no document, so it is excluded with a
    # reason rather than left to rot as unmatched.
    printf '{"reason":"Monthly account fee; no document to match."}' \
        > "${state}/bank-exclude.json"
    ctl bank exclude "$(transaction_by_reference "${statement}" FEE-03)" \
        "@${state}/bank-exclude.json" > /dev/null \
        || die "could not exclude the account fee"

    # Left open on purpose. The feed is a month of three rows against a
    # year of trade, so the statement and the posted cash balance differ;
    # an open reconciliation records that difference, which is what the
    # record is for and what month end actually looks like before the
    # work is done.
    printf '{"state":"open"}' > "${state}/bank-reconcile.json"
    ctl bank reconcile "${statement}" "@${state}/bank-reconcile.json" \
        > /dev/null || die "could not open a reconciliation"
}

# ──────────────────────────────────────────────────────────────────────────
# The sales side
#
# A deal's stage is not a word typed into a field; it is the stage record
# it currently sits in, and the history of how it got there is a row per
# move. A lead is not a contact; it becomes one, atomically, when it is
# converted. Both are seeded through the services so that the funnel and
# stage-duration reports have real timings rather than invented ones.
# ──────────────────────────────────────────────────────────────────────────

# A pipeline stage's id, by the name the default process gives it.
# `venturectl deal move` takes the id, not the name.
stage_id () {
    local name="$1"
    local id

    id="$(ctl --format json list pipeline_stage limit=20 2>/dev/null | python3 -c \
        'import json, sys
try:
    rows = json.load(sys.stdin).get("records", [])
    print(next(str(r["id"]) for r in rows if r.get("name") == sys.argv[1]))
except Exception:
    pass' "${name}" 2>/dev/null || true)"

    [[ -n "${id}" ]] || die "the default pipeline has no ${name} stage"

    printf '%s' "${id}"
}

# A deal's id, by name. The moves below are written against the deals
# seed_relations creates, and naming them beats counting on the order
# they were inserted in.
deal_id () {
    local name="$1"
    local id

    id="$(ctl --format json list deal limit=50 2>/dev/null | python3 -c \
        'import json, sys
try:
    rows = json.load(sys.stdin).get("records", [])
    print(next(str(r["id"]) for r in rows if r.get("name") == sys.argv[1]))
except Exception:
    pass' "${name}" 2>/dev/null || true)"

    [[ -n "${id}" ]] || die "there is no deal called ${name}"

    printf '%s' "${id}"
}

seed_pipelines () {
    step "Sales stages, and deals moved through them"

    local qualified
    local proposal
    local negotiation
    local won
    local lost
    local price
    local bellhaven
    local harbour
    local thorne
    local ardley

    # Every organisation starts with a default pipeline and its six
    # stages, so the seeding uses that rather than inventing a second
    # process the reports would have to choose between.
    price="$(make_record loss_reason name="Price" active=true)"
    add loss_reason name="Timing" active=true
    add loss_reason name="Went elsewhere" active=true
    add loss_reason name="No budget" active=true

    qualified="$(stage_id qualified)"
    proposal="$(stage_id proposal)"
    negotiation="$(stage_id negotiation)"
    won="$(stage_id won)"
    lost="$(stage_id lost)"

    # Each move writes a stage entry, and it is those entries -- not the
    # deal's current stage -- that the funnel and stage-duration reports
    # read. Walking each deal to where it stands is what gives them
    # something to measure.
    bellhaven="$(deal_id "Bellhaven, spring order")"
    harbour="$(deal_id "Cold Harbour, ten seats")"
    thorne="$(deal_id "Thorne & Sons")"
    ardley="$(deal_id "Ardley Trust")"

    ctl deal move "${bellhaven}" "${qualified}" "Reached the buyer." \
        > /dev/null || die "could not qualify the Bellhaven deal"
    ctl deal move "${bellhaven}" "${proposal}" "Quote Q-1014 sent." \
        > /dev/null || die "could not move the Bellhaven deal to proposal"

    ctl deal move "${harbour}" "${qualified}" "Ten seats confirmed as the scope." \
        > /dev/null || die "could not qualify the Cold Harbour deal"
    ctl deal move "${harbour}" "${negotiation}" "Haggling over the annual discount." \
        > /dev/null || die "could not move the Cold Harbour deal on"

    ctl deal move "${ardley}" "${won}" "Signed for three years." > /dev/null \
        || die "could not win the Ardley deal"

    # One lost, with a reason, because a funnel with no losses in it is
    # not a funnel. The reason is set first: it is a field on the deal,
    # and the move is what closes it.
    ctl update deal "${thorne}" loss_reason_id="${price}" > /dev/null \
        || die "could not set a loss reason on the Thorne deal"
    ctl deal move "${thorne}" "${lost}" "Went with a cheaper printer." \
        > /dev/null || die "could not close the Thorne deal as lost"
}

seed_leads () {
    local press="$1"
    local studio="$2"

    step "Inbound leads, and one converted into a customer"

    local nina

    # The form the website posts to, and the rule that decides whose
    # lead it is.
    add lead_form name="Website enquiry" venture_id="${studio}" active=true \
        redirect_url="https://harrow.example/thanks" honeypot=company_url \
        on_duplicate=merge
    add lead_assignment_rule name="Web enquiries" venture_id="${studio}" \
        source=web assignees=owner strategy=round_robin active=true

    nina="$(make_record lead name="Nina Alvez" company_name="Alvez & Co" \
        email=nina@alvez.example phone="+44 7700 900412" \
        website="https://alvez.example" venture_id="${studio}" source=web \
        owner=owner status=new score=72 first_seen_at="$(day -9)" \
        last_activity_at="$(day -2)" \
        notes="Wants to move six people off a spreadsheet.")"

    add lead name="Tomas Wend" company_name="Wend Bookshop" \
        email=tomas@wendbooks.example venture_id="${press}" source=referral \
        owner=owner status=working score=48 first_seen_at="$(day -5)" \
        notes="Saw The Long Field at a fair."
    add lead name="Priya Raman" company_name="Raman Media" \
        email=priya@ramanmedia.example venture_id="${studio}" source=event \
        owner=owner status=unqualified score=12 first_seen_at="$(day -24)" \
        unqualified_reason="Wanted a free tier we do not offer." \
        recycle_until="$(day 60)"
    add lead name="Auden Frye" company_name="Frye Consulting" \
        email=auden@fryeconsulting.example venture_id="${studio}" source=web \
        owner=owner status=new score=33 first_seen_at="$(day -1)"

    # Qualifying first is the service's rule, and conversion is one
    # atomic step that writes the company, the contact and the deal.
    ctl update lead "${nina}" status=qualified > /dev/null \
        || die "could not qualify Nina Alvez"
    ctl lead convert "${nina}" deal=yes > /dev/null \
        || die "could not convert Nina Alvez"
}

seed_activities () {
    local shop="$1"
    local agency="$2"
    local buyer="$3"

    step "The worklist: calls made, calls due, one overdue"

    local done_call

    add activity_type name="Discovery call" kind=call default_duration=30 active=true
    add activity_type name="Demo" kind=meeting default_duration=45 active=true
    add activity_type name="Follow-up" kind=followup default_duration=15 active=true

    # One already done, with an outcome, so a contact's timeline is not
    # empty.
    done_call="$(make_record activity subject="Call Ruth about the spring order" \
        kind=call owner=owner due_at="$(day -3)" priority=high status=planned \
        company_id="${shop}" contact_id="${buyer}" \
        body="Confirm the quantity before quoting.")"

    ctl activity complete "${done_call}" \
        outcome="Wants 160 paperbacks for March. Quote to follow." \
        > /dev/null || die "could not complete the call to Ruth"

    # One overdue, which is the row the worklist exists to surface.
    add activity subject="Chase Cold Harbour on INV-0042" kind=followup \
        owner=owner due_at="$(day -4)" priority=urgent status=planned \
        company_id="${agency}" body="Thirty days past terms."

    add activity subject="Demo for Alvez & Co" kind=meeting owner=owner \
        starts_at="$(day 2)T10:00:00Z" ends_at="$(day 2)T10:45:00Z" \
        due_at="$(day 2)" \
        priority=normal status=planned remind_at="$(day 1)" \
        body="Six seats, coming off a spreadsheet."
    add activity subject="Quarterly check-in" kind=call owner=owner \
        due_at="$(day 9)" priority=low status=planned recurrence=monthly \
        company_id="${shop}"
    add activity subject="Send the spring catalogue" kind=email owner=owner \
        due_at="$(day 5)" priority=normal status=planned company_id="${shop}"
}

seed_quotes () {
    local press="$1"
    local shop="$2"
    local buyer="$3"
    local book="$4"

    step "A price list, and a quote accepted into an invoice"

    local list
    local quote
    local pending

    list="$(make_record price_list name="Trade $(date -u +%Y)" currency=USD \
        is_default=true)"
    add price_list_item price_list_id="${list}" product_id="${book}" \
        unit_price=9.50 min_quantity=100

    quote="$(make_record quote number=Q-1014 venture_id="${press}" \
        company_id="${shop}" contact_id="${buyer}" status=draft \
        issued_at="$(day -6)" valid_until="$(day 24)" currency=USD \
        terms="30 days from acceptance" \
        notes="Carriage included on orders over 100 copies.")"
    add quote_line quote_id="${quote}" product_id="${book}" \
        description="The Long Field, paperback" quantity=160 unit_price=9.50 \
        position=1

    # Sending stamps the token the acceptance link carries; accepting
    # raises the invoice. Both are the service's to do.
    ctl quote send "${quote}" > /dev/null || die "could not send quote Q-1014"
    ctl quote accept "${quote}" by="Ruth Ellery" > /dev/null \
        || die "could not accept quote Q-1014"

    # A second quote left open, so the quotes report has a live one.
    pending="$(make_record quote number=Q-1015 venture_id="${press}" \
        company_id="${shop}" status=draft issued_at="$(day -1)" \
        valid_until="$(day 29)" currency=USD terms="30 days")"
    add quote_line quote_id="${pending}" product_id="${book}" \
        description="The Long Field, hardback" quantity=40 unit_price=16.00 \
        position=1

    ctl quote send "${pending}" > /dev/null || die "could not send quote Q-1015"
}

# ──────────────────────────────────────────────────────────────────────────
# Recurring revenue, follow-ups and post
#
# Harrow Studio sells a subscription, so it is the venture that has an
# MRR to report, customers at different points in their life, and a
# dunning ladder for the ones whose card failed.
# ──────────────────────────────────────────────────────────────────────────

seed_billing () {
    local studio="$1"
    local agency="$2"
    local shop="$3"
    local press="$4"

    step "Plans, subscriptions and a dunning ladder"

    local plan
    local yearly
    local monthly

    plan="$(make_record plan venture_id="${studio}" name="Harrow Studio" \
        code=harrow active=true \
        description="One product, one plan, billed by the seat.")"

    yearly="$(make_record plan_price plan_id="${plan}" currency=USD \
        interval=year amount=180.00 per_seat=true trial_days=0 active=true)"
    make_record plan_price plan_id="${plan}" currency=USD \
        interval=month amount=18.00 per_seat=true trial_days=14 active=true > /dev/null

    # The press sells its own plan: a plan belongs to a venture and is
    # offered only to that venture's customers, so Bellhaven -- a press
    # customer -- is on this one, not the studio's.
    local trade
    trade="$(make_record plan venture_id="${press}" name="Trade account" \
        code=trade active=true \
        description="Standing order for bookshops, billed each quarter.")"
    monthly="$(make_record plan_price plan_id="${trade}" currency=USD \
        interval=quarter amount=75.00 per_seat=false trial_days=14 active=true)"

    # What happens when a card keeps failing. Without these the dunning
    # sweep has nothing to do.
    add dunning_step day_offset=1 action=notice active=true
    add dunning_step day_offset=3 action=retry active=true
    add dunning_step day_offset=7 action=notice active=true
    add dunning_step day_offset=21 action=pause active=true
    add dunning_step day_offset=30 action=cancel active=true

    # A paying customer and one still in trial, so MRR and the trial
    # count are both non-zero.
    ctl billing start company_id="${agency}" plan_price_id="${yearly}" seats=10 \
        > /dev/null || die "could not start the Cold Harbour subscription"
    ctl billing start company_id="${shop}" plan_price_id="${monthly}" seats=3 \
        > /dev/null || die "could not start the Bellhaven subscription"
}

seed_sequences () {
    local studio="$1"
    local buyer="$2"

    step "A follow-up sequence, enrolled and run"

    local sequence
    local when

    sequence="$(make_record sequence name="Trial onboarding" \
        venture_id="${studio}" goal=reply active=true exit_on_reply=true \
        exit_on_unsubscribe=true exit_on_deal_won=true timezone=UTC \
        send_window_start=9 send_window_end=17 weekdays="1,2,3,4,5")"

    add sequence_step sequence_id="${sequence}" position=1 delay_days=0 \
        channel=email active=true subject="Getting started with Harrow" \
        body="Thanks for trying it. Here is the five-minute version."
    add sequence_step sequence_id="${sequence}" position=2 delay_days=3 \
        channel=call_task active=true subject="Check how the trial is going"
    add sequence_step sequence_id="${sequence}" position=3 delay_days=7 \
        channel=email active=true subject="Anything in the way?" \
        body="If it is not working out, a one-line reply helps us."

    # Somebody who asked not to be written to again. The sequence reads
    # this before every send, so it belongs in a demo of one.
    add suppression email=priya@ramanmedia.example reason=unsubscribed \
        at="$(day -18)"

    ctl sequence enroll "${sequence}" contact_id="${buyer}" > /dev/null \
        || die "could not enrol a contact in the sequence"

    # The sequence only sends inside its window, so the sweep is run at a
    # weekday morning rather than whenever the demo happens to start.
    when="$(python3 -c 'import datetime
at = datetime.datetime.now(datetime.UTC).replace(
    hour=10, minute=0, second=0, microsecond=0)
while at.isoweekday() > 5:
    at += datetime.timedelta(days=1)
print(at.strftime("%Y-%m-%dT%H:%M:%SZ"))')"

    ctl sequence run --as-of "${when}" > /dev/null \
        || die "could not run the sequence"
}

seed_mail () {
    step "Templates, and an outbox with something in it"

    # A template substitutes a record's own fields, so these read the way
    # the mail the services send does.
    add mail_template name="Invoice issued" subject="Invoice {number}" \
        text_body="Invoice {number} is attached and due on {due_at}." \
        html_body="<p>Invoice <strong>{number}</strong> is attached and due on {due_at}.</p>"
    add mail_template name="Quote sent" subject="Your quote, {number}" \
        text_body="Here is quote {number}. It holds until {valid_until}." \
        html_body="<p>Here is quote <strong>{number}</strong>.</p>"
    add mail_template name="Payment received" subject="Thanks -- payment received" \
        text_body="We have received {amount}. Nothing further is needed." \
        html_body="<p>We have received <strong>{amount}</strong>.</p>"

    # Queued, not sent: the demo has no SMTP server and should not
    # pretend otherwise. The outbox is the point -- a durable queue with
    # attempts and errors is what the module adds.
    ctl mail send to=ruth@bellhaven.example subject="Your spring quote" \
        text_body="Quote Q-1014 is attached. It holds for thirty days." \
        > /dev/null || die "could not queue the quote mail"
    ctl mail send to=accounts@coldharbour.example \
        subject="INV-0042 is past its terms" \
        text_body="Invoice INV-0042 was due thirty days ago." \
        > /dev/null || die "could not queue the reminder mail"
}

seed_assets () {
    local studio="$1"

    step "A capitalised laptop and rent paid in advance"

    local laptop
    local period

    laptop="$(make_record fixed_asset name="Laptop, 14-inch" tag=FA-0001 \
        venture_id="${studio}" category=Equipment acquired_at="$(month_start 4)" \
        cost=1100.00 salvage_value=100.00 useful_life_months=36 \
        method=straight_line status=draft \
        asset_account_id="$(account_id 1500)" \
        accumulated_depreciation_account_id="$(account_id 1590)" \
        depreciation_expense_account_id="$(account_id 6850)")"

    # Placing it in service is what builds the thirty-six month schedule.
    ctl asset place "${laptop}" in_service_at="$(month_start 4)" > /dev/null \
        || die "could not place the laptop in service"

    # Rent paid up front, spread over the months it covers.
    add deferral kind=prepayment description="Studio rent, six months" \
        total=1800.00 start="$(month_start 3)" months=6 \
        source_account_id="$(account_id 1400)" \
        target_account_id="$(account_id 6600)" \
        funding_account_id="$(account_id 1000)" status=active

    # Post the month that has already passed, so there is a depreciation
    # journal to look at rather than only a schedule.
    period="$(date -u -d "$(month_start 3)" +%Y-%m)"
    ctl assets run-period "${period}" > /dev/null \
        || die "could not run the ${period} asset schedules"
}

seed_access () {
    step "Teams, and who is in them"

    local team

    team="$(make_record team name="Support")"
    add team name="Editorial"

    # The owner is already a member of the organisation; putting them in
    # a team is what gives the ownership rules something to resolve.
    add team_membership user_id=1 team_id="${team}" active=true
}


seed_desk () {
    local studio="$1"

    step "The support desk: service levels, macros, a rota and a sprint"

    local u

    for u in bob:Bob carol:Carol dave:Dave
    do
        add user username="${u%%:*}" display_name="${u##*:}" role=editor active=true
    done

    add sla_policy name="Support urgent" kind=external priority=urgent \
        first_response_hours=1 resolution_hours=8 active=true
    add sla_policy name="Support high" kind=external priority=high \
        first_response_hours=4 resolution_hours=24 active=true
    add sla_policy name="Everything else" all_kinds=true all_priorities=true \
        first_response_hours=24 resolution_hours=120 active=true

    add macro name="Ask for logs" description="When we need more detail" \
        body="Hi, {me} here on {ticket}. Could you attach the logs from the last hour so we can trace it?" \
        apply_status=true status=blocked add_tags="waiting-on-customer" \
        active=true position=1
    add macro name="Close as fixed" description="It shipped" \
        body="This went out in the latest release; closing. Shout if it comes back." \
        apply_status=true status=done active=true position=2
    add macro name="Escalate to engineering" \
        body="Passing this to engineering now. I will come back to you with what they find." \
        apply_status=true status=in_progress apply_priority=true priority=high \
        add_tags=escalated active=true position=3

    add sprint name="Sprint 11" goal="Billing tidy-up" status=completed \
        starts_on="$(day -28)" ends_on="$(day -14)" capacity_points=30 \
        venture_id="${studio}"

    local sprint
    sprint="$(make_record sprint name="Sprint 12" \
        goal="Ship the workdesk and stop the checkout bleeding" status=active \
        starts_on="$(day -6)" ends_on="$(day 8)" capacity_points=30 \
        venture_id="${studio}")"

    printf '%s' "${sprint}"
}

seed_tickets () {
    local studio="$1"
    local sprint="$2"

    step "Tickets, a conversation, logged time and a rating"

    local checkout
    local billing
    local epub
    local libsoup
    local rotate

    # An explicit assignee keeps a ticket off the rota, which is what lets
    # the demo show both: these are placed, and the two at the end are not.
    checkout="$(make_record ticket title="Checkout returns 500 on Apple Pay" \
        kind=external priority=urgent status=in_progress assignee=owner \
        tags="checkout,payments" sprint_id="${sprint}" story_points=5 \
        issue_type=bug venture_id="${studio}")"
    billing="$(make_record ticket title="Subscription renewal charged twice" \
        kind=external priority=urgent status=triage assignee=dave \
        tags="billing,payments" sprint_id="${sprint}" story_points=3 \
        issue_type=bug venture_id="${studio}")"
    add ticket title="Dark mode flickers on reload" kind=external priority=normal \
        status=todo assignee=bob tags=ui sprint_id="${sprint}" story_points=2 \
        issue_type=bug venture_id="${studio}"
    epub="$(make_record ticket title="Export to EPUB drops footnotes" \
        kind=external priority=high status=in_progress assignee=carol tags=export \
        sprint_id="${sprint}" story_points=5 issue_type=bug venture_id="${studio}")"
    add ticket title="Customer wants a CSV export of invoices" kind=external \
        priority=normal status=triage assignee=carol tags="billing,feature-request" \
        issue_type=story venture_id="${studio}"
    add ticket title="Write the release notes for 0.6" kind=internal priority=high \
        status=todo assignee=owner tags=docs sprint_id="${sprint}" story_points=2 \
        issue_type=task venture_id="${studio}"
    rotate="$(make_record ticket title="Rotate the forge access token" \
        kind=internal priority=low status=done assignee=dave tags=ops \
        story_points=1 issue_type=task venture_id="${studio}")"
    libsoup="$(make_record ticket title="Upgrade to libsoup 3.6" kind=internal \
        priority=normal status=review assignee=bob tags=deps sprint_id="${sprint}" \
        story_points=3 issue_type=task venture_id="${studio}")"
    add ticket title="Spike: offline editing" kind=internal priority=low \
        status=todo assignee=carol tags=research story_points=8 \
        issue_type=research venture_id="${studio}"

    # Two that nobody has picked up, because a support queue always has
    # some: these were raised before the rota below existed.
    add ticket title="Fonts render bold on the Windows export" kind=external \
        priority=normal tags=export venture_id="${studio}"
    add ticket title="Licence key not delivered after payment" kind=external \
        priority=high tags="billing,payments" venture_id="${studio}"

    # The rota, and then two more tickets for it to assign. Created in
    # this order on purpose: a demo that showed only assigned tickets
    # would not look like a queue, and one with no rota would not show
    # what a rota does.
    add routing_rule name="Support rota" assignees="bob, carol, dave" \
        strategy=round_robin all_kinds=true all_priorities=true active=true
    add routing_rule name="Billing goes to dave" assignees=dave strategy=first \
        tag=billing all_kinds=true all_priorities=true active=true

    add ticket title="Invoice VAT line is wrong for Ireland" kind=external \
        priority=high tags=billing venture_id="${studio}"
    add ticket title="Search misses titles with an apostrophe" kind=external \
        priority=normal tags=search venture_id="${studio}"

    add ticket_comment ticket_id="${checkout}" author=carol internal=false \
        body="Reproduced on staging. It only happens when the card is Apple Pay and the currency is GBP. @owner this one is losing us money every hour."
    add ticket_comment ticket_id="${checkout}" author=carol internal=true \
        body="Gateway logs show a 422 coming back from the payment provider, not from us. Chasing them now."
    add ticket_comment ticket_id="${billing}" author=dave internal=false \
        body="Two charges confirmed on the account. Refunding the second and holding the renewal job. @owner @bob eyes on this please."
    add ticket_comment ticket_id="${epub}" author=carol internal=false \
        body="Footnote markers survive but the bodies are dropped, and only in the EPUB3 path."
    add ticket_comment ticket_id="${libsoup}" author=bob internal=false \
        body="@owner ready for review when you have a minute."

    ctl ticket "${checkout}" worklog 2.5 "tracing the gateway" > /dev/null
    ctl ticket "${checkout}" worklog 1.0 "call with the payment provider" > /dev/null
    ctl ticket "${epub}" worklog 3.25 "EPUB3 writer" > /dev/null
    ctl ticket "${libsoup}" worklog 0.75 "bump and rebuild" > /dev/null

    ctl watch ticket "${billing}" > /dev/null
    ctl watch ticket "${epub}" > /dev/null

    ctl --format json update ticket "${rotate}" satisfaction=good \
        satisfaction_comment="Quick and quiet, thanks" > /dev/null

    # One that was promised and missed.
    #
    # The clocks are stamped from the moment a ticket is saved, so a
    # demo cannot raise an overdue ticket -- it has to raise one and
    # then move its deadline into the past, which is what a ticket that
    # sat over a weekend looks like in the table. The sweep then marks
    # it exactly as it would at three in the morning.
    local missed

    missed="$(make_record ticket title="Refund promised on Friday, still not sent" \
        kind=external priority=high status=in_progress assignee=bob \
        tags="billing,refund" issue_type=task venture_id="${studio}")"
    ctl --format json update ticket "${missed}" \
        resolution_due_at="$(day -1)" > /dev/null
    add ticket_comment ticket_id="${missed}" author=bob internal=true \
        body="Waiting on finance to confirm the original payment cleared."

    curl --silent --show-error --max-time 20 --fail --output /dev/null \
         --header "Authorization: Bearer ${token}" \
         --header 'Content-Type: application/json' --data '{}' \
         "${base_url}/api/v1/sla/sweep" \
        || die "could not run the service-level sweep"

    add saved_view name="My urgent" entity_type=ticket \
        query="assignee=owner&priority=urgent" pinned=true position=1 \
        owner_user_id=1
    add saved_view name="Unassigned support" entity_type=ticket \
        query="kind=external&assignee=" board=true pinned=true position=2 \
        owner_user_id=1
}

seed_factory () {
    local studio="$1"

    step "A release that went out, and the incident that followed"

    local production
    local release
    local deployment

    production="$(make_record environment name=production kind=production \
        venture_id="${studio}" url="https://harrow.example" active=true \
        description="What customers are on.")"
    add environment name=staging kind=staging venture_id="${studio}" \
        url="https://staging.harrow.example" active=true

    add milestone name="0.6, the workdesk" venture_id="${studio}" status=active \
        due_on="$(day 12)" \
        description="Inbox, service levels, sprints and the desk around a ticket."

    # A forge and one repository, so builds and releases have somewhere to
    # live: the default branch is what "main is red" and a release's
    # readiness are judged by. Inactive, with an address that resolves
    # nowhere -- nothing here ever calls it.
    local forge
    local repo
    forge="$(make_record forge name="Harrow forge" kind=forgejo \
        base_url="https://git.harrow.example" token=demo-token active=false)"
    repo="$(make_record forge_repo name="harrow/studio" forge_id="${forge}" \
        default_branch=main)"

    # The release before, still on record as having run in production, so
    # the environment's Roll back has somewhere to go back to.
    local previous
    previous="$(make_record release number=0.4.0 name="Record links" \
        status=released released_at="$(day -40)" tag=v0.4.0)"
    add deployment release_id="${previous}" environment_id="${production}" \
        status=succeeded deployed_at="$(day -40)" deployed_by=owner

    release="$(make_record release number=0.5.0 name=Dashboards status=released \
        released_at="$(day -7)" tag=v0.5.0)"
    add release number=0.6.0 name="The workdesk" status=in_progress \
        repo_id="${repo}"

    add build title="Release 0.5.0" status=succeeded trigger=webhook workflow=ci \
        repo_id="${repo}" release_id="${release}" \
        number=214 ref=main started_at="$(day -7)" finished_at="$(day -7)"
    add build title="Nightly" status=failed trigger=rule workflow=nightly \
        repo_id="${repo}" \
        number=215 ref=main started_at="$(day -1)" finished_at="$(day -1)" \
        log_excerpt="2 of 214 assertions failed in test-money"

    deployment="$(make_record deployment release_id="${release}" \
        environment_id="${production}" status=succeeded deployed_at="$(day -7)" \
        deployed_by=owner)"

    add incident title="Checkout down for EU customers" severity=sev1 \
        status=resolved environment_id="${production}" release_id="${release}" \
        deployment_id="${deployment}" started_at="$(day -2)" \
        resolved_at="$(day -2)" \
        summary="500s on every Apple Pay order for four hours. Mitigated by disabling Apple Pay, fixed by the payment provider."
    add incident title="EPUB export failing for large books" severity=sev3 \
        status=open environment_id="${production}" release_id="${release}" \
        started_at="$(day -1)" \
        summary="Books over 400 pages time out in the EPUB writer."

    add agent_budget name="Monthly agent spend" period=monthly limit=25.00 \
        warn_percent=80 hard_stop=true active=true \
        notes="What the coding runs may cost before they stop."
    add webhook name="Ops chat" url="http://127.0.0.1:9999/hook" \
        events="ticket.created, incident.*" active=true include_record=false \
        description="Nothing is listening on that port; press Test to watch a delivery fail honestly."
}

# ──────────────────────────────────────────────────────────────────────────
# A virtual economy
#
# The same software, pointed at something that is not a business: an
# auction-house trade inside a game world, kept in the game's own gold. It
# is a second organization -- its own entity, its own book currency -- so
# its money never lands in the press's or the studio's figures, and every
# report run for it answers in gold.
#
# Nothing here is specific to one game, and nothing is a feature of its
# own. Gold is a currency record with coins; the characters and the bank
# are locations; the item groups are a category tree; the price feeds are
# observation sources; the auction house is a channel; farming is sessions;
# a profession is a goal with recipe steps. docs/examples/game-economy.org
# walks through the same model by hand.
# ──────────────────────────────────────────────────────────────────────────

# A copper count as gold, silver and copper, the way an amount in GOLD is
# written: 1234567 is "123g 45s 67c GOLD". Only the coins that are not zero
# are named; a coin may appear once and a bare number among them is refused.
gold () {
    local copper="$1"
    local text=""

    (( copper / 10000 > 0 )) && text+="$(( copper / 10000 ))g "
    (( copper / 100 % 100 > 0 )) && text+="$(( copper / 100 % 100 ))s "
    (( copper % 100 > 0 || copper == 0 )) && text+="$(( copper % 100 ))c "

    printf '%sGOLD' "${text}"
}

# A timestamp N hours from now (negative is the past), for the records a
# game economy dates to the hour: listings, sales, price checks.
at_hour () {
    date -u -d "$1 hours" +%Y-%m-%dT%H:00:00Z
}

# The same to the minute, for a session's start and end.
at_minute () {
    date -u -d "$1 minutes" +%Y-%m-%dT%H:%M:00Z
}

seed_virtual_economy () {
    step "A second organization: an auction-house trade kept in gold"

    local org
    local venture
    local materials herbs ore crafted potions flasks
    local brisk tallow bank
    local silverleaf duskroot copper ironstone vial mortar healing flask
    local healing_recipe flask_recipe
    local goal
    local dashboard
    local item

    # The unit of account first: every money field below is written in it,
    # and the organization names it as its book currency. Four digits of
    # minor unit, because a hundred copper make a silver and a hundred
    # silver a gold.
    add currency code=GOLD name=Gold kind=virtual exponent=4 \
        symbol_position=suffix \
        denominations='[{"suffix":"g","units":10000},{"suffix":"s","units":100},{"suffix":"c","units":1}]' \
        description="The in-game gold of Evermoor Online. 100 copper to the silver, 100 silver to the gold."

    org="$(make_record organization name="Evermoor Trading" slug=evermoor \
        kind=personal default_currency=GOLD fiscal_year_start_month=1 \
        active=true notes="An auction-house trade inside a game world, kept in its own gold.")"

    # What gold is worth in dollars, from the price of the game's token:
    # a rate is exact, a numerator over a denominator, never a float.
    add exchange_rate organization_id="${org}" from_currency=GOLD \
        to_currency=USD rate_numerator=8 rate_denominator=100000 \
        effective_at="$(day -90)" source=manual \
        reason="Token price: 20 USD buys 250,000 gold"

    # The venture type holds it to what a virtual economy has to say for
    # itself -- the world is required -- and its attributes ride in the
    # same request that creates it.
    venture="$(make_record venture organization_id="${org}" \
        name="Silverfen auction house" slug=silverfen-ah \
        venture_type=virtual_economy status=active priority=normal \
        description="Buy low, craft, sell high: herbs and ore in, potions and flasks out." \
        started_at="$(day -120)" \
        attributes.world="Evermoor Online" attributes.region="Silverfen (EU)" \
        attributes.currency_code=GOLD attributes.faction=Tidewardens \
        attributes.handle="Brisk#2211" attributes.marketplace=auction_house)"

    # The item groups a trading addon would call its groups, as a tree
    # that only products may use.
    materials="$(make_record category organization_id="${org}" name=Materials applies_to=product position=1)"
    herbs="$(make_record category organization_id="${org}" name=Herbs parent_id="${materials}" applies_to=product position=1)"
    ore="$(make_record category organization_id="${org}" name=Ore parent_id="${materials}" applies_to=product position=2)"
    crafted="$(make_record category organization_id="${org}" name=Crafted applies_to=product position=2)"
    potions="$(make_record category organization_id="${org}" name=Potions parent_id="${crafted}" applies_to=product position=1)"
    flasks="$(make_record category organization_id="${org}" name=Flasks parent_id="${crafted}" applies_to=product position=2)"

    # Two characters and the guild bank: the places stock can be.
    brisk="$(make_record location organization_id="${org}" name=Brisk kind=character active=true \
        description="The gatherer: herbalism and mining.")"
    tallow="$(make_record location organization_id="${org}" name=Tallow kind=character active=true \
        description="The crafter: alchemy.")"
    bank="$(make_record location organization_id="${org}" name="Guild bank" kind=bank active=true \
        description="Finished goods waiting to be listed.")"

    silverleaf="$(make_record product organization_id="${org}" venture_id="${venture}" \
        name=Silverleaf sku=HERB-SL category_id="${herbs}" tags=herb,farmable \
        list_price="$(gold 1850)" active=true)"
    duskroot="$(make_record product organization_id="${org}" venture_id="${venture}" \
        name=Duskroot sku=HERB-DR category_id="${herbs}" tags=herb,farmable \
        list_price="$(gold 4200)" active=true)"
    copper="$(make_record product organization_id="${org}" venture_id="${venture}" \
        name="Copper ore" sku=ORE-CU category_id="${ore}" tags=ore,farmable \
        list_price="$(gold 1200)" active=true)"
    ironstone="$(make_record product organization_id="${org}" venture_id="${venture}" \
        name="Ironstone ore" sku=ORE-FE category_id="${ore}" tags=ore,farmable \
        list_price="$(gold 6500)" active=true)"
    vial="$(make_record product organization_id="${org}" venture_id="${venture}" \
        name="Crystal vial" sku=MAT-VIAL category_id="${materials}" tags=vendor \
        cost="$(gold 400)" list_price="$(gold 400)" active=true)"
    mortar="$(make_record product organization_id="${org}" venture_id="${venture}" \
        name="Mortar and pestle" sku=TOOL-MP category_id="${materials}" tags=tool \
        cost="$(gold 25000)" active=true)"
    healing="$(make_record product organization_id="${org}" venture_id="${venture}" \
        name="Healing potion" sku=POT-HEAL category_id="${potions}" tags=consumable \
        list_price="$(gold 9500)" active=true)"
    flask="$(make_record product organization_id="${org}" venture_id="${venture}" \
        name="Flask of endurance" sku=FLK-END category_id="${flasks}" tags=consumable \
        list_price="$(gold 68000)" active=true)"

    # One stock item per product, where it is kept.
    #
    # Only the vials and the mortar get an opening count. A sale issues
    # its units from FIFO cost layers and refuses units that have none,
    # and a hand-typed adjustment carries no layer -- so everything that
    # is sold here arrives the way it would in the game: gathered on a
    # posted session (a zero-cost layer: nobody paid for the herbs) or
    # crafted from those (a layer carrying what the inputs cost). The
    # vials and the mortar are only ever consumed or needed by a craft,
    # which takes unlayered units at no cost.
    local product location opening
    while read -r product location opening
    do
        item="$(make_record inventory_item organization_id="${org}" \
            product_id="${product}" venture_id="${venture}" location_id="${location}")"

        if (( opening > 0 ))
        then
            add inventory_txn organization_id="${org}" inventory_item_id="${item}" \
                kind=adjustment quantity="${opening}" occurred_at="$(day -65)" \
                reference=opening notes="Counted on the character"
        fi
    done <<< "${silverleaf} ${brisk} 0
${duskroot} ${brisk} 0
${copper} ${brisk} 0
${ironstone} ${brisk} 0
${vial} ${tallow} 200
${mortar} ${tallow} 1
${healing} ${bank} 0
${flask} ${bank} 0"

    step "Farming sessions, three of them posted into stock"

    local session
    local route start minutes
    local n=0
    while read -r route start minutes
    do
        n=$(( n + 1 ))
        session="$(make_record session organization_id="${org}" venture_id="${venture}" \
            name="${route//_/ } $(date -u -d "${start} hours" +%m-%d)" activity="${route//_/ }" \
            location_id="${brisk}" started_at="$(at_minute $(( start * 60 )))" \
            ended_at="$(at_minute $(( start * 60 + minutes )))" \
            cost="$(gold 1500)" notes="Repairs and a flight")"

        if [[ "${route}" == "herb_route" ]]
        then
            add session_yield organization_id="${org}" session_id="${session}" \
                product_id="${silverleaf}" quantity=$(( 70 + n * 2 )) unit_value="$(gold 1850)"
            add session_yield organization_id="${org}" session_id="${session}" \
                product_id="${duskroot}" quantity=$(( 14 + n )) unit_value="$(gold 4200)"
        else
            add session_yield organization_id="${org}" session_id="${session}" \
                product_id="${copper}" quantity=$(( 56 + n * 2 )) unit_value="$(gold 1200)"
            add session_yield organization_id="${org}" session_id="${session}" \
                product_id="${ironstone}" quantity=$(( 8 + n )) unit_value="$(gold 6500)"
        fi

        # What the mobs dropped, sold to a vendor on the spot: money, not goods.
        add session_yield organization_id="${org}" session_id="${session}" \
            amount="$(gold $(( 21500 + n * 1300 )))" notes="Vendor sales"

        if (( n <= 3 ))
        then
            ctl act session "${session}" post > /dev/null \
                || die "could not post the ${route} session"
        fi
    done <<< "herb_route -480 75
ore_route -360 60
herb_route -240 90
ore_route -144 55
herb_route -48 80"

    step "Two recipes, and the potions and flasks crafted from the harvest"

    # Filed by their output, not by a category of their own: the tree above
    # groups products only (applies_to=product), and a recipe is not one.

    healing_recipe="$(make_record recipe organization_id="${org}" venture_id="${venture}" \
        name="Healing potion" output_product_id="${healing}" output_quantity=1 \
        active=true)"
    add recipe_component organization_id="${org}" recipe_id="${healing_recipe}" product_id="${silverleaf}" quantity=2
    add recipe_component organization_id="${org}" recipe_id="${healing_recipe}" product_id="${vial}" quantity=1
    add recipe_component organization_id="${org}" recipe_id="${healing_recipe}" product_id="${mortar}" quantity=1 reusable=true

    flask_recipe="$(make_record recipe organization_id="${org}" venture_id="${venture}" \
        name="Flask of endurance" output_product_id="${flask}" output_quantity=1 \
        active=true)"
    add recipe_component organization_id="${org}" recipe_id="${flask_recipe}" product_id="${duskroot}" quantity=3
    add recipe_component organization_id="${org}" recipe_id="${flask_recipe}" product_id="${ironstone}" quantity=1
    add recipe_component organization_id="${org}" recipe_id="${flask_recipe}" product_id="${vial}" quantity=1
    add recipe_component organization_id="${org}" recipe_id="${flask_recipe}" product_id="${mortar}" quantity=1 reusable=true

    # Herbs and vials leave stock, potions and flasks arrive, and the
    # mortar is needed and kept. Enough for what the listings below sell:
    # thirty potions and a handful of flasks.
    ctl act recipe "${healing_recipe}" craft times=32 > /dev/null \
        || die "could not craft the healing potions"
    ctl act recipe "${flask_recipe}" craft times=5 > /dev/null \
        || die "could not craft the flasks"

    step "Sixty days of prices from two sources"

    # Two feeds that disagree a little, as they do: the market's own
    # smoothed value and the region's average. A history for the herb and
    # the potion made from it, every fourth day -- enough for the price
    # history report to have a shape and the demo to stay quick -- and one
    # current market value for everything else a recipe or the shopping
    # list has to price.
    local base
    local days
    local wiggle
    while read -r product base
    do
        for (( days = 60; days >= 4; days -= 4 ))
        do
            wiggle=$(( ((days * 7) % 9) - 4 ))
            add price_observation organization_id="${org}" product_id="${product}" \
                source="market value" price="$(gold $(( base + base * wiggle / 40 )))" \
                volume=$(( 200 + (days * 13) % 170 )) observed_at="$(at_hour $(( -24 * days )))"
            add price_observation organization_id="${org}" product_id="${product}" \
                source="region average" price="$(gold $(( base + base * (wiggle + 2) / 50 )))" \
                observed_at="$(at_hour $(( -24 * days - 6 )))"
        done
    done <<< "${silverleaf} 1850
${healing} 9500"

    while read -r product base
    do
        add price_observation organization_id="${org}" product_id="${product}" \
            source="market value" price="$(gold "${base}")" volume=150 \
            observed_at="$(at_hour -30)"
    done <<< "${duskroot} 4200
${copper} 1200
${ironstone} 6500
${vial} 400
${flask} 68000"

    step "Thirty auction-house listings and the sales they became"

    # A ten-listing cycle of outcomes, so the sale rate is something other
    # than 0% or 100%: expired and cancelled offers count as unsold, a
    # partial one counts what it sold, and the four still open at the end
    # are left out of the rate until they close.
    local outcomes=(sold sold expired partial sold cancelled sold expired sold partial)
    local i outcome quantity sold unit listed closed sale_arg sale fees deposit
    local products=("${healing}" "${flask}" "${silverleaf}" "${copper}")
    local stacks=(5 1 20 20)
    local prices=(9500 68000 1850 1200)
    for (( i = 0; i < 30; i++ ))
    do
        product="${products[i % 4]}"
        quantity="${stacks[i % 4]}"
        unit=$(( prices[i % 4] + prices[i % 4] * ((i * 5) % 7 - 3) / 50 ))
        deposit=$(( unit * quantity * 15 / 1000 ))

        if (( i >= 26 ))
        then
            add listing organization_id="${org}" product_id="${product}" \
                channel="auction house" quantity="${quantity}" \
                unit_price="$(gold "${unit}")" deposit="$(gold "${deposit}")" \
                listed_at="$(at_hour $(( -6 - (29 - i) * 5 )))" outcome=open
            continue
        fi

        outcome="${outcomes[i % 10]}"
        listed=$(( -24 * (58 - 2 * i) ))
        closed=$(( listed + 30 ))

        case "${outcome}" in
            sold)    sold="${quantity}" ;;
            partial) sold=$(( quantity > 1 ? quantity / 2 : 1 )) ;;
            *)       sold=0 ;;
        esac

        # A single flask cannot sell in part; it sold.
        if [[ "${outcome}" == "partial" && "${quantity}" -eq 1 ]]
        then
            outcome=sold
        fi

        sale_arg=()
        fees=0
        if (( sold > 0 ))
        then
            # The house keeps five percent of what sold.
            fees=$(( unit * sold * 5 / 100 ))
            sale="$(make_record sale organization_id="${org}" venture_id="${venture}" \
                product_id="${product}" occurred_at="$(at_hour "${closed}")" \
                quantity="${sold}" gross="$(gold $(( unit * sold )))" \
                fees="$(gold "${fees}")" channel="auction house" \
                buyer_name="Auction house")"
            sale_arg=(sale_id="${sale}")
        fi

        add listing organization_id="${org}" product_id="${product}" \
            channel="auction house" quantity="${quantity}" quantity_sold="${sold}" \
            unit_price="$(gold "${unit}")" deposit="$(gold "${deposit}")" \
            fees="$(gold "${fees}")" listed_at="$(at_hour "${listed}")" \
            closed_at="$(at_hour "${closed}")" outcome="${outcome}" "${sale_arg[@]}"
    done

    step "A profession to level, in recipe steps"

    goal="$(make_record goal organization_id="${org}" venture_id="${venture}" \
        name="Alchemy 1-300" metric=skill unit=level start_value=1 \
        current_value=140 target_value=300 due_on="$(day 45)" status=active \
        notes="Level alchemy on Tallow, crafting what sells on the way.")"

    # The last step crafts nothing, so it names no recipe at all: a
    # reference of 0 would be a record that does not exist.
    local position name from to recipe reps finished crafts
    while read -r position from to recipe reps finished name
    do
        crafts=()
        if [[ "${recipe}" != "-" ]]
        then
            crafts=(recipe_id="${recipe}" repetitions="${reps}")
        fi

        add goal_step organization_id="${org}" goal_id="${goal}" position="${position}" \
            name="${name}" from_value="${from}" to_value="${to}" done="${finished}" \
            "${crafts[@]}"
    done <<< "1 1 60 ${healing_recipe} 60 true Healing potions
2 60 110 ${healing_recipe} 50 true More healing potions
3 110 175 ${flask_recipe} 70 false Flasks of endurance
4 175 240 ${flask_recipe} 65 false More flasks
5 240 300 - - false Trainer quests to the cap"

    step "A dashboard for the trade"

    dashboard="$(make_record dashboard organization_id="${org}" name="Evermoor" slug=evermoor \
        purpose=overview layout=three_columns venture_id="${venture}" position=20 \
        description="The auction-house trade: what sold, what it cost, how far the profession has to go. Pick Evermoor Trading in the sidebar to see it.")"

    add dashboard_widget organization_id="${org}" dashboard_id="${dashboard}" kind=sum \
        title="Auction house gross, 30 days" entity_type=sale field=gross \
        period=last_30_days options='{"date_field": "occurred_at"}' span=normal position=1
    add dashboard_widget organization_id="${org}" dashboard_id="${dashboard}" kind=sum \
        title="House cut, 30 days" entity_type=sale field=fees \
        period=last_30_days options='{"date_field": "occurred_at"}' span=normal position=2
    add dashboard_widget organization_id="${org}" dashboard_id="${dashboard}" kind=progress \
        title="Alchemy" entity_type=goal record_id="${goal}" field=current_value \
        options='{"target_field": "target_value", "start_field": "start_value"}' span=normal position=3
    add dashboard_widget organization_id="${org}" dashboard_id="${dashboard}" kind=sum \
        title="Minutes farmed, 30 days" entity_type=session field=minutes \
        period=last_30_days options='{"date_field": "started_at"}' span=normal position=4
    add dashboard_widget organization_id="${org}" dashboard_id="${dashboard}" kind=report \
        title="Sale rate by product" report_name=listing_performance period=last_90_days \
        options='{"group_by": "product"}' span=wide position=5
    add dashboard_widget organization_id="${org}" dashboard_id="${dashboard}" kind=report \
        title="Gold per hour" report_name=session_performance period=last_90_days \
        span=full position=6
}

seed_dashboards () {
    step "Three dashboards"

    local file
    local body

    for file in demo-today demo-dashboard demo-money
    do
        body="$(cat "${root}/data/examples/${file}.json")"

        curl --silent --show-error --max-time 20 --fail --output /dev/null \
             --header "Authorization: Bearer ${token}" \
             --header 'Content-Type: application/json' \
             --data "${body}" \
             "${base_url}/api/v1/dashboards/import" \
            || die "could not import data/examples/${file}.json"
    done
}

# ──────────────────────────────────────────────────────────────────────────
# What to say when it is up
# ──────────────────────────────────────────────────────────────────────────

announce () {
    local counts

    counts="$(ctl --format json list ticket limit=1 2>/dev/null | python3 -c \
        'import json,sys
try:
    print(json.load(sys.stdin).get("total", "?"))
except Exception:
    print("?")' 2>/dev/null || printf '?')"

    say ""
    say "${BOLD}VENTURE is running with example data.${OFF}"
    say ""
    say "  ${BOLD}${base_url}${OFF}"
    say ""
    say "  username  ${BOLD}owner${OFF}"
    say "  password  ${BOLD}${password}${OFF}"
    say ""
    say "${DIM}Two ventures, a year of trade posted to a general ledger, and"
    say "${counts} tickets on a desk with service levels, a sprint, a release"
    say "and an incident. Money, sales and the books are all seeded."
    say "A second organization, Evermoor Trading, runs an auction-house"
    say "trade in a game world's gold: pick it in the sidebar.${OFF}"
    say ""
    say "  Today             ${base_url}/            ${DIM}(the home dashboard)${OFF}"
    say "  The desk          ${base_url}/dashboards/desk"
    say "  Month end         ${base_url}/dashboards/month-end"
    say "  Inbox             ${base_url}/inbox"
    say "  Ticket board      ${base_url}/tickets"
    say "  Sprints           ${base_url}/sprints"
    say "  Factory           ${base_url}/factory"
    say "  Reports           ${base_url}/reports"
    say "  The gold trade    ${base_url}/dashboards/evermoor  ${DIM}(pick Evermoor Trading first)${OFF}"
    say "  Every record type ${base_url}/entities        ${DIM}(leads, quotes, bills, journals…)${OFF}"
    say ""
    say "${DIM}From the command line:"
    say "  export VENTURE_SERVER=${base_url}"
    say "  export VENTURE_TOKEN=\$(cat ${state}/token)"
    say "  build/${build_type}/venturectl list ticket"
    say "The database is ${state}/venture.db and is rebuilt on every run.${OFF}"
    say ""
}

# ──────────────────────────────────────────────────────────────────────────
# The whole of it
# ──────────────────────────────────────────────────────────────────────────

do_stop () {
    local pid

    if pid="$(previous_pid)"
    then
        step "Stopping the demo running as pid ${pid}"
        kill "${pid}" 2>/dev/null || true
        sleep 0.5
        kill -9 "${pid}" 2>/dev/null || true
    else
        say "No demo of ours is running."
    fi

    if [[ -d "${state}" ]]
    then
        step "Removing ${state}"
        rm -rf "${state}"
    fi
}

do_start () {
    require_tools
    require_binaries

    # Ours, from an earlier run: stop it. Somebody else's: say so and stop,
    # because seeding example data into a real instance is the one mistake
    # this script must never make.
    if port_is_taken
    then
        if previous_pid > /dev/null
        then
            stop_previous
        else
            die "something is already answering on ${base_url}. If that is a VENTURE you care about, leave it alone; otherwise stop it, or run this with --port."
        fi
    fi

    trap cleanup EXIT INT TERM

    start_server
    wait_for_health
    read_owner_password
    mint_token

    home_org="$(default_organization)"
    [[ -n "${home_org}" ]] || die "the server has no default organisation"

    local business
    local relations
    local receipt
    local press
    local studio
    local book
    local shop
    local agency
    local buyer
    local paid
    local owing
    local sprint

    # The books come first: with a posting profile in place, every
    # document seeded below posts to the ledger as it is written, which
    # is what gives the demo a general ledger rather than an empty one.
    seed_books

    # Assigned first, then split. A here-string would take the exit
    # status of `read` rather than of the seeding, so a failure inside
    # the command substitution would not stop the run.
    business="$(seed_business)"
    read -r press studio book <<< "${business}"

    # The second organization, straight after the first one's ventures: a
    # game economy in its own gold, with nothing shared with the books.
    seed_virtual_economy

    relations="$(seed_relations "${press}" "${studio}")"
    read -r shop agency buyer paid owing <<< "${relations}"

    receipt="$(seed_receivables "${shop}" "${agency}" "${paid}")"
    seed_payables "${press}" "${studio}"
    seed_banking "${receipt}"

    seed_pipelines
    seed_leads "${press}" "${studio}"
    seed_activities "${shop}" "${agency}" "${buyer}"
    seed_quotes "${press}" "${shop}" "${buyer}" "${book}"

    seed_billing "${studio}" "${agency}" "${shop}" "${press}"
    seed_sequences "${studio}" "${buyer}"
    seed_mail
    seed_assets "${studio}"
    seed_access

    sprint="$(seed_desk "${studio}")"
    seed_tickets "${studio}" "${sprint}"
    seed_factory "${studio}"
    seed_dashboards

    # Last, and in this order: the backfill sweeps up anything the
    # services did not post as it happened, and closing a period then
    # freezes the reports over books that are finally complete.
    seed_ledger_catchup
    seed_periods

    : "${owing}"

    announce

    if [[ "${detach}" == "true" ]]
    then
        # Up, seeded, and staying. Nothing left to clean up on the way out.
        trap - EXIT INT TERM
        say "${DIM}Running in the background. Stop it with:"
        say "  tools/venture-demo.sh --stop${OFF}"
        say ""
        exit 0
    fi

    say "${DIM}Ctrl+C stops it and leaves the data in build/demo.${OFF}"
    say ""

    # The trap does the stopping; this just keeps the script alive while
    # the server is. Status 127 means the pid is not our child, so this
    # did not hold the port -- say so instead of exiting quietly and
    # letting the trap shoot the instance.
    local status=0
    wait "${server_pid}" 2>/dev/null || status=$?

    if [[ ${status} -eq 127 ]]
    then
        die "the server is not a child of this script, so the demo cannot stay up"
    fi
}

main () {
    parse_arguments "$@"
    resolve_paths

    case "${action}" in
        stop)  do_stop  ;;
        *)     do_start ;;
    esac
}

# The foreground regression test sources this file and drives start_server
# with a stub binary. Executing the file is what actually starts a demo.
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]
then
    main "$@"
fi
