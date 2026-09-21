#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Run the real authorization-code/PKCE fixture in an owned disposable realm.
set -euo pipefail

usage () {
    cat <<'HELP'
Usage: tools/venture-test-oidc-keycloak.sh

Options: -h, --help; --license

Builds the DEBUG OIDC test, starts a disposable Keycloak container on a random
loopback port, runs explicit linking and subsequent SSO, then removes only that
container. Requires podman, curl and the ordinary VENTURE build dependencies.
VENTURE_TEST_OIDC_IMAGE overrides the default quay.io/keycloak/keycloak:26.7.3.
All users and credentials belong to the synthetic imported fixture.

Example: tools/venture-test-oidc-keycloak.sh
HELP
}
case "${1:-}" in
    -h|--help) usage; exit 0 ;;
    --license) printf '%s\n' 'AGPL-3.0-or-later: https://www.gnu.org/licenses/agpl-3.0.html'; exit 0 ;;
esac
if [[ $# != 0 ]]; then
    printf 'Unexpected argument; use --help.\n' >&2
    exit 2
fi
repo=$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd -- "$repo"
for command in podman curl make; do
    command -v "$command" >/dev/null || { printf 'Required command: %s\n' "$command" >&2; exit 1; }
done
make DEBUG=1 build/debug/tests/test-oidc
fixture_dir=$(mktemp -d "${TMPDIR:-/tmp}/venture-oidc-keycloak.XXXXXXXX")
cleanup() {
    local status=$?
    if [[ -s $fixture_dir/cid ]]; then
        podman rm -f "$(<"$fixture_dir/cid")" >/dev/null || status=1
    fi
    rm -rf -- "$fixture_dir"
    exit "$status"
}
trap cleanup EXIT
cp -- tests/fixtures/oidc/keycloak-realm.json "$fixture_dir/realm.json"
image=${VENTURE_TEST_OIDC_IMAGE:-quay.io/keycloak/keycloak:26.7.3}
podman run -d --cidfile "$fixture_dir/cid" --publish 127.0.0.1::8080 \
    --volume "$fixture_dir/realm.json:/opt/keycloak/data/import/venture.json:ro,Z" \
    "$image" start-dev --import-realm --http-enabled=true --hostname-strict=false >/dev/null
container=$(<"$fixture_dir/cid")
address=$(podman port "$container" 8080/tcp)
if [[ ! $address =~ ^127\.0\.0\.1:([0-9]+)$ ]]; then
    printf 'Fixture did not receive one loopback port.\n' >&2
    exit 1
fi
issuer="http://$address/realms/venture-125-fixture"
printf 'Waiting for disposable Keycloak at %s\n' "$issuer"
ready=false
for ((attempt = 0; attempt < 120; attempt++)); do
    if curl --fail --silent --max-time 2 "$issuer/.well-known/openid-configuration" >"$fixture_dir/discovery.json"; then
        ready=true
        break
    fi
    sleep 1
done
if [[ $ready != true ]]; then
    podman logs "$container" >&2
    printf 'Disposable provider did not become ready.\n' >&2
    exit 1
fi
VENTURE_TEST_OIDC_KEYCLOAK_ISSUER="$issuer" build/debug/tests/test-oidc -p /oidc/real-keycloak
VENTURE_TEST_OIDC_KEYCLOAK_ISSUER="$issuer" build/debug/tests/test-oidc -p /oidc/browser-keycloak
