#!/usr/bin/env bash
# feed-provider.sh - An exec plugin that is a market data source provider,
# for test-feeds
#
# Copyright (C) 2026 Zach Podbielniak
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# It reads its one-line request, finds the unit it was asked for, and
# answers in the JSON-lines protocol of docs/plugins.org: a venue named for
# the unit, two instruments, a complete snapshot and three listings with
# ids and the time each has left. A unit called "broken" writes one listing
# and then an error, which is a partial batch.
#
# It never parses JSON: bash has none, and a pattern finds the unit.

set -euo pipefail

request=''
IFS= read -r request || true

unit='default'

if [[ "${request}" =~ \"unit\":\"([A-Za-z0-9_-]+)\" ]]
then
    unit="${BASH_REMATCH[1]}"
fi

printf '{"type":"venue","key":"%s","name":"Realm %s","currency":"GOLD","group":"eu"}\n' "${unit}" "${unit}"
printf '{"type":"instrument","key":"ore","name":"Iron ore","category":"Materials/Ore"}\n'
printf '{"type":"instrument","key":"herb","name":"Peacebloom","category":"Materials/Herb"}\n'
printf '{"type":"snapshot","venue":"%s","taken_at":"2026-10-03T12:00:00Z","complete":true}\n' "${unit}"
printf '{"type":"listing","venue":"%s","instrument":"ore","price":"1.25","quantity":20,"id":"a1","expires_in_min":7200}\n' "${unit}"

if [[ "${unit}" == 'broken' ]]
then
    printf '{"type":"error","message":"the far end stopped answering","retry_after":30}\n'
    exit 0
fi

printf '{"type":"listing","venue":"%s","instrument":"ore","price":"1.50","quantity":5,"id":"a2","expires_in_min":1800}\n' "${unit}"
printf '{"type":"listing","venue":"%s","instrument":"herb","price":"0.75","quantity":40,"id":"b1"}\n' "${unit}"
printf '{"type":"cursor","value":"page-2"}\n'
