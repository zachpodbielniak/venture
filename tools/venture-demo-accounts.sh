#!/usr/bin/env bash
#
# venture-demo-accounts.sh - Evermoor's own accounts for the demo, the same every time
#
# Copyright (C) 2026 Zach Podbielniak
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Writes one push body in the JSON-lines protocol's account-operations
# messages (docs/plugins.org, "The account-operations messages"): what
# `tsmctl export --format venture` would say about the demo's game world
# if Brisk, Tallow and an old alt played it with TradeSkillMaster and
# DataStore installed. The demo sends it with `venturectl feeds push` to a
# `push` data source, so the accounts pages, the mirror, the alerts and
# the books fill exactly as they would from tsmctl.
#
# What is in it:
#
# - two logins, the way a player with two game licences has them: "Main"
#   (EVERMOOR1) and "Alt account" (EVERMOOR2), both under one platform
#   account, so the attention list has a login to switch to;
# - six accounts: on Main, Brisk on Thornmere (the gatherer), Tallow on
#   Silverfen (the crafter), Quill on Duskwatch (an alt nobody has logged
#   in for three weeks) and Main's own warband bank; on the Alt account,
#   Wren on Moonwell, with an auction that ran out after the logout and
#   one about to; and the guild bank, which any login reaches;
# - each account's snapshot, its gold as history (one point a day that
#   changed), its holdings by place, its posted auctions -- forty-two, some
#   expired and waiting for a login, some running out within twelve
#   hours -- and its mail, one of which is about to be lost;
# - sixty days of the ledger: sales after the house's cut, buys, vendor
#   income, postage and repairs, expired and cancelled auctions. Brisk
#   buys moonsteel bars cheaply on Thornmere and Tallow sells them dear
#   on Silverfen two days later, which is what the flips page matches;
# - TSM's AuctionDB figures for the six realms and the region, so the
#   holdings have a value and the days-to-sell a rate.
#
# Like tools/venture-demo-market.sh, whose tables and hash it sources,
# nothing reads the clock but the anchor: every time is an offset from it
# and every number comes from the seeded hash, so one anchor gives one
# body, byte for byte.
#
# Every account's gold is replayed as it is written: the balance after
# each ledger row is the one before plus the row, and a buy that the
# purse could not pay for is not made. tests/demo-accounts.sh replays the
# body again, independently, at awkward anchors and fails if any purse
# goes below zero or a balance line disagrees with the ledger -- the books
# post this ledger day by day into the characters' purses, which may
# never be spent below zero.

set -euo pipefail

here="$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

# The realms, items, hash and decimal formatting of the market generator.
# Sourcing it defines them and runs nothing.
# shellcheck source=tools/venture-demo-market.sh
source "${here}/venture-demo-market.sh"

readonly ACCOUNTS_VERSION="1.0.0"

out=""
anchor=""
days=60
seed=20261004

say () {
    printf '%s\n' "$*"
}

die () {
    printf 'venture-demo-accounts: %s\n' "$*" >&2
    exit 1
}

usage () {
    cat <<'USAGE'
venture-demo-accounts.sh - deterministic operator accounts for the VENTURE demo

Usage:
  tools/venture-demo-accounts.sh --out FILE [--anchor EPOCH | --now TIME] [options]

Options:
  -o, --out FILE      Write the push body to FILE (required; - is stdout)
  -a, --anchor EPOCH  The economy's clock, in Unix seconds on the hour
  -n, --now TIME      The same as any time `date -d` reads, rounded down to
                      the hour (default: VENTURE_DEMO_ECONOMY_NOW, else now)
  -d, --days N        Days of ledger (default 60, 7 to 120)
  -s, --seed N        The hash seed (default 20261004)
  -h, --help          This
      --license       Licence and copyright

What it writes: one JSON-lines body for a `push` data source whose
currency is GOLD (exponent 4) and whose namespaces are the market
source's (venues realm, instruments item): realms and the region as
venues, the items, two logins and six accounts with their snapshots,
balances, holdings, positions and mail, the ledger oldest first, and AuctionDB
stats for each realm and the region.

Examples:
  tools/venture-demo-accounts.sh --out build/demo/market/accounts.jsonl
  tools/venture-demo-accounts.sh -o - --anchor 1781524800 | head
  tools/venture-demo-accounts.sh -o /tmp/a.jsonl -n 2026-06-15T12:55:00Z -d 14
  venturectl feeds push 7 /tmp/a.jsonl --wait organization_id=2
USAGE
}

licence () {
    cat <<'LICENCE'
venture-demo-accounts.sh, part of VENTURE.
Copyright (C) 2026 Zach Podbielniak

This program is free software: you can redistribute it and/or modify it
under the terms of the GNU Affero General Public License as published by
the Free Software Foundation, either version 3 of the License, or (at your
option) any later version. It is distributed WITHOUT ANY WARRANTY; without
even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
PURPOSE. See <https://www.gnu.org/licenses/> for the full text.
LICENCE
}

parse_arguments () {
    local now_text="${VENTURE_DEMO_ECONOMY_NOW:-}"
    local now

    while [[ $# -gt 0 ]]
    do
        case "$1" in
            -o|--out)
                [[ $# -ge 2 ]] || die "--out needs a file"
                out="$2"
                shift 2
                ;;
            -a|--anchor)
                [[ $# -ge 2 ]] || die "--anchor needs Unix seconds"
                anchor="$2"
                shift 2
                ;;
            -n|--now)
                [[ $# -ge 2 ]] || die "--now needs a time"
                now_text="$2"
                shift 2
                ;;
            -d|--days)
                [[ $# -ge 2 ]] || die "--days needs a number"
                days="$2"
                shift 2
                ;;
            -s|--seed)
                [[ $# -ge 2 ]] || die "--seed needs a number"
                seed="$2"
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
                say "venture-demo-accounts.sh ${ACCOUNTS_VERSION}"
                exit 0
                ;;
            *)
                die "unknown option \"$1\". Try --help."
                ;;
        esac
    done

    [[ -n "${out}" ]] || die "--out is required. Try --help."

    if [[ ! "${days}" =~ ^[0-9]+$ ]] || (( days < 7 || days > 120 ))
    then
        die "--days must be 7 to 120"
    fi

    if [[ ! "${seed}" =~ ^[0-9]+$ ]] || (( seed >= 2147483648 ))
    then
        die "--seed must be a number below 2^31"
    fi

    if [[ -n "${anchor}" ]]
    then
        [[ "${anchor}" =~ ^[0-9]+$ ]] || die "--anchor must be Unix seconds"
        (( anchor % 3600 == 0 )) || die "--anchor must be on the hour, as the demo's economy clock is"
    else
        if [[ -n "${now_text}" ]]
        then
            now="$(date -u -d "${now_text}" +%s 2>/dev/null)" \
                || die "\"${now_text}\" is not a time date can read"
        else
            now="$(date -u +%s)"
        fi
        anchor=$(( now / 3600 * 3600 ))
    fi
}

# ──────────────────────────────────────────────────────────────────────────
# The accounts
# ──────────────────────────────────────────────────────────────────────────

# key|name|kind|group|venue|realm index (-1 none)|hours unseen before the
# anchor|opening gold in copper|attrs. Brisk logged out eight hours ago,
# Tallow two, Quill three weeks back; the banks were last written when
# Tallow logged out.
readonly ACCOUNTS=(
    'Brisk-Thornmere|Brisk|character|Thornmere|thornmere|1|8|450000|{"class":"RANGER","color":"#aad372","level":60,"profession:Mining":300,"profession_max:Mining":300,"profession:Smithing":180,"profession_max:Smithing":300,"profession_tiers:Smithing":"Classic 300/300; Evermoor 180/300","race":"Wildkin","faction":"Tidewardens","guild":"Tidewardens","login_account":"EVERMOOR1","played_seconds":1840320}'
    'Tallow-Silverfen|Tallow|character|Silverfen|silverfen|0|2|1200000|{"class":"ALCHEMIST","color":"#3fc7eb","level":60,"profession:Alchemy":300,"profession_max:Alchemy":300,"profession:Herbalism":265,"profession_max:Herbalism":300,"profession_tiers:Alchemy":"Classic 300/300; Evermoor 300/300","race":"Human","faction":"Tidewardens","guild":"Tidewardens","login_account":"EVERMOOR1","played_seconds":2611080}'
    'Quill-Duskwatch|Quill|character|Duskwatch|duskwatch|5|480|150000|{"class":"SCRIBE","color":"#fff468","level":34,"profession:Inscription":120,"profession_max:Inscription":300,"race":"Gnome","faction":"Tidewardens","login_account":"EVERMOOR1","played_seconds":201600}'
    'warbank:EVERMOOR1|Warband bank|shared|||-1|2|3000000|{"login_account":"EVERMOOR1"}'
    'guild:Tidewardens-Silverfen|Tidewardens guild bank|guild|Silverfen||-1|2|9000000|{"guild":"Tidewardens","realm":"Silverfen"}'
    'Wren-Moonwell|Wren|character|Moonwell|moonwell|2|30|250000|{"class":"TAILOR","color":"#f48cba","level":48,"profession:Tailoring":210,"profession_max:Tailoring":300,"profession_tiers:Tailoring":"Classic 210/300","race":"Human","faction":"Tidewardens","login_account":"EVERMOOR2","played_seconds":402400}'
)

# The logins, as tsmctl names them: the game's account folder is the key,
# account_labels in its config the name. key|name. Both licences sit
# under one platform account (the group), but each keeps its own warband
# bank, tsmctl's default.
readonly LOGINS=(
    'EVERMOOR1|Main'
    'EVERMOOR2|Alt account'
)
readonly LOGIN_GROUP="evermoor-platform"

# Which login reaches each account; the guild bank is reached from any,
# so it names none.
declare -A ACCOUNT_LOGIN=(
    [Brisk-Thornmere]=EVERMOOR1
    [Tallow-Silverfen]=EVERMOOR1
    [Quill-Duskwatch]=EVERMOOR1
    [warbank:EVERMOOR1]=EVERMOOR1
    [Wren-Moonwell]=EVERMOOR2
)

# The item index of a key, and every key's base price in copper.
declare -A item_index=()
declare -a item_key=() item_name=() item_category=() item_base=() item_stack=()

load_items () {
    local i key name category base stack

    for (( i = 0; i < ${#ITEMS[@]}; i++ ))
    do
        IFS='|' read -r key name category base stack _ <<< "${ITEMS[i]}"
        item_index["${key}"]="${i}"
        item_key[i]="${key}"
        item_name[i]="${name}"
        item_category[i]="${category}"
        item_base[i]="${base}"
        item_stack[i]="${stack}"
    done
}

# An item's price level on a realm, in permille of its base: the realm's
# own, or the planted one where the market generator plants one.
permille=0

price_level () {
    local r="$1"
    local key="$2"
    local planted p_realm p_item p_level

    permille="${REALM_LEVEL[r]}"

    for planted in "${PLANTED[@]}"
    do
        read -r p_realm p_item p_level <<< "${planted}"
        if [[ "${p_realm}" == "${REALMS[r]}" && "${p_item}" == "${key}" ]]
        then
            permille="${p_level}"
        fi
    done
}

# A unit price in copper on realm r for item key, jittered by the hash
# (a, b) between -6% and +6%, never below a copper.
price=0

unit_price () {
    local r="$1"
    local key="$2"
    local a="$3"
    local b="$4"
    local i="${item_index[${key}]}"

    price_level "${r}" "${key}"
    mix "${r}" "${i}" "${a}" "${b}"
    price=$(( item_base[i] * permille * (940 + hashed % 121) / 1000000 ))
    (( price >= 1 )) || price=1
}

# ──────────────────────────────────────────────────────────────────────────
# The ledger, replayed per account as it is written
# ──────────────────────────────────────────────────────────────────────────

declare -A balance=()       # account key -> copper now
declare -a txn_lines=()     # "at|seq|line", sorted before writing
declare -A balance_lines=() # account key -> the account's balance lines
declare -A day_changed=()   # account key -> 1 when today moved its gold
txn_seq=0

# Records one ledger row and moves the account's gold. Kind sale and
# income add, buy and expense take away (and are refused -- not made --
# when the purse cannot pay), expired and cancelled carry no money.
#
# txn ACCOUNT KIND AT VENUE INSTRUMENT QUANTITY UNIT_COPPER AMOUNT_COPPER SOURCE COUNTERPARTY
txn () {
    local account="$1"
    local kind="$2"
    local at="$3"
    local venue="$4"
    local instrument="$5"
    local quantity="$6"
    local unit="$7"
    local amount="$8"
    local source="$9"
    local counterparty="${10}"
    local stamp line=""
    local id

    case "${kind}" in
        buy|expense)
            if (( balance[${account}] < amount ))
            then
                return 0
            fi
            balance["${account}"]=$(( balance[${account}] - amount ))
            day_changed["${account}"]=1
            ;;
        sale|income)
            balance["${account}"]=$(( balance[${account}] + amount ))
            day_changed["${account}"]=1
            ;;
    esac

    txn_seq=$(( txn_seq + 1 ))
    printf -v stamp '%(%Y-%m-%dT%H:%M:%SZ)T' "${at}"
    printf -v id 'evm-%s-%d-%d' "${account%%-*}" "${at}" "${txn_seq}"
    id="${id//[^A-Za-z0-9:._-]/-}"
    line="{\"type\":\"txn\",\"id\":\"${id}\",\"account\":\"${account}\""
    if [[ -n "${venue}" ]]
    then
        line+=",\"venue\":\"${venue}\""
    fi
    line+=",\"kind\":\"${kind}\""
    if [[ -n "${instrument}" ]]
    then
        line+=",\"instrument\":\"${instrument}\",\"quantity\":${quantity}"
    fi
    case "${kind}" in
        sale|buy)
            as_decimal "${unit}" 4
            line+=",\"unit_price\":\"${decimal}\""
            as_decimal "${amount}" 4
            line+=",\"amount\":\"${decimal}\""
            ;;
        income|expense)
            as_decimal "${amount}" 4
            line+=",\"amount\":\"${decimal}\""
            ;;
    esac
    if [[ -n "${counterparty}" ]]
    then
        line+=",\"counterparty\":\"${counterparty}\""
    fi
    line+=",\"source\":\"${source}\",\"at\":\"${stamp}\"}"
    txn_lines+=("${at}|${txn_seq}|${line}")
}

# A sale after the house's five percent, as TSM records it: the unit price
# is what the seller kept, and the amount is that times the quantity.
sell () {
    local account="$1"
    local at="$2"
    local r="$3"
    local key="$4"
    local quantity="$5"
    local a="$6"
    local b="$7"
    local buyer="$8"
    local kept

    unit_price "${r}" "${key}" "${a}" "${b}"
    kept=$(( price * 95 / 100 ))
    (( kept >= 1 )) || kept=1
    txn "${account}" sale "${at}" "${REALMS[r]}" "${key}" "${quantity}" "${kept}" \
        $(( kept * quantity )) Auction "${buyer}"
}

buy () {
    local account="$1"
    local at="$2"
    local r="$3"
    local key="$4"
    local quantity="$5"
    local a="$6"
    local b="$7"
    local seller="$8"

    unit_price "${r}" "${key}" "${a}" "${b}"
    txn "${account}" buy "${at}" "${REALMS[r]}" "${key}" "${quantity}" "${price}" \
        $(( price * quantity )) Auction "${seller}"
}

# The balance point that closes a day an account's gold moved on.
close_day () {
    local account="$1"
    local at="$2"
    local stamp

    if [[ -z "${day_changed[${account}]:-}" ]]
    then
        return 0
    fi

    unset 'day_changed[${account}]'
    printf -v stamp '%(%Y-%m-%dT%H:%M:%SZ)T' "${at}"
    as_decimal "${balance[${account}]}" 4
    balance_lines["${account}"]+="{\"type\":\"balance\",\"account\":\"${account}\",\"currency\":\"GOLD\",\"amount\":\"${decimal}\",\"at\":\"${stamp}\"}"$'\n'
}

readonly BUYERS=(Mossbeard Lirael Corvane Theddry Ysolde Pemberton Kask Orwyn)
readonly SELLERS=(Grindle Vashti Holloway Brannoc Sefa Tamsin)

# Sixty days of what each character did, oldest first. Every slot of a
# day is at a fixed hour, so rows of one account come in time order and
# the balance is replayed in that order; today stops at the account's
# last logout, because TSM writes nothing while nobody is logged in.
write_ledger () {
    local d day_start at last_seen t buyer seller q stack
    local brisk_seen=$(( anchor - 8 * 3600 ))
    local tallow_seen=$(( anchor - 2 * 3600 ))
    local quill_seen=$(( anchor - 480 * 3600 ))
    local midnight=$(( anchor / 86400 * 86400 ))
    local -a brisk_goods=(duskroot copper-ore ironstone-ore mithril-ore tin-ore)

    for (( d = days - 1; d >= 0; d-- ))
    do
        day_start=$(( midnight - d * 86400 ))

        # ── Brisk: gathers on Thornmere and sells what he gathers ──
        last_seen="${brisk_seen}"
        for (( t = 0; t < 3; t++ ))
        do
            mix 11 "${d}" "${t}" 1
            (( t == 0 || hashed % 3 != 0 )) || continue
            at=$(( day_start + (9 + t * 4) * 3600 + hashed % 3000 ))
            (( at < last_seen )) || continue
            q="${brisk_goods[$(( (hashed >> 4) % 5 ))]}"
            buyer="${BUYERS[$(( (hashed >> 8) % ${#BUYERS[@]} ))]}"
            stack=20
            [[ "${q}" != mithril-ore ]] || stack=10
            sell Brisk-Thornmere "${at}" 1 "${q}" "${stack}" "${d}" $(( 30 + t )) "${buyer}"
        done
        # Vendor income from the farming runs, and the odd expired stack.
        at=$(( day_start + 21 * 3600 ))
        if (( at < last_seen ))
        then
            mix 11 "${d}" 9 2
            txn Brisk-Thornmere income "${at}" "" "" 0 0 $(( 3000 + hashed % 9000 )) Vendor ""
        fi
        # Every third day the cheap moonsteel bars, and the postage that
        # sends them to Tallow.
        if (( d % 3 == 2 ))
        then
            at=$(( day_start + 11 * 3600 + 600 ))
            if (( at < last_seen ))
            then
                mix 11 "${d}" 7 3
                seller="${SELLERS[$(( hashed % ${#SELLERS[@]} ))]}"
                buy Brisk-Thornmere "${at}" 1 moonsteel-bar 5 "${d}" 40 "${seller}"
                txn Brisk-Thornmere expense $(( at + 900 )) "" "" 0 0 30 Postage ""
            fi
        fi
        if (( d % 5 == 1 ))
        then
            at=$(( day_start + 19 * 3600 ))
            (( at >= last_seen )) || txn Brisk-Thornmere expired "${at}" thornmere ironstone-ore 20 0 0 Auction ""
        fi
        close_day Brisk-Thornmere $(( day_start + 23 * 3600 < last_seen ? day_start + 23 * 3600 : last_seen ))

        # ── Tallow: brews on Silverfen and sells potions, flasks and bars ──
        last_seen="${tallow_seen}"
        for (( t = 0; t < 2; t++ ))
        do
            mix 12 "${d}" "${t}" 1
            (( t == 0 || hashed % 2 == 0 )) || continue
            at=$(( day_start + (10 + t * 6) * 3600 + hashed % 3000 ))
            (( at < last_seen )) || continue
            buyer="${BUYERS[$(( (hashed >> 8) % ${#BUYERS[@]} ))]}"
            sell Tallow-Silverfen "${at}" 0 healing-potion 5 "${d}" $(( 50 + t )) "${buyer}"
        done
        # The bars Brisk bought two days ago, sold dear: the flips.
        if (( d % 3 == 0 && d + 2 < days ))
        then
            at=$(( day_start + 13 * 3600 ))
            if (( at < last_seen ))
            then
                mix 12 "${d}" 7 1
                buyer="${BUYERS[$(( (hashed >> 8) % ${#BUYERS[@]} ))]}"
                sell Tallow-Silverfen "${at}" 0 moonsteel-bar 5 "${d}" 60 "${buyer}"
            fi
        fi
        if (( d % 2 == 0 ))
        then
            at=$(( day_start + 18 * 3600 + 1200 ))
            if (( at < last_seen ))
            then
                mix 12 "${d}" 8 1
                buyer="${BUYERS[$(( (hashed >> 8) % ${#BUYERS[@]} ))]}"
                sell Tallow-Silverfen "${at}" 0 flask-of-endurance 1 "${d}" 61 "${buyer}"
            fi
        fi
        if (( d % 6 == 3 ))
        then
            at=$(( day_start + 20 * 3600 ))
            (( at >= last_seen )) || txn Tallow-Silverfen cancelled "${at}" silverfen flask-of-endurance 1 0 0 Auction ""
        fi
        if (( d % 4 == 1 ))
        then
            at=$(( day_start + 9 * 3600 ))
            (( at >= last_seen )) || txn Tallow-Silverfen buy "${at}" "" crystal-vial 20 400 8000 Vendor "Alchemy supplies"
            at=$(( day_start + 19 * 3600 + 1800 ))
            (( at >= last_seen )) || txn Tallow-Silverfen expired "${at}" silverfen mana-potion 5 0 0 Auction ""
        fi
        if (( d % 7 == 4 ))
        then
            at=$(( day_start + 22 * 3600 ))
            (( at >= last_seen )) || txn Tallow-Silverfen expense "${at}" "" "" 0 0 12500 "Repair Bill" ""
        fi
        close_day Tallow-Silverfen $(( day_start + 23 * 3600 + 1800 < last_seen ? day_start + 23 * 3600 + 1800 : last_seen ))

        # ── Quill: traded pets on Duskwatch until three weeks ago ──
        last_seen="${quill_seen}"
        if (( d % 4 == 0 ))
        then
            at=$(( day_start + 15 * 3600 ))
            if (( at < last_seen ))
            then
                mix 13 "${d}" 1 1
                buyer="${BUYERS[$(( (hashed >> 8) % ${#BUYERS[@]} ))]}"
                sell Quill-Duskwatch "${at}" 5 bog-frog 1 "${d}" 70 "${buyer}"
            fi
        fi
        if (( d % 4 == 2 ))
        then
            at=$(( day_start + 14 * 3600 ))
            if (( at < last_seen ))
            then
                mix 13 "${d}" 2 1
                seller="${SELLERS[$(( hashed % ${#SELLERS[@]} ))]}"
                buy Quill-Duskwatch "${at}" 4 bog-frog 1 "${d}" 71 "${seller}"
            fi
        fi
        close_day Quill-Duskwatch $(( day_start + 23 * 3600 < last_seen ? day_start + 23 * 3600 : last_seen ))
    done
}

# ──────────────────────────────────────────────────────────────────────────
# What each account holds, has posted and has waiting
# ──────────────────────────────────────────────────────────────────────────

declare -A auction_units=() # "account|key" -> units posted
declare -A position_lines=()
declare -A inbound_lines=()
declare -A holding_lines=()
position_count=0

# position ACCOUNT REALM_INDEX KEY QUANTITY HOURS_POSTED_AGO DURATION_HOURS SALT
position () {
    local account="$1"
    local r="$2"
    local key="$3"
    local quantity="$4"
    local ago="$5"
    local hours_listed="$6"
    local salt="$7"
    local posted=$(( anchor - ago * 3600 ))
    local expires=$(( posted + hours_listed * 3600 ))
    local posted_stamp expires_stamp unit bid=""

    unit_price "${r}" "${key}" 90 "${salt}"
    unit="${price}"
    position_count=$(( position_count + 1 ))
    printf -v posted_stamp '%(%Y-%m-%dT%H:%M:%SZ)T' "${posted}"
    printf -v expires_stamp '%(%Y-%m-%dT%H:%M:%SZ)T' "${expires}"
    as_decimal "${unit}" 4

    # A bid on the dearer single items, as the house allows.
    if (( quantity == 1 && unit > 20000 ))
    then
        local bid_copper=$(( unit * 80 / 100 ))
        local text="${decimal}"
        as_decimal "${bid_copper}" 4
        bid=",\"bid\":\"${decimal}\""
        decimal="${text}"
    fi

    position_lines["${account}"]+="{\"type\":\"position\",\"account\":\"${account}\",\"venue\":\"${REALMS[r]}\",\"id\":\"$(( 1700000000 + position_count * 7919 + salt ))\",\"instrument\":\"${key}\",\"quantity\":${quantity},\"price\":\"${decimal}\"${bid},\"expires_at\":\"${expires_stamp}\",\"posted_at\":\"${posted_stamp}\"}"$'\n'
    auction_units["${account}|${key}"]=$(( ${auction_units["${account}|${key}"]:-0} + quantity ))
}

# Forty-two posted auctions. A stack posted H hours ago for D hours expires at
# D - H from the anchor: Brisk's and Tallow's include a few that ran out
# after they logged out and wait for a login to be collected, a few that
# run out within twelve hours, and the rest later; Quill's ran out weeks
# ago and are still on his books.
write_positions () {
    local i key ago hours_listed
    local -a tallow=(
        "healing-potion 5" "healing-potion 5" "healing-potion 5" "healing-potion 5"
        "healing-potion 5" "healing-potion 5" "healing-potion 5" "healing-potion 5"
        "flask-of-endurance 1" "flask-of-endurance 1" "flask-of-endurance 1" "flask-of-endurance 1"
        "moonsteel-bar 5" "moonsteel-bar 5" "moonsteel-bar 5"
        "swiftness-potion 5" "swiftness-potion 5"
        "elixir-of-giants 5" "elixir-of-giants 5"
        "mana-potion 5" "mana-potion 5" "mana-potion 5"
    )
    local -a brisk=(
        "duskroot 20" "duskroot 20" "duskroot 20" "duskroot 20"
        "copper-ore 20" "copper-ore 20" "copper-ore 20"
        "ironstone-ore 20" "ironstone-ore 20" "ironstone-ore 20"
        "mithril-ore 10" "mithril-ore 10"
        "tin-ore 20" "tin-ore 20"
    )
    local -a quill=("bog-frog 1" "ember-staff 1" "glowing-shard 1" "glowing-shard 1")
    local quantity

    # Hours posted ago and duration, by slot: slot 0 of ten expired after
    # the logout (posted 49 or 50 h ago for 48 h on Tallow, whose logout
    # was 2 h ago; 52 h for Brisk, 8 h ago), slots 1-2 run out within
    # 2 to 11 hours, the rest 13 to 46 hours out.
    for (( i = 0; i < ${#tallow[@]}; i++ ))
    do
        read -r key quantity <<< "${tallow[i]}"
        mix 21 "${i}" 0 1
        case $(( i % 10 )) in
            0) ago=$(( 49 + hashed % 2 )); hours_listed=48 ;;
            1|2) ago=$(( 3 + hashed % 6 )); hours_listed=12 ;;
            *) hours_listed=48; ago=$(( 2 + hashed % 33 )) ;;
        esac
        position Tallow-Silverfen 0 "${key}" "${quantity}" "${ago}" "${hours_listed}" "${i}"
    done

    for (( i = 0; i < ${#brisk[@]}; i++ ))
    do
        read -r key quantity <<< "${brisk[i]}"
        mix 22 "${i}" 0 1
        case $(( i % 10 )) in
            0) ago=$(( 51 + hashed % 2 )); hours_listed=48 ;;
            1|2) ago=$(( 14 + hashed % 9 )); hours_listed=24 ;;
            *) hours_listed=48; ago=$(( 8 + hashed % 27 )) ;;
        esac
        position Brisk-Thornmere 1 "${key}" "${quantity}" "${ago}" "${hours_listed}" $(( 100 + i ))
    done

    for (( i = 0; i < ${#quill[@]}; i++ ))
    do
        read -r key quantity <<< "${quill[i]}"
        position Quill-Duskwatch 5 "${key}" "${quantity}" $(( 482 + i )) 48 $(( 200 + i ))
    done

    # Wren, on the alt login, logged out thirty hours ago: one stack
    # posted seventy hours ago for 48 ran out after that logout and waits
    # to be collected, the other runs out eight hours from now -- the
    # reasons the attention list names the Alt account.
    position Wren-Moonwell 2 runecloth 20 70 48 300
    position Wren-Moonwell 2 runecloth 20 40 48 301
}

# inbound ACCOUNT ID SENDER SUBJECT MONEY_COPPER KEY QUANTITY EXPIRES_HOURS RETURNED
inbound () {
    local account="$1"
    local id="$2"
    local sender="$3"
    local subject="$4"
    local money="$5"
    local key="$6"
    local quantity="$7"
    local hours="$8"
    local returned="$9"
    local stamp line

    printf -v stamp '%(%Y-%m-%dT%H:%M:%SZ)T' $(( anchor + hours * 3600 ))
    line="{\"type\":\"inbound\",\"account\":\"${account}\",\"id\":\"${id}\",\"sender\":\"${sender}\",\"subject\":\"${subject}\""
    if (( money > 0 ))
    then
        as_decimal "${money}" 4
        line+=",\"money\":\"${decimal}\""
    fi
    if [[ -n "${key}" ]]
    then
        line+=",\"instrument\":\"${key}\",\"quantity\":${quantity}"
    fi
    line+=",\"expires_at\":\"${stamp}\",\"returned\":${returned}}"
    inbound_lines["${account}"]+="${line}"$'\n'
}

write_inbound () {
    # Gold from sales that came in after the logout, a returned stack
    # about to be lost (the mail alert's), and Quill's old returns.
    inbound Tallow-Silverfen evm-mail-t1 "Auction House" "Auction successful: Healing potion (5)" 67690 "" 0 718 false
    inbound Tallow-Silverfen evm-mail-t2 "Auction House" "Auction successful: Flask of endurance" 74290 "" 0 712 false
    inbound Tallow-Silverfen evm-mail-t3 "Auction House" "Auction expired: Mana potion (5)" 0 mana-potion 5 30 true
    inbound Brisk-Thornmere evm-mail-b1 "Auction House" "Auction successful: Duskroot (20)" 63840 "" 0 690 false
    inbound Brisk-Thornmere evm-mail-b2 "Tallow" "Vials for the next batch" 0 crystal-vial 20 600 false
    inbound Quill-Duskwatch evm-mail-q1 "Auction House" "Auction expired: Ember staff" 0 ember-staff 1 230 true
    inbound Wren-Moonwell evm-mail-w1 "Tallow" "Cloth money" 50000 "" 0 640 false
}

# holding ACCOUNT PLACE KEY QUANTITY
holding () {
    holding_lines["$1"]+="{\"type\":\"holding\",\"account\":\"$1\",\"place\":\"$2\",\"instrument\":\"$3\",\"quantity\":$4,\"at\":\"@AT@\"}"$'\n'
}

write_holdings () {
    local entry account key

    holding Brisk-Thornmere bag silverleaf 120
    holding Brisk-Thornmere bag duskroot 60
    holding Brisk-Thornmere bag copper-ore 200
    holding Brisk-Thornmere bag ironstone-ore 80
    holding Brisk-Thornmere bag mithril-ore 40
    holding Brisk-Thornmere bank moonsteel-bar 10
    holding Brisk-Thornmere mail crystal-vial 20

    holding Tallow-Silverfen bag healing-potion 25
    holding Tallow-Silverfen bag crystal-vial 60
    holding Tallow-Silverfen bag flask-of-endurance 4
    holding Tallow-Silverfen bag mortar-and-pestle 1
    holding Tallow-Silverfen bank moonsteel-bar 5
    holding Tallow-Silverfen bank swiftness-potion 10
    holding Tallow-Silverfen reagent_bank silverleaf 80
    holding Tallow-Silverfen reagent_bank duskroot 40
    holding Tallow-Silverfen mail mana-potion 5

    holding Quill-Duskwatch bag bog-frog 1
    holding Quill-Duskwatch bag glowing-shard 3
    holding Quill-Duskwatch mail ember-staff 1

    holding warbank:EVERMOOR1 warbank arcane-dust 200
    holding warbank:EVERMOOR1 warbank runecloth 400
    holding warbank:EVERMOOR1 warbank recipe-flask-of-wisdom 1

    holding guild:Tidewardens-Silverfen guild flask-of-wisdom 6
    holding guild:Tidewardens-Silverfen guild elixir-of-giants 20
    holding guild:Tidewardens-Silverfen guild tidewarden-blade 1

    holding Wren-Moonwell bag runecloth 60
    holding Wren-Moonwell bank arcane-dust 40

    # What is on the auction house is held there too, as tsmctl reports
    # it: the posted stacks summed per item.
    while IFS= read -r entry
    do
        account="${entry%%|*}"
        key="${entry#*|}"
        holding "${account}" auction "${key}" "${auction_units[${entry}]}"
    done < <(printf '%s\n' "${!auction_units[@]}" | LC_ALL=C sort)
}

# ──────────────────────────────────────────────────────────────────────────
# AuctionDB: a scan per realm and the region's figures
# ──────────────────────────────────────────────────────────────────────────

write_market () {
    local r i stamp key price_min market historical listings
    local sale_avg sale_rate sold

    for (( r = 0; r < ${#REALMS[@]}; r++ ))
    do
        printf -v stamp '%(%Y-%m-%dT%H:%M:%SZ)T' $(( anchor - 3600 ))
        printf '{"type":"snapshot","venue":"%s","taken_at":"%s","complete":false}\n' "${REALMS[r]}" "${stamp}"

        for (( i = 0; i < ${#item_key[@]}; i++ ))
        do
            key="${item_key[i]}"
            price_level "${r}" "${key}"
            market=$(( item_base[i] * permille / 1000 ))
            (( market >= 1 )) || market=1
            mix "${r}" "${i}" 77 1
            price_min=$(( market * (930 + hashed % 60) / 1000 ))
            (( price_min >= 1 )) || price_min=1
            historical=$(( market * (980 + (hashed >> 6) % 50) / 1000 ))
            listings=$(( 1 + (hashed >> 10) % 40 ))
            as_decimal "${price_min}" 4
            local min_text="${decimal}"
            as_decimal "${market}" 4
            local market_text="${decimal}"
            as_decimal "${historical}" 4
            printf '{"type":"stat","venue":"%s","instrument":"%s","min":"%s","market":"%s","historical":"%s","listings":%d}\n' \
                "${REALMS[r]}" "${key}" "${min_text}" "${market_text}" "${decimal}" "${listings}"
        done
    done

    printf -v stamp '%(%Y-%m-%dT%H:%M:%SZ)T' $(( anchor - 6 * 3600 ))
    printf '{"type":"snapshot","venue":"region-%s","taken_at":"%s","complete":false}\n' "${GROUP}" "${stamp}"

    for (( i = 0; i < ${#item_key[@]}; i++ ))
    do
        key="${item_key[i]}"
        market="${item_base[i]}"
        mix 9 "${i}" 78 1
        sale_avg=$(( market * (900 + hashed % 80) / 1000 ))
        historical=$(( market * (990 + (hashed >> 6) % 40) / 1000 ))
        # Stacked goods sell by the hundred a day across the region; a
        # single weapon or pet a few a week.
        sale_rate=$(( 5 + (hashed >> 3) % 55 ))
        if (( item_stack[i] > 1 ))
        then
            sold=$(( 40 + (hashed >> 9) % 900 ))
        else
            sold=$(( 1 + (hashed >> 9) % 12 ))
        fi
        as_decimal "${market}" 4
        local market_text="${decimal}"
        as_decimal "${historical}" 4
        local historical_text="${decimal}"
        as_decimal "${sale_avg}" 4
        printf '{"type":"stat","venue":"region-%s","instrument":"%s","market":"%s","historical":"%s","sale_avg":"%s","sale_rate":"0.%02d","sold_per_day":"%d.%d"}\n' \
            "${GROUP}" "${key}" "${market_text}" "${historical_text}" "${decimal}" "${sale_rate}" \
            "$(( sold ))" "$(( (hashed >> 12) % 10 ))"
    done
}

# ──────────────────────────────────────────────────────────────────────────
# The body
# ──────────────────────────────────────────────────────────────────────────

write_body () {
    local r i entry key name kind group venue unseen opening attrs
    local seen stamp line

    # Venues and items first, as tsmctl writes them: the realms in their
    # region, and the region's own pseudo-venue for its figures.
    for (( r = 0; r < ${#REALMS[@]}; r++ ))
    do
        json_escape "${REALM_NAMES[r]}"
        printf '{"type":"venue","key":"%s","name":"%s","kind":"auction_house","group":"%s","currency":"GOLD"}\n' \
            "${REALMS[r]}" "${json_text}" "${GROUP}"
    done
    printf '{"type":"venue","key":"region-%s","name":"Evermoor EU region","kind":"region","group":"%s","currency":"GOLD"}\n' \
        "${GROUP}" "${GROUP}"

    for (( i = 0; i < ${#item_key[@]}; i++ ))
    do
        json_escape "${item_name[i]}"
        printf '{"type":"instrument","key":"%s","name":"%s","kind":"item","category":"%s","attrs":{"vendor_sell":%d}}\n' \
            "${item_key[i]}" "${json_text}" "${item_category[i]}" "$(( item_base[i] / 20 ))"
    done

    # The logins before the accounts that name them.
    for entry in "${LOGINS[@]}"
    do
        IFS='|' read -r key name <<< "${entry}"
        printf '{"type":"login","key":"%s","name":"%s","kind":"game_account","group":"%s"}\n' \
            "${key}" "${name}" "${LOGIN_GROUP}"
    done

    for entry in "${ACCOUNTS[@]}"
    do
        IFS='|' read -r key name kind group venue _ unseen opening attrs <<< "${entry}"
        seen=$(( anchor - unseen * 3600 ))
        printf -v stamp '%(%Y-%m-%dT%H:%M:%SZ)T' "${seen}"
        line="{\"type\":\"account\",\"key\":\"${key}\",\"name\":\"${name}\",\"kind\":\"${kind}\""
        [[ -z "${group}" ]] || line+=",\"group\":\"${group}\""
        [[ -z "${venue}" ]] || line+=",\"venue\":\"${venue}\""
        if [[ "${kind}" == character ]]
        then
            line+=",\"last_seen\":\"${stamp}\""
        fi
        if [[ -n "${ACCOUNT_LOGIN[${key}]:-}" ]]
        then
            line+=",\"login\":\"${ACCOUNT_LOGIN[${key}]}\""
        fi
        line+=",\"attrs\":${attrs}}"
        printf '%s\n' "${line}"
    done

    # Per account: what the snapshot restates, then the rows themselves.
    # Characters cover all four kinds (DataStore read their auctions and
    # mail); the banks only holdings and gold.
    for entry in "${ACCOUNTS[@]}"
    do
        IFS='|' read -r key name kind group venue _ unseen opening attrs <<< "${entry}"
        seen=$(( anchor - unseen * 3600 ))
        printf -v stamp '%(%Y-%m-%dT%H:%M:%SZ)T' "${seen}"

        if [[ "${kind}" == character ]]
        then
            printf '{"type":"account_snapshot","account":"%s","at":"%s","covers":["holdings","positions","inbound","balances"]}\n' \
                "${key}" "${stamp}"
        else
            printf '{"type":"account_snapshot","account":"%s","at":"%s","covers":["holdings","balances"]}\n' \
                "${key}" "${stamp}"
        fi

        printf '%s' "${opening_lines[${key}]}" "${balance_lines[${key}]:-}"
        printf '%s' "${holding_lines[${key}]:-}" | sed "s/@AT@/${stamp}/g"
        printf '%s' "${position_lines[${key}]:-}" "${inbound_lines[${key}]:-}"
    done

    # The ledger, oldest first.
    printf '%s\n' "${txn_lines[@]}" | sort -t'|' -k1,1n -k2,2n | cut -d'|' -f3-

    write_market
}

declare -A opening_lines=()

main () {
    local entry key name kind group venue unseen opening attrs stamp

    parse_arguments "$@"
    load_items

    # Every account's gold before the ledger begins, dated the day before
    # its first row, so the books' opening has a balance to start from --
    # or at the account's last sighting, for an alt last seen before the
    # ledger even starts: a balance after the snapshot that restates it
    # would be one from the future.
    for entry in "${ACCOUNTS[@]}"
    do
        local opened=$(( anchor / 86400 * 86400 - days * 86400 ))

        IFS='|' read -r key name kind group venue _ unseen opening attrs <<< "${entry}"
        balance["${key}"]="${opening}"
        (( opened <= anchor - unseen * 3600 )) || opened=$(( anchor - unseen * 3600 ))
        printf -v stamp '%(%Y-%m-%dT%H:%M:%SZ)T' "${opened}"
        as_decimal "${opening}" 4
        opening_lines["${key}"]="{\"type\":\"balance\",\"account\":\"${key}\",\"currency\":\"GOLD\",\"amount\":\"${decimal}\",\"at\":\"${stamp}\"}"$'\n'
    done

    write_ledger
    write_positions
    write_inbound
    write_holdings

    if [[ "${out}" == "-" ]]
    then
        write_body
    else
        mkdir -p -- "$(dirname -- "${out}")"
        write_body > "${out}.part"
        mv -- "${out}.part" "${out}"
    fi
}

if [[ "${BASH_SOURCE[0]}" == "${0}" ]]
then
    main "$@"
fi
