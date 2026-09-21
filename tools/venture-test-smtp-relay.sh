#!/bin/bash
# SPDX-License-Identifier: AGPL-3.0-or-later
set -euo pipefail

usage () {
	cat <<'HELP'
Usage: venture-test-smtp-relay.sh [TEST-BINARY]
Run the organization SMTP isolation fixture against a disposable TLS relay.

Options: -h, --help; --license
Default binary: build/debug/tests/test-mail-routing in this checkout.
Example: make DEBUG=1 test-mail-relay

Requires: Bash 5, Podman (or VENTURE_TEST_CONTAINER_ENGINE), OpenSSL, curl,
coreutils. The pinned Mailpit image is pulled if missing.
Publishes TAP on stdout. Returns the test status; timeout is 124.
The relay always stops. Successful fixture files are removed; failure
files remain at the printed path for diagnosis. Uses synthetic credentials.
HELP
}
case "${1:-}" in
	-h|--help) usage; exit 0 ;;
	--license) printf '%s\n' 'AGPL-3.0-or-later: https://www.gnu.org/licenses/agpl-3.0.html'; exit 0 ;;
	-*) printf 'Unknown option: %s\n' "$1" >&2; exit 2 ;;
esac
if (( $# > 1 ))
then
	usage >&2
	exit 2
fi
relay_engine="${VENTURE_TEST_CONTAINER_ENGINE:-podman}"
for tool in "$relay_engine" openssl curl mktemp timeout realpath unlink rmdir
do
	if ! command -v "$tool" >/dev/null 2>&1
	then
		printf 'Missing test dependency: %s\n' "$tool" >&2
		exit 1
	fi
done
repo_dir=$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
relay_binary=$(realpath -- "${1:-$repo_dir/build/debug/tests/test-mail-routing}")
if [[ ! -x "$relay_binary" ]]
then
	printf 'Build the DEBUG test binary first: %s\n' "$relay_binary" >&2
	exit 1
fi
read -r relay_uuid < /proc/sys/kernel/random/uuid
relay_name="venture-smtp-${relay_uuid}"
relay_dir=''
cleanup () {
	local status="$1" exists_status=0 fixture_file
	trap - EXIT
	timeout 10 "$relay_engine" container exists "$relay_name" || exists_status=$?
	if (( exists_status == 0 ))
	then
		if ! timeout 15 "$relay_engine" stop --time 2 "$relay_name" >/dev/null
		then
			printf 'Could not stop test relay %s\n' "$relay_name" >&2
			if (( status == 0 )); then status=1; fi
		fi
	elif (( exists_status != 1 ))
	then
		printf 'Could not inspect test relay cleanup: %s\n' "$relay_name" >&2
		if (( status == 0 )); then status=1; fi
	fi
	if [[ -n "$relay_dir" && -d "$relay_dir" ]]
	then
		if (( status == 0 ))
		then
			# The mount is read-only and this script creates exactly these
			# three files. Refuse unexpected contents instead of broad deletion.
			for fixture_file in key.pem cert.pem openssl.log
			do
				if ! unlink -- "$relay_dir/$fixture_file"
				then
					printf 'Could not remove owned fixture file: %s\n' "$fixture_file" >&2
					status=1
				fi
			done
			if ! rmdir -- "$relay_dir"
			then
				printf 'Could not remove fixture directory: %s\n' "$relay_dir" >&2
				status=1
			fi
		else
			printf 'SMTP failure evidence retained: %s\n' "$relay_dir" >&2
		fi
	fi
	exit "$status"
}
trap 'cleanup "$?"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
umask 077
relay_dir=$(mktemp -d "${TMPDIR:-/tmp}/venture-smtp-relay.XXXXXX")
openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days 1 \
	-subj /CN=localhost -addext 'subjectAltName=DNS:localhost,IP:127.0.0.1' \
	-keyout "$relay_dir/key.pem" -out "$relay_dir/cert.pem" >"$relay_dir/openssl.log" 2>&1
timeout 120 "$relay_engine" run --rm -d --name "$relay_name" --user 0 \
	-p 127.0.0.1::1025 -p 127.0.0.1::8025 \
	-v "$relay_dir:/fixture:ro,Z" \
	-e 'MP_SMTP_AUTH=org-a:alpha-test org-b:bravo-test' \
	docker.io/axllent/mailpit:v1.29.2 \
	--smtp-tls-cert /fixture/cert.pem --smtp-tls-key /fixture/key.pem \
	--smtp-require-starttls --smtp-strict-rfc-headers >/dev/null
smtp_address=$("$relay_engine" port "$relay_name" 1025/tcp)
api_address=$("$relay_engine" port "$relay_name" 8025/tcp)
export VENTURE_TEST_SMTP_PORT="${smtp_address##*:}"
export VENTURE_TEST_SMTP_CA_FILE="$relay_dir/cert.pem"
export VENTURE_TEST_SMTP_API="http://$api_address"
relay_ready=false
for (( attempt=0; attempt<60; attempt++ ))
do
	if curl --fail --silent --max-time 1 "$VENTURE_TEST_SMTP_API/api/v1/messages" >/dev/null
	then
		relay_ready=true
		break
	fi
	sleep 0.1
done
if [[ "$relay_ready" != true ]]
then
	printf 'Test relay failed to become ready\n' >&2
	exit 1
fi
cd -- "$repo_dir"
timeout 180 "$relay_binary" -p /mail-routing/real-tls-isolation
