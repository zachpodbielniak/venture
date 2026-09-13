/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <string.h>
#include <libsoup/soup.h>
#include <unistd.h>
#include "venture-test-util.h"
#include "db/venture-migrations.h"

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
	gboolean started;
	f->state_dir = g_dir_make_tmp("venture-leads-XXXXXX", NULL);
	f->port = (guint16)g_random_int_range(20000, 60000);
	g_object_set(f->config, "state-dir", f->state_dir, "server-port", (gint64)f->port,
		"server-bind-address", "127.0.0.1", "security-require-auth", FALSE, NULL);
	f->server = venture_web_server_new(f->context, &error);
	g_assert_no_error(error);
	started = venture_web_server_start(f->server, &error);
	g_assert_no_error(error);
	g_assert_true(started);
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
	g_autoptr(VentureEntity) campaign = record(f, "campaign", "Acquisition");
	g_autofree gchar *path = NULL;
	gint64 company = 0, contact = 0, deal = 0;
	VentureLeadStatus status;
	(void)data;
	save(f, campaign);
	g_object_set(lead, "campaign-id", venture_entity_get_id(campaign), NULL);
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
	{
		GType types[] = { VENTURE_TYPE_COMPANY, VENTURE_TYPE_CONTACT, VENTURE_TYPE_DEAL };
		gint64 ids[] = { company, contact, deal };
		guint i;
		for (i = 0; i < G_N_ELEMENTS(types); i++)
		{
			g_autoptr(VentureEntity) linked = venture_database_get(f->db, types[i], ids[i], NULL);
			g_autofree gchar *source = NULL;
			gint64 attribution = 0;
			g_object_get(linked, "source", &source, "campaign-id", &attribution, NULL);
			g_assert_cmpstr(source, ==, "web");
			g_assert_cmpint(attribution, ==, venture_entity_get_id(campaign));
		}
	}
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

static VentureReportResult *
run_report(Fixture *f, const gchar *name, VentureDateRange *period)
{
	g_autoptr(GError) error = NULL;
	VentureReport *report = venture_report_registry_lookup(venture_context_get_report_registry(f->context), name);
	VentureReportResult *result = venture_report_generate(report, f->context, period, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	return result;
}

static gdouble
metric(VentureReportResult *report, const gchar *key)
{
	GPtrArray *metrics = venture_report_result_get_metrics(report);
	guint i;
	for (i = 0; i < metrics->len; i++)
	{
		VentureMetric *m = g_ptr_array_index(metrics, i);
		if (g_str_equal(venture_metric_get_key(m), key)) return venture_metric_get_number(m);
	}
	g_error("Missing metric %s", key);
	return 0;
}

static void
test_report_values(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) a = record(f, "lead", "A");
	g_autoptr(VentureEntity) b = record(f, "lead", "B");
	g_autoptr(VentureEntity) converted = NULL;
	g_autoptr(VentureReportResult) sources = NULL;
	g_autoptr(VentureReportResult) response = NULL;
	g_autoptr(VentureReportResult) due = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GDateTime) created = NULL;
	g_autoptr(GDateTime) first = NULL;
	g_autoptr(GDateTime) later = NULL;
	g_autoptr(GDateTime) yesterday = NULL;
	g_autoptr(VentureInteraction) outbound = venture_interaction_new();
	g_autoptr(VentureInteraction) inbound = venture_interaction_new();
	g_autoptr(VentureDateRange) old_period = venture_date_range_new_quarter(2020, 1, NULL);
	JsonObject *row;
	(void)data;
	g_object_set(a, "source", "web", "status", VENTURE_LEAD_QUALIFIED, NULL);
	g_object_set(b, "source", "web", NULL);
	save(f, a); save(f, b);
	g_object_get(a, "created-at", &created, NULL);
	first = g_date_time_add_seconds(created, 30);
	later = g_date_time_add_seconds(created, 60);
	yesterday = g_date_time_add_days(created, -1);
	g_object_set(outbound, "organization-id", f->org, "lead-id", venture_entity_get_id(a),
		"subject", "First reply", "outbound", TRUE, "occurred-at", first, NULL);
	g_object_set(inbound, "organization-id", f->org, "lead-id", venture_entity_get_id(a),
		"subject", "Inbound", "occurred-at", created, NULL);
	save(f, VENTURE_ENTITY(outbound)); save(f, VENTURE_ENTITY(inbound));
	/* A fresh object avoids reusing the first reply's UUID. */
	g_clear_object(&outbound);
	outbound = venture_interaction_new();
	g_object_set(outbound, "organization-id", f->org, "lead-id", venture_entity_get_id(a),
		"subject", "Later reply", "outbound", TRUE, "occurred-at", later, NULL);
	save(f, VENTURE_ENTITY(outbound));
	{
		gint64 id = venture_entity_get_id(a);
		g_clear_object(&a);
		a = venture_database_get(f->db, VENTURE_TYPE_LEAD, id, NULL);
	}
	converted = venture_lead_service_convert(venture_database_get_lead_service(f->db), a, NULL, NULL, NULL);
	g_assert_nonnull(converted);
	g_object_set(b, "status", VENTURE_LEAD_RECYCLED, "unqualified-reason", "Budget next quarter", "recycle-until", yesterday, NULL);
	save(f, b);
	sources = run_report(f, "lead_sources", NULL);
	g_assert_cmpfloat(metric(sources, "leads"), ==, 2);
	node = venture_report_result_to_json(sources);
	row = json_array_get_object_element(json_object_get_array_member(json_node_get_object(node), "rows"), 0);
	g_assert_cmpfloat(json_object_get_double_member(row, "conversion_rate"), ==, 50);
	g_assert_cmpint(json_object_get_int_member(row, "converted"), ==, 1);
	response = run_report(f, "lead_response_time", NULL);
	g_assert_cmpfloat(metric(response, "average_seconds"), ==, 30);
	g_assert_cmpfloat(metric(response, "unanswered"), ==, 1);
	due = run_report(f, "leads_recycled_due", NULL);
	g_assert_cmpfloat(metric(due, "leads_recycled_due"), ==, 1);
	g_clear_object(&sources);
	sources = run_report(f, "lead_sources", old_period);
	g_assert_cmpfloat(metric(sources, "leads"), ==, 0);
}

static void
test_strategy(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) rule = record(f, "lead_assignment_rule", "Rule");
	g_autoptr(VentureEntity) a = record(f, "lead", "A");
	g_autoptr(VentureEntity) b = record(f, "lead", "B");
	g_autofree gchar *owner = NULL;
	VentureRoutingStrategy strategy = GPOINTER_TO_INT(data);
	g_object_set(rule, "active", TRUE, "assignees", "alice,bob", "strategy", strategy, "source", "web", NULL);
	save(f, rule);
	g_object_set(a, "owner", "alice", NULL); save(f, a);
	g_object_set(b, "source", "web", NULL); save(f, b);
	g_object_get(b, "owner", &owner, NULL);
	g_assert_cmpstr(owner, ==, strategy == VENTURE_ROUTING_STRATEGY_FIRST ? "alice" : "bob");
	g_assert_true(venture_lead_service_reassign(venture_database_get_lead_service(f->db), b, "carol", NULL, NULL));
	g_clear_pointer(&owner, g_free);
	g_object_get(b, "owner", &owner, NULL);
	g_assert_cmpstr(owner, ==, "carol");
	g_assert_cmpint(count(f, "interaction"), ==, 1);
}

static void
test_contact_company_duplicates(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) existing = record(f, (const gchar *)data, "Existing");
	g_autoptr(VentureEntity) lead = record(f, "lead", "Inquiry");
	g_autoptr(GError) error = NULL;
	g_object_set(existing, "email", "Alice+old@EXAMPLE.com", NULL);
	save(f, existing);
	g_object_set(lead, "email", "alice@example.com", NULL);
	g_assert_false(venture_database_save(f->db, lead, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_ALREADY_EXISTS);
}

static void
test_capture_limits(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = record(f, "lead_form", "Web");
	(void)data;
	g_object_set(form, "public-token", "limits", "active", TRUE, NULL); save(f, form);
	g_object_set(f->config, "security-login-rate-limit", (gint64)2, NULL);
	start_http(f);
	g_assert_cmpuint(post(f, "/f/limits", "name=A", FALSE), ==, 200);
	g_assert_cmpuint(post(f, "/f/limits", "name=B", FALSE), ==, 200);
	g_assert_cmpuint(post(f, "/f/limits", "name=C", FALSE), ==, 429);
	g_assert_cmpint(count(f, "lead"), ==, 2);
}

static void
test_capture_policy(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = record(f, "lead_form", "Web");
	g_autoptr(JsonObject) fields = json_object_new();
	g_autoptr(GError) error = NULL;
	VentureLeadService *service = venture_database_get_lead_service(f->db);
	const gchar *policy = data;
	g_object_set(form, "public-token", "policy", "active", TRUE, "on-duplicate", policy, NULL); save(f, form);
	json_object_set_string_member(fields, "name", "A");
	json_object_set_string_member(fields, "email", "a@example.com");
	g_assert_true(venture_lead_service_capture(service, "policy", fields, NULL, &error));
	if (g_str_equal(policy, "reject"))
	{
		g_assert_false(venture_lead_service_capture(service, "policy", fields, NULL, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_ALREADY_EXISTS);
		g_assert_cmpint(count(f, "lead"), ==, 1);
	}
	else
	{
		g_assert_true(venture_lead_service_capture(service, "policy", fields, NULL, &error));
		g_assert_no_error(error);
		g_assert_cmpint(count(f, "lead"), ==, 2);
	}
}

static void
test_disabled(Fixture *f, gconstpointer data)
{
	(void)data;
	venture_config_set_module_enabled(f->config, "leads", FALSE);
	g_assert_cmpuint(venture_entity_registry_lookup(venture_entity_registry_get_default(), "lead"), ==, G_TYPE_INVALID);
	g_assert_null(venture_report_registry_lookup(venture_context_get_report_registry(f->context), "lead_sources"));
	start_http(f);
	g_assert_cmpuint(post(f, "/f/off", "name=A", FALSE), ==, 404);
	g_assert_cmpuint(post(f, "/api/v1/leads/1/convert", "{}", TRUE), ==, 404);
	venture_config_set_module_enabled(f->config, "leads", TRUE);
}

static GError *
veto_conversion(VentureLeadService *service, VentureEntity *lead, gpointer data)
{
	(void)service; (void)lead; (void)data;
	return g_error_new_literal(VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Conversion vetoed");
}

static void
converted_notice(VentureLeadService *service, VentureEntity *lead, gpointer data)
{
	guint *counted = data;
	(void)service; (void)lead;
	(*counted)++;
}

static void
test_conversion_signals(Fixture *f, gconstpointer data)
{
	VentureLeadService *service = venture_database_get_lead_service(f->db);
	g_autoptr(VentureEntity) lead = record(f, "lead", "A");
	g_autoptr(VentureEntity) converted = NULL;
	g_autoptr(GError) error = NULL;
	gulong veto;
	guint notices = 0;
	(void)data;
	g_assert_cmpuint(g_signal_lookup("converting", G_OBJECT_TYPE(service)), !=, 0);
	g_assert_cmpuint(g_signal_lookup("converted", G_OBJECT_TYPE(service)), !=, 0);
	g_object_set(lead, "status", VENTURE_LEAD_QUALIFIED, NULL); save(f, lead);
	g_signal_connect(service, "converted", G_CALLBACK(converted_notice), &notices);
	veto = g_signal_connect(service, "converting", G_CALLBACK(veto_conversion), NULL);
	converted = venture_lead_service_convert(service, lead, NULL, NULL, &error);
	g_assert_null(converted); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error); g_signal_handler_disconnect(service, veto);
	g_assert_cmpint(count(f, "company"), ==, 0);
	g_assert_true(venture_database_begin(f->db, &error));
	converted = venture_lead_service_convert(service, lead, NULL, NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(converted);
	g_assert_cmpuint(notices, ==, 0);
	venture_database_rollback(f->db);
	g_assert_cmpuint(notices, ==, 0); g_assert_cmpint(count(f, "company"), ==, 0);
	g_clear_object(&converted);
	converted = venture_lead_service_convert(service, lead, NULL, NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(converted);
	g_assert_cmpuint(notices, ==, 1);
	g_signal_handlers_disconnect_by_data(service, &notices);
}

static void
test_invalid_options(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) lead = record(f, "lead", "A");
	g_autoptr(JsonObject) options = json_object_new();
	g_autoptr(VentureEntity) converted = NULL;
	g_autoptr(GError) error = NULL;
	(void)data;
	g_object_set(lead, "status", VENTURE_LEAD_QUALIFIED, NULL); save(f, lead);
	json_object_set_int_member(options, "company_id", -1);
	converted = venture_lead_service_convert(venture_database_get_lead_service(f->db), lead, options, NULL, &error);
	g_assert_null(converted);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_cmpint(count(f, "company"), ==, 0);
}

static void
test_upgrade(Fixture *f, gconstpointer data)
{
	g_autofree gchar *sql = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) contact = record(f, "contact", "Retained customer");
	g_autoptr(VentureEntity) loaded = NULL;
	gint64 id;
	(void)data;
	g_assert_true(g_file_get_contents("migrations/sqlite/000101_leads.sql", &sql, NULL, &error));
	g_assert_no_error(error);
	save(f, contact); id = venture_entity_get_id(contact);
	venture_database_execute(f->db,
		"DELETE FROM schema_migrations WHERE version=101; DROP TABLE leads; DROP INDEX idx_interactions_lead_id; ALTER TABLE interactions DROP COLUMN lead_id", NULL, &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	loaded = venture_database_get(f->db, VENTURE_TYPE_CONTACT, id, &error);
	g_assert_no_error(error); g_assert_nonnull(loaded);
	g_assert_cmpint(venture_entity_get_version(loaded), ==, venture_entity_get_version(contact));
	/* The batch rejects inconsistent installed lead storage, and tolerates a disabled module. */
	g_assert_true(venture_database_execute(f->db, "ALTER TABLE leads DROP COLUMN company_name", NULL, &error));
	g_assert_true(venture_database_begin(f->db, &error));
	g_assert_false(venture_database_execute(f->db, sql, NULL, &error));
	venture_database_rollback(f->db);
	g_assert_nonnull(error); g_clear_error(&error);
	g_assert_true(venture_database_execute(f->db, "DROP TABLE leads", NULL, &error));
	g_assert_true(venture_database_execute(f->db, sql, NULL, &error));
	g_assert_no_error(error);
}

static void
test_redirect(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = record(f, "lead_form", "Web");
	(void)data;
	g_object_set(form, "public-token", "redirect", "active", TRUE,
		"redirect-url", "https://example.com/thanks", "honeypot", "fax", NULL); save(f, form);
	start_http(f);
	g_assert_cmpuint(post(f, "/f/redirect", "name=A", FALSE), ==, 302);
	g_assert_cmpuint(post(f, "/f/redirect", "name=Robot&fax=spam", FALSE), ==, 200);
}

static void
test_activity_clock(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) lead = record(f, "lead", "A");
	g_autoptr(VentureEntity) stored = NULL;
	g_autoptr(VentureInteraction) interaction = venture_interaction_new();
	g_autoptr(GDateTime) created = NULL;
	g_autoptr(GDateTime) later = NULL;
	g_autoptr(GDateTime) activity = NULL;
	(void)data;
	save(f, lead);
	g_object_get(lead, "created-at", &created, NULL);
	later = g_date_time_add_seconds(created, 60);
	g_object_set(interaction, "organization-id", f->org, "lead-id", venture_entity_get_id(lead),
		"subject", "Follow-up", "outbound", TRUE, "occurred-at", later, NULL);
	save(f, VENTURE_ENTITY(interaction));
	stored = venture_database_get(f->db, VENTURE_TYPE_LEAD, venture_entity_get_id(lead), NULL);
	g_object_get(stored, "last-activity-at", &activity, NULL);
	g_assert_cmpint(g_date_time_compare(activity, later), ==, 0);
}

static void
test_organization_scope(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) organization = record(f, "organization", "Other organization");
	g_autoptr(VentureEntity) company = record(f, "company", "Existing company");
	g_autoptr(VentureEntity) a = record(f, "lead", "A");
	g_autoptr(VentureEntity) b = record(f, "lead", "B");
	g_autoptr(VentureEntity) rule = record(f, "lead_assignment_rule", "Private rota");
	g_autoptr(VentureEntity) converted = NULL;
	g_autoptr(JsonObject) options = json_object_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *owner = NULL;
	(void)data;
	save(f, organization); save(f, company);
	g_object_set(rule, "assignees", "private-owner", "active", TRUE, NULL); save(f, rule);
	g_object_set(a, "email", "a@example.com", NULL); save(f, a);
	g_object_set(b, "organization-id", venture_entity_get_id(organization), "email", "a@example.com",
		"status", VENTURE_LEAD_QUALIFIED, NULL); save(f, b);
	g_object_get(b, "owner", &owner, NULL); g_assert_true(venture_string_is_empty(owner));
	json_object_set_int_member(options, "company_id", venture_entity_get_id(company));
	converted = venture_lead_service_convert(venture_database_get_lead_service(f->db), b, options, NULL, &error);
	g_assert_null(converted); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_cmpint(count(f, "company"), ==, 1); g_assert_cmpint(count(f, "contact"), ==, 0);
}

static void
test_stale_approval(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) lead = record(f, "lead", "A");
	g_autoptr(GError) error = NULL;
	g_autofree gchar *id = NULL;
	VentureConfirmation *confirmation;
	VentureConfirmationStore *store = venture_context_get_confirmations(f->context);
	VentureActor actor;
	(void)data;
	actor.kind = VENTURE_ACTOR_KIND_AI; actor.name = "assistant"; actor.prompt = "Convert this lead";
	actor.request_id = NULL; actor.approved_by = NULL;
	g_object_set(lead, "status", VENTURE_LEAD_QUALIFIED, NULL); save(f, lead);
	confirmation = venture_lead_service_stage_convert(venture_database_get_lead_service(f->db), store, lead, NULL, &actor, &error);
	g_assert_no_error(error); g_assert_nonnull(confirmation);
	id = g_strdup(venture_confirmation_get_id(confirmation));
	g_object_set(lead, "notes", "Changed after proposal", NULL); save(f, lead);
	g_assert_false(venture_confirmation_store_approve(store, id, "operator", &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	g_assert_cmpint(count(f, "company"), ==, 0);
	g_assert_null(venture_confirmation_store_find(store, id));
}

static void
test_ui_convert(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) lead = record(f, "lead", "A");
	g_autofree gchar *path = NULL;
	(void)data;
	g_object_set(lead, "status", VENTURE_LEAD_QUALIFIED, NULL); save(f, lead);
	start_http(f);
	path = g_strdup_printf("/leads/%" G_GINT64_FORMAT "/convert", venture_entity_get_id(lead));
	g_assert_cmpuint(post(f, path, "", FALSE), ==, 302);
	g_assert_cmpint(count(f, "company"), ==, 1);
	g_assert_cmpint(count(f, "contact"), ==, 1);
	g_assert_cmpint(count(f, "deal"), ==, 1);
}

static void
test_assistant_tool(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureAiService) ai = NULL;
	g_autoptr(JsonNode) tools = NULL;
	g_autofree gchar *text = NULL;
	g_autoptr(GError) error = NULL;
	(void)data;
	g_setenv("VENTURE_LEADS_TEST_KEY", "fixture-key-not-a-credential", TRUE);
	g_object_set(f->config, "ai-api-key-env", "VENTURE_LEADS_TEST_KEY", NULL);
	ai = venture_ai_service_new(f->context, &error);
	g_assert_no_error(error); g_assert_nonnull(ai);
	tools = venture_ai_service_describe_tools(ai);
	text = venture_json_to_string(tools, FALSE);
	g_assert_nonnull(strstr(text, "venture_lead_convert"));
	g_clear_object(&ai); g_clear_pointer(&tools, json_node_unref); g_clear_pointer(&text, g_free);
	g_object_set(f->config, "ai-policy", VENTURE_AI_POLICY_READ_ONLY, NULL);
	ai = venture_ai_service_new(f->context, &error);
	g_assert_no_error(error); g_assert_nonnull(ai);
	tools = venture_ai_service_describe_tools(ai);
	text = venture_json_to_string(tools, FALSE);
	g_assert_null(strstr(text, "venture_lead_convert"));
	g_unsetenv("VENTURE_LEADS_TEST_KEY");
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
	g_test_add("/leads/report-values", Fixture, NULL, setup, test_report_values, teardown);
	g_test_add("/leads/least-busy", Fixture, GINT_TO_POINTER(VENTURE_ROUTING_STRATEGY_LEAST_BUSY), setup, test_strategy, teardown);
	g_test_add("/leads/first-assignment", Fixture, GINT_TO_POINTER(VENTURE_ROUTING_STRATEGY_FIRST), setup, test_strategy, teardown);
	g_test_add("/leads/contact-duplicate", Fixture, "contact", setup, test_contact_company_duplicates, teardown);
	g_test_add("/leads/company-duplicate", Fixture, "company", setup, test_contact_company_duplicates, teardown);
	g_test_add("/leads/capture-limits", Fixture, NULL, setup, test_capture_limits, teardown);
	g_test_add("/leads/capture-reject", Fixture, "reject", setup, test_capture_policy, teardown);
	g_test_add("/leads/capture-create", Fixture, "create", setup, test_capture_policy, teardown);
	g_test_add("/leads/disabled", Fixture, NULL, setup, test_disabled, teardown);
	g_test_add("/leads/conversion-signals", Fixture, NULL, setup, test_conversion_signals, teardown);
	g_test_add("/leads/invalid-options", Fixture, NULL, setup, test_invalid_options, teardown);
	g_test_add("/leads/upgrade", Fixture, NULL, setup, test_upgrade, teardown);
	g_test_add("/leads/redirect", Fixture, NULL, setup, test_redirect, teardown);
	g_test_add("/leads/activity-clock", Fixture, NULL, setup, test_activity_clock, teardown);
	g_test_add("/leads/organization-scope", Fixture, NULL, setup, test_organization_scope, teardown);
	g_test_add("/leads/stale-approval", Fixture, NULL, setup, test_stale_approval, teardown);
	g_test_add("/leads/ui-convert", Fixture, NULL, setup, test_ui_convert, teardown);
	g_test_add("/leads/assistant-tool", Fixture, NULL, setup, test_assistant_tool, teardown);
	return g_test_run();
}
