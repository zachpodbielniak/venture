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
int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/dunning/policy", Fixture, NULL, setup, test_policy, teardown);
	g_test_add("/dunning/cadence", Fixture, NULL, setup, test_cadence, teardown);
	g_test_add("/dunning/settlement", Fixture, NULL, setup, test_settlement, teardown);
	g_test_add("/dunning/optout", Fixture, NULL, setup, test_optout, teardown);
	g_test_add("/dunning/escalation", Fixture, NULL, setup, test_escalation, teardown);
	g_test_add("/dunning/report", Fixture, NULL, setup, test_report, teardown);
	return g_test_run();
}
