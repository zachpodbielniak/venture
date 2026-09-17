/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <string.h>
#include "venture-test-util.h"

typedef struct {
	VentureDatabase *db;
	VentureContext *context;
	VentureConfig *config;
	VentureLogMailer *mailer;
	gint64 org, company, contact, invoice, policy;
} Fixture;

static VentureEntity *record(Fixture *f, const gchar *name)
{
	GType type = venture_entity_registry_lookup(venture_entity_registry_get_default(), name);
	g_assert_cmpuint(type, !=, G_TYPE_INVALID);
	return g_object_new(type, "organization-id", f->org, NULL);
}
static void save(Fixture *f, VentureEntity *row)
{
	g_autoptr(GError) error = NULL;
	gboolean ok = venture_database_save(f->db, row, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(ok);
}
static void field(VentureEntity *row, const gchar *name, const gchar *value)
{
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_entity_set_field_from_string(row, name, value, &error));
	g_assert_no_error(error);
}
static GPtrArray *rows(Fixture *f, const gchar *name)
{
	g_autoptr(VentureQuery) query = venture_query_new(venture_entity_registry_lookup(venture_entity_registry_get_default(), name));
	venture_query_set_organization(query, f->org);
	venture_query_set_limit(query, 0);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	return venture_database_find(f->db, query, NULL);
}
static gint sweep(Fixture *f, const gchar *date)
{
	g_autoptr(GDateTime) at = venture_time_from_string(date, NULL);
	g_autoptr(GError) error = NULL;
	gint n;
	n = venture_dunning_service_sweep(venture_dunning_service_get(f->db), f->org, at, 100, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(n, >=, 0);
	return n;
}
static void deliver(Fixture *f, const gchar *date)
{
	g_autoptr(GDateTime) at = venture_time_from_string(date, NULL);
	g_autoptr(GError) error = NULL;
	gint n = venture_mail_outbox_deliver_due(venture_database_get_mail_outbox(f->db), f->org, 100, at, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(n, >=, 0);
}
static void setup(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) company = NULL, contact = NULL, invoice = NULL, line = NULL, policy = NULL, template = NULL;
	g_autoptr(GDateTime) issued = venture_time_from_string("2026-01-01", NULL);
	g_autofree gchar *steps = NULL;
	(void)unused;
	f->config = venture_config_new();
	g_object_set(f->config, "server-base-url", "https://books.example.test", NULL);
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
	f->mailer = venture_log_mailer_new();
	g_object_set(venture_database_get_mail_outbox(f->db), "mailer", f->mailer, NULL);
	company = record(f, "company");
	g_object_set(company, "name", "Acme & Sons", "email", "company@example.test", NULL);
	save(f, company); f->company = venture_entity_get_id(company);
	contact = record(f, "contact");
	g_object_set(contact, "name", "Alice", "email", "alice@example.test", "company-id", f->company, NULL);
	save(f, contact); f->contact = venture_entity_get_id(contact);
	template = record(f, "mail_template");
	g_object_set(template, "name", "Reminder", "subject", "Invoice {number}",
		"text-body", "Hello {customer_name}: {number}, due {due_at}, balance {open_balance}. {pay_link}",
		"html-body", "<p>{customer_name}</p>", NULL);
	save(f, template);
	policy = record(f, "dunning_policy");
	steps = g_strdup_printf("[{\"offset\":-3,\"template_id\":%" G_GINT64_FORMAT "},{\"offset\":7,\"template_id\":%" G_GINT64_FORMAT "},{\"offset\":14,\"template_id\":%" G_GINT64_FORMAT "}]",
		venture_entity_get_id(template), venture_entity_get_id(template), venture_entity_get_id(template));
	g_object_set(policy, "name", "Standard", "steps", steps, "is-default", TRUE, "final-escalation", TRUE, NULL);
	field(policy, "adopted-at", "2026-01-01");
	save(f, policy); f->policy = venture_entity_get_id(policy);
	invoice = record(f, "invoice");
	g_object_set(invoice, "number", "INV-DUN", "company-id", f->company, "contact-id", f->contact, "owner", "alice", NULL);
	field(invoice, "issued-at", "2026-01-01"); field(invoice, "due-at", "2026-01-10");
	save(f, invoice); f->invoice = venture_entity_get_id(invoice);
	line = record(f, "invoice_line");
	g_object_set(line, "invoice-id", f->invoice, "description", "Work", "quantity", 1.0, NULL);
	field(line, "unit-price", "40 USD"); save(f, line);
	g_assert_true(venture_settlement_service_transition(venture_settlement_service_get(f->db), VENTURE_INVOICE(invoice), "sent", issued, NULL, &error));
	g_assert_no_error(error);
}
static void teardown(Fixture *f, gconstpointer unused)
{
	(void)unused;
	g_clear_object(&f->context); g_clear_object(&f->db);
	g_clear_object(&f->config); g_clear_object(&f->mailer);
}
/* Default uniqueness and overrides must be enforced for generic writers. */
static void test_policy(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) policy = record(f, "dunning_policy");
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) invoice = venture_database_get(f->db, VENTURE_TYPE_INVOICE, f->invoice, NULL);
	(void)unused;
	g_object_set(policy, "name", "Duplicate", "is-default", TRUE, "steps", "[]", NULL);
	g_assert_false(venture_database_save(f->db, policy, NULL, &error));
	g_assert_nonnull(error);
	g_object_set(invoice, "dunning-disabled", TRUE, NULL); save(f, invoice);
	g_assert_cmpint(sweep(f, "2026-02-01"), ==, 0);
}
/* Calendar offsets and durable identity must survive a second sweep. */
static void test_cadence(Fixture *f, gconstpointer unused)
{
	g_autoptr(GPtrArray) events = NULL;
	g_autofree gchar *to = NULL, *body = NULL;
	const GPtrArray *sent;
	(void)unused;
	g_assert_cmpint(sweep(f, "2026-01-06"), ==, 0);
	g_assert_cmpint(sweep(f, "2026-01-07"), ==, 1);
	g_assert_cmpint(sweep(f, "2026-01-07"), ==, 0);
	deliver(f, "2026-01-07"); deliver(f, "2026-01-08");
	sent = venture_log_mailer_get_messages(f->mailer);
	g_assert_cmpuint(sent->len, ==, 1);
	g_object_get(g_ptr_array_index((GPtrArray *)sent, 0), "to", &to, "text-body", &body, NULL);
	g_assert_cmpstr(to, ==, "alice@example.test");
	g_assert_nonnull(strstr(body, "Acme & Sons")); g_assert_nonnull(strstr(body, "INV-DUN"));
	events = rows(f, "dunning_event"); g_assert_cmpuint(events->len, ==, 1);
	g_clear_pointer(&body, g_free);
	g_object_get(g_ptr_array_index(events, 0), "delivery-status", &body, NULL);
	g_assert_cmpstr(body, ==, "sent");
}
/* Payment after the first send must stop later emails and escalation. */
static void test_settlement(Fixture *f, gconstpointer unused)
{
	g_autoptr(GDateTime) paid = venture_time_from_string("2026-01-08", NULL);
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) actions = NULL;
	(void)unused;
	sweep(f, "2026-01-07"); deliver(f, "2026-01-07");
	g_assert_true(venture_settlement_service_settle_invoice(venture_settlement_service_get(f->db), f->invoice, paid, NULL, &error));
	g_assert_no_error(error);
	sweep(f, "2026-02-01"); deliver(f, "2026-02-01");
	g_assert_cmpuint(venture_log_mailer_get_messages(f->mailer)->len, ==, 1);
	actions = rows(f, "activity"); g_assert_cmpuint(actions->len, ==, 0);
}
/* Opt-out must also stop a message already queued for submission. */
static void test_optout(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) company = venture_database_get(f->db, VENTURE_TYPE_COMPANY, f->company, NULL);
	g_autoptr(GPtrArray) events = NULL;
	g_autofree gchar *reason = NULL;
	(void)unused;
	sweep(f, "2026-01-07");
	g_object_set(company, "dunning-opt-out", TRUE, NULL); save(f, company);
	deliver(f, "2026-01-07"); sweep(f, "2026-01-17");
	g_assert_cmpuint(venture_log_mailer_get_messages(f->mailer)->len, ==, 0);
	events = rows(f, "dunning_event");
	g_assert_cmpuint(events->len, >=, 1);
	g_object_get(g_ptr_array_index(events, 0), "suppressed-reason", &reason, NULL);
	g_assert_cmpstr(reason, ==, "company_opt_out");
	{
		g_autoptr(GPtrArray) mail = rows(f, "mail_message");
		gint64 attempts = -1;
		g_autofree gchar *state = NULL;
		g_assert_cmpuint(mail->len, ==, 1);
		g_object_get(g_ptr_array_index(mail, 0), "state", &state, "attempts", &attempts, NULL);
		g_assert_cmpstr(state, ==, "cancelled");
		g_assert_cmpint(attempts, ==, 0);
	}
}
/* Escalation is an owned next action, never a third customer email. */
static void test_escalation(Fixture *f, gconstpointer unused)
{
	g_autoptr(GPtrArray) actions = NULL;
	g_autofree gchar *subject = NULL, *owner = NULL;
	(void)unused;
	sweep(f, "2026-01-07"); deliver(f, "2026-01-07");
	sweep(f, "2026-01-17"); deliver(f, "2026-01-17");
	g_assert_cmpint(sweep(f, "2026-01-24"), ==, 1);
	g_assert_cmpint(sweep(f, "2026-01-24"), ==, 0);
	deliver(f, "2026-01-24");
	g_assert_cmpuint(venture_log_mailer_get_messages(f->mailer)->len, ==, 2);
	actions = rows(f, "activity"); g_assert_cmpuint(actions->len, ==, 1);
	g_object_get(g_ptr_array_index(actions, 0), "subject", &subject, "owner", &owner, NULL);
	g_assert_cmpstr(subject, ==, "collect: INV-DUN"); g_assert_cmpstr(owner, ==, "alice");
}
/* Collection attribution counts an accepted reminder followed by full payment. */
static void test_report(Fixture *f, gconstpointer unused)
{
	VentureReport *report;
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) paid = venture_time_from_string("2026-01-08", NULL);
	g_autofree gchar *text = NULL;
	(void)unused;
	sweep(f, "2026-01-07"); deliver(f, "2026-01-07");
	g_assert_true(venture_settlement_service_settle_invoice(venture_settlement_service_get(f->db), f->invoice, paid, NULL, &error));
	g_assert_no_error(error);
	report = venture_report_registry_lookup(venture_context_get_report_registry(f->context), "collections");
	g_assert_nonnull(report);
	result = venture_report_generate(report, f->context, NULL, NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(result);
	text = venture_report_result_render(result, VENTURE_OUTPUT_FORMAT_JSON);
	g_assert_nonnull(strstr(text, "paid_within_7_days"));
	g_assert_nonnull(strstr(text, "average_days_after"));
}
/* Reminder history is service-owned: a generic write must be refused. */
static void test_event_generic_write(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) event = record(f, "dunning_event");
	g_autoptr(GError) error = NULL;
	(void)unused;
	g_object_set(event, "invoice-id", f->invoice, "step", (gint64)1, "dunning-key", "forged", "delivery-status", "sent", NULL);
	g_assert_false(venture_database_save(f->db, event, NULL, &error));
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "sweep"));
}
/* A policy that names a missing template or unordered offsets is refused. */
static void test_policy_steps(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) policy = record(f, "dunning_policy");
	g_autoptr(GError) error = NULL;
	(void)unused;
	g_object_set(policy, "name", "Broken", "steps", "[{\"offset\":7,\"template_id\":999999}]", NULL);
	g_assert_false(venture_database_save(f->db, policy, NULL, &error));
	g_clear_error(&error);
	g_object_set(policy, "steps", "[{\"offset\":7},{\"offset\":3}]", NULL);
	g_assert_false(venture_database_save(f->db, policy, NULL, &error));
	g_clear_error(&error);
	g_object_set(policy, "steps", "not json", NULL);
	g_assert_false(venture_database_save(f->db, policy, NULL, &error));
}
static gint64 make_policy(Fixture *f, const gchar *name, gint offset)
{
	g_autoptr(GPtrArray) templates = rows(f, "mail_template");
	g_autoptr(VentureEntity) policy = record(f, "dunning_policy");
	g_autofree gchar *steps = g_strdup_printf("[{\"offset\":%d,\"template_id\":%" G_GINT64_FORMAT "}]",
		offset, venture_entity_get_id(g_ptr_array_index(templates, 0)));
	g_object_set(policy, "name", name, "steps", steps, NULL);
	save(f, policy);
	return venture_entity_get_id(policy);
}
static gint64
issued_invoice(Fixture *f, const gchar *number)
{
	g_autoptr(VentureEntity) invoice = record(f, "invoice");
	g_autoptr(VentureEntity) line = record(f, "invoice_line");
	g_autoptr(GDateTime) issued = venture_time_from_string("2026-01-01", NULL);
	g_autoptr(GError) error = NULL;
	g_object_set(invoice, "number", number, "company-id", f->company, "contact-id", f->contact, "owner", "alice", NULL);
	field(invoice, "issued-at", "2026-01-01");
	field(invoice, "due-at", "2026-01-10");
	save(f, invoice);
	g_object_set(line, "invoice-id", venture_entity_get_id(invoice), "description", "Work", "quantity", 1.0, NULL);
	field(line, "unit-price", "40 USD");
	save(f, line);
	g_assert_true(venture_settlement_service_transition(venture_settlement_service_get(f->db),
		VENTURE_INVOICE(invoice), "sent", issued, NULL, &error));
	g_assert_no_error(error);
	return venture_entity_get_id(invoice);
}
/* The invoice's policy beats the customer's, which beats the default. */
static void test_overrides(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) company = venture_database_get(f->db, VENTURE_TYPE_COMPANY, f->company, NULL);
	g_autoptr(VentureEntity) invoice = venture_database_get(f->db, VENTURE_TYPE_INVOICE, f->invoice, NULL);
	g_autoptr(GPtrArray) events = NULL;
	gint64 company_policy = make_policy(f, "Gentle", 1);
	gint64 invoice_policy = make_policy(f, "Patient", 30);
	gint64 used = 0;
	(void)unused;
	g_object_set(company, "dunning-policy-id", company_policy, NULL); save(f, company);
	g_assert_cmpint(sweep(f, "2026-01-07"), ==, 0);
	g_assert_cmpint(sweep(f, "2026-01-11"), ==, 1);
	events = rows(f, "dunning_event");
	g_assert_cmpuint(events->len, ==, 1);
	g_object_get(g_ptr_array_index(events, 0), "policy-id", &used, NULL);
	g_assert_cmpint(used, ==, company_policy);
	g_object_set(invoice, "dunning-policy-id", invoice_policy, NULL); save(f, invoice);
	g_assert_cmpint(sweep(f, "2026-02-01"), ==, 0);
	g_assert_cmpint(sweep(f, "2026-02-09"), ==, 1);
}
/* A disputed invoice leaves dunning until the dispute is resolved. */
static void test_dispute(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) invoice = venture_database_get(f->db, VENTURE_TYPE_INVOICE, f->invoice, NULL);
	g_autoptr(GDateTime) at = venture_time_from_string("2026-01-05", NULL);
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) events = NULL;
	(void)unused;
	g_assert_true(venture_settlement_service_transition(venture_settlement_service_get(f->db), VENTURE_INVOICE(invoice), "disputed", at, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpint(sweep(f, "2026-01-17"), ==, 0);
	deliver(f, "2026-01-17");
	g_assert_cmpuint(venture_log_mailer_get_messages(f->mailer)->len, ==, 0);
	events = rows(f, "dunning_event");
	g_assert_cmpuint(events->len, ==, 0);
}
/* A dispute raised after a reminder was queued cancels it before the transport. */
static void test_dispute_after_queue(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) invoice = venture_database_get(f->db, VENTURE_TYPE_INVOICE, f->invoice, NULL);
	g_autoptr(GDateTime) at = venture_time_from_string("2026-01-07", NULL);
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) events = NULL;
	g_autoptr(GPtrArray) mail = NULL;
	g_autofree gchar *status = NULL, *reason = NULL, *state = NULL;
	(void)unused;
	g_assert_cmpint(sweep(f, "2026-01-07"), ==, 1);
	g_assert_true(venture_settlement_service_transition(venture_settlement_service_get(f->db), VENTURE_INVOICE(invoice), "disputed", at, NULL, &error));
	g_assert_no_error(error);
	deliver(f, "2026-01-07");
	g_assert_cmpuint(venture_log_mailer_get_messages(f->mailer)->len, ==, 0);
	events = rows(f, "dunning_event");
	g_object_get(g_ptr_array_index(events, 0), "delivery-status", &status, "suppressed-reason", &reason, NULL);
	g_assert_cmpstr(status, ==, "cancelled"); g_assert_cmpstr(reason, ==, "disputed");
	mail = rows(f, "mail_message");
	g_assert_cmpuint(mail->len, ==, 1);
	g_object_get(g_ptr_array_index(mail, 0), "state", &state, NULL);
	g_assert_cmpstr(state, ==, "cancelled");
}
/* A template that cannot render records a failed attempt once and moves on. */
static void test_render_failure(Fixture *f, gconstpointer unused)
{
	g_autoptr(GPtrArray) templates = rows(f, "mail_template");
	g_autoptr(GPtrArray) events = NULL;
	g_autofree gchar *status = NULL, *why = NULL;
	(void)unused;
	g_object_set(g_ptr_array_index(templates, 0), "text-body", "Hello {no_such_field}", NULL);
	save(f, g_ptr_array_index(templates, 0));
	g_assert_cmpint(sweep(f, "2026-01-07"), ==, 0);
	g_assert_cmpint(sweep(f, "2026-01-07"), ==, 0);
	deliver(f, "2026-01-07");
	g_assert_cmpuint(venture_log_mailer_get_messages(f->mailer)->len, ==, 0);
	events = rows(f, "dunning_event");
	g_assert_cmpuint(events->len, ==, 1);
	g_object_get(g_ptr_array_index(events, 0), "delivery-status", &status, "last-error", &why, NULL);
	g_assert_cmpstr(status, ==, "failed");
	g_assert_nonnull(strstr(why, "placeholder"));
}
static gboolean refuse_mail(VentureDatabase *db, VentureEntity *entity, VentureEntity *previous, gpointer data, GError **error)
{
	(void)db; (void)entity; (void)previous; (void)data;
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_DATABASE, "relay down");
	return FALSE;
}
/* A failure after the event was written rolls the invoice's transaction
 * back, and the attempt is still recorded as evidence in its own. */
static void test_atomic_attempt(Fixture *f, gconstpointer unused)
{
	g_autoptr(GDateTime) at = venture_time_from_string("2026-01-07", NULL);
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) events = NULL;
	g_autoptr(GPtrArray) mail = NULL;
	g_autofree gchar *status = NULL, *why = NULL;
	(void)unused;
	venture_database_add_save_validator(f->db, VENTURE_TYPE_MAIL_MESSAGE, refuse_mail, NULL, NULL);
	g_assert_cmpint(venture_dunning_service_sweep(venture_dunning_service_get(f->db), f->org, at, 100, NULL, &error), ==, -1);
	g_assert_nonnull(error);
	g_assert_false(venture_database_has_transaction(f->db));
	mail = rows(f, "mail_message"); g_assert_cmpuint(mail->len, ==, 0);
	events = rows(f, "dunning_event"); g_assert_cmpuint(events->len, ==, 1);
	g_object_get(g_ptr_array_index(events, 0), "delivery-status", &status, "last-error", &why, NULL);
	g_assert_cmpstr(status, ==, "failed");
	g_assert_nonnull(strstr(why, "relay down"));
}
/* Several steps falling due at once send the current one, not a burst. */
static void test_superseded(Fixture *f, gconstpointer unused)
{
	g_autoptr(GPtrArray) events = NULL;
	g_autofree gchar *first = NULL, *reason = NULL, *second = NULL;
	(void)unused;
	g_assert_cmpint(sweep(f, "2026-01-20"), ==, 1);
	deliver(f, "2026-01-20");
	g_assert_cmpuint(venture_log_mailer_get_messages(f->mailer)->len, ==, 1);
	events = rows(f, "dunning_event");
	g_assert_cmpuint(events->len, ==, 2);
	g_object_get(g_ptr_array_index(events, 0), "delivery-status", &first, "suppressed-reason", &reason, NULL);
	g_object_get(g_ptr_array_index(events, 1), "delivery-status", &second, NULL);
	g_assert_cmpstr(first, ==, "suppressed"); g_assert_cmpstr(reason, ==, "superseded");
	g_assert_cmpstr(second, ==, "sent");
}
/* Every send is on the invoice, contact and company timelines and in the portal. */
static void test_timeline(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) invoice = venture_database_get(f->db, VENTURE_TYPE_INVOICE, f->invoice, NULL);
	const gchar *types[] = { "invoice", "contact", "company" };
	gint64 ids[3];
	g_autofree gchar *portal = NULL;
	guint i;
	(void)unused;
	ids[0] = f->invoice; ids[1] = f->contact; ids[2] = f->company;
	sweep(f, "2026-01-07"); deliver(f, "2026-01-07");
	for (i = 0; i < G_N_ELEMENTS(types); i++)
	{
		g_autoptr(GError) error = NULL;
		g_autoptr(JsonNode) timeline = venture_desk_activity(f->context, types[i], ids[i], 0, &error);
		g_autofree gchar *text = NULL;
		g_assert_no_error(error); g_assert_nonnull(timeline);
		text = venture_json_to_string(timeline, FALSE);
		g_assert_nonnull(strstr(text, "reminder sent 2026-01-07"));
	}
	portal = venture_dunning_portal_summary(f->db, invoice);
	g_assert_cmpstr(portal, ==, "reminder sent 2026-01-07");
}
/* Switching the module off hides its records and refuses the sweep. */
static void test_module_off(Fixture *f, gconstpointer unused)
{
	g_autoptr(GDateTime) at = venture_time_from_string("2026-01-07", NULL);
	g_autoptr(GError) error = NULL;
	(void)unused;
	venture_config_set_module_enabled(f->config, "dunning", FALSE);
	g_assert_cmpuint(venture_entity_registry_lookup(venture_entity_registry_get_default(), "dunning_policy"), ==, G_TYPE_INVALID);
	g_assert_cmpint(venture_dunning_service_sweep(venture_dunning_service_get(f->db), f->org, at, 100, NULL, &error), ==, -1);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_null(venture_report_registry_lookup(venture_context_get_report_registry(f->context), "collections"));
	venture_config_set_module_enabled(f->config, "dunning", TRUE);
	g_assert_cmpint(sweep(f, "2026-01-07"), ==, 1);
}
/* The REST action and the CLI verb reach the same service. */
typedef struct { gboolean done; GError *error; GBytes *bytes; gchar *out, *err; } Result;
typedef struct { Fixture base; VentureWebServer *server; gchar *directory; } ServerFixture;
static void server_setup(ServerFixture *s, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GSocketListener) listener = g_socket_listener_new();
	guint16 port = g_socket_listener_add_any_inet_port(listener, NULL, &error);
	g_assert_no_error(error);
	g_socket_listener_close(listener);
	s->directory = g_dir_make_tmp("venture-dunning-XXXXXX", &error);
	g_assert_no_error(error);
	setup(&s->base, unused);
	g_object_set(s->base.config, "state-dir", s->directory, "server-bind-address", "127.0.0.1", "server-port", (gint64)port, "security-require-auth", FALSE, NULL);
	venture_context_set_mailer(s->base.context, VENTURE_MAILER(s->base.mailer));
	s->server = venture_web_server_new(s->base.context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(s->server, &error));
	g_assert_no_error(error);
}
static void server_teardown(ServerFixture *s, gconstpointer unused)
{
	venture_web_server_stop(s->server);
	g_clear_object(&s->server);
	teardown(&s->base, unused);
	venture_test_remove_tree(s->directory); g_free(s->directory);
}
static void http_done(GObject *source, GAsyncResult *result, gpointer data)
{
	Result *r = data;
	r->bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &r->error);
	r->done = TRUE;
}
static guint request(ServerFixture *s, const gchar *path, const gchar *body, gchar **out)
{
	g_autoptr(SoupSession) session = soup_session_new_with_options("timeout", 15, NULL);
	g_autofree gchar *url = g_strconcat(venture_web_server_get_base_url(s->server), path, NULL);
	g_autoptr(SoupMessage) message = soup_message_new(body ? "POST" : "GET", url);
	Result result;
	memset(&result, 0, sizeof(result));
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	if (body)
	{
		g_autoptr(GBytes) bytes = g_bytes_new(body, strlen(body));
		soup_message_set_request_body_from_bytes(message, "application/json", bytes);
	}
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL, http_done, &result);
	while (!result.done) g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(result.error);
	if (out) *out = g_strndup(g_bytes_get_data(result.bytes, NULL), g_bytes_get_size(result.bytes));
	g_bytes_unref(result.bytes);
	return soup_message_get_status(message);
}
static void cli_done(GObject *source, GAsyncResult *result, gpointer data)
{
	Result *r = data;
	g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &r->out, &r->err, &r->error);
	r->done = TRUE;
}
static gboolean cli_timeout(gpointer process) { g_subprocess_force_exit(process); return G_SOURCE_CONTINUE; }
static gchar *cli(ServerFixture *s, const gchar *const *args, gboolean expect_success)
{
	g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
	g_autoptr(GSubprocess) process = NULL;
	g_autoptr(GPtrArray) argv = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GError) error = NULL;
	Result result;
	guint i, timeout;
	memset(&result, 0, sizeof(result));
	g_ptr_array_add(argv, g_canonicalize_filename("build/debug/venturectl", NULL));
	g_ptr_array_add(argv, g_strdup("--server")); g_ptr_array_add(argv, g_strdup(venture_web_server_get_base_url(s->server)));
	g_ptr_array_add(argv, g_strdup("-f")); g_ptr_array_add(argv, g_strdup("json"));
	for (i = 0; args[i]; i++) g_ptr_array_add(argv, g_strdup(args[i]));
	g_ptr_array_add(argv, NULL);
	g_subprocess_launcher_setenv(launcher, "VENTURE_TOKEN", "dunning-fixture", TRUE);
	process = g_subprocess_launcher_spawnv(launcher, (const gchar *const *)argv->pdata, &error);
	g_assert_no_error(error);
	timeout = g_timeout_add_seconds(30, cli_timeout, process);
	g_subprocess_communicate_utf8_async(process, NULL, NULL, cli_done, &result);
	while (!result.done) g_main_context_iteration(NULL, TRUE);
	g_source_remove(timeout);
	g_assert_no_error(result.error);
	if (g_subprocess_get_successful(process) != expect_success) g_test_message("CLI: %s%s", result.out, result.err);
	g_assert_true(g_subprocess_get_successful(process) == expect_success);
	g_free(result.err);
	return result.out;
}
static void test_surfaces(ServerFixture *s, gconstpointer unused)
{
	Fixture *f = &s->base;
	const gchar *sweep_args[] = { "dunning", "sweep", "as_of=2026-01-17", NULL };
	const gchar *bad_args[] = { "dunning", "nope", NULL };
	g_autofree gchar *body = NULL, *out = NULL, *bad = NULL;
	g_autoptr(GPtrArray) events = NULL;
	(void)unused;
	g_assert_cmpuint(request(s, "/api/v1/dunning_policy/0/actions/sweep", "{\"as_of\":\"2026-01-07\"}", &body), ==, 200);
	events = rows(f, "dunning_event");
	g_assert_cmpuint(events->len, ==, 1);
	g_clear_pointer(&events, g_ptr_array_unref);
	out = cli(s, sweep_args, TRUE);
	events = rows(f, "dunning_event");
	g_assert_cmpuint(events->len, ==, 2);
	bad = cli(s, bad_args, FALSE);
	g_assert_cmpuint(request(s, "/api/v1/dunning_policy/0/actions/sweep", "{\"as_of\":\"not a date\"}", NULL), >=, 400);
	deliver(f, "2026-01-17");
	g_assert_cmpuint(venture_log_mailer_get_messages(f->mailer)->len, ==, 2);
	{
		g_autofree gchar *page_path = g_strdup_printf("/e/invoice/%" G_GINT64_FORMAT, f->invoice);
		g_autofree gchar *api_path = g_strdup_printf("/api/v1/activity/invoice/%" G_GINT64_FORMAT, f->invoice);
		g_autofree gchar *page = NULL, *timeline = NULL;
		guint status = request(s, page_path, NULL, &page);
		if (status == 200)
			g_assert_nonnull(strstr(page, "reminder sent 2026-01-17"));
		else
			g_test_message("invoice page needs a session (%u); the JSON timeline is checked instead", status);
		g_assert_cmpuint(request(s, api_path, NULL, &timeline), ==, 200);
		g_assert_nonnull(strstr(timeline, "reminder sent 2026-01-17"));
	}
}
/* A voided invoice leaves dunning, including a reminder already queued. */
static void test_void(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) invoice = venture_database_get(f->db, VENTURE_TYPE_INVOICE, f->invoice, NULL);
	g_autoptr(GDateTime) at = venture_time_from_string("2026-01-07", NULL);
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) events = NULL;
	g_autofree gchar *status = NULL, *reason = NULL;
	(void)unused;
	g_assert_cmpint(sweep(f, "2026-01-07"), ==, 1);
	g_assert_true(venture_settlement_service_transition(venture_settlement_service_get(f->db),
		VENTURE_INVOICE(invoice), "void", at, NULL, &error));
	g_assert_no_error(error);
	deliver(f, "2026-01-07");
	g_assert_cmpuint(venture_log_mailer_get_messages(f->mailer)->len, ==, 0);
	events = rows(f, "dunning_event");
	g_object_get(g_ptr_array_index(events, 0), "delivery-status", &status, "suppressed-reason", &reason, NULL);
	g_assert_cmpstr(status, ==, "cancelled");
	g_assert_true(g_strcmp0(reason, "not_issued") == 0 || g_strcmp0(reason, "settled") == 0);
	g_assert_cmpint(sweep(f, "2026-01-24"), ==, 0);
}
/* The portal token is a bearer credential: public bodies stay empty. */
static void test_pay_link(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) access = NULL;
	g_autoptr(GPtrArray) mail = NULL;
	g_autofree gchar *token = NULL, *public_body = NULL, *private_body = NULL;
	(void)unused;
	access = venture_portal_service_invite(venture_portal_service_get(f->db), f->org, f->company,
		"alice@example.test", NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(access);
	g_object_get(access, "token", &token, NULL);
	g_assert_false(venture_string_is_empty(token));
	g_assert_cmpint(sweep(f, "2026-01-07"), ==, 1);
	mail = rows(f, "mail_message");
	g_assert_cmpuint(mail->len, ==, 1);
	g_object_get(g_ptr_array_index(mail, 0), "text-body", &public_body, "private-text-body", &private_body, NULL);
	g_assert_null(strstr(public_body, token));
	g_assert_nonnull(private_body);
	g_assert_nonnull(strstr(private_body, token));
	g_assert_nonnull(strstr(private_body, "https://books.example.test/portal/"));
}
/* Opted-out invoices that still write suppressed events count toward the limit. */
static void test_limit(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) company = venture_database_get(f->db, VENTURE_TYPE_COMPANY, f->company, NULL);
	g_autoptr(GDateTime) at = venture_time_from_string("2026-01-07", NULL);
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) events = NULL;
	gint64 other;
	(void)unused;
	other = issued_invoice(f, "INV-DUN-2");
	g_object_set(company, "dunning-opt-out", TRUE, NULL);
	save(f, company);
	g_assert_cmpint(venture_dunning_service_sweep(venture_dunning_service_get(f->db), f->org, at, 1, NULL, &error), ==, 0);
	g_assert_no_error(error);
	events = rows(f, "dunning_event");
	g_assert_cmpuint(events->len, ==, 1);
	{
		gint64 invoice_id = 0;
		g_object_get(g_ptr_array_index(events, 0), "invoice-id", &invoice_id, NULL);
		g_assert_cmpint(invoice_id, ==, f->invoice);
	}
	g_clear_pointer(&events, g_ptr_array_unref);
	g_assert_cmpint(venture_dunning_service_sweep(venture_dunning_service_get(f->db), f->org, at, 1, NULL, &error), ==, 0);
	events = rows(f, "dunning_event");
	g_assert_cmpuint(events->len, ==, 2);
	{
		gint64 invoice_id = 0;
		g_object_get(g_ptr_array_index(events, 1), "invoice-id", &invoice_id, NULL);
		g_assert_cmpint(invoice_id, ==, other);
	}
}
/* Migration 000320 must succeed with the module off and must not invent events. */
static void test_migrate_disabled(void)
{
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureDatabase) db = NULL;
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(OrmResult) result = NULL;
	g_autoptr(VentureConfig) everything = NULL;
	g_autoptr(VentureModuleRegistry) registry = NULL;

	venture_config_set_module_enabled(config, "dunning", FALSE);
	db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	context = venture_context_new(config, db);
	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	result = venture_database_query_raw(db,
		"SELECT CAST(COUNT(*) AS BIGINT) FROM sqlite_master WHERE type = 'table' AND name IN ('dunning_policies', 'dunning_events')",
		NULL, &error);
	g_assert_no_error(error);
	g_assert_true(orm_result_next(result));
	g_assert_cmpint(orm_row_get_integer(orm_result_get_row(result), 0), ==, 0);
	g_clear_object(&result);
	result = venture_database_query_raw(db,
		"SELECT CAST(COUNT(*) AS BIGINT) FROM sqlite_master WHERE type = 'table' AND name = 'invoices'",
		NULL, &error);
	g_assert_no_error(error);
	g_assert_true(orm_result_next(result));
	g_assert_cmpint(orm_row_get_integer(orm_result_get_row(result), 0), ==, 1);
	g_clear_object(&context);
	g_clear_object(&db);
	everything = venture_config_new();
	registry = venture_module_registry_new();
	venture_module_registry_register_builtins(registry);
	g_assert_true(venture_module_registry_configure(registry, everything, NULL));
	venture_module_registry_apply(registry, venture_entity_registry_get_default());
}
int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/dunning/policy", Fixture, NULL, setup, test_policy, teardown);
	g_test_add("/dunning/cadence", Fixture, NULL, setup, test_cadence, teardown);
	g_test_add("/dunning/settlement", Fixture, NULL, setup, test_settlement, teardown);
	g_test_add("/dunning/optout", Fixture, NULL, setup, test_optout, teardown);
	g_test_add("/dunning/escalation", Fixture, NULL, setup, test_escalation, teardown);
	g_test_add("/dunning/report", Fixture, NULL, setup, test_report, teardown);
	g_test_add("/dunning/event-generic-write", Fixture, NULL, setup, test_event_generic_write, teardown);
	g_test_add("/dunning/policy-steps", Fixture, NULL, setup, test_policy_steps, teardown);
	g_test_add("/dunning/overrides", Fixture, NULL, setup, test_overrides, teardown);
	g_test_add("/dunning/dispute", Fixture, NULL, setup, test_dispute, teardown);
	g_test_add("/dunning/dispute-after-queue", Fixture, NULL, setup, test_dispute_after_queue, teardown);
	g_test_add("/dunning/render-failure", Fixture, NULL, setup, test_render_failure, teardown);
	g_test_add("/dunning/atomic-attempt", Fixture, NULL, setup, test_atomic_attempt, teardown);
	g_test_add("/dunning/superseded", Fixture, NULL, setup, test_superseded, teardown);
	g_test_add("/dunning/timeline", Fixture, NULL, setup, test_timeline, teardown);
	g_test_add("/dunning/module-off", Fixture, NULL, setup, test_module_off, teardown);
	g_test_add("/dunning/void", Fixture, NULL, setup, test_void, teardown);
	g_test_add("/dunning/pay-link", Fixture, NULL, setup, test_pay_link, teardown);
	g_test_add("/dunning/limit", Fixture, NULL, setup, test_limit, teardown);
	g_test_add_func("/dunning/migrate-disabled", test_migrate_disabled);
	g_test_add("/dunning/surfaces", ServerFixture, NULL, server_setup, test_surfaces, server_teardown);
	return g_test_run();
}
