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
	/* Both supported drivers execute parameter-free SQL batches natively.
	 * Preserve quoting, comments and trigger bodies; orm-glib owns the
	 * enclosing transaction and records history only after this succeeds. */
	return orm_connection_execute(connection, sql, error);
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
