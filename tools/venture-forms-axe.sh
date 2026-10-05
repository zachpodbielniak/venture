#!/usr/bin/env bash
#
# venture-forms-axe.sh - Run axe-core over the forms module's renderings
#
# Copyright (C) 2026 Zach Podbielniak
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# The suite checks the embed contract's accessibility rules itself
# (/forms/a11y-contract, tests/test-forms.c) on every build. This runs a
# second, independent checker -- axe-core, in jsdom -- over the same
# renderings: every field kind, blank, refused and thanked, bare and as a
# whole page. It is a documented tool rather than part of `make test`
# because it needs node and two npm packages, which the build does not.
#
#   make DEBUG=1 test-one T=test-forms          # builds the test
#   tools/venture-forms-axe.sh                  # renders, then checks
#   tools/venture-forms-axe.sh --keep DIR       # keep the renderings in DIR
#
# Colour contrast is not checked: jsdom has no layout, and the colours on a
# host page are the host's. Exit status is 0 when axe reports no violation,
# 1 when it reports any, 2 on a usage or setup error.

set -euo pipefail

readonly VERSION="1.0.0"

usage() {
	cat <<'USAGE'
Usage: tools/venture-forms-axe.sh [--keep DIR] [--test BINARY]

Renders every form field kind in every state through the test suite and
runs axe-core over the result.

Options:
  --keep DIR     write the renderings to DIR and leave them there
  --test BINARY  the test-forms binary (default build/debug/tests/test-forms)
  -h, --help     show this help
  --version      show the version

Environment:
  VENTURE_AXE_CACHE  where axe-core and jsdom are installed
                     (default ${XDG_CACHE_HOME:-~/.cache}/venture-axe)

Example:
  make DEBUG=1 test-one T=test-forms && tools/venture-forms-axe.sh
USAGE
}

keep=""
binary="build/debug/tests/test-forms"

while (($# > 0)); do
	case "$1" in
	--keep)
		[[ $# -ge 2 ]] || { usage >&2; exit 2; }
		keep="$2"
		shift 2
		;;
	--test)
		[[ $# -ge 2 ]] || { usage >&2; exit 2; }
		binary="$2"
		shift 2
		;;
	-h | --help)
		usage
		exit 0
		;;
	--version)
		printf 'venture-forms-axe %s\n' "$VERSION"
		exit 0
		;;
	*)
		usage >&2
		exit 2
		;;
	esac
done

command -v node >/dev/null 2>&1 || { echo "node is required" >&2; exit 2; }
command -v npm >/dev/null 2>&1 || { echo "npm is required" >&2; exit 2; }
[[ -x "$binary" ]] || { echo "no test binary at $binary; build it first" >&2; exit 2; }

cache="${VENTURE_AXE_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/venture-axe}"
if [[ ! -d "$cache/node_modules/axe-core" || ! -d "$cache/node_modules/jsdom" ]]; then
	echo "==> installing axe-core and jsdom into $cache" >&2
	mkdir -p "$cache"
	(CDPATH='' cd -- "$cache" && npm init -y >/dev/null && npm install --silent axe-core@4 jsdom@24 >&2)
fi

if [[ -n "$keep" ]]; then
	mkdir -p "$keep"
	out="$keep"
else
	out="$(mktemp -d "${TMPDIR:-/tmp}/venture-forms-axe-XXXXXX")"
	trap 'rm -rf -- "$out"' EXIT
fi

echo "==> rendering into $out" >&2
VENTURE_FORMS_A11Y_DIR="$out" "$binary" -p /forms/a11y-contract >/dev/null

NODE_PATH="$cache/node_modules" node - "$out" <<'NODE'
const fs = require("fs");
const path = require("path");
const { JSDOM } = require("jsdom");
const axeSource = fs.readFileSync(require.resolve("axe-core/axe.min.js"), "utf8");

(async () => {
	const dir = process.argv[2];
	let failed = 0;
	for (const name of fs.readdirSync(dir).filter((f) => f.endsWith(".html")).sort()) {
		const dom = new JSDOM(fs.readFileSync(path.join(dir, name), "utf8"), { runScripts: "outside-only" });
		dom.window.eval(axeSource);
		const result = await dom.window.axe.run(dom.window.document, {
			rules: { "color-contrast": { enabled: false } },
		});
		const count = result.violations.length;
		console.log(`${count === 0 ? "ok" : "FAIL"} ${name}: ${count} violation(s), ${result.passes.length} rule(s) passed`);
		for (const violation of result.violations) {
			failed++;
			console.log(`  ${violation.id}: ${violation.help}`);
			for (const node of violation.nodes) console.log(`    ${node.target.join(" ")}`);
		}
	}
	process.exit(failed === 0 ? 0 : 1);
})();
NODE
