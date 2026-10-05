#!/usr/bin/env bash
# export.sh - World of Warcraft accounts and markets, read by tsmctl
#
# Copyright (C) 2026 Zach Podbielniak
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# The program behind the `tsmctl` data source provider (see
# tsmctl.plugin.yaml beside it). VENTURE runs it once per unit of a data
# source, writes one JSON request line on its standard input, and reads
# JSON lines back (docs/plugins.org, "The JSON-lines protocol").
#
# It does one thing: runs `tsmctl export --format venture` -- tsmctl found
# on PATH, never a path a setting names -- and, when that succeeds, copies
# the export to standard output. tsmctl writes the account-operations
# lines (account, account_snapshot, balance, holding, position, inbound,
# txn) and the AuctionDB venues, instruments, snapshots and stats itself,
# already checked against the contract; this program adds nothing to them.
#
# The export goes to a private file first and is printed only once tsmctl
# has exited 0. A run that printed half an export would hand VENTURE an
# account_snapshot without the rows it restates, and a snapshot removes
# what it does not restate: a crash half way through an account would read
# as "this character has no auctions". A failed export prints one `error`
# line and nothing else, so the run fails and nothing of it is stored.
#
# The data source's settings it reads (all optional):
#
#     accounts: [ZAKMANN]      # WoW account folders (--account), list or
#                              # comma-separated; default every one
#     sources: tsm,datastore   # SavedVariables to read (--sources)
#     market: scope            # AuctionDB figures: scope, all, none
#     since: 90d               # how far back the ledger goes (--since)
#     currency: GOLD           # the source's currency (--currency)
#     include_internal: false  # keep trades between your own characters
#     offline: false           # use tsmctl's cache, never ssh
#
# Each value is held to a pattern before it reaches tsmctl's argv and is
# passed as --option=value, so no setting can become an option of its own.
# Where the WoW install is (host, wow_dir) is tsmctl's own configuration,
# never a setting: a data source names what to read, not where to ssh.
#
# Usage: export.sh < request     (as the exec runtime runs it)
#        export.sh --help | --license

set -euo pipefail

usage () {
    cat <<'USAGE'
export.sh - World of Warcraft data from tsmctl as VENTURE JSON lines

VENTURE runs this as the exec plugin `tsmctl`; it reads one JSON request
line on standard input, runs `tsmctl export --format venture` with the
data source's settings, and writes the export on standard output -- all
of it, or, when tsmctl fails, one error line and nothing else.

tsmctl must be on PATH (it lives in the dotfiles, bin/scripts/tsmctl) and
configured for the WoW install (~/.config/tsmctl/config.yaml).

Options:
  -h, --help     this text
  --license      the licence

Examples:
  # What a sync with no settings sends back:
  printf '%s\n' '{"protocol":1,"command":"fetch","params":{"unit":"default","source":{}}}' | ./export.sh

  # One WoW account, TSM and DataStore only, no AuctionDB figures, 30 days of ledger:
  printf '%s\n' '{"params":{"source":{"accounts":["ZAKMANN"],"sources":"tsm,datastore","market":"none","since":"30d"}}}' | ./export.sh

  # From tsmctl's cache, without contacting the WoW machine:
  printf '%s\n' '{"params":{"source":{"offline":true}}}' | ./export.sh | head
USAGE
}

license () {
    cat <<'LICENSE'
export.sh - Copyright (C) 2026 Zach Podbielniak

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

# Text made safe inside a JSON string: backslash and quote escaped, every
# control character turned into a space.
json_escape () {
    if [[ $# -ne 1 ]]
    then
        echo "json_escape() requires 1 positional argument" >&2
        exit 1
    fi

    local text="${1}"

    text="${text//\\/\\\\}"
    text="${text//\"/\\\"}"
    text="${text//[$'\001'-$'\037']/ }"
    printf '%s' "${text}"
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

# A string setting from the request's "source" object, or the default.
# Every value this program accepts is held to a pattern with no quote or
# backslash in it, so the JSON escaping a match might carry is refused by
# that pattern rather than unescaped here.
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

# A list setting, one value a line: a JSON array of strings, or one
# string of comma-separated values (what a YAML `accounts: A,B` gives).
setting_list () {
    if [[ $# -ne 2 ]]
    then
        echo "setting_list() requires 2 positional arguments" >&2
        exit 1
    fi

    local source="${1}"
    local name="${2}"
    local array_pattern="\"${name}\":\\[([^]]*)\\]"
    local item_pattern='^[[:space:]]*"(([^"\\]|\\.)*)"[[:space:]]*,?(.*)$'
    local body
    local value

    if [[ "${source}" =~ ${array_pattern} ]]
    then
        body="${BASH_REMATCH[1]}"

        while [[ "${body}" =~ ${item_pattern} ]]
        do
            printf '%s\n' "${BASH_REMATCH[1]}"
            body="${BASH_REMATCH[3]}"
        done

        # Anything left that is not whitespace was not a string.
        if [[ -n "${body//[[:space:]]/}" ]]
        then
            printf '%s\n' "${body}"
        fi

        return
    fi

    value="$(setting "${source}" "${name}" '')"

    if [[ -n "${value}" ]]
    then
        printf '%s\n' "${value//,/$'\n'}"
    fi
}

# A boolean setting: the default unless the source says true or false.
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
    local tsmctl
    local value
    local sources
    local market
    local since
    local currency
    local scratch
    local export_file
    local lines
    local status
    local -a argv
    local -a accounts

    IFS= read -r request || true

    # Only the data source's own settings are read: the part of the
    # request after "source":, which the exec provider fills from them.
    if [[ "${request}" == *'"source":{'* ]]
    then
        source="${request#*\"source\":\{}"
    fi

    # tsmctl from PATH -- the server's, captured when the plugin loaded --
    # and never from a setting, which would let whoever edits a data
    # source choose what this program runs.
    if ! tsmctl="$(command -v tsmctl)"
    then
        fail "tsmctl is not on the server's PATH (${PATH}); install it from the dotfiles (bin/scripts/tsmctl) where the server can find it, or push from the WoW machine with \`tsmctl venture push\` instead"
    fi

    argv=("${tsmctl}" export --format venture)

    # WoW account folders: ZAKMANN, 123456789#1. No leading dash, nothing
    # a shell or an option parser would read as more than a name.
    mapfile -t accounts < <(setting_list "${source}" accounts)
    for value in "${accounts[@]}"
    do
        value="${value#"${value%%[![:space:]]*}"}"
        value="${value%"${value##*[![:space:]]}"}"

        if [[ -z "${value}" ]]
        then
            continue
        fi

        if [[ ! "${value}" =~ ^[A-Za-z0-9#_][A-Za-z0-9#_.]{0,63}$ ]]
        then
            fail "accounts: \"${value}\" is not a WoW account folder name (letters, digits, '#', '_' and '.')"
        fi

        argv+=("--account=${value}")
    done

    sources="$(setting "${source}" sources '')"
    if [[ -z "${sources}" ]]
    then
        sources="$(setting_list "${source}" sources | paste -sd, -)"
    fi
    if [[ -n "${sources}" ]]
    then
        sources="${sources// /}"
        if [[ ! "${sources}" =~ ^(tsm|datastore|altoholic|syndicator|baganator|all)(,(tsm|datastore|altoholic|syndicator|baganator|all))*$ ]]
        then
            fail "sources must be a comma-separated list of tsm, datastore, syndicator or all"
        fi
        argv+=("--sources=${sources}")
    fi

    market="$(setting "${source}" market '')"
    if [[ -n "${market}" ]]
    then
        if [[ ! "${market}" =~ ^(scope|all|none)$ ]]
        then
            fail "market must be scope, all or none"
        fi
        argv+=("--market=${market}")
    fi

    since="$(setting "${source}" since '')"
    if [[ -n "${since}" ]]
    then
        if [[ ! "${since}" =~ ^([0-9]{1,10}[hdwmy]?|today|yesterday|week|month|ytd|all|[0-9]{4}-[0-9]{2}-[0-9]{2}(T[0-9]{2}:[0-9]{2}(:[0-9]{2})?)?)$ ]]
        then
            fail "since must be like 90d, 12h, 2w, ytd, all or 2026-09-01"
        fi
        argv+=("--since=${since}")
    fi

    currency="$(setting "${source}" currency '')"
    if [[ -n "${currency}" ]]
    then
        if [[ ! "${currency}" =~ ^[A-Za-z][A-Za-z0-9_]{1,14}$ ]]
        then
            fail "currency must be a currency code such as GOLD"
        fi
        argv+=("--currency=${currency^^}")
    fi

    if [[ "$(setting_bool "${source}" include_internal false)" == 'true' ]]
    then
        argv+=(--include-internal)
    fi

    if [[ "$(setting_bool "${source}" offline false)" == 'true' ]]
    then
        argv+=(--offline)
    fi

    # The export lands in a directory only this run can read, and is
    # printed only when tsmctl says it is complete.
    scratch="$(mktemp -d "${TMPDIR:-/tmp}/venture-tsmctl.XXXXXXXX")" \
        || fail "could not make a private directory for the export"
    # shellcheck disable=SC2064 # the path is fixed now, on purpose
    trap "rm -rf -- '${scratch}'" EXIT
    export_file="${scratch}/export.jsonl"
    argv+=("--file=${export_file}")

    # Standard error is tsmctl's own: its warnings (a host it could not
    # reach, so it used the cache; an optional addon file it could not
    # read) reach the run's error when the export fails, and are kept
    # with the run's stderr otherwise.
    status=0
    "${argv[@]}" < /dev/null || status=$?

    if (( status != 0 ))
    then
        fail "tsmctl export exited with status ${status}; nothing was stored (its standard error says why)"
    fi

    if [[ ! -s "${export_file}" ]]
    then
        fail "tsmctl export wrote nothing; is the WoW install configured in tsmctl's config.yaml?"
    fi

    lines="$(wc -l < "${export_file}")"
    cat -- "${export_file}"
    log_line info "tsmctl export: ${lines// /} lines"
}

main "$@"
