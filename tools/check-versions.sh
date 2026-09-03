#!/usr/bin/env bash
# check-versions.sh - Fail when a documented version has drifted from config.mk
#
# Copyright (C) 2026 Zach Podbielniak
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Every release bumps VERSION_* in config.mk and then edits the two places
# that quote a version back at the reader: the deployment compose file's
# pinned image tag, and the startup line in the quickstart.
#
# Those edits were being made with sed, and a sed whose pattern does not
# match changes nothing and says nothing. Both drifted four releases behind
# that way -- the compose file still pinned 0.2.0 while the binary reported
# 0.3.4 -- which is worse than a stale number: it is a deployment file that
# pulls a version nobody is testing.
#
# So the check is mechanical rather than remembered.

set -euo pipefail

cd "$(dirname "$0")/.."

major=$(sed -n 's/^VERSION_MAJOR := *//p' config.mk)
minor=$(sed -n 's/^VERSION_MINOR := *//p' config.mk)
micro=$(sed -n 's/^VERSION_MICRO := *//p' config.mk)
version="${major}.${minor}.${micro}"

status=0

check () {
    local file="$1" pattern="$2" what="$3" found

    #
    # A file that is not here is not a failure. The container build copies
    # only what compiles -- src, data, tools, tests -- so compose.deploy.yaml
    # and docs/ are legitimately absent there, and failing on that makes the
    # check unusable in the one place it runs unattended.
    #
    # A file that *is* here but has lost the line is still a failure, because
    # that is the drift this exists to catch. The two cases are reported
    # differently on purpose: "not in this checkout" and "the line moved" send
    # somebody to very different places.
    #
    if [[ ! -f "$file" ]]
    then
        echo "check-versions: $file: not in this checkout, skipped"
        return
    fi

    found=$(grep -oE "$pattern" "$file" 2>/dev/null | head -1 || true)

    if [[ -z "$found" ]]
    then
        echo "check-versions: $file: no $what found -- has the line moved?" >&2
        status=1
        return
    fi

    if [[ "$found" != *"$version"* ]]
    then
        echo "check-versions: $file: $what says '$found', config.mk says $version" >&2
        status=1
    fi
}

check compose.deploy.yaml 'zachpodbielniak/venture:[0-9]+\.[0-9]+\.[0-9]+' \
      "pinned image tag"
check docs/quickstart.org '^VENTURE [0-9]+\.[0-9]+\.[0-9]+ listening' \
      "startup line"

if [[ $status -eq 0 ]]
then
    echo "check-versions: $version everywhere it could look"
fi

exit $status
