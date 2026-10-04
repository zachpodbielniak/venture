#!/usr/bin/env bash
#
# venture-demo-market.sh - Market data for the demo, the same every time
#
# Copyright (C) 2026 Zach Podbielniak
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Writes the JSON-lines files the demo's file_jsonl data sources read
# (docs/plugins.org, "The JSON-lines protocol"): fourteen days of hourly
# auction-house snapshots for six realms of the demo's game world, a
# marketplace's listings for the dropshipping venture, and two bookmakers'
# odds for the surebet. Nothing here talks to a server; the demo syncs the
# files through the real feed path afterwards, so the store fills exactly
# as it would from any producer.
#
# Two things are deliberate:
#
# - Nothing reads the clock but the anchor. Every timestamp is an offset
#   from one instant on the hour -- the demo's economy clock -- and every
#   price comes from a seeded hash of (realm, item, slot, generation),
#   never from $RANDOM or the time. The same anchor gives the same bytes;
#   an anchor a week later gives the same prices at the same offsets; any
#   other moves only the weekly rhythm under them (two percent either
#   way), never the planted opportunities. So the trades the demo records
#   against these prices are funded, and its alert fires, at any time of
#   day. tests/demo-market.sh holds it to all three.
#
# - The market has things in it to find. Three items are planted with a
#   lasting cross-realm spread (moonsteel bars, the bog frog, mithril
#   ore), and the healing potion's herb is cheap on Thornmere while the
#   potion is dear on Silverfen, which is the craft the demo records. The
#   rest is ordinary: realms dearer and cheaper than one another, a weekly
#   rhythm, listings that sell and listings that expire.

set -euo pipefail

readonly VERSION="1.0.0"

# Every %(...)T below formats in UTC, whatever the caller's zone.
export TZ=UTC

out=""
anchor=""
days=14
seed=20261004

say () {
    printf '%s\n' "$*"
}

die () {
    printf 'venture-demo-market: %s\n' "$*" >&2
    exit 1
}

usage () {
    cat <<'USAGE'
venture-demo-market.sh - deterministic market data for the VENTURE demo

Usage:
  tools/venture-demo-market.sh --out DIR [--anchor EPOCH | --now TIME] [options]

Options:
  -o, --out DIR       Write the files under DIR (created; required)
  -a, --anchor EPOCH  The economy's clock, in Unix seconds on the hour; the
                      newest snapshot is taken at it
  -n, --now TIME      The same as any time `date -d` reads, rounded down to
                      the hour (default: VENTURE_DEMO_ECONOMY_NOW, else now)
  -d, --days N        Days of hourly history (default 14, 1 to 60)
  -s, --seed N        The hash seed (default 20261004); changes every price
  -h, --help          This
      --license       Licence and copyright

What it writes:
  DIR/evermoor/<realm>.jsonl   six realms (group evermoor-eu, GOLD), one
                               complete snapshot an hour each
  DIR/usd/marketplace.jsonl    ShopMart's listings of the supplier's SKUs (USD)
  DIR/usd/odds.jsonl           Northbet and Pinehill's odds on two matches

Examples:
  tools/venture-demo-market.sh --out build/demo/market
  tools/venture-demo-market.sh -o /tmp/m --now 2026-06-15T12:55:00Z
  tools/venture-demo-market.sh -o /tmp/m --anchor 1781524800 --days 2
  tools/venture-demo-market.sh -o /tmp/a -a 1781524800
  tools/venture-demo-market.sh -o /tmp/b -a 1781524800 && diff -r /tmp/a /tmp/b
USAGE
}

licence () {
    cat <<'LICENCE'
venture-demo-market.sh, part of VENTURE.
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
                [[ $# -ge 2 ]] || die "--out needs a directory"
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
                say "venture-demo-market.sh ${VERSION}"
                exit 0
                ;;
            *)
                die "unknown option \"$1\". Try --help."
                ;;
        esac
    done

    [[ -n "${out}" ]] || die "--out is required. Try --help."
    if [[ ! "${days}" =~ ^[0-9]+$ ]] || (( days < 1 || days > 60 ))
    then
        die "--days must be 1 to 60"
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
# The hash
#
# A small integer mixer (the "lowbias" finaliser) over 31 bits, so every
# product stays under 2^62 and bash's signed 64-bit arithmetic never
# overflows: overflow is undefined in the C bash is written in, and an
# answer that depended on it would not be the same everywhere.
# ──────────────────────────────────────────────────────────────────────────

hashed=0

# mix A B C D: sets ${hashed} to a number in [0, 2^31) from four small
# non-negative integers and the seed.
mix () {
    local h

    # Assignments, not (( )): an arithmetic command whose value is 0
    # "fails", and under set -e a hash of 0 would end the run.
    h=$(( (seed ^ ($1 * 73856093) ^ ($2 * 19349663) ^ ($3 * 83492791) ^ ($4 * 2654435)) & 0x7fffffff ))
    h=$(( ((h ^ (h >> 16)) * 0x45d9f3b) & 0x7fffffff ))
    h=$(( ((h ^ (h >> 16)) * 0x45d9f3b) & 0x7fffffff ))
    hashed=$(( h ^ (h >> 16) ))
}

# A count of minor units as a decimal string at @exponent places, the way
# the protocol wants money: never a JSON number, never a float.
decimal=""

as_decimal () {
    local minor="$1"
    local exponent="$2"
    local scale=$(( 10 ** exponent ))

    printf -v decimal '%d.%0*d' $(( minor / scale )) "${exponent}" $(( minor % scale ))
}

# ──────────────────────────────────────────────────────────────────────────
# The game world
# ──────────────────────────────────────────────────────────────────────────

# Six realms, one region. Their price level in permille of the item's base:
# Thornmere is a quiet realm with a glut of gatherers, Duskwatch a small
# one where everything is scarce, Silverfen the busy home realm.
readonly REALMS=(silverfen thornmere moonwell greyharbor emberfall duskwatch)
readonly REALM_NAMES=(Silverfen Thornmere Moonwell Greyharbor Emberfall Duskwatch)
readonly REALM_LEVEL=(1150 820 1000 1060 930 1240)
readonly REALM_POPULATION=(high low medium medium medium low)
readonly GROUP="evermoor-eu"

# Forty-one items: key, name, category path, base price in copper, the
# usual stack, and on how many of the six realms anyone lists it. The
# first seven keys are the demo's own products, which the demo links to
# instruments by these exact keys.
readonly ITEMS=(
    "silverleaf|Silverleaf|Trade goods/Herbs|1850|20|6"
    "duskroot|Duskroot|Trade goods/Herbs|4200|20|6"
    "copper-ore|Copper ore|Trade goods/Ore|1200|20|6"
    "ironstone-ore|Ironstone ore|Trade goods/Ore|6500|20|6"
    "crystal-vial|Crystal vial|Trade goods/Reagents|400|20|6"
    "healing-potion|Healing potion|Consumables/Potions|9500|5|6"
    "flask-of-endurance|Flask of endurance|Consumables/Flasks|68000|1|6"
    "mortar-and-pestle|Mortar and pestle|Gear/Tools|25000|1|4"
    "moonpetal|Moonpetal|Trade goods/Herbs|2600|20|6"
    "sungrass|Sungrass|Trade goods/Herbs|3300|20|6"
    "ghostcap|Ghostcap|Trade goods/Herbs|5400|10|5"
    "bloodthistle|Bloodthistle|Trade goods/Herbs|900|20|6"
    "dreamfoil|Dreamfoil|Trade goods/Herbs|4700|20|5"
    "tin-ore|Tin ore|Trade goods/Ore|1500|20|6"
    "mithril-ore|Mithril ore|Trade goods/Ore|8800|20|6"
    "thorium-ore|Thorium ore|Trade goods/Ore|11200|10|5"
    "starmetal-ore|Starmetal ore|Trade goods/Ore|14500|5|4"
    "moonsteel-bar|Moonsteel bar|Trade goods/Bars|12000|5|6"
    "copper-bar|Copper bar|Trade goods/Bars|2800|20|6"
    "iron-bar|Iron bar|Trade goods/Bars|9000|10|5"
    "runecloth|Runecloth|Trade goods/Cloth|2100|20|6"
    "mageweave|Mageweave|Trade goods/Cloth|1600|20|6"
    "rugged-leather|Rugged leather|Trade goods/Leather|2700|10|5"
    "arcane-dust|Arcane dust|Trade goods/Reagents|3500|10|6"
    "glowing-shard|Glowing shard|Trade goods/Reagents|18000|1|5"
    "mana-potion|Mana potion|Consumables/Potions|8000|5|6"
    "swiftness-potion|Swiftness potion|Consumables/Potions|6000|5|6"
    "elixir-of-giants|Elixir of giants|Consumables/Potions|15500|5|4"
    "flask-of-wisdom|Flask of wisdom|Consumables/Flasks|72000|1|5"
    "spiced-chili|Spiced chili|Consumables/Food|1200|10|6"
    "smoked-trout|Smoked trout|Consumables/Food|900|10|6"
    "tidewarden-blade|Tidewarden blade|Gear/Weapons|1450000|1|3"
    "ember-staff|Ember staff|Gear/Weapons|980000|1|3"
    "moonweave-robe|Moonweave robe|Gear/Armor|620000|1|4"
    "ironbark-shield|Ironbark shield|Gear/Armor|410000|1|4"
    "silverfen-ring|Silverfen signet|Gear/Armor|220000|1|4"
    "bog-frog|Bog frog|Pets|350000|1|6"
    "ember-whelp|Ember whelp|Pets|2800000|1|3"
    "clockwork-owl|Clockwork owl|Pets|1200000|1|4"
    "recipe-flask-of-wisdom|Recipe: Flask of wisdom|Recipes|1500000|1|3"
    "recipe-ember-staff|Recipe: Ember staff|Recipes|900000|1|3"
)

# The planted opportunities: realm, item, price level in permille, which
# replaces the realm's own. Kept in step with what the demo records and
# with docs/examples/arbitrage-wow-cross-realm.org and
# arbitrage-crafting.org, which quote them.
readonly PLANTED=(
    "thornmere moonsteel-bar 620"
    "silverfen moonsteel-bar 1450"
    "duskwatch moonsteel-bar 1500"
    "emberfall bog-frog 560"
    "duskwatch bog-frog 1380"
    "moonwell mithril-ore 600"
    "greyharbor mithril-ore 1250"
    "thornmere silverleaf 450"
    "silverfen healing-potion 1500"
)

# The anchor's hour on the listings' own clock: any number larger than the
# longest history (60 days of hours), so every hour counts up from zero.
readonly HOUR_ORIGIN=100000

# Monday first: a little dearer at the weekend, when people play.
readonly WEEKDAY_PERMILLE=(990 985 980 995 1010 1030 1020)

# WoW's time-left buckets as the least time a listing still had, in
# seconds (docs/plugins.org: expires_in_min is seconds, and a minimum).
# Inline arithmetic rather than a function: it runs for every listing, and
# a command substitution there is a fork a listing.
#   left = R >= 43200 ? 43200 : R >= 7200 ? 7200 : R >= 1800 ? 1800 : 0

# A JSON string's body: the names here are ours, so only quotes and
# backslashes could ever need it, and none do -- but a generator that
# writes a protocol escapes, rather than trusting its own data.
json_text=""

json_escape () {
    json_text="${1//\\/\\\\}"
    json_text="${json_text//\"/\\\"}"
}

write_realm () {
    local r="$1"
    local file="$2"
    local realm="${REALMS[r]}"
    local hours=$(( days * 24 ))
    local n_items=${#ITEMS[@]}
    local i k t

    # Per item: its price level here, whether the realm lists it, and per
    # listing slot the cycle that decides when each listing lives.
    local -a level present slots
    local -a phase life cycle duration stack
    local -a key name category base
    local key_i name_i category_i base_i stack_i realms_i
    local planted p_realm p_item p_level

    for (( i = 0; i < n_items; i++ ))
    do
        IFS='|' read -r key_i name_i category_i base_i stack_i realms_i <<< "${ITEMS[i]}"
        key[i]="${key_i}"
        name[i]="${name_i}"
        category[i]="${category_i}"
        base[i]="${base_i}"
        level[i]="${REALM_LEVEL[r]}"

        for planted in "${PLANTED[@]}"
        do
            read -r p_realm p_item p_level <<< "${planted}"
            if [[ "${p_realm}" == "${realm}" && "${p_item}" == "${key_i}" ]]
            then
                level[i]="${p_level}"
            fi
        done

        # Rarer things are listed on fewer realms; which ones is the
        # hash's choice, except that a planted realm always lists its item.
        mix "${r}" "${i}" 0 1
        present[i]=$(( (hashed % 6) < realms_i ? 1 : 0 ))
        if [[ "${level[i]}" != "${REALM_LEVEL[r]}" ]]
        then
            present[i]=1
        fi

        mix "${r}" "${i}" 0 2
        slots[i]=$(( stack_i > 1 ? 2 + hashed % 2 : 1 + hashed % 2 ))

        for (( k = 0; k < slots[i]; k++ ))
        do
            mix "${r}" "${i}" "${k}" 3
            phase[i * 3 + k]=$(( hashed % 48 ))

            # Slot 0 sells within hours of a two-day listing; slots 1 and 2
            # sometimes run the whole of a short listing and expire.
            mix "${r}" "${i}" "${k}" 4
            case "${k}" in
                0)
                    duration[i * 3 + k]=48
                    life[i * 3 + k]=$(( 3 + hashed % 9 ))
                    ;;
                1)
                    duration[i * 3 + k]=12
                    life[i * 3 + k]=$(( hashed % 3 == 0 ? 12 : 5 + hashed % 5 ))
                    ;;
                *)
                    duration[i * 3 + k]=24
                    life[i * 3 + k]=$(( hashed % 4 == 0 ? 24 : 6 + hashed % 11 ))
                    ;;
            esac
            # A slot may stand empty for an hour between listings --
            # except where an opportunity is planted, which must be there
            # in every snapshot, the newest included, for the demo to find.
            if [[ "${level[i]}" != "${REALM_LEVEL[r]}" ]]
            then
                cycle[i * 3 + k]="${life[i * 3 + k]}"
            else
                cycle[i * 3 + k]=$(( life[i * 3 + k] + (hashed >> 8) % 2 ))
            fi

            mix "${r}" "${i}" "${k}" 5
            stack[i * 3 + k]=$(( stack_i > 1 ? stack_i / 2 + 1 + hashed % (stack_i / 2 + 1) : 1 ))
        done
    done

    {
        json_escape "${REALM_NAMES[r]}"
        printf '{"type":"venue","key":"%s","name":"%s","kind":"auction_house","group":"%s","currency":"GOLD","attrs":{"population":"%s"}}\n' \
            "${realm}" "${json_text}" "${GROUP}" "${REALM_POPULATION[r]}"

        # vendor_sell is an integer count of copper, as the Blizzard
        # provider stores it and the wow_auction fee model reads it: a
        # decimal string there is no vendor price at all, and the
        # deposit silently vanishes. The demo's venues charge the percent
        # model's deposit_percent, so this attribute is what a venue
        # switched to wow_auction would read.
        for (( i = 0; i < n_items; i++ ))
        do
            json_escape "${name[i]}"
            printf '{"type":"instrument","key":"%s","name":"%s","kind":"item","category":"%s","attrs":{"vendor_sell":%d}}\n' \
                "${key[i]}" "${json_text}" "${category[i]}" "$(( base[i] / 20 ))"
        done

        # Oldest first: a snapshot older than the venue's newest is only
        # history, so the newest must come last to be the current one.
        local at stamp weekday g age gen_key price lines remaining left u
        local -A gen_price=()

        for (( t = 0; t < hours; t++ ))
        do
            at=$(( anchor - (hours - 1 - t) * 3600 ))
            printf -v stamp '%(%Y-%m-%dT%H:%M:%SZ)T' "${at}"
            weekday=$(( (at / 86400 + 3) % 7 ))
            # The listings' clock counts from the anchor, not from the
            # first hour written, so --days changes how far back the
            # history goes and never what the newest snapshot holds.
            u=$(( HOUR_ORIGIN - (hours - 1 - t) ))
            lines=""

            for (( i = 0; i < n_items; i++ ))
            do
                (( present[i] )) || continue

                for (( k = 0; k < slots[i]; k++ ))
                do
                    g=$(( (u + phase[i * 3 + k]) / cycle[i * 3 + k] ))
                    age=$(( (u + phase[i * 3 + k]) % cycle[i * 3 + k] ))
                    (( age < life[i * 3 + k] )) || continue

                    # One price for a listing's whole life: a seller
                    # posts once. -8% to +8% around the realm's level,
                    # and the day of the week it went up. Remembered as
                    # the decimal it is written as, so each listing is
                    # hashed and formatted once, not once an hour.
                    gen_key="${i}.${k}.${g}"
                    price="${gen_price[${gen_key}]:-}"
                    if [[ -z "${price}" ]]
                    then
                        mix "${r}" "${i}" "${k}" $(( 100 + g ))
                        price=$(( base[i] * level[i] * (920 + hashed % 161) * WEEKDAY_PERMILLE[weekday] / 1000000000 ))
                        (( price >= 1 )) || price=1
                        as_decimal "${price}" 4
                        price="${decimal}"
                        gen_price["${gen_key}"]="${price}"
                    fi

                    remaining=$(( (duration[i * 3 + k] - age) * 3600 ))
                    left=$(( remaining >= 43200 ? 43200 : remaining >= 7200 ? 7200 : remaining >= 1800 ? 1800 : 0 ))
                    lines+="{\"type\":\"listing\",\"venue\":\"${realm}\",\"instrument\":\"${key[i]}\",\"id\":\"${realm}.${key[i]}.${k}.${g}\",\"price\":\"${price}\",\"quantity\":${stack[i * 3 + k]},\"expires_in_min\":${left}}"$'\n'
                done
            done

            printf '{"type":"snapshot","venue":"%s","taken_at":"%s","complete":true}\n%s' \
                "${realm}" "${stamp}" "${lines}"
        done
    } > "${file}"
}

# ──────────────────────────────────────────────────────────────────────────
# The dropshipping marketplace
# ──────────────────────────────────────────────────────────────────────────

# The supplier's SKUs (plugins/exec/supplier-csv/sample-price-list.csv, by
# the same keys) as other sellers list them on ShopMart, in cents.
readonly SKUS=(
    "DS-1001|Bamboo cutting board|1999"
    "DS-1002|Silicone baking mat, set of 2|1249"
    "DS-1003|Stainless steel straws (8 pack)|799"
    "DS-1004|Ceramic pour-over cone \"V60\" style|1695"
    "DS-1005|Cast iron trivet|2200"
    "DS-1006|Linen tea towel|949"
)

write_marketplace () {
    local file="$1"
    local hours=$(( days * 24 ))
    local i k t at stamp g age price lines sku name cents remaining left u

    {
        printf '{"type":"venue","key":"shopmart","name":"ShopMart","kind":"marketplace","group":"us","currency":"USD"}\n'

        for (( i = 0; i < ${#SKUS[@]}; i++ ))
        do
            IFS='|' read -r sku name cents <<< "${SKUS[i]}"
            json_escape "${name}"
            printf '{"type":"instrument","key":"%s","name":"%s","kind":"sku","category":"Kitchen"}\n' \
                "${sku}" "${json_text}"
        done

        for (( t = 0; t < hours; t++ ))
        do
            at=$(( anchor - (hours - 1 - t) * 3600 ))
            printf -v stamp '%(%Y-%m-%dT%H:%M:%SZ)T' "${at}"
            u=$(( HOUR_ORIGIN - (hours - 1 - t) ))
            lines=""

            for (( i = 0; i < ${#SKUS[@]}; i++ ))
            do
                IFS='|' read -r sku name cents <<< "${SKUS[i]}"

                # Two competing sellers a SKU, each relisting every day or
                # so at a price a few percent either side of the going one.
                for (( k = 0; k < 2; k++ ))
                do
                    mix 7 "${i}" "${k}" 6
                    g=$(( (u + hashed % 24) / (20 + k * 9) ))
                    age=$(( (u + hashed % 24) % (20 + k * 9) ))
                    mix 7 "${i}" "${k}" $(( 200 + g ))
                    price=$(( cents * (960 + hashed % 81) / 1000 ))
                    as_decimal "${price}" 2
                    remaining=$(( (30 * 24 - age) * 3600 ))
                    left=$(( remaining >= 43200 ? 43200 : remaining >= 7200 ? 7200 : remaining >= 1800 ? 1800 : 0 ))
                    lines+="{\"type\":\"listing\",\"venue\":\"shopmart\",\"instrument\":\"${sku}\",\"id\":\"shopmart.${sku}.${k}.${g}\",\"price\":\"${decimal}\",\"quantity\":$(( 1 + hashed % 3 )),\"expires_in_min\":${left}}"$'\n'
                done
            done

            printf '{"type":"snapshot","venue":"shopmart","taken_at":"%s","complete":true}\n%s' \
                "${stamp}" "${lines}"
        done
    } > "${file}"
}

# ──────────────────────────────────────────────────────────────────────────
# The bookmakers
# ──────────────────────────────────────────────────────────────────────────

# Two tennis matches, two outcomes each, priced at two bookmakers. The
# first was played two days before the anchor -- the demo's settled
# surebet was staked on it -- and the second is still to be played; both
# are mispriced between the books, by about six and four percent.
write_odds () {
    local file="$1"
    local stamp hour
    local -a quote

    {
        printf '{"type":"venue","key":"northbet","name":"Northbet","kind":"bookmaker","group":"us","currency":"USD"}\n'
        printf '{"type":"venue","key":"pinehill","name":"Pinehill Sports","kind":"bookmaker","group":"us","currency":"USD"}\n'

        printf -v stamp '%(%Y-%m-%dT%H:%M:%SZ)T' $(( anchor - 47 * 3600 ))
        printf '{"type":"instrument","key":"evt-varga-okafor","name":"M. Varga v T. Okafor","kind":"event","category":"Tennis/Harbor Open","attrs":{"commence_time":"%s"}}\n' "${stamp}"
        printf '{"type":"instrument","key":"evt-varga-okafor:varga","name":"M. Varga","kind":"outcome","category":"Tennis/Harbor Open","parent":"evt-varga-okafor"}\n'
        printf '{"type":"instrument","key":"evt-varga-okafor:okafor","name":"T. Okafor","kind":"outcome","category":"Tennis/Harbor Open","parent":"evt-varga-okafor"}\n'

        printf -v stamp '%(%Y-%m-%dT%H:%M:%SZ)T' $(( anchor + 26 * 3600 ))
        printf '{"type":"instrument","key":"evt-lindqvist-moreau","name":"S. Lindqvist v A. Moreau","kind":"event","category":"Tennis/Harbor Open","attrs":{"commence_time":"%s"}}\n' "${stamp}"
        printf '{"type":"instrument","key":"evt-lindqvist-moreau:lindqvist","name":"S. Lindqvist","kind":"outcome","category":"Tennis/Harbor Open","parent":"evt-lindqvist-moreau"}\n'
        printf '{"type":"instrument","key":"evt-lindqvist-moreau:moreau","name":"A. Moreau","kind":"outcome","category":"Tennis/Harbor Open","parent":"evt-lindqvist-moreau"}\n'

        # hours-before-anchor venue outcome odds, oldest first. The books
        # drift together until, an hour or two before each match, they
        # disagree: Varga 2.10 at Northbet against Okafor 2.15 at Pinehill
        # is the pair the demo staked (1/2.10 + 1/2.15 = 0.941).
        for quote in \
            "54 northbet evt-varga-okafor:varga 1.95" \
            "54 northbet evt-varga-okafor:okafor 1.90" \
            "54 pinehill evt-varga-okafor:varga 1.92" \
            "54 pinehill evt-varga-okafor:okafor 1.93" \
            "51 northbet evt-varga-okafor:varga 2.10" \
            "51 northbet evt-varga-okafor:okafor 1.80" \
            "51 pinehill evt-varga-okafor:varga 1.85" \
            "51 pinehill evt-varga-okafor:okafor 2.15" \
            "3 northbet evt-lindqvist-moreau:lindqvist 1.70" \
            "3 northbet evt-lindqvist-moreau:moreau 2.25" \
            "3 pinehill evt-lindqvist-moreau:lindqvist 1.66" \
            "3 pinehill evt-lindqvist-moreau:moreau 2.30" \
            "0 northbet evt-lindqvist-moreau:lindqvist 1.80" \
            "0 northbet evt-lindqvist-moreau:moreau 2.05" \
            "0 pinehill evt-lindqvist-moreau:lindqvist 1.62" \
            "0 pinehill evt-lindqvist-moreau:moreau 2.40"
        do
            read -r -a quote <<< "${quote}"
            hour="${quote[0]}"
            printf -v stamp '%(%Y-%m-%dT%H:%M:%SZ)T' $(( anchor - hour * 3600 ))
            printf '{"type":"quote","venue":"%s","instrument":"%s","odds":"%s","format":"decimal","side":"back","taken_at":"%s"}\n' \
                "${quote[1]}" "${quote[2]}" "${quote[3]}" "${stamp}"
        done
    } > "${file}"
}

main () {
    local r

    parse_arguments "$@"

    mkdir -p -- "${out}/evermoor" "${out}/usd"

    for (( r = 0; r < ${#REALMS[@]}; r++ ))
    do
        write_realm "${r}" "${out}/evermoor/${REALMS[r]}.jsonl"
    done

    write_marketplace "${out}/usd/marketplace.jsonl"
    write_odds "${out}/usd/odds.jsonl"
}

# tests/demo-market.sh sources this file for its tables; executing it is
# what writes the files.
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]
then
    main "$@"
fi
