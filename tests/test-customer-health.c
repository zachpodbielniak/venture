/*
 * test-customer-health.c - Customer health: the list behind the churn number
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Churn says how many customers left; health says which ones are about to.
 * Three companies stand in for the three bands: one touched last week, one
 * that has gone quiet, one that has gone quiet with an old overdue invoice
 * and a queue of tickets. Every threshold, the band it produces, the sweep
 * that turns a red company into somebody's next action, the churn card's
 * "at risk" line and the company page are checked against that fixture.
 * Every date is anchored on an explicit as_of so the numbers can be checked
 * by hand and do not drift with the calendar.
 */

#include <venture.h>

#include <libsoup/soup.h>
#include <string.h>

#include "venture-test-util.h"

#define AS_OF "2026-06-01"

typedef struct
{
	VentureDatabase *db;
	VentureConfig *config;
	VentureContext *context;
	VentureLogMailer *mailer;
	gint64 org;
	gint64 green;
	gint64 amber;
	gint64 red;
	gint64 red_invoice;
} Fixture;

static void
save(Fixture *f, VentureEntity *record)
{
	g_autoptr(GError) error = NULL;
	gboolean ok = venture_database_save(f->db, record, NULL, &error);
	if (!ok)
		g_error("save %s: %s", G_OBJECT_TYPE_NAME(record), error != NULL ? error->message : "(no error)");
}

static VentureEntity *
record_new(Fixture *f, const gchar *name)
{
	GType type = venture_entity_registry_lookup(venture_entity_registry_get_default(), name);
	VentureEntity *record;
	g_assert_cmpuint(type, !=, G_TYPE_INVALID);
	record = g_object_new(type, NULL);
	venture_entity_set_organization_id(record, f->org);
	return record;
}

static void
field(VentureEntity *record, const gchar *name, const gchar *value)
{
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_entity_set_field_from_string(record, name, value, &error));
	g_assert_no_error(error);
}

static gint64
user(Fixture *f, const gchar *username)
{
	g_autoptr(VentureUser) record = venture_user_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(record), f->org);
	g_object_set(record, "username", username, "role", VENTURE_USER_ROLE_EDITOR, "active", TRUE, NULL);
	g_assert_true(venture_user_set_password(record, "password-for-tests-1", 100000, NULL));
	save(f, VENTURE_ENTITY(record));
	return venture_entity_get_id(VENTURE_ENTITY(record));
}

static gint64
company(Fixture *f, const gchar *name, gint64 owner_user_id)
{
	g_autoptr(VentureEntity) record = record_new(f, "company");
	g_object_set(record, "name", name, "kind", VENTURE_COMPANY_KIND_CUSTOMER,
		"owner-user-id", owner_user_id, "email", "billing@example.test", NULL);
	save(f, record);
	return venture_entity_get_id(record);
}

static void
interaction(Fixture *f, gint64 company_id, const gchar *when)
{
	g_autoptr(VentureEntity) record = record_new(f, "interaction");
	g_object_set(record, "company-id", company_id, "subject", "Call", "kind", VENTURE_INTERACTION_KIND_CALL, NULL);
	field(record, "occurred-at", when);
	save(f, record);
}

/* A done activity is a touch; a planned one is a promise. Completion goes
 * through the service, so the completed-at stamp is the real one. */
static void
done_activity(Fixture *f, gint64 company_id, const gchar *when)
{
	g_autoptr(VentureEntity) record = record_new(f, "activity");
	g_autoptr(VentureEntity) done = NULL;
	g_autoptr(GError) error = NULL;
	g_object_set(record, "subject", "Visit", "kind", VENTURE_ACTIVITY_KIND_MEETING, "owner", "bob",
		"company-id", company_id, "related-type", "company", "related-id", company_id,
		"status", VENTURE_ACTIVITY_STATUS_PLANNED, NULL);
	field(record, "due-at", when);
	save(f, record);
	done = venture_activity_service_complete(venture_database_get_activity_service(f->db), record, "Met", NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(done);
	/* The service stamps now; the fixture wants a date it can reason
	 * about, so the stamp is moved through the interaction it recorded. */
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_INTERACTION);
		g_autoptr(GPtrArray) rows = NULL;
		guint i;
		venture_query_set_organization(query, f->org);
		g_assert_true(venture_query_add_filter_int(query, "company-id", VENTURE_FILTER_OP_EQ, company_id, &error));
		rows = venture_database_find(f->db, query, &error);
		g_assert_no_error(error);
		for (i = 0; i < rows->len; i++)
		{
			field(g_ptr_array_index(rows, i), "occurred-at", when);
			save(f, g_ptr_array_index(rows, i));
		}
	}
}

static void
inbound_mail(Fixture *f, gint64 company_id, const gchar *when)
{
	g_autoptr(VentureEntity) contact = record_new(f, "contact");
	g_autoptr(VentureEntity) mail = record_new(f, "mail_inbound");
	g_object_set(contact, "name", "Reader", "email", "reader@example.test", "company-id", company_id, NULL);
	save(f, contact);
	g_object_set(mail, "contact-id", venture_entity_get_id(contact), "subject", "Re: renewal",
		"uid-key", "acct:INBOX:1", "from-address", "reader@example.test", NULL);
	field(mail, "received-at", when);
	save(f, mail);
}

static gint64
deal(Fixture *f, gint64 company_id, VentureDealStage stage)
{
	g_autoptr(VentureEntity) record = record_new(f, "deal");
	g_object_set(record, "name", "Renewal", "company-id", company_id, "stage", stage, NULL);
	field(record, "value", "1000 USD");
	save(f, record);
	return venture_entity_get_id(record);
}

static void
ticket(Fixture *f, gint64 company_id, VentureTicketStatus status, gboolean breached)
{
	g_autoptr(VentureEntity) record = record_new(f, "ticket");
	g_object_set(record, "title", "Help", "company-id", company_id, "status", status, "sla-breached", breached, NULL);
	save(f, record);
}

/* An issued invoice, left open; the settlement service owns the status. */
static gint64
sent_invoice(Fixture *f, gint64 company_id, const gchar *number, const gchar *amount, const gchar *issued, const gchar *due)
{
	g_autoptr(VentureEntity) invoice = record_new(f, "invoice");
	g_autoptr(VentureEntity) line = record_new(f, "invoice_line");
	g_autoptr(GDateTime) at = venture_time_from_string(issued, NULL);
	g_autoptr(GError) error = NULL;
	g_object_set(invoice, "number", number, "company-id", company_id, "owner", "alice", NULL);
	field(invoice, "issued-at", issued);
	field(invoice, "due-at", due);
	save(f, invoice);
	g_object_set(line, "invoice-id", venture_entity_get_id(invoice), "description", "Work", "quantity", 1.0, NULL);
	field(line, "unit-price", amount);
	save(f, line);
	g_assert_true(venture_settlement_service_transition(venture_settlement_service_get(f->db),
		VENTURE_INVOICE(invoice), "sent", at, NULL, &error));
	g_assert_no_error(error);
	return venture_entity_get_id(invoice);
}

/* Settled in full by a receipt on @paid: what trailing revenue counts. */
static void
paid_invoice(Fixture *f, gint64 company_id, const gchar *number, const gchar *amount, const gchar *paid)
{
	g_autoptr(VentureEntity) payment = record_new(f, "payment");
	gint64 invoice_id = sent_invoice(f, company_id, number, amount, paid, paid);
	g_object_set(payment, "customer-id", company_id, "invoice-id", invoice_id, "method", "manual", NULL);
	field(payment, "amount", amount);
	field(payment, "date", paid);
	save(f, payment);
}

/* Two reminder steps, 7 and 14 days after due, so a dunning sweep on the
 * 20th of May has reached step 2 for an invoice due on the 1st. */
static void
dunning_history(Fixture *f, const gchar *sweep_at)
{
	g_autoptr(VentureEntity) template = record_new(f, "mail_template");
	g_autoptr(VentureEntity) policy = record_new(f, "dunning_policy");
	g_autoptr(GDateTime) at = venture_time_from_string(sweep_at, NULL);
	g_autofree gchar *steps = NULL;
	g_autoptr(GError) error = NULL;
	gint sent;
	g_object_set(template, "name", "Reminder", "subject", "Invoice {number}", "text-body", "{number} is due", NULL);
	save(f, template);
	steps = g_strdup_printf("[{\"offset\":7,\"template_id\":%" G_GINT64_FORMAT "},{\"offset\":14,\"template_id\":%" G_GINT64_FORMAT "}]",
		venture_entity_get_id(template), venture_entity_get_id(template));
	g_object_set(policy, "name", "Standard", "steps", steps, "is-default", TRUE, NULL);
	field(policy, "adopted-at", "2026-01-01");
	save(f, policy);
	sent = venture_dunning_service_sweep(venture_dunning_service_get(f->db), f->org, at, 100, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(sent, >=, 1);
}

static void
setup(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	gint64 alice;
	gint64 bob;
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
	alice = user(f, "alice");
	bob = user(f, "bob");

	/* Green: a reader wrote back last week. Nothing else is happening. */
	f->green = company(f, "Green Grocers", bob);
	inbound_mail(f, f->green, "2026-05-26");

	/* Amber: quiet for 61 days, but one open deal, an invoice only six
	 * days overdue and paid revenue inside and outside the window. */
	f->amber = company(f, "Amber Analytics", bob);
	done_activity(f, f->amber, "2026-04-01");
	deal(f, f->amber, VENTURE_DEAL_STAGE_PROPOSAL);
	deal(f, f->amber, VENTURE_DEAL_STAGE_WON);
	paid_invoice(f, f->amber, "A-1", "100 USD", "2025-12-01");
	paid_invoice(f, f->amber, "A-2", "50 USD", "2025-05-01");
	sent_invoice(f, f->amber, "A-3", "70 USD", "2026-05-01", "2026-05-26");

	/* Red: quiet for 92 days, an invoice 31 days overdue that the reminder
	 * policy has reached step 2 on, three open tickets with one breached. */
	f->red = company(f, "Red Robotics", alice);
	interaction(f, f->red, "2026-03-01");
	f->red_invoice = sent_invoice(f, f->red, "R-1", "400 USD", "2026-04-01", "2026-05-01");
	ticket(f, f->red, VENTURE_TICKET_STATUS_TODO, FALSE);
	ticket(f, f->red, VENTURE_TICKET_STATUS_IN_PROGRESS, TRUE);
	ticket(f, f->red, VENTURE_TICKET_STATUS_BLOCKED, FALSE);
	ticket(f, f->red, VENTURE_TICKET_STATUS_DONE, TRUE);
	dunning_history(f, "2026-05-20");

	/* A supplier is not a customer, whatever its invoices say. */
	{
		g_autoptr(VentureEntity) supplier = record_new(f, "company");
		g_object_set(supplier, "name", "Steel Supply", "kind", VENTURE_COMPANY_KIND_SUPPLIER, NULL);
		save(f, supplier);
	}
}

static void
teardown(Fixture *f, gconstpointer unused)
{
	(void)unused;
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
	g_clear_object(&f->mailer);
}

static GDateTime *
as_of(void)
{
	return venture_time_from_string(AS_OF, NULL);
}

static VentureReportResult *
run_report(Fixture *f, JsonObject *options)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDateRange) period = NULL;
	VentureReport *report = venture_report_registry_lookup(venture_context_get_report_registry(f->context), "customer_health");
	VentureReportResult *result;
	g_assert_nonnull(report);
	period = venture_context_parse_period(f->context, "2026-05", &error);
	g_assert_no_error(error);
	json_object_set_string_member(options, "as_of", AS_OF);
	result = venture_report_generate(report, f->context, period, options, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	return result;
}

static gint64
count_metric(VentureReportResult *result, const gchar *key)
{
	GPtrArray *metrics = venture_report_result_get_metrics(result);
	guint i;
	for (i = 0; i < metrics->len; i++)
	{
		VentureMetric *candidate = g_ptr_array_index(metrics, i);
		if (g_strcmp0(venture_metric_get_key(candidate), key) == 0)
			return (gint64)venture_metric_get_number(candidate);
	}
	g_error("no metric %s", key);
	return -1;
}

static gchar *
cell(VentureReportResult *result, guint row, const gchar *key)
{
	return venture_report_result_format_cell(result, row, key);
}

static guint
row_named(VentureReportResult *result, const gchar *name)
{
	guint i;
	for (i = 0; i < venture_report_result_get_row_count(result); i++)
	{
		g_autofree gchar *value = cell(result, i, "company");
		if (g_strcmp0(value, name) == 0)
			return i;
	}
	g_error("no row for %s", name);
	return 0;
}

static VentureCustomerHealth *
health_of(Fixture *f, gint64 company_id)
{
	g_autoptr(VentureEntity) record = venture_database_get(f->db, VENTURE_TYPE_COMPANY, company_id, NULL);
	g_autoptr(GDateTime) at = as_of();
	g_autoptr(GError) error = NULL;
	VentureCustomerHealth *health;
	g_assert_nonnull(record);
	health = venture_customer_health_for_company(f->context, record, at, &error);
	g_assert_no_error(error);
	g_assert_nonnull(health);
	return health;
}

static GPtrArray *
planned_check_ins(Fixture *f)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACTIVITY);
	g_autoptr(GError) error = NULL;
	GPtrArray *rows;
	venture_query_set_organization(query, f->org);
	venture_query_set_limit(query, 0);
	g_assert_true(venture_query_add_filter_string(query, "subject", VENTURE_FILTER_OP_EQ, "check in: Red Robotics", &error));
	g_assert_true(venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, &error));
	rows = venture_database_find(f->db, query, &error);
	g_assert_no_error(error);
	g_assert_nonnull(rows);
	return rows;
}

/* --- 1. Thresholds --------------------------------------------------------- */

/* The documented defaults are 30 / 15 / 3; a headline_setting row tunes
 * them per organisation and zero means the default, as the other headline
 * knobs already work. */
static void
test_thresholds(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) setting = NULL;
	VentureHealthThresholds thresholds;
	(void)unused;
	venture_customer_health_thresholds(f->context, f->org, &thresholds);
	g_assert_cmpint(thresholds.touch_days, ==, 30);
	g_assert_cmpint(thresholds.overdue_days, ==, 15);
	g_assert_cmpint(thresholds.open_tickets, ==, 3);

	setting = record_new(f, "headline_setting");
	g_object_set(setting, "health-touch-days", (gint64)100, "health-overdue-days", (gint64)45, NULL);
	save(f, setting);
	venture_customer_health_thresholds(f->context, f->org, &thresholds);
	g_assert_cmpint(thresholds.touch_days, ==, 100);
	g_assert_cmpint(thresholds.overdue_days, ==, 45);
	g_assert_cmpint(thresholds.open_tickets, ==, 3);

	/* Under the looser thresholds the red company has only its tickets
	 * against it and drops to amber: the band is a function of the
	 * settings, not a stored label. */
	{
		g_autoptr(VentureCustomerHealth) health = health_of(f, f->red);
		g_assert_cmpint(venture_customer_health_get_band(health), ==, VENTURE_HEALTH_BAND_AMBER);
	}
}

/* --- 2. Per-company health --------------------------------------------------- */

static void
test_company_health(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureCustomerHealth) green = health_of(f, f->green);
	g_autoptr(VentureCustomerHealth) amber = health_of(f, f->amber);
	g_autoptr(VentureCustomerHealth) red = health_of(f, f->red);
	GPtrArray *reasons;
	(void)unused;

	/* Inbound mail through a contact is a touch. */
	g_assert_cmpint(venture_customer_health_get_days_since_touch(green), ==, 6);
	g_assert_cmpint(venture_customer_health_get_band(green), ==, VENTURE_HEALTH_BAND_GREEN);
	g_assert_cmpstr(venture_customer_health_get_owner(green), ==, "bob");
	g_assert_null(venture_customer_health_get_revenue_12m(green));

	/* A completed activity is a touch; an overdue invoice inside the
	 * threshold is counted but is not a reason; the won deal is closed. */
	g_assert_cmpint(venture_customer_health_get_days_since_touch(amber), ==, 61);
	g_assert_cmpint(venture_customer_health_get_open_deals(amber), ==, 1);
	g_assert_cmpint(venture_customer_health_get_overdue_invoices(amber), ==, 1);
	g_assert_cmpint(venture_customer_health_get_overdue_days(amber), ==, 6);
	g_assert_cmpint(venture_customer_health_get_open_tickets(amber), ==, 0);
	g_assert_cmpint(venture_customer_health_get_dunning_step(amber), ==, 0);
	g_assert_nonnull(venture_customer_health_get_revenue_12m(amber));
	g_assert_cmpint(venture_money_get_amount(venture_customer_health_get_revenue_12m(amber)), ==, 10000);
	g_assert_cmpint(venture_customer_health_get_band(amber), ==, VENTURE_HEALTH_BAND_AMBER);
	g_assert_cmpint(venture_customer_health_get_flags(amber), ==, VENTURE_HEALTH_FLAG_QUIET);

	/* Every threshold tripped. */
	g_assert_cmpint(venture_customer_health_get_days_since_touch(red), ==, 92);
	g_assert_cmpint(venture_customer_health_get_overdue_invoices(red), ==, 1);
	g_assert_cmpint(venture_customer_health_get_overdue_days(red), ==, 31);
	g_assert_cmpint(venture_customer_health_get_open_tickets(red), ==, 3);
	g_assert_cmpint(venture_customer_health_get_sla_breaches(red), ==, 1);
	g_assert_cmpint(venture_customer_health_get_dunning_step(red), ==, 2);
	g_assert_cmpint(venture_customer_health_get_band(red), ==, VENTURE_HEALTH_BAND_RED);
	g_assert_cmpint(venture_customer_health_get_flags(red), ==,
		VENTURE_HEALTH_FLAG_QUIET | VENTURE_HEALTH_FLAG_OVERDUE | VENTURE_HEALTH_FLAG_TICKETS);
	g_assert_cmpstr(venture_customer_health_get_owner(red), ==, "alice");
	g_assert_cmpstr(venture_health_band_to_string(VENTURE_HEALTH_BAND_RED), ==, "red");

	/* Three reasons, always, each saying where the company stands
	 * against its threshold. */
	reasons = venture_customer_health_get_reasons(red);
	g_assert_cmpuint(reasons->len, ==, 3);
	g_assert_nonnull(strstr(g_ptr_array_index(reasons, 0), "92 days"));
	g_assert_nonnull(strstr(g_ptr_array_index(reasons, 1), "31 days"));
	g_assert_nonnull(strstr(g_ptr_array_index(reasons, 2), "3 open"));
	reasons = venture_customer_health_get_reasons(green);
	g_assert_cmpuint(reasons->len, ==, 3);
}

/* --- 3. The report ------------------------------------------------------------ */

static void
test_report(Fixture *f, gconstpointer unused)
{
	g_autoptr(JsonObject) options = json_object_new();
	g_autoptr(VentureReportResult) result = NULL;
	guint row;
	(void)unused;

	result = run_report(f, options);
	/* Three customers; the supplier is not one. Worst first by default. */
	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 3);
	g_assert_cmpint(count_metric(result, "red"), ==, 1);
	g_assert_cmpint(count_metric(result, "amber"), ==, 1);
	g_assert_cmpint(count_metric(result, "green"), ==, 1);
	g_assert_cmpint(count_metric(result, "customers"), ==, 3);
	{
		g_autofree gchar *first = cell(result, 0, "company");
		g_assert_cmpstr(first, ==, "Red Robotics");
	}
	row = row_named(result, "Red Robotics");
	{
		g_autofree gchar *band = cell(result, row, "band");
		g_autofree gchar *owner = cell(result, row, "owner");
		g_autofree gchar *touch = cell(result, row, "last_touch");
		g_autofree gchar *days = cell(result, row, "days_since_touch");
		g_autofree gchar *overdue = cell(result, row, "overdue_invoices");
		g_autofree gchar *overdue_days = cell(result, row, "overdue_days");
		g_autofree gchar *tickets = cell(result, row, "open_tickets");
		g_autofree gchar *breaches = cell(result, row, "sla_breaches");
		g_autofree gchar *step = cell(result, row, "dunning_step");
		g_autofree gchar *deals = cell(result, row, "open_deals");
		g_assert_cmpstr(band, ==, "red");
		g_assert_cmpstr(owner, ==, "alice");
		g_assert_cmpstr(touch, ==, "2026-03-01");
		g_assert_cmpstr(days, ==, "92");
		g_assert_cmpstr(overdue, ==, "1");
		g_assert_cmpstr(overdue_days, ==, "31");
		g_assert_cmpstr(tickets, ==, "3");
		g_assert_cmpstr(breaches, ==, "1");
		g_assert_cmpstr(step, ==, "2");
		g_assert_cmpstr(deals, ==, "0");
	}
	row = row_named(result, "Amber Analytics");
	{
		g_autofree gchar *revenue = cell(result, row, "revenue_12m");
		g_autofree gchar *deals = cell(result, row, "open_deals");
		g_assert_cmpstr(revenue, ==, "$100.00");
		g_assert_cmpstr(deals, ==, "1");
	}
	g_clear_object(&result);

	/* Filter by band. */
	json_object_set_string_member(options, "band", "red");
	result = run_report(f, options);
	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 1);
	{
		g_autofree gchar *name = cell(result, 0, "company");
		g_assert_cmpstr(name, ==, "Red Robotics");
	}
	/* The counts describe the whole organisation, not the filtered list,
	 * so the card and the filtered page agree. */
	g_assert_cmpint(count_metric(result, "customers"), ==, 3);
	g_clear_object(&result);

	/* Filter by owner. */
	json_object_remove_member(options, "band");
	json_object_set_string_member(options, "owner", "bob");
	result = run_report(f, options);
	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 2);
	g_clear_object(&result);

	/* Sort by any column, descending with a leading minus. */
	json_object_remove_member(options, "owner");
	json_object_set_string_member(options, "sort", "-revenue_12m");
	result = run_report(f, options);
	{
		g_autofree gchar *first = cell(result, 0, "company");
		g_assert_cmpstr(first, ==, "Amber Analytics");
	}
	g_clear_object(&result);
	json_object_set_string_member(options, "sort", "company");
	result = run_report(f, options);
	{
		g_autofree gchar *first = cell(result, 0, "company");
		g_assert_cmpstr(first, ==, "Amber Analytics");
	}
	g_clear_object(&result);

	/* A band or a column that does not exist is refused, not ignored. */
	{
		g_autoptr(JsonObject) bad = json_object_new();
		g_autoptr(VentureDateRange) period = venture_context_parse_period(f->context, "2026-05", NULL);
		g_autoptr(GError) error = NULL;
		VentureReport *report = venture_report_registry_lookup(venture_context_get_report_registry(f->context), "customer_health");
		json_object_set_string_member(bad, "band", "purple");
		g_assert_null(venture_report_generate(report, f->context, period, bad, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
		g_clear_error(&error);
		json_object_remove_member(bad, "band");
		json_object_set_string_member(bad, "sort", "nope");
		g_assert_null(venture_report_generate(report, f->context, period, bad, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	}
}

/* --- 4. The sweep --------------------------------------------------------------- */

/* A save validator standing in for a write that fails part-way through
 * the sweep: the check-in for one company is refused. */
static gboolean
refuse_rust_check_in(VentureDatabase *database, VentureEntity *entity, VentureEntity *previous, gpointer user_data, GError **error)
{
	g_autofree gchar *subject = NULL;
	(void)database;
	(void)previous;
	(void)user_data;
	g_object_get(entity, "subject", &subject, NULL);
	if (g_strcmp0(subject, "check in: Rust Rentals") == 0)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "refused for the test");
		return FALSE;
	}
	return TRUE;
}

static void
test_sweep(Fixture *f, gconstpointer unused)
{
	g_autoptr(GDateTime) at = as_of();
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	(void)unused;

	/* One red company, one check-in, for its account owner. */
	g_assert_cmpint(venture_customer_health_sweep(f->context, f->org, at, 0, NULL, &error), ==, 1);
	g_assert_no_error(error);
	rows = planned_check_ins(f);
	g_assert_cmpuint(rows->len, ==, 1);
	{
		VentureEntity *activity = g_ptr_array_index(rows, 0);
		g_autofree gchar *owner = NULL;
		g_autofree gchar *related = NULL;
		g_autofree gchar *body = NULL;
		gint64 related_id = 0;
		gint64 company_id = 0;
		gint kind = -1;
		gint status = -1;
		g_object_get(activity, "owner", &owner, "related-type", &related, "related-id", &related_id,
			"company-id", &company_id, "kind", &kind, "status", &status, "body", &body, NULL);
		g_assert_cmpstr(owner, ==, "alice");
		g_assert_cmpstr(related, ==, "company");
		g_assert_cmpint(related_id, ==, f->red);
		g_assert_cmpint(company_id, ==, f->red);
		g_assert_cmpint(kind, ==, VENTURE_ACTIVITY_KIND_FOLLOWUP);
		g_assert_cmpint(status, ==, VENTURE_ACTIVITY_STATUS_PLANNED);
		g_assert_nonnull(strstr(body, "92 days"));
	}
	g_clear_pointer(&rows, g_ptr_array_unref);

	/* Never a second while the first is open. */
	g_assert_cmpint(venture_customer_health_sweep(f->context, f->org, at, 0, NULL, &error), ==, 0);
	g_assert_no_error(error);
	rows = planned_check_ins(f);
	g_assert_cmpuint(rows->len, ==, 1);
	g_clear_pointer(&rows, g_ptr_array_unref);

	/* Once it is closed the next sweep may raise another. */
	{
		g_autoptr(GPtrArray) open = planned_check_ins(f);
		g_autoptr(VentureEntity) cancelled = venture_activity_service_act(
			venture_database_get_activity_service(f->db), g_ptr_array_index(open, 0), "cancel", NULL, NULL, &error);
		g_assert_no_error(error);
		g_assert_nonnull(cancelled);
	}
	g_assert_cmpint(venture_customer_health_sweep(f->context, f->org, at, 0, NULL, &error), ==, 1);
	g_assert_no_error(error);
	rows = planned_check_ins(f);
	g_assert_cmpuint(rows->len, ==, 2);
	g_clear_pointer(&rows, g_ptr_array_unref);

	/* Bounded: a limit of one visits one company, and the second red
	 * company waits for the next call. */
	{
		gint64 extra = company(f, "Rust Rentals", 0);
		interaction(f, extra, "2026-01-01");
		sent_invoice(f, extra, "X-1", "10 USD", "2026-01-01", "2026-02-01");
		ticket(f, extra, VENTURE_TICKET_STATUS_TODO, TRUE);
	}
	{
		g_autoptr(GPtrArray) before = planned_check_ins(f);
		g_autoptr(GPtrArray) after = NULL;
		g_autoptr(VentureEntity) cancelled = NULL;
		/* Close Red's again so two companies are due at once. */
		cancelled = venture_activity_service_act(venture_database_get_activity_service(f->db),
			g_ptr_array_index(before, 1), "cancel", NULL, NULL, &error);
		g_assert_no_error(error);
		g_assert_nonnull(cancelled);
		g_assert_cmpint(venture_customer_health_sweep(f->context, f->org, at, 1, NULL, &error), ==, 1);
		g_assert_no_error(error);
		g_assert_cmpint(venture_customer_health_sweep(f->context, f->org, at, 0, NULL, &error), ==, 1);
		g_assert_no_error(error);
		g_assert_cmpint(venture_customer_health_sweep(f->context, f->org, at, 0, NULL, &error), ==, 0);
		g_assert_no_error(error);
		after = planned_check_ins(f);
		g_assert_cmpuint(after->len, ==, 3);
	}

	/* All or nothing: a refused save part-way through the sweep leaves
	 * no check-in behind, so a rerun starts from a clean slate rather
	 * than from half a list. Both companies are red and open; the
	 * second is refused, so the first's write has to be rolled back. */
	{
		g_autoptr(GPtrArray) open = planned_check_ins(f);
		g_autoptr(VentureEntity) cancelled = NULL;
		g_autoptr(GPtrArray) rust = NULL;
		g_autoptr(GPtrArray) after = NULL;
		g_autoptr(GPtrArray) before = NULL;
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACTIVITY);
		cancelled = venture_activity_service_act(venture_database_get_activity_service(f->db),
			g_ptr_array_index(open, 2), "cancel", NULL, NULL, &error);
		g_assert_no_error(error);
		g_assert_nonnull(cancelled);
		venture_query_set_organization(query, f->org);
		g_assert_true(venture_query_add_filter_string(query, "subject", VENTURE_FILTER_OP_EQ, "check in: Rust Rentals", &error));
		rust = venture_database_find(f->db, query, &error);
		g_assert_no_error(error);
		g_assert_cmpuint(rust->len, ==, 1);
		cancelled = venture_activity_service_act(venture_database_get_activity_service(f->db),
			g_ptr_array_index(rust, 0), "cancel", NULL, NULL, &error);
		g_assert_no_error(error);
		g_assert_nonnull(cancelled);
		before = planned_check_ins(f);
		venture_database_add_save_validator(f->db, VENTURE_TYPE_ACTIVITY, refuse_rust_check_in, NULL, NULL);
		g_assert_cmpint(venture_customer_health_sweep(f->context, f->org, at, 0, NULL, &error), ==, -1);
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
		g_clear_error(&error);
		after = planned_check_ins(f);
		g_assert_cmpuint(after->len, ==, before->len);
	}

	/* An organisation is required. */
	g_assert_cmpint(venture_customer_health_sweep(f->context, 0, at, 0, NULL, &error), ==, -1);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);

	/* Without the activities module there is nowhere to put a next
	 * action, and the sweep says so rather than silently doing nothing. */
	venture_config_set_module_enabled(f->config, "activities", FALSE);
	g_assert_cmpint(venture_customer_health_sweep(f->context, f->org, at, 0, NULL, &error), ==, -1);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	venture_config_set_module_enabled(f->config, "activities", TRUE);

	/* And with the module itself off, the report is hidden and the sweep
	 * is refused. */
	venture_config_set_module_enabled(f->config, "customer_health", FALSE);
	g_assert_null(venture_report_registry_lookup(venture_context_get_report_registry(f->context), "customer_health"));
	g_assert_cmpint(venture_customer_health_sweep(f->context, f->org, at, 0, NULL, &error), ==, -1);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	venture_config_set_module_enabled(f->config, "customer_health", TRUE);
	g_assert_nonnull(venture_report_registry_lookup(venture_context_get_report_registry(f->context), "customer_health"));
}

/* --- 5. The churn card ------------------------------------------------------------ */

static JsonObject *
churn_line(JsonNode *cards, const gchar *label)
{
	JsonObject *card = json_array_get_object_element(json_node_get_array(cards), 2);
	JsonArray *lines;
	guint i;
	g_assert_cmpstr(json_object_get_string_member(card, "key"), ==, "churn");
	lines = json_object_get_array_member(card, "lines");
	for (i = 0; i < json_array_get_length(lines); i++)
	{
		JsonObject *line = json_array_get_object_element(lines, i);
		if (g_strcmp0(json_object_get_string_member(line, "label"), label) == 0)
			return line;
	}
	return NULL;
}

static void
test_churn_card(Fixture *f, gconstpointer unused)
{
	g_autoptr(JsonNode) cards = NULL;
	g_autoptr(VentureDateRange) period = NULL;
	g_autoptr(GError) error = NULL;
	JsonObject *line;
	(void)unused;

	period = venture_context_parse_period(f->context, "2026-05", &error);
	g_assert_no_error(error);
	cards = venture_headline_home_cards(f->context, f->org, period, &error);
	g_assert_no_error(error);
	g_assert_nonnull(cards);
	g_assert_cmpuint(json_array_get_length(json_node_get_array(cards)), ==, 5);

	/* The existing lines stay; "at risk" is added with a link to the
	 * report filtered to red, as of the period's end. */
	g_assert_nonnull(churn_line(cards, "Recurring churn"));
	g_assert_nonnull(churn_line(cards, "Gone quiet"));
	line = churn_line(cards, "At risk");
	g_assert_nonnull(line);
	g_assert_cmpstr(json_object_get_string_member(line, "value"), ==, "1");
	g_assert_true(g_str_has_prefix(json_object_get_string_member(line, "link"), "/reports/customer_health?"));
	g_assert_nonnull(strstr(json_object_get_string_member(line, "link"), "band=red"));
	g_assert_nonnull(strstr(json_object_get_string_member(line, "link"), "period=2026-05"));

	/* With the module off the line is gone and the card is as it was. */
	g_clear_pointer(&cards, json_node_unref);
	venture_config_set_module_enabled(f->config, "customer_health", FALSE);
	cards = venture_headline_home_cards(f->context, f->org, period, &error);
	g_assert_no_error(error);
	g_assert_null(churn_line(cards, "At risk"));
	g_assert_nonnull(churn_line(cards, "Gone quiet"));
	venture_config_set_module_enabled(f->config, "customer_health", TRUE);
}

/* --- 6. Over HTTP and the CLI ------------------------------------------------------- */

typedef struct
{
	gboolean done;
	GError *error;
	GBytes *bytes;
	gchar *out;
	gchar *err;
} Result;

typedef struct
{
	Fixture base;
	VentureWebServer *server;
	gchar *directory;
} ServerFixture;

static void
server_setup(ServerFixture *s, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GSocketListener) listener = g_socket_listener_new();
	guint16 port = g_socket_listener_add_any_inet_port(listener, NULL, &error);
	g_assert_no_error(error);
	g_socket_listener_close(listener);
	s->directory = g_dir_make_tmp("venture-health-XXXXXX", &error);
	g_assert_no_error(error);
	setup(&s->base, unused);
	g_object_set(s->base.config, "state-dir", s->directory, "server-bind-address", "127.0.0.1",
		"server-port", (gint64)port, "security-require-auth", FALSE, NULL);
	s->server = venture_web_server_new(s->base.context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(s->server, &error));
	g_assert_no_error(error);
}

static void
server_teardown(ServerFixture *s, gconstpointer unused)
{
	venture_web_server_stop(s->server);
	g_clear_object(&s->server);
	teardown(&s->base, unused);
	venture_test_remove_tree(s->directory);
	g_free(s->directory);
}

static void
http_done(GObject *source, GAsyncResult *result, gpointer data)
{
	Result *r = data;
	r->bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &r->error);
	r->done = TRUE;
}

static guint
request(ServerFixture *s, const gchar *path, const gchar *body, gchar **out)
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
cli(ServerFixture *s, const gchar *const *args, gboolean expect_success)
{
	g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
	g_autoptr(GSubprocess) process = NULL;
	g_autoptr(GPtrArray) argv = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GError) error = NULL;
	Result result;
	guint i;
	guint timeout;
	memset(&result, 0, sizeof(result));
	g_ptr_array_add(argv, g_canonicalize_filename("build/debug/venturectl", NULL));
	g_ptr_array_add(argv, g_strdup("--server"));
	g_ptr_array_add(argv, g_strdup(venture_web_server_get_base_url(s->server)));
	g_ptr_array_add(argv, g_strdup("-f"));
	g_ptr_array_add(argv, g_strdup("json"));
	for (i = 0; args[i]; i++)
		g_ptr_array_add(argv, g_strdup(args[i]));
	g_ptr_array_add(argv, NULL);
	g_subprocess_launcher_setenv(launcher, "VENTURE_TOKEN", "health-fixture", TRUE);
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
test_surfaces(ServerFixture *s, gconstpointer unused)
{
	Fixture *f = &s->base;
	const gchar *sweep_args[] = { "customers", "health-sweep", "as_of=" AS_OF, NULL };
	const gchar *bad_args[] = { "customers", "nope", NULL };
	g_autofree gchar *body = NULL;
	g_autofree gchar *out = NULL;
	g_autofree gchar *bad = NULL;
	g_autofree gchar *page = NULL;
	g_autofree gchar *report = NULL;
	g_autofree gchar *page_path = g_strdup_printf("/e/company/%" G_GINT64_FORMAT, f->red);
	g_autoptr(GPtrArray) rows = NULL;
	(void)unused;

	/* REST: the sweep, bounded, reporting what it made. */
	g_assert_cmpuint(request(s, "/api/v1/customers/health/sweep?as_of=" AS_OF, "{}", &body), ==, 200);
	{
		g_autoptr(JsonNode) node = venture_json_parse(body, NULL);
		g_assert_nonnull(node);
		g_assert_cmpint(json_object_get_int_member(json_node_get_object(node), "created"), ==, 1);
	}
	rows = planned_check_ins(f);
	g_assert_cmpuint(rows->len, ==, 1);
	g_clear_pointer(&rows, g_ptr_array_unref);

	/* CLI: the same service; nothing new while the first is open. */
	out = cli(s, sweep_args, TRUE);
	{
		g_autoptr(JsonNode) node = venture_json_parse(out, NULL);
		g_assert_nonnull(node);
		g_assert_cmpint(json_object_get_int_member(json_node_get_object(node), "created"), ==, 0);
	}
	rows = planned_check_ins(f);
	g_assert_cmpuint(rows->len, ==, 1);
	bad = cli(s, bad_args, FALSE);
	g_assert_cmpuint(request(s, "/api/v1/customers/health/sweep?as_of=not-a-date", "{}", NULL), >=, 400);

	/* The company page: the band and its three reasons. */
	g_assert_cmpuint(request(s, page_path, NULL, &page), ==, 200);
	g_assert_nonnull(strstr(page, "health-band red"));
	g_assert_nonnull(strstr(page, "overdue"));
	g_assert_nonnull(strstr(page, "open ticket"));
	g_assert_nonnull(strstr(page, "Last touch"));

	/* The report page, filtered to red, lists only the red company. */
	g_assert_cmpuint(request(s, "/reports/customer_health?period=2026-05&band=red&as_of=" AS_OF, NULL, &report), ==, 200);
	g_assert_nonnull(strstr(report, "Red Robotics"));
	g_assert_null(strstr(report, "Amber Analytics"));
	g_clear_pointer(&report, g_free);
	g_assert_cmpuint(request(s, "/api/v1/reports/customer_health?period=2026-05&sort=-revenue_12m&as_of=" AS_OF, NULL, &report), ==, 200);
	g_assert_nonnull(strstr(report, "Amber Analytics"));

	/* Switched off, the route is gone. */
	venture_config_set_module_enabled(f->config, "customer_health", FALSE);
	g_assert_cmpuint(request(s, "/api/v1/customers/health/sweep", "{}", NULL), ==, 404);
	venture_config_set_module_enabled(f->config, "customer_health", TRUE);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	venture_entity_registry_register_builtins(venture_entity_registry_get_default());
	g_test_add("/customer_health/thresholds", Fixture, NULL, setup, test_thresholds, teardown);
	g_test_add("/customer_health/company", Fixture, NULL, setup, test_company_health, teardown);
	g_test_add("/customer_health/report", Fixture, NULL, setup, test_report, teardown);
	g_test_add("/customer_health/sweep", Fixture, NULL, setup, test_sweep, teardown);
	g_test_add("/customer_health/churn_card", Fixture, NULL, setup, test_churn_card, teardown);
	g_test_add("/customer_health/surfaces", ServerFixture, NULL, server_setup, test_surfaces, server_teardown);
	return g_test_run();
}
