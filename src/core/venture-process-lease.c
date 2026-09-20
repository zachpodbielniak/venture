/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include <glib/gstdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>

/* Advisory locks are already database-scoped in PostgreSQL. A fixed namespace
 * therefore excludes another server for this database without cross-tenant
 * contention on unrelated databases in the same cluster. */
#define LEASE_TRY_SQL "SELECT CASE WHEN pg_try_advisory_lock(1447382612, 1) THEN 1 ELSE 0 END"
#define LEASE_RELEASE_SQL "SELECT pg_advisory_unlock(1447382612, 1)"
#define LEASE_DATA "venture-process-lease"
struct _VentureProcessLease
{
	GObject parent_instance;
	VentureDatabase *database;
	gint state_fd;
	gint database_fd;
	gboolean postgres_locked;
};
G_DEFINE_FINAL_TYPE(VentureProcessLease, venture_process_lease, G_TYPE_OBJECT)

static void
venture_process_lease_finalize(GObject *object)
{
	VentureProcessLease *self = VENTURE_PROCESS_LEASE(object);
	if (self->database)
	{
		g_autoptr(GRecMutexLocker) lock = venture_database_lock_scope(self->database);
		if (g_object_get_data(G_OBJECT(self->database), LEASE_DATA) == self)
			g_object_set_data(G_OBJECT(self->database), LEASE_DATA, NULL);
		if (self->postgres_locked)
		{
			g_autoptr(OrmResult) released = venture_database_query_raw(self->database, LEASE_RELEASE_SQL, NULL, NULL);
		}
	}
	if (self->database_fd >= 0) close(self->database_fd);
	if (self->state_fd >= 0) close(self->state_fd);
	g_clear_object(&self->database);
	G_OBJECT_CLASS(venture_process_lease_parent_class)->finalize(object);
}
static void
venture_process_lease_class_init(VentureProcessLeaseClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = venture_process_lease_finalize;
}
static void
venture_process_lease_init(VentureProcessLease *self)
{
	self->state_fd = -1;
	self->database_fd = -1;
}
static gint
lease_file(const gchar *path, gboolean create, GError **error)
{
	GStatBuf info;
	gint fd = g_open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW | (create ? O_CREAT : 0), 0600);
	if (fd < 0 || fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != geteuid() ||
		(create && ((info.st_mode & 0777) != 0600 || info.st_nlink != 1)))
	{
		if (fd >= 0) close(fd);
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
			"Process lease needs an owned regular state/database file; links are refused");
		return -1;
	}
	if (flock(fd, LOCK_EX | LOCK_NB) != 0)
	{
		close(fd);
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
			"Another server or maintenance process holds this workspace");
		return -1;
	}
	return fd;
}
VentureProcessLease *
venture_process_lease_acquire(VentureDatabase *database, const gchar *state_directory, GError **error)
{
	g_autoptr(VentureProcessLease) self = NULL;
	g_autoptr(GRecMutexLocker) lock = NULL;
	g_autoptr(OrmResult) result = NULL;
	g_autofree gchar *path = NULL;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	g_return_val_if_fail(state_directory != NULL, NULL);
	lock = venture_database_lock_scope(database);
	if (g_object_get_data(G_OBJECT(database), LEASE_DATA) || venture_database_has_transaction(database))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
			"Acquire the process lease once, before starting transactions or services");
		return NULL;
	}
	self = g_object_new(VENTURE_TYPE_PROCESS_LEASE, NULL);
	self->database = g_object_ref(database);
	path = g_build_filename(state_directory, ".venture-process.lock", NULL);
	self->state_fd = lease_file(path, TRUE, error);
	if (self->state_fd < 0) return NULL;
	if (venture_database_get_backend(database) == VENTURE_DATABASE_BACKEND_POSTGRES)
	{
		result = venture_database_query_raw(database, LEASE_TRY_SQL, NULL, error);
		if (!result) return NULL;
		self->postgres_locked = orm_result_next(result) && orm_row_get_integer(orm_result_get_row(result), 0) == 1;
		if (!self->postgres_locked)
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
				"Another server or maintenance process holds this database");
			return NULL;
		}
	}
	else
	{
		result = venture_database_query_raw(database, "PRAGMA database_list", NULL, error);
		if (!result) return NULL;
		while (orm_result_next(result))
		{
			OrmRow *row = orm_result_get_row(result);
			const gchar *filename = orm_row_get_string(row, 2);
			if (g_strcmp0(orm_row_get_string(row, 1), "main") || !filename || !*filename) continue;
			self->database_fd = lease_file(filename, FALSE, error);
			if (self->database_fd < 0) return NULL;
			break;
		}
	}
	g_object_set_data(G_OBJECT(database), LEASE_DATA, self);
	return g_steal_pointer(&self);
}
