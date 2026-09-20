#!/bin/bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Called by GTest with its own private fixture directory.
set -euo pipefail
umask 077
fixture=$1
project=$(pwd)
tool=$project/tools/venture-tenant-backup
mkdir -m 700 "$fixture/bin" "$fixture/host" "$fixture/host/studio" "$fixture/host/studio/control" "$fixture/host/studio/config" "$fixture/host/studio/state"
export RECOVERY_FIXTURE=$fixture
export PATH=$fixture/bin:$PATH
host_id=11111111-1111-4111-8111-111111111111
app_id=$(printf '%064d' 1)
db_id=$(printf '%064d' 2)
image=sha256:$(printf '%064d' 3)
printf 'a separate synthetic recovery key for tests only\n' > "$fixture/key"
printf '{"schema":1,"host_id":"%s"}\n' "$host_id" > "$fixture/host/host.json"
jq -n --arg host "$host_id" --arg app "$app_id" --arg db "$db_id" --arg image "$image" \
	'{schema:1,host_id:$host,tenant_id:"studio",workspace_id:"22222222-2222-4222-8222-222222222222",app_container:$app,db_container:$db,app_image:$image,database:"venture",db_role:"venture",postgres_major:18}' > "$fixture/host/studio/manifest.json"
jq -n --arg id "$app_id" --arg image "$image" --arg host "$host_id" --arg state "$fixture/host/studio/state" --arg config "$fixture/host/studio/config" \
	'[{Id:$id,Image:$image,Config:{Labels:{"io.venture.host":$host,"io.venture.tenant":"studio","io.venture.role":"application"}},State:{Running:false},Mounts:[{Source:$state,Destination:"/var/lib/venture",RW:true},{Source:$config,Destination:"/etc/venture",RW:false}]}]' > "$fixture/app.json"
jq -n --arg id "$db_id" --arg host "$host_id" \
	'[{Id:$id,Config:{Labels:{"io.venture.host":$host,"io.venture.tenant":"studio","io.venture.role":"database"}},State:{Running:true}}]' > "$fixture/db.json"
cat > "$fixture/bin/podman" <<'STUB'
#!/bin/bash
set -euo pipefail
case $1 in
	inspect)
		case ${2: -1} in 1) cat "$RECOVERY_FIXTURE/app.json";; 2) cat "$RECOVERY_FIXTURE/db.json";; *) exit 93;; esac ;;
	exec)
		case " $* " in
			*' psql '*)
				query=$(cat)
				case $query in
					*server_version_num*) printf '180004\n';;
					*pg_roles*) if [[ -e $RECOVERY_FIXTURE/privileged ]]; then printf '0\n'; else printf '1\n'; fi;;
					*pg_class*) if [[ -e $RECOVERY_FIXTURE/nonempty ]]; then printf '1\n'; else printf '0\n'; fi;;
					*) exit 94;;
				esac;;
			*' pg_dump '*) [[ ! -e $RECOVERY_FIXTURE/fail-dump ]] || exit 71; printf 'synthetic custom PostgreSQL dump\n';;
			*' pg_restore '*) cat > "$RECOVERY_FIXTURE/restored.dump"; [[ ! -e $RECOVERY_FIXTURE/fail-restore ]] || exit 72;;
			*) exit 95;;
		esac;;
	*) exit 96;;
esac
STUB
chmod 755 "$fixture/bin/podman"
args=(--root "$fixture/host" --tenant studio --key-file "$fixture/key")
expect_failure () {
	if "$@" > "$fixture/refusal.log" 2>&1
	then
		printf 'Expected refusal from %s\n' "$1" >&2
		exit 1
	fi
}
"$tool" --help > "$fixture/help"
"$tool" --license > "$fixture/license"
grep -q 'Restore never starts' "$fixture/help"
expect_failure "$tool" --tenant ../neighbor create "$fixture/invalid.gpg"
expect_failure "$tool" --root
printf 'x\nmisleading padding that makes this file long enough\n' > "$fixture/padded-key"
expect_failure "$tool" --root "$fixture/host" --tenant studio --key-file "$fixture/padded-key" create "$fixture/weak.gpg"
printf 'private attachment\n' > "$fixture/host/studio/state/receipt.txt"
printf 'synthetic integration identity\n' > "$fixture/host/studio/config/integration-key"
# Labels, process state and mounts are checked before dumping any business data.
cp "$fixture/app.json" "$fixture/app.good"
jq '.[0].Config.Labels["io.venture.tenant"]="neighbor"' "$fixture/app.good" > "$fixture/app.json"
expect_failure "$tool" "${args[@]}" create "$fixture/wrong-tenant.gpg"
[[ ! -e $fixture/wrong-tenant.gpg ]]
jq '.[0].State.Running=true' "$fixture/app.good" > "$fixture/app.json"
expect_failure "$tool" "${args[@]}" create "$fixture/running.gpg"
jq '.[0].Mounts[0].Source="/some/other/tenant"' "$fixture/app.good" > "$fixture/app.json"
expect_failure "$tool" "${args[@]}" create "$fixture/wrong-root.gpg"
cp "$fixture/app.good" "$fixture/app.json"
touch "$fixture/privileged"
expect_failure "$tool" "${args[@]}" create "$fixture/privileged.gpg"
rm "$fixture/privileged"
# A failed producer cannot publish an apparently successful archive.
touch "$fixture/fail-dump"
expect_failure "$tool" "${args[@]}" create "$fixture/failed.gpg"
[[ ! -e $fixture/failed.gpg ]]
rm "$fixture/fail-dump"
ln -s /etc/passwd "$fixture/host/studio/state/outside"
expect_failure "$tool" "${args[@]}" create "$fixture/symlink.gpg"
rm "$fixture/host/studio/state/outside"
ln "$fixture/host/studio/state/receipt.txt" "$fixture/host/studio/state/hardlink"
expect_failure "$tool" "${args[@]}" create "$fixture/hardlink.gpg"
rm "$fixture/host/studio/state/hardlink"
# Podman versions expose the same immutable digest with or without its prefix.
jq '.[0].Image |= sub("^sha256:"; "")' "$fixture/app.good" > "$fixture/app.json"
"$tool" "${args[@]}" create "$fixture/complete.gpg" > "$fixture/create.log"
[[ -s $fixture/complete.gpg ]]
[[ $(stat -c %a "$fixture/complete.gpg") == 600 ]]
"$tool" "${args[@]}" verify "$fixture/complete.gpg" > "$fixture/verified.json"
jq -e '.tenant_id == "studio" and .postgres_major == 18' "$fixture/verified.json" >/dev/null
# Authenticated archives still require structural validation. A credential
# holder can supply a malformed archive; extraction cannot escape staging.
mkdir -m 700 "$fixture/gnupg" "$fixture/payload" "$fixture/malicious"
gpg_args=(--homedir "$fixture/gnupg" --batch --yes --no-tty --pinentry-mode loopback --no-symkey-cache --passphrase-fd 3)
gpg "${gpg_args[@]}" --output "$fixture/outer.tar" --decrypt "$fixture/complete.gpg" 3<"$fixture/key" 2>"$fixture/gpg.log"
tar -xf "$fixture/outer.tar" -C "$fixture/payload"
printf 'outside must not be written\n' > "$fixture/malicious/escape"
for shape in traversal symlink digest
do
	case $shape in
		traversal) tar -cf "$fixture/payload/state.tar" -C "$fixture/malicious" --transform='s,^escape$,../escape,' escape ;;
		symlink) ln -s /etc/passwd "$fixture/malicious/link"; tar -cf "$fixture/payload/state.tar" -C "$fixture/malicious" link ;;
		digest) tar -cf "$fixture/payload/state.tar" -C "$fixture/malicious" escape ;;
	esac
	digest=$(sha256sum "$fixture/payload/state.tar")
	if [[ $shape != digest ]]
	then
		jq --arg digest "${digest%% *}" '.state_sha256=$digest' "$fixture/payload/manifest.json" > "$fixture/modified.json"
		mv "$fixture/modified.json" "$fixture/payload/manifest.json"
	fi
	tar -cf "$fixture/malformed.tar" -C "$fixture/payload" manifest.json database.dump state.tar config.tar
	gpg "${gpg_args[@]}" --symmetric --cipher-algo AES256 --output "$fixture/$shape.gpg" "$fixture/malformed.tar" 3<"$fixture/key" 2>"$fixture/gpg.log"
	expect_failure "$tool" "${args[@]}" verify "$fixture/$shape.gpg"
done
[[ ! -e $fixture/host/studio/control/escape ]]
# A held maintenance lock prevents concurrent snapshot/restore writers.
exec 8>"$fixture/host/studio/control/maintenance.lock"
flock -n 8
expect_failure "$tool" "${args[@]}" verify "$fixture/complete.gpg"
exec 8>&-
cp "$fixture/complete.gpg" "$fixture/tampered.gpg"
printf X | dd of="$fixture/tampered.gpg" bs=1 seek=80 conv=notrunc status=none
expect_failure "$tool" "${args[@]}" verify "$fixture/tampered.gpg"
mkdir -m 700 "$fixture/host/neighbor" "$fixture/host/neighbor/control"
jq '.tenant_id="neighbor"' "$fixture/host/studio/manifest.json" > "$fixture/host/neighbor/manifest.json"
expect_failure "$tool" --root "$fixture/host" --tenant neighbor --key-file "$fixture/key" verify "$fixture/complete.gpg"
# A reused local slug must not accept a different workspace's snapshot.
cp "$fixture/host/studio/manifest.json" "$fixture/registration.good"
jq '.workspace_id="33333333-3333-4333-8333-333333333333"' "$fixture/registration.good" > "$fixture/host/studio/manifest.json"
expect_failure "$tool" "${args[@]}" verify "$fixture/complete.gpg"
cp "$fixture/registration.good" "$fixture/host/studio/manifest.json"
original=$(sha256sum "$fixture/complete.gpg")
expect_failure "$tool" "${args[@]}" create "$fixture/complete.gpg"
[[ $(sha256sum "$fixture/complete.gpg") == "$original" ]]
# Bad authentication and existing destination data never reach pg_restore.
printf 'a different synthetic recovery key for tests\n' > "$fixture/wrong-key"
expect_failure "$tool" --root "$fixture/host" --tenant studio --key-file "$fixture/wrong-key" verify "$fixture/complete.gpg"
expect_failure "$tool" "${args[@]}" restore "$fixture/complete.gpg"
[[ ! -e $fixture/restored.dump ]]
rm "$fixture/host/studio/state/receipt.txt" "$fixture/host/studio/config/integration-key"
touch "$fixture/nonempty"
expect_failure "$tool" "${args[@]}" restore "$fixture/complete.gpg"
[[ ! -e $fixture/restored.dump ]]
rm "$fixture/nonempty"
touch "$fixture/fail-restore"
expect_failure "$tool" "${args[@]}" restore "$fixture/complete.gpg"
[[ ! -e $fixture/host/studio/state/receipt.txt && ! -e $fixture/host/studio/control/last-restore.json ]]
rm "$fixture/fail-restore"
"$tool" "${args[@]}" restore "$fixture/complete.gpg" > "$fixture/restore.log"
printf 'private attachment\n' > "$fixture/expected-attachment"
cmp "$fixture/expected-attachment" "$fixture/host/studio/state/receipt.txt"
grep -q 'synthetic integration identity' "$fixture/host/studio/config/integration-key"
grep -q 'synthetic custom PostgreSQL dump' "$fixture/restored.dump"
jq -e '.tenant_id == "studio"' "$fixture/host/studio/control/last-restore.json" >/dev/null
# A neighbor and the caller's unrelated files are never part of cleanup.
printf 'neighbor stays live\n' > "$fixture/neighbor"
expect_failure "$tool" "${args[@]}" restore "$fixture/complete.gpg"
[[ -s $fixture/neighbor ]]
[[ -z $(find "$fixture/host/studio/control" -maxdepth 1 -name 'recovery.*' -print -quit) ]]
printf 'Tenant recovery tool: ownership, stopped-state, encryption, atomic publication, destination refusal and restore fixtures passed.\n'
