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
# sprint, a release that went out and an incident that followed -- and tell
# you how to sign in.
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
detach="false"
action="start"

server_pid=""
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
  -h, --help          This
      --license       Licence and copyright

Environment:
  PORT                Same as --port
  BUILD_TYPE          debug (default) or release -- which build tree to run

Examples:
  make demo                       # build, seed, run in the foreground
  tools/venture-demo.sh           # the same, without building first
  tools/venture-demo.sh -d        # leave it running in the background
  tools/venture-demo.sh --stop    # stop that one and delete its data
  tools/venture-demo.sh -p 9100   # somewhere else

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
    state="${root}/build/demo"
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

    (
        CDPATH='' cd -- "${root}"
        VENTURE_PLUGIN_PATH="${outdir}/plugins" \
        VENTURE_VENTURE_TYPE_PATH="${root}/data/venture-types" \
        VENTURE_POD_MODULE_PATH="${outdir}/pod-modules:${root}/deps/podomation/build/${build_type}/modules" \
        VENTURE_UI_THEME="${VENTURE_UI_THEME:-mocha}" \
        VENTURE_SESSION_SECRET="venture-demo-secret" \
        "${outdir}/venture" \
            --database "sqlite://${state}/venture.db" \
            --state-dir "${state}" \
            --port "${port}" \
            > "${state}/server.log" 2>&1 &

        printf '%s' "$!" > "${state}/venture.pid"
    )

    server_pid="$(cat "${state}/venture.pid")"
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

    printf '%s %s' "${press}" "${studio}"
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

    paid="$(make_record invoice number=INV-0041 status=paid company_id="${shop}" \
        contact_id="${buyer}" venture_id="${press}" issued_at="$(day -52)" \
        due_at="$(day -22)" paid_at="$(day -19)" terms="30 days")"
    owing="$(make_record invoice number=INV-0042 status=sent company_id="${agency}" \
        venture_id="${studio}" issued_at="$(day -48)" due_at="$(day -18)" \
        terms="30 days")"

    add invoice_line invoice_id="${paid}" description="Paperbacks, 120" \
        quantity=120 unit_price=9.50 position=1
    add invoice_line invoice_id="${paid}" description="Carriage" quantity=1 \
        unit_price=45.00 position=2
    add invoice_line invoice_id="${owing}" description="Ten seats, one year" \
        quantity=10 unit_price=180.00 position=1

    add campaign name="Spring catalogue" venture_id="${press}" status=running \
        channel=email started_at="$(day -20)" budget=400.00 spend=280.00 \
        revenue=1900.00 impressions=4200 clicks=310 conversions=41
    add idea title="A subscription box for the press" status=researching \
        priority=normal opportunity=7 confidence=5 effort=6 \
        estimated_revenue=12000.00 \
        description="Four titles a year, posted. Would smooth the cash flow."
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

    release="$(make_record release number=0.5.0 name=Dashboards status=released \
        released_at="$(day -7)" tag=v0.5.0)"
    add release number=0.6.0 name="The workdesk" status=in_progress

    add build title="Release 0.5.0" status=succeeded trigger=webhook workflow=ci \
        number=214 ref=main started_at="$(day -7)" finished_at="$(day -7)"
    add build title="Nightly" status=failed trigger=rule workflow=nightly \
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

seed_dashboards () {
    step "Two dashboards"

    local file
    local body

    for file in demo-dashboard demo-money
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
    say "${DIM}Two ventures, a year of trade, ${counts} tickets on a desk with"
    say "service levels, a sprint, a release and an incident.${OFF}"
    say ""
    say "  The desk          ${base_url}/            ${DIM}(the home dashboard)${OFF}"
    say "  Month end         ${base_url}/dashboards/month-end"
    say "  Inbox             ${base_url}/inbox"
    say "  Ticket board      ${base_url}/tickets"
    say "  Sprints           ${base_url}/sprints"
    say "  Factory           ${base_url}/factory"
    say "  Reports           ${base_url}/reports"
    say ""
    say "${DIM}From the command line:"
    say "  build/${build_type}/venturectl -s ${base_url} -t \$TOKEN list ticket"
    say "The database is build/demo/venture.db and is rebuilt on every run.${OFF}"
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

    local ventures
    local press
    local studio
    local sprint

    ventures="$(seed_business)"
    press="${ventures%% *}"
    studio="${ventures##* }"
    : "${press}"

    seed_relations "${press}" "${studio}"
    sprint="$(seed_desk "${studio}")"
    seed_tickets "${studio}" "${sprint}"
    seed_factory "${studio}"
    seed_dashboards

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
    # the server is.
    wait "${server_pid}" 2>/dev/null || true
}

main () {
    parse_arguments "$@"
    resolve_paths

    case "${action}" in
        stop)  do_stop  ;;
        *)     do_start ;;
    esac
}

main "$@"
