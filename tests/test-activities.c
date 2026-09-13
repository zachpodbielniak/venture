/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <string.h>
#include <libsoup/soup.h>
#include "venture-test-util.h"

typedef struct
{
	VentureDatabase *database;
	VentureConfig *config;
	VentureContext *context;
	VentureWebServer *server;
	SoupSession *session;
	gchar *state_dir;
	gchar *url;
} Fixture;

static void
fixture_set_up(Fixture *fixture, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GSocketListener) probe = g_socket_listener_new();
	gboolean started;
	guint port;
	/* Find an available port: concurrent PID namespaces have equal PIDs,
	 * and GTest resets its random stream before each case. */
	port = g_socket_listener_add_any_inet_port(probe, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(port, >, 0);
	g_clear_object(&probe);

	fixture->state_dir = g_dir_make_tmp("venture-activities-XXXXXX", NULL);
	fixture->url = g_strdup_printf("http://127.0.0.1:%u", port);
	fixture->config = venture_config_new();
	g_object_set(fixture->config, "state-dir", fixture->state_dir,
		"server-bind-address", "127.0.0.1", "server-port", (gint64)port,
		"security-require-auth", data != NULL, NULL);
	fixture->database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	fixture->context = venture_context_new(fixture->config, fixture->database);
	g_assert_true(venture_database_migrate(fixture->database, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	fixture->server = venture_web_server_new(fixture->context, &error);
	g_assert_no_error(error);
	started = venture_web_server_start(fixture->server, &error);
	g_assert_no_error(error);
	g_assert_true(started);
	fixture->session = soup_session_new();
}

static void
fixture_tear_down(Fixture *fixture, gconstpointer data)
{
	venture_web_server_stop(fixture->server);
	g_clear_object(&fixture->session);
	g_clear_object(&fixture->server);
	g_clear_object(&fixture->context);
	g_clear_object(&fixture->database);
	g_clear_object(&fixture->config);
	venture_test_remove_tree(fixture->state_dir);
	g_free(fixture->state_dir);
	g_free(fixture->url);
}

typedef struct
{
	gboolean done;
	GBytes *body;
	GError *error;
} Request;

static void
request_done(GObject *source, GAsyncResult *result, gpointer data)
{
	Request *request = data;
	request->body = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &request->error);
	request->done = TRUE;
}

static guint
request(Fixture *fixture, const gchar *method, const gchar *path, const gchar *mime, const gchar *body, gchar **response)
{
	g_autofree gchar *url = g_strconcat(fixture->url, path, NULL);
	g_autoptr(SoupMessage) message = soup_message_new(method, url);
	Request result = { FALSE, NULL, NULL };
	guint status;

	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	if (NULL != body)
	{
		g_autoptr(GBytes) bytes = g_bytes_new(body, strlen(body));
		soup_message_set_request_body_from_bytes(message, mime, bytes);
	}
	soup_session_send_and_read_async(fixture->session, message, G_PRIORITY_DEFAULT, NULL, request_done, &result);
	while (!result.done)
		g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(result.error);
	*response = g_strndup(g_bytes_get_data(result.body, NULL), g_bytes_get_size(result.body));
	status = soup_message_get_status(message);
	g_bytes_unref(result.body);
	return status;
}



/* Planned work must be discoverable by every generated surface. */
static void
test_records(void)
{
	VentureEntityRegistry *registry = venture_entity_registry_get_default();
	g_autoptr(VentureEntity) activity = NULL;
	g_autoptr(VentureEntity) activity_type = NULL;
	g_autoptr(GError) error = NULL;
	GType type = venture_entity_registry_lookup(registry, "activity");
	const gchar *fields[] = { "organization-id", "kind", "subject", "body",
		"owner", "due-at", "starts-at", "ends-at", "priority", "status",
		"completed-at", "outcome", "related-type", "related-id", "contact-id",
		"company-id", "deal-id", "remind-at", "recurrence" };
	guint i;

	g_assert_cmpuint(type, !=, G_TYPE_INVALID);
	activity = g_object_new(type, NULL);
	for (i = 0; i < G_N_ELEMENTS(fields); i++)
		g_assert_nonnull(g_object_class_find_property(G_OBJECT_GET_CLASS(activity), fields[i]));
	g_assert_true(venture_entity_set_field_from_string(activity, "kind", "meeting", &error));
	g_assert_true(venture_entity_set_field_from_string(activity, "status", "planned", &error));
	g_assert_true(venture_entity_set_field_from_string(activity, "recurrence", "monthly", &error));
	g_assert_no_error(error);
	type = venture_entity_registry_lookup(registry, "activity_type");
	g_assert_cmpuint(type, !=, G_TYPE_INVALID);
	activity_type = g_object_new(type, NULL);
	g_assert_nonnull(g_object_class_find_property(G_OBJECT_GET_CLASS(activity_type), "default-duration"));
	g_assert_nonnull(g_object_class_find_property(G_OBJECT_GET_CLASS(activity_type), "active"));
}

static VentureEntity *
planned(Fixture *f, const gchar *subject)
{
	VentureEntity *row = VENTURE_ENTITY(venture_activity_new());
	g_autoptr(GDateTime) due = venture_time_from_string("2026-09-13T10:00:00Z", NULL);
	g_autoptr(GError) error = NULL;
	g_object_set(row, "subject", subject, "owner", "local", "due-at", due, NULL);
	venture_entity_set_organization_id(row, 1);
	g_assert_true(venture_database_save(f->database, row, NULL, &error));
	g_assert_no_error(error);
	return row;
}

static guint
action(Fixture *f, VentureEntity *row, const gchar *verb, const gchar *json, gchar **body)
{
	g_autofree gchar *path = g_strdup_printf("/api/v1/activities/%" G_GINT64_FORMAT "/%s",
		venture_entity_get_id(row), verb);
	return request(f, "POST", path, "application/json", json, body);
}

static gint64
count_rows(Fixture *f, GType type)
{
	g_autoptr(VentureQuery) q = venture_query_new(type);
	venture_query_set_organization(q, 1);
	return venture_database_count(f->database, q, NULL);
}

static void
test_complete(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) row = planned(f, "Discovery call");
	g_autoptr(VentureCompany) company = venture_company_new();
	g_autoptr(VentureEntity) stored = NULL;
	g_autoptr(VentureQuery) q = NULL;
	g_autoptr(VentureEntity) history = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *outcome = NULL;
	gint status;
	g_object_set(company, "name", "Customer", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(company), 1);
	g_assert_true(venture_database_save(f->database, VENTURE_ENTITY(company), NULL, NULL));
	g_object_set(row, "company-id", venture_entity_get_id(VENTURE_ENTITY(company)),
		"recurrence", VENTURE_ACTIVITY_RECURRENCE_WEEKLY, NULL);
	g_assert_true(venture_database_save(f->database, row, NULL, NULL));
	g_assert_cmpuint(action(f, row, "complete", "{\"outcome\":\"Agreed proposal\"}", &body), ==, 200);
	stored = venture_database_get(f->database, VENTURE_TYPE_ACTIVITY, venture_entity_get_id(row), NULL);
	g_object_get(stored, "status", &status, "outcome", &outcome, NULL);
	g_assert_cmpint(status, ==, VENTURE_ACTIVITY_STATUS_DONE);
	g_assert_cmpstr(outcome, ==, "Agreed proposal");
	g_assert_cmpint(count_rows(f, VENTURE_TYPE_ACTIVITY), ==, 2);
	g_assert_cmpint(count_rows(f, VENTURE_TYPE_INTERACTION), ==, 1);
	q = venture_query_new(VENTURE_TYPE_INTERACTION);
	history = venture_database_find_one(f->database, q, NULL);
	g_clear_pointer(&outcome, g_free);
	g_object_get(history, "body", &outcome, NULL);
	g_assert_cmpstr(outcome, ==, "Agreed proposal");
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(action(f, stored, "complete", "{}", &body), ==, 409);
	g_assert_cmpint(count_rows(f, VENTURE_TYPE_ACTIVITY), ==, 2);
}

static void
test_bypass(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) row = planned(f, "Cannot skip history");
	g_autoptr(GError) error = NULL;
	g_object_set(row, "status", VENTURE_ACTIVITY_STATUS_DONE, NULL);
	g_assert_false(venture_database_save(f->database, row, NULL, &error));
	g_assert_nonnull(strstr(error->message, "VentureActivityService"));
}

static void
test_actions(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) row = planned(f, "Move the call");
	g_autofree gchar *body = NULL;
	g_assert_cmpuint(action(f, row, "snooze", "{\"until\":\"2026-10-01T10:00:00Z\"}", &body), ==, 200);
	g_assert_nonnull(strstr(body, "2026-10-01"));
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(action(f, row, "snooze", "{\"until\":\"nonsense\"}", &body), ==, 422);
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(action(f, row, "reassign", "{\"owner\":\"other\"}", &body), ==, 200);
	g_assert_nonnull(strstr(body, "other"));
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(action(f, row, "cancel", "{}", &body), ==, 200);
	g_assert_nonnull(strstr(body, "cancelled"));
}

static void
test_reminders(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) row = planned(f, "Call reminder");
	g_autoptr(VentureUser) user = venture_user_new();
	g_autoptr(GDateTime) past = venture_time_from_string("2020-01-01", NULL);
	g_autofree gchar *body = NULL;
	g_object_set(user, "username", "local", "active", TRUE, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(user), 1);
	g_assert_true(venture_database_save(f->database, VENTURE_ENTITY(user), NULL, NULL));
	g_object_set(row, "remind-at", past, NULL);
	g_assert_true(venture_database_save(f->database, row, NULL, NULL));
	g_assert_cmpuint(request(f, "POST", "/api/v1/activities/sweep", "application/json", "{}", &body), ==, 200);
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(request(f, "POST", "/api/v1/activities/sweep", "application/json", "{}", &body), ==, 200);
	g_assert_cmpint(count_rows(f, VENTURE_TYPE_NOTIFICATION), ==, 1);
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_NOTIFICATION);
		g_autoptr(VentureEntity) notice = venture_database_find_one(f->database, query, NULL);
		gint kind;
		g_object_get(notice, "kind", &kind, NULL);
		g_assert_cmpint(kind, ==, VENTURE_NOTIFICATION_KIND_SYSTEM);
	}

}

static void
test_calendar(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) row = planned(f, "Call, then; confirm\nNext");
	g_autofree gchar *body = NULL;
	g_object_set(row, "kind", VENTURE_ACTIVITY_KIND_CALL, NULL);
	g_assert_true(venture_database_save(f->database, row, NULL, NULL));
	g_assert_cmpuint(request(f, "GET", "/api/v1/activities.ics?owner=local", NULL, NULL, &body), ==, 200);
	g_assert_nonnull(strstr(body, "BEGIN:VCALENDAR\r\n"));
	g_assert_nonnull(strstr(body, "DTSTART:20260913T100000Z\r\n"));
	g_assert_nonnull(strstr(body, "SUMMARY:Call\\, then\\; confirm\\nNext\r\n"));
	g_assert_nonnull(strstr(body, "UID:"));
	g_assert_nonnull(strstr(body, "DTSTAMP:"));
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(request(f, "GET", "/api/v1/activities.ics?owner=other", NULL, NULL, &body), ==, 200);
	g_assert_null(strstr(body, "BEGIN:VEVENT"));
}

static void
test_worklist(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) row = planned(f, "My overdue action");
	g_autoptr(GDateTime) past = venture_time_from_string("2020-01-01", NULL);
	g_autofree gchar *body = NULL;
	g_object_set(row, "due-at", past, NULL);
	g_assert_true(venture_database_save(f->database, row, NULL, NULL));
	g_assert_cmpuint(request(f, "GET", "/worklist", NULL, NULL, &body), ==, 200);
	g_assert_nonnull(strstr(body, "My day"));
	g_assert_nonnull(strstr(body, "My overdue action"));
	g_assert_nonnull(strstr(body, "Done"));
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(request(f, "GET", "/api/v1/reports/worklist", NULL, NULL, &body), ==, 200);
	g_assert_nonnull(strstr(body, "activities_overdue"));
	g_assert_nonnull(strstr(body, "activities_today"));
}

static void
test_related(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureCompany) company = venture_company_new();
	g_autofree gchar *body = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *link = NULL;
	g_object_set(company, "name", "Account", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(company), 1);
	g_assert_true(venture_database_save(f->database, VENTURE_ENTITY(company), NULL, NULL));
	path = g_strdup_printf("/e/company/%" G_GINT64_FORMAT, venture_entity_get_id(VENTURE_ENTITY(company)));
	link = g_strdup_printf("/e/activity/new?company_id=%" G_GINT64_FORMAT, venture_entity_get_id(VENTURE_ENTITY(company)));
	g_assert_cmpuint(request(f, "GET", path, NULL, NULL, &body), ==, 200);
	g_assert_nonnull(strstr(body, link));
	g_assert_nonnull(strstr(body, "New activity"));
}

static gboolean
reject_history(VentureDatabase *db, VentureEntity *row, VentureEntity *previous, gpointer data, GError **error)
{
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Injected interaction failure");
	return FALSE;
}

static void
test_rollback(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) row = planned(f, "Atomic completion");
	g_autoptr(VentureCompany) company = venture_company_new();
	g_autoptr(VentureEntity) stored = NULL;
	g_autofree gchar *body = NULL;
	gint status;
	gint64 audits;
	g_object_set(company, "name", "Account", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(company), 1);
	g_assert_true(venture_database_save(f->database, VENTURE_ENTITY(company), NULL, NULL));
	g_object_set(row, "company-id", venture_entity_get_id(VENTURE_ENTITY(company)), "recurrence", VENTURE_ACTIVITY_RECURRENCE_DAILY, NULL);
	g_assert_true(venture_database_save(f->database, row, NULL, NULL));
	audits = count_rows(f, VENTURE_TYPE_AUDIT_ENTRY);
	venture_database_add_save_validator(f->database, VENTURE_TYPE_INTERACTION, reject_history, NULL, NULL);
	g_assert_cmpuint(action(f, row, "complete", "{}", &body), ==, 422);
	g_assert_nonnull(strstr(body, "Injected interaction failure"));
	stored = venture_database_get(f->database, VENTURE_TYPE_ACTIVITY, venture_entity_get_id(row), NULL);
	g_object_get(stored, "status", &status, NULL);
	g_assert_cmpint(status, ==, VENTURE_ACTIVITY_STATUS_PLANNED);
	g_assert_cmpint(count_rows(f, VENTURE_TYPE_ACTIVITY), ==, 1);
	g_assert_cmpint(count_rows(f, VENTURE_TYPE_INTERACTION), ==, 0);
	g_assert_cmpint(count_rows(f, VENTURE_TYPE_AUDIT_ENTRY), ==, audits);
}

static void
test_staged(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) row = planned(f, "Approve completion");
	g_autofree gchar *body = NULL;
	g_autofree gchar *path = g_strdup_printf("/api/v1/activities/%" G_GINT64_FORMAT "/complete?stage=1", venture_entity_get_id(row));
	g_autoptr(GPtrArray) pending = NULL;
	g_autoptr(VentureEntity) stored = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *id = NULL;
	gint status;
	g_assert_cmpuint(request(f, "POST", path, "application/json", "{\"outcome\":\"Approved outcome\"}", &body), ==, 202);
	stored = venture_database_get(f->database, VENTURE_TYPE_ACTIVITY, venture_entity_get_id(row), NULL);
	g_object_get(stored, "status", &status, NULL);
	g_assert_cmpint(status, ==, VENTURE_ACTIVITY_STATUS_PLANNED);
	pending = venture_confirmation_store_list_pending(venture_context_get_confirmations(f->context));
	g_assert_cmpuint(pending->len, ==, 1);
	id = g_strdup(venture_confirmation_get_id(g_ptr_array_index(pending, 0)));
	g_assert_true(venture_confirmation_store_approve(venture_context_get_confirmations(f->context), id, "local", &error));
	g_assert_no_error(error);
	g_clear_object(&stored);
	stored = venture_database_get(f->database, VENTURE_TYPE_ACTIVITY, venture_entity_get_id(row), NULL);
	g_object_get(stored, "status", &status, NULL);
	g_assert_cmpint(status, ==, VENTURE_ACTIVITY_STATUS_DONE);
}

typedef struct
{
	gboolean done;
	gchar *out;
	gchar *err;
	GError *error;
} CliResult;

static void
cli_done(GObject *source, GAsyncResult *result, gpointer data)
{
	CliResult *outcome = data;
	g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &outcome->out, &outcome->err, &outcome->error);
	outcome->done = TRUE;
}

static void
test_cli(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) row = planned(f, "CLI completion");
	g_autoptr(GSubprocess) child = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, venture_entity_get_id(row));
	CliResult result = { FALSE, NULL, NULL, NULL };
	child = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE, &error,
		"build/debug/venturectl", "--server", f->url, "activity", "complete", id, "outcome=CLI outcome", NULL);
	g_assert_no_error(error);
	g_subprocess_communicate_utf8_async(child, NULL, NULL, cli_done, &result);
	while (!result.done)
		g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(result.error);
	g_test_message("CLI stderr: %s", result.err);
	g_assert_true(g_subprocess_get_successful(child));
	g_assert_nonnull(strstr(result.out, "done"));
	g_assert_nonnull(strstr(result.out, "CLI outcome"));
	g_free(result.out);
	g_free(result.err);
}

static void
test_docs(void)
{
	g_autofree gchar *text = NULL;
	g_assert_true(g_file_get_contents("docs/activities.org", &text, NULL, NULL));
	g_assert_nonnull(strstr(text, "VentureActivityService"));
	g_assert_nonnull(strstr(text, "activities.ics"));
}

static void
test_auth(Fixture *f, gconstpointer data)
{
	g_autofree gchar *body = NULL;
	g_assert_cmpuint(request(f, "GET", "/worklist", NULL, NULL, &body), ==, 302);
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(request(f, "GET", "/api/v1/activities.ics", NULL, NULL, &body), ==, 401);
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(request(f, "POST", "/api/v1/activities/1/complete", "application/json", "{}", &body), ==, 401);
	g_clear_pointer(&body, g_free);
}

static void
test_disabled(Fixture *f, gconstpointer data)
{
	g_autofree gchar *body = NULL;
	venture_config_set_module_enabled(f->config, "activities", FALSE);
	g_assert_cmpuint(request(f, "GET", "/worklist", NULL, NULL, &body), ==, 404);
	g_assert_cmpuint(venture_entity_registry_lookup(venture_entity_registry_get_default(), "activity"), ==, G_TYPE_INVALID);
	venture_config_set_module_enabled(f->config, "activities", TRUE);
}

static void
test_migration(Fixture *f, gconstpointer data)
{
	g_autoptr(OrmResult) result = venture_database_query_raw(f->database,
		"SELECT CAST(COUNT(*) AS BIGINT) FROM sqlite_master WHERE type='index' AND name='activities_notification_target'", NULL, NULL);
	g_assert_nonnull(result);
	g_assert_true(orm_result_next(result));
	g_assert_cmpint(orm_row_get_integer(orm_result_get_row(result), 0), ==, 1);
}

static GError *
completion_veto(VentureActivityService *service, VentureEntity *row, gpointer data)
{
	return g_error_new_literal(VENTURE_ERROR, VENTURE_ERROR_CONFLICT, "Completion veto");
}

static void
test_veto(Fixture *f, gconstpointer data)
{
	VentureActivityService *service = venture_database_get_activity_service(f->database);
	g_autoptr(VentureEntity) row = planned(f, "Veto completion");
	g_autoptr(VentureEntity) done = NULL;
	g_autoptr(GError) error = NULL;
	g_assert_cmpuint(g_signal_lookup("completing", G_OBJECT_TYPE(service)), !=, 0);
	g_signal_connect(service, "completing", G_CALLBACK(completion_veto), NULL);
	done = venture_activity_service_complete(service, row, "Outcome", NULL, &error);
	g_assert_null(done);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	g_assert_cmpstr(error->message, ==, "Completion veto");
}

static void
test_upgrade_restart(void)
{
	g_autofree gchar *directory = g_dir_make_tmp("venture-activities-upgrade-XXXXXX", NULL);
	g_autofree gchar *uri = g_strdup_printf("sqlite://%s/database.db", directory);
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureDatabase) db = NULL;
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(VentureCompany) company = venture_company_new();
	g_autoptr(GError) error = NULL;
	guint run;
	gint64 id;
	venture_config_set_module_enabled(config, "activities", FALSE);
	db = venture_database_new(uri, &error);
	g_assert_no_error(error);
	context = venture_context_new(config, db);
	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	venture_entity_set_organization_id(VENTURE_ENTITY(company), 1);
	g_object_set(company, "name", "Existing customer", NULL);
	g_assert_true(venture_database_save(db, VENTURE_ENTITY(company), NULL, &error));
	id = venture_entity_get_id(VENTURE_ENTITY(company));
	/* Recreate the previous migration history with real CRM data, and no
	 * activity table because the module has always been disabled. */
	g_assert_true(venture_database_execute(db, "DELETE FROM schema_migrations WHERE version=120; DROP INDEX activities_notification_target", NULL, &error));
	g_clear_object(&context);
	g_clear_object(&db);
	for (run = 0; run < 2; run++)
	{
		g_autoptr(VentureEntity) existing = NULL;
		g_autoptr(OrmResult) result = NULL;
		g_autofree gchar *name = NULL;
		if (run == 1)
			venture_config_set_module_enabled(config, "activities", TRUE);
		db = venture_database_new(uri, &error);
		context = venture_context_new(config, db);
		g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error));
		g_assert_no_error(error);
		existing = venture_database_get(db, VENTURE_TYPE_COMPANY, id, &error);
		g_assert_no_error(error);
		g_object_get(existing, "name", &name, NULL);
		g_assert_cmpstr(name, ==, "Existing customer");
		result = venture_database_query_raw(db, "SELECT CAST(COUNT(*) AS BIGINT) FROM schema_migrations WHERE version=120", NULL, &error);
		g_assert_no_error(error);
		g_assert_true(orm_result_next(result));
		g_assert_cmpint(orm_row_get_integer(orm_result_get_row(result), 0), ==, 1);
		g_clear_object(&result);
		g_clear_object(&context);
		g_clear_object(&db);
	}
	venture_test_remove_tree(directory);
}

static void
test_widget_sources(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) row = planned(f, "Next call");
	g_autoptr(VentureDashboardWidget) widget = venture_dashboard_widget_new();
	g_autoptr(VentureWidgetResult) result = NULL;
	g_autoptr(GDateTime) due = venture_time_now();
	gint64 organization = 1;
	VentureWidgetScope scope = { &organization, 1, 1, "local", 0 };
	g_object_set(row, "due-at", due, NULL);
	g_assert_true(venture_database_save(f->database, row, NULL, NULL));
	g_object_set(widget, "kind", "count", "entity-type", "activity", "filter", "status=planned&owner={me}", NULL);
	result = venture_dashboard_render_widget(f->context, widget, &scope);
	g_assert_null(result->error);
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(result->data), "count"), ==, 1);
	g_clear_pointer(&result, venture_widget_result_free);
	g_object_set(widget, "kind", "upcoming", "field", "due_at", "options", "{\"days\":14}", NULL);
	result = venture_dashboard_render_widget(f->context, widget, &scope);
	g_assert_null(result->error);
	g_assert_nonnull(strstr(result->html, "Next call"));
	g_clear_pointer(&result, venture_widget_result_free);
	g_object_set(widget, "kind", "metric", "report-name", "worklist", "field", "activities_today", NULL);
	result = venture_dashboard_render_widget(f->context, widget, &scope);
	g_assert_null(result->error);
	g_assert_nonnull(strstr(result->html, "1"));
}

static void
test_reference_retention(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) row = planned(f, "Keep old relation");
	g_autoptr(VentureCompany) company = venture_company_new();
	g_autoptr(GError) error = NULL;
	g_object_set(company, "name", "Former account", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(company), 1);
	g_assert_true(venture_database_save(f->database, VENTURE_ENTITY(company), NULL, NULL));
	g_object_set(row, "company-id", venture_entity_get_id(VENTURE_ENTITY(company)), NULL);
	g_assert_true(venture_database_save(f->database, row, NULL, NULL));
	g_assert_true(venture_database_delete(f->database, VENTURE_ENTITY(company), NULL, NULL));
	g_object_set(row, "subject", "Keep historical reference editable", NULL);
	g_assert_true(venture_database_save(f->database, row, NULL, &error));
	g_assert_no_error(error);
}

static void
test_recurrence_dates(Fixture *f, gconstpointer data)
{
	const gchar *expected[] = { "2026-02-01", "2026-02-07", "2026-02-28" };
	guint r;
	for (r = 0; r < 3; r++)
	{
		g_autoptr(VentureEntity) row = planned(f, "Repeat");
		g_autoptr(VentureEntity) done = NULL;
		g_autoptr(GDateTime) original = venture_time_from_string("2026-01-31T10:00:00Z", NULL);
		g_autoptr(GDateTime) expected_date = venture_time_from_string(expected[r], NULL);
		g_autoptr(GDateTime) due = NULL;
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACTIVITY);
		g_autoptr(VentureEntity) next = NULL;
		g_autoptr(GError) error = NULL;
		g_object_set(row, "recurrence", (gint)r + 1, "due-at", original, NULL);
		g_assert_true(venture_database_save(f->database, row, NULL, NULL));
		done = venture_activity_service_complete(venture_database_get_activity_service(f->database), row, "Repeated", NULL, &error);
		g_assert_no_error(error);
		g_assert_nonnull(done);
		venture_query_set_organization(query, 1);
		venture_query_add_filter_int(query, "recurrence", VENTURE_FILTER_OP_EQ, r + 1, NULL);
		venture_query_add_filter_string(query, "status", VENTURE_FILTER_OP_EQ, "planned", NULL);
		next = venture_database_find_one(f->database, query, &error);
		g_assert_no_error(error);
		g_assert_nonnull(next);
		g_object_get(next, "due-at", &due, NULL);
		g_assert_cmpint(g_date_time_get_year(due), ==, g_date_time_get_year(expected_date));
		g_assert_cmpint(g_date_time_get_month(due), ==, g_date_time_get_month(expected_date));
		g_assert_cmpint(g_date_time_get_day_of_month(due), ==, g_date_time_get_day_of_month(expected_date));
		g_assert_cmpint(g_date_time_get_hour(due), ==, 10);
	}
}

static void
test_assistant_staged(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureAiService) ai = NULL;
	g_autoptr(JsonNode) tools = NULL;
	g_autofree gchar *text = NULL;
	g_autoptr(VentureEntity) row = planned(f, "Assistant plan");
	g_autoptr(GError) error = NULL;
	VentureConfirmation *confirmation;
	VentureActor actor;
	g_object_set(f->config, "ai-enabled", TRUE, "ai-provider", "ollama", NULL);
	ai = venture_ai_service_new(f->context, &error);
	g_assert_no_error(error);
	g_assert_nonnull(ai);
	tools = venture_ai_service_describe_tools(ai);
	text = venture_json_to_string(tools, FALSE);
	g_assert_nonnull(strstr(text, "venture_activity_complete"));
	actor.kind = VENTURE_ACTOR_KIND_AI;
	actor.name = "assistant";
	actor.prompt = "Complete the call";
	actor.request_id = NULL;
	actor.approved_by = NULL;
	confirmation = venture_confirmation_store_stage_activity_complete(venture_context_get_confirmations(f->context),
		row, "Assistant outcome", &actor, "assistant", &error);
	g_assert_no_error(error);
	g_assert_nonnull(confirmation);
	g_assert_true(venture_confirmation_store_approve(venture_context_get_confirmations(f->context),
		venture_confirmation_get_id(confirmation), "local", &error));
	g_assert_no_error(error);
}

static void
test_stale_approval(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) row = planned(f, "Frozen approval version");
	g_autoptr(GError) error = NULL;
	g_autofree gchar *id = NULL;
	VentureActor actor;
	VentureConfirmation *confirmation;
	actor.kind = VENTURE_ACTOR_KIND_AI;
	actor.name = "assistant";
	actor.prompt = NULL;
	actor.request_id = NULL;
	actor.approved_by = NULL;
	confirmation = venture_confirmation_store_stage_activity_complete(venture_context_get_confirmations(f->context), row, "Done", &actor, "assistant", &error);
	g_assert_no_error(error);
	id = g_strdup(venture_confirmation_get_id(confirmation));
	g_object_set(row, "subject", "Somebody changed this", NULL);
	g_assert_true(venture_database_save(f->database, row, NULL, NULL));
	g_assert_false(venture_confirmation_store_approve(venture_context_get_confirmations(f->context), id, "local", &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
}

static void
test_polymorphic_deleted(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureCompany) company = venture_company_new();
	g_autoptr(VentureEntity) row = planned(f, "New relation to deleted account");
	g_autoptr(GError) error = NULL;
	g_object_set(company, "name", "Former account", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(company), 1);
	g_assert_true(venture_database_save(f->database, VENTURE_ENTITY(company), NULL, NULL));
	g_assert_true(venture_database_delete(f->database, VENTURE_ENTITY(company), NULL, NULL));
	g_object_set(row, "related-type", "company", "related-id", venture_entity_get_id(VENTURE_ENTITY(company)), NULL);
	g_assert_false(venture_database_save(f->database, row, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

static void
test_deleted_action(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) row = planned(f, "Deleted activity");
	g_autoptr(VentureEntity) done = NULL;
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_database_delete(f->database, row, NULL, NULL));
	done = venture_activity_service_complete(venture_database_get_activity_service(f->database), row, "Done", NULL, &error);
	g_assert_null(done);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
}

static void
test_organization_and_dates(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureOrganization) organization = venture_organization_new();
	g_autoptr(VentureCompany) company = venture_company_new();
	g_autoptr(VentureEntity) row = planned(f, "Scoped plan");
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) start = venture_time_from_string("2026-09-13T10:00:00Z", NULL);
	g_autoptr(GDateTime) end = venture_time_from_string("2026-09-13T09:00:00Z", NULL);
	g_object_set(organization, "name", "Other books", "slug", "other-books", NULL);
	g_assert_true(venture_database_save(f->database, VENTURE_ENTITY(organization), NULL, NULL));
	g_object_set(company, "name", "Other account", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(company), venture_entity_get_id(VENTURE_ENTITY(organization)));
	g_assert_true(venture_database_save(f->database, VENTURE_ENTITY(company), NULL, NULL));
	g_object_set(row, "company-id", venture_entity_get_id(VENTURE_ENTITY(company)), NULL);
	g_assert_false(venture_database_save(f->database, row, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	g_object_set(row, "company-id", (gint64)0, "related-type", "company", "related-id", venture_entity_get_id(VENTURE_ENTITY(company)), NULL);
	g_assert_false(venture_database_save(f->database, row, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	g_object_set(row, "related-type", NULL, "related-id", (gint64)0, "starts-at", start, "ends-at", end, NULL);
	g_assert_false(venture_database_save(f->database, row, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

static void
test_related_prefill_and_open(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureCompany) company = venture_company_new();
	g_autoptr(VentureEntity) open = planned(f, "Open next action");
	g_autoptr(VentureEntity) closed = planned(f, "Closed action");
	g_autoptr(VentureEntity) done = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *open_link = NULL;
	g_autofree gchar *closed_link = NULL;
	g_object_set(company, "name", "Prefilled account", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(company), 1);
	g_assert_true(venture_database_save(f->database, VENTURE_ENTITY(company), NULL, NULL));
	g_object_set(open, "company-id", venture_entity_get_id(VENTURE_ENTITY(company)), NULL);
	g_object_set(closed, "company-id", venture_entity_get_id(VENTURE_ENTITY(company)), NULL);
	g_assert_true(venture_database_save(f->database, open, NULL, NULL));
	g_assert_true(venture_database_save(f->database, closed, NULL, NULL));
	done = venture_activity_service_complete(venture_database_get_activity_service(f->database), closed, "Recorded", NULL, NULL);
	g_assert_nonnull(done);
	path = g_strdup_printf("/e/company/%" G_GINT64_FORMAT, venture_entity_get_id(VENTURE_ENTITY(company)));
	open_link = g_strdup_printf("href=\"/e/activity/%" G_GINT64_FORMAT "\"", venture_entity_get_id(open));
	closed_link = g_strdup_printf("href=\"/e/activity/%" G_GINT64_FORMAT "\"", venture_entity_get_id(closed));
	g_assert_cmpuint(request(f, "GET", path, NULL, NULL, &body), ==, 200);
	g_assert_nonnull(strstr(body, open_link));
	g_assert_null(strstr(body, closed_link));
	g_clear_pointer(&body, g_free);
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/e/activity/new?company_id=%" G_GINT64_FORMAT, venture_entity_get_id(VENTURE_ENTITY(company)));
	g_assert_cmpuint(request(f, "GET", path, NULL, NULL, &body), ==, 200);
	{
		const gchar *field = strstr(body, "<select name=\"company-id\">");
		const gchar *end;
		g_autofree gchar *options = NULL;
		g_autofree gchar *selected = g_strdup_printf("value=\"%" G_GINT64_FORMAT "\" selected", venture_entity_get_id(VENTURE_ENTITY(company)));
		g_assert_nonnull(field);
		end = strstr(field, "</select>");
		g_assert_nonnull(end);
		options = g_strndup(field, end - field);
		g_assert_nonnull(strstr(options, selected));
	}
}

static void
test_cli_queues(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) row = planned(f, "CLI queue item");
	g_autoptr(GDateTime) past = venture_time_from_string("2020-01-01", NULL);
	guint i;
	const gchar *queues[] = { "mine", "overdue", "today" };
	g_object_set(row, "due-at", past, NULL);
	g_assert_true(venture_database_save(f->database, row, NULL, NULL));
	for (i = 0; i < G_N_ELEMENTS(queues); i++)
	{
		g_autoptr(GSubprocess) child = NULL;
		g_autoptr(GError) error = NULL;
		CliResult result = { FALSE, NULL, NULL, NULL };
		child = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE, &error,
			"build/debug/venturectl", "--server", f->url, "activity", "list", queues[i], NULL);
		g_assert_no_error(error);
		g_subprocess_communicate_utf8_async(child, NULL, NULL, cli_done, &result);
		while (!result.done)
			g_main_context_iteration(NULL, TRUE);
		g_assert_no_error(result.error);
		g_assert_true(g_subprocess_get_successful(child));
		if (i < 2)
			g_assert_nonnull(strstr(result.out, "CLI queue item"));
		else
			g_assert_null(strstr(result.out, "CLI queue item"));
		g_free(result.out);
		g_free(result.err);
	}
}

typedef struct
{
	VentureActivityService *service;
	gboolean called;
	gint nested_result;
} ReminderProbe;

static void
reminder_reentry(VentureDatabase *db, VentureEntity *row, gboolean created, gpointer data)
{
	ReminderProbe *probe = data;
	g_autoptr(GError) error = NULL;
	if (VENTURE_IS_NOTIFICATION(row) && !probe->called)
	{
		probe->called = TRUE;
		probe->nested_result = venture_activity_service_sweep(probe->service, 1, 200, &error);
	}
}

static void
test_reminder_reentry(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) row = planned(f, "Reentrant reminder");
	g_autoptr(VentureUser) user = venture_user_new();
	g_autoptr(GDateTime) past = venture_time_from_string("2020-01-01", NULL);
	g_autoptr(GError) error = NULL;
	ReminderProbe probe = { NULL, FALSE, 0 };
	gulong handler;
	gint delivered;
	probe.service = venture_database_get_activity_service(f->database);
	g_object_set(user, "username", "local", "active", TRUE, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(user), 1);
	g_assert_true(venture_database_save(f->database, VENTURE_ENTITY(user), NULL, NULL));
	g_object_set(row, "remind-at", past, NULL);
	g_assert_true(venture_database_save(f->database, row, NULL, NULL));
	handler = g_signal_connect(f->database, "entity-saved", G_CALLBACK(reminder_reentry), &probe);
	delivered = venture_activity_service_sweep(probe.service, 1, 200, &error);
	g_signal_handler_disconnect(f->database, handler);
	g_assert_no_error(error);
	g_assert_cmpint(delivered, ==, 1);
	g_assert_true(probe.called);
	g_assert_cmpint(probe.nested_result, ==, -1);
	g_assert_cmpint(count_rows(f, VENTURE_TYPE_NOTIFICATION), ==, 1);
}

static void
test_monthly_meeting_duration(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) row = planned(f, "Month-end meeting");
	g_autoptr(VentureEntity) done = NULL;
	g_autoptr(VentureEntity) next = NULL;
	g_autoptr(GDateTime) starts = venture_time_from_string("2026-01-30T10:00:00Z", NULL);
	g_autoptr(GDateTime) ends = venture_time_from_string("2026-01-31T10:00:00Z", NULL);
	g_autoptr(GDateTime) next_starts = NULL;
	g_autoptr(GDateTime) next_ends = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACTIVITY);
	g_object_set(row, "kind", VENTURE_ACTIVITY_KIND_MEETING, "recurrence", VENTURE_ACTIVITY_RECURRENCE_MONTHLY,
		"starts-at", starts, "ends-at", ends, "due-at", starts, NULL);
	g_assert_true(venture_database_save(f->database, row, NULL, NULL));
	done = venture_activity_service_complete(venture_database_get_activity_service(f->database), row, "Met", NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(done);
	venture_query_set_organization(query, 1);
	venture_query_add_filter_string(query, "status", VENTURE_FILTER_OP_EQ, "planned", NULL);
	next = venture_database_find_one(f->database, query, &error);
	g_assert_no_error(error);
	g_object_get(next, "starts-at", &next_starts, "ends-at", &next_ends, NULL);
	g_assert_cmpint(g_date_time_get_month(next_starts), ==, 2);
	g_assert_cmpint(g_date_time_get_day_of_month(next_starts), ==, 28);
	g_assert_cmpint(g_date_time_difference(next_ends, next_starts), ==, g_date_time_difference(ends, starts));
}

/* A retained subject must not log completion against another company. */
static void
test_related_edit(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureCompany) company = venture_company_new();
	g_autoptr(VentureCompany) other = venture_company_new();
	g_autoptr(VentureEntity) row = planned(f, "Consistent subject");
	g_autoptr(GError) error = NULL;
	(void)data;
	g_object_set(company, "organization-id", (gint64)1, "name", "Original", NULL);
	g_object_set(other, "organization-id", (gint64)1, "name", "Other", NULL);
	g_assert_true(venture_database_save(f->database, VENTURE_ENTITY(company), NULL, &error));
	g_assert_true(venture_database_save(f->database, VENTURE_ENTITY(other), NULL, &error));
	g_object_set(row, "related-type", "company", "related-id", venture_entity_get_id(VENTURE_ENTITY(company)), NULL);
	g_assert_true(venture_database_save(f->database, row, NULL, &error));
	g_object_set(row, "company-id", venture_entity_get_id(VENTURE_ENTITY(other)), NULL);
	g_assert_false(venture_database_save(f->database, row, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/activities/records", test_records);
	g_test_add("/activities/complete", Fixture, NULL, fixture_set_up, test_complete, fixture_tear_down);
	g_test_add("/activities/bypass", Fixture, NULL, fixture_set_up, test_bypass, fixture_tear_down);
	g_test_add("/activities/actions", Fixture, NULL, fixture_set_up, test_actions, fixture_tear_down);
	g_test_add("/activities/reminders", Fixture, NULL, fixture_set_up, test_reminders, fixture_tear_down);
	g_test_add("/activities/calendar", Fixture, NULL, fixture_set_up, test_calendar, fixture_tear_down);
	g_test_add("/activities/worklist", Fixture, NULL, fixture_set_up, test_worklist, fixture_tear_down);
	g_test_add("/activities/related-edit", Fixture, NULL, fixture_set_up, test_related_edit, fixture_tear_down);
	g_test_add("/activities/related", Fixture, NULL, fixture_set_up, test_related, fixture_tear_down);
	g_test_add("/activities/rollback", Fixture, NULL, fixture_set_up, test_rollback, fixture_tear_down);
	g_test_add("/activities/staged", Fixture, NULL, fixture_set_up, test_staged, fixture_tear_down);
	g_test_add("/activities/cli", Fixture, NULL, fixture_set_up, test_cli, fixture_tear_down);
	g_test_add("/activities/auth", Fixture, GINT_TO_POINTER(1), fixture_set_up, test_auth, fixture_tear_down);
	g_test_add_func("/activities/docs", test_docs);
	g_test_add("/activities/migration", Fixture, NULL, fixture_set_up, test_migration, fixture_tear_down);
	g_test_add("/activities/disabled", Fixture, NULL, fixture_set_up, test_disabled, fixture_tear_down);
	g_test_add("/activities/veto", Fixture, NULL, fixture_set_up, test_veto, fixture_tear_down);
	g_test_add("/activities/widget_sources", Fixture, NULL, fixture_set_up, test_widget_sources, fixture_tear_down);
	g_test_add_func("/activities/upgrade_restart", test_upgrade_restart);
	g_test_add("/activities/reference_retention", Fixture, NULL, fixture_set_up, test_reference_retention, fixture_tear_down);
	g_test_add("/activities/recurrence_dates", Fixture, NULL, fixture_set_up, test_recurrence_dates, fixture_tear_down);
	g_test_add("/activities/assistant_staged", Fixture, NULL, fixture_set_up, test_assistant_staged, fixture_tear_down);
	g_test_add("/activities/stale_approval", Fixture, NULL, fixture_set_up, test_stale_approval, fixture_tear_down);
	g_test_add("/activities/polymorphic_deleted", Fixture, NULL, fixture_set_up, test_polymorphic_deleted, fixture_tear_down);
	g_test_add("/activities/deleted_action", Fixture, NULL, fixture_set_up, test_deleted_action, fixture_tear_down);
	g_test_add("/activities/organization_and_dates", Fixture, NULL, fixture_set_up, test_organization_and_dates, fixture_tear_down);
	g_test_add("/activities/related_prefill_and_open", Fixture, NULL, fixture_set_up, test_related_prefill_and_open, fixture_tear_down);
	g_test_add("/activities/cli_queues", Fixture, NULL, fixture_set_up, test_cli_queues, fixture_tear_down);
	g_test_add("/activities/reminder_reentry", Fixture, NULL, fixture_set_up, test_reminder_reentry, fixture_tear_down);
	g_test_add("/activities/monthly_meeting_duration", Fixture, NULL, fixture_set_up, test_monthly_meeting_duration, fixture_tear_down);
	return g_test_run();
}
