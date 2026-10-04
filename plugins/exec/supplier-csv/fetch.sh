#!/usr/bin/env bash
# fetch.sh - A dropshipping supplier's CSV price list, as market data
#
# Copyright (C) 2026 Zach Podbielniak
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# The program behind the `supplier_csv` data source provider (see
# supplier-csv.plugin.yaml beside it). VENTURE runs it once per unit of a
# data source, writes one JSON request line on its standard input, and
# reads JSON lines back (docs/plugins.org, "The JSON-lines protocol").
#
# What it reads: a CSV file in this plugin's own directory, with the header
#
#     sku,name,price,currency,quantity,shipping
#
# and one line per product the supplier sells. price and shipping are
# decimals in the line's currency ("6.40", "1.95"; shipping may be empty
# for free), quantity is what the supplier has in stock.
#
# What it answers:
#
#   - the supplier as a venue (kind supplier);
#   - each SKU as an instrument (kind sku), with the price, the shipping
#     and the currency in its attrs;
#   - one complete snapshot, dated by the file's modification time;
#   - one listing per SKU in stock, at the landed price -- price plus
#     shipping, unless the source says include_shipping: false -- since
#     what a dropshipper pays per unit is both. A SKU with no stock gets
#     no listing, and the complete snapshot shows it out of stock.
#
# The data source's settings it reads (all optional):
#
#     file: sample-price-list.csv   # a file in this directory
#     venue: supplier               # the venue's key
#     venue_name: Acme Wholesale    # its name
#     group: us                     # its group, for region figures
#     include_shipping: true
#
# The file is chosen by the data source, which an editor writes, so it is
# held to this directory: a plain relative name, no "..", resolved and
# compared against the directory plus a separator. The program runs as the
# server's user; this check is what keeps a setting from making it read
# something else of that user's.
#
# It never parses JSON in general -- bash has no parser -- only the few
# settings it needs, by pattern, from the request's "source" object.
#
# Usage: fetch.sh < request     (as the exec runtime runs it)
#        fetch.sh --help | --license

set -euo pipefail
shopt -s extglob

# The longest a field may be, and the most rows one run reads: a price list
# is a few thousand lines, and a runaway file must not fill the store.
readonly MAX_FIELD=512
readonly MAX_ROWS=50000

# Problems with individual rows are logged, at most this many.
readonly MAX_ROW_LOGS=20

usage () {
    cat <<'USAGE'
fetch.sh - a supplier's CSV price list as VENTURE market data (JSON lines)

VENTURE runs this as the exec plugin `supplier-csv`; it reads one JSON
request line on standard input and writes venue, instrument, snapshot and
listing lines on standard output.

Options:
  -h, --help     this text
  --license      the licence

Examples:
  # What a sync of the sample price list sends back:
  printf '%s\n' '{"protocol":1,"command":"fetch","params":{"unit":"default","source":{}}}' | ./fetch.sh

  # Another file in this directory, a named venue, prices without shipping:
  printf '%s\n' '{"params":{"unit":"default","source":{"file":"acme.csv","venue":"acme","include_shipping":false}}}' | ./fetch.sh
USAGE
}

license () {
    cat <<'LICENSE'
fetch.sh - Copyright (C) 2026 Zach Podbielniak

This program is free software: you can redistribute it and/or modify it
under the terms of the GNU Affero General Public License as published by
the Free Software Foundation, either version 3 of the License, or (at your
option) any later version.

This program is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public
License for more details: <https://www.gnu.org/licenses/>.
LICENSE
}

# Writes one protocol `error` line and stops: the run fails with the
# message, and nothing before it was data.
fail () {
    if [[ $# -ne 1 ]]
    then
        echo "fail() requires 1 positional argument" >&2
        exit 1
    fi

    local message="${1}"

    printf '{"type":"error","message":"%s"}\n' "$(json_escape "${message}")"
    exit 0
}

# A `log` line, for the run's notes.
log_line () {
    if [[ $# -ne 2 ]]
    then
        echo "log_line() requires 2 positional arguments" >&2
        exit 1
    fi

    local level="${1}"
    local message="${2}"

    printf '{"type":"log","level":"%s","message":"%s"}\n' "${level}" "$(json_escape "${message}")"
}

# Text made safe inside a JSON string: backslash and quote escaped, tabs
# spelt, every other control character turned into a space.
json_escape () {
    if [[ $# -ne 1 ]]
    then
        echo "json_escape() requires 1 positional argument" >&2
        exit 1
    fi

    local text="${1}"

    text="${text//\\/\\\\}"
    text="${text//\"/\\\"}"
    text="${text//$'\t'/\\t}"
    text="${text//[$'\001'-$'\037']/ }"
    printf '%s' "${text}"
}

# A string setting from the request's "source" object, still JSON-escaped
# (it goes straight back out inside a JSON string), or the default.
setting () {
    if [[ $# -ne 3 ]]
    then
        echo "setting() requires 3 positional arguments" >&2
        exit 1
    fi

    local source="${1}"
    local name="${2}"
    local default="${3}"
    local pattern="\"${name}\":\"(([^\"\\\\]|\\\\.)*)\""

    if [[ "${source}" =~ ${pattern} ]]
    then
        printf '%s' "${BASH_REMATCH[1]}"
    else
        printf '%s' "${default}"
    fi
}

# A boolean setting: true unless the source says false.
setting_bool () {
    if [[ $# -ne 3 ]]
    then
        echo "setting_bool() requires 3 positional arguments" >&2
        exit 1
    fi

    local source="${1}"
    local name="${2}"
    local default="${3}"
    local pattern="\"${name}\":(true|false)"

    if [[ "${source}" =~ ${pattern} ]]
    then
        printf '%s' "${BASH_REMATCH[1]}"
    else
        printf '%s' "${default}"
    fi
}

# Adds two decimal strings exactly, at the longer one's places: money is
# never a float, even in bash.
add_decimal () {
    if [[ $# -ne 2 ]]
    then
        echo "add_decimal() requires 2 positional arguments" >&2
        exit 1
    fi

    local a="${1}"
    local b="${2}"
    local a_int="${a%%.*}"
    local b_int="${b%%.*}"
    local a_frac=''
    local b_frac=''
    local places
    local total
    local text

    if [[ "${a}" == *.* ]]
    then
        a_frac="${a#*.}"
    fi

    if [[ "${b}" == *.* ]]
    then
        b_frac="${b#*.}"
    fi

    places=$(( ${#a_frac} > ${#b_frac} ? ${#a_frac} : ${#b_frac} ))

    while (( ${#a_frac} < places ))
    do
        a_frac+='0'
    done

    while (( ${#b_frac} < places ))
    do
        b_frac+='0'
    done

    total=$(( 10#${a_int}${a_frac} + 10#${b_int}${b_frac} ))

    if (( places == 0 ))
    then
        printf '%d' "${total}"
        return
    fi

    printf -v text '%0*d' "$(( places + 1 ))" "${total}"
    printf '%s.%s' "${text:0:${#text}-places}" "${text: -places}"
}

# Splits one CSV line into the global array FIELDS: commas separate, a
# field in double quotes may hold commas, and "" inside one is a quote.
FIELDS=()

split_csv () {
    if [[ $# -ne 1 ]]
    then
        echo "split_csv() requires 1 positional argument" >&2
        exit 1
    fi

    local line="${1}"
    local field=''
    local quoted=0
    local i
    local c

    FIELDS=()

    for (( i = 0; i < ${#line}; i++ ))
    do
        c="${line:i:1}"

        if (( quoted ))
        then
            if [[ "${c}" == '"' ]]
            then
                if [[ "${line:i+1:1}" == '"' ]]
                then
                    field+='"'
                    (( i += 1 ))
                else
                    quoted=0
                fi
            else
                field+="${c}"
            fi
        elif [[ "${c}" == '"' ]]
        then
            quoted=1
        elif [[ "${c}" == ',' ]]
        then
            FIELDS+=("${field}")
            field=''
        else
            field+="${c}"
        fi
    done

    FIELDS+=("${field}")
}

main () {
    if [[ $# -gt 0 ]]
    then
        case "${1}" in
            -h|--help)
                usage
                exit 0
                ;;
            --license)
                license
                exit 0
                ;;
            *)
                usage >&2
                exit 1
                ;;
        esac
    fi

    local request=''
    local source=''
    local root
    local file
    local path
    local venue
    local venue_name
    local group
    local include_shipping
    local taken_at
    local header
    local line
    local rows=0
    local listed=0
    local skipped=0

    IFS= read -r request || true

    # Only the data source's own settings are read: the part of the
    # request after "source":, which the exec provider fills from them.
    if [[ "${request}" == *'"source":{'* ]]
    then
        source="${request#*\"source\":\{}"
    fi

    # CDPATH would make cd print where it landed, into this substitution.
    root="$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
    file="$(setting "${source}" file 'sample-price-list.csv')"
    venue="$(setting "${source}" venue 'supplier')"
    venue_name="$(setting "${source}" venue_name 'Supplier')"
    group="$(setting "${source}" group '')"
    include_shipping="$(setting_bool "${source}" include_shipping true)"

    if [[ ! "${venue}" =~ ^[A-Za-z0-9._-]{1,64}$ ]]
    then
        fail "venue must be a key of letters, digits, '.', '_' or '-'"
    fi

    # The price list, held to this directory: a plain relative name, then
    # resolved and compared against the directory plus a separator, so a
    # symlink out of it or a sibling named <root>-evil is refused too.
    if [[ ! "${file}" =~ ^[A-Za-z0-9._-]+(/[A-Za-z0-9._-]+)*$ ]] || [[ "${file}" == *..* ]]
    then
        fail "file must be a plain name in the plugin's directory, such as sample-price-list.csv"
    fi

    if ! path="$(realpath -e -- "${root}/${file}" 2>/dev/null)"
    then
        fail "the price list ${file} is not in the plugin's directory"
    fi

    if [[ "${path}" != "${root}/"* ]] || [[ ! -f "${path}" ]]
    then
        fail "the price list ${file} resolves outside the plugin's directory"
    fi

    # The snapshot is as old as the file: a list nobody re-exported is
    # yesterday's prices, and the store should say so.
    taken_at="$(date -u -r "${path}" '+%Y-%m-%dT%H:%M:%SZ')"

    exec 3< "${path}"

    IFS= read -r header <&3 || fail "the price list ${file} is empty"
    header="${header%$'\r'}"

    if [[ "${header}" != 'sku,name,price,currency,quantity,shipping' ]]
    then
        fail "the price list's header must be sku,name,price,currency,quantity,shipping"
    fi

    if [[ -n "${group}" ]]
    then
        printf '{"type":"venue","key":"%s","name":"%s","kind":"supplier","group":"%s"}\n' \
            "${venue}" "${venue_name}" "${group}"
    else
        printf '{"type":"venue","key":"%s","name":"%s","kind":"supplier"}\n' "${venue}" "${venue_name}"
    fi

    printf '{"type":"snapshot","venue":"%s","taken_at":"%s","complete":true}\n' "${venue}" "${taken_at}"

    while IFS= read -r line <&3 || [[ -n "${line}" ]]
    do
        local sku name price currency quantity shipping landed

        line="${line%$'\r'}"

        if [[ -z "${line}" ]]
        then
            continue
        fi

        (( rows += 1 ))

        if (( rows > MAX_ROWS ))
        then
            log_line warning "the price list has more than ${MAX_ROWS} rows; the rest are left out"
            break
        fi

        split_csv "${line}"

        if (( ${#FIELDS[@]} != 6 ))
        then
            (( skipped += 1 ))
            if (( skipped <= MAX_ROW_LOGS ))
            then
                log_line warning "row ${rows}: ${#FIELDS[@]} fields, not 6"
            fi
            continue
        fi

        sku="${FIELDS[0]}"
        name="${FIELDS[1]}"
        price="${FIELDS[2]}"
        currency="${FIELDS[3]}"
        quantity="${FIELDS[4]}"
        shipping="${FIELDS[5]:-0}"

        # Each field held to what the protocol and the store accept; a row
        # that is not is skipped and said, never guessed at.
        if [[ ! "${sku}" =~ ^[A-Za-z0-9._-]{1,64}$ ]] ||
           (( ${#name} > MAX_FIELD )) ||
           [[ ! "${price}" =~ ^[0-9]{1,15}(\.[0-9]{1,4})?$ ]] ||
           [[ ! "${shipping}" =~ ^[0-9]{1,15}(\.[0-9]{1,4})?$ ]] ||
           [[ ! "${currency}" =~ ^[A-Z][A-Z0-9_]{1,14}$ ]] ||
           [[ ! "${quantity}" =~ ^[0-9]{1,9}$ ]]
        then
            (( skipped += 1 ))
            if (( skipped <= MAX_ROW_LOGS ))
            then
                log_line warning "row ${rows}: a sku, price, currency, quantity or shipping is malformed"
            fi
            continue
        fi

        if [[ -z "${name}" ]]
        then
            name="${sku}"
        fi

        printf '{"type":"instrument","key":"%s","name":"%s","kind":"sku","attrs":{"price":"%s","shipping":"%s","currency":"%s"}}\n' \
            "${sku}" "$(json_escape "${name}")" "${price}" "${shipping}" "${currency}"

        if (( 10#${quantity} == 0 ))
        then
            continue
        fi

        landed="${price}"

        if [[ "${include_shipping}" == 'true' ]]
        then
            landed="$(add_decimal "${price}" "${shipping}")"
        fi

        printf '{"type":"listing","venue":"%s","instrument":"%s","price":"%s","currency":"%s","quantity":%d,"id":"%s"}\n' \
            "${venue}" "${sku}" "${landed}" "${currency}" "$(( 10#${quantity} ))" "${sku}"
        (( listed += 1 ))
    done

    exec 3<&-

    log_line info "${file}: ${rows} rows, ${listed} listed, ${skipped} skipped"
}

main "$@"
