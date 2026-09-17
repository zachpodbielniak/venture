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
	/* A failed or still-queued step is visible, not folded into nothing. */
	g_assert_nonnull(strstr(text, "\"failed\""));
	g_assert_nonnull(strstr(text, "\"queued\""));
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
 * back, and the attempt is still recorded as evidence in its own. The sweep
 * then goes on: it used to return -1 on the first failing invoice, so every
 * invoice behind it was never reminded, every day, until someone noticed. */
static void test_atomic_attempt(Fixture *f, gconstpointer unused)
{
	g_autoptr(GDateTime) at = venture_time_from_string("2026-01-07", NULL);
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) events = NULL;
	g_autoptr(GPtrArray) mail = NULL;
	g_autoptr(JsonNode) answer = NULL;
	JsonObject *object;
	guint i;
	(void)unused;
	issued_invoice(f, "INV-DUN-2");
	venture_database_add_save_validator(f->db, VENTURE_TYPE_MAIL_MESSAGE, refuse_mail, NULL, NULL);
	g_test_expect_message("Venture", G_LOG_LEVEL_WARNING, "dunning: invoice #*failed and was skipped:*relay down*");
	g_test_expect_message("Venture", G_LOG_LEVEL_WARNING, "dunning: invoice #*failed and was skipped:*relay down*");
	answer = venture_dunning_service_sweep_detailed(venture_dunning_service_get(f->db), f->org, at, 100, FALSE, NULL, &error);
	g_test_assert_expected_messages();
	g_assert_no_error(error);
	g_assert_nonnull(answer);
	object = json_node_get_object(answer);
	g_assert_cmpint(json_object_get_int_member(object, "failed"), ==, 2);
	g_assert_cmpint(json_object_get_int_member(object, "queued"), ==, 0);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(object, "failed_invoice_ids")), ==, 2);
	g_assert_cmpint(json_array_get_int_element(json_object_get_array_member(object, "failed_invoice_ids"), 0), ==, f->invoice);
	g_assert_false(venture_database_has_transaction(f->db));
	mail = rows(f, "mail_message"); g_assert_cmpuint(mail->len, ==, 0);
	events = rows(f, "dunning_event"); g_assert_cmpuint(events->len, ==, 2);
	for (i = 0; i < events->len; i++)
	{
		g_autofree gchar *status = NULL, *why = NULL;
		g_object_get(g_ptr_array_index(events, i), "delivery-status", &status, "last-error", &why, NULL);
		g_assert_cmpstr(status, ==, "failed");
		g_assert_nonnull(strstr(why, "relay down"));
	}
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
	g_autofree gchar *org = g_strdup_printf("organization_id=%" G_GINT64_FORMAT, f->org);
	const gchar *preview_args[] = { "dunning", "sweep", "as_of=2026-01-17", org, "limit=10", "dry_run=true", NULL };
	const gchar *sweep_args[] = { "dunning", "sweep", "as_of=2026-01-17", org, "limit=10", NULL };
	const gchar *bad_args[] = { "dunning", "nope", NULL };
	g_autofree gchar *body = NULL, *out = NULL, *bad = NULL, *preview = NULL;
	g_autoptr(GPtrArray) events = NULL;
	g_autoptr(JsonNode) response = NULL;
	g_autoptr(JsonNode) answer = NULL;
	(void)unused;
	g_assert_cmpuint(request(s, "/api/v1/dunning_policy/0/actions/sweep", "{\"as_of\":\"2026-01-07\"}", &body), ==, 200);
	/* The action answers with what it did, not a blank policy. */
	response = json_from_string(body, NULL);
	g_assert_nonnull(response);
	answer = json_from_string(json_object_get_string_member(json_node_get_object(response), "last_sweep"), NULL);
	g_assert_nonnull(answer);
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(answer), "queued"), ==, 1);
	events = rows(f, "dunning_event");
	g_assert_cmpuint(events->len, ==, 1);
	g_clear_pointer(&events, g_ptr_array_unref);
	deliver(f, "2026-01-07");
	/* Typed CLI arguments: organization_id and limit are integers, dry_run a
	 * boolean; as text the action refused all three. */
	preview = cli(s, preview_args, TRUE);
	g_assert_nonnull(strstr(preview, "plan"));
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
/* --- Hardening ------------------------------------------------------------ */

static JsonNode *sweep_answer(Fixture *f, const gchar *date, gboolean dry_run)
{
	g_autoptr(GDateTime) at = venture_time_from_string(date, NULL);
	g_autoptr(GError) error = NULL;
	JsonNode *answer = venture_dunning_service_sweep_detailed(venture_dunning_service_get(f->db), f->org, at, 100, dry_run, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(answer);
	return answer;
}
static gint64 answer_int(JsonNode *answer, const gchar *member)
{
	return json_object_get_int_member(json_node_get_object(answer), member);
}
static gchar *row_text(GPtrArray *table, guint i, const gchar *name)
{
	gchar *value = NULL;
	g_object_get(g_ptr_array_index(table, i), name, &value, NULL);
	return value;
}
static gint64 row_number(GPtrArray *table, guint i, const gchar *name)
{
	gint64 value = 0;
	g_object_get(g_ptr_array_index(table, i), name, &value, NULL);
	return value;
}
static void deliver_at(Fixture *f, GDateTime *at)
{
	g_autoptr(GError) error = NULL;
	gint n = venture_mail_outbox_deliver_due(venture_database_get_mail_outbox(f->db), f->org, 100, at, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(n, >=, 0);
}
static void assert_event(Fixture *f, guint index, const gchar *status, const gchar *reason)
{
	g_autoptr(GPtrArray) events = rows(f, "dunning_event");
	g_autofree gchar *got_status = NULL, *got_reason = NULL;
	g_assert_cmpuint(events->len, >, index);
	got_status = row_text(events, index, "delivery-status");
	got_reason = row_text(events, index, "suppressed-reason");
	g_assert_cmpstr(got_status, ==, status);
	if (reason != NULL)
		g_assert_cmpstr(got_reason, ==, reason);
}
static gint64 template_id(Fixture *f)
{
	g_autoptr(GPtrArray) templates = rows(f, "mail_template");
	return venture_entity_get_id(g_ptr_array_index(templates, 0));
}
/* The fixture policy with offsets @offsets; @escalate_last omits the last
 * step's template, which only an escalating last step may do. */
static void set_offsets(Fixture *f, const gint *offsets, guint count, gboolean escalate_last)
{
	g_autoptr(VentureEntity) policy = venture_database_get(f->db, VENTURE_TYPE_DUNNING_POLICY, f->policy, NULL);
	g_autoptr(GString) steps = g_string_new("[");
	guint i;
	for (i = 0; i < count; i++)
	{
		if (escalate_last && i + 1 == count)
			g_string_append_printf(steps, "%s{\"offset\":%d}", i ? "," : "", offsets[i]);
		else
			g_string_append_printf(steps, "%s{\"offset\":%d,\"template_id\":%" G_GINT64_FORMAT "}", i ? "," : "", offsets[i], template_id(f));
	}
	g_string_append(steps, "]");
	g_object_set(policy, "steps", steps->str, NULL);
	save(f, policy);
}

/* A contact soft-deleted after invoicing must not wedge the sweep. The event
 * used to copy the deleted row's id, the save refused it, the failure record
 * was refused the same way and every later sweep died on that invoice. */
static void test_deleted_contact(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) contact = venture_database_get(f->db, VENTURE_TYPE_CONTACT, f->contact, NULL);
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) events = NULL;
	g_autoptr(GPtrArray) actions = NULL;
	g_autofree gchar *to = NULL;
	(void)unused;
	issued_invoice(f, "INV-DUN-2");
	g_assert_true(venture_database_delete(f->db, contact, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpint(sweep(f, "2026-01-07"), ==, 2);
	events = rows(f, "dunning_event");
	g_assert_cmpuint(events->len, ==, 2);
	g_assert_cmpint(row_number(events, 0, "contact-id"), ==, 0);
	g_assert_cmpint(row_number(events, 0, "company-id"), ==, f->company);
	deliver(f, "2026-01-07");
	g_assert_cmpuint(venture_log_mailer_get_messages(f->mailer)->len, ==, 2);
	g_object_get(g_ptr_array_index((GPtrArray *)venture_log_mailer_get_messages(f->mailer), 0), "to", &to, NULL);
	g_assert_cmpstr(to, ==, "company@example.test");
	sweep(f, "2026-01-17"); deliver(f, "2026-01-17");
	g_assert_cmpint(sweep(f, "2026-01-24"), ==, 2);
	actions = rows(f, "activity");
	g_assert_cmpuint(actions->len, ==, 2);
	g_assert_cmpint(row_number(actions, 0, "contact-id"), ==, 0);
}

/* The pay link is the customer's bearer credential. It used to go to the
 * reminder's recipient whoever the access was issued to, and over plain
 * HTTP; it now needs the same mailbox and the portal's HTTPS rule. The base
 * URL is read live, so correcting it takes effect on the next reminder. */
static void test_pay_link_guard(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) bob = NULL, alice = NULL;
	g_autoptr(GPtrArray) mail = NULL;
	g_autofree gchar *first = NULL, *second = NULL, *third = NULL, *token = NULL;
	(void)unused;
	bob = venture_portal_service_invite(venture_portal_service_get(f->db), f->org, f->company, "bob@example.test", NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(sweep(f, "2026-01-07"), ==, 1);
	alice = venture_portal_service_invite(venture_portal_service_get(f->db), f->org, f->company, "ALICE@Example.test", NULL, &error);
	g_assert_no_error(error);
	g_object_get(alice, "token", &token, NULL);
	g_object_set(f->config, "server-base-url", "http://books.example.test", NULL);
	g_assert_cmpint(sweep(f, "2026-01-17"), ==, 1);
	g_object_set(f->config, "server-base-url", "https://books.example.test/", NULL);
	issued_invoice(f, "INV-DUN-2");
	g_assert_cmpint(sweep(f, "2026-01-18"), ==, 1);
	mail = rows(f, "mail_message");
	g_assert_cmpuint(mail->len, ==, 3);
	first = row_text(mail, 0, "private-text-body");
	second = row_text(mail, 1, "private-text-body");
	third = row_text(mail, 2, "private-text-body");
	g_assert_true(venture_string_is_empty(first));
	g_assert_true(venture_string_is_empty(second));
	g_assert_nonnull(third);
	g_assert_nonnull(strstr(third, token));
	g_assert_nonnull(strstr(third, "https://books.example.test/portal/"));
}

/* An HTML-only template's pay link lives in the private HTML body; the
 * transport used to send the private text and drop the HTML, so the link
 * vanished. */
static void test_private_html(Fixture *f, gconstpointer unused)
{
	g_autoptr(GPtrArray) templates = rows(f, "mail_template");
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) access = NULL;
	g_autoptr(GPtrArray) mail = NULL;
	g_autoptr(JsonNode) serialized = NULL;
	g_autofree gchar *token = NULL, *html = NULL, *private_html = NULL, *json = NULL, *sent_html = NULL;
	(void)unused;
	g_object_set(g_ptr_array_index(templates, 0), "text-body", "", "html-body", "<p>Pay {pay_link}</p>", NULL);
	save(f, g_ptr_array_index(templates, 0));
	access = venture_portal_service_invite(venture_portal_service_get(f->db), f->org, f->company, "alice@example.test", NULL, &error);
	g_assert_no_error(error);
	g_object_get(access, "token", &token, NULL);
	g_assert_cmpint(sweep(f, "2026-01-07"), ==, 1);
	mail = rows(f, "mail_message");
	html = row_text(mail, 0, "html-body");
	private_html = row_text(mail, 0, "private-html-body");
	g_assert_null(strstr(html, token));
	g_assert_nonnull(private_html);
	g_assert_nonnull(strstr(private_html, token));
	serialized = venture_serializable_to_json(VENTURE_SERIALIZABLE(g_ptr_array_index(mail, 0)), FALSE);
	json = venture_json_to_string(serialized, FALSE);
	g_assert_null(strstr(json, token));
	g_assert_null(strstr(json, "private_html_body"));
	deliver(f, "2026-01-07");
	g_object_get(g_ptr_array_index((GPtrArray *)venture_log_mailer_get_messages(f->mailer), 0), "private-html-body", &sent_html, NULL);
	g_assert_nonnull(strstr(sent_html, token));
}

/* A step is its offset. Prepending a step to a policy used to shift every
 * position, so the 7-day reminder already sent as step 2 went out again as
 * step 3. Rows an older install keyed by position must still count. */
static void test_policy_edit(Fixture *f, gconstpointer unused)
{
	static const gint prepended[] = { -5, -3, 7, 14 };
	g_autoptr(GError) error = NULL;
	g_autofree gchar *legacy = NULL;
	g_autoptr(GPtrArray) actions = NULL;
	(void)unused;
	g_assert_cmpint(sweep(f, "2026-01-07"), ==, 1); deliver(f, "2026-01-07");
	g_assert_cmpint(sweep(f, "2026-01-17"), ==, 1); deliver(f, "2026-01-17");
	legacy = g_strdup_printf("UPDATE dunning_events SET dunning_key = 'dunning:inv:%" G_GINT64_FORMAT ":policy:%" G_GINT64_FORMAT ":step:' || step",
		f->invoice, f->policy);
	g_assert_true(venture_database_execute(f->db, legacy, NULL, &error));
	g_assert_no_error(error);
	set_offsets(f, prepended, G_N_ELEMENTS(prepended), TRUE);
	g_assert_cmpint(sweep(f, "2026-01-20"), ==, 0);
	deliver(f, "2026-01-20");
	g_assert_cmpuint(venture_log_mailer_get_messages(f->mailer)->len, ==, 2);
	g_assert_cmpint(sweep(f, "2026-01-24"), ==, 1);
	actions = rows(f, "activity");
	g_assert_cmpuint(actions->len, ==, 1);
}

/* A queued reminder overtaken by a later step is cancelled at delivery. With
 * mail unconfigured or the relay down, step 1 and step 2 used to go out
 * together on recovery. */
static void test_queue_superseded(Fixture *f, gconstpointer unused)
{
	g_autoptr(JsonNode) answer = NULL;
	(void)unused;
	g_assert_cmpint(sweep(f, "2026-01-07"), ==, 1);
	g_assert_cmpint(sweep(f, "2026-01-17"), ==, 1);
	deliver(f, "2026-01-17");
	g_assert_cmpuint(venture_log_mailer_get_messages(f->mailer)->len, ==, 1);
	assert_event(f, 0, "cancelled", "superseded");
	assert_event(f, 1, "sent", NULL);
	/* The sweep says when nothing can deliver what it queues. */
	g_object_set(venture_database_get_mail_outbox(f->db), "mailer", NULL, NULL);
	answer = sweep_answer(f, "2026-01-18", FALSE);
	g_assert_nonnull(strstr(json_array_get_string_element(json_object_get_array_member(json_node_get_object(answer), "warnings"), 0),
		"No mail transport"));
}

/* A reminder whose balance moved before submission would state the wrong
 * amount; it is cancelled, and the next step renders the new balance. */
static void test_queue_balance(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) payment = record(f, "payment");
	g_autoptr(GError) error = NULL;
	(void)unused;
	g_assert_cmpint(sweep(f, "2026-01-07"), ==, 1);
	g_object_set(payment, "customer-id", f->company, "invoice-id", f->invoice, "method", "manual", NULL);
	field(payment, "amount", "10 USD");
	field(payment, "date", "2026-01-07");
	g_assert_true(venture_settlement_service_apply_payment(venture_settlement_service_get(f->db), VENTURE_PAYMENT(payment), NULL, NULL, &error));
	g_assert_no_error(error);
	deliver(f, "2026-01-08");
	g_assert_cmpuint(venture_log_mailer_get_messages(f->mailer)->len, ==, 0);
	assert_event(f, 0, "cancelled", "balance_changed");
}

/* A due date moved out after queueing makes the step not due yet. The
 * settlement service freezes an issued invoice's dates, so the move is made
 * underneath it, the way a lifecycle plugin or a data correction would. */
static void test_queue_due_moved(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *sql = NULL;
	(void)unused;
	g_assert_cmpint(sweep(f, "2026-01-07"), ==, 1);
	sql = g_strdup_printf("UPDATE invoices SET due_at = replace(due_at, '2026-01-10', '2026-02-10') WHERE id = %" G_GINT64_FORMAT, f->invoice);
	g_assert_true(venture_database_execute(f->db, sql, NULL, &error));
	g_assert_no_error(error);
	deliver(f, "2026-01-07");
	g_assert_cmpuint(venture_log_mailer_get_messages(f->mailer)->len, ==, 0);
	assert_event(f, 0, "cancelled", "no_longer_due");
}

/* A reminder queued more than a week before delivery resumes is stale. */
static void test_queue_stale(Fixture *f, gconstpointer unused)
{
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GDateTime) later = g_date_time_add_days(now, 8);
	g_autoptr(GDateTime) soon = g_date_time_add_days(now, 6);
	g_autoptr(GError) error = NULL;
	(void)unused;
	issued_invoice(f, "INV-DUN-2");
	g_assert_cmpint(sweep(f, "2026-01-07"), ==, 2);
	/* Six days after queueing is still fresh; eight is stale. */
	g_assert_cmpint(venture_mail_outbox_deliver_due(venture_database_get_mail_outbox(f->db), f->org, 1, soon, NULL, &error), ==, 1);
	g_assert_no_error(error);
	deliver_at(f, later);
	g_assert_cmpuint(venture_log_mailer_get_messages(f->mailer)->len, ==, 1);
	assert_event(f, 0, "sent", NULL);
	assert_event(f, 1, "cancelled", "stale");
}

/* A failed step is retried deliberately, under a new key, once. Failures
 * were final: a fixed template could never send the step it broke. */
static void test_retry(Fixture *f, gconstpointer unused)
{
	g_autoptr(GPtrArray) templates = rows(f, "mail_template");
	VentureActionRegistry *registry = venture_database_get_action_registry(f->db);
	g_autoptr(GHashTable) params = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)json_node_unref);
	g_autoptr(GPtrArray) events = NULL;
	g_autoptr(VentureEntity) retried = NULL;
	g_autoptr(VentureEntity) again = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *key = NULL, *status = NULL;
	gint64 failed_id;
	(void)unused;
	g_object_set(g_ptr_array_index(templates, 0), "text-body", "Hello {no_such_field}", NULL);
	save(f, g_ptr_array_index(templates, 0));
	g_assert_cmpint(sweep(f, "2026-01-07"), ==, 0);
	events = rows(f, "dunning_event");
	failed_id = venture_entity_get_id(g_ptr_array_index(events, 0));
	g_object_set(g_ptr_array_index(templates, 0), "text-body", "Hello {customer_name}", NULL);
	save(f, g_ptr_array_index(templates, 0));
	g_assert_nonnull(venture_action_registry_lookup(registry, "dunning_event", "retry"));
	retried = venture_action_registry_perform(registry, "dunning_event", failed_id, "retry", params, NULL, VENTURE_USER_ROLE_EDITOR, &error);
	g_assert_no_error(error);
	g_assert_nonnull(retried);
	g_object_get(retried, "dunning-key", &key, "delivery-status", &status, NULL);
	g_assert_true(g_str_has_suffix(key, ":retry:1"));
	g_assert_cmpstr(status, ==, "queued");
	deliver(f, "2026-01-07");
	g_assert_cmpuint(venture_log_mailer_get_messages(f->mailer)->len, ==, 1);
	again = venture_action_registry_perform(registry, "dunning_event", failed_id, "retry", params, NULL, VENTURE_USER_ROLE_EDITOR, &error);
	g_assert_null(again);
	g_assert_nonnull(error);
	g_clear_error(&error);
	/* A sent reminder is not a failure to retry. */
	g_assert_null(venture_action_registry_perform(registry, "dunning_event", venture_entity_get_id(retried), "retry", params,
		NULL, VENTURE_USER_ROLE_EDITOR, &error));
	g_assert_nonnull(error);
}

/*
 * A mail message names its reminder through ordinary related fields that
 * anybody who may write a message can fill in. Only the message the sweep
 * recorded on the event may move its status: a forged one used to flip a
 * sent reminder back to queued, and a forged failure made a retry -- a
 * second email to the customer -- legal.
 */
static void test_forged_message(Fixture *f, gconstpointer unused)
{
	g_autoptr(GPtrArray) events = NULL;
	g_autoptr(VentureEntity) forged = NULL;
	gint64 event_id;
	(void)unused;
	g_assert_cmpint(sweep(f, "2026-01-07"), ==, 1);
	deliver(f, "2026-01-07");
	assert_event(f, 0, "sent", NULL);
	events = rows(f, "dunning_event");
	event_id = venture_entity_get_id(g_ptr_array_index(events, 0));
	forged = record(f, "mail_message");
	g_object_set(forged, "to", "someone@elsewhere.test", "subject", "Not a reminder", "text-body", "Hello",
		"related-type", "dunning_event", "related-id", event_id, NULL);
	save(f, forged);
	assert_event(f, 0, "sent", NULL);
}

/* Adopting a policy over an old ledger used to supersede every step and
 * escalate at once: a collect task per overdue invoice and not one email.
 * Catch-up sends the latest email step, and the escalation keeps its spacing
 * from the adoption day. */
static void test_adoption(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) policy = venture_database_get(f->db, VENTURE_TYPE_DUNNING_POLICY, f->policy, NULL);
	g_autoptr(JsonNode) answer = NULL;
	g_autoptr(GPtrArray) actions = NULL;
	g_autoptr(GPtrArray) events = NULL;
	(void)unused;
	field(policy, "adopted-at", "2026-03-01");
	save(f, policy);
	answer = sweep_answer(f, "2026-03-01", FALSE);
	g_assert_cmpint(answer_int(answer, "queued"), ==, 1);
	g_assert_cmpint(answer_int(answer, "escalated"), ==, 0);
	g_assert_cmpint(answer_int(answer, "suppressed"), ==, 1);
	assert_event(f, 0, "suppressed", "before_adoption");
	events = rows(f, "dunning_event");
	g_assert_cmpuint(events->len, ==, 2);
	g_assert_cmpint(row_number(events, 1, "offset-days"), ==, 7);
	actions = rows(f, "activity");
	g_assert_cmpuint(actions->len, ==, 0);
	g_assert_cmpint(sweep(f, "2026-03-02"), ==, 0);
	g_assert_cmpint(sweep(f, "2026-03-07"), ==, 0);
	g_assert_cmpint(sweep(f, "2026-03-08"), ==, 1);
	g_clear_pointer(&actions, g_ptr_array_unref);
	actions = rows(f, "activity");
	g_assert_cmpuint(actions->len, ==, 1);
}

/* An escalation needs an owner: the invoice's, else the customer's owner,
 * else the policy's escalation owner. Invoices from before the owner field
 * escalated to nobody. */
static void test_escalation_owner(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) user = record(f, "user");
	g_autoptr(VentureEntity) company = venture_database_get(f->db, VENTURE_TYPE_COMPANY, f->company, NULL);
	g_autoptr(VentureEntity) invoice = venture_database_get(f->db, VENTURE_TYPE_INVOICE, f->invoice, NULL);
	g_autoptr(VentureEntity) policy = venture_database_get(f->db, VENTURE_TYPE_DUNNING_POLICY, f->policy, NULL);
	g_autoptr(VentureEntity) second = NULL;
	g_autoptr(GPtrArray) actions = NULL;
	g_autofree gchar *first_owner = NULL, *second_owner = NULL;
	gint64 other;
	(void)unused;
	g_object_set(user, "username", "carol", "password-hash", "x", NULL);
	save(f, user);
	g_object_set(company, "owner-user-id", venture_entity_get_id(user), NULL); save(f, company);
	g_object_set(invoice, "owner", "", NULL); save(f, invoice);
	g_object_set(policy, "escalation-owner", "dave", NULL); save(f, policy);
	sweep(f, "2026-01-07"); deliver(f, "2026-01-07");
	sweep(f, "2026-01-17"); deliver(f, "2026-01-17");
	g_assert_cmpint(sweep(f, "2026-01-24"), ==, 1);
	g_clear_object(&company);
	company = venture_database_get(f->db, VENTURE_TYPE_COMPANY, f->company, NULL);
	g_object_set(company, "owner-user-id", (gint64)0, NULL); save(f, company);
	other = issued_invoice(f, "INV-DUN-2");
	second = venture_database_get(f->db, VENTURE_TYPE_INVOICE, other, NULL);
	g_object_set(second, "owner", "", NULL); save(f, second);
	g_assert_cmpint(sweep(f, "2026-02-01"), ==, 1);
	actions = rows(f, "activity");
	g_assert_cmpuint(actions->len, ==, 2);
	first_owner = row_text(actions, 0, "owner");
	second_owner = row_text(actions, 1, "owner");
	g_assert_cmpstr(first_owner, ==, "carol");
	g_assert_cmpstr(second_owner, ==, "dave");
}

/* An opt-out stops the customer's email, not the internal collect task. It
 * used to suppress the escalation as well, so the invoice was forgotten. */
static void test_optout_escalates(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) company = venture_database_get(f->db, VENTURE_TYPE_COMPANY, f->company, NULL);
	g_autoptr(GPtrArray) actions = NULL;
	(void)unused;
	g_object_set(company, "dunning-opt-out", TRUE, NULL); save(f, company);
	g_assert_cmpint(sweep(f, "2026-01-07"), ==, 0);
	g_assert_cmpint(sweep(f, "2026-01-17"), ==, 0);
	g_assert_cmpint(sweep(f, "2026-01-24"), ==, 1);
	deliver(f, "2026-01-24");
	g_assert_cmpuint(venture_log_mailer_get_messages(f->mailer)->len, ==, 0);
	assert_event(f, 0, "suppressed", "company_opt_out");
	assert_event(f, 2, "escalated", NULL);
	actions = rows(f, "activity");
	g_assert_cmpuint(actions->len, ==, 1);
}

static gboolean refusing;
static gboolean refuse_mail_while(VentureDatabase *db, VentureEntity *entity, VentureEntity *previous, gpointer data, GError **error)
{
	(void)db; (void)entity; (void)previous; (void)data;
	if (!refusing)
		return TRUE;
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_DATABASE, "relay down");
	return FALSE;
}

/* When the newest due step fails, its rollback also undoes the older steps
 * it superseded. Those used to come due again the next day, so a 7-day
 * reminder went out after the 14-day one had been attempted. */
static void test_failed_latest(Fixture *f, gconstpointer unused)
{
	g_autoptr(JsonNode) answer = NULL;
	g_autoptr(GPtrArray) events = NULL;
	(void)unused;
	refusing = TRUE;
	venture_database_add_save_validator(f->db, VENTURE_TYPE_MAIL_MESSAGE, refuse_mail_while, NULL, NULL);
	g_test_expect_message("Venture", G_LOG_LEVEL_WARNING, "dunning: invoice #*failed and was skipped:*relay down*");
	answer = sweep_answer(f, "2026-01-20", FALSE);
	g_test_assert_expected_messages();
	refusing = FALSE;
	g_assert_cmpint(answer_int(answer, "failed"), ==, 1);
	events = rows(f, "dunning_event");
	g_assert_cmpuint(events->len, ==, 1);
	g_assert_cmpint(row_number(events, 0, "offset-days"), ==, 7);
	g_assert_cmpint(sweep(f, "2026-01-21"), ==, 0);
	deliver(f, "2026-01-21");
	g_assert_cmpuint(venture_log_mailer_get_messages(f->mailer)->len, ==, 0);
}

/* A promise to pay pauses reminders without recording anything, and a pause
 * set after queueing cancels at delivery. */
static void test_pause(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) invoice = venture_database_get(f->db, VENTURE_TYPE_INVOICE, f->invoice, NULL);
	g_autoptr(VentureEntity) company = NULL;
	g_autoptr(GPtrArray) events = NULL;
	(void)unused;
	field(invoice, "dunning-paused-until", "2026-01-12");
	g_object_set(invoice, "dunning-pause-reason", "promised by phone", NULL);
	save(f, invoice);
	g_assert_cmpint(sweep(f, "2026-01-08"), ==, 0);
	events = rows(f, "dunning_event");
	g_assert_cmpuint(events->len, ==, 0);
	g_assert_cmpint(sweep(f, "2026-01-12"), ==, 1);
	company = venture_database_get(f->db, VENTURE_TYPE_COMPANY, f->company, NULL);
	field(company, "dunning-paused-until", "2026-01-30");
	save(f, company);
	deliver(f, "2026-01-12");
	g_assert_cmpuint(venture_log_mailer_get_messages(f->mailer)->len, ==, 0);
	assert_event(f, 0, "cancelled", "paused");
}

/* The recurring module's collection case and dunning must not both chase
 * one invoice: a held or disputed case, or a promise still in the future,
 * keeps dunning away. */
static void test_collection_case(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) kase = record(f, "collection_case");
	g_autoptr(GPtrArray) events = NULL;
	(void)unused;
	g_object_set(kase, "invoice-id", f->invoice, NULL);
	field(kase, "status", "held");
	save(f, kase);
	g_assert_cmpint(sweep(f, "2026-01-17"), ==, 0);
	field(kase, "status", "open");
	field(kase, "promised-at", "2026-01-20");
	save(f, kase);
	g_assert_cmpint(sweep(f, "2026-01-17"), ==, 0);
	events = rows(f, "dunning_event");
	g_assert_cmpuint(events->len, ==, 0);
	g_assert_cmpint(sweep(f, "2026-01-20"), ==, 1);
}

/* A dry run is the plan and nothing else; a real sweep refuses a future
 * as_of, which used to fire every step due by then and burn the cadence,
 * and stamps queued-at with the real time. */
static void test_dry_run(Fixture *f, gconstpointer unused)
{
	g_autoptr(JsonNode) answer = sweep_answer(f, "2026-01-20", TRUE);
	g_autoptr(GPtrArray) events = NULL;
	g_autoptr(GPtrArray) mail = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GDateTime) future = g_date_time_add_days(now, 30);
	g_autoptr(GDateTime) queued_at = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) ahead = NULL;
	JsonArray *plan;
	JsonObject *send;
	(void)unused;
	plan = json_object_get_array_member(json_node_get_object(answer), "plan");
	g_assert_cmpuint(json_array_get_length(plan), ==, 2);
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(plan, 0), "outcome"), ==, "suppress");
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(plan, 0), "reason"), ==, "superseded");
	send = json_array_get_object_element(plan, 1);
	g_assert_cmpstr(json_object_get_string_member(send, "outcome"), ==, "send");
	g_assert_cmpint(json_object_get_int_member(send, "offset"), ==, 7);
	g_assert_cmpstr(json_object_get_string_member(send, "recipient"), ==, "alice@example.test");
	g_assert_cmpstr(json_object_get_string_member(send, "subject"), ==, "Invoice INV-DUN");
	g_assert_cmpstr(json_object_get_string_member(send, "invoice_number"), ==, "INV-DUN");
	g_assert_cmpint(answer_int(answer, "queued"), ==, 1);
	events = rows(f, "dunning_event"); g_assert_cmpuint(events->len, ==, 0);
	mail = rows(f, "mail_message"); g_assert_cmpuint(mail->len, ==, 0);
	ahead = venture_dunning_service_sweep_detailed(venture_dunning_service_get(f->db), f->org, future, 100, TRUE, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(ahead);
	g_assert_cmpint(venture_dunning_service_sweep(venture_dunning_service_get(f->db), f->org, future, 100, NULL, &error), ==, -1);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_pointer(&events, g_ptr_array_unref);
	events = rows(f, "dunning_event"); g_assert_cmpuint(events->len, ==, 0);
	g_assert_cmpint(sweep(f, "2026-01-07"), ==, 1);
	g_clear_pointer(&events, g_ptr_array_unref);
	events = rows(f, "dunning_event");
	g_object_get(g_ptr_array_index(events, 0), "queued-at", &queued_at, NULL);
	g_assert_cmpint(g_date_time_difference(queued_at, now), >=, -G_TIME_SPAN_HOUR);
	g_assert_cmpint(g_date_time_difference(queued_at, now), <=, G_TIME_SPAN_HOUR);
}

/* One row per overdue invoice, saying where it stands and what comes next. */
static void test_worklist(Fixture *f, gconstpointer unused)
{
	VentureReport *report = venture_report_registry_lookup(venture_context_get_report_registry(f->context), "dunning_worklist");
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(JsonNode) json = NULL;
	g_autoptr(GError) error = NULL;
	JsonObject *row;
	(void)unused;
	g_assert_nonnull(report);
	sweep(f, "2026-01-07"); deliver(f, "2026-01-07");
	result = venture_report_generate(report, f->context, NULL, NULL, &error);
	g_assert_no_error(error);
	json = venture_report_result_to_json(result);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(json), "rows")), ==, 1);
	row = json_array_get_object_element(json_object_get_array_member(json_node_get_object(json), "rows"), 0);
	g_assert_cmpstr(json_object_get_string_member(row, "invoice"), ==, "INV-DUN");
	g_assert_cmpstr(json_object_get_string_member(row, "customer"), ==, "Acme & Sons");
	g_assert_cmpstr(json_object_get_string_member(row, "aging"), ==, "90+");
	g_assert_cmpstr(json_object_get_string_member(row, "last_status"), ==, "sent");
	g_assert_cmpstr(json_object_get_string_member(row, "last_date"), ==, "2026-01-07");
	g_assert_cmpstr(json_object_get_string_member(row, "next"), ==, "step 2 (offset 7)");
	g_assert_cmpstr(json_object_get_string_member(row, "next_date"), ==, "2026-01-17");
	g_assert_cmpstr(json_object_get_string_member(row, "owner"), ==, "alice");
	venture_config_set_module_enabled(f->config, "dunning", FALSE);
	g_assert_null(venture_report_registry_lookup(venture_context_get_report_registry(f->context), "dunning_worklist"));
	venture_config_set_module_enabled(f->config, "dunning", TRUE);
}

/* The invoice timeline says what happens next, computed, never written. */
static void test_next_reminder(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) timeline = NULL;
	g_autoptr(GPtrArray) events = NULL;
	g_autofree gchar *json = NULL;
	(void)unused;
	sweep(f, "2026-01-07"); deliver(f, "2026-01-07");
	timeline = venture_desk_activity(f->context, "invoice", f->invoice, 0, &error);
	g_assert_no_error(error);
	json = venture_json_to_string(timeline, FALSE);
	g_assert_nonnull(strstr(json, "next reminder: step 2 on 2026-01-17"));
	events = rows(f, "dunning_event");
	g_assert_cmpuint(events->len, ==, 1);
}

/* A test send goes to the acting user only, writes no event and never
 * carries the customer's pay link. */
static void test_test_send(Fixture *f, gconstpointer unused)
{
	VentureDunningService *service = venture_dunning_service_get(f->db);
	VentureActionRegistry *registry = venture_database_get_action_registry(f->db);
	g_autoptr(VentureEntity) policy = venture_database_get(f->db, VENTURE_TYPE_DUNNING_POLICY, f->policy, NULL);
	g_autoptr(VentureEntity) user = record(f, "user");
	g_autoptr(VentureEntity) silent = record(f, "user");
	g_autoptr(VentureEntity) access = NULL;
	g_autoptr(VentureMailMessage) message = NULL;
	g_autoptr(VentureMailMessage) refused = NULL;
	g_autoptr(GPtrArray) events = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *to = NULL, *subject = NULL, *body = NULL, *token = NULL, *private_body = NULL;
	gboolean stageable = TRUE;
	VentureActor actor;
	(void)unused;
	actor.kind = VENTURE_ACTOR_KIND_USER;
	actor.name = "tester";
	actor.prompt = NULL;
	actor.request_id = NULL;
	actor.approved_by = NULL;
	refused = venture_dunning_service_test_send(service, policy, f->invoice, FALSE, 0, &actor, &error);
	g_assert_null(refused);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	g_object_set(user, "username", "tester", "email", "tester@example.test", "password-hash", "x", NULL);
	save(f, user);
	access = venture_portal_service_invite(venture_portal_service_get(f->db), f->org, f->company, "alice@example.test", NULL, &error);
	g_assert_no_error(error);
	g_object_get(access, "token", &token, NULL);
	message = venture_dunning_service_test_send(service, policy, f->invoice, FALSE, 0, &actor, &error);
	g_assert_no_error(error);
	g_assert_nonnull(message);
	g_object_get(message, "to", &to, "subject", &subject, "text-body", &body, "private-text-body", &private_body, NULL);
	g_assert_cmpstr(to, ==, "tester@example.test");
	g_assert_cmpstr(subject, ==, "[TEST] Invoice INV-DUN");
	g_assert_null(strstr(body, token));
	g_assert_nonnull(strstr(body, "[pay link withheld from test sends]"));
	g_assert_true(venture_string_is_empty(private_body));
	events = rows(f, "dunning_event");
	g_assert_cmpuint(events->len, ==, 0);
	g_clear_object(&refused);
	refused = venture_dunning_service_test_send(service, policy, f->invoice, TRUE, 14, &actor, &error);
	g_assert_null(refused);
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_object_set(silent, "username", "silent", "password-hash", "x", NULL);
	save(f, silent);
	actor.name = "silent";
	refused = venture_dunning_service_test_send(service, policy, f->invoice, TRUE, 7, &actor, &error);
	g_assert_null(refused);
	g_assert_nonnull(strstr(error->message, "no email"));
	g_object_get(venture_action_registry_lookup(registry, "dunning_policy", "test_send"), "stageable", &stageable, NULL);
	g_assert_false(stageable);
}

/* A policy naming invoice whose policy was deleted falls back to the next
 * level, with one warning, instead of silently leaving dunning. */
static void test_policy_fallback(Fixture *f, gconstpointer unused)
{
	gint64 gone = make_policy(f, "Gone", 1);
	g_autoptr(VentureEntity) invoice = venture_database_get(f->db, VENTURE_TYPE_INVOICE, f->invoice, NULL);
	g_autoptr(VentureEntity) second = NULL;
	g_autoptr(VentureEntity) policy = NULL;
	g_autoptr(GPtrArray) events = NULL;
	g_autoptr(GError) error = NULL;
	gint64 other;
	(void)unused;
	other = issued_invoice(f, "INV-DUN-2");
	second = venture_database_get(f->db, VENTURE_TYPE_INVOICE, other, NULL);
	g_object_set(invoice, "dunning-policy-id", gone, NULL); save(f, invoice);
	g_object_set(second, "dunning-policy-id", gone, NULL); save(f, second);
	policy = venture_database_get(f->db, VENTURE_TYPE_DUNNING_POLICY, gone, NULL);
	g_assert_true(venture_database_delete(f->db, policy, NULL, &error));
	g_assert_no_error(error);
	g_test_expect_message("Venture", G_LOG_LEVEL_WARNING, "dunning: invoice #*names dunning_policy #*deleted or missing*");
	g_assert_cmpint(sweep(f, "2026-01-07"), ==, 2);
	g_test_assert_expected_messages();
	events = rows(f, "dunning_event");
	g_assert_cmpuint(events->len, ==, 2);
	g_assert_cmpint(row_number(events, 0, "policy-id"), ==, f->policy);
	g_assert_cmpint(row_number(events, 1, "policy-id"), ==, f->policy);
}

/* Offsets are JSON integers within a year, at most twenty steps; "abc", null,
 * 7.9 and true used to be coerced into 0, 0, 7 and 1. Only an escalating last
 * step may omit its template. */
static void test_policy_validation(Fixture *f, gconstpointer unused)
{
	gint64 t = template_id(f);
	g_autoptr(VentureEntity) policy = record(f, "dunning_policy");
	g_autoptr(GError) error = NULL;
	g_autoptr(GString) many = g_string_new("[");
	const gchar *offsets[] = { "\"abc\"", "null", "7.9", "true", "366", "-366", "7.0" };
	guint i;
	(void)unused;
	g_object_set(policy, "name", "Strict", NULL);
	for (i = 0; i < G_N_ELEMENTS(offsets); i++)
	{
		g_autofree gchar *steps = g_strdup_printf("[{\"offset\":%s,\"template_id\":%" G_GINT64_FORMAT "}]", offsets[i], t);
		g_object_set(policy, "steps", steps, NULL);
		g_assert_false(venture_database_save(f->db, policy, NULL, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
		g_clear_error(&error);
	}
	{
		g_autofree gchar *steps = g_strdup_printf("[{\"offset\":7,\"template_id\":\"%" G_GINT64_FORMAT "\"}]", t);
		g_object_set(policy, "steps", steps, NULL);
		g_assert_false(venture_database_save(f->db, policy, NULL, &error));
		g_clear_error(&error);
	}
	for (i = 0; i < 21; i++)
		g_string_append_printf(many, "%s{\"offset\":%u,\"template_id\":%" G_GINT64_FORMAT "}", i ? "," : "", i, t);
	g_string_append(many, "]");
	g_object_set(policy, "steps", many->str, NULL);
	g_assert_false(venture_database_save(f->db, policy, NULL, &error));
	g_clear_error(&error);
	{
		g_autofree gchar *steps = g_strdup_printf("[{\"offset\":7,\"template_id\":%" G_GINT64_FORMAT "},{\"offset\":14}]", t);
		g_object_set(policy, "steps", steps, "final-escalation", FALSE, NULL);
		g_assert_false(venture_database_save(f->db, policy, NULL, &error));
		g_clear_error(&error);
		g_object_set(policy, "final-escalation", TRUE, "last-sweep", "{\"queued\":1}", NULL);
		g_assert_false(venture_database_save(f->db, policy, NULL, &error));
		g_clear_error(&error);
		g_object_set(policy, "last-sweep", NULL, NULL);
		save(f, policy);
	}
}

/* The report counts events queued in the period and attributes customers'
 * policies the way the sweep resolves them. */
static void test_report_period(Fixture *f, gconstpointer unused)
{
	VentureReport *report = venture_report_registry_lookup(venture_context_get_report_registry(f->context), "collections");
	g_autoptr(VentureEntity) company = venture_database_get(f->db, VENTURE_TYPE_COMPANY, f->company, NULL);
	g_autoptr(VentureDateRange) past = venture_date_range_new_month(2020, 1, NULL);
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GDateTime) start = g_date_time_add_days(now, -1);
	g_autoptr(GDateTime) end = g_date_time_add_days(now, 1);
	g_autoptr(VentureDateRange) current = venture_date_range_new(start, end);
	g_autoptr(GDateTime) paid = venture_time_from_string("2026-01-12", NULL);
	g_autoptr(GError) error = NULL;
	gint64 gentle;
	gint pass;
	(void)unused;
	sweep(f, "2026-01-07"); deliver(f, "2026-01-07");
	for (pass = 0; pass < 2; pass++)
	{
		g_autoptr(VentureReportResult) result = venture_report_generate(report, f->context, pass ? current : past, NULL, &error);
		g_autoptr(JsonNode) json = NULL;
		JsonObject *row;
		g_assert_no_error(error);
		json = venture_report_result_to_json(result);
		row = json_array_get_object_element(json_object_get_array_member(json_node_get_object(json), "rows"), 0);
		g_assert_cmpfloat(json_object_get_double_member(row, "reminders_sent"), ==, pass ? 1.0 : 0.0);
	}
	gentle = make_policy(f, "Gentle", 1);
	g_object_set(company, "dunning-policy-id", gentle, NULL); save(f, company);
	g_assert_true(venture_settlement_service_settle_invoice(venture_settlement_service_get(f->db), f->invoice, paid, NULL, &error));
	g_assert_no_error(error);
	{
		g_autoptr(VentureReportResult) result = venture_report_generate(report, f->context, NULL, NULL, &error);
		g_autoptr(JsonNode) json = venture_report_result_to_json(result);
		JsonArray *table = json_object_get_array_member(json_node_get_object(json), "rows");
		guint i;
		gboolean seen = FALSE;
		for (i = 0; i < json_array_get_length(table); i++)
		{
			JsonObject *row = json_array_get_object_element(table, i);
			gdouble days = json_object_get_double_member(row, "average_days_after") +
				json_object_get_double_member(row, "average_days_before");
			/* The invoice names no policy; its customer names Gentle, so the
			 * default policy must not claim it. */
			if (g_strcmp0(json_object_get_string_member(row, "policy"), "Gentle") == 0)
			{
				g_assert_cmpfloat(days, >, 0.0);
				seen = TRUE;
			}
			else
				g_assert_cmpfloat(days, ==, 0.0);
		}
		g_assert_true(seen);
	}
}

/* Switching the module off cancels what it queued rather than letting the
 * outbox send reminders nothing any longer governs, and reminder history
 * cannot be deleted out from under the idempotency it provides. */
static void test_module_off_queued(Fixture *f, gconstpointer unused)
{
	g_autoptr(GPtrArray) events = NULL;
	g_autoptr(GError) error = NULL;
	(void)unused;
	g_assert_cmpint(sweep(f, "2026-01-07"), ==, 1);
	events = rows(f, "dunning_event");
	g_assert_false(venture_database_delete(f->db, g_ptr_array_index(events, 0), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	g_assert_false(venture_database_purge(f->db, g_ptr_array_index(events, 0), NULL, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	venture_config_set_module_enabled(f->config, "dunning", FALSE);
	deliver(f, "2026-01-07");
	venture_config_set_module_enabled(f->config, "dunning", TRUE);
	g_assert_cmpuint(venture_log_mailer_get_messages(f->mailer)->len, ==, 0);
	assert_event(f, 0, "cancelled", "module_disabled");
}

static gint64 scalar(Fixture *f, const gchar *sql)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(OrmResult) result = venture_database_query_raw(f->db, sql, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(orm_result_next(result));
	return orm_row_get_integer(orm_result_get_row(result), 0);
}

/* 000330 and 000331 are recorded on a fresh database and see every column
 * they guard; a guard naming a table that does not exist would count zero
 * and pass forever. Run against a half-present group, the guard refuses. */
static void test_migration_columns(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureDatabase) partial = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *guard = NULL;
	(void)unused;
	g_assert_cmpint(scalar(f, "SELECT CAST(COUNT(*) AS BIGINT) FROM schema_migrations WHERE version IN (330, 331)"), ==, 2);
	g_assert_cmpint(scalar(f, "SELECT CAST(COUNT(*) AS BIGINT) FROM pragma_table_info('dunning_policies') WHERE name IN ('escalation_owner', 'last_sweep')"), ==, 2);
	g_assert_cmpint(scalar(f, "SELECT CAST(COUNT(*) AS BIGINT) FROM pragma_table_info('dunning_events') WHERE name LIKE 'rendered_balance_%'"), ==, 3);
	g_assert_cmpint(scalar(f, "SELECT CAST(COUNT(*) AS BIGINT) FROM pragma_table_info('invoices') WHERE name IN ('dunning_paused_until', 'dunning_pause_reason')"), ==, 2);
	g_assert_cmpint(scalar(f, "SELECT CAST(COUNT(*) AS BIGINT) FROM pragma_table_info('companies') WHERE name IN ('dunning_paused_until', 'dunning_pause_reason')"), ==, 2);
	g_assert_cmpint(scalar(f, "SELECT CAST(COUNT(*) AS BIGINT) FROM pragma_table_info('mail_messages') WHERE name = 'private_html_body'"), ==, 1);
	g_assert_true(g_file_get_contents("migrations/sqlite/000330_dunning_hardening.sql", &guard, NULL, &error));
	g_assert_no_error(error);
	partial = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_execute(partial, "CREATE TABLE dunning_policies (id INTEGER, escalation_owner TEXT)", NULL, &error));
	g_assert_no_error(error);
	g_assert_false(venture_database_execute(partial, guard, NULL, &error));
	g_assert_nonnull(error);
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
	g_test_add("/dunning/deleted-contact", Fixture, NULL, setup, test_deleted_contact, teardown);
	g_test_add("/dunning/pay-link-guard", Fixture, NULL, setup, test_pay_link_guard, teardown);
	g_test_add("/dunning/private-html", Fixture, NULL, setup, test_private_html, teardown);
	g_test_add("/dunning/policy-edit", Fixture, NULL, setup, test_policy_edit, teardown);
	g_test_add("/dunning/queue-superseded", Fixture, NULL, setup, test_queue_superseded, teardown);
	g_test_add("/dunning/queue-balance", Fixture, NULL, setup, test_queue_balance, teardown);
	g_test_add("/dunning/queue-due-moved", Fixture, NULL, setup, test_queue_due_moved, teardown);
	g_test_add("/dunning/queue-stale", Fixture, NULL, setup, test_queue_stale, teardown);
	g_test_add("/dunning/retry", Fixture, NULL, setup, test_retry, teardown);
	g_test_add("/dunning/adoption", Fixture, NULL, setup, test_adoption, teardown);
	g_test_add("/dunning/escalation-owner", Fixture, NULL, setup, test_escalation_owner, teardown);
	g_test_add("/dunning/optout-escalates", Fixture, NULL, setup, test_optout_escalates, teardown);
	g_test_add("/dunning/failed-latest", Fixture, NULL, setup, test_failed_latest, teardown);
	g_test_add("/dunning/forged-message", Fixture, NULL, setup, test_forged_message, teardown);
	g_test_add("/dunning/pause", Fixture, NULL, setup, test_pause, teardown);
	g_test_add("/dunning/collection-case", Fixture, NULL, setup, test_collection_case, teardown);
	g_test_add("/dunning/dry-run", Fixture, NULL, setup, test_dry_run, teardown);
	g_test_add("/dunning/worklist", Fixture, NULL, setup, test_worklist, teardown);
	g_test_add("/dunning/next-reminder", Fixture, NULL, setup, test_next_reminder, teardown);
	g_test_add("/dunning/test-send", Fixture, NULL, setup, test_test_send, teardown);
	g_test_add("/dunning/policy-fallback", Fixture, NULL, setup, test_policy_fallback, teardown);
	g_test_add("/dunning/policy-validation", Fixture, NULL, setup, test_policy_validation, teardown);
	g_test_add("/dunning/report-period", Fixture, NULL, setup, test_report_period, teardown);
	g_test_add("/dunning/module-off-queued", Fixture, NULL, setup, test_module_off_queued, teardown);
	g_test_add("/dunning/migration-columns", Fixture, NULL, setup, test_migration_columns, teardown);
	g_test_add_func("/dunning/migrate-disabled", test_migrate_disabled);
	g_test_add("/dunning/surfaces", ServerFixture, NULL, server_setup, test_surfaces, server_teardown);
	return g_test_run();
}
