/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <libsoup/soup.h>
#include "venture-test-util.h"

/* Every surface must discover the same five durable record types. */
static void
test_records(void)
{
	const gchar *names[] = { "sequence", "sequence_step", "sequence_enrollment",
		"sequence_delivery", "suppression" };
	guint i;
	VentureEntityRegistry *registry = venture_entity_registry_get_default();

	venture_entity_registry_register_builtins(registry);
	for (i = 0; i < G_N_ELEMENTS(names); i++)
		g_assert_cmpuint(venture_entity_registry_lookup(registry, names[i]), !=, G_TYPE_INVALID);
}

typedef struct
{
	VentureDatabase *db;
	VentureContext *context;
	VentureConfig *config;
	VentureEntity *sequence;
	VentureEntity *contact;
	VentureEntity *step;
	gchar *directory;
	gchar *uri;
} Fixture;

static void
save(Fixture *f, VentureEntity *row)
{
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_database_save(f->db, row, NULL, &error));
	g_assert_no_error(error);
}

static void
setup(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	(void)unused;
	f->config = venture_config_new();
	if (unused != NULL)
	{
		f->directory = g_dir_make_tmp("venture-sequence-restart-XXXXXX", NULL);
		f->uri = g_strdup_printf("sqlite://%s/data.db", f->directory);
	}
	f->db = venture_database_new(f->uri != NULL ? f->uri : "sqlite://:memory:", &error);
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->contact = g_object_new(VENTURE_TYPE_CONTACT, "organization-id", (gint64)1,
		"name", "Ada", "email", "Ada@Example.com", NULL);
	save(f, f->contact);
	f->sequence = g_object_new(VENTURE_TYPE_SEQUENCE, "organization-id", (gint64)1,
		"name", "Welcome", "active", TRUE, "timezone", "America/New_York",
		"send-window-start", (gint64)9, "send-window-end", (gint64)17,
		"weekdays", "1,2,3,4,5", "exit-on-reply", TRUE,
		"exit-on-unsubscribe", TRUE, "exit-on-deal-won", TRUE, NULL);
	save(f, f->sequence);
	f->step = g_object_new(VENTURE_TYPE_SEQUENCE_STEP, "organization-id", (gint64)1,
		"sequence-id", venture_entity_get_id(f->sequence), "position", (gint64)10,
		"active", TRUE, "subject", "Hello {contact.name}", "body", "For {contact.email}", NULL);
	save(f, f->step);
}

static void
teardown(Fixture *f, gconstpointer unused)
{
	(void)unused;
	g_clear_object(&f->step);
	g_clear_object(&f->contact);
	g_clear_object(&f->sequence);
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
	if (f->directory != NULL)
		venture_test_remove_tree(f->directory);
	g_free(f->directory);
	g_free(f->uri);
}

static VentureEntity *
enrollment(Fixture *f)
{
	g_autoptr(GDateTime) at = g_date_time_new_from_iso8601("2026-09-11T22:00:00Z", NULL);
	return g_object_new(VENTURE_TYPE_SEQUENCE_ENROLLMENT, "organization-id", (gint64)1,
		"sequence-id", venture_entity_get_id(f->sequence),
		"contact-id", venture_entity_get_id(f->contact), "enrolled-at", at,
		"enrollment-reason", "Requested information", NULL);
}

/* Friday after closing must become Monday 09:00 in the chosen timezone. */
static void
test_enroll_window(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) row = enrollment(f);
	g_autoptr(GDateTime) next = NULL;
	g_autofree gchar *text = NULL;
	gint64 position;
	(void)unused;
	save(f, row);
	g_object_get(row, "next-run-at", &next, "current-step", &position, NULL);
	g_assert_nonnull(next);
	text = g_date_time_format_iso8601(next);
	g_assert_cmpstr(text, ==, "2026-09-14T13:00:00Z");
	g_assert_cmpint(position, ==, 10);
}

static void
test_duplicate(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) first = enrollment(f);
	g_autoptr(VentureEntity) second = enrollment(f);
	g_autoptr(GError) error = NULL;
	(void)unused;
	save(f, first);
	g_assert_false(venture_database_save(f->db, second, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_ALREADY_EXISTS);
}

static void
test_suppression(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) first = enrollment(f);
	g_autoptr(VentureEntity) suppression = NULL;
	g_autoptr(VentureEntity) stored = NULL;
	g_autoptr(VentureEntity) second = NULL;
	g_autofree gchar *email = NULL;
	g_autofree gchar *reason = NULL;
	g_autoptr(GError) error = NULL;
	gint status;
	(void)unused;
	save(f, first);
	suppression = g_object_new(VENTURE_TYPE_SUPPRESSION, "organization-id", (gint64)1,
		"email", " ADA@EXAMPLE.COM ", NULL);
	save(f, suppression);
	g_object_get(suppression, "email", &email, NULL);
	g_assert_cmpstr(email, ==, "ada@example.com");
	stored = venture_database_get(f->db, VENTURE_TYPE_SEQUENCE_ENROLLMENT,
		venture_entity_get_id(first), &error);
	g_assert_no_error(error);
	g_object_get(stored, "status", &status, "exit-reason", &reason, NULL);
	g_assert_cmpint(status, ==, 3);
	g_assert_nonnull(reason);
	second = enrollment(f);
	g_assert_false(venture_database_save(f->db, second, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

static void
test_reply(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) row = enrollment(f);
	g_autoptr(VentureEntity) interaction = NULL;
	g_autoptr(VentureEntity) stored = NULL;
	g_autoptr(VentureEntity) lead = NULL;
	gint status;
	if (unused != NULL)
	{
		lead = g_object_new(VENTURE_TYPE_LEAD, "organization-id", (gint64)1, "name", "Inquiry", NULL);
		save(f, lead);
	}
	save(f, row);
	interaction = g_object_new(VENTURE_TYPE_INTERACTION, "organization-id", (gint64)1,
		"contact-id", venture_entity_get_id(f->contact), "subject", "Reply", "outbound", FALSE, NULL);
	/* Lead history and sequence exits must compose in one interaction save. */
	if (lead != NULL)
		g_object_set(interaction, "lead-id", venture_entity_get_id(lead), NULL);
	save(f, interaction);
	stored = venture_database_get(f->db, VENTURE_TYPE_SEQUENCE_ENROLLMENT,
		venture_entity_get_id(row), NULL);
	g_object_get(stored, "status", &status, NULL);
	g_assert_cmpint(status, ==, 3);
	if (lead != NULL)
	{
		g_autoptr(VentureEntity) updated = venture_database_get(f->db, VENTURE_TYPE_LEAD, venture_entity_get_id(lead), NULL);
		g_autoptr(GDateTime) activity_at = NULL;
		g_object_get(updated, "last-activity-at", &activity_at, NULL);
		g_assert_nonnull(activity_at);
	}
}

static void
test_won(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) row = enrollment(f);
	g_autoptr(VentureEntity) deal = g_object_new(VENTURE_TYPE_DEAL,
		"organization-id", (gint64)1, "name", "Purchase",
		"contact-id", venture_entity_get_id(f->contact), NULL);
	g_autoptr(VentureEntity) stored = NULL;
	g_autoptr(VentureQuery) stages_query = venture_query_new(VENTURE_TYPE_PIPELINE_STAGE);
	g_autoptr(GPtrArray) stages = NULL;
	g_autoptr(VentureDeal) moved = NULL;
	g_autoptr(GError) error = NULL;
	gint64 pipeline_id;
	gint status;
	(void)unused;
	save(f, deal);
	g_object_set(row, "deal-id", venture_entity_get_id(deal), NULL);
	save(f, row);
	/* Winning through the pipeline service must atomically exit sequences;
	 * nested service wrappers must not consume the stage permit twice. */
	g_object_get(deal, "pipeline-id", &pipeline_id, NULL);
	g_assert_true(venture_query_add_filter_int(stages_query, "pipeline-id", VENTURE_FILTER_OP_EQ, pipeline_id, &error));
	g_assert_true(venture_query_add_filter_string(stages_query, "kind", VENTURE_FILTER_OP_EQ, "won", &error));
	stages = venture_database_find(f->db, stages_query, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(stages->len, ==, 1);
	moved = venture_deal_service_move_stage(venture_database_get_deal_service(f->db),
		VENTURE_DEAL(deal), venture_entity_get_id(g_ptr_array_index(stages, 0)),
		"Customer accepted", NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(moved);
	stored = venture_database_get(f->db, VENTURE_TYPE_SEQUENCE_ENROLLMENT,
		venture_entity_get_id(row), NULL);
	g_object_get(stored, "status", &status, NULL);
	g_assert_cmpint(status, ==, 3);
}

static gint
run(Fixture *f, const gchar *time, GError **error)
{
	g_autoptr(GDateTime) at = g_date_time_new_from_iso8601(time, NULL);
	return venture_sequence_service_run_due(venture_sequence_service_get(f->db), 1, at, NULL, error);
}

static GPtrArray *
deliveries(Fixture *f)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_SEQUENCE_DELIVERY);
	g_autoptr(GError) error = NULL;
	GPtrArray *found = venture_database_find(f->db, query, &error);
	g_assert_no_error(error);
	return found;
}

static void
test_sweep(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) row = enrollment(f);
	g_autoptr(GPtrArray) found = NULL;
	g_autoptr(VentureEntity) stored = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *subject = NULL;
	gint status, state;
	(void)unused;
	save(f, row);
	g_assert_cmpint(run(f, "2026-09-14T12:59:59Z", &error), ==, 0);
	g_assert_no_error(error);
	g_assert_cmpint(run(f, "2026-09-14T13:00:00Z", &error), ==, 1);
	g_assert_no_error(error);
	g_assert_cmpint(run(f, "2026-09-14T13:00:00Z", &error), ==, 0);
	g_assert_no_error(error);
	found = deliveries(f);
	g_assert_cmpuint(found->len, ==, 1);
	g_object_get(g_ptr_array_index(found, 0), "subject", &subject, "state", &state, NULL);
	g_assert_cmpstr(subject, ==, "Hello Ada");
	g_assert_cmpint(state, ==, 0);
	stored = venture_database_get(f->db, VENTURE_TYPE_SEQUENCE_ENROLLMENT, venture_entity_get_id(row), NULL);
	g_object_get(stored, "status", &status, NULL);
	g_assert_cmpint(status, ==, 2);
}

static void
test_edit(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) row = enrollment(f);
	g_autoptr(VentureEntity) later = g_object_new(VENTURE_TYPE_SEQUENCE_STEP,
		"organization-id", (gint64)1, "sequence-id", venture_entity_get_id(f->sequence),
		"position", (gint64)30, "delay-days", (gint64)1, "active", TRUE, "subject", "Second", NULL);
	g_autoptr(VentureEntity) inserted = NULL;
	g_autoptr(GPtrArray) found = NULL;
	g_autoptr(GError) error = NULL;
	guint i, skipped = 0;
	(void)unused;
	save(f, later);
	save(f, row);
	g_assert_cmpint(run(f, "2026-09-14T13:00:00Z", &error), ==, 1);
	g_assert_no_error(error);
	inserted = g_object_new(VENTURE_TYPE_SEQUENCE_STEP, "organization-id", (gint64)1,
		"sequence-id", venture_entity_get_id(f->sequence), "position", (gint64)20,
		"active", TRUE, "subject", "Too late", NULL);
	save(f, inserted);
	g_object_set(f->step, "position", (gint64)40, NULL);
	save(f, f->step);
	g_assert_cmpint(run(f, "2026-09-15T13:00:00Z", &error), ==, 1);
	g_assert_no_error(error);
	g_assert_cmpint(run(f, "2026-09-16T13:00:00Z", &error), ==, 0);
	g_assert_no_error(error);
	found = deliveries(f);
	g_assert_cmpuint(found->len, ==, 3);
	for (i = 0; i < found->len; i++)
	{
		gint state;
		g_autofree gchar *reason = NULL;
		g_object_get(g_ptr_array_index(found, i), "state", &state, "error", &reason, NULL);
		if (state == 3)
		{
			skipped++;
			g_assert_nonnull(reason);
		}
	}
	g_assert_cmpuint(skipped, ==, 1);
}

static gboolean
reject_completed(VentureDatabase *db, VentureEntity *row, VentureEntity *previous,
	gpointer data, GError **error)
{
	gint status;
	(void)db; (void)previous; (void)data;
	g_object_get(row, "status", &status, NULL);
	if (status != 2)
		return TRUE;
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_DATABASE, "Injected final write failure");
	return FALSE;
}

static void
test_atomic(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) row = enrollment(f);
	g_autoptr(GPtrArray) found = NULL;
	g_autoptr(GError) error = NULL;
	(void)unused;
	save(f, row);
	venture_database_add_save_validator(f->db, VENTURE_TYPE_SEQUENCE_ENROLLMENT,
		reject_completed, NULL, NULL);
	g_assert_cmpint(run(f, "2026-09-14T13:00:00Z", &error), ==, -1);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_DATABASE);
	found = deliveries(f);
	g_assert_cmpuint(found->len, ==, 0);
}

static void
test_manual(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) row = enrollment(f);
	g_autoptr(GError) error = NULL;
	VentureSequenceService *service = venture_sequence_service_get(f->db);
	(void)unused;
	save(f, row);
	g_assert_true(venture_sequence_service_transition(service, 1, venture_entity_get_id(row), "pause", NULL, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpint(run(f, "2026-09-14T13:00:00Z", &error), ==, 0);
	g_assert_no_error(error);
	g_assert_true(venture_sequence_service_transition(service, 1, venture_entity_get_id(row), "resume", NULL, NULL, &error));
	g_assert_no_error(error);
	g_assert_true(venture_sequence_service_transition(service, 1, venture_entity_get_id(row), "exit", "Operator request", NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpint(run(f, "2026-09-14T13:00:00Z", &error), ==, 0);
	g_assert_no_error(error);
	g_assert_false(venture_sequence_service_transition(service, 1, venture_entity_get_id(row), "resume", NULL, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
}

static void
test_render_failure(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) row = enrollment(f);
	g_autoptr(GPtrArray) found = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *reason = NULL;
	gint state;
	(void)unused;
	g_object_set(f->step, "body", "Hello {contact.nonexistent}", NULL);
	save(f, f->step);
	save(f, row);
	g_assert_cmpint(run(f, "2026-09-14T13:00:00Z", &error), ==, 1);
	g_assert_no_error(error);
	found = deliveries(f);
	g_assert_cmpuint(found->len, ==, 1);
	g_object_get(g_ptr_array_index(found, 0), "state", &state, "error", &reason, NULL);
	g_assert_cmpint(state, ==, 2);
	g_assert_nonnull(reason);
}

static void
test_reports(Fixture *f, gconstpointer unused)
{
	VentureReportRegistry *registry = venture_context_get_report_registry(f->context);
	g_autoptr(VentureEntity) first = enrollment(f);
	g_autoptr(VentureEntity) second = NULL;
	g_autoptr(VentureEntity) interaction = NULL;
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(JsonNode) json = NULL;
	g_autoptr(GError) error = NULL;
	JsonObject *row;
	(void)unused;
	g_assert_nonnull(venture_report_registry_lookup(registry, "sequence_performance"));
	g_assert_nonnull(venture_report_registry_lookup(registry, "sequence_failures"));
	save(f, first);
	g_assert_cmpint(run(f, "2026-09-14T13:00:00Z", &error), ==, 1);
	g_assert_no_error(error);
	second = enrollment(f);
	g_object_set(f->sequence, "goal", 1, NULL);
	save(f, f->sequence);
	save(f, second);
	interaction = g_object_new(VENTURE_TYPE_INTERACTION, "organization-id", (gint64)1,
		"contact-id", venture_entity_get_id(f->contact), "subject", "Interested", "outbound", FALSE, NULL);
	save(f, interaction);
	result = venture_report_generate(venture_report_registry_lookup(registry, "sequence_performance"), f->context, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	json = venture_report_result_to_json(result);
	row = json_array_get_object_element(json_object_get_array_member(json_node_get_object(json), "rows"), 0);
	g_assert_cmpfloat(json_object_get_double_member(row, "enrolled"), ==, 2);
	g_assert_cmpfloat(json_object_get_double_member(row, "completed"), ==, 1);
	g_assert_cmpfloat(json_object_get_double_member(row, "exited"), ==, 1);
	g_assert_cmpfloat(json_object_get_double_member(row, "replies"), ==, 1);
	g_assert_cmpfloat(json_object_get_double_member(row, "goal_met"), ==, 1);
	g_assert_nonnull(strstr(json_object_get_string_member(row, "exit_reasons"), "reply"));
}

typedef struct { gboolean done; GBytes *body; GError *error; } HttpResult;
static void
http_done(GObject *source, GAsyncResult *result, gpointer data)
{
	HttpResult *reply = data;
	reply->body = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &reply->error);
	reply->done = TRUE;
}

static guint
http(SoupSession *session, const gchar *base, const gchar *method,
	const gchar *path, const gchar *body, gchar **text)
{
	g_autofree gchar *url = g_strconcat(base, path, NULL);
	g_autoptr(SoupMessage) message = soup_message_new(method, url);
	HttpResult result = { FALSE, NULL, NULL };
	guint status;
	if (body != NULL)
	{
		g_autoptr(GBytes) bytes = g_bytes_new(body, strlen(body));
		soup_message_set_request_body_from_bytes(message, "application/json", bytes);
	}
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL, http_done, &result);
	while (!result.done)
		g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(result.error);
	*text = g_strndup(g_bytes_get_data(result.body, NULL), g_bytes_get_size(result.body));
	g_bytes_unref(result.body);
	status = soup_message_get_status(message);
	return status;
}

typedef struct { gboolean done; gchar *out; gchar *err; GError *error; } CliResult;
static void
cli_done(GObject *source, GAsyncResult *result, gpointer data)
{
	CliResult *reply = data;
	g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &reply->out, &reply->err, &reply->error);
	reply->done = TRUE;
}

static void
check_cli(const gchar *base, const gchar *verb, const gchar *arg, const gchar *value)
{
	g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
	g_autoptr(GSubprocess) child = NULL;
	g_autoptr(GError) error = NULL;
	CliResult result = { FALSE, NULL, NULL, NULL };
	const gchar *args[] = { "build/debug/venturectl", "--server", base, "sequence", verb, arg, value, NULL };
	g_subprocess_launcher_unsetenv(launcher, "VENTURE_TOKEN");
	child = g_subprocess_launcher_spawnv(launcher, args, &error);
	g_assert_no_error(error);
	g_subprocess_communicate_utf8_async(child, NULL, NULL, cli_done, &result);
	while (!result.done)
		g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(result.error);
	g_test_message("CLI stderr: %s", result.err);
	g_assert_true(g_subprocess_get_successful(child));
	g_assert_nonnull(result.out);
	g_free(result.out);
	g_free(result.err);
}

static void
test_surfaces(Fixture *f, gconstpointer unused)
{
	g_autoptr(GSocketListener) probe = g_socket_listener_new();
	g_autoptr(VentureWebServer) server = NULL;
	g_autoptr(SoupSession) session = soup_session_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *state = g_dir_make_tmp("venture-sequence-web-XXXXXX", NULL);
	g_autofree gchar *base = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *payload = NULL;
	g_autofree gchar *reply = NULL;
	guint port = g_socket_listener_add_any_inet_port(probe, NULL, &error);
	(void)unused;
	g_assert_no_error(error);
	g_clear_object(&probe);
	base = g_strdup_printf("http://127.0.0.1:%u", port);
	g_object_set(f->config, "state-dir", state, "server-bind-address", "127.0.0.1",
		"server-port", (gint64)port, "security-require-auth", FALSE, NULL);
	server = venture_web_server_new(f->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(server, &error));
	g_assert_no_error(error);
	path = g_strdup_printf("/api/v1/sequence/%" G_GINT64_FORMAT "/enroll", venture_entity_get_id(f->sequence));
	payload = g_strdup_printf("{\"contact_id\":%" G_GINT64_FORMAT ",\"enrolled_at\":\"2026-09-11T22:00:00Z\"}", venture_entity_get_id(f->contact));
	g_assert_cmpuint(http(session, base, "POST", path, payload, &reply), ==, 201);
	g_assert_nonnull(strstr(reply, "next_run_at"));
	g_clear_pointer(&reply, g_free);
	g_assert_cmpuint(http(session, base, "POST", "/api/v1/sequences/run?stage=1",
		"{}", &reply), >=, 400);
	g_clear_pointer(&reply, g_free);
	g_assert_cmpuint(http(session, base, "POST", "/api/v1/sequence_enrollment/1/pause", "{}", &reply), ==, 200);
	g_clear_pointer(&reply, g_free);
	g_assert_cmpuint(http(session, base, "POST", "/api/v1/sequence_enrollment/1/resume", "{}", &reply), ==, 200);
	g_clear_pointer(&reply, g_free);
	g_assert_cmpuint(http(session, base, "POST", "/api/v1/sequences/run",
		"{\"as_of\":\"2026-09-14T13:00:00Z\"}", &reply), ==, 200);
	g_clear_pointer(&reply, g_free);
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/e/contact/%" G_GINT64_FORMAT, venture_entity_get_id(f->contact));
	g_assert_cmpuint(http(session, base, "GET", path, NULL, &reply), ==, 200);
	g_assert_nonnull(strstr(reply, "Enroll in sequence"));
	g_assert_nonnull(strstr(reply, "Sequence history"));
	check_cli(base, "status", "1", NULL);
	check_cli(base, "enroll", "1", "contact_id=1");
	check_cli(base, "run", "--as-of", "2026-09-14T13:00:00Z");
	g_clear_pointer(&reply, g_free);
	venture_config_set_module_enabled(f->config, "sequences", FALSE);
	g_assert_cmpuint(http(session, base, "POST", "/api/v1/sequences/run", "{}", &reply), ==, 404);
	venture_config_set_module_enabled(f->config, "sequences", TRUE);
	venture_web_server_stop(server);
	g_clear_object(&server);
	venture_test_remove_tree(state);
}

static void
test_restart(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) row = enrollment(f);
	g_autoptr(GPtrArray) found = NULL;
	g_autoptr(OrmResult) migration = NULL;
	g_autoptr(GError) error = NULL;
	(void)unused;
	save(f, row);
	g_assert_cmpint(run(f, "2026-09-14T13:00:00Z", &error), ==, 1);
	g_assert_no_error(error);
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	f->db = venture_database_new(f->uri, &error);
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	g_assert_cmpint(run(f, "2026-09-14T13:00:00Z", &error), ==, 0);
	g_assert_no_error(error);
	found = deliveries(f);
	g_assert_cmpuint(found->len, ==, 1);
	migration = venture_database_query_raw(f->db, "SELECT name FROM schema_migrations WHERE version = 80", NULL, &error);
	g_assert_no_error(error);
	g_assert_true(orm_result_next(migration));
	g_assert_nonnull(strstr(orm_row_get_string(orm_result_get_row(migration), 0), "sequences"));
}

static void
test_adapter(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) row = enrollment(f);
	g_autoptr(GPtrArray) found = NULL;
	g_autoptr(VentureEntity) stored = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *external = NULL;
	(void)unused;
	save(f, row);
	g_assert_cmpint(run(f, "2026-09-14T13:00:00Z", &error), ==, 1);
	g_assert_no_error(error);
	found = deliveries(f);
	g_object_set(g_ptr_array_index(found, 0), "state", 1, "external-message-id", "provider-42", NULL);
	save(f, g_ptr_array_index(found, 0));
	stored = venture_database_get(f->db, VENTURE_TYPE_SEQUENCE_DELIVERY,
		venture_entity_get_id(g_ptr_array_index(found, 0)), &error);
	g_assert_no_error(error);
	g_object_get(stored, "external-message-id", &external, NULL);
	g_assert_cmpstr(external, ==, "provider-42");
	g_object_set(stored, "state", 0, NULL);
	g_assert_false(venture_database_save(f->db, stored, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

static void
test_derived_refusal(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) row = enrollment(f);
	g_autoptr(GDateTime) fake = g_date_time_new_from_iso8601("2026-09-01T12:00:00Z", NULL);
	g_autoptr(GError) error = NULL;
	(void)unused;
	g_object_set(row, "goal-met-at", fake, NULL);
	g_assert_false(venture_database_save(f->db, row, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

static void
test_meeting_goal(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) row = enrollment(f);
	g_autoptr(VentureEntity) interaction = NULL;
	g_autoptr(VentureEntity) stored = NULL;
	g_autoptr(GDateTime) goal = NULL;
	(void)unused;
	g_object_set(f->sequence, "goal", 2, "exit-on-reply", FALSE, NULL);
	save(f, f->sequence);
	save(f, row);
	interaction = g_object_new(VENTURE_TYPE_INTERACTION, "organization-id", (gint64)1,
		"contact-id", venture_entity_get_id(f->contact), "subject", "Discovery booked",
		"kind", VENTURE_INTERACTION_KIND_MEETING, "outbound", TRUE, NULL);
	save(f, interaction);
	stored = venture_database_get(f->db, VENTURE_TYPE_SEQUENCE_ENROLLMENT, venture_entity_get_id(row), NULL);
	g_object_get(stored, "goal-met-at", &goal, NULL);
	g_assert_nonnull(goal);
}

static void
test_staged(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) row = enrollment(f);
	g_autoptr(VentureEntity) suppression = NULL;
	g_autoptr(VentureEntity) stored = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) next = NULL;
	VentureConfirmationStore *store = venture_context_get_confirmations(f->context);
	VentureConfirmation *confirmation;
	VentureActor actor;
	g_autofree gchar *id = NULL;
	(void)unused;
	actor.kind = VENTURE_ACTOR_KIND_AI;
	actor.name = "assistant";
	actor.prompt = "Enroll Ada after the inquiry";
	actor.request_id = NULL;
	actor.approved_by = NULL;
	confirmation = venture_confirmation_store_stage(store, VENTURE_AUDIT_ACTION_CREATE,
		row, NULL, &actor, "assistant", &error);
	g_assert_no_error(error);
	g_assert_nonnull(confirmation);
	g_assert_cmpint(venture_entity_get_id(row), ==, 0);
	id = g_strdup(venture_confirmation_get_id(confirmation));
	g_assert_true(venture_confirmation_store_approve(store, id, "owner", &error));
	g_assert_no_error(error);
	stored = venture_database_get(f->db, VENTURE_TYPE_SEQUENCE_ENROLLMENT, 1, &error);
	g_assert_no_error(error);
	g_object_get(stored, "next-run-at", &next, NULL);
	g_assert_nonnull(next);
	g_clear_object(&row);
	row = enrollment(f);
	confirmation = venture_confirmation_store_stage(store, VENTURE_AUDIT_ACTION_CREATE,
		row, NULL, &actor, "assistant", &error);
	g_assert_no_error(error);
	g_clear_pointer(&id, g_free);
	id = g_strdup(venture_confirmation_get_id(confirmation));
	suppression = g_object_new(VENTURE_TYPE_SUPPRESSION, "organization-id", (gint64)1,
		"email", "ada@example.com", NULL);
	save(f, suppression);
	g_assert_false(venture_confirmation_store_approve(store, id, "owner", &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

static void
test_tasks(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) owner = g_object_new(VENTURE_TYPE_USER,
		"organization-id", (gint64)1, "username", "sequence-owner", "active", TRUE, NULL);
	g_autoptr(VentureEntity) row = enrollment(f);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_NOTIFICATION);
	g_autoptr(GPtrArray) found = NULL;
	g_autoptr(GError) error = NULL;
	gint64 user;
	gint channel = GPOINTER_TO_INT(unused);
	save(f, owner);
	g_object_set(row, "owner-id", venture_entity_get_id(owner), NULL);
	g_object_set(f->step, "channel", channel, NULL);
	save(f, f->step);
	save(f, row);
	g_assert_cmpint(run(f, "2026-09-14T13:00:00Z", &error), ==, 1);
	g_assert_no_error(error);
	g_assert_cmpint(run(f, "2026-09-14T13:00:00Z", &error), ==, 0);
	g_assert_no_error(error);
	venture_query_add_filter_string(query, "target-type", VENTURE_FILTER_OP_EQ, "sequence_enrollment", NULL);
	found = venture_database_find(f->db, query, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(found->len, ==, 1);
	g_object_get(g_ptr_array_index(found, 0), "user-id", &user, NULL);
	g_assert_cmpint(user, ==, venture_entity_get_id(owner));
}

static void
test_module_upgrade(void)
{
	g_autofree gchar *directory = g_dir_make_tmp("venture-sequence-upgrade-XXXXXX", NULL);
	g_autofree gchar *uri = g_strdup_printf("sqlite://%s/database.db", directory);
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureDatabase) db = NULL;
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(VentureEntity) contact = g_object_new(VENTURE_TYPE_CONTACT,
		"organization-id", (gint64)1, "name", "Existing customer", NULL);
	g_autoptr(VentureEntity) stored = NULL;
	g_autoptr(GDateTime) at = g_date_time_new_now_utc();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *name = NULL;
	venture_config_set_module_enabled(config, "sequences", FALSE);
	db = venture_database_new(uri, &error);
	g_assert_no_error(error);
	context = venture_context_new(config, db);
	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	g_assert_true(venture_database_save(db, contact, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpint(venture_sequence_service_run_due(venture_sequence_service_get(db), 1, at, NULL, &error), ==, -1);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_clear_error(&error);
	venture_config_set_module_enabled(config, "sequences", TRUE);
	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	stored = venture_database_get(db, VENTURE_TYPE_CONTACT, venture_entity_get_id(contact), &error);
	g_assert_no_error(error);
	g_object_get(stored, "name", &name, NULL);
	g_assert_cmpstr(name, ==, "Existing customer");
	g_assert_cmpint(venture_sequence_service_run_due(venture_sequence_service_get(db), 1, at, NULL, &error), ==, 0);
	g_assert_no_error(error);
	g_clear_object(&context);
	g_clear_object(&db);
	venture_test_remove_tree(directory);
}

static void
test_refusals(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) row = enrollment(f);
	g_autoptr(GError) error = NULL;
	gint which = GPOINTER_TO_INT(data);
	VentureEntity *target = row;
	switch (which)
	{
	case 1: g_object_set(row, "contact-id", (gint64)99999, NULL); break;
	case 2: g_object_set(row, "organization-id", (gint64)99999, NULL); break;
	case 3:
		g_object_set(f->sequence, "active", FALSE, NULL);
		save(f, f->sequence);
		break;
	case 4:
		g_object_set(f->step, "active", FALSE, NULL);
		save(f, f->step);
		break;
	case 5: target = f->sequence; g_object_set(target, "timezone", "No/Such_Zone", NULL); break;
	case 6: target = f->sequence; g_object_set(target, "send-window-end", (gint64)8, NULL); break;
	case 7: target = f->sequence; g_object_set(target, "weekdays", "8", NULL); break;
	case 8: target = f->step; g_object_set(target, "delay-days", (gint64)-1, NULL); break;
	case 9: target = f->step; g_object_set(target, "position", (gint64)0, NULL); break;
	default: g_assert_not_reached();
	}
	g_assert_false(venture_database_save(f->db, target, NULL, &error));
	g_assert_nonnull(error);
}

static void
test_dst(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) row = enrollment(f);
	g_autoptr(GDateTime) start = g_date_time_new_from_iso8601("2026-03-06T15:00:00Z", NULL);
	g_autoptr(GDateTime) next = NULL;
	g_autofree gchar *text = NULL;
	(void)unused;
	g_object_set(f->step, "delay-days", (gint64)3, NULL);
	save(f, f->step);
	g_object_set(row, "enrolled-at", start, NULL);
	save(f, row);
	g_object_get(row, "next-run-at", &next, NULL);
	text = g_date_time_format_iso8601(next);
	g_assert_cmpstr(text, ==, "2026-03-09T14:00:00Z");
}

static void
test_suppression_unique(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) first = g_object_new(VENTURE_TYPE_SUPPRESSION,
		"organization-id", (gint64)1, "email", "ADA@EXAMPLE.COM", NULL);
	g_autoptr(VentureEntity) second = g_object_new(VENTURE_TYPE_SUPPRESSION,
		"organization-id", (gint64)1, "email", " ada@example.com ", NULL);
	g_autoptr(VentureEntity) org = g_object_new(VENTURE_TYPE_ORGANIZATION, "name", "Another organization", NULL);
	g_autoptr(GError) error = NULL;
	(void)unused;
	save(f, first);
	g_assert_false(venture_database_save(f->db, second, NULL, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	save(f, org);
	g_object_set(second, "organization-id", venture_entity_get_id(org), NULL);
	save(f, second);
}

/* Calendar overflow is an ordinary validation error, never a GLib assertion. */
static void
test_date_limit(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) row = enrollment(f);
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) queued = NULL;
	(void)data;
	save(f, row);
	/* A valid ISO timestamp can have no representable next sending day. */
	g_assert_cmpint(run(f, "9999-12-31T23:00:00Z", &error), ==, -1);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	queued = deliveries(f);
	g_assert_cmpuint(queued->len, ==, 0);
}

/* Soft deletion retains references, but must never queue a later outreach. */
static void
test_deleted_reference(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) row = enrollment(f);
	g_autoptr(VentureEntity) stored = NULL;
	g_autoptr(GPtrArray) queued = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *reason = NULL;
	gint status;
	VentureEntity *deleted = data != NULL ? f->sequence : f->contact;
	save(f, row);
	g_assert_true(venture_database_delete(f->db, deleted, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpint(run(f, "2026-09-14T14:00:00Z", &error), ==, 0);
	g_assert_no_error(error);
	queued = deliveries(f);
	g_assert_cmpuint(queued->len, ==, 0);
	stored = venture_database_get(f->db, VENTURE_TYPE_SEQUENCE_ENROLLMENT, venture_entity_get_id(row), &error);
	g_assert_no_error(error);
	g_object_get(stored, "status", &status, "exit-reason", &reason, NULL);
	g_assert_cmpint(status, ==, 3);
	g_assert_cmpstr(reason, ==, data != NULL ? "sequence_deleted" : "contact_deleted");
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/sequences/records", test_records);
	g_test_add("/sequences/enroll_window", Fixture, NULL, setup, test_enroll_window, teardown);
	g_test_add("/sequences/duplicate", Fixture, NULL, setup, test_duplicate, teardown);
	g_test_add("/sequences/suppression", Fixture, NULL, setup, test_suppression, teardown);
	g_test_add("/sequences/reply", Fixture, NULL, setup, test_reply, teardown);
	g_test_add("/sequences/lead-reply", Fixture, GINT_TO_POINTER(1), setup, test_reply, teardown);
	g_test_add("/sequences/won", Fixture, NULL, setup, test_won, teardown);
	g_test_add("/sequences/sweep", Fixture, NULL, setup, test_sweep, teardown);
	g_test_add("/sequences/date_limit", Fixture, NULL, setup, test_date_limit, teardown);
	g_test_add("/sequences/deleted_contact", Fixture, NULL, setup, test_deleted_reference, teardown);
	g_test_add("/sequences/deleted_sequence", Fixture, "disk", setup, test_deleted_reference, teardown);
	g_test_add("/sequences/edit", Fixture, NULL, setup, test_edit, teardown);
	g_test_add("/sequences/atomic", Fixture, NULL, setup, test_atomic, teardown);
	g_test_add("/sequences/manual", Fixture, NULL, setup, test_manual, teardown);
	g_test_add("/sequences/render_failure", Fixture, NULL, setup, test_render_failure, teardown);
	g_test_add("/sequences/reports", Fixture, NULL, setup, test_reports, teardown);
	g_test_add("/sequences/surfaces", Fixture, NULL, setup, test_surfaces, teardown);
	g_test_add("/sequences/restart", Fixture, "disk", setup, test_restart, teardown);
	g_test_add("/sequences/adapter", Fixture, NULL, setup, test_adapter, teardown);
	g_test_add("/sequences/derived_refusal", Fixture, NULL, setup, test_derived_refusal, teardown);
	g_test_add("/sequences/meeting_goal", Fixture, NULL, setup, test_meeting_goal, teardown);
	g_test_add("/sequences/staged", Fixture, NULL, setup, test_staged, teardown);
	g_test_add("/sequences/call_task", Fixture, GINT_TO_POINTER(1), setup, test_tasks, teardown);
	g_test_add("/sequences/sms_task", Fixture, GINT_TO_POINTER(2), setup, test_tasks, teardown);
	g_test_add_func("/sequences/module_upgrade", test_module_upgrade);
	g_test_add("/sequences/refusal/1", Fixture, GINT_TO_POINTER(1), setup, test_refusals, teardown);
	g_test_add("/sequences/refusal/2", Fixture, GINT_TO_POINTER(2), setup, test_refusals, teardown);
	g_test_add("/sequences/refusal/3", Fixture, GINT_TO_POINTER(3), setup, test_refusals, teardown);
	g_test_add("/sequences/refusal/4", Fixture, GINT_TO_POINTER(4), setup, test_refusals, teardown);
	g_test_add("/sequences/refusal/5", Fixture, GINT_TO_POINTER(5), setup, test_refusals, teardown);
	g_test_add("/sequences/refusal/6", Fixture, GINT_TO_POINTER(6), setup, test_refusals, teardown);
	g_test_add("/sequences/refusal/7", Fixture, GINT_TO_POINTER(7), setup, test_refusals, teardown);
	g_test_add("/sequences/refusal/8", Fixture, GINT_TO_POINTER(8), setup, test_refusals, teardown);
	g_test_add("/sequences/refusal/9", Fixture, GINT_TO_POINTER(9), setup, test_refusals, teardown);
	g_test_add("/sequences/dst", Fixture, NULL, setup, test_dst, teardown);
	g_test_add("/sequences/suppression_unique", Fixture, NULL, setup, test_suppression_unique, teardown);
	return g_test_run();
}
