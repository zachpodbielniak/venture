#!/usr/bin/env bash
#
# venture-models.sh - generate the model catalogue from ai-glib's headers
#
# Copyright (C) 2026 Zach Podbielniak
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# ai-glib declares every model it knows as a #define in its provider
# headers -- AI_CURSOR_MODEL_CLAUDE_OPUS_5_HIGH and a hundred others. That
# is the authoritative list, and it moves whenever the submodule does.
#
# So the catalogue is not written by hand. This reads those headers and
# emits a C table, the same way rules.mk turns the stylesheet into a string
# constant. A model added to ai-glib appears in VENTURE's dropdown on the
# next build, and one removed from it disappears, with nobody remembering
# to copy anything.

# The usage messages name the function in backticks, which is this
# codebase's convention for them rather than an attempt at substitution.
# The directive has to precede the first command to apply to the file.
# shellcheck disable=SC2016

set -euo pipefail

VERSION="0.6.0"

usage () {
    cat <<'USAGE'
venture-models.sh - generate the model catalogue from ai-glib's headers

Usage:
  venture-models.sh <provider-header-dir>
  venture-models.sh --help
  venture-models.sh --license

Reads every ai-*-client.h in the directory, collects the model constants
each declares, and writes a C header to stdout.

Arguments:
  <provider-header-dir>   usually deps/ai-glib/src/providers

Options:
  -h, --help      This message
  -V, --version   Print the version
      --license   Licence and copyright

Examples:
  venture-models.sh deps/ai-glib/src/providers > build/venture-models.h
  venture-models.sh deps/ai-glib/src/providers | grep claude-code
USAGE
}

licence () {
    cat <<'LICENCE'
venture-models.sh, part of VENTURE.
Copyright (C) 2026 Zach Podbielniak

This program is free software: you can redistribute it and/or modify it
under the terms of the GNU Affero General Public License as published by
the Free Software Foundation, either version 3 of the License, or (at your
option) any later version.
LICENCE
}

# The constant prefix each provider's models are declared under, paired
# with the name ai_provider_type_to_string() gives that provider.
#
# Longest prefix first: AI_CLAUDE_CODE_MODEL_ also matches a naive test
# for AI_CLAUDE_MODEL_, and claude-code's aliases would otherwise be filed
# under the HTTP provider.
readonly PREFIXES=(
    "AI_CLAUDE_CODE_MODEL_:claude-code"
    "AI_CLAUDE_TMUX_MODEL_:claude-tmux"
    "AI_CLAUDE_MODEL_:claude"
    "AI_CODEX_CLI_MODEL_:codex-cli"
    "AI_CURSOR_MODEL_:cursor"
    "AI_ANTIGRAVITY_MODEL_:antigravity"
    "AI_GROK_BUILD_MODEL_:grok-build"
    "AI_GROK_MODEL_:grok"
    "AI_OPENAI_MODEL_:openai"
    "AI_GEMINI_MODEL_:gemini"
    "AI_OLLAMA_MODEL_:ollama"
    "AI_OPENCODE_MODEL_:opencode"
)

# And the default each provider falls back to, which is a separate define.
readonly DEFAULTS=(
    "AI_CLAUDE_CODE_DEFAULT_MODEL:claude-code"
    "AI_CLAUDE_TMUX_DEFAULT_MODEL:claude-tmux"
    "AI_CLAUDE_DEFAULT_MODEL:claude"
    "AI_CODEX_CLI_DEFAULT_MODEL:codex-cli"
    "AI_CURSOR_DEFAULT_MODEL:cursor"
    "AI_ANTIGRAVITY_DEFAULT_MODEL:antigravity"
    "AI_GROK_BUILD_DEFAULT_MODEL:grok-build"
    "AI_GROK_DEFAULT_MODEL:grok"
    "AI_OPENAI_DEFAULT_MODEL:openai"
    "AI_GEMINI_DEFAULT_MODEL:gemini"
    "AI_OLLAMA_DEFAULT_MODEL:ollama"
    "AI_OPENCODE_DEFAULT_MODEL:opencode"
)

# Every #define of a model constant, as "PREFIXED_NAME value".
collect_defines () {
    if [[ $# -ne 1 ]]
    then
        echo '`collect_defines()` requires 1 positional argument' >&2
        echo 'collect_defines <dir>' >&2
        exit 1
    fi

    local dir="${1}"

    # Only the string-valued ones. A define whose body is another macro is
    # an alias and would emit the macro's name as a model id.
    grep -rhoE '^#define[[:space:]]+AI_[A-Z0-9_]*MODEL[A-Z0-9_]*[[:space:]]+"[^"]+"' \
        "${dir}"/*.h 2>/dev/null |
        sed -E 's/^#define[[:space:]]+([A-Z0-9_]+)[[:space:]]+"([^"]+)"$/\1 \2/' |
        sort -u
}

# Which provider a constant belongs to, or nothing.
provider_for () {
    if [[ $# -ne 1 ]]
    then
        echo '`provider_for()` requires 1 positional argument' >&2
        echo 'provider_for <constant>' >&2
        exit 1
    fi

    local name="${1}"
    local entry

    for entry in "${PREFIXES[@]}"
    do
        if [[ "${name}" == "${entry%%:*}"* ]]
        then
            printf '%s' "${entry#*:}"
            return 0
        fi
    done

    return 0
}

default_for () {
    if [[ $# -ne 2 ]]
    then
        echo '`default_for()` requires 2 positional arguments' >&2
        echo 'default_for <provider> <defines>' >&2
        exit 1
    fi

    local provider="${1}"
    local defines="${2}"
    local entry

    for entry in "${DEFAULTS[@]}"
    do
        if [[ "${entry#*:}" == "${provider}" ]]
        then
            awk -v want="${entry%%:*}" '$1 == want { print $2; exit }' \
                <<< "${defines}"
            return 0
        fi
    done

    return 0
}

generate () {
    if [[ $# -ne 1 ]]
    then
        echo '`generate()` requires 1 positional argument' >&2
        echo 'generate <dir>' >&2
        exit 1
    fi

    local dir="${1}"
    local defines
    local provider
    local seen=""

    defines="$(collect_defines "${dir}")"

    if [[ -z "${defines}" ]]
    then
        echo "venture-models.sh: no model constants found in ${dir}" >&2
        exit 1
    fi

    echo "/* Generated by tools/venture-models.sh - do not edit. */"
    echo

    # One array per provider, in the order the prefix table lists them.
    local entry
    for entry in "${PREFIXES[@]}"
    do
        provider="${entry#*:}"

        # A provider may appear twice in the table only by mistake, but
        # emitting its array twice would not compile, so guard anyway.
        if [[ " ${seen} " == *" ${provider} "* ]]
        then
            continue
        fi

        seen="${seen} ${provider}"

        local ids
        ids="$(
            while read -r name value
            do
                [[ -z "${name}" ]] && continue

                if [[ "$(provider_for "${name}")" == "${provider}" ]]
                then
                    printf '%s\n' "${value}"
                fi
            done <<< "${defines}" | sort -u
        )"

        printf 'static const gchar *const venture_models_%s[] = {\n' \
            "${provider//-/_}"

        if [[ -n "${ids}" ]]
        then
            while read -r id
            do
                [[ -z "${id}" ]] && continue
                printf '\t"%s",\n' "${id}"
            done <<< "${ids}"
        fi

        printf '\tNULL\n};\n\n'
    done

    # And the index that ties a provider name to its array and default.
    echo "static const VentureModelSet venture_model_sets[] = {"

    seen=""
    for entry in "${PREFIXES[@]}"
    do
        provider="${entry#*:}"

        if [[ " ${seen} " == *" ${provider} "* ]]
        then
            continue
        fi

        seen="${seen} ${provider}"

        local fallback
        fallback="$(default_for "${provider}" "${defines}")"

        printf '\t{ "%s", venture_models_%s, "%s" },\n' \
            "${provider}" "${provider//-/_}" "${fallback}"
    done

    echo "	{ NULL, NULL, NULL }"
    echo "};"
}

main () {
    local dir=""

    while [[ $# -gt 0 ]]
    do
        case "${1}" in
            -h|--help)
                usage
                exit 0
                ;;
            -V|--version)
                echo "venture-models.sh ${VERSION}"
                exit 0
                ;;
            --license)
                licence
                exit 0
                ;;
            -*)
                echo "venture-models.sh: unknown option ${1}" >&2
                usage >&2
                exit 1
                ;;
            *)
                dir="${1}"
                shift
                ;;
        esac

        if [[ $# -gt 0 && "${1}" == -* ]]
        then
            continue
        fi

        [[ $# -gt 0 && -n "${dir}" ]] && shift
    done

    if [[ -z "${dir}" ]]
    then
        echo 'venture-models.sh: a provider header directory is required' >&2
        usage >&2
        exit 1
    fi

    if [[ ! -d "${dir}" ]]
    then
        echo "venture-models.sh: no directory at ${dir}" >&2
        exit 1
    fi

    generate "${dir}"
}

main "$@"
