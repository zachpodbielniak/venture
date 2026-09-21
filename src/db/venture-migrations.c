/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include "db/venture-migrations.h"

typedef struct
{
	gint64 version;
	const gchar *name;
	const gchar *sql;
	OrmMigrationFunc apply;
} VentureMigrationSpec;

#include "venture-migration-sql.h"

gboolean
venture_migrations_execute_sql(
	OrmConnection *connection,
	const gchar *sql,
	GError **error
){
	/* Optional module data patches must not manufacture tables outside the
	 * entity registry. The directive remains part of the checksummed batch. */
	if (g_str_has_prefix(sql, "-- requires-table"))
	{
		static const gchar prefix[] = "-- requires-table: ";
		g_autofree gchar *table = NULL;
		g_autoptr(OrmInspector) inspector = NULL;
		g_autoptr(GError) local_error = NULL;
		const gchar *end;
		gsize i;
		gboolean exists;
		if (!g_str_has_prefix(sql, prefix) || !(end = strchr(sql, '\n')))
			goto malformed;
		table = g_strndup(sql + strlen(prefix), end - sql - strlen(prefix));
		if (!*table || strlen(table) > 63 || !(g_ascii_islower(table[0]) || table[0] == '_'))
			goto malformed;
		for (i = 1; table[i]; i++)
			if (!g_ascii_islower(table[i]) && !g_ascii_isdigit(table[i]) && table[i] != '_')
				goto malformed;
		inspector = orm_inspector_new(connection, error);
		if (!inspector) return FALSE;
		exists = orm_inspector_has_table(inspector, table, NULL, &local_error);
		if (local_error)
		{
			g_propagate_error(error, g_steal_pointer(&local_error));
			return FALSE;
		}
		if (!exists) return TRUE;
	}
	/* Both supported drivers execute parameter-free SQL batches natively.
	 * Preserve quoting, comments and trigger bodies; orm-glib owns the
	 * enclosing transaction and records history only after this succeeds. */
	return orm_connection_execute(connection, sql, error);
malformed:
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		"Migration requires-table directive must name one unquoted lowercase table on its first line");
	return FALSE;
}

OrmMigrator *
venture_migrations_new(
	OrmConnection *connection,
	VentureDatabaseBackend backend,
	GError **error
){
	g_autoptr(GPtrArray) migrations = NULL;
	const VentureMigrationSpec *specs;
	guint count;
	guint i;

	if (backend == VENTURE_DATABASE_BACKEND_SQLITE)
	{
		specs = venture_migrations_sqlite;
		count = G_N_ELEMENTS(venture_migrations_sqlite);
	}
	else if (backend == VENTURE_DATABASE_BACKEND_POSTGRES)
	{
		specs = venture_migrations_postgresql;
		count = G_N_ELEMENTS(venture_migrations_postgresql);
	}
	else
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_UNSUPPORTED,
			"No SQL migrations are shipped for this database backend");
		return NULL;
	}
	migrations = g_ptr_array_new_with_free_func((GDestroyNotify)orm_migration_free);
	for (i = 0; i < count; i++)
		g_ptr_array_add(migrations, orm_migration_new_callback(specs[i].version,
			specs[i].name, specs[i].sql, specs[i].apply, NULL));
	return orm_migrator_new(connection, (OrmMigration *const *)migrations->pdata,
		migrations->len, error);
}
