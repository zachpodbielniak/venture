#!/usr/bin/env bash
#
# demo-market.sh - the demo's market data and its trades, without a server
#
# Copyright (C) 2026 Zach Podbielniak
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# The demo generates fourteen days of market data with
# tools/venture-demo-market.sh, syncs it, and records five trades whose
# legs spend characters' purses and bookmaker wallets. Three things can go
# wrong there that only show up as a failed `make demo`, and only some of
# the time:
#
#   - the generator stops being deterministic -- something reads the clock
#     or $RANDOM -- and the demo's prices, the alert that must fire and the
#     trades' amounts drift apart from one run to the next;
#   - a line it writes breaks the JSON-lines protocol (a price as a JSON
#     number, a time with no zone, a count that is not an integer), which
#     fails the whole sync with a line number and nothing else;
#   - a trade spends a purse before the purse was funded. The holding
#     floor refuses it, at the clock times where two offsets happen to
#     swap -- the bug tests/demo-clock.sh was written for, in gold.
#
# What breaks if this regresses: `make demo`, and test-docs' quickstart
# that runs it, fail for some anchors and pass for others.
#
# The generator is run for real (three days, to stay quick) at fixed
# anchors and checked byte for byte, against the protocol, and for the
# opportunities the demo relies on. The seed is sourced with everything
# that talks to a server replaced by recorders, run at awkward clock
# times, and its money movements replayed in date order per purse.

set -euo pipefail

root="$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
demo="${root}/tools/venture-demo.sh"
generator="${root}/tools/venture-demo-market.sh"

fail () {
    printf 'not ok %s\n' "$*" >&2
    exit 1
}

[[ -f "${demo}" ]] || fail "missing ${demo}"
[[ -x "${generator}" ]] || fail "missing or not executable: ${generator}"
command -v python3 > /dev/null 2>&1 || fail "python3 is needed, as the demo needs it"

work="$(mktemp -d "${TMPDIR:-/tmp}/venture-demo-market-XXXXXX")"
trap '[[ -n "${KEEP:-}" ]] || rm -rf "${work}"' EXIT

# ──────────────────────────────────────────────────────────────────────────
# The generator
# ──────────────────────────────────────────────────────────────────────────

# An anchor on the hour, the same instant written as a time rather than
# seconds, an anchor a week later -- which must move the times and nothing
# else -- and one three days and five hours on, which shifts the weekly
# rhythm under every price and must still hold the planted opportunities.
readonly ANCHOR_A=1781524800
readonly ANCHOR_B=$(( ANCHOR_A + 7 * 86400 ))
readonly ANCHOR_C=$(( ANCHOR_A + 3 * 86400 + 5 * 3600 ))

"${generator}" --out "${work}/a1" --anchor "${ANCHOR_A}" --days 3 \
    || fail "the generator failed at anchor ${ANCHOR_A}"
"${generator}" --out "${work}/a2" --now "@$(( ANCHOR_A + 1799 ))" --days 3 \
    || fail "the generator failed at --now, half an hour past the anchor"
"${generator}" --out "${work}/b" --anchor "${ANCHOR_B}" --days 3 \
    || fail "the generator failed at anchor ${ANCHOR_B}"
"${generator}" --out "${work}/c" --anchor "${ANCHOR_C}" --days 3 \
    || fail "the generator failed at anchor ${ANCHOR_C}"

# Byte for byte, whether the anchor is given in seconds or as a time it
# rounds down to.
diff -r "${work}/a1" "${work}/a2" > /dev/null \
    || fail "the generator wrote different bytes for the same anchor"

# A refusal, not a guess: an anchor off the hour is not the demo's clock.
if "${generator}" --out "${work}/bad" --anchor $(( ANCHOR_A + 60 )) 2> /dev/null
then
    fail "the generator accepted an anchor that is not on the hour"
fi

# The rest is easier to say in Python: the protocol rules, the planted
# opportunities, the churn the sale estimate needs, and that a second
# anchor moves only the times.
python3 - "${work}/a1" "${work}/b" "${work}/c" "${ANCHOR_A}" "${ANCHOR_C}" <<'PYTHON' || fail "the generated market data is wrong (above)"
import json
import os
import re
import sys

dir_a, dir_b, dir_c = sys.argv[1], sys.argv[2], sys.argv[3]
anchor_a, anchor_c = int(sys.argv[4]), int(sys.argv[5])

DECIMAL = re.compile(r'^[0-9]+(\.[0-9]+)?$')
STAMP = re.compile(r'^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z$')
KNOWN = {"venue", "instrument", "snapshot", "listing", "stat", "quote", "entry",
         "record", "cursor", "not_modified", "log", "error", "result"}
problems = []


def problem(text):
    if len(problems) < 20:
        problems.append(text)


def epoch(stamp):
    import calendar
    import time
    return calendar.timegm(time.strptime(stamp, "%Y-%m-%dT%H:%M:%SZ"))


def read(path, places, anchor):
    """Checks one file against protocol 1 and returns its snapshots."""
    venues, instruments = set(), set()
    snapshots = []
    current = None
    for n, line in enumerate(open(path, encoding="utf-8"), 1):
        where = "%s:%d" % (os.path.basename(path), n)
        if len(line.encode()) > 1024 * 1024:
            problem("%s: longer than file_jsonl's line cap" % where)
        message = json.loads(line)
        kind = message.get("type")
        if kind not in KNOWN:
            problem("%s: unknown type %r" % (where, kind))
            continue
        for key in ("taken_at", "published_at"):
            if key in message and not STAMP.match(message[key]):
                problem("%s: %s %r has no zone or is not ISO 8601" % (where, key, message[key]))
        if kind == "venue":
            venues.add(message["key"])
        elif kind == "instrument":
            instruments.add(message["key"])
            parent = message.get("parent")
            if parent is not None and parent not in instruments:
                problem("%s: parent %r named before it was declared" % (where, parent))
        elif kind == "snapshot":
            if message["venue"] not in venues:
                problem("%s: a snapshot of undeclared venue %r" % (where, message["venue"]))
            if message.get("complete") is not True:
                problem("%s: a snapshot that is not complete; vanished listings would not be judged" % where)
            current = {"venue": message["venue"], "at": epoch(message["taken_at"]), "listings": []}
            snapshots.append(current)
        elif kind == "listing":
            if current is None or message["venue"] != current["venue"]:
                problem("%s: a listing outside its venue's snapshot" % where)
                continue
            if message["instrument"] not in instruments:
                problem("%s: a listing of undeclared instrument %r" % (where, message["instrument"]))
            price = message["price"]
            if not isinstance(price, str) or not DECIMAL.match(price) or len(price) > 40:
                problem("%s: price %r is not a decimal string" % (where, price))
            elif "." in price and len(price.split(".")[1]) > places:
                problem("%s: price %r has more places than the currency's %d" % (where, price, places))
            for key, least in (("quantity", 1), ("expires_in_min", 0)):
                value = message.get(key)
                if type(value) is not int or value < least:
                    problem("%s: %s %r is not an integer of at least %d" % (where, key, value, least))
            if not isinstance(message.get("id"), str):
                problem("%s: a listing with no id; the sale estimate needs one" % where)
            current["listings"].append(message)
        elif kind == "quote":
            if not DECIMAL.match(message.get("odds", "")) or float(message["odds"]) <= 1:
                problem("%s: odds %r are not decimal odds over 1" % (where, message.get("odds")))
            if epoch(message["taken_at"]) > anchor:
                problem("%s: a quote after the anchor" % where)
    times = [s["at"] for s in snapshots]
    if times != sorted(set(times)):
        problem("%s: snapshots are not one an hour, oldest first" % os.path.basename(path))
    if snapshots and times[-1] != anchor:
        problem("%s: the newest snapshot is not at the anchor" % os.path.basename(path))
    if snapshots and any(b - a != 3600 for a, b in zip(times, times[1:])):
        problem("%s: an hour is missing" % os.path.basename(path))
    return snapshots


def cheapest(snapshot, instrument):
    prices = [int(l["price"].replace(".", "")) for l in snapshot["listings"] if l["instrument"] == instrument]
    return min(prices) if prices else None


def check(directory, anchor):
    realms = {}
    for name in sorted(os.listdir(os.path.join(directory, "evermoor"))):
        realms[name[:-len(".jsonl")]] = read(os.path.join(directory, "evermoor", name), 4, anchor)
    if sorted(realms) != ["duskwatch", "emberfall", "greyharbor", "moonwell", "silverfen", "thornmere"]:
        problem("the realms are %s" % sorted(realms))
    read(os.path.join(directory, "usd", "marketplace.jsonl"), 2, anchor)
    read(os.path.join(directory, "usd", "odds.jsonl"), 2, anchor)

    # The planted opportunities, in every snapshot: the demo's alert,
    # scans and trades are written against them. Prices are copper.
    for at in range(len(realms["thornmere"])):
        snap = {realm: snaps[at] for realm, snaps in realms.items()}
        leaf = cheapest(snap["thornmere"], "silverleaf")
        if leaf is None or leaf >= 1000:
            problem("Thornmere's silverleaf is %r copper; the demo's alert fires under 1000" % leaf)
        bars, dear = cheapest(snap["thornmere"], "moonsteel-bar"), cheapest(snap["silverfen"], "moonsteel-bar")
        if bars is None or dear is None or bars * 2 > dear:
            problem("moonsteel bars are %r on Thornmere and %r on Silverfen; the flip needs double" % (bars, dear))
        potion = cheapest(snap["silverfen"], "healing-potion")
        if potion is None or potion < 12000:
            problem("Silverfen's healing potion is %r copper; the craft sells at over 12000" % potion)
        for realm, s in snap.items():
            for l in s["listings"]:
                if l["instrument"] == "flask-of-endurance" and int(l["price"].replace(".", "")) >= 200000:
                    problem("a flask at %s on %s: the alert that must never fire would" % (l["price"], realm))

    # Churn: listings that vanish while they still had time left are what
    # the store counts as sold; ones that ran out are expired. Both must
    # happen, or the sale estimate reads empty.
    sold = expired = 0
    for snaps in realms.values():
        for before, after in zip(snaps, snaps[1:]):
            still = {l["id"] for l in after["listings"]}
            for l in before["listings"]:
                if l["id"] not in still:
                    if l["expires_in_min"] > 3600:
                        sold += 1
                    else:
                        expired += 1
    if sold < 100 or expired < 10:
        problem("only %d listings sold and %d expired over three days" % (sold, expired))


check(dir_a, anchor_a)
check(dir_c, anchor_c)

# A week later moves the times and nothing else.
STAMPS = re.compile(r'"(taken_at|commence_time)":"[^"]*"')
for path, _, files in os.walk(dir_a):
    for name in files:
        a = open(os.path.join(path, name), encoding="utf-8").read()
        b = open(os.path.join(dir_b, os.path.relpath(path, dir_a), name), encoding="utf-8").read()
        if STAMPS.sub("", a) != STAMPS.sub("", b):
            problem("%s: an anchor a week later changed more than the times" % name)

for text in problems:
    print("  " + text, file=sys.stderr)
sys.exit(1 if problems else 0)
PYTHON

# ──────────────────────────────────────────────────────────────────────────
# The seed
# ──────────────────────────────────────────────────────────────────────────

# shellcheck disable=SC1090
source "${demo}"

readonly SEP=$'\x1f'

# One call a line. A value with a newline in it -- fee parameters, a
# source's settings -- is written with the newline as a space, or the rest
# of the call would read as a line of its own.
record () {
    local IFS="${SEP}"
    local call="$*"

    printf '%s\n' "${call//$'\n'/ }" >> "${log}"
}

next_id () {
    local id

    id=$(( $(cat "${work}/next-id") + 1 ))
    printf '%s' "${id}" > "${work}/next-id"
    printf '%s' "${id}"
}

# The ids come from a file because these run inside $(...).
make_record () {
    local id

    id="$(next_id)"
    record "${id}" "$@"
    printf '%s' "${id}"
}

ctl () {
    record ctl "$@"
}

step () {
    :
}

account_id () {
    printf '1000'
}

generate_market () {
    :
}

sync_source () {
    record sync "$@"
}

promote () {
    local id

    id="$(next_id)"
    record promote "${id}" "$@"
    printf '%s' "${id}"
}

record_trade () {
    local id

    id="$(next_id)"
    record trade "${id}" "$@"
    printf '%s' "${id}"
}

trade_leg_id () {
    printf 'leg-%s-%s' "$1" "$2"
}

trade_action () {
    record action "$@"
}

evaluate_rule () {
    record evaluate "$@"
}

# What the calculator says for 2.10 and 2.15 over 100.00 USD.
surebet_split () {
    printf '50.59 USD\n106.23 USD\n49.41 USD\n106.23 USD\n'
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

    # Both read by the sourced seed: the default organization's id and the
    # economy's ids, filled again by seed_virtual_economy.
    # shellcheck disable=SC2034
    home_org=1
    # shellcheck disable=SC2034
    economy=()
    seed_virtual_economy
    seed_evermoor_market
    seed_trading_desk

    # economy_anchor is set by set_economy_clock in the sourced demo.
    # shellcheck disable=SC2154
    python3 - "${log}" "${economy_anchor}" "${label}" > "${work}/offsets-${#runs[@]}" <<'PYTHON' \
        || fail "${label}: the trades' money does not add up (above)"
import calendar
import json
import re
import sys
import time

log_path, anchor, label = sys.argv[1], int(sys.argv[2]), sys.argv[3]
SEP = "\x1f"
EXPONENT = {"GOLD": 4, "USD": 2, "TICKET": 0, "BREWFEST": 0}
STAMP = re.compile(r"[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z")
problems = []


def epoch(stamp):
    return calendar.timegm(time.strptime(stamp, "%Y-%m-%dT%H:%M:%SZ"))


def money(text, default=None):
    """Minor units and currency of a demo amount: 7g 23s GOLD, 0.6800 GOLD, 60.00."""
    text = text.strip()
    coins = re.fullmatch(r"((?:[0-9]+[gsc] )+)GOLD", text)
    if coins:
        units = {"g": 10000, "s": 100, "c": 1}
        return sum(int(c[:-1]) * units[c[-1]] for c in coins.group(1).split()), "GOLD"
    plain = re.fullmatch(r"(-?)([0-9]+)(?:\.([0-9]+))?(?: ([A-Z][A-Z0-9_]+))?", text)
    if not plain or (plain.group(4) is None and default is None):
        raise ValueError("cannot read the amount %r" % text)
    currency = plain.group(4) or default
    places = EXPONENT[currency]
    fraction = (plain.group(3) or "").ljust(places, "0")
    if len(fraction) > places:
        raise ValueError("%r has more places than %s" % (text, currency))
    value = int(plain.group(2)) * 10 ** places + int(fraction or 0)
    return (-value if plain.group(1) else value), currency


def fields(args):
    out = {}
    for arg in args:
        if "=" in arg:
            key, value = arg.split("=", 1)
            out[key] = value
    return out


calls = [line.rstrip("\n").split(SEP) for line in open(log_path, encoding="utf-8")]
records, kinds = {}, {}
for call in calls:
    if call[0].isdigit():
        records[call[0]] = fields(call[2:])
        kinds[call[0]] = call[1]
    elif call[0] in ("promote", "trade"):
        records[call[1]] = fields(call[2:])
        kinds[call[1]] = call[0]

# Where money is held: a location, through the account (purse, wallet) on
# it. A venue's money moves through its location; a venue with none moves
# the organization's cash, which is not a holding and has no floor.
account_location = {i: r["location_id"] for i, r in records.items()
                    if kinds[i] == "account" and r.get("location_id")}
venue_location = {i: r["location_id"] for i, r in records.items()
                  if kinds[i] == "venue" and r.get("location_id")}
posted_sessions, posted_journals = set(), set()
for call in calls:
    if call[0] == "ctl" and call[1:3] == ["update", "venue"] and "location_id" in fields(call[4:]):
        venue_location[call[3]] = fields(call[4:])["location_id"]
    if call[0] == "ctl" and call[1:3] == ["act", "session"] and call[4:5] == ["post"]:
        posted_sessions.add(call[3])
    if call[0] == "ctl" and call[1:3] == ["journal", "post"]:
        posted_journals.add(call[3])

events = []        # (time, 0 spend | 1 income, location, currency, delta, what)


def move(at, location, delta, currency, what):
    if location and delta:
        events.append((at, 0 if delta < 0 else 1, location, currency, delta, what))


for i, r in records.items():
    kind = kinds[i]
    if kind == "session_yield" and r.get("amount") and r["session_id"] in posted_sessions:
        session = records[r["session_id"]]
        value, currency = money(r["amount"])
        move(session["ended_at"], session["location_id"], value, currency, "session %s" % r["session_id"])
    elif kind == "sale" and r.get("cash_account_id") in account_location:
        gross, currency = money(r["gross"])
        fees = money(r["fees"])[0] if r.get("fees") else 0
        move(r["occurred_at"], account_location[r["cash_account_id"]], gross - fees, currency, "sale %s" % i)
    elif kind == "expense" and r.get("cash_account_id") in account_location:
        value, currency = money(r["amount"])
        move(r["occurred_at"], account_location[r["cash_account_id"]], -value, currency, "expense %s" % i)
    elif kind == "holding_txn" and r.get("account_id") in account_location:
        value, currency = money(r["amount"])
        move(r["occurred_at"], account_location[r["account_id"]], value, currency, "holding_txn %s" % i)
    elif kind == "journal_line" and r.get("account_id") in account_location and r["journal_id"] in posted_journals:
        journal = records[r["journal_id"]]
        value, currency = money(r["amount"], journal.get("currency"))
        move(journal["occurred_at"], account_location[r["account_id"]],
             value if r["side"] == "debit" else -value, currency, "journal %s" % r["journal_id"])

# The trades: executed legs at their time, planned ones when the execute
# action runs them, each on its venue's purse.
OUT = {"buy", "stake", "fee"}
IN = {"sell", "payout", "refund"}
trades, executes, closes = {}, {}, {}
for call in calls:
    if call[0] == "action" and call[1] == "execute":
        executes[call[2]] = fields(call[3:])["occurred_at"]
    elif call[0] == "action" and call[1] in ("close", "abandon"):
        closes[call[2]] = fields(call[3:]).get("closed_at") or fields(call[3:]).get("abandoned_at")
used_locations = set()
for i, r in records.items():
    if kinds[i] != "trade":
        continue
    legs = json.loads(r["legs"])
    trades[i] = legs
    last = None
    for leg in legs:
        at = leg.get("occurred_at")
        if leg["status"] == "planned":
            at = executes.get("leg-%s-%s" % (i, leg["kind"]))
            if at is None:
                continue
        elif leg["status"] != "executed":
            continue
        if leg.get("amount"):
            value, currency = money(leg["amount"])
        else:
            unit, currency = money(leg["unit_price"])
            value = unit * leg["quantity"]
        fees = money(leg["fees"])[0] if leg.get("fees") else 0
        if leg["kind"] in OUT or (leg["kind"] == "transfer" and value > 0):
            delta = -abs(value) - fees
        elif leg["kind"] in IN or (leg["kind"] == "transfer" and value < 0):
            delta = abs(value) - fees
        else:
            delta = -fees
        location = venue_location.get(str(leg.get("venue_id")))
        if location:
            used_locations.add(location)
        move(at, location, delta, currency, "trade %s %s leg" % (i, leg["kind"]))
        last = max(last, at) if last else at
    if i not in closes:
        problems.append("trade %s (%s) is never closed or abandoned" % (i, r.get("name")))
    elif last and epoch(closes[i]) < epoch(last):
        problems.append("trade %s closes at %s, before its last leg at %s" % (i, closes[i], last))

if len(trades) != 5:
    problems.append("%d trades recorded; the demo records five" % len(trades))
if len(used_locations) < 4:
    problems.append("the trades spend from %d purses and wallets; Brisk, Tallow and both books expected"
                    % len(used_locations))

# Undated movements (the transfer action) happen now, after everything.
balance = {}
for at, _, location, currency, delta, what in sorted(events, key=lambda e: (epoch(e[0]), e[1])):
    if epoch(at) > anchor:
        problems.append("%s is dated %s, after the anchor" % (what, at))
    key = (location, currency)
    balance[key] = balance.get(key, 0) + delta
    if balance[key] < 0:
        problems.append("%s at %s spends location %s's %s before it was funded (balance %d)"
                        % (what, at, location, currency, balance[key]))

for text in problems[:20]:
    print("  %s: %s" % (label, text), file=sys.stderr)

# Every timestamp as an offset from the anchor, for the caller to compare.
for line in open(log_path, encoding="utf-8"):
    for stamp in STAMP.findall(line):
        print(epoch(stamp) - anchor)
sys.exit(1 if problems else 0)
PYTHON

    if (( ${#runs[@]} > 0 )) && ! cmp -s "${work}/offsets-0" "${work}/offsets-${#runs[@]}"
    then
        fail "${label}: the seed's dates sit at different offsets than at ${runs[0]}, so their order depends on the clock"
    fi

    runs+=("${label}")
}

declare -a runs=()
log=""

# On the hour, in its last minutes, across a day, a leap day and a year.
for when in \
    "2026-06-15T12:00:00Z" \
    "2026-06-15T12:59:59Z" \
    "2024-02-29T00:00:00Z" \
    "2025-12-31T23:59:59Z" \
    ""
do
    run_at "${when}"
done

printf 'ok the market data is deterministic and every trade is funded at %d clock times\n' "${#runs[@]}"
