#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Compile paired migration SQL into the server; never depend on its cwd.
set -euo pipefail
export LC_ALL=C
usage () {
	cat <<'HELP'
Usage: venture-migrations.sh DIRECTORY
       venture-migrations.sh --help | --license
Generate embedded migrations from paired sqlite/ and postgresql/ SQL files.
Example: tools/venture-migrations.sh migrations > build/migrations.h
Files: NNNNNN_lowercase_name.sql; versions must increase and be unique.
Scripts may contain multiple statements; transaction control is forbidden.
HELP
}
case ${1:-} in
	-h|--help) usage; exit 0 ;;
	--license) echo 'SPDX-License-Identifier: AGPL-3.0-or-later'; exit 0 ;;
esac
[[ $# -eq 1 ]] || { usage >&2; exit 1; }
root=$1
shopt -s nullglob
sqlite=("$root/sqlite/"*.sql)
postgres=("$root/postgresql/"*.sql)
[[ ${#sqlite[@]} -gt 0 && ${#sqlite[@]} -eq ${#postgres[@]} ]] || {
	echo 'Migrations require matching, nonempty backend lists' >&2; exit 1;
}
previous=0
for file in "${sqlite[@]}"; do
	name=${file##*/}
	[[ $name =~ ^([0-9]{6})_([a-z][a-z0-9_]*)\.sql$ ]] || {
		echo "Invalid migration filename: $name" >&2; exit 1;
	}
	version=$((10#${BASH_REMATCH[1]}))
	[[ $version -gt $previous && -s $root/postgresql/$name && -s $file ]] || {
		echo "Missing backend SQL, empty SQL or duplicate/out-of-order version: $name" >&2; exit 1;
	}
	previous=$version
done
printf '/* Generated migration SQL; edit migrations/, never this file. */\n'
for backend in sqlite postgresql; do
	for file in "$root/$backend/"*.sql; do
		name=${file##*/}; number=${name%%_*}; symbol=${backend}_${number}
		printf 'static const gchar venture_sql_%s[] =\n' "$symbol"
		sed 's/\\/\\\\/g; s/"/\\"/g; s/^/"/; s/$/\\n"/' "$file"
		printf ';\nstatic gboolean venture_up_%s(OrmConnection *connection, OrmDialect *dialect, GError **error)\n{\n' "$symbol"
		printf '\t(void)dialect;\n\treturn venture_migrations_execute_sql(connection, venture_sql_%s, error);\n}\n' "$symbol"
	done
	printf 'static const VentureMigrationSpec venture_migrations_%s[] = {\n' "$backend"
	for file in "$root/$backend/"*.sql; do
		name=${file##*/}; number=${name%%_*}; title=${name#*_}; title=${title%.sql}
		printf '\t{ %s, "%s", venture_sql_%s_%s, venture_up_%s_%s },\n' "$((10#$number))" "$title" "$backend" "$number" "$backend" "$number"
	done
	printf '};\n'
done
