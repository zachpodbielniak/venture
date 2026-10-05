#!/usr/bin/env bash
#
# demo-accounts.sh - the demo's operator accounts, without a server
#
# Copyright (C) 2026 Zach Podbielniak
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# The demo pushes Evermoor's characters, banks, auctions, mail and sixty
# days of ledger, written by tools/venture-demo-accounts.sh, to a `push`
# data source, and posts the ledger to the books a day at a time into the
# characters' purses. What can go wrong there only shows up as a failed
# `make demo`, and only at some clock times:
#
#   - the generator stops being deterministic, and the attention list,
#     the alert that must fire and the books drift from run to run;
#   - a line breaks the account-operations contract (an unknown member, a
#     price with five places, a time with no zone, rows of an account
#     before its snapshot), which fails the whole push with a line number;
#   - a purse is spent before it was funded: a buy the gold on hand could
#     not pay for, or a balance line that disagrees with the ledger it
#     closes. The books post exactly this ledger into holdings that may
#     never go below zero, so the push would go in and `accounts post`
#     would be refused, or post capital nobody had;
#   - the anchor lands where the demo's story stops holding: no auction
#     waiting for a login, none running out within twelve hours, no mail
#     about to be lost, no flips, no stale alt.
#
# What breaks if this regresses: `make demo` fails at some hours of the
# day and passes at others, or the /accounts pages it shows are empty
# where the walk-through says they are not.
#
# The generator is run at anchors on the hour, half an hour past one (the
# same bytes), a week on, just after midnight UTC (today has no ledger
# yet) and at an odd hour, and every body is checked by an independent
# reading of the contract and replayed account by account.

set -euo pipefail

root="$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
generator="${root}/tools/venture-demo-accounts.sh"
demo="${root}/tools/venture-demo.sh"

fail () {
    printf 'not ok %s\n' "$*" >&2
    exit 1
}

[[ -x "${generator}" ]] || fail "missing or not executable: ${generator}"
command -v python3 > /dev/null 2>&1 || fail "python3 is needed, as the demo needs it"

work="$(mktemp -d "${TMPDIR:-/tmp}/venture-demo-accounts-XXXXXX")"
trap '[[ -n "${KEEP:-}" ]] || rm -rf "${work}"' EXIT

# A Monday noon, the same half an hour later, a week on, ten past
# midnight UTC two days later (rounded to the hour: Tallow's logout was
# the evening before), and an odd hour three days and five hours on.
readonly ANCHOR_A=1781524800
readonly ANCHOR_B=$(( ANCHOR_A + 7 * 86400 ))
readonly ANCHOR_C=$(( ANCHOR_A / 86400 * 86400 + 2 * 86400 ))
readonly ANCHOR_D=$(( ANCHOR_A + 3 * 86400 + 5 * 3600 ))
readonly ANCHOR_E=$(( ANCHOR_C + 3600 ))

"${generator}" --out "${work}/a1.jsonl" --anchor "${ANCHOR_A}" \
    || fail "the generator failed at anchor ${ANCHOR_A}"
"${generator}" --out "${work}/a2.jsonl" --now "@$(( ANCHOR_A + 1799 ))" \
    || fail "the generator failed at --now, half an hour past the anchor"
cmp -s "${work}/a1.jsonl" "${work}/a2.jsonl" \
    || fail "the generator wrote different bytes for the same anchor"

for anchor in "${ANCHOR_B}" "${ANCHOR_C}" "${ANCHOR_D}" "${ANCHOR_E}"
do
    "${generator}" --out "${work}/${anchor}.jsonl" --anchor "${anchor}" \
        || fail "the generator failed at anchor ${anchor}"
done

"${generator}" --out "${work}/short.jsonl" --anchor "${ANCHOR_D}" --days 7 \
    || fail "the generator failed with --days 7"

# A refusal, not a guess: an anchor off the hour is not the demo's clock.
if "${generator}" --out "${work}/bad.jsonl" --anchor $(( ANCHOR_A + 60 )) 2> /dev/null
then
    fail "the generator accepted an anchor that is not on the hour"
fi

# ──────────────────────────────────────────────────────────────────────────
# The contract and the replay
# ──────────────────────────────────────────────────────────────────────────

check () {
    local file="$1"
    local anchor="$2"
    local days="$3"

    python3 - "${file}" "${anchor}" "${days}" <<'PYTHON' || fail "$(basename "${file}"): see above"
import datetime
import json
import re
import sys

path, anchor, days = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])

DECIMAL = re.compile(r"^[0-9]+(\.[0-9]{1,4})?$")
TIME = re.compile(r"^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z$")

# The members each account-operations kind may carry: anything else is
# refused by the server, the whole push with it.
MEMBERS = {
    "account": ({"type", "key", "kind"}, {"name", "group", "venue", "last_seen", "attrs"}),
    "account_snapshot": ({"type", "account", "at", "covers"}, set()),
    "balance": ({"type", "account", "currency", "amount"}, {"at"}),
    "holding": ({"type", "account", "place", "instrument", "quantity"}, {"at"}),
    "position": ({"type", "account", "venue", "id", "instrument", "quantity", "price", "expires_at"},
                 {"bid", "posted_at"}),
    "inbound": ({"type", "account", "id"},
                {"sender", "subject", "money", "instrument", "quantity", "expires_at", "returned", "cod"}),
    "txn": ({"type", "id", "account", "kind", "at"},
            {"venue", "instrument", "quantity", "unit_price", "amount", "counterparty", "source"}),
}
MONEY = {"amount", "price", "bid", "money", "cod", "unit_price", "min", "market", "historical", "sale_avg"}
TIMES = {"at", "last_seen", "expires_at", "posted_at", "taken_at"}
PLACES = {"bag", "bank", "reagent_bank", "warbank", "guild", "mail", "auction", "void", "equipped",
          "currency", "other"}


def die(n, message):
    print(f"not ok line {n}: {message}", file=sys.stderr)
    sys.exit(1)


def when(text):
    return int(datetime.datetime.strptime(text, "%Y-%m-%dT%H:%M:%SZ")
               .replace(tzinfo=datetime.timezone.utc).timestamp())


venues, instruments, accounts = set(), set(), {}
snapshotted, rows_seen = {}, set()
balances, txns, positions, inbound, ids = {}, [], [], [], set()
last_txn_at = 0

for n, line in enumerate(open(path, encoding="utf-8"), 1):
    message = json.loads(line)
    kind = message.get("type")
    if not isinstance(message, dict) or kind is None:
        die(n, "not an object with a type")
    for name, value in message.items():
        if name in MONEY and not (isinstance(value, str) and DECIMAL.match(value)):
            die(n, f"{name} is not a decimal string of at most four places")
        if name in TIMES and not (isinstance(value, str) and TIME.match(value)):
            die(n, f"{name} is not a UTC time")
        if name == "quantity" and not (isinstance(value, int) and value >= 0):
            die(n, "quantity is not a whole number")
    if kind in MEMBERS:
        required, optional = MEMBERS[kind]
        missing = required - message.keys()
        unknown = message.keys() - required - optional
        if missing or unknown:
            die(n, f"{kind}: missing {sorted(missing)}, unknown {sorted(unknown)}")
    if kind == "venue":
        venues.add(message["key"])
    elif kind == "instrument":
        instruments.add(message["key"])
    elif kind == "account":
        accounts[message["key"]] = message
        if message["kind"] not in ("character", "shared", "guild", "other"):
            die(n, "an account kind outside the contract")
    elif kind == "account_snapshot":
        account = message["account"]
        if account in snapshotted:
            die(n, "two snapshots for one account")
        if account in rows_seen:
            die(n, "a snapshot after the account's rows")
        snapshotted[account] = (when(message["at"]), set(message["covers"]))
    if kind in ("balance", "holding", "position", "inbound"):
        account = message["account"]
        rows_seen.add(account)
        if account not in accounts:
            die(n, f"a row for an account never described: {account}")
        covered = {"balance": "balances", "holding": "holdings", "position": "positions",
                   "inbound": "inbound"}[kind]
        if account in snapshotted and covered not in snapshotted[account][1]:
            die(n, f"{account}'s snapshot does not cover {covered}")
    if kind == "balance":
        if message["currency"] != "GOLD":
            die(n, "a balance not in the source's currency")
        balances.setdefault(message["account"], []).append((when(message["at"]), message["amount"]))
    elif kind == "holding":
        if message["place"] not in PLACES:
            die(n, "a place outside the contract")
        if message["instrument"] not in instruments:
            die(n, "a holding of an undeclared instrument")
    elif kind == "position":
        if message["venue"] not in venues or message["instrument"] not in instruments:
            die(n, "a position at an undeclared venue or of an undeclared instrument")
        if message["quantity"] < 1:
            die(n, "a position of nothing")
        if message["id"] in ids:
            die(n, "a position id twice")
        ids.add(message["id"])
        positions.append(message)
    elif kind == "inbound":
        if not ({"money", "instrument", "cod"} & message.keys()):
            die(n, "an inbound with neither money, an item nor cash on delivery")
        if message["id"] in ids:
            die(n, "an inbound id twice")
        ids.add(message["id"])
        inbound.append(message)
    elif kind == "txn":
        at = when(message["at"])
        if at < last_txn_at:
            die(n, "the ledger is not oldest first")
        last_txn_at = at
        if message["id"] in ids:
            die(n, "a txn id twice")
        ids.add(message["id"])
        k = message["kind"]
        if k in ("sale", "buy", "expired", "cancelled") and "instrument" not in message:
            die(n, f"a {k} with no instrument")
        if k in ("expired", "cancelled") and ("amount" in message or "unit_price" in message):
            die(n, f"an {k} row carrying money")
        if k in ("sale", "buy", "income", "expense") and "amount" not in message:
            die(n, f"a {k} with no amount")
        if "venue" in message and message["venue"] not in venues:
            die(n, "a txn at an undeclared venue")
        if at > anchor:
            die(n, "a ledger row after the anchor")
        txns.append(message)

def copper(text):
    whole, _, frac = text.partition(".")
    return int(whole) * 10000 + int((frac + "0000")[:4])

# Every account's gold, replayed: the earliest balance line opens it, the
# ledger moves it, and no buy or expense may take it below zero. Each
# balance line must be the replayed figure at its time; a balance dated
# after the account's snapshot would be a balance from the future.
for key in accounts:
    points = sorted(balances.get(key, []))
    if not points:
        print(f"not ok {key} has no gold at all", file=sys.stderr)
        sys.exit(1)
    first_txn = min((when(t["at"]) for t in txns if t["account"] == key), default=None)
    if first_txn is not None and points[0][0] >= first_txn:
        print(f"not ok {key}'s first balance is not before its first ledger row", file=sys.stderr)
        sys.exit(1)
    gold = copper(points[0][1])
    pending = [t for t in txns if t["account"] == key]
    i = 0
    for at, amount in points:
        while i < len(pending) and when(pending[i]["at"]) <= at:
            t = pending[i]
            sign = {"sale": 1, "income": 1, "buy": -1, "expense": -1}.get(t["kind"], 0)
            gold += sign * copper(t.get("amount", "0"))
            if gold < 0:
                print(f"not ok {key} spent gold it did not have at {t['at']}", file=sys.stderr)
                sys.exit(1)
            i += 1
        if gold != copper(amount):
            print(f"not ok {key}'s balance at {at} is {amount}, the ledger says {gold}", file=sys.stderr)
            sys.exit(1)
        if key in snapshotted and at > snapshotted[key][0]:
            print(f"not ok {key} has a balance after its snapshot", file=sys.stderr)
            sys.exit(1)
    for t in pending[i:]:
        sign = {"sale": 1, "income": 1, "buy": -1, "expense": -1}.get(t["kind"], 0)
        gold += sign * copper(t.get("amount", "0"))
        if gold < 0:
            print(f"not ok {key} spent gold it did not have at {t['at']}", file=sys.stderr)
            sys.exit(1)
    if pending[i:]:
        print(f"not ok {key} has ledger rows after its last balance", file=sys.stderr)
        sys.exit(1)

# Positions were posted while their character was logged in, and expire
# after they were posted.
for p in positions:
    snapshot_at = snapshotted[p["account"]][0]
    if when(p.get("posted_at", p["expires_at"])) > snapshot_at:
        print(f"not ok position {p['id']} was posted after its account's snapshot", file=sys.stderr)
        sys.exit(1)
    if "posted_at" in p and when(p["expires_at"]) <= when(p["posted_at"]):
        print(f"not ok position {p['id']} expires before it was posted", file=sys.stderr)
        sys.exit(1)

# The story the walk-through tells, whatever the hour.
def require(condition, what):
    if not condition:
        print(f"not ok at anchor {anchor}: {what}", file=sys.stderr)
        sys.exit(1)

require(len(accounts) == 5, "five accounts")
require(len(positions) == 40, "forty posted auctions")
for who in ("Brisk-Thornmere", "Tallow-Silverfen"):
    mine = [when(p["expires_at"]) for p in positions if p["account"] == who]
    seen = snapshotted[who][0]
    require(any(seen < e <= anchor for e in mine), f"{who} has an auction that ran out after the logout")
    require(any(anchor + 3600 < e <= anchor + 12 * 3600 for e in mine),
            f"{who} has an auction running out within twelve hours")
require(any(anchor < when(m["expires_at"]) <= anchor + 72 * 3600 for m in inbound),
        "a mail about to be lost")
quill = accounts["Quill-Duskwatch"]
require(anchor - when(quill["last_seen"]) >= 14 * 86400, "an alt away long enough to be stale")
require(anchor - when(quill["last_seen"]) < 30 * 86400, "the alt not away long enough for the 30-day rule")
bought = [t for t in txns if t["kind"] == "buy" and t.get("instrument") == "moonsteel-bar"]
sold = [t for t in txns if t["kind"] == "sale" and t.get("instrument") == "moonsteel-bar"]
require(len(bought) >= days // 4 and len(sold) >= days // 4, "moonsteel bars bought and sold: the flips")
require(all(t["account"] == "Brisk-Thornmere" for t in bought), "Brisk buys the bars")
require(all(t["account"] == "Tallow-Silverfen" for t in sold), "Tallow sells them")
first_day = (anchor // 86400 - days + 1) * 86400
require(min(when(t["at"]) for t in txns) >= first_day, f"no ledger before the {days} days")
PYTHON
}

check "${work}/a1.jsonl" "${ANCHOR_A}" 60
for anchor in "${ANCHOR_B}" "${ANCHOR_C}" "${ANCHOR_D}" "${ANCHOR_E}"
do
    check "${work}/${anchor}.jsonl" "${anchor}" 60
done
check "${work}/short.jsonl" "${ANCHOR_D}" 7

# A week later is the same week: the same story at the same offsets.
diff <(sed 's/"[0-9-]*T[0-9:]*Z"/"T"/g; s/evm-[A-Za-z]*-[0-9]*-/evm-/g' "${work}/a1.jsonl") \
     <(sed 's/"[0-9-]*T[0-9:]*Z"/"T"/g; s/evm-[A-Za-z]*-[0-9]*-/evm-/g' "${work}/${ANCHOR_B}.jsonl") > /dev/null \
    || fail "a week later the body differs in more than its dates"

# ──────────────────────────────────────────────────────────────────────────
# The demo's side: the push names its organization and its namespaces
# ──────────────────────────────────────────────────────────────────────────

# Evermoor is the second organization: every feeds call there must name
# it, or it lands in (or reads from) the default one. And the source must
# share the market source's namespaces, or the positions name realms and
# items the arbitrage seed never promoted and no listing finds a product.
body="$(sed -n '/^seed_evermoor_accounts () {/,/^}/p' "${demo}")"
[[ -n "${body}" ]] || fail "the demo has no seed_evermoor_accounts"

# The patterns are the demo's own text, dollar signs and all.
# shellcheck disable=SC2016
{
    grep -qF 'feeds push "${source}" "${body}" --wait "organization_id=${org}"' <<< "${body}" \
        || fail "the demo's push does not name Evermoor's organization"
    grep -qF 'accounts post "${source}" "organization_id=${org}"' <<< "${body}" \
        || fail "the demo's books post does not name Evermoor's organization"
    grep -qF 'venue_namespace=realm instrument_namespace=item' <<< "${body}" \
        || fail "the push source does not share the market source's namespaces"
    grep -qF 'dashboard create operations "organization_id=${org}"' <<< "${body}" \
        || fail "the Operations dashboard is not filed under Evermoor"
}

printf 'ok demo-accounts: deterministic, within the contract, every purse funded\n'
