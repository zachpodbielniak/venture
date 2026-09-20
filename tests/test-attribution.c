/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <string.h>
#include <libsoup/soup.h>
#include "venture-test-accounting.h"
#include "venture-test-util.h"
#include "db/venture-migrations.h"

/* Every surface must discover the same owned site and retained evidence. */
static void test_records(void)
{
	const gchar *names[] = { "attribution_site", "attribution_visitor", "attribution_touch",
		"attribution_submission", "attribution_binding" };
	guint i;
	for (i = 0; i < G_N_ELEMENTS(names); i++)
		g_assert_cmpuint(venture_entity_registry_lookup(venture_entity_registry_get_default(), names[i]), !=, G_TYPE_INVALID);
}
/* A generic import must not manufacture analytics permission or evidence. */
static void test_evidence_guard(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDatabase) database = venture_test_accounting_database(&error);
	g_autoptr(VentureEntity) visitor = g_object_new(VENTURE_TYPE_ATTRIBUTION_VISITOR,
		"organization-id", (gint64)1, "token-hash", "forged-consent", NULL);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(database, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	g_assert_false(venture_database_save(database, visitor, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	venture_test_accounting_database_cleanup(database);
}
typedef struct {
	VentureDatabase *db;
	VentureAttributionService *service;
	VentureEntity *form;
	VentureEntity *site;
	GDateTime *now;
	gchar *directory, *uri;
} Fixture;
static void persist(Fixture *f, VentureEntity *row)
{
	g_autoptr(GError) error = NULL;
	if (!venture_database_save(f->db, row, NULL, &error)) g_error("Persist %s: %s", G_OBJECT_TYPE_NAME(row), error ? error->message : "unknown error");
	g_assert_no_error(error);
}
static const gchar restart_mode[] = "restart";
static void setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	f->directory = NULL; f->uri = NULL;
	if (data == restart_mode && !g_getenv("VENTURE_TEST_ACCOUNTING_POSTGRES_URI")) {
		f->directory = g_dir_make_tmp("venture-attribution-restart-XXXXXX", &error); g_assert_no_error(error);
		f->uri = g_strdup_printf("sqlite://%s/attribution.db", f->directory);
		f->db = venture_database_new(f->uri, &error);
	} else f->db = venture_test_accounting_database(&error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error)); g_assert_no_error(error);
	f->service = venture_attribution_service_get(f->db); f->now = venture_time_now();
	f->form = g_object_new(VENTURE_TYPE_LEAD_FORM, "organization-id", (gint64)1,
		"name", "Site inquiry", "active", TRUE, "honeypot", "company_fax", NULL); persist(f, f->form);
	f->site = g_object_new(VENTURE_TYPE_ATTRIBUTION_SITE, "organization-id", (gint64)1,
		"name", "Portfolio site", "origin", "https://site.example.test", "external-site-id", "site_one",
		"external-tenant-id", "tenant_one", "lead-form-id", venture_entity_get_id(f->form),
		"consent-policy", "analytics-v1", "active", TRUE, NULL); persist(f, f->site);
}
static void teardown(Fixture *f, gconstpointer data)
{
	g_clear_object(&f->site); g_clear_object(&f->form); g_clear_pointer(&f->now, g_date_time_unref);
	venture_test_accounting_database_cleanup(f->db); g_clear_object(&f->db);
	if (f->directory) venture_test_remove_tree(f->directory);
	g_free(f->directory); g_free(f->uri);
}
static GPtrArray *all_rows(Fixture *f, GType type)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	g_autoptr(GError) error = NULL;
	GPtrArray *rows;
	venture_query_set_organization(query, 1); venture_query_set_limit(query, 0);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	rows = venture_database_find(f->db, query, &error); g_assert_no_error(error); g_assert_nonnull(rows); return rows;
}
static gchar *grant(Fixture *f)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) response = venture_attribution_service_grant(f->service,
		venture_entity_get_uuid(f->site), "https://site.example.test", "analytics-v1", f->now, &error);
	g_assert_no_error(error); g_assert_nonnull(response);
	return g_strdup(json_object_get_string_member(json_node_get_object(response), "token"));
}
static JsonObject *page(void)
{
	JsonObject *fields = json_object_new();
	json_object_set_string_member(fields, "page", "https://site.example.test/offer?email=never-store@example.test#private");
	json_object_set_string_member(fields, "referrer", "https://search.example.test/query?private=yes");
	json_object_set_string_member(fields, "utm_source", "newsletter");
	json_object_set_string_member(fields, "utm_medium", "email");
	return fields;
}
/* Known email permission must never be an anonymous tracking capability. */
static void test_consent(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonObject) fields = page();
	g_autoptr(JsonNode) invalid = NULL;
	g_autoptr(VentureAttributionTouch) touch = NULL;
	g_autoptr(GPtrArray) visitors = NULL, marketing = NULL;
	g_autofree gchar *token = NULL, *path = NULL, *referrer = NULL;
	touch = venture_attribution_service_observe(f->service, venture_entity_get_uuid(f->site),
		"https://site.example.test", "unconsented", "page-1", fields, f->now, &error);
	g_assert_null(touch); g_assert_nonnull(error); g_clear_error(&error);
	invalid = venture_attribution_service_grant(f->service, venture_entity_get_uuid(f->site),
		"https://attacker.example.test", "analytics-v1", f->now, &error);
	g_assert_null(invalid); g_assert_nonnull(error); g_clear_error(&error);
	invalid = venture_attribution_service_grant(f->service, venture_entity_get_uuid(f->site),
		"https://site.example.test", "email-marketing", f->now, &error);
	g_assert_null(invalid); g_assert_nonnull(error); g_clear_error(&error);
	token = grant(f); g_assert_cmpuint(strlen(token), ==, 64);
	touch = venture_attribution_service_observe(f->service, venture_entity_get_uuid(f->site),
		"https://site.example.test", token, "page-1", fields, f->now, &error);
	g_assert_no_error(error); g_assert_nonnull(touch);
	g_object_get(touch, "path", &path, "referrer-origin", &referrer, NULL);
	g_assert_cmpstr(path, ==, "/offer"); g_assert_cmpstr(referrer, ==, "https://search.example.test");
	visitors = all_rows(f, VENTURE_TYPE_ATTRIBUTION_VISITOR); marketing = all_rows(f, VENTURE_TYPE_MARKETING_CONSENT);
	g_assert_cmpuint(visitors->len, ==, 1); g_assert_cmpuint(marketing->len, ==, 0);
	g_test_message("Explicit anonymous analytics consent creates one visitor; email permission remains absent; page/referrer query strings discarded");
}
/* Network retries preserve one observation; changed payloads cannot steal its identity. */
static void test_observation_replay(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *token = grant(f);
	g_autoptr(JsonObject) fields = page();
	g_autoptr(VentureAttributionTouch) first = NULL, repeated = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	first = venture_attribution_service_observe(f->service, venture_entity_get_uuid(f->site),
		"https://site.example.test", token, "page-1", fields, f->now, &error);
	g_assert_no_error(error); g_assert_nonnull(first);
	repeated = venture_attribution_service_observe(f->service, venture_entity_get_uuid(f->site),
		"https://site.example.test", token, "page-1", fields, f->now, &error);
	g_assert_no_error(error); g_assert_cmpint(venture_entity_get_id(VENTURE_ENTITY(first)), ==, venture_entity_get_id(VENTURE_ENTITY(repeated)));
	g_clear_object(&repeated); json_object_set_string_member(fields, "utm_source", "changed");
	repeated = venture_attribution_service_observe(f->service, venture_entity_get_uuid(f->site),
		"https://site.example.test", token, "page-1", fields, f->now, &error);
	g_assert_null(repeated); g_assert_nonnull(error); g_clear_error(&error);
	rows = all_rows(f, VENTURE_TYPE_ATTRIBUTION_TOUCH); g_assert_cmpuint(rows->len, ==, 1);
}
/* A retained site cannot be relabeled into another remote identity. */
static void test_site_identity(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_object_set(f->site, "external-tenant-id", "another_tenant", NULL);
	g_assert_false(venture_database_save(f->db, f->site, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}
/* Withdrawal is effective after enqueue-style observation races and erases navigation linkage. */
static void test_withdrawal(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *token = grant(f), *path = NULL;
	g_autoptr(JsonObject) fields = page();
	g_autoptr(VentureAttributionTouch) touch = venture_attribution_service_observe(f->service,
		venture_entity_get_uuid(f->site), "https://site.example.test", token, "page-1", fields, f->now, &error);
	g_autoptr(VentureEntity) retained = NULL;
	gint64 visitor = -1;
	g_assert_no_error(error); g_assert_nonnull(touch);
	g_assert_true(venture_attribution_service_withdraw(f->service, venture_entity_get_uuid(f->site), token, f->now, &error));
	g_assert_no_error(error);
	g_assert_true(venture_attribution_service_withdraw(f->service, venture_entity_get_uuid(f->site), token, f->now, &error));
	g_assert_no_error(error);
	retained = venture_database_get(f->db, VENTURE_TYPE_ATTRIBUTION_TOUCH, venture_entity_get_id(VENTURE_ENTITY(touch)), &error);
	g_assert_no_error(error); g_object_get(retained, "visitor-id", &visitor, "path", &path, NULL);
	g_assert_cmpint(visitor, ==, 0); g_assert_true(!path || !*path);
	g_clear_object(&touch);
	touch = venture_attribution_service_observe(f->service, venture_entity_get_uuid(f->site),
		"https://site.example.test", token, "page-2", fields, f->now, &error);
	g_assert_null(touch); g_assert_nonnull(error);
	g_test_message("Repeated withdrawal retains one decision; visitor linkage/path removed and subsequent observation refused");
}
static void test_rate_limit(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *token = grant(f);
	g_autoptr(JsonObject) fields = page();
	g_autoptr(VentureAttributionTouch) refused = NULL;
	g_autoptr(GDateTime) later = g_date_time_add_seconds(f->now, 61);
	g_autoptr(GPtrArray) rows = NULL;
	guint i;
	for (i = 0; i < 60; i++) {
		g_autofree gchar *key = g_strdup_printf("page-%u", i);
		g_autoptr(VentureAttributionTouch) touch = venture_attribution_service_observe(f->service,
			venture_entity_get_uuid(f->site), "https://site.example.test", token, key, fields, f->now, &error);
		g_assert_no_error(error); g_assert_nonnull(touch);
	}
	refused = venture_attribution_service_observe(f->service, venture_entity_get_uuid(f->site),
		"https://site.example.test", token, "page-60", fields, f->now, &error);
	g_assert_null(refused); g_assert_error(error, G_IO_ERROR, G_IO_ERROR_BUSY); g_clear_error(&error);
	refused = venture_attribution_service_observe(f->service, venture_entity_get_uuid(f->site),
		"https://site.example.test", token, "page-60", fields, later, &error);
	g_assert_no_error(error); g_assert_nonnull(refused);
	rows = all_rows(f, VENTURE_TYPE_ATTRIBUTION_TOUCH); g_assert_cmpuint(rows->len, ==, 61);
}
static void test_expiration(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *token = grant(f);
	g_autoptr(GDateTime) later = g_date_time_add_days(f->now, 31);
	g_autoptr(JsonObject) fields = page();
	g_autoptr(VentureAttributionTouch) refused = venture_attribution_service_observe(f->service,
		venture_entity_get_uuid(f->site), "https://site.example.test", token, "expired", fields, later, &error);
	g_assert_null(refused); g_assert_nonnull(error); g_clear_error(&error);
	g_assert_cmpint(venture_attribution_service_sweep(f->service, 1, 1, later, NULL, &error), ==, 1);
	g_assert_no_error(error);
	g_assert_cmpint(venture_attribution_service_sweep(f->service, 1, 1, later, NULL, &error), ==, 0);
	g_assert_no_error(error);
	g_assert_false(venture_attribution_service_withdraw(f->service, venture_entity_get_uuid(f->site), token, later, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
}
static void test_disabled_withdrawal(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureContext) context = venture_context_new(config, f->db);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *token = grant(f);
	venture_config_set_module_enabled(config, "attribution", FALSE);
	g_assert_true(venture_attribution_service_withdraw(f->service, venture_entity_get_uuid(f->site), token, f->now, &error));
	g_assert_no_error(error);
	venture_config_set_module_enabled(config, "attribution", TRUE);
}
/* Verified attribution enters the existing capture transaction, not a second CRM writer. */
static void test_capture_result(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonObject) fields = json_object_new();
	g_autoptr(VentureEntity) captured = NULL, repeated = NULL;
	g_autoptr(VentureEntity) campaign = g_object_new(VENTURE_TYPE_CAMPAIGN, "organization-id", (gint64)1, "name", "Autumn", NULL);
	g_autofree gchar *token = NULL, *source = NULL;
	gint64 campaign_id = 0;
	persist(f, campaign); g_object_get(f->form, "public-token", &token, NULL);
	json_object_set_string_member(fields, "name", "Captured Visitor"); json_object_set_string_member(fields, "email", "visitor@example.test");
	json_object_set_string_member(fields, "source", "spoofed-body-source");
	g_assert_true(venture_lead_service_capture_result(venture_database_get_lead_service(f->db), token, fields,
		"verified-source", venture_entity_get_id(campaign), &captured, NULL, &error));
	g_assert_no_error(error); g_assert_true(VENTURE_IS_LEAD(captured));
	g_object_get(captured, "source", &source, "campaign-id", &campaign_id, NULL);
	g_assert_cmpstr(source, ==, "verified-source"); g_assert_cmpint(campaign_id, ==, venture_entity_get_id(campaign));
	g_assert_true(venture_lead_service_capture_result(venture_database_get_lead_service(f->db), token, fields,
		"later-source", 0, &repeated, NULL, &error));
	g_assert_no_error(error); g_assert_cmpint(venture_entity_get_id(repeated), ==, venture_entity_get_id(captured));
	g_clear_pointer(&source, g_free); g_object_get(repeated, "source", &source, NULL); g_assert_cmpstr(source, ==, "verified-source");
}
static JsonObject *submission(const gchar *id, const gchar *token)
{
	JsonObject *payload = json_object_new(), *fields = json_object_new();
	json_object_set_int_member(payload, "version", 1); json_object_set_string_member(payload, "submission_id", id);
	if (token) json_object_set_string_member(payload, "visitor_token", token);
	json_object_set_string_member(fields, "name", "Form Visitor"); json_object_set_string_member(fields, "email", "form@example.test");
	json_object_set_string_member(fields, "message", "Please contact me about the service.");
	json_object_set_object_member(payload, "fields", fields); return payload;
}
/* Retries must not create another lead, history event or permission grant. */
static void test_form_capture(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *token = grant(f), *source = NULL;
	g_autoptr(JsonObject) fields = page(), payload = submission("lightsite-event-1", token);
	g_autoptr(VentureAttributionTouch) touch = venture_attribution_service_observe(f->service,
		venture_entity_get_uuid(f->site), "https://site.example.test", token, "page-1", fields, f->now, &error);
	g_autoptr(VentureAttributionSubmission) first = NULL, repeated = NULL;
	g_autoptr(GPtrArray) leads = NULL, history = NULL, consent = NULL;
	gint64 lead_id = 0;
	g_assert_no_error(error); g_assert_nonnull(touch);
	first = venture_attribution_service_capture(f->service, venture_entity_get_uuid(f->site), "https://site.example.test", payload, f->now, &error);
	g_assert_no_error(error); g_assert_nonnull(first);
	repeated = venture_attribution_service_capture(f->service, venture_entity_get_uuid(f->site), "https://site.example.test", payload, f->now, &error);
	g_assert_no_error(error); g_assert_nonnull(repeated);
	g_assert_cmpint(venture_entity_get_id(VENTURE_ENTITY(first)), ==, venture_entity_get_id(VENTURE_ENTITY(repeated)));
	g_object_get(first, "lead-id", &lead_id, "first-source", &source, NULL);
	g_assert_cmpint(lead_id, >, 0); g_assert_cmpstr(source, ==, "newsletter");
	leads = all_rows(f, VENTURE_TYPE_LEAD); history = all_rows(f, VENTURE_TYPE_INTERACTION); consent = all_rows(f, VENTURE_TYPE_MARKETING_CONSENT);
	g_assert_cmpuint(leads->len, ==, 1); g_assert_cmpuint(consent->len, ==, 0);
	g_clear_object(&repeated); json_object_set_string_member(json_object_get_object_member(payload, "fields"), "email", "changed@example.test");
	repeated = venture_attribution_service_capture(f->service, venture_entity_get_uuid(f->site), "https://site.example.test", payload, f->now, &error);
	g_assert_null(repeated); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	g_test_message("Consented form retained newsletter source; exact retry returned one submission/lead and created no email consent");
}
static void email_permission(Fixture *f, JsonObject *payload)
{
	JsonObject *permission = json_object_new();
	g_autofree gchar *at = venture_time_to_string(f->now);
	g_object_set(f->site, "marketing-policy", "email-v1", "marketing-statement", "I request this organization's email news", NULL); persist(f, f->site);
	json_object_set_boolean_member(permission, "granted", TRUE);
	json_object_set_string_member(permission, "policy", "email-v1");
	json_object_set_string_member(permission, "statement", "I request this organization's email news");
	json_object_set_string_member(permission, "occurred_at", at);
	json_object_set_object_member(payload, "marketing", permission);
}
/* Phone/website dedupe cannot transfer a different mailbox's permission. */
static void test_capture_address_permission(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) known = g_object_new(VENTURE_TYPE_CONTACT, "organization-id", (gint64)1,
		"name", "Existing contact", "email", "original@example.test", "phone", "+15555550123", NULL);
	g_autoptr(JsonObject) payload = submission("different-address", NULL);
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureAttributionSubmission) captured = NULL;
	g_autoptr(GPtrArray) consents = NULL;
	persist(f, known); email_permission(f, payload);
	json_object_set_string_member(json_object_get_object_member(payload, "fields"), "phone", "+15555550123");
	captured = venture_attribution_service_capture(f->service, venture_entity_get_uuid(f->site), "https://site.example.test", payload, f->now, &error);
	g_assert_no_error(error); g_assert_nonnull(captured);
	consents = all_rows(f, VENTURE_TYPE_MARKETING_CONSENT); g_assert_cmpuint(consents->len, ==, 0);
	g_test_message("Phone dedupe retained inquiry but did not grant the old address permission claimed for a different mailbox");
}
static void test_conversion_binding(Fixture *f, gconstpointer data)
{
	g_autoptr(JsonObject) payload = submission("convert-one", NULL), options = json_object_new();
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureAttributionSubmission) captured = venture_attribution_service_capture(f->service,
		venture_entity_get_uuid(f->site), "https://site.example.test", payload, f->now, &error);
	g_autoptr(VentureEntity) lead = NULL, converted = NULL;
	g_autoptr(GPtrArray) bindings = NULL;
	g_autoptr(GDateTime) bound = NULL;
	gint64 lead_id = 0, company = 0, deal = 0;
	g_assert_no_error(error); g_assert_nonnull(captured); g_object_get(captured, "lead-id", &lead_id, NULL);
	lead = venture_database_get(f->db, VENTURE_TYPE_LEAD, lead_id, &error); g_assert_no_error(error);
	g_object_set(lead, "status", VENTURE_LEAD_QUALIFIED, NULL); persist(f, lead);
	json_object_set_boolean_member(options, "deal", TRUE);
	converted = venture_lead_service_convert(venture_database_get_lead_service(f->db), lead, options, NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(converted);
	bindings = all_rows(f, VENTURE_TYPE_ATTRIBUTION_BINDING); g_assert_cmpuint(bindings->len, ==, 1);
	g_object_get(g_ptr_array_index(bindings, 0), "company-id", &company, "deal-id", &deal, "bound-at", &bound, NULL);
	g_assert_cmpint(company, >, 0); g_assert_cmpint(deal, >, 0); g_assert_nonnull(bound);
	g_test_message("Canonical conversion linked retained acquisition to one company/contact/deal inside the existing transaction");
}
typedef struct { gint64 contact; gboolean invoked; } CaptureHook;
static void replace_email_on_history(VentureDatabase *database, VentureEntity *row, gboolean created, gpointer data)
{
	CaptureHook *hook = data;
	g_autoptr(VentureEntity) contact = NULL;
	g_autoptr(GError) error = NULL;
	if (!VENTURE_IS_INTERACTION(row) || hook->invoked) return;
	hook->invoked = TRUE;
	contact = venture_database_get(database, VENTURE_TYPE_CONTACT, hook->contact, &error); g_assert_no_error(error);
	g_object_set(contact, "email", "changed-during-capture@example.test", NULL);
	g_assert_true(venture_database_save(database, contact, NULL, &error)); g_assert_no_error(error);
}
static void test_capture_address_recheck(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) known = g_object_new(VENTURE_TYPE_CONTACT, "organization-id", (gint64)1,
		"name", "Existing contact", "email", "form@example.test", NULL);
	g_autoptr(JsonObject) payload = submission("reentrant-address", NULL);
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureAttributionSubmission) captured = NULL;
	g_autoptr(GPtrArray) consents = NULL;
	CaptureHook hook;
	gulong handler;
	persist(f, known); email_permission(f, payload); hook.contact = venture_entity_get_id(known); hook.invoked = FALSE;
	handler = g_signal_connect(f->db, "entity-saved", G_CALLBACK(replace_email_on_history), &hook);
	captured = venture_attribution_service_capture(f->service, venture_entity_get_uuid(f->site), "https://site.example.test", payload, f->now, &error);
	g_signal_handler_disconnect(f->db, handler);
	g_assert_no_error(error); g_assert_nonnull(captured); g_assert_true(hook.invoked);
	consents = all_rows(f, VENTURE_TYPE_MARKETING_CONSENT); g_assert_cmpuint(consents->len, ==, 0);
}
static const gchar signing_secret[] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
static VentureIntegrationConnection *connect_site(Fixture *f)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) key = g_bytes_new_static(signing_secret, 32);
	g_autoptr(JsonNode) settings = json_from_string("{\"tenant_id\":\"tenant_one\",\"environment\":\"test\",\"signing_secret\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"}", &error);
	VentureIntegrationConnection *connection;
	g_assert_no_error(error);
	g_assert_true(venture_integration_service_set_key(venture_integration_service_get(f->db), key, &error)); g_assert_no_error(error);
	connection = venture_attribution_settings_configure(f->db, 1, settings, 0, 0, NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(connection);
	g_object_set(f->site, "connection-id", venture_entity_get_id(VENTURE_ENTITY(connection)), NULL); persist(f, f->site);
	return connection;
}
static gchar *sign_body(const gchar *timestamp, const gchar *body)
{
	g_autofree gchar *material = g_strdup_printf("%s\n%s", timestamp, body);
	return g_compute_hmac_for_string(G_CHECKSUM_SHA256, (const guchar *)signing_secret, strlen(signing_secret), material, -1);
}
/* The verified Lightsite event's tenant/site cannot be replaced by caller IDs. */
static void test_signed_capture(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureIntegrationConnection) connection = connect_site(f);
	g_autoptr(JsonObject) payload = submission("lightsite-original-uuid", NULL);
	g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) bytes = NULL;
	g_autoptr(VentureAttributionSubmission) captured = NULL, repeated = NULL;
	g_autofree gchar *body = NULL, *signature = NULL, *timestamp = g_strdup_printf("%" G_GINT64_FORMAT, g_date_time_to_unix(f->now));
	gint64 id = venture_entity_get_id(VENTURE_ENTITY(connection)), stored = 0;
	json_object_set_string_member(payload, "tenant_id", "tenant_one");
	json_object_set_string_member(payload, "site_id", "site_one");
	json_object_set_string_member(payload, "origin", "https://site.example.test");
	json_node_set_object(node, payload); body = json_to_string(node, FALSE); bytes = g_bytes_new(body, strlen(body)); signature = sign_body(timestamp, body);
	captured = venture_attribution_service_receive(f->service, venture_entity_get_uuid(f->site), id, timestamp, signature, bytes, f->now, &error);
	g_assert_no_error(error); g_assert_nonnull(captured);
	g_object_get(captured, "connection-id", &stored, NULL); g_assert_cmpint(stored, ==, id);
	repeated = venture_attribution_service_receive(f->service, venture_entity_get_uuid(f->site), id, timestamp, signature, bytes, f->now, &error);
	g_assert_no_error(error); g_assert_nonnull(repeated);
	g_assert_cmpint(venture_entity_get_id(VENTURE_ENTITY(captured)), ==, venture_entity_get_id(VENTURE_ENTITY(repeated)));
	g_clear_object(&repeated); signature[0] = signature[0] == 'a' ? 'b' : 'a';
	repeated = venture_attribution_service_receive(f->service, venture_entity_get_uuid(f->site), id, timestamp, signature, bytes, f->now, &error);
	g_assert_null(repeated); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
}
static void test_signed_boundaries(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureIntegrationConnection) connection = connect_site(f), rotated = NULL;
	g_autoptr(JsonObject) payload = submission("signed-boundaries", NULL);
	g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT), settings = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) bytes = NULL;
	g_autoptr(VentureAttributionSubmission) result = NULL;
	g_autofree gchar *body = NULL, *signature = NULL, *timestamp = g_strdup_printf("%" G_GINT64_FORMAT, g_date_time_to_unix(f->now));
	g_autoptr(GDateTime) later = g_date_time_add_seconds(f->now, 301);
	gint64 id = venture_entity_get_id(VENTURE_ENTITY(connection));
	json_object_set_string_member(payload, "tenant_id", "wrong_tenant");
	json_object_set_string_member(payload, "site_id", "site_one");
	json_object_set_string_member(payload, "origin", "https://site.example.test");
	json_node_set_object(node, payload); body = json_to_string(node, FALSE); bytes = g_bytes_new(body, strlen(body)); signature = sign_body(timestamp, body);
	result = venture_attribution_service_receive(f->service, venture_entity_get_uuid(f->site), id, timestamp, signature, bytes, f->now, &error);
	g_assert_null(result); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION); g_clear_error(&error);
	json_object_set_string_member(payload, "tenant_id", "tenant_one");
	g_clear_pointer(&body, g_free); g_clear_pointer(&bytes, g_bytes_unref); g_clear_pointer(&signature, g_free);
	body = json_to_string(node, FALSE); bytes = g_bytes_new(body, strlen(body)); signature = sign_body(timestamp, body);
	result = venture_attribution_service_receive(f->service, venture_entity_get_uuid(f->site), id, timestamp, signature, bytes, later, &error);
	g_assert_null(result); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED); g_clear_error(&error);
	result = venture_attribution_service_receive(f->service, venture_entity_get_uuid(f->site), id + 1, timestamp, signature, bytes, f->now, &error);
	g_assert_null(result); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION); g_clear_error(&error);
	settings = json_from_string("{\"tenant_id\":\"tenant_one\",\"environment\":\"test\",\"signing_secret\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}", &error); g_assert_no_error(error);
	rotated = venture_attribution_settings_configure(f->db, 1, settings, venture_entity_get_version(VENTURE_ENTITY(connection)), id, NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(rotated);
	result = venture_attribution_service_receive(f->service, venture_entity_get_uuid(f->site), id, timestamp, signature, bytes, f->now, &error);
	g_assert_null(result); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED); g_clear_error(&error);
	g_assert_true(venture_integration_service_disable(venture_integration_service_get(f->db), 1, id, venture_entity_get_version(VENTURE_ENTITY(rotated)), NULL, &error)); g_assert_no_error(error);
	result = venture_attribution_service_receive(f->service, venture_entity_get_uuid(f->site), id, timestamp, signature, bytes, f->now, &error);
	g_assert_null(result); g_assert_nonnull(error);
	{
		g_autoptr(GPtrArray) rows = all_rows(f, VENTURE_TYPE_LEAD); g_assert_cmpuint(rows->len, ==, 0);
	}
}
static void test_site_removal(Fixture *f, gconstpointer data)
{
	g_autofree gchar *token = grant(f);
	g_autoptr(GError) error = NULL;
	g_assert_false(venture_database_delete(f->db, f->site, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION); g_clear_error(&error);
	g_assert_true(venture_attribution_service_withdraw(f->service, venture_entity_get_uuid(f->site), token, f->now, &error)); g_assert_no_error(error);
}
typedef struct { gboolean done; GBytes *body; GError *error; } AttributionReply;
static void attribution_reply(GObject *source, GAsyncResult *result, gpointer data)
{
	AttributionReply *reply = data;
	reply->body = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &reply->error); reply->done = TRUE;
}
static guint attribution_http_with_host(SoupSession *session, const gchar *base, const gchar *method, const gchar *path,
	const gchar *origin, const gchar *payload, gchar **text, gchar **cors, const gchar *host)
{
	g_autofree gchar *url = g_strconcat(base, path, NULL);
	g_autoptr(SoupMessage) message = soup_message_new(method, url);
	AttributionReply reply = { FALSE, NULL, NULL };
	guint status;
	if (payload) {
		g_autoptr(GBytes) bytes = g_bytes_new(payload, strlen(payload));
		soup_message_set_request_body_from_bytes(message, payload[0] == '{' ? "text/plain;charset=UTF-8" : "application/x-www-form-urlencoded", bytes);
	}
	if (origin) soup_message_headers_append(soup_message_get_request_headers(message), "Origin", origin);
	if (host) soup_message_headers_replace(soup_message_get_request_headers(message), "Host", host);
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL, attribution_reply, &reply);
	while (!reply.done) g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(reply.error); status = soup_message_get_status(message);
	*text = g_strndup(g_bytes_get_data(reply.body, NULL), g_bytes_get_size(reply.body)); g_bytes_unref(reply.body);
	if (cors) *cors = g_strdup(soup_message_headers_get_one(soup_message_get_response_headers(message), "Access-Control-Allow-Origin"));
	return status;
}
static guint attribution_http(SoupSession *session, const gchar *base, const gchar *method, const gchar *path,
	const gchar *origin, const gchar *payload, gchar **text, gchar **cors)
{
	return attribution_http_with_host(session, base, method, path, origin, payload, text, cors, NULL);
}
static void test_http(Fixture *f, gconstpointer data)
{
	g_autoptr(GSocketListener) probe = g_socket_listener_new();
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(VentureWebServer) server = NULL;
	g_autoptr(SoupSession) session = g_object_new(SOUP_TYPE_SESSION, "timeout", 10, NULL);
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autofree gchar *directory = g_dir_make_tmp("venture-attribution-web-XXXXXX", NULL), *base = NULL, *text = NULL, *path = NULL, *cors = NULL, *body = NULL, *token = NULL;
	guint port = g_socket_listener_add_any_inet_port(probe, NULL, &error); g_assert_no_error(error); g_clear_object(&probe);
	base = g_strdup_printf("http://127.0.0.1:%u", port);
	g_object_set(config, "state-dir", directory, "server-bind-address", "127.0.0.1", "server-port", (gint64)port,
		"security-require-auth", TRUE, "server-base-url", base, NULL);
	if (data) {
		g_object_set(config, "hosted-enabled", TRUE,
			"hosted-workspace-id", "24af2d1e-cb78-48f3-9c22-cfdc8ecdc7c1", "hosted-origin", base, NULL);
		g_assert_true(venture_tenant_service_configure(venture_tenant_service_get(f->db), config, &error));
		g_assert_true(venture_tenant_service_initialize(venture_tenant_service_get(f->db), &error));
		g_assert_no_error(error);
	}
	context = venture_context_new(config, f->db); server = venture_web_server_new(context, &error); g_assert_no_error(error);
	g_assert_true(venture_web_server_start(server, &error)); g_assert_no_error(error);
	g_assert_cmpuint(attribution_http(session, base, "GET", "/attribution.js", NULL, NULL, &text, NULL), ==, 200);
	g_assert_nonnull(strstr(text, "VentureAttribution")); g_clear_pointer(&text, g_free);
	path = g_strdup_printf("/attribution/%s/grant", venture_entity_get_uuid(f->site));
	g_assert_cmpuint(attribution_http(session, base, "POST", path, "https://evil.example.test", "{\"policy\":\"analytics-v1\"}", &text, &cors), ==, 404);
	g_assert_null(cors); g_clear_pointer(&text, g_free);
	g_assert_cmpuint(attribution_http(session, base, "POST", path, "https://site.example.test", "{\"policy\":\"analytics-v1\"}", &text, &cors), ==, 200);
	g_assert_cmpstr(cors, ==, "https://site.example.test"); g_clear_pointer(&cors, g_free);
	node = json_from_string(text, &error); g_assert_no_error(error); token = g_strdup(json_object_get_string_member(json_node_get_object(node), "token")); g_clear_pointer(&text, g_free);
	g_clear_pointer(&path, g_free); path = g_strdup_printf("/attribution/%s/observe", venture_entity_get_uuid(f->site));
	body = g_strdup_printf("{\"token\":\"%s\",\"event_id\":\"http-event\",\"fields\":{\"page\":\"https://site.example.test/offer?private=discard\",\"utm_source\":\"newsletter\"}}", token);
	g_assert_cmpuint(attribution_http(session, base, "POST", path, "https://site.example.test", body, &text, NULL), ==, 200); g_clear_pointer(&text, g_free);
	g_assert_cmpuint(attribution_http(session, base, "POST", path, "https://site.example.test", body, &text, NULL), ==, 200); g_clear_pointer(&text, g_free);
	rows = all_rows(f, VENTURE_TYPE_ATTRIBUTION_TOUCH); g_assert_cmpuint(rows->len, ==, 1);
	g_clear_pointer(&path, g_free); path = g_strdup_printf("/attribution/%s/withdraw", venture_entity_get_uuid(f->site));
	g_clear_pointer(&body, g_free); body = g_strdup_printf("{\"token\":\"%s\"}", token);
	g_assert_cmpuint(attribution_http(session, base, "POST", path, "https://site.example.test", body, &text, NULL), ==, 200); g_clear_pointer(&text, g_free);
	g_assert_cmpuint(attribution_http(session, base, "POST", "/hooks/lightsite/nope/1", NULL, "{}", &text, NULL), ==, 404);
	if (data) {
		VentureTenantService *tenant = venture_tenant_service_get(f->db);
		const gchar *states[] = { "read_only", "suspended" };
		guint i;
		g_clear_pointer(&text, g_free); g_clear_pointer(&path, g_free);
		path = g_strdup_printf("/attribution/%s/grant", venture_entity_get_uuid(f->site));
		/* Only this explicitly capability-bound endpoint may delegate Origin.
		 * Neither a forged workspace Host nor ordinary API CSRF is exempt. */
		g_assert_cmpuint(attribution_http_with_host(session, base, "POST", path,
			"https://site.example.test", "{\"policy\":\"analytics-v1\"}", &text, NULL,
			"neighbor.example.test"), ==, 403); g_clear_pointer(&text, g_free);
		g_assert_cmpuint(attribution_http(session, base, "POST", "/api/v1/company",
			"https://site.example.test", "{}", &text, NULL), ==, 403); g_clear_pointer(&text, g_free);
		for (i = 0; i < G_N_ELEMENTS(states); i++) {
			g_assert_true(venture_tenant_service_set_state_operator(tenant, states[i], "Pause public intake", &error));
			g_assert_no_error(error);
			g_assert_cmpuint(attribution_http(session, base, "POST", path, "https://site.example.test",
				"{\"policy\":\"analytics-v1\"}", &text, NULL), ==, 403); g_clear_pointer(&text, g_free);
		}
		g_assert_true(venture_tenant_service_set_state_operator(tenant, "active", "Resume public intake", &error));
		g_assert_no_error(error);
		g_assert_cmpuint(attribution_http(session, base, "POST", path, "https://site.example.test",
			"{\"policy\":\"analytics-v1\"}", &text, NULL), ==, 200); g_clear_pointer(&text, g_free);
	}
	venture_web_server_stop(server); g_clear_object(&server); g_clear_object(&context); venture_test_remove_tree(directory);
}
typedef struct { gboolean done; gchar *out; gchar *err; GError *error; } AttributionCli;
static void attribution_cli_done(GObject *source, GAsyncResult *result, gpointer data)
{
	AttributionCli *reply = data;
	g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &reply->out, &reply->err, &reply->error); reply->done = TRUE;
}
static gboolean attribution_cli_timeout(gpointer data)
{
	g_subprocess_force_exit(G_SUBPROCESS(data)); return G_SOURCE_CONTINUE;
}
/* CLI, JSON, CSV and page links must retain the same attribution question. */
static void test_report_surfaces(Fixture *f, gconstpointer data)
{
	g_autoptr(GSocketListener) probe = g_socket_listener_new();
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(VentureWebServer) server = NULL;
	g_autoptr(SoupSession) session = g_object_new(SOUP_TYPE_SESSION, "timeout", 10, NULL);
	g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
	g_autoptr(GSubprocess) process = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *directory = g_dir_make_tmp("venture-attribution-surfaces-XXXXXX", NULL), *base = NULL, *text = NULL;
	g_autofree gchar *token = grant(f);
	g_autoptr(JsonObject) fields = page(), payload = submission("surface-capture", token);
	g_autoptr(VentureAttributionTouch) touch = NULL;
	g_autoptr(VentureAttributionSubmission) captured = NULL;
	AttributionCli reply = { FALSE, NULL, NULL, NULL };
	const gchar *argv[] = { "build/debug/venturectl", "--server", NULL, "--format", "json", "report", "attribution", "all", "model=last", "details=true", "organization_id=1", NULL };
	guint timeout, port = g_socket_listener_add_any_inet_port(probe, NULL, &error); g_assert_no_error(error); g_clear_object(&probe);
	base = g_strdup_printf("http://127.0.0.1:%u", port); argv[2] = base;
	touch = venture_attribution_service_observe(f->service, venture_entity_get_uuid(f->site), "https://site.example.test", token, "surface-first", fields, f->now, &error); g_assert_no_error(error);
	g_clear_object(&touch); json_object_set_string_member(fields, "utm_source", "partner");
	touch = venture_attribution_service_observe(f->service, venture_entity_get_uuid(f->site), "https://site.example.test", token, "surface-last", fields, f->now, &error); g_assert_no_error(error);
	captured = venture_attribution_service_capture(f->service, venture_entity_get_uuid(f->site), "https://site.example.test", payload, f->now, &error); g_assert_no_error(error);
	/* Authentication has its own real-server suite; this disposable loopback
	 * instance tests option transport without manufacturing a session. */
	g_object_set(config, "state-dir", directory, "server-bind-address", "127.0.0.1", "server-port", (gint64)port,
		"security-require-auth", FALSE, "server-base-url", base, NULL);
	context = venture_context_new(config, f->db); server = venture_web_server_new(context, &error); g_assert_no_error(error);
	g_assert_true(venture_web_server_start(server, &error)); g_assert_no_error(error);
	g_assert_cmpuint(attribution_http(session, base, "GET", "/api/v1/reports/attribution?period=all&model=last&details=true&organization_id=1", NULL, NULL, &text, NULL), ==, 200);
	g_assert_nonnull(strstr(text, "partner")); g_assert_nonnull(strstr(text, "last")); g_clear_pointer(&text, g_free);
	g_assert_cmpuint(attribution_http(session, base, "GET", "/reports/attribution?period=all&model=last&details=true&organization_id=1", NULL, NULL, &text, NULL), ==, 200);
	g_assert_nonnull(strstr(text, "model=last")); g_assert_nonnull(strstr(text, "details=true")); g_clear_pointer(&text, g_free);
	g_assert_cmpuint(attribution_http(session, base, "GET", "/api/v1/reports/attribution?period=all&model=last&details=true&organization_id=1&format=csv", NULL, NULL, &text, NULL), ==, 200);
	g_assert_nonnull(strstr(text, "partner")); g_clear_pointer(&text, g_free);
	g_subprocess_launcher_unsetenv(launcher, "VENTURE_TOKEN");
	process = g_subprocess_launcher_spawnv(launcher, argv, &error); g_assert_no_error(error);
	timeout = g_timeout_add_seconds(30, attribution_cli_timeout, process);
	g_subprocess_communicate_utf8_async(process, NULL, NULL, attribution_cli_done, &reply);
	while (!reply.done) g_main_context_iteration(NULL, TRUE);
	g_source_remove(timeout); g_assert_no_error(reply.error);
	if (!g_subprocess_get_successful(process)) g_test_message("CLI error: %s", reply.err);
	g_assert_true(g_subprocess_get_successful(process)); g_assert_nonnull(strstr(reply.out, "partner")); g_assert_nonnull(strstr(reply.out, "last"));
	g_test_message("Built CLI report JSON: %s", reply.out);
	g_free(reply.out); g_free(reply.err);
	{
		g_autoptr(VentureIntegrationConnection) lightsite = connect_site(f), other = NULL, current = NULL;
		g_autoptr(JsonNode) settings = json_from_string("{\"fixture\":true}", NULL);
		g_autofree gchar *form = NULL;
		other = venture_integration_service_configure(venture_integration_service_get(f->db), 1, "other_fixture", "account", "test", settings, 0, NULL, &error); g_assert_no_error(error);
		form = g_strdup_printf("operation=disconnect&connection_id=%" G_GINT64_FORMAT "&version=%" G_GINT64_FORMAT,
			venture_entity_get_id(VENTURE_ENTITY(other)), venture_entity_get_version(VENTURE_ENTITY(other)));
		g_assert_cmpuint(attribution_http(session, base, "POST", "/organizations/1/settings/attribution", NULL, form, &text, NULL), ==, 400);
		g_clear_pointer(&text, g_free);
		current = venture_integration_service_find(venture_integration_service_get(f->db), 1, "other_fixture", &error); g_assert_no_error(error); g_assert_nonnull(current);
		g_assert_cmpuint(attribution_http(session, base, "GET", "/organizations/1/settings/attribution", NULL, NULL, &text, NULL), ==, 200);
		g_assert_nonnull(strstr(text, "type=\"password\"")); g_assert_null(strstr(text, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"));
		g_clear_pointer(&text, g_free); g_clear_pointer(&form, g_free);
		form = g_strdup_printf("operation=disconnect&connection_id=%" G_GINT64_FORMAT "&version=%" G_GINT64_FORMAT,
			venture_entity_get_id(VENTURE_ENTITY(lightsite)), venture_entity_get_version(VENTURE_ENTITY(lightsite)));
		g_assert_cmpuint(attribution_http(session, base, "POST", "/organizations/1/settings/attribution", NULL, form, &text, NULL), ==, 200);
		g_assert_nonnull(strstr(text, "Disconnected")); g_clear_pointer(&text, g_free);
	}
	g_test_message("Built venturectl report attribution all model=last details=true organization_id=1 agrees with JSON, CSV and preserved page links; settings reject another provider's disconnect identity");
	venture_web_server_stop(server); g_clear_object(&server); g_clear_object(&context); venture_test_remove_tree(directory);
}
static void test_script(void)
{
	const gchar *argv[] = { "node", "tests/attribution-script.cjs", NULL };
	g_autofree gchar *output = NULL, *failure = NULL;
	g_autoptr(GError) error = NULL;
	gint status;
	/* Node is a development-test dependency; absence must not silently skip
	 * the consent boundary that protects the public browser integration. */
	g_assert_true(g_spawn_sync(NULL, (gchar **)argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, &output, &failure, &status, &error));
	g_assert_no_error(error); if (status) g_test_message("Script failure: %s", failure);
	g_assert_true(g_spawn_check_wait_status(status, &error)); g_assert_no_error(error); g_test_message("%s", output);
}
static JsonObject *report_row(JsonNode *node, const gchar *measure, const gchar *source)
{
	JsonArray *rows = json_object_get_array_member(json_node_get_object(node), "rows");
	guint i;
	for (i = 0; i < json_array_get_length(rows); i++) {
		JsonObject *row = json_array_get_object_element(rows, i);
		if (!g_strcmp0(venture_json_object_get_string(row, "measure", ""), measure) &&
			!g_strcmp0(venture_json_object_get_string(row, "source", ""), source)) return row;
	}
	return NULL;
}
static VentureMetric *attribution_metric(VentureReportResult *report, const gchar *key)
{
	GPtrArray *metrics = venture_report_result_get_metrics(report);
	guint i;
	for (i = 0; i < metrics->len; i++) {
		VentureMetric *metric = g_ptr_array_index(metrics, i);
		if (!strcmp(venture_metric_get_key(metric), key)) return metric;
	}
	return NULL;
}
static void test_report_models(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureContext) context = venture_context_new(config, f->db);
	g_autofree gchar *token = grant(f);
	g_autoptr(JsonObject) fields = page(), payload = submission("model-one", token), options = json_object_new();
	g_autoptr(VentureAttributionTouch) first = NULL, last = NULL;
	g_autoptr(VentureAttributionSubmission) capture = NULL;
	g_autoptr(VentureReportResult) report = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GDateTime) later = g_date_time_add_seconds(f->now, 1), end = g_date_time_add_seconds(f->now, 10);
	g_autoptr(VentureDateRange) period = venture_date_range_new(f->now, end);
	g_autoptr(GError) error = NULL;
	JsonObject *row;
	VentureReport *definition = venture_report_registry_lookup(venture_context_get_report_registry(context), "attribution");
	g_assert_nonnull(definition);
	first = venture_attribution_service_observe(f->service, venture_entity_get_uuid(f->site), "https://site.example.test", token, "first", fields, f->now, &error); g_assert_no_error(error);
	json_object_set_string_member(fields, "utm_source", "partner");
	last = venture_attribution_service_observe(f->service, venture_entity_get_uuid(f->site), "https://site.example.test", token, "last", fields, later, &error); g_assert_no_error(error);
	capture = venture_attribution_service_capture(f->service, venture_entity_get_uuid(f->site), "https://site.example.test", payload, later, &error); g_assert_no_error(error); g_assert_nonnull(capture);
	report = venture_report_generate(definition, context, period, options, &error); g_assert_no_error(error); g_assert_nonnull(report);
	g_assert_nonnull(attribution_metric(report, "visitor_submission_rate"));
	g_assert_cmpfloat(venture_metric_get_number(attribution_metric(report, "visitor_submission_rate")), ==, 1.0);
	g_assert_cmpfloat(venture_metric_get_number(attribution_metric(report, "captured_lead_conversion_rate")), ==, 0.0);
	node = venture_report_result_to_json(report); row = report_row(node, "submissions", "newsletter"); g_assert_nonnull(row); g_assert_cmpint(json_object_get_int_member(row, "count"), ==, 1);
	row = report_row(node, "leads", "newsletter"); g_assert_nonnull(row); g_assert_cmpint(json_object_get_int_member(row, "count"), ==, 1);
	g_clear_object(&report); g_clear_pointer(&node, json_node_unref); json_object_set_string_member(options, "model", "last");
	report = venture_report_generate(definition, context, period, options, &error); g_assert_no_error(error); g_assert_nonnull(report);
	node = venture_report_result_to_json(report); row = report_row(node, "submissions", "partner"); g_assert_nonnull(row); g_assert_cmpint(json_object_get_int_member(row, "count"), ==, 1);
	g_assert_nonnull(strstr(json_object_get_string_member(row, "source_records"), "attribution_submission:"));
	g_clear_object(&report); g_clear_pointer(&node, json_node_unref); json_object_set_int_member(options, "organization_id", 999);
	report = venture_report_generate(definition, context, period, options, &error); g_assert_no_error(error); g_assert_nonnull(report);
	node = venture_report_result_to_json(report); g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(node), "rows")), ==, 0);
}
/* Invalid options must not silently answer a different model or organization. */
static void test_report_options(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureContext) context = venture_context_new(config, f->db);
	g_autoptr(VentureDateRange) period = venture_date_range_new(NULL, NULL);
	g_autoptr(JsonObject) options = json_object_new();
	g_autoptr(VentureReportResult) report = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *invalid[] = { "model", "details", "organization_id" };
	VentureReport *definition = venture_report_registry_lookup(venture_context_get_report_registry(context), "attribution");
	guint i;
	for (i = 0; i < G_N_ELEMENTS(invalid); i++) {
		json_object_set_array_member(options, invalid[i], json_array_new());
		report = venture_report_generate(definition, context, period, options, &error);
		g_assert_null(report); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
		g_clear_error(&error); json_object_remove_member(options, invalid[i]);
	}
	json_object_set_string_member(options, "details", "perhaps");
	report = venture_report_generate(definition, context, period, options, &error);
	g_assert_null(report); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}
static void money_field(VentureEntity *row, const gchar *field, const gchar *text)
{
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_entity_set_field_from_string(row, field, text, &error)); g_assert_no_error(error);
}
static void assert_report_money(JsonNode *node, const gchar *measure, const gchar *source, gint64 expected)
{
	JsonObject *row = report_row(node, measure, source);
	g_autoptr(VentureMoney) money = NULL;
	g_autoptr(GError) error = NULL;
	g_assert_nonnull(row); money = venture_money_from_json(json_object_get_member(row, "amount"), "USD", &error);
	g_assert_no_error(error); g_assert_nonnull(money); g_assert_cmpint(venture_money_get_amount(money), ==, expected);
}
static void calendar_financial_event(Fixture *f, VentureEntity *company, const gchar *number,
	GDateTime *date, const gchar *net, const gchar *cash)
{
	g_autoptr(VentureEntity) invoice = g_object_new(VENTURE_TYPE_INVOICE, "organization-id", (gint64)1,
		"company-id", venture_entity_get_id(company), "number", number, "issued-at", date, "due-at", date, NULL);
	g_autoptr(VentureEntity) line = NULL, payment = NULL;
	g_autoptr(GError) error = NULL;
	persist(f, invoice);
	line = g_object_new(VENTURE_TYPE_INVOICE_LINE, "organization-id", (gint64)1,
		"invoice-id", venture_entity_get_id(invoice), "description", "Date-bound attribution", "quantity", 1.0, NULL);
	money_field(line, "unit-price", net); persist(f, line);
	g_assert_true(venture_settlement_service_transition(venture_settlement_service_get(f->db), VENTURE_INVOICE(invoice), "sent", date, NULL, &error));
	g_assert_no_error(error);
	payment = g_object_new(VENTURE_TYPE_PAYMENT, "organization-id", (gint64)1,
		"customer-id", venture_entity_get_id(company), "invoice-id", venture_entity_get_id(invoice), "date", date, "method", "verified-bank-receipt", NULL);
	money_field(payment, "amount", cash); persist(f, payment);
}
/* Calendar midnight is not evidence that a same-day invoice preceded capture.
 * Its immutable event creation orders that case; genuine earlier evidence and
 * precise timestamps must still refuse retroactive acquisition attribution. */
static void test_financial_calendar_dates(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureContext) context = venture_context_new(config, f->db);
	g_autoptr(VentureEntity) company = g_object_new(VENTURE_TYPE_COMPANY, "organization-id", (gint64)1, "name", "Existing financial customer", NULL);
	g_autoptr(GDateTime) day = g_date_time_new_utc(g_date_time_get_year(f->now), g_date_time_get_month(f->now), g_date_time_get_day_of_month(f->now), 0, 0, 0);
	g_autoptr(GDateTime) yesterday = g_date_time_add_days(day, -1), later = NULL;
	g_autofree gchar *token = grant(f);
	g_autoptr(JsonObject) fields = page(), payload = submission("calendar-date-capture", token), options = json_object_new();
	g_autoptr(VentureAttributionTouch) touch = NULL;
	g_autoptr(VentureAttributionSubmission) captured = NULL;
	g_autoptr(VentureEntity) lead = NULL, converted = NULL;
	g_autoptr(VentureDateRange) period = venture_date_range_new(NULL, NULL);
	g_autoptr(VentureReportResult) report = NULL;
	g_autoptr(JsonNode) result = NULL;
	g_autoptr(GError) error = NULL;
	gint64 lead_id;
	VentureReport *definition = venture_report_registry_lookup(venture_context_get_report_registry(context), "attribution");
	(void)data;
	persist(f, company);
	calendar_financial_event(f, company, "DATE-BEFORE", day, "30 USD", "10 USD");
	touch = venture_attribution_service_observe(f->service, venture_entity_get_uuid(f->site), "https://site.example.test", token, "calendar-touch", fields, f->now, &error);
	g_assert_no_error(error);
	captured = venture_attribution_service_capture(f->service, venture_entity_get_uuid(f->site), "https://site.example.test", payload, f->now, &error);
	g_assert_no_error(error);
	g_object_get(captured, "lead-id", &lead_id, NULL);
	lead = venture_database_get(f->db, VENTURE_TYPE_LEAD, lead_id, &error); g_assert_no_error(error);
	g_object_set(lead, "status", VENTURE_LEAD_QUALIFIED, NULL); persist(f, lead);
	json_object_set_int_member(options, "company_id", venture_entity_get_id(company)); json_object_set_boolean_member(options, "deal", TRUE);
	converted = venture_lead_service_convert(venture_database_get_lead_service(f->db), lead, options, NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(converted);
	calendar_financial_event(f, company, "DATE-AFTER", day, "200 USD", "75 USD");
	calendar_financial_event(f, company, "DATE-YESTERDAY", yesterday, "40 USD", "20 USD");
	calendar_financial_event(f, company, "PRECISE-BEFORE", f->now, "50 USD", "25 USD");
	later = venture_time_now();
	calendar_financial_event(f, company, "PRECISE-AFTER", later, "60 USD", "30 USD");
	report = venture_report_generate(definition, context, period, NULL, &error); g_assert_no_error(error);
	result = venture_report_result_to_json(report);
	assert_report_money(result, "invoiced_net", "newsletter", 26000);
	assert_report_money(result, "cash_receipts", "newsletter", 10500);
	assert_report_money(result, "invoiced_net", "unknown", 12000);
	assert_report_money(result, "cash_receipts", "unknown", 5500);
}

/* The accounting source's identity, not the number of tracking touches, owns each amount. */
static void test_financial_journey(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureContext) context = venture_context_new(config, f->db);
	g_autoptr(VentureIntegrationConnection) connection = connect_site(f);
	g_autoptr(VentureEntity) campaign = g_object_new(VENTURE_TYPE_CAMPAIGN, "organization-id", (gint64)1, "name", "Source campaign", "channel", "email", "started-at", f->now, NULL);
	g_autoptr(JsonNode) mapping = json_node_new(JSON_NODE_OBJECT), envelope = json_node_new(JSON_NODE_OBJECT), report_json = NULL;
	g_autoptr(JsonObject) map = json_object_new(), fields = page(), payload = NULL, options = json_object_new();
	g_autoptr(VentureAttributionTouch) touch = NULL;
	g_autoptr(VentureAttributionSubmission) captured = NULL;
	g_autoptr(VentureEntity) lead = NULL, converted = NULL, deal = NULL, invoice = NULL, line = NULL, payment = NULL, refund = NULL;
	g_autoptr(GPtrArray) allocations = NULL;
	g_autoptr(GDateTime) issue = NULL, paid = NULL, second = NULL, returned = NULL, end = NULL;
	g_autoptr(VentureDateRange) period = NULL, delayed = NULL;
	g_autoptr(VentureReportResult) report = NULL;
	g_autoptr(GBytes) bytes = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *token = NULL, *body = NULL, *signature = NULL, *timestamp = g_strdup_printf("%" G_GINT64_FORMAT, g_date_time_to_unix(f->now));
	gint64 lead_id = 0, company_id = 0, deal_id = 0;
	VentureReport *definition = venture_report_registry_lookup(venture_context_get_report_registry(context), "attribution");
	money_field(campaign, "spend", "100 USD"); money_field(campaign, "revenue", "999 USD"); persist(f, campaign);
	json_object_set_int_member(map, "newsletter", venture_entity_get_id(campaign)); json_node_set_object(mapping, map);
	{
		g_autofree gchar *mapping_text = json_to_string(mapping, FALSE);
		g_object_set(f->site, "campaign-map", mapping_text, NULL); persist(f, f->site);
	}
	token = grant(f); json_object_set_string_member(fields, "utm_campaign", "newsletter");
	touch = venture_attribution_service_observe(f->service, venture_entity_get_uuid(f->site), "https://site.example.test", token, "journey-touch", fields, f->now, &error); g_assert_no_error(error);
	payload = submission("lightsite-financial-journey", token); json_object_set_string_member(payload, "tenant_id", "tenant_one"); json_object_set_string_member(payload, "site_id", "site_one"); json_object_set_string_member(payload, "origin", "https://site.example.test");
	json_node_set_object(envelope, payload); body = json_to_string(envelope, FALSE); bytes = g_bytes_new(body, strlen(body)); signature = sign_body(timestamp, body);
	captured = venture_attribution_service_receive(f->service, venture_entity_get_uuid(f->site), venture_entity_get_id(VENTURE_ENTITY(connection)), timestamp, signature, bytes, f->now, &error); g_assert_no_error(error); g_assert_nonnull(captured);
	g_object_get(captured, "lead-id", &lead_id, NULL); lead = venture_database_get(f->db, VENTURE_TYPE_LEAD, lead_id, &error); g_assert_no_error(error);
	g_object_set(lead, "status", VENTURE_LEAD_QUALIFIED, NULL); persist(f, lead); json_object_set_boolean_member(options, "deal", TRUE);
	converted = venture_lead_service_convert(venture_database_get_lead_service(f->db), lead, options, NULL, &error); g_assert_no_error(error); g_assert_nonnull(converted);
	g_object_get(converted, "converted-company-id", &company_id, "converted-deal-id", &deal_id, NULL); g_assert_cmpint(company_id, >, 0);
	deal = venture_database_get(f->db, VENTURE_TYPE_DEAL, deal_id, &error); g_assert_no_error(error); money_field(deal, "value", "500 USD"); persist(f, deal);
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_PIPELINE_STAGE);
		g_autoptr(VentureEntity) won = NULL;
		g_autoptr(VentureDeal) moved = NULL;
		venture_query_set_organization(query, 1); venture_query_add_filter_string(query, "kind", VENTURE_FILTER_OP_EQ, "won", NULL);
		won = venture_database_find_one(f->db, query, &error); g_assert_no_error(error); g_assert_nonnull(won);
		moved = venture_deal_service_move_stage(venture_database_get_deal_service(f->db), VENTURE_DEAL(deal), venture_entity_get_id(won), "Accepted proposal", NULL, &error);
		g_assert_no_error(error); g_assert_nonnull(moved);
	}
	issue = venture_time_now();
	invoice = g_object_new(VENTURE_TYPE_INVOICE, "organization-id", (gint64)1, "company-id", company_id, "number", "ATTR-1", "issued-at", issue, "due-at", issue, NULL); persist(f, invoice);
	line = g_object_new(VENTURE_TYPE_INVOICE_LINE, "organization-id", (gint64)1, "invoice-id", venture_entity_get_id(invoice), "description", "Acquired work", "quantity", 1.0, NULL); money_field(line, "unit-price", "200 USD"); persist(f, line);
	if (!venture_settlement_service_transition(venture_settlement_service_get(f->db), VENTURE_INVOICE(invoice), "sent", issue, NULL, &error)) g_error("Invoice issue: %s", error->message);
	g_assert_no_error(error);
	paid = venture_time_now();
	payment = g_object_new(VENTURE_TYPE_PAYMENT, "organization-id", (gint64)1, "customer-id", company_id, "invoice-id", venture_entity_get_id(invoice), "date", paid, "method", "verified-bank-receipt", NULL); money_field(payment, "amount", "75 USD"); persist(f, payment);
	second = venture_time_now();
	g_clear_object(&payment); payment = g_object_new(VENTURE_TYPE_PAYMENT, "organization-id", (gint64)1, "customer-id", company_id, "invoice-id", venture_entity_get_id(invoice), "date", second, "method", "verified-bank-receipt", NULL); money_field(payment, "amount", "125 USD"); persist(f, payment);
	allocations = all_rows(f, VENTURE_TYPE_PAYMENT_ALLOCATION); g_assert_cmpuint(allocations->len, ==, 2);
	returned = venture_time_now();
	refund = g_object_new(VENTURE_TYPE_REFUND, "organization-id", (gint64)1, "customer-id", company_id, "allocation-id", venture_entity_get_id(g_ptr_array_index(allocations, 0)), "date", returned, NULL); money_field(refund, "amount", "25 USD"); persist(f, refund);
	end = venture_time_now(); period = venture_date_range_new(f->now, end); delayed = venture_date_range_new(issue, paid);
	report = venture_report_generate(definition, context, period, NULL, &error); g_assert_no_error(error); report_json = venture_report_result_to_json(report);
	assert_report_money(report_json, "won_deal_value", "newsletter", 50000); assert_report_money(report_json, "invoiced_net", "newsletter", 20000);
	assert_report_money(report_json, "cash_receipts", "newsletter", 20000); assert_report_money(report_json, "cash_refunds", "newsletter", -2500);
	g_assert_cmpint(json_object_get_int_member(report_row(report_json, "customers", "newsletter"), "count"), ==, 1);
	g_assert_nonnull(attribution_metric(report, "cac_customer_difference"));
	g_assert_cmpfloat(venture_metric_get_number(attribution_metric(report, "cac_customer_difference")), ==, 0.0);
	g_assert_cmpint(json_object_get_int_member(report_row(report_json, "invoiced_net", "newsletter"), "campaign_id"), ==, venture_entity_get_id(campaign));
	g_clear_object(&report); g_clear_pointer(&report_json, json_node_unref);
	report = venture_report_generate(definition, context, delayed, NULL, &error); g_assert_no_error(error); report_json = venture_report_result_to_json(report);
	assert_report_money(report_json, "invoiced_net", "newsletter", 20000); g_assert_null(report_row(report_json, "cash_receipts", "newsletter"));
	/* A second currency and company merge cannot relabel retained acquisition
	 * or multiply the settled USD journey by the number of source joins. */
	{
		g_autoptr(VentureEntity) euro = NULL, euro_line = NULL, original = NULL, survivor = NULL, merged = NULL, candidate = NULL;
		g_autoptr(GPtrArray) candidates = NULL, bindings = NULL;
		g_autoptr(GDateTime) euro_date = venture_time_now();
		g_autoptr(VentureDateRange) all_time = venture_date_range_new(NULL, NULL);
		g_autofree gchar *name = NULL;
		JsonArray *rows;
		guint i, currencies = 0;
		gint64 retained_company = 0;
		euro = g_object_new(VENTURE_TYPE_INVOICE, "organization-id", (gint64)1, "company-id", company_id, "number", "ATTR-EUR", "issued-at", euro_date, "due-at", euro_date, NULL); persist(f, euro);
		euro_line = g_object_new(VENTURE_TYPE_INVOICE_LINE, "organization-id", (gint64)1, "invoice-id", venture_entity_get_id(euro), "description", "Separate EUR work", "quantity", 1.0, NULL);
		money_field(euro_line, "unit-price", "90 EUR"); persist(f, euro_line);
		g_assert_true(venture_settlement_service_transition(venture_settlement_service_get(f->db), VENTURE_INVOICE(euro), "sent", euro_date, NULL, &error)); g_assert_no_error(error);
		original = venture_database_get(f->db, VENTURE_TYPE_COMPANY, company_id, &error); g_assert_no_error(error); g_object_get(original, "name", &name, NULL);
		g_object_set(original, "email", "merge-proof@example.test", NULL); persist(f, original);
		survivor = g_object_new(VENTURE_TYPE_COMPANY, "organization-id", (gint64)1, "name", name, "email", "merge-proof@example.test", NULL); persist(f, survivor);
		g_assert_cmpint(venture_dedupe_service_scan(venture_dedupe_service_get(f->db), 1, "company", NULL, &error), >=, 1); g_assert_no_error(error);
		candidates = all_rows(f, VENTURE_TYPE_DUPLICATE_CANDIDATE);
		for (i = 0; i < candidates->len; i++) {
			gint64 a = 0, b = 0;
			VentureEntity *row = g_ptr_array_index(candidates, i);
			g_object_get(row, "record-a", &a, "record-b", &b, NULL);
			if (a == company_id && b == venture_entity_get_id(survivor)) candidate = g_object_ref(row);
		}
		g_assert_nonnull(candidate);
		merged = venture_dedupe_service_merge(venture_dedupe_service_get(f->db), candidate, venture_entity_get_id(survivor), NULL, &error); g_assert_no_error(error); g_assert_nonnull(merged);
		bindings = all_rows(f, VENTURE_TYPE_ATTRIBUTION_BINDING); g_assert_cmpuint(bindings->len, ==, 1);
		g_object_get(g_ptr_array_index(bindings, 0), "company-id", &retained_company, NULL); g_assert_cmpint(retained_company, ==, company_id);
		g_clear_object(&report); g_clear_pointer(&report_json, json_node_unref);
		report = venture_report_generate(definition, context, all_time, NULL, &error); g_assert_no_error(error); report_json = venture_report_result_to_json(report);
		assert_report_money(report_json, "cash_receipts", "newsletter", 20000); assert_report_money(report_json, "cash_refunds", "newsletter", -2500);
		rows = json_object_get_array_member(json_node_get_object(report_json), "rows");
		for (i = 0; i < json_array_get_length(rows); i++) {
			JsonObject *row = json_array_get_object_element(rows, i);
			if (!g_strcmp0(venture_json_object_get_string(row, "measure", ""), "invoiced_net")) {
				g_autoptr(VentureMoney) amount = venture_money_from_json(json_object_get_member(row, "amount"), "USD", &error); g_assert_no_error(error);
				g_assert_cmpstr(venture_json_object_get_string(row, "source", ""), ==, "newsletter");
				if (!strcmp(venture_money_get_currency(amount), "EUR")) g_assert_cmpint(venture_money_get_amount(amount), ==, 9000);
				else { g_assert_cmpstr(venture_money_get_currency(amount), ==, "USD"); g_assert_cmpint(venture_money_get_amount(amount), ==, 20000); }
				currencies++;
			}
		}
		g_assert_cmpuint(currencies, ==, 2);
	}
	g_test_message("Signed site capture -> canonical conversion -> USD500 won deal -> USD200 issued net -> delayed USD75+125 cash -> USD25 refund; period boundary excludes pending cash; merge preserves original source and EUR90 remains a separate invoice bucket");
}
/* Replay identities and withdrawn capability state survive application reconnect. */
static void test_restart(Fixture *f, gconstpointer data)
{
	g_autofree gchar *token = grant(f), *schema = NULL, *sql = NULL;
	g_autoptr(JsonObject) payload = submission("durable-submission", token);
	g_autoptr(VentureAttributionSubmission) first = NULL, replay = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;
	first = venture_attribution_service_capture(f->service, venture_entity_get_uuid(f->site), "https://site.example.test", payload, f->now, &error); g_assert_no_error(error); g_assert_nonnull(first);
	g_assert_true(venture_attribution_service_withdraw(f->service, venture_entity_get_uuid(f->site), token, f->now, &error)); g_assert_no_error(error);
	schema = g_strdup(g_object_get_data(G_OBJECT(f->db), "accounting-test-schema"));
	g_clear_object(&f->db);
	f->db = venture_database_new(schema ? g_getenv("VENTURE_TEST_ACCOUNTING_POSTGRES_URI") : f->uri, &error); g_assert_no_error(error);
	if (schema) {
		sql = g_strdup_printf("SET search_path TO %s", schema);
		g_assert_true(venture_database_execute(f->db, sql, NULL, &error)); g_assert_no_error(error);
		g_object_set_data_full(G_OBJECT(f->db), "accounting-test-schema", g_strdup(schema), g_free);
	}
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error)); g_assert_no_error(error);
	f->service = venture_attribution_service_get(f->db);
	replay = venture_attribution_service_capture(f->service, venture_entity_get_uuid(f->site), "https://site.example.test", payload, f->now, &error); g_assert_no_error(error);
	g_assert_cmpint(venture_entity_get_id(VENTURE_ENTITY(replay)), ==, venture_entity_get_id(VENTURE_ENTITY(first)));
	g_clear_object(&replay); json_object_set_string_member(payload, "submission_id", "withdrawn-new-form");
	replay = venture_attribution_service_capture(f->service, venture_entity_get_uuid(f->site), "https://site.example.test", payload, f->now, &error);
	g_assert_null(replay); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND); g_clear_error(&error);
	rows = all_rows(f, VENTURE_TYPE_ATTRIBUTION_SUBMISSION); g_assert_cmpuint(rows->len, ==, 1);
	g_clear_pointer(&rows, g_ptr_array_unref); rows = all_rows(f, VENTURE_TYPE_LEAD); g_assert_cmpuint(rows->len, ==, 1);
	g_assert_true(venture_attribution_service_withdraw(f->service, venture_entity_get_uuid(f->site), token, f->now, &error)); g_assert_no_error(error);
	g_test_message("Closed/reopened application database: exact form retry keeps one submission/lead; withdrawn visitor cannot create new attributed evidence");
}
static void test_independent_permissions(Fixture *f, gconstpointer data)
{
	g_autoptr(JsonObject) payload = submission("email-only", NULL);
	g_autoptr(VentureAttributionSubmission) captured = NULL;
	g_autoptr(GPtrArray) visitors = NULL, consents = NULL;
	g_autoptr(GError) error = NULL;
	email_permission(f, payload);
	captured = venture_attribution_service_capture(f->service, venture_entity_get_uuid(f->site), "https://site.example.test", payload, f->now, &error); g_assert_no_error(error); g_assert_nonnull(captured);
	visitors = all_rows(f, VENTURE_TYPE_ATTRIBUTION_VISITOR); consents = all_rows(f, VENTURE_TYPE_MARKETING_CONSENT);
	g_assert_cmpuint(visitors->len, ==, 0); g_assert_cmpuint(consents->len, ==, 1);
	g_clear_object(&captured);
	{
		g_autoptr(VentureEntity) suppression = g_object_new(VENTURE_TYPE_SUPPRESSION, "organization-id", (gint64)1, "email", "form@example.test", NULL);
		g_autofree gchar *state = NULL;
		persist(f, suppression); json_object_set_string_member(payload, "submission_id", "suppressed-email");
		captured = venture_attribution_service_capture(f->service, venture_entity_get_uuid(f->site), "https://site.example.test", payload, f->now, &error); g_assert_no_error(error); g_assert_nonnull(captured);
		g_object_get(captured, "marketing-status", &state, NULL); g_assert_cmpstr(state, ==, "suppressed");
		g_clear_pointer(&consents, g_ptr_array_unref); consents = all_rows(f, VENTURE_TYPE_MARKETING_CONSENT); g_assert_cmpuint(consents->len, ==, 1);
	}
}
static void test_configuration_authority(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) user = g_object_new(VENTURE_TYPE_USER, "username", "site-editor", "active", TRUE, "role", VENTURE_USER_ROLE_EDITOR, NULL);
	g_autoptr(VentureEntity) membership = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP, "organization-id", (gint64)1, "active", TRUE, "role", VENTURE_ORGANIZATION_ROLE_EDITOR, NULL);
	g_autoptr(VentureAccessScope) scope = NULL;
	g_autoptr(VentureIntegrationConnection) connection = connect_site(f), denied = NULL;
	g_autoptr(JsonNode) settings = venture_integration_service_resolve(venture_integration_service_get(f->db), 1, venture_entity_get_id(VENTURE_ENTITY(connection)), FALSE, NULL);
	g_autoptr(GError) error = NULL;
	VentureAuthPrincipal principal;
	persist(f, user); g_object_set(membership, "user-id", venture_entity_get_id(user), NULL); persist(f, membership);
	principal.user_id = venture_entity_get_id(user); principal.token_id = 0; principal.role = VENTURE_USER_ROLE_EDITOR;
	principal.name = (gchar *)"site-editor"; principal.authenticated = TRUE;
	scope = venture_access_policy_enter(venture_database_get_access_policy(f->db), &principal);
	denied = venture_attribution_settings_configure(f->db, 1, settings, venture_entity_get_version(VENTURE_ENTITY(connection)), venture_entity_get_id(VENTURE_ENTITY(connection)), NULL, &error);
	g_assert_null(denied); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED); g_clear_error(&error);
	g_assert_cmpint(venture_attribution_service_sweep(f->service, 1, 1, f->now, NULL, &error), ==, -1); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error); g_object_set(f->site, "consent-policy", "editor-replacement", NULL);
	g_assert_false(venture_database_save(f->db, f->site, NULL, &error)); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error); g_clear_object(&scope);
	g_object_set(membership, "role", VENTURE_ORGANIZATION_ROLE_ADMIN, NULL); persist(f, membership);
	scope = venture_access_policy_enter(venture_database_get_access_policy(f->db), &principal);
	g_assert_true(venture_database_save(f->db, f->site, NULL, &error)); g_assert_no_error(error);
	denied = venture_attribution_settings_configure(f->db, 2, settings, 0, 0, NULL, &error);
	g_assert_null(denied); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED); g_clear_error(&error);
	denied = venture_attribution_settings_configure(f->db, 1, settings, venture_entity_get_version(VENTURE_ENTITY(connection)), venture_entity_get_id(VENTURE_ENTITY(connection)), NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(denied);
	g_assert_cmpint(venture_attribution_service_sweep(f->service, 1, 1, f->now, NULL, &error), ==, 0); g_assert_no_error(error);
	{
		g_autoptr(JsonNode) values = json_from_string("{\"organization_id\":1,\"limit\":1}", NULL);
		g_autoptr(GHashTable) parameters = venture_action_parameters_from_json(values, &error);
		g_autoptr(VentureEntity) result = NULL;
		g_assert_no_error(error);
		result = venture_action_registry_perform(venture_database_get_action_registry(f->db), "attribution_visitor", 0, "retention_sweep", parameters, NULL, VENTURE_USER_ROLE_EDITOR, &error);
		g_assert_no_error(error); g_assert_nonnull(result); g_assert_cmpint(venture_entity_get_organization_id(result), ==, 1);
		g_clear_object(&result); json_node_set_int(g_hash_table_lookup(parameters, "organization_id"), 2);
		result = venture_action_registry_perform(venture_database_get_action_registry(f->db), "attribution_visitor", 0, "retention_sweep", parameters, NULL, VENTURE_USER_ROLE_EDITOR, &error);
		g_assert_null(result); g_assert_nonnull(error); g_clear_error(&error);
	}
	g_test_message("Organization editor cannot change analytics policy or signing settings; own organization admin succeeds and another organization is refused");
}
static void test_upgrade(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureContext) context = venture_context_new(config, f->db);
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) visitors = NULL;
	g_autoptr(VentureEntity) form = NULL;
	venture_config_set_module_enabled(config, "attribution", FALSE);
	g_assert_true(venture_database_execute(f->db, "DELETE FROM schema_migrations WHERE version >= 430;"
		"DROP TABLE attribution_bindings; DROP TABLE attribution_submissions; DROP TABLE attribution_touches; DROP TABLE attribution_visitors; DROP TABLE attribution_sites", NULL, &error)); g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error)); g_assert_no_error(error);
	venture_config_set_module_enabled(config, "attribution", TRUE);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error)); g_assert_no_error(error);
	visitors = all_rows(f, VENTURE_TYPE_ATTRIBUTION_VISITOR); g_assert_cmpuint(visitors->len, ==, 0);
	form = venture_database_get(f->db, VENTURE_TYPE_LEAD_FORM, venture_entity_get_id(f->form), &error); g_assert_no_error(error); g_assert_nonnull(form);
}
static void test_migration_guard(Fixture *f, gconstpointer data)
{
	g_autofree gchar *sql = NULL, *path = g_strdup_printf("migrations/%s/000430_attribution.sql", g_getenv("VENTURE_TEST_ACCOUNTING_POSTGRES_URI") ? "postgresql" : "sqlite");
	g_autoptr(GError) error = NULL;
	g_assert_true(g_file_get_contents(path, &sql, NULL, &error)); g_assert_no_error(error);
	g_assert_true(venture_database_begin(f->db, &error)); g_assert_no_error(error);
	g_assert_true(venture_database_execute(f->db, "DROP INDEX uq_attribution_submissions_organization_submission_key", NULL, &error)); g_assert_no_error(error);
	g_assert_false(venture_migrations_execute_sql(venture_database_get_connection(f->db), sql, &error)); g_assert_nonnull(error); g_clear_error(&error);
	venture_database_rollback(f->db);
	g_assert_true(venture_migrations_execute_sql(venture_database_get_connection(f->db), sql, &error)); g_assert_no_error(error);
}
#include "attribution-boundaries.inc"

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/attribution/records", test_records);
	g_test_add_func("/attribution/evidence-guard", test_evidence_guard);
	g_test_add("/attribution/consent", Fixture, NULL, setup, test_consent, teardown);
	g_test_add("/attribution/observation-replay", Fixture, NULL, setup, test_observation_replay, teardown);
	g_test_add("/attribution/site-boundaries", Fixture, NULL, setup, test_site_boundaries, teardown);
	g_test_add("/attribution/repeated-evidence", Fixture, NULL, setup, test_repeated_evidence, teardown);
	g_test_add("/attribution/lookback", Fixture, NULL, setup, test_lookback, teardown);
	g_test_add("/attribution/site-identity", Fixture, NULL, setup, test_site_identity, teardown);
	g_test_add("/attribution/nested-withdrawal", Fixture, NULL, setup, test_nested_withdrawal, teardown);
	g_test_add("/attribution/private-history", Fixture, NULL, setup, test_private_history, teardown);
	g_test_add("/attribution/withdrawal", Fixture, NULL, setup, test_withdrawal, teardown);
	g_test_add("/attribution/rate-limit", Fixture, NULL, setup, test_rate_limit, teardown);
	g_test_add("/attribution/expiration", Fixture, NULL, setup, test_expiration, teardown);
	g_test_add("/attribution/disabled-withdrawal", Fixture, NULL, setup, test_disabled_withdrawal, teardown);
	g_test_add("/attribution/capture-result", Fixture, NULL, setup, test_capture_result, teardown);
	g_test_add("/attribution/form-capture", Fixture, NULL, setup, test_form_capture, teardown);
	g_test_add("/attribution/capture-address-permission", Fixture, NULL, setup, test_capture_address_permission, teardown);
	g_test_add("/attribution/conversion-binding", Fixture, NULL, setup, test_conversion_binding, teardown);
	g_test_add("/attribution/capture-address-recheck", Fixture, NULL, setup, test_capture_address_recheck, teardown);
	g_test_add("/attribution/signed-capture", Fixture, NULL, setup, test_signed_capture, teardown);
	g_test_add("/attribution/signed-boundaries", Fixture, NULL, setup, test_signed_boundaries, teardown);
	g_test_add("/attribution/site-removal", Fixture, NULL, setup, test_site_removal, teardown);
	g_test_add("/attribution/http", Fixture, NULL, setup, test_http, teardown);
	g_test_add("/attribution/http-hosted", Fixture, "hosted", setup, test_http, teardown);
	g_test_add("/attribution/report-surfaces", Fixture, NULL, setup, test_report_surfaces, teardown);
	g_test_add_func("/attribution/script", test_script);
	g_test_add("/attribution/report-options", Fixture, NULL, setup, test_report_options, teardown);
	g_test_add("/attribution/report-models", Fixture, NULL, setup, test_report_models, teardown);
	g_test_add("/attribution/financial-calendar-dates", Fixture, NULL, setup, test_financial_calendar_dates, teardown);
	g_test_add("/attribution/financial-journey", Fixture, NULL, setup, test_financial_journey, teardown);
	g_test_add("/attribution/restart", Fixture, restart_mode, setup, test_restart, teardown);
	g_test_add("/attribution/independent-permissions", Fixture, NULL, setup, test_independent_permissions, teardown);
	g_test_add("/attribution/configuration-authority", Fixture, NULL, setup, test_configuration_authority, teardown);
	g_test_add("/attribution/upgrade", Fixture, NULL, setup, test_upgrade, teardown);
	g_test_add("/attribution/migration-guard", Fixture, NULL, setup, test_migration_guard, teardown);
	return g_test_run();
}
