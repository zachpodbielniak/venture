#!/usr/bin/env bash
# automation-handler.sh - An exec plugin that answers an automation handler,
# for test-automation
#
# Copyright (C) 2026 Zach Podbielniak
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# It hands its request back as a `log` line -- so the test can see the
# command and the arguments that arrived -- and then answers. Asked for
# "fallback" it writes two cursors and no `result`, which is the case where
# the handler counts messages instead of reading a count.
#
# It never parses its request: bash has no JSON parser, and escaping the
# backslashes and quotes is enough to carry the line inside a string.

set -euo pipefail

request=''
IFS= read -r request || true

escaped="${request//\\/\\\\}"
escaped="${escaped//\"/\\\"}"

printf '{"type":"log","message":"%s"}\n' "${escaped}"

if [[ "${request}" == *'"fallback"'* ]]
then
    printf '{"type":"cursor","value":"a"}\n'
    printf '{"type":"cursor","value":"b"}\n'
    exit 0
fi

printf '{"type":"result","count":2,"summary":"two of them"}\n'
