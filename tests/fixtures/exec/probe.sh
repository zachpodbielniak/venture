#!/usr/bin/env bash
# probe.sh - An exec plugin that misbehaves on request, for test-plugin-runtime
#
# Copyright (C) 2026 Zach Podbielniak
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# The first argument picks what it does. Each mode is one way a real plugin
# can go wrong -- or right -- and the test copies this file into a scratch
# plugin directory, writes a manifest naming the mode, and checks VENTURE's
# side of the bargain: the deadline, the caps, the environment, the
# redaction, the protocol.
#
# It never parses its request: bash has no JSON parser, and the tests only
# need to see what arrived, so `echo` hands the line back verbatim inside a
# record.

set -uo pipefail

# Read the one request line. A mode that ignores stdin skips this, which is
# itself a case worth testing.
read_request () {
    local line=''

    IFS= read -r line || true
    printf '%s' "${line}"
}

mode="${1:-echo}"

case "${mode}" in
    echo)
        request="$(read_request)"
        printf '{"type":"log","level":"info","message":"hello from probe"}\n'
        printf '{"type":"record","record_type":"request","fields":%s}\n' \
            "${request}"
        printf '{"type":"record","record_type":"env","fields":{"leak":"%s","declared":"%s","has_path":"%s","has_home":"%s","argc":"%s","args":"%s","cwd":"%s"}}\n' \
            "${VENTURE_TEST_LEAK:-}" "${VENTURE_TEST_DECLARED:-}" \
            "${PATH:+yes}" "${HOME:+yes}" "$#" "$*" "${PWD}"
        printf '{"type":"cursor","value":"c-1"}\n'
        # The request again, on stderr: if a secret were in it and not
        # redacted, the test would see it there.
        printf 'request was %s\n' "${request}" >&2
        ;;
    sleep)
        # A child of the script, not the script: the deadline has to stop
        # the whole process group, or this sleep outlives the run holding
        # stdout open. Its pid is left in the plugin directory so the test
        # can check it is gone.
        sleep 30 &
        printf '%s' "$!" > sleep.pid
        wait
        printf '{"type":"log","message":"woke up"}\n'
        ;;
    flood)
        read_request > /dev/null
        for _ in $(seq 1 5000)
        do
            printf '{"type":"log","message":"flooding the output past its cap"}\n'
        done
        ;;
    stderr-flood)
        read_request > /dev/null
        for _ in $(seq 1 5000)
        do
            printf 'chatter on standard error, line after line\n' >&2
        done
        printf '{"type":"cursor","value":"done"}\n'
        ;;
    fail)
        read_request > /dev/null
        printf '{"type":"log","message":"about to fail"}\n'
        printf 'something broke upstream\n' >&2
        exit 3
        ;;
    error)
        read_request > /dev/null
        printf '{"type":"stat","venue":"v","instrument":"i","min":"1.00"}\n'
        printf '{"type":"error","message":"upstream said no","retry_after":60}\n'
        ;;
    garbage)
        read_request > /dev/null
        printf '{"type":"log","message":"fine so far"}\n'
        printf 'this is not json\n'
        ;;
    no-read)
        # Exits without reading its request. The server's write then fails;
        # it must fail as an error code, not as a SIGPIPE that kills it.
        printf '{"type":"log","message":"did not read"}\n'
        ;;
    *)
        printf 'unknown mode %s\n' "${mode}" >&2
        exit 2
        ;;
esac
