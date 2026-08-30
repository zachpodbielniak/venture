#!/usr/bin/env bash
# venture-test-litter.sh - Fail a green test run that left temp dirs behind.
#
# Copyright (C) 2026 Zach Podbielniak
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# g_rmdir() does nothing at all to a directory that is not empty, and
# g_file_delete() on one fails the same way with an error nobody reads.  So a
# fixture that wrote a file inside the directory it made leaves that directory
# behind -- silently, once per test per run.  Five fixtures here did: a full
# green suite left seventeen, and "All 14 test binaries passed" said nothing
# about it.
#
# This adds the missing half of that sentence: *and the machine is as it was.*
#
# It is a before/after comparison of the same run rather than a check that the
# temporary directory is empty, because a previous *failing* run leaves its
# directories on purpose -- they are the evidence -- and blaming this run for
# them would train everybody to ignore the check.
#
# `make test` gives each run a private TMPDIR and g_get_tmp_dir() honours it,
# which is what makes the comparison exact: two worktrees building at once
# cannot see each other's fixtures.  Sharing /tmp would make every parallel
# build a false failure.
#
# Usage: venture-test-litter.sh snapshot <file>
#        venture-test-litter.sh check <file>
#
# What it cannot see, stated rather than implied: a directory whose name is not
# a g_dir_make_tmp() string literal in tests/test-*.c.  tests/test-config.c
# built one from g_get_tmp_dir() and a fixed name and was invisible here for
# exactly that reason; it now uses g_dir_make_tmp() like everything else, but
# the next one written that way would be invisible too.
#
# The alternative -- a list of prefixes written down here -- would drift from
# the suite instead, which is worse.  This at least grows with it, and the
# bare `rmdir` of the private TMPDIR in `make test` is a second net: a run that
# left anything under a name this cannot match still leaves its TMPDIR behind
# where somebody will find it.

set -euo pipefail

# Where GLib will put them.  g_get_tmp_dir() reads TMPDIR, then TMP, then
# TEMP, and falls back to /tmp; looking only at /tmp would make this report
# OK while the suite littered somewhere else, which is the failure it exists
# for wearing a different hat.
tmp_dir () {
    if [[ -n "${TMPDIR:-}" ]]
    then
        echo "${TMPDIR}"
    elif [[ -n "${TMP:-}" ]]
    then
        echo "${TMP}"
    elif [[ -n "${TEMP:-}" ]]
    then
        echo "${TEMP}"
    else
        echo '/tmp'
    fi
}

# Every prefix a fixture asks g_dir_make_tmp() for, read from the fixtures
# themselves.  The suite grows; a list written down here would not.
list_dirs () {
    local root
    local prefix
    local path
    local -a prefixes

    root="$(tmp_dir)"

    mapfile -t prefixes < <(sed -n \
        's/.*g_dir_make_tmp *( *"\([^"]*\)XXXXXX".*/\1/p' \
        tests/test-*.c | sort -u)

    if [[ ${#prefixes[@]} -eq 0 ]]
    then
        return 0
    fi

    for prefix in "${prefixes[@]}"
    do
        # A prefix matching nothing leaves the glob unexpanded, which is why
        # each candidate is tested for being a directory rather than printed.
        for path in "${root}/${prefix}"*
        do
            if [[ -d "${path}" ]]
            then
                echo "${path}"
            fi
        done
    done

    return 0
}

main () {
    if [[ $# -ne 2 ]]
    then
        echo 'venture-test-litter.sh requires 2 positional arguments'
        echo 'venture-test-litter.sh <snapshot|check> <file>'
        exit 1
    fi

    local mode="${1}"
    local file="${2}"
    local after
    local new

    case "${mode}" in
        snapshot)
            list_dirs | sort -u > "${file}"
            exit 0
            ;;
        check)
            ;;
        *)
            echo "test-litter: unknown mode '${mode}'"
            exit 1
            ;;
    esac

    if [[ ! -f "${file}" ]]
    then
        echo "test-litter: no snapshot at ${file}"
        echo 'test-litter: refusing to call this run clean'
        exit 1
    fi

    after="${file}.after"
    new="${file}.new"

    list_dirs | sort -u > "${after}"

    # comm rather than piping into grep: under `set -o pipefail` a
    # `| grep -q` that matches early exits 141 on SIGPIPE, which reads as the
    # check itself having failed.
    comm -13 "${file}" "${after}" > "${new}"

    if [[ -s "${new}" ]]
    then
        echo 'test-litter: the suite passed but left temporary directories behind:'
        sed 's/^/  /' "${new}"
        echo 'test-litter: a fixture removed its file and not the directory holding it'
        echo 'test-litter: use venture_test_remove_tree(), not g_rmdir() or g_file_delete()'
        rm -f "${after}" "${new}"
        exit 1
    fi

    rm -f "${file}" "${after}" "${new}"
    exit 0
}

main "$@"
