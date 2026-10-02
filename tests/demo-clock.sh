#!/usr/bin/env bash
#
# demo-clock.sh - the demo's game economy must seed at any time of day
#
# Copyright (C) 2026 Zach Podbielniak
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# seed_virtual_economy dates its records with at_hour and at_minute. Each
# used to read the wall clock and round its own answer, so two offsets
# fifty minutes apart swapped places in the last ten minutes of every
# hour: the six-token Brewfest spend ("26 hours ago, on the hour") landed
# before the session that earned the tokens ended ("1610 minutes ago"),
# the purse refused to go below zero, and `make demo` and test-docs'
# quickstart died -- one run in six, by the clock.
#
# What breaks if this regresses: the demo fails for a stretch of every
# hour and passes on a rerun a few minutes later.
#
# This sources the demo, replaces the three functions that talk to a
# server with recorders, and runs the economy's seed with its clock
# pinned (VENTURE_DEMO_ECONOMY_NOW) to instants on and around hour, day,
# month and year boundaries, plus the real present. For each it checks:
#
#   - no timestamp is in the future of the pinned instant;
#   - every timestamp sits at the same offset from the clock's anchor as
#     in every other run, so the order of events cannot depend on when
#     the seed runs;
#   - replaying the TICKET and BREWFEST holding movements in date order,
#     no purse is spent before it was funded -- a spend dated at the same
#     instant as the income that pays for it counts as too early;
#   - the transfers between characters carry no date, so they happen
#     after everything above.

set -euo pipefail

root="$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
demo="${root}/tools/venture-demo.sh"

fail () {
    printf 'not ok %s\n' "$*" >&2
    exit 1
}

[[ -f "${demo}" ]] || fail "missing ${demo}"

work="$(mktemp -d "${TMPDIR:-/tmp}/venture-demo-clock-XXXXXX")"
trap 'rm -rf "${work}"' EXIT

# shellcheck disable=SC1090
source "${demo}"

# Field separator for the call log: arguments carry spaces and quotes.
readonly SEP=$'\x1f'

record () {
    local IFS="${SEP}"

    printf '%s\n' "$*" >> "${log}"
}

# The ids come from a file because make_record runs inside $(...).
make_record () {
    local id

    id=$(( $(cat "${work}/next-id") + 1 ))
    printf '%s' "${id}" > "${work}/next-id"
    record "${id}" "$@"
    printf '%s' "${id}"
}

ctl () {
    record ctl "$@"
}

step () {
    :
}

# Every economy timestamp as seconds from the anchor, in call order.
# economy_anchor is set by set_economy_clock in the sourced demo.
# shellcheck disable=SC2154
offsets () {
    grep -oE '[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z' "${log}" \
        | while read -r stamp
        do
            printf '%s\n' $(( $(date -u -d "${stamp}" +%s) - economy_anchor ))
        done
}

# The value of key=VALUE among the fields of one logged call.
field () {
    local key="$1"
    shift

    local arg

    for arg in "$@"
    do
        if [[ "${arg}" == "${key}="* ]]
        then
            printf '%s' "${arg#"${key}="}"
            return 0
        fi
    done

    return 1
}

# Replays the virtual-currency holding movements in date order.
check_holdings () {
    local label="$1"

    local -A ended=()
    local -a events=()
    local -a call
    local amount at session

    # First pass: when each session ends, which is when its yield lands.
    while IFS="${SEP}" read -r -a call
    do
        if [[ "${call[1]}" == "session" ]]
        then
            ended["${call[0]}"]="$(field ended_at "${call[@]:2}")"
        fi
    done < "${log}"

    while IFS="${SEP}" read -r -a call
    do
        if [[ "${call[0]}" == "ctl" ]]
        then
            # The only virtual-currency movement through ctl is the
            # location transfer action, and it must stay undated.
            if field amount "${call[@]:1}" > /dev/null &&
               { field occurred_at "${call[@]:1}" > /dev/null || field date "${call[@]:1}" > /dev/null; }
            then
                fail "${label}: a transfer carries a date: ${call[*]}"
            fi
            continue
        fi

        amount="$(field amount "${call[@]:2}" || true)"
        [[ "${amount}" =~ ^(-?[0-9]+)\ (TICKET|BREWFEST)$ ]] || continue

        case "${call[1]}" in
            session_yield)
                session="$(field session_id "${call[@]:2}")"
                at="${ended[${session}]:-}"
                [[ -n "${at}" ]] || fail "${label}: a yield names session ${session}, which was never made"
                # 1 sorts an income after a spend at the same instant.
                events+=("${at} 1 ${BASH_REMATCH[2]} ${BASH_REMATCH[1]}")
                ;;
            expense)
                at="$(field occurred_at "${call[@]:2}")"
                events+=("${at} 0 ${BASH_REMATCH[2]} -${BASH_REMATCH[1]}")
                ;;
            holding_txn)
                at="$(field occurred_at "${call[@]:2}")"
                events+=("${at} 0 ${BASH_REMATCH[2]} ${BASH_REMATCH[1]}")
                ;;
            *)
                fail "${label}: a ${call[1]} moves ${amount}, which this check does not know how to date"
                ;;
        esac
    done < "${log}"

    (( ${#events[@]} >= 4 )) || fail "${label}: only ${#events[@]} holding movements found; is the check still reading the seed?"

    local -A balance=([TICKET]=0 [BREWFEST]=0)
    local stamp order currency delta

    while read -r stamp order currency delta
    do
        balance["${currency}"]=$(( balance[${currency}] + delta ))
        (( balance[${currency}] >= 0 )) \
            || fail "${label}: ${currency} is spent at ${stamp} before it was earned (balance ${balance[${currency}]})"
    done < <(printf '%s\n' "${events[@]}" | sort)

    # Declared only so the loop variable is not reported as unused.
    : "${order}"
}

run_at () {
    local when="$1"
    local label="${when:-the real present}"

    log="${work}/log-${#runs[@]}"
    : > "${log}"
    printf '0' > "${work}/next-id"

    if [[ -n "${when}" ]]
    then
        export VENTURE_DEMO_ECONOMY_NOW="${when}"
    else
        unset VENTURE_DEMO_ECONOMY_NOW
    fi

    seed_virtual_economy

    [[ -s "${log}" ]] || fail "${label}: the seed made nothing"

    local now_epoch
    local -a these

    if [[ -n "${when}" ]]
    then
        now_epoch="$(date -u -d "${when}" +%s)"
    else
        now_epoch="$(date -u +%s)"
    fi
    (( economy_anchor <= now_epoch )) || fail "${label}: the anchor is in the future"

    mapfile -t these < <(offsets)
    (( ${#these[@]} > 50 )) || fail "${label}: only ${#these[@]} timestamps; the seed changed shape"

    local offset
    for offset in "${these[@]}"
    do
        (( offset <= 0 )) || fail "${label}: a record is dated ${offset}s after the anchor, in the future"
    done

    if (( ${#runs[@]} == 0 ))
    then
        reference=("${these[@]}")
    elif [[ "${these[*]}" != "${reference[*]}" ]]
    then
        fail "${label}: the economy's dates sit at different offsets than at ${runs[0]}, so their order depends on the clock"
    fi

    check_holdings "${label}"
    runs+=("${label}")
}

declare -a runs=()
declare -a reference=()
log=""

# On the hour, a second before it, and in the last ten minutes of it --
# where the old rounding failed -- across a day, a month, a leap day and
# a year.
for when in \
    "2026-06-15T12:00:00Z" \
    "2026-06-15T12:05:00Z" \
    "2026-06-15T12:55:00Z" \
    "2026-06-15T12:59:59Z" \
    "2026-02-28T23:55:00Z" \
    "2024-02-29T00:00:00Z" \
    "2025-12-31T23:59:59Z" \
    ""
do
    run_at "${when}"
done

# A pinned clock in the future is refused rather than seeded.
if ( VENTURE_DEMO_ECONOMY_NOW="$(date -u -d '+2 hours' +%Y-%m-%dT%H:%M:%SZ)"; export VENTURE_DEMO_ECONOMY_NOW; set_economy_clock ) 2> /dev/null
then
    fail "a VENTURE_DEMO_ECONOMY_NOW in the future was accepted"
fi

printf 'ok the economy seeds the same at %d clock times\n' "${#runs[@]}"
