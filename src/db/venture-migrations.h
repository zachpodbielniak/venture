/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_MIGRATIONS_H
#define VENTURE_MIGRATIONS_H

/* Private to startup and its regression suite. */
/**
 * venture_migrations_execute_sql: (skip)
 * @connection: exclusively held migration connection
 * @sql: the complete embedded SQL batch
 * @error: (out) (optional): failure
 *
 * A strict first-line -- requires-table: name directive skips the batch when
 * that optional module table is absent. The directive remains checksummed.
 * Returns: TRUE if all statements succeeded or the declared table is absent, without committing
 */
gboolean venture_migrations_execute_sql(OrmConnection *connection,
	const gchar *sql, GError **error);
/**
 * venture_migrations_new: (skip)
 * @connection: idle connection
 * @backend: the configured database backend
 * @error: (out) (optional): failure
 *
 * Returns: (transfer full) (nullable): runner for the complete embedded history
 */
OrmMigrator *venture_migrations_new(OrmConnection *connection,
	VentureDatabaseBackend backend, GError **error);
#endif
