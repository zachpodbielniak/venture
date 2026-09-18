/*
 * test-backup-schedule.c - Scheduled, retained, verified backups and the
 * restore drill (issue #92).
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <venture.h>
#include <string.h>
#include <glib/gstdio.h>
#include <libsoup/soup.h>
#include "venture-test-util.h"

typedef struct
{
	VentureDatabase *db;
	VentureConfig *config;
	VentureContext *context;
	gchar *directory;
	gint64 org;
} Fixture;

static void
setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	(void)data;
	f->directory = g_dir_make_tmp("venture-backups-XXXXXX", &error);
	g_assert_no_error(error);
	f->config = venture_config_new();
	g_object_set(f->config, "state-dir", f->directory, NULL);
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
}

static void
teardown(Fixture *f, gconstpointer data)
{
	(void)data;
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
	venture_test_remove_tree(f->directory);
	g_free(f->directory);
}

static gint64
account_id(Fixture *f, const gchar *code)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	g_autoptr(VentureEntity) row = NULL;
	venture_query_set_organization(query, f->org);
	venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, code, NULL);
	row = venture_database_find_one(f->db, query, NULL);
	g_assert_nonnull(row);
	return venture_entity_get_id(row);
}

/* One posted journal and one customer with an invoice: enough for a trial
 * balance, ledger totals and document counts to mean something. */
static void
post_journal(Fixture *f, const gchar *amount, const gchar *day)
{
	g_autoptr(VentureJournal) journal = venture_journal_new();
	g_autoptr(VentureJournalLine) debit = venture_journal_line_new();
	g_autoptr(VentureJournalLine) credit = venture_journal_line_new();
	g_autoptr(GPtrArray) lines = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(GDateTime) when = g_date_time_new_from_iso8601(day, NULL);
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureJournal) posted = NULL;
	g_object_set(journal, "source-type", "organization", "source-id", f->org,
		"occurred-at", when, "currency", "USD", "organization-id", f->org, NULL);
	g_object_set(debit, "account-id", account_id(f, "1000"), "side", VENTURE_LEDGER_SIDE_DEBIT, "organization-id", f->org, NULL);
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(debit), "amount", amount, NULL));
	g_object_set(credit, "account-id", account_id(f, "4000"), "side", VENTURE_LEDGER_SIDE_CREDIT, "organization-id", f->org, NULL);
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(credit), "amount", amount, NULL));
	g_ptr_array_add(lines, g_object_ref(debit));
	g_ptr_array_add(lines, g_object_ref(credit));
	posted = venture_posting_service_post(venture_database_get_posting_service(f->db), journal, lines, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(posted);
}

static void
seed_books(Fixture *f)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureCompany) customer = venture_company_new();
	g_autoptr(VentureInvoice) invoice = venture_invoice_new();
	post_journal(f, "50 USD", "2026-08-10T00:00:00Z");
	g_object_set(customer, "name", "Acme", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(customer), f->org);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(customer), NULL, &error));
	g_assert_no_error(error);
	venture_entity_set_organization_id(VENTURE_ENTITY(invoice), f->org);
	g_object_set(invoice, "number", "INV-BAK", "company-id", venture_entity_get_id(VENTURE_ENTITY(customer)), NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(invoice), NULL, &error));
	g_assert_no_error(error);
}

static VentureEntity *
make_schedule(Fixture *f, const gchar *scope, const gchar *cron, gint64 retention, gboolean verify)
{
	g_autoptr(GError) error = NULL;
	VentureBackupSchedule *schedule = venture_backup_schedule_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(schedule), f->org);
	g_object_set(schedule, "name", "Nightly", "scope", scope, "schedule", cron, "retention", retention,
		"destination", f->directory, "verify", verify, NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(schedule), NULL, &error));
	g_assert_no_error(error);
	return VENTURE_ENTITY(schedule);
}

static gint
run_due(Fixture *f, const gchar *at)
{
	g_autoptr(GDateTime) as_of = g_date_time_new_from_iso8601(at, NULL);
	g_autoptr(GError) error = NULL;
	gint ran;
	g_assert_nonnull(as_of);
	ran = venture_backup_schedule_service_run_due(venture_backup_schedule_service_get(f->db), f->org, as_of, NULL, &error);
	g_assert_no_error(error);
	return ran;
}

static GPtrArray *
runs(Fixture *f)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_BACKUP_RUN);
	g_autoptr(GError) error = NULL;
	GPtrArray *rows;
	venture_query_set_organization(query, f->org);
	venture_query_set_limit(query, 0);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	rows = venture_database_find(f->db, query, &error);
	g_assert_no_error(error);
	return rows;
}

static gchar *
text(VentureEntity *e, const gchar *field)
{
	gchar *value = NULL;
	g_object_get(e, field, &value, NULL);
	return value;
}

static gint64
number(VentureEntity *e, const gchar *field)
{
	gint64 value = 0;
	g_object_get(e, field, &value, NULL);
	return value;
}

static void
assert_run_file(VentureEntity *run)
{
	g_autofree gchar *path = text(run, "path");
	g_autofree gchar *sha = text(run, "sha256");
	g_autofree gchar *contents = NULL;
	g_autofree gchar *digest = NULL;
	gsize length = 0;
	g_autoptr(GError) error = NULL;
	g_assert_true(g_file_get_contents(path, &contents, &length, &error));
	g_assert_no_error(error);
	g_assert_cmpint(number(run, "size"), ==, (gint64)length);
	g_assert_cmpint(length, >, 0);
	digest = g_compute_checksum_for_data(G_CHECKSUM_SHA256, (const guchar *)contents, length);
	g_assert_cmpstr(sha, ==, digest);
}

/* A due schedule writes a version-4 snapshot into the destination and
 * records started, finished, size, sha256 and status. It is not run twice
 * on the same day. */
static void
test_sweep_writes_snapshot(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) schedule = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GDateTime) started = NULL, finished = NULL, last = NULL;
	g_autoptr(VentureEntity) stored = NULL;
	VentureEntity *run;
	g_autofree gchar *status = NULL, *path = NULL, *contents = NULL, *scope = NULL, *kind = NULL;
	(void)data;
	seed_books(f);
	schedule = make_schedule(f, "organization", "0 2 * * *", 3, FALSE);
	g_assert_cmpint(run_due(f, "2026-09-01T01:00:00Z"), ==, 0);
	g_assert_cmpint(run_due(f, "2026-09-01T02:30:00Z"), ==, 1);
	g_assert_cmpint(run_due(f, "2026-09-01T03:00:00Z"), ==, 0);
	rows = runs(f);
	g_assert_cmpuint(rows->len, ==, 1);
	run = g_ptr_array_index(rows, 0);
	status = text(run, "status");
	path = text(run, "path");
	scope = text(run, "scope");
	kind = text(run, "kind");
	g_object_get(run, "started-at", &started, "finished-at", &finished, NULL);
	g_assert_cmpstr(status, ==, "succeeded");
	g_assert_cmpstr(scope, ==, "organization");
	g_assert_cmpstr(kind, ==, "backup");
	g_assert_cmpint(number(run, "schedule-id"), ==, venture_entity_get_id(schedule));
	g_assert_nonnull(started);
	g_assert_nonnull(finished);
	g_assert_true(g_str_has_prefix(path, f->directory));
	assert_run_file(run);
	g_assert_true(g_file_get_contents(path, &contents, NULL, NULL));
	g_assert_nonnull(strstr(contents, "\"version\":4"));
	g_assert_nonnull(strstr(contents, "\"invoice\""));
	stored = venture_database_get(f->db, VENTURE_TYPE_BACKUP_SCHEDULE, venture_entity_get_id(schedule), NULL);
	g_object_get(stored, "last-run-at", &last, NULL);
	g_assert_nonnull(last);
	/* The scheduled sweep does not verify unless the schedule says so. */
	{
		g_autofree gchar *verification = text(run, "verification");
		g_assert_true(venture_string_is_empty(verification));
	}
}

/* Successful backups beyond retention are pruned oldest-first; the run
 * record survives as evidence with status pruned. */
static void
test_retention_prunes_oldest(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) schedule = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	guint i;
	(void)data;
	seed_books(f);
	schedule = make_schedule(f, "organization", "daily", 2, FALSE);
	g_assert_cmpint(run_due(f, "2026-09-01T00:30:00Z"), ==, 1);
	g_assert_cmpint(run_due(f, "2026-09-02T00:30:00Z"), ==, 1);
	g_assert_cmpint(run_due(f, "2026-09-03T00:30:00Z"), ==, 1);
	rows = runs(f);
	g_assert_cmpuint(rows->len, ==, 3);
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *run = g_ptr_array_index(rows, i);
		g_autofree gchar *status = text(run, "status");
		g_autofree gchar *path = text(run, "path");
		if (i == 0)
		{
			g_assert_cmpstr(status, ==, "pruned");
			g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
		}
		else
		{
			g_assert_cmpstr(status, ==, "succeeded");
			assert_run_file(run);
		}
	}
}

/* A failed run records its status and message, keeps every earlier file
 * and prunes nothing even when the count exceeds retention. */
static void
test_failure_keeps_everything(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) schedule = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) failed = NULL, stored = NULL;
	g_autofree gchar *blocker = g_build_filename(f->directory, "blocker", NULL);
	g_autofree gchar *blocked = g_build_filename(blocker, "sub", NULL);
	guint i;
	(void)data;
	seed_books(f);
	schedule = make_schedule(f, "organization", "daily", 5, FALSE);
	g_assert_cmpint(run_due(f, "2026-09-01T00:30:00Z"), ==, 1);
	g_assert_cmpint(run_due(f, "2026-09-02T00:30:00Z"), ==, 1);
	/* A regular file where the destination directory should be. The sweep
	 * stamped last-run-at, so edit the stored row, not the stale handle. */
	g_assert_true(g_file_set_contents(blocker, "not a directory", -1, NULL));
	stored = venture_database_get(f->db, VENTURE_TYPE_BACKUP_SCHEDULE, venture_entity_get_id(schedule), &error);
	g_assert_no_error(error);
	g_object_set(stored, "destination", blocked, "retention", (gint64)1, NULL);
	g_assert_true(venture_database_save(f->db, stored, NULL, &error));
	g_assert_no_error(error);
	failed = venture_backup_schedule_service_run(venture_backup_schedule_service_get(f->db),
		VENTURE_BACKUP_SCHEDULE(stored), NULL, NULL, &error);
	g_assert_null(failed);
	g_assert_nonnull(error);
	g_clear_error(&error);
	rows = runs(f);
	g_assert_cmpuint(rows->len, ==, 3);
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *run = g_ptr_array_index(rows, i);
		g_autofree gchar *status = text(run, "status");
		g_autofree gchar *message = text(run, "message");
		if (i < 2)
		{
			g_assert_cmpstr(status, ==, "succeeded");
			assert_run_file(run);
		}
		else
		{
			g_assert_cmpstr(status, ==, "failed");
			g_assert_false(venture_string_is_empty(message));
		}
	}
}

/* The field tables accept a schedule, and the service refuses the shapes a
 * sweep could not act on: an unparseable cron, an unknown scope, and a
 * retention that would keep nothing. */
static void
test_schedule_validation(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureBackupSchedule) schedule = venture_backup_schedule_new();
	g_autoptr(GError) error = NULL;
	(void)data;
	venture_entity_set_organization_id(VENTURE_ENTITY(schedule), f->org);
	g_object_set(schedule, "name", "Bad", "scope", "organization", "schedule", "every tuesday", "retention", (gint64)1, NULL);
	g_assert_false(venture_database_save(f->db, VENTURE_ENTITY(schedule), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	g_object_set(schedule, "schedule", "daily", "scope", "galaxy", NULL);
	g_assert_false(venture_database_save(f->db, VENTURE_ENTITY(schedule), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	g_object_set(schedule, "scope", "installation", "retention", (gint64)-1, NULL);
	g_assert_false(venture_database_save(f->db, VENTURE_ENTITY(schedule), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	g_object_set(schedule, "retention", (gint64)0, NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(schedule), NULL, &error));
	g_assert_no_error(error);
}

/* A backup_run is evidence: generic writes and removals are refused by
 * name, and the refusal names the service. */
static void
test_run_is_service_owned(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureBackupRun) run = venture_backup_run_new();
	g_autoptr(VentureEntity) schedule = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;
	(void)data;
	seed_books(f);
	venture_entity_set_organization_id(VENTURE_ENTITY(run), f->org);
	g_object_set(run, "kind", "backup", "status", "succeeded", NULL);
	g_assert_false(venture_database_save(f->db, VENTURE_ENTITY(run), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_nonnull(strstr(error->message, "VentureBackupScheduleService"));
	g_clear_error(&error);
	schedule = make_schedule(f, "organization", "daily", 2, FALSE);
	g_assert_cmpint(run_due(f, "2026-09-01T00:30:00Z"), ==, 1);
	rows = runs(f);
	g_assert_false(venture_database_delete(f->db, g_ptr_array_index(rows, 0), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

static JsonObject *
report_of(VentureEntity *run, JsonNode **owner)
{
	g_autofree gchar *report = text(run, "report");
	g_autoptr(GError) error = NULL;
	g_assert_false(venture_string_is_empty(report));
	*owner = venture_json_parse(report, &error);
	g_assert_no_error(error);
	return json_node_get_object(*owner);
}

/* Verification restores the snapshot into a temporary empty database and
 * ties trial balance, ledger totals and document counts to the live books. */
static void
test_verify_ties(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) schedule = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GDateTime) verified = NULL;
	g_autoptr(VentureEntity) stored = NULL;
	g_autofree gchar *verification = NULL;
	JsonObject *report, *counts;
	VentureEntity *run;
	(void)data;
	seed_books(f);
	schedule = make_schedule(f, "organization", "daily", 2, FALSE);
	g_assert_cmpint(run_due(f, "2026-09-01T00:30:00Z"), ==, 1);
	rows = runs(f);
	run = g_ptr_array_index(rows, 0);
	g_assert_true(venture_backup_schedule_service_verify(venture_backup_schedule_service_get(f->db),
		VENTURE_BACKUP_RUN(run), NULL, &error));
	g_assert_no_error(error);
	stored = venture_database_get(f->db, VENTURE_TYPE_BACKUP_RUN, venture_entity_get_id(run), &error);
	g_assert_no_error(error);
	verification = text(stored, "verification");
	g_object_get(stored, "verified-at", &verified, NULL);
	g_assert_cmpstr(verification, ==, "tie");
	g_assert_nonnull(verified);
	report = report_of(stored, &node);
	g_assert_cmpstr(json_object_get_string_member(report, "result"), ==, "tie");
	g_assert_true(json_object_get_boolean_member(json_object_get_object_member(report, "trial_balance"), "tie"));
	g_assert_true(json_object_get_boolean_member(json_object_get_object_member(report, "ledger_totals"), "tie"));
	counts = json_object_get_object_member(report, "document_counts");
	g_assert_true(json_object_has_member(counts, "invoice"));
	g_assert_cmpint(json_object_get_int_member(json_object_get_object_member(counts, "invoice"), "live"), ==, 1);
	g_assert_cmpint(json_object_get_int_member(json_object_get_object_member(counts, "invoice"), "restored"), ==, 1);
	g_assert_cmpint(json_object_get_int_member(json_object_get_object_member(counts, "journal"), "restored"), ==, 1);
	/* The live database is untouched by the temporary restore. */
	{
		g_autoptr(VentureQuery) orgs = venture_query_new(VENTURE_TYPE_ORGANIZATION);
		g_autoptr(VentureQuery) journals = venture_query_new(VENTURE_TYPE_JOURNAL);
		g_assert_cmpint(venture_database_count(f->db, orgs, NULL), ==, 1);
		venture_query_set_organization(journals, f->org);
		g_assert_cmpint(venture_database_count(f->db, journals, NULL), ==, 1);
	}
}

/* Books that moved on since the snapshot report a mismatch naming what
 * differs, and a schedule with verify set is verified by the sweep itself. */
static void
test_verify_mismatch_and_scheduled(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) schedule = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(VentureEntity) stored = NULL;
	g_autofree gchar *verification = NULL, *scheduled = NULL;
	JsonObject *report;
	JsonArray *mismatches;
	VentureEntity *run;
	(void)data;
	seed_books(f);
	schedule = make_schedule(f, "organization", "daily", 2, TRUE);
	g_assert_cmpint(run_due(f, "2026-09-01T00:30:00Z"), ==, 1);
	rows = runs(f);
	run = g_ptr_array_index(rows, 0);
	scheduled = text(run, "verification");
	g_assert_cmpstr(scheduled, ==, "tie");
	post_journal(f, "25 USD", "2026-08-20T00:00:00Z");
	g_assert_true(venture_backup_schedule_service_verify(venture_backup_schedule_service_get(f->db),
		VENTURE_BACKUP_RUN(run), NULL, &error));
	g_assert_no_error(error);
	stored = venture_database_get(f->db, VENTURE_TYPE_BACKUP_RUN, venture_entity_get_id(run), &error);
	verification = text(stored, "verification");
	g_assert_cmpstr(verification, ==, "mismatch");
	report = report_of(stored, &node);
	g_assert_cmpstr(json_object_get_string_member(report, "result"), ==, "mismatch");
	g_assert_false(json_object_get_boolean_member(json_object_get_object_member(report, "trial_balance"), "tie"));
	g_assert_false(json_object_get_boolean_member(json_object_get_object_member(report, "ledger_totals"), "tie"));
	mismatches = json_object_get_array_member(report, "mismatches");
	g_assert_cmpuint(json_array_get_length(mismatches), >, 0);
	g_assert_cmpint(json_object_get_int_member(json_object_get_object_member(
		json_object_get_object_member(report, "document_counts"), "journal"), "live"), ==, 2);
}

/* A run whose file is gone, or that never succeeded, cannot be verified. */
static void
test_verify_refusals(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) schedule = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	VentureEntity *run;
	(void)data;
	seed_books(f);
	schedule = make_schedule(f, "organization", "daily", 1, FALSE);
	g_assert_cmpint(run_due(f, "2026-09-01T00:30:00Z"), ==, 1);
	g_assert_cmpint(run_due(f, "2026-09-02T00:30:00Z"), ==, 1);
	rows = runs(f);
	run = g_ptr_array_index(rows, 0);
	g_assert_false(venture_backup_schedule_service_verify(venture_backup_schedule_service_get(f->db),
		VENTURE_BACKUP_RUN(run), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	run = g_ptr_array_index(rows, 1);
	path = text(run, "path");
	g_assert_cmpint(g_unlink(path), ==, 0);
	g_assert_false(venture_backup_schedule_service_verify(venture_backup_schedule_service_get(f->db),
		VENTURE_BACKUP_RUN(run), NULL, &error));
	g_assert_nonnull(error);
}

/* An installation schedule copies the whole database; verification opens
 * the copy and ties every organization. */
static void
test_installation_backup(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) schedule = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDatabase) copy = NULL;
	g_autoptr(VentureQuery) users = venture_query_new(VENTURE_TYPE_USER);
	g_autoptr(VentureUser) user = venture_user_new();
	g_autoptr(VentureEntity) stored = NULL;
	g_autofree gchar *path = NULL, *uri = NULL, *scope = NULL, *verification = NULL;
	VentureEntity *run;
	(void)data;
	seed_books(f);
	/* Credentials are part of an installation backup and not of a snapshot. */
	g_object_set(user, "username", "owner", "role", VENTURE_USER_ROLE_OWNER, "active", TRUE, NULL);
	g_assert_true(venture_user_set_password(user, "owner-password-1", 1000, NULL));
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(user), NULL, &error));
	g_assert_no_error(error);
	schedule = make_schedule(f, "installation", "0 3 * * 0", 2, TRUE);
	g_assert_cmpint(run_due(f, "2026-09-06T03:00:00Z"), ==, 1);
	rows = runs(f);
	g_assert_cmpuint(rows->len, ==, 1);
	run = g_ptr_array_index(rows, 0);
	scope = text(run, "scope");
	path = text(run, "path");
	verification = text(run, "verification");
	g_assert_cmpstr(scope, ==, "installation");
	if (g_strcmp0(verification, "tie") != 0)
	{
		g_autofree gchar *report = text(run, "report");
		g_test_message("installation report: %s", report);
	}
	g_assert_cmpstr(verification, ==, "tie");
	g_assert_true(g_str_has_suffix(path, ".sqlite"));
	assert_run_file(run);
	uri = g_strconcat("sqlite://", path, NULL);
	copy = venture_database_new(uri, &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_database_count(copy, users, &error), ==, 1);
	g_assert_no_error(error);
	stored = venture_database_get(copy, VENTURE_TYPE_ORGANIZATION, f->org, &error);
	g_assert_no_error(error);
	g_assert_nonnull(stored);
}

/* The drill is verification kept as its own named run with the report. */
static void
test_restore_drill(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) schedule = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) drill = NULL, latest = NULL, stored = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *kind = NULL, *name = NULL, *verification = NULL, *status = NULL;
	VentureEntity *run;
	(void)data;
	seed_books(f);
	g_assert_null(venture_backup_schedule_service_latest_run(venture_backup_schedule_service_get(f->db), f->org, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_clear_error(&error);
	schedule = make_schedule(f, "organization", "daily", 2, FALSE);
	g_assert_cmpint(run_due(f, "2026-09-01T00:30:00Z"), ==, 1);
	rows = runs(f);
	run = g_ptr_array_index(rows, 0);
	latest = venture_backup_schedule_service_latest_run(venture_backup_schedule_service_get(f->db), f->org, &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_entity_get_id(latest), ==, venture_entity_get_id(run));
	drill = venture_backup_schedule_service_restore_drill(venture_backup_schedule_service_get(f->db),
		VENTURE_BACKUP_RUN(run), "Quarterly drill", NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(drill);
	kind = text(drill, "kind");
	name = text(drill, "name");
	verification = text(drill, "verification");
	status = text(drill, "status");
	g_assert_cmpstr(kind, ==, "drill");
	g_assert_cmpstr(name, ==, "Quarterly drill");
	g_assert_cmpstr(verification, ==, "tie");
	g_assert_cmpstr(status, ==, "succeeded");
	g_assert_cmpint(number(drill, "source-run-id"), ==, venture_entity_get_id(run));
	stored = venture_database_get(f->db, VENTURE_TYPE_BACKUP_RUN, venture_entity_get_id(drill), &error);
	g_assert_no_error(error);
	g_assert_cmpstr(json_object_get_string_member(report_of(stored, &node), "result"), ==, "tie");
	/* Drills are not backups: retention never counts or prunes them, and
	 * the latest backup is still the snapshot, not the drill. */
	g_clear_object(&latest);
	latest = venture_backup_schedule_service_latest_run(venture_backup_schedule_service_get(f->db), f->org, &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_entity_get_id(latest), ==, venture_entity_get_id(run));
}

/* Switching the module off hides the records and refuses the sweep. */
static void
test_module_off(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	(void)data;
	venture_config_set_module_enabled(f->config, "backup", FALSE);
	g_assert_cmpuint(venture_entity_registry_lookup(venture_entity_registry_get_default(), "backup_schedule"), ==, G_TYPE_INVALID);
	g_assert_cmpuint(venture_entity_registry_lookup(venture_entity_registry_get_default(), "backup_run"), ==, G_TYPE_INVALID);
	g_assert_cmpint(venture_backup_schedule_service_run_due(venture_backup_schedule_service_get(f->db), f->org, NULL, NULL, &error), ==, -1);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	venture_config_set_module_enabled(f->config, "backup", TRUE);
}

/* Migration 000370 must succeed with the module off, leaving no backup
 * tables behind, and with it on both tables exist and carry no invented
 * runs. */
static void
test_migrate_disabled(void)
{
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureDatabase) db = NULL;
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(OrmResult) result = NULL;
	g_autoptr(VentureConfig) everything = NULL;
	g_autoptr(VentureModuleRegistry) registry = NULL;
	venture_config_set_module_enabled(config, "backup", FALSE);
	db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	context = venture_context_new(config, db);
	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	result = venture_database_query_raw(db,
		"SELECT CAST(COUNT(*) AS BIGINT) FROM sqlite_master WHERE type = 'table' AND name IN ('backup_schedules', 'backup_runs')",
		NULL, &error);
	g_assert_no_error(error);
	g_assert_true(orm_result_next(result));
	g_assert_cmpint(orm_row_get_integer(orm_result_get_row(result), 0), ==, 0);
	g_clear_object(&result);
	g_clear_object(&context);
	g_clear_object(&db);
	everything = venture_config_new();
	registry = venture_module_registry_new();
	venture_module_registry_register_builtins(registry);
	g_assert_true(venture_module_registry_configure(registry, everything, NULL));
	venture_module_registry_apply(registry, venture_entity_registry_get_default());
	db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	result = venture_database_query_raw(db,
		"SELECT CAST(COUNT(*) AS BIGINT) FROM sqlite_master WHERE type = 'table' AND name IN ('backup_schedules', 'backup_runs')",
		NULL, &error);
	g_assert_no_error(error);
	g_assert_true(orm_result_next(result));
	g_assert_cmpint(orm_row_get_integer(orm_result_get_row(result), 0), ==, 2);
	g_clear_object(&result);
	result = venture_database_query_raw(db, "SELECT CAST(COUNT(*) AS BIGINT) FROM backup_runs", NULL, &error);
	g_assert_no_error(error);
	g_assert_true(orm_result_next(result));
	g_assert_cmpint(orm_row_get_integer(orm_result_get_row(result), 0), ==, 0);
}

/* --- Web page, REST actions and the CLI ---------------------------------- */

typedef struct { gboolean done; GError *error; GBytes *bytes; gchar *out, *err; } Result;
typedef struct { Fixture base; VentureWebServer *server; gchar *owner, *editor, *viewer; } ServerFixture;

/* A bearer bound to a real member of the organization: the org-access
 * middleware sends a principal with no membership to /account before any
 * page handler runs, so a role gate is only provable through a member.
 * Every member holds the same finance membership, which reads backup
 * records; what differs is the global role the Backups gate checks. */
static gchar *
issue_token(ServerFixture *s, const gchar *name, VentureUserRole role)
{
	g_autoptr(VentureApiToken) token = venture_api_token_new();
	g_autoptr(VentureUser) user = venture_user_new();
	g_autoptr(VentureEntity) member = NULL;
	g_autoptr(GError) error = NULL;
	gchar *secret;
	g_object_set(user, "username", name, "role", role, "active", TRUE, NULL);
	g_assert_true(venture_user_set_password(user, "backup-test-password", 1000, NULL));
	g_assert_true(venture_database_save(s->base.db, VENTURE_ENTITY(user), NULL, &error));
	g_assert_no_error(error);
	member = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP, "user-id", venture_entity_get_id(VENTURE_ENTITY(user)),
		"organization-id", s->base.org, "role", VENTURE_ORGANIZATION_ROLE_FINANCE, "active", TRUE, NULL);
	g_assert_true(venture_database_save(s->base.db, member, NULL, &error));
	g_assert_no_error(error);
	g_object_set(token, "name", name, "role", role, "user-id", venture_entity_get_id(VENTURE_ENTITY(user)), NULL);
	secret = venture_api_token_generate(token);
	g_assert_true(venture_database_save(s->base.db, VENTURE_ENTITY(token), NULL, &error));
	g_assert_no_error(error);
	return secret;
}

static void
server_setup(ServerFixture *s, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GSocketListener) listener = g_socket_listener_new();
	guint16 port = g_socket_listener_add_any_inet_port(listener, NULL, &error);
	g_assert_no_error(error);
	g_socket_listener_close(listener);
	g_setenv("VENTURE_TEST_SESSION_SECRET", "backup-test-secret", TRUE);
	setup(&s->base, data);
	g_object_set(s->base.config, "server-bind-address", "127.0.0.1", "server-port", (gint64)port,
		"security-session-secret-env", "VENTURE_TEST_SESSION_SECRET", NULL);
	s->server = venture_web_server_new(s->base.context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(s->server, &error));
	g_assert_no_error(error);
	s->owner = issue_token(s, "owner", VENTURE_USER_ROLE_OWNER);
	s->editor = issue_token(s, "accountant", VENTURE_USER_ROLE_EDITOR);
	s->viewer = issue_token(s, "reader", VENTURE_USER_ROLE_VIEWER);
}

static void
server_teardown(ServerFixture *s, gconstpointer data)
{
	venture_web_server_stop(s->server);
	g_clear_object(&s->server);
	g_free(s->owner);
	g_free(s->editor);
	g_free(s->viewer);
	teardown(&s->base, data);
}

static void
http_done(GObject *source, GAsyncResult *result, gpointer data)
{
	Result *r = data;
	r->bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &r->error);
	r->done = TRUE;
}

static guint
request(ServerFixture *s, const gchar *token, const gchar *method, const gchar *path, const gchar *type,
	const gchar *body, gchar **out)
{
	g_autoptr(SoupSession) session = soup_session_new_with_options("timeout", 15, NULL);
	g_autofree gchar *url = g_strconcat(venture_web_server_get_base_url(s->server), path, NULL);
	g_autofree gchar *bearer = g_strconcat("Bearer ", token, NULL);
	g_autoptr(SoupMessage) message = soup_message_new(method, url);
	Result result;
	memset(&result, 0, sizeof(result));
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	soup_message_headers_append(soup_message_get_request_headers(message), "Authorization", bearer);
	if (body)
	{
		g_autoptr(GBytes) bytes = g_bytes_new(body, strlen(body));
		soup_message_set_request_body_from_bytes(message, type, bytes);
	}
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL, http_done, &result);
	while (!result.done)
		g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(result.error);
	if (out)
		*out = g_strndup(g_bytes_get_data(result.bytes, NULL), g_bytes_get_size(result.bytes));
	g_bytes_unref(result.bytes);
	return soup_message_get_status(message);
}

static void
cli_done(GObject *source, GAsyncResult *result, gpointer data)
{
	Result *r = data;
	g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &r->out, &r->err, &r->error);
	r->done = TRUE;
}

static gboolean
cli_timeout(gpointer process)
{
	g_subprocess_force_exit(process);
	return G_SOURCE_CONTINUE;
}

static gchar *
cli(ServerFixture *s, const gchar *token, const gchar *const *args, gboolean expect_success)
{
	g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
	g_autoptr(GSubprocess) process = NULL;
	g_autoptr(GPtrArray) argv = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GError) error = NULL;
	Result result;
	guint i, timeout;
	memset(&result, 0, sizeof(result));
	g_ptr_array_add(argv, g_canonicalize_filename("build/debug/venturectl", NULL));
	g_ptr_array_add(argv, g_strdup("--server"));
	g_ptr_array_add(argv, g_strdup(venture_web_server_get_base_url(s->server)));
	g_ptr_array_add(argv, g_strdup("--token"));
	g_ptr_array_add(argv, g_strdup(token));
	g_ptr_array_add(argv, g_strdup("-f"));
	g_ptr_array_add(argv, g_strdup("json"));
	for (i = 0; args[i]; i++)
		g_ptr_array_add(argv, g_strdup(args[i]));
	g_ptr_array_add(argv, NULL);
	process = g_subprocess_launcher_spawnv(launcher, (const gchar *const *)argv->pdata, &error);
	g_assert_no_error(error);
	timeout = g_timeout_add_seconds(30, cli_timeout, process);
	g_subprocess_communicate_utf8_async(process, NULL, NULL, cli_done, &result);
	while (!result.done)
		g_main_context_iteration(NULL, TRUE);
	g_source_remove(timeout);
	g_assert_no_error(result.error);
	if (g_subprocess_get_successful(process) != expect_success)
		g_test_message("CLI: %s%s", result.out, result.err);
	g_assert_true(g_subprocess_get_successful(process) == expect_success);
	g_free(result.err);
	return result.out;
}

static void
test_surfaces(ServerFixture *s, gconstpointer data)
{
	Fixture *f = &s->base;
	g_autoptr(VentureEntity) schedule = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autofree gchar *page = NULL, *run_form = NULL, *verify_form = NULL, *drill_form = NULL;
	g_autofree gchar *run_path = NULL, *verify_path = NULL, *body = NULL, *out = NULL;
	g_autofree gchar *run_id = NULL, *schedule_id = NULL;
	VentureEntity *run;
	(void)data;
	seed_books(f);
	schedule = make_schedule(f, "organization", "daily", 3, FALSE);
	schedule_id = g_strdup_printf("%" G_GINT64_FORMAT, venture_entity_get_id(schedule));
	run_form = g_strdup_printf("action=run&schedule_id=%s", schedule_id);
	/* The owner runs a backup from the settings page. Accountants (editor)
	 * and read-only users may look, but not trigger anything. */
	g_assert_cmpuint(request(s, s->editor, "POST", "/settings/backups", "application/x-www-form-urlencoded", run_form, NULL), ==, 403);
	g_assert_cmpuint(request(s, s->viewer, "POST", "/settings/backups", "application/x-www-form-urlencoded", run_form, NULL), ==, 403);
	g_assert_cmpuint(request(s, s->owner, "POST", "/settings/backups", "application/x-www-form-urlencoded", run_form, NULL), ==, 302);
	rows = runs(f);
	g_assert_cmpuint(rows->len, ==, 1);
	run = g_ptr_array_index(rows, 0);
	run_id = g_strdup_printf("%" G_GINT64_FORMAT, venture_entity_get_id(run));
	verify_form = g_strdup_printf("action=verify&run_id=%s", run_id);
	drill_form = g_strdup_printf("action=drill&run_id=%s&name=Web%%20drill", run_id);
	g_assert_cmpuint(request(s, s->editor, "POST", "/settings/backups", "application/x-www-form-urlencoded", verify_form, NULL), ==, 403);
	g_assert_cmpuint(request(s, s->owner, "POST", "/settings/backups", "application/x-www-form-urlencoded", verify_form, NULL), ==, 302);
	g_assert_cmpuint(request(s, s->owner, "POST", "/settings/backups", "application/x-www-form-urlencoded", drill_form, NULL), ==, 302);
	g_assert_cmpuint(request(s, s->viewer, "GET", "/settings/backups", NULL, NULL, &page), ==, 200);
	{
		g_autofree gchar *sha = text(run, "sha256");
		g_assert_nonnull(strstr(page, sha));
		g_assert_nonnull(strstr(page, "tie"));
		g_assert_nonnull(strstr(page, "Web drill"));
		g_assert_null(strstr(page, "action=\"/settings/backups\""));
	}
	g_clear_pointer(&page, g_free);
	g_assert_cmpuint(request(s, s->owner, "GET", "/settings/backups", NULL, NULL, &page), ==, 200);
	g_assert_nonnull(strstr(page, "action=\"/settings/backups\""));
	/* REST actions carry the same role gate. */
	run_path = g_strdup_printf("/api/v1/backup_schedule/%s/actions/run", schedule_id);
	verify_path = g_strdup_printf("/api/v1/backup_run/%s/actions/verify", run_id);
	g_assert_cmpuint(request(s, s->editor, "POST", run_path, "application/json", "{}", NULL), ==, 403);
	g_assert_cmpuint(request(s, s->viewer, "POST", verify_path, "application/json", "{}", NULL), ==, 403);
	g_assert_cmpuint(request(s, s->owner, "POST", run_path, "application/json", "{}", &body), ==, 200);
	g_assert_nonnull(strstr(body, "\"succeeded\""));
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(request(s, s->owner, "POST", verify_path, "application/json", "{}", &body), ==, 200);
	g_assert_nonnull(strstr(body, "\"tie\""));
	/* The CLI verbs reach the same actions. */
	{
		const gchar *verify_args[] = { "backup", "verify", run_id, NULL };
		const gchar *drill_args[] = { "backup", "restore-drill", "name=CLI drill", NULL };
		const gchar *run_args[] = { "backup", "run", schedule_id, NULL };
		const gchar *bad_args[] = { "backup", "nope", NULL };
		g_autofree gchar *drill = NULL, *ran = NULL, *bad = NULL, *refused = NULL;
		out = cli(s, s->owner, verify_args, TRUE);
		g_assert_nonnull(strstr(out, "\"tie\""));
		drill = cli(s, s->owner, drill_args, TRUE);
		g_assert_nonnull(strstr(drill, "\"drill\""));
		g_assert_nonnull(strstr(drill, "CLI drill"));
		ran = cli(s, s->owner, run_args, TRUE);
		g_assert_nonnull(strstr(ran, "\"succeeded\""));
		bad = cli(s, s->owner, bad_args, FALSE);
		refused = cli(s, s->editor, verify_args, FALSE);
	}
	g_clear_pointer(&rows, g_ptr_array_unref);
	rows = runs(f);
	/* web run, REST run, CLI run = three backups; web drill and CLI drill. */
	g_assert_cmpuint(rows->len, ==, 5);
}

/* With the module off the page and the actions are gone. */
static void
test_surfaces_module_off(ServerFixture *s, gconstpointer data)
{
	(void)data;
	venture_config_set_module_enabled(s->base.config, "backup", FALSE);
	g_assert_cmpuint(request(s, s->owner, "GET", "/settings/backups", NULL, NULL, NULL), ==, 404);
	g_assert_cmpuint(request(s, s->owner, "POST", "/api/v1/backup_schedule/1/actions/run", "application/json", "{}", NULL), ==, 404);
	venture_config_set_module_enabled(s->base.config, "backup", TRUE);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/backup/schedule/sweep", Fixture, NULL, setup, test_sweep_writes_snapshot, teardown);
	g_test_add("/backup/schedule/retention", Fixture, NULL, setup, test_retention_prunes_oldest, teardown);
	g_test_add("/backup/schedule/failure", Fixture, NULL, setup, test_failure_keeps_everything, teardown);
	g_test_add("/backup/schedule/validation", Fixture, NULL, setup, test_schedule_validation, teardown);
	g_test_add("/backup/schedule/service-owned", Fixture, NULL, setup, test_run_is_service_owned, teardown);
	g_test_add("/backup/schedule/installation", Fixture, NULL, setup, test_installation_backup, teardown);
	g_test_add("/backup/verify/tie", Fixture, NULL, setup, test_verify_ties, teardown);
	g_test_add("/backup/verify/mismatch", Fixture, NULL, setup, test_verify_mismatch_and_scheduled, teardown);
	g_test_add("/backup/verify/refusals", Fixture, NULL, setup, test_verify_refusals, teardown);
	g_test_add("/backup/drill", Fixture, NULL, setup, test_restore_drill, teardown);
	g_test_add("/backup/module-off", Fixture, NULL, setup, test_module_off, teardown);
	g_test_add_func("/backup/migration", test_migrate_disabled);
	g_test_add("/backup/surfaces", ServerFixture, NULL, server_setup, test_surfaces, server_teardown);
	g_test_add("/backup/surfaces/module-off", ServerFixture, NULL, server_setup, test_surfaces_module_off, server_teardown);
	return g_test_run();
}
