/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <glib/gstdio.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include "venture-test-util.h"

static gchar *executable;

/* A separate process proves that a different state path does not bypass the
 * database lease, and that normal release/crash-free exit relinquishes it. */
static gboolean
probe(const gchar *uri, const gchar *state)
{
	g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
	g_autoptr(GSubprocess) child = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *diagnostic = NULL;
	g_subprocess_launcher_setenv(launcher, "VENTURE_LEASE_TEST_URI", uri, TRUE);
	g_subprocess_launcher_setenv(launcher, "VENTURE_LEASE_TEST_STATE", state, TRUE);
	child = g_subprocess_launcher_spawn(launcher, &error, executable, "--lease-probe", NULL);
	g_assert_no_error(error); g_assert_nonnull(child);
	g_assert_true(g_subprocess_communicate_utf8(child, NULL, NULL, NULL, &diagnostic, &error));
	g_assert_no_error(error);
	g_assert_true(g_subprocess_get_if_exited(child));
	g_assert_cmpint(g_subprocess_get_exit_status(child), <=, 1);
	return g_subprocess_get_successful(child);
}
static void
assert_crash_releases(VentureDatabase *database, const gchar *uri,
	const gchar *state, const gchar *other_state)
{
	g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE);
	g_autoptr(GSubprocess) child = NULL;
	g_autoptr(GDataInputStream) output = NULL;
	g_autoptr(VentureProcessLease) lease = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *line = NULL;
	guint attempts;
	g_subprocess_launcher_setenv(launcher, "VENTURE_LEASE_TEST_URI", uri, TRUE);
	g_subprocess_launcher_setenv(launcher, "VENTURE_LEASE_TEST_STATE", other_state, TRUE);
	child = g_subprocess_launcher_spawn(launcher, &error, executable, "--lease-hold", NULL);
	g_assert_no_error(error); g_assert_nonnull(child);
	output = g_data_input_stream_new(g_subprocess_get_stdout_pipe(child));
	line = g_data_input_stream_read_line(output, NULL, NULL, &error);
	g_assert_no_error(error); g_assert_cmpstr(line, ==, "held");
	lease = venture_process_lease_acquire(database, state, &error);
	g_assert_null(lease); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	g_clear_error(&error);
	g_subprocess_force_exit(child);
	g_assert_true(g_subprocess_wait(child, NULL, &error));
	g_assert_no_error(error);
	/* PostgreSQL must observe the vanished client before releasing its session
	 * lock. Bound that handoff rather than assuming both processes run at once. */
	for (attempts = 0; attempts < 50; attempts++)
	{
		lease = venture_process_lease_acquire(database, state, &error);
		if (lease) break;
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
		g_clear_error(&error);
		g_usleep(100000);
	}
	g_assert_no_error(error); g_assert_nonnull(lease);
}

static void
test_backend(gconstpointer data)
{
	gboolean postgres = GPOINTER_TO_INT(data);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *root = NULL, *a = NULL, *b = NULL, *uri = NULL, *other_uri = NULL, *link = NULL;
	g_autoptr(VentureDatabase) database = NULL, neighbor = NULL;
	g_autoptr(VentureProcessLease) lease = NULL, duplicate = NULL, separate = NULL;
	const gchar *pg = g_getenv("VENTURE_TEST_ACCOUNTING_POSTGRES_URI");
	if (postgres && !pg) { g_test_skip("Explicit isolated PostgreSQL fixture required"); return; }
	root = g_dir_make_tmp("venture-process-lease-XXXXXX", &error);
	g_assert_no_error(error);
	a = g_build_filename(root, "a", NULL); b = g_build_filename(root, "b", NULL);
	g_assert_cmpint(g_mkdir(a, 0700), ==, 0); g_assert_cmpint(g_mkdir(b, 0700), ==, 0);
	uri = postgres ? g_strdup(pg) : g_strdup_printf("sqlite://%s/database.db", root);
	other_uri = g_strdup_printf("sqlite://%s/neighbor.db", root);
	database = venture_database_new(uri, &error);
	g_assert_no_error(error); g_assert_nonnull(database);
	lease = venture_process_lease_acquire(database, a, &error);
	g_assert_no_error(error); g_assert_nonnull(lease);
	g_assert_false(probe(uri, b));
	duplicate = venture_process_lease_acquire(database, b, &error);
	g_assert_null(duplicate); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	g_clear_error(&error);
	neighbor = venture_database_new(other_uri, &error);
	g_assert_no_error(error);
	separate = venture_process_lease_acquire(neighbor, a, &error);
	g_assert_null(separate); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	g_clear_error(&error);
	separate = venture_process_lease_acquire(neighbor, b, &error);
	g_assert_no_error(error); g_assert_nonnull(separate);
	g_clear_object(&separate);
	g_clear_object(&lease);
	g_assert_true(probe(uri, b));
	/* Reacquiring through the original connection must not leak a recursive
	 * PostgreSQL advisory-lock count or leave a stale repository marker. */
	lease = venture_process_lease_acquire(database, a, &error);
	g_assert_no_error(error); g_assert_nonnull(lease);
	g_clear_object(&lease);
	g_assert_true(probe(uri, b));
	assert_crash_releases(database, uri, a, b);
	link = g_build_filename(a, ".venture-process.lock", NULL);
	g_assert_cmpint(g_chmod(link, 0644), ==, 0);
	lease = venture_process_lease_acquire(database, a, &error);
	g_assert_null(lease); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
	g_clear_error(&error);
	/* Opening a hostile FIFO must refuse promptly, not wait for a writer. */
	g_assert_cmpint(g_unlink(link), ==, 0);
	g_assert_cmpint(mkfifo(link, 0600), ==, 0);
	g_assert_false(probe(uri, a));
	g_clear_object(&neighbor); g_clear_object(&database);
	venture_test_remove_tree(root);
}
int main(int argc, char **argv)
{
	if (argc == 2 && (!strcmp(argv[1], "--lease-probe") || !strcmp(argv[1], "--lease-hold")))
	{
		g_autoptr(GError) error = NULL;
		g_autoptr(VentureDatabase) database = NULL;
		g_autoptr(VentureProcessLease) lease = NULL;
		alarm(20);
		database = venture_database_new(g_getenv("VENTURE_LEASE_TEST_URI"), &error);
		if (!database) return 2;
		lease = venture_process_lease_acquire(database, g_getenv("VENTURE_LEASE_TEST_STATE"), &error);
		if (lease && !strcmp(argv[1], "--lease-hold"))
		{
			g_print("held\n"); fflush(stdout);
			g_usleep(30 * G_USEC_PER_SEC);
		}
		return lease ? 0 : 1;
	}
	executable = g_canonicalize_filename(argv[0], NULL);
	g_test_init(&argc, &argv, NULL);
	g_test_add_data_func("/process-lease/sqlite", GINT_TO_POINTER(FALSE), test_backend);
	g_test_add_data_func("/process-lease/postgresql", GINT_TO_POINTER(TRUE), test_backend);
	{
		gint status = g_test_run();
		g_free(executable);
		return status;
	}
}
