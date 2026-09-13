/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <string.h>
#include <libsoup/soup.h>
#include <unistd.h>
#include "venture-test-util.h"

static void
test_records(void)
{
	static const gchar *const names[] = { "lead", "lead_form", "lead_assignment_rule" };
	g_autoptr(VentureModuleRegistry) modules = venture_module_registry_new();
	guint i;

	venture_module_registry_register_builtins(modules);
	for (i = 0; i < G_N_ELEMENTS(names); i++)
	{
		GType type = venture_entity_registry_lookup(venture_entity_registry_get_default(), names[i]);
		VentureModule *module;
		g_assert_cmpuint(type, !=, G_TYPE_INVALID);
		module = venture_module_registry_get_module_for_type(modules, names[i]);
		g_assert_nonnull(module);
		g_assert_cmpstr(venture_module_get_name(module), ==, "leads");
	}
}

typedef struct {
	VentureDatabase *db;
	VentureConfig *config;
	VentureContext *context;
	gint64 org;
	VentureWebServer *server;
	SoupSession *session;
	gchar *state_dir;
	guint16 port;
} Fixture;

static void
setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	(void)data;
	f->config = venture_config_new();
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
	if (f->server != NULL) venture_web_server_stop(f->server);
	g_clear_object(&f->server);
	g_clear_object(&f->session);
	if (f->state_dir != NULL) { venture_test_remove_tree(f->state_dir); g_free(f->state_dir); }
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
}

static VentureEntity *
record(Fixture *f, const gchar *type, const gchar *name)
{
	GType t = venture_entity_registry_lookup(venture_entity_registry_get_default(), type);
	VentureEntity *e;
	g_assert_cmpuint(t, !=, G_TYPE_INVALID);
	e = g_object_new(t, "name", name, "organization-id", f->org, NULL);
	return e;
}

static void
save(Fixture *f, VentureEntity *e)
{
	g_autoptr(GError) error = NULL;
	gboolean ok = venture_database_save(f->db, e, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(ok);
}

static void
test_normalize(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) lead = record(f, "lead", "Inquiry");
	g_autofree gchar *email = NULL;
	g_autofree gchar *phone = NULL;
	g_autofree gchar *website = NULL;
	(void)data;
	g_object_set(lead, "email", " Alice+form@EXAMPLE.COM ", "phone", "+1 (555) 123-4567",
		"website", "https://www.Example.COM/path", NULL);
	save(f, lead);
	g_object_get(lead, "email", &email, "phone", &phone, "website", &website, NULL);
	g_assert_cmpstr(email, ==, "alice@example.com");
	g_assert_cmpstr(phone, ==, "15551234567");
	g_assert_cmpstr(website, ==, "example.com");
}

static void
test_assignment(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) rule = record(f, "lead_assignment_rule", "Rota");
	g_autoptr(VentureEntity) a = record(f, "lead", "A");
	g_autoptr(VentureEntity) b = record(f, "lead", "B");
	g_autofree gchar *owner = NULL;
	(void)data;
	g_object_set(rule, "active", TRUE, "assignees", "alice, bob", NULL);
	save(f, rule);
	save(f, a);
	save(f, b);
	g_object_get(a, "owner", &owner, NULL);
	g_assert_cmpstr(owner, ==, "alice");
	g_clear_pointer(&owner, g_free);
	g_object_get(b, "owner", &owner, NULL);
	g_assert_cmpstr(owner, ==, "bob");
}

static void
test_generic_conversion_refused(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) lead = record(f, "lead", "Inquiry");
	g_autoptr(GError) error = NULL;
	(void)data;
	g_object_set(lead, "status", VENTURE_LEAD_CONVERTED, NULL);
	g_assert_false(venture_database_save(f->db, lead, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_nonnull(strstr(error->message, "VentureLeadService"));
}

static void
test_duplicate_refused(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) a = record(f, "lead", "First");
	g_autoptr(VentureEntity) b = record(f, "lead", "Again");
	g_autoptr(GError) error = NULL;
	(void)data;
	g_object_set(a, "email", "alice@example.com", NULL);
	g_object_set(b, "email", "ALICE+web@example.com", NULL);
	save(f, a);
	g_assert_false(venture_database_save(f->db, b, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_ALREADY_EXISTS);
}

static gint64
count(Fixture *f, const gchar *type)
{
	g_autoptr(VentureQuery) q = venture_query_new(venture_entity_registry_lookup(venture_entity_registry_get_default(), type));
	venture_query_set_organization(q, f->org);
	return venture_database_count(f->db, q, NULL);
}

static void
test_history(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) lead = record(f, "lead", "Inquiry");
	(void)data;
	save(f, lead);
	g_object_set(lead, "status", VENTURE_LEAD_QUALIFIED, NULL);
	save(f, lead);
	g_assert_cmpint(count(f, "interaction"), ==, 1);
}

static void
test_recycle_reason(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) lead = record(f, "lead", "Inquiry");
	g_autoptr(GError) error = NULL;
	(void)data;
	save(f, lead);
	g_object_set(lead, "status", VENTURE_LEAD_RECYCLED, NULL);
	g_assert_false(venture_database_save(f->db, lead, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

static void
start_http(Fixture *f)
{
	g_autoptr(GError) error = NULL;
	f->state_dir = g_dir_make_tmp("venture-leads-XXXXXX", NULL);
	f->port = (guint16)g_random_int_range(20000, 60000);
	g_object_set(f->config, "state-dir", f->state_dir, "server-port", (gint64)f->port,
		"server-bind-address", "127.0.0.1", "security-require-auth", FALSE, NULL);
	f->server = venture_web_server_new(f->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(f->server, &error));
	g_assert_no_error(error);
	f->session = soup_session_new();
}

typedef struct { gboolean done; GBytes *body; GError *error; } Reply;

static void
received(GObject *source, GAsyncResult *result, gpointer data)
{
	Reply *reply = data;
	reply->body = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &reply->error);
	reply->done = TRUE;
}

static guint
post(Fixture *f, const gchar *path, const gchar *body, gboolean json)
{
	g_autofree gchar *uri = g_strdup_printf("http://127.0.0.1:%u%s", f->port, path);
	g_autoptr(SoupMessage) msg = soup_message_new("POST", uri);
	g_autoptr(GBytes) bytes = g_bytes_new(body, strlen(body));
	Reply reply = { FALSE, NULL, NULL };
	soup_message_set_flags(msg, SOUP_MESSAGE_NO_REDIRECT);
	soup_message_set_request_body_from_bytes(msg, json ? "application/json" : "application/x-www-form-urlencoded", bytes);
	soup_session_send_and_read_async(f->session, msg, G_PRIORITY_DEFAULT, NULL, received, &reply);
	while (!reply.done) g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(reply.error);
	g_clear_pointer(&reply.body, g_bytes_unref);
	return soup_message_get_status(msg);
}

static void
test_capture(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = record(f, "lead_form", "Web");
	(void)data;
	g_object_set(form, "public-token", "capture-test", "active", TRUE, "on-duplicate", "merge", "honeypot", "fax", NULL);
	save(f, form);
	start_http(f);
	g_assert_cmpuint(post(f, "/f/capture-test", "name=Alice&email=Alice%2Bweb%40example.com", FALSE), ==, 200);
	g_assert_cmpuint(post(f, "/f/capture-test", "{\"name\":\"Alice\",\"email\":\"alice@example.com\"}", TRUE), ==, 200);
	g_assert_cmpint(count(f, "lead"), ==, 1);
	g_assert_cmpint(count(f, "interaction"), ==, 1);
	g_assert_cmpuint(post(f, "/f/capture-test", "name=Robot&fax=spam", FALSE), ==, 200);
	g_assert_cmpint(count(f, "lead"), ==, 1);
}

static void
test_convert(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) lead = record(f, "lead", "Alice");
	g_autoptr(VentureEntity) stored = NULL;
	g_autofree gchar *path = NULL;
	gint64 company = 0, contact = 0, deal = 0;
	VentureLeadStatus status;
	(void)data;
	g_object_set(lead, "company-name", "Example", "source", "web", "status", VENTURE_LEAD_QUALIFIED, NULL);
	save(f, lead);
	start_http(f);
	path = g_strdup_printf("/api/v1/leads/%" G_GINT64_FORMAT "/convert", venture_entity_get_id(lead));
	g_assert_cmpuint(post(f, path, "{\"deal\":true}", TRUE), ==, 200);
	stored = venture_database_get(f->db, G_OBJECT_TYPE(lead), venture_entity_get_id(lead), NULL);
	g_object_get(stored, "status", &status, "converted-company-id", &company,
		"converted-contact-id", &contact, "converted-deal-id", &deal, NULL);
	g_assert_cmpint(status, ==, VENTURE_LEAD_CONVERTED);
	g_assert_cmpint(company, >, 0);
	g_assert_cmpint(contact, >, 0);
	g_assert_cmpint(deal, >, 0);
	g_assert_cmpint(count(f, "company"), ==, 1);
	g_assert_cmpint(count(f, "contact"), ==, 1);
	g_assert_cmpint(count(f, "deal"), ==, 1);
	g_assert_cmpuint(post(f, path, "{}", TRUE), ==, 409);
}

static gboolean
fail_deal(VentureDatabase *db, VentureEntity *entity, VentureEntity *previous, gpointer data, GError **error)
{
	(void)db; (void)entity; (void)previous; (void)data;
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Injected deal failure");
	return FALSE;
}

static void
test_convert_rollback(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) lead = record(f, "lead", "Alice");
	g_autofree gchar *path = NULL;
	(void)data;
	g_object_set(lead, "company-name", "Example", "status", VENTURE_LEAD_QUALIFIED, NULL);
	save(f, lead);
	venture_database_add_save_validator(f->db, VENTURE_TYPE_DEAL, fail_deal, NULL, NULL);
	start_http(f);
	path = g_strdup_printf("/api/v1/leads/%" G_GINT64_FORMAT "/convert", venture_entity_get_id(lead));
	g_assert_cmpuint(post(f, path, "{\"deal\":true}", TRUE), ==, 422);
	g_assert_cmpint(count(f, "company"), ==, 0);
	g_assert_cmpint(count(f, "contact"), ==, 0);
	g_assert_cmpint(count(f, "deal"), ==, 0);
}

static void
test_reports(Fixture *f, gconstpointer data)
{
	VentureReportRegistry *registry = venture_context_get_report_registry(f->context);
	(void)data;
	g_assert_nonnull(venture_report_registry_lookup(registry, "lead_sources"));
	g_assert_nonnull(venture_report_registry_lookup(registry, "lead_response_time"));
	g_assert_nonnull(venture_report_registry_lookup(registry, "leads_recycled_due"));
}

static void
test_staged_convert(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) lead = record(f, "lead", "Alice");
	g_autoptr(GPtrArray) pending = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *confirmation = NULL;
	VentureConfirmationStore *store = venture_context_get_confirmations(f->context);
	(void)data;
	g_object_set(lead, "status", VENTURE_LEAD_QUALIFIED, NULL);
	save(f, lead);
	start_http(f);
	path = g_strdup_printf("/api/v1/leads/%" G_GINT64_FORMAT "/convert?stage=1", venture_entity_get_id(lead));
	g_assert_cmpuint(post(f, path, "{\"deal\":false}", TRUE), ==, 202);
	g_assert_cmpint(count(f, "company"), ==, 0);
	pending = venture_confirmation_store_list_pending(store);
	g_assert_cmpuint(pending->len, ==, 1);
	confirmation = g_strdup(venture_confirmation_get_id(g_ptr_array_index(pending, 0)));
	g_assert_true(venture_confirmation_store_approve(store, confirmation, "operator", &error));
	g_assert_no_error(error);
	g_assert_cmpint(count(f, "company"), ==, 1);
	g_assert_cmpint(count(f, "contact"), ==, 1);
	g_assert_cmpint(count(f, "deal"), ==, 0);
}

typedef struct { gboolean done; gchar *out; gchar *err; GError *error; } CliReply;

static void
cli_received(GObject *source, GAsyncResult *result, gpointer data)
{
	CliReply *reply = data;
	g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &reply->out, &reply->err, &reply->error);
	reply->done = TRUE;
}

static void
test_cli_convert(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) lead = record(f, "lead", "Alice");
	g_autoptr(GSubprocess) process = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *id = NULL;
	CliReply reply = { FALSE, NULL, NULL, NULL };
	(void)data;
	g_object_set(lead, "status", VENTURE_LEAD_QUALIFIED, NULL);
	save(f, lead);
	start_http(f);
	id = g_strdup_printf("%" G_GINT64_FORMAT, venture_entity_get_id(lead));
	process = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE,
		&error, "build/debug/venturectl", "--server", venture_web_server_get_base_url(f->server),
		"lead", "convert", id, "deal=no", NULL);
	g_assert_no_error(error);
	g_subprocess_communicate_utf8_async(process, NULL, NULL, cli_received, &reply);
	while (!reply.done) g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(reply.error);
	g_test_message("CLI: %s", reply.err != NULL ? reply.err : "");
	g_free(reply.out); g_free(reply.err);
	g_assert_true(g_subprocess_get_successful(process));
	g_assert_cmpint(count(f, "contact"), ==, 1);
	g_assert_cmpint(count(f, "deal"), ==, 0);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	venture_entity_registry_register_builtins(venture_entity_registry_get_default());
	g_test_add_func("/leads/records", test_records);
	g_test_add("/leads/normalize", Fixture, NULL, setup, test_normalize, teardown);
	g_test_add("/leads/assignment", Fixture, NULL, setup, test_assignment, teardown);
	g_test_add("/leads/generic-conversion-refused", Fixture, NULL, setup, test_generic_conversion_refused, teardown);
	g_test_add("/leads/duplicate-refused", Fixture, NULL, setup, test_duplicate_refused, teardown);
	g_test_add("/leads/history", Fixture, NULL, setup, test_history, teardown);
	g_test_add("/leads/recycle-reason", Fixture, NULL, setup, test_recycle_reason, teardown);
	g_test_add("/leads/capture", Fixture, NULL, setup, test_capture, teardown);
	g_test_add("/leads/convert", Fixture, NULL, setup, test_convert, teardown);
	g_test_add("/leads/convert-rollback", Fixture, NULL, setup, test_convert_rollback, teardown);
	g_test_add("/leads/reports", Fixture, NULL, setup, test_reports, teardown);
	g_test_add("/leads/staged-convert", Fixture, NULL, setup, test_staged_convert, teardown);
	g_test_add("/leads/cli-convert", Fixture, NULL, setup, test_cli_convert, teardown);
	return g_test_run();
}
