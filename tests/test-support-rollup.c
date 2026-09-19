/*
 * test-support-rollup.c - Support cost and ticket volume per customer
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * One worked example -- two customers, a ticket nobody filed under a
 * company, and one from the month before -- that every figure of the
 * rollup is checked against by hand. Then the rates and their defaults,
 * the options and every refusal, the product breakdown, the one function
 * the home card and the company page both read, the role and module
 * gates, the CSV export and the CLI verb.
 */

#include <venture.h>

#include <libsoup/soup.h>
#include <string.h>
#include <unistd.h>

#include "venture-test-util.h"

typedef struct
{
	VentureDatabase *db;
	VentureConfig *config;
	VentureContext *context;
	gint64 org;
} Fixture;

static void
setup(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	f->config = venture_config_new();
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
}

static void
teardown(Fixture *f, gconstpointer unused)
{
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
}

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

static gint64
company(Fixture *f, const gchar *name)
{
	g_autoptr(VentureEntity) record = record_new(f, "company");
	g_object_set(record, "name", name, NULL);
	save(f, record);
	return venture_entity_get_id(record);
}

/* @at plus @minutes, as a datetime. */
static GDateTime *
later(const gchar *at, gint minutes)
{
	g_autoptr(GDateTime) base = venture_time_from_string(at, NULL);
	g_assert_nonnull(base);
	return g_date_time_add_minutes(base, minutes);
}

/*
 * A ticket raised at @created for @company_id (0 for none). @responded and
 * @resolved are minutes after it was raised, or -1 for never. The created
 * date is set before the first save, which keeps it.
 */
static gint64
ticket(Fixture *f, gint64 company_id, const gchar *created, VentureTicketStatus status,
       gint responded, gint resolved, gboolean breached, VentureSatisfaction satisfaction)
{
	g_autoptr(VentureEntity) record = record_new(f, "ticket");
	g_autoptr(GDateTime) raised = venture_time_from_string(created, NULL);
	g_assert_nonnull(raised);
	g_object_set(record, "title", "Help", "status", status, "kind", VENTURE_TICKET_KIND_EXTERNAL,
		"sla-breached", breached, "satisfaction", satisfaction, "created-at", raised, NULL);
	if (company_id > 0)
		g_object_set(record, "company-id", company_id, NULL);
	if (responded >= 0)
	{
		g_autoptr(GDateTime) at = later(created, responded);
		g_object_set(record, "first-responded-at", at, NULL);
	}
	if (resolved >= 0)
	{
		g_autoptr(GDateTime) at = later(created, resolved);
		g_object_set(record, "resolved-at", at, NULL);
	}
	save(f, record);
	return venture_entity_get_id(record);
}

/* Hours logged through the desk service, so the ticket's logged-hours
 * follows and the rows are what the desk itself writes. */
static void
log_hours(Fixture *f, gint64 ticket_id, gdouble hours)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) worklog = venture_desk_log_work(f->context, ticket_id, hours, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(worklog);
}

/* The organisation's headline settings row, created or updated. */
static void
rates(Fixture *f, gint64 hourly, gint64 per_ticket, const gchar *worklog_rate)
{
	g_autoptr(VentureEntity) setting = venture_headline_setting_find(f->db, f->org);
	g_autoptr(GError) error = NULL;
	if (setting == NULL)
		setting = record_new(f, "headline_setting");
	g_object_set(setting, "support-hourly-rate", hourly, "support-ticket-rate", per_ticket, NULL);
	if (worklog_rate != NULL)
	{
		g_assert_true(venture_entity_set_field_from_string(setting, "hourly-rate", worklog_rate, &error));
		g_assert_no_error(error);
	}
	save(f, setting);
}

static VentureReportResult *
run(Fixture *f, const gchar *period_text, JsonObject *options, GError **error)
{
	g_autoptr(VentureDateRange) period = NULL;
	VentureReport *report = venture_report_registry_lookup(venture_context_get_report_registry(f->context), "support_rollup");
	g_assert_nonnull(report);
	period = venture_context_parse_period(f->context, period_text, error);
	g_assert_nonnull(period);
	return venture_report_generate(report, f->context, period, options, error);
}

static VentureReportResult *
run_ok(Fixture *f, const gchar *period_text, JsonObject *options)
{
	g_autoptr(GError) error = NULL;
	VentureReportResult *result = run(f, period_text, options, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	return result;
}

/* Options as "key=value" pairs, strings unless the value parses as an
 * integer -- what the query string and the CLI hand a report. */
static JsonObject *
options(const gchar *first, ...)
{
	JsonObject *object = json_object_new();
	const gchar *pair = first;
	va_list ap;
	va_start(ap, first);
	while (pair != NULL)
	{
		g_auto(GStrv) parts = g_strsplit(pair, "=", 2);
		gchar *end = NULL;
		gint64 number = g_ascii_strtoll(parts[1], &end, 10);
		if (end != NULL && *end == '\0' && parts[1][0] != '\0')
			json_object_set_int_member(object, parts[0], number);
		else
			json_object_set_string_member(object, parts[0], parts[1]);
		pair = va_arg(ap, const gchar *);
	}
	va_end(ap);
	return object;
}

static VentureMetric *
metric(VentureReportResult *result, const gchar *key)
{
	GPtrArray *metrics = venture_report_result_get_metrics(result);
	guint i;
	for (i = 0; i < metrics->len; i++)
	{
		VentureMetric *candidate = g_ptr_array_index(metrics, i);
		if (g_strcmp0(venture_metric_get_key(candidate), key) == 0)
			return candidate;
	}
	g_error("no metric %s", key);
	return NULL;
}

static gint64
money_metric(VentureReportResult *result, const gchar *key)
{
	const VentureMoney *amount = venture_metric_get_money(metric(result, key));
	g_assert_nonnull(amount);
	return venture_money_get_amount(amount);
}

static gint64
count_metric(VentureReportResult *result, const gchar *key)
{
	return (gint64)venture_metric_get_number(metric(result, key));
}

static gchar *
cell(VentureReportResult *result, guint row, const gchar *key)
{
	return venture_report_result_format_cell(result, row, key);
}

/* The row whose first column reads @name, or G_MAXUINT. */
static guint
row_named(VentureReportResult *result, const gchar *column, const gchar *name)
{
	guint i;
	for (i = 0; i < venture_report_result_get_row_count(result); i++)
	{
		g_autofree gchar *text = cell(result, i, column);
		if (g_strcmp0(text, name) == 0)
			return i;
	}
	return G_MAXUINT;
}

static void
assert_cell(VentureReportResult *result, guint row, const gchar *key, const gchar *expected)
{
	g_autofree gchar *text = cell(result, row, key);
	g_assert_cmpstr(text, ==, expected);
}

static const gchar *
note(VentureReportResult *result)
{
	g_autoptr(JsonNode) node = venture_report_result_to_json(result);
	JsonObject *object = json_node_get_object(node);
	const gchar *text = venture_json_object_get_string(object, "note", "");
	/* The node owns the string; the report owns the same text. */
	return g_intern_string(text);
}

/* The worked example: August 2026. */
typedef struct { gint64 acme, bolt, t1, t2, t3, t4, t5; } Desk;

static void
desk(Fixture *f, Desk *d)
{
	d->acme = company(f, "Acme");
	d->bolt = company(f, "Bolt");
	/* Acme: answered in 30 minutes, resolved in two hours, rated good,
	 * an hour and a half logged; and one still open past its target. */
	d->t1 = ticket(f, d->acme, "2026-08-03", VENTURE_TICKET_STATUS_DONE, 30, 120, FALSE, VENTURE_SATISFACTION_GOOD);
	d->t2 = ticket(f, d->acme, "2026-08-10", VENTURE_TICKET_STATUS_TODO, -1, -1, TRUE, VENTURE_SATISFACTION_UNRATED);
	log_hours(f, d->t1, 1.5);
	/* Bolt: one ticket, answered in an hour, resolved in three, rated bad,
	 * no time logged. */
	d->t3 = ticket(f, d->bolt, "2026-08-15", VENTURE_TICKET_STATUS_DONE, 60, 180, FALSE, VENTURE_SATISFACTION_BAD);
	/* Nobody's ticket, still open. */
	d->t4 = ticket(f, 0, "2026-08-20", VENTURE_TICKET_STATUS_IN_PROGRESS, -1, -1, FALSE, VENTURE_SATISFACTION_UNRATED);
	/* July's ticket, with time logged in August: neither counts. */
	d->t5 = ticket(f, d->acme, "2026-07-31", VENTURE_TICKET_STATUS_DONE, 5, 10, FALSE, VENTURE_SATISFACTION_GOOD);
	log_hours(f, d->t5, 4.0);
}

/*
 * Every figure of the worked example by hand. Acme's minutes at 60 an
 * hour are 90.00; its ticket without minutes costs the flat 25; Bolt's
 * and the unfiled ticket cost 25 each. The total is the sum of the rows.
 */
static void
test_rows(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureReportResult) result = NULL;
	g_autofree gchar *csv = NULL;
	Desk d;
	guint acme, bolt, none;

	desk(f, &d);
	rates(f, 60, 25, NULL);
	result = run_ok(f, "2026-08", NULL);

	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 3);
	acme = row_named(result, "company", "Acme");
	bolt = row_named(result, "company", "Bolt");
	none = row_named(result, "company", "(no company)");
	g_assert_cmpuint(acme, !=, G_MAXUINT);
	g_assert_cmpuint(bolt, !=, G_MAXUINT);
	g_assert_cmpuint(none, !=, G_MAXUINT);
	/* The default order: the customer costing the most first. */
	g_assert_cmpuint(acme, ==, 0);

	assert_cell(result, acme, "tickets", "2");
	assert_cell(result, acme, "closed", "1");
	assert_cell(result, acme, "open", "1");
	assert_cell(result, acme, "breaches", "1");
	assert_cell(result, acme, "response", "30");
	assert_cell(result, acme, "resolution", "120");
	assert_cell(result, acme, "csat", "100.0%");
	assert_cell(result, acme, "minutes", "90");
	assert_cell(result, acme, "support_cost", "$115.00");

	assert_cell(result, bolt, "tickets", "1");
	assert_cell(result, bolt, "closed", "1");
	assert_cell(result, bolt, "open", "0");
	assert_cell(result, bolt, "breaches", "0");
	assert_cell(result, bolt, "response", "60");
	assert_cell(result, bolt, "resolution", "180");
	assert_cell(result, bolt, "csat", "0.0%");
	assert_cell(result, bolt, "minutes", "0");
	assert_cell(result, bolt, "support_cost", "$25.00");

	assert_cell(result, none, "tickets", "1");
	assert_cell(result, none, "open", "1");
	assert_cell(result, none, "response", "");
	assert_cell(result, none, "csat", "");
	assert_cell(result, none, "support_cost", "$25.00");

	g_assert_cmpint(count_metric(result, "companies"), ==, 3);
	g_assert_cmpint(count_metric(result, "tickets"), ==, 4);
	g_assert_cmpint(count_metric(result, "closed"), ==, 2);
	g_assert_cmpint(count_metric(result, "open"), ==, 2);
	g_assert_cmpint(count_metric(result, "breaches"), ==, 1);
	/* Medians over the whole desk: reply {30, 60}, resolve {120, 180}. */
	g_assert_cmpint(count_metric(result, "response"), ==, 45);
	g_assert_cmpint(count_metric(result, "resolution"), ==, 150);
	g_assert_cmpfloat(venture_metric_get_number(metric(result, "csat")), ==, 0.5);
	g_assert_cmpint(count_metric(result, "minutes"), ==, 90);
	g_assert_cmpint(money_metric(result, "support_cost"), ==, 16500);
	g_assert_nonnull(strstr(note(result), "1 tickets name no company"));

	/* The CSV export renders the same rows. */
	csv = venture_report_result_render(result, VENTURE_OUTPUT_FORMAT_CSV);
	g_assert_nonnull(strstr(csv, "Acme"));
	g_assert_nonnull(strstr(csv, "115.00"));

	/* July has its own ticket, and August's minutes do not leak into it. */
	{
		g_autoptr(VentureReportResult) july = run_ok(f, "2026-07", NULL);
		g_assert_cmpuint(venture_report_result_get_row_count(july), ==, 1);
		assert_cell(july, 0, "minutes", "240");
		g_assert_cmpint(money_metric(july, "support_cost"), ==, 24000);
	}
}

/*
 * The rates and their defaults. No rate: n/a, never a zero that looks like
 * free support. A zero support_hourly_rate uses the worklog hourly_rate the
 * gross margin already prices hours at; a rate in another currency is left
 * out and said so; and each rate on its own prices only what it covers.
 */
static void
test_rates(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureReportResult) unpriced = NULL;
	g_autoptr(VentureReportResult) worklog_rate = NULL;
	g_autoptr(VentureReportResult) overridden = NULL;
	g_autoptr(VentureReportResult) foreign = NULL;
	g_autoptr(VentureReportResult) flat_only = NULL;
	g_autoptr(VentureReportResult) rounded = NULL;
	Desk d;
	guint acme;

	desk(f, &d);

	unpriced = run_ok(f, "2026-08", NULL);
	g_assert_cmpstr(venture_metric_get_text(metric(unpriced, "support_cost")), ==, "n/a");
	acme = row_named(unpriced, "company", "Acme");
	assert_cell(unpriced, acme, "support_cost", "");
	g_assert_nonnull(strstr(note(unpriced), "No support rate is configured"));

	/* The worklog rate, in the book currency: 90 minutes at 40 is 60.00;
	 * the ticket without minutes costs nothing, and the note says so. */
	rates(f, 0, 0, "40 USD");
	worklog_rate = run_ok(f, "2026-08", NULL);
	acme = row_named(worklog_rate, "company", "Acme");
	assert_cell(worklog_rate, acme, "support_cost", "$60.00");
	g_assert_cmpint(money_metric(worklog_rate, "support_cost"), ==, 6000);
	g_assert_nonnull(strstr(note(worklog_rate), "worklog hourly_rate"));
	g_assert_nonnull(strstr(note(worklog_rate), "3 tickets have no logged minutes"));

	/* An integer support rate beats it. */
	rates(f, 60, 0, "40 USD");
	overridden = run_ok(f, "2026-08", NULL);
	acme = row_named(overridden, "company", "Acme");
	assert_cell(overridden, acme, "support_cost", "$90.00");
	g_assert_null(strstr(note(overridden), "worklog hourly_rate"));

	/* A worklog rate in another currency is not converted. */
	rates(f, 0, 0, "40 EUR");
	foreign = run_ok(f, "2026-08", NULL);
	g_assert_cmpstr(venture_metric_get_text(metric(foreign, "support_cost")), ==, "n/a");
	g_assert_nonnull(strstr(note(foreign), "40.00 EUR"));

	/* A flat rate alone prices only tickets without minutes: 3 x 25, and
	 * Acme's 90 minutes are counted but cost nothing. */
	rates(f, 0, 25, "0 USD");
	flat_only = run_ok(f, "2026-08", NULL);
	acme = row_named(flat_only, "company", "Acme");
	assert_cell(flat_only, acme, "support_cost", "$25.00");
	assert_cell(flat_only, acme, "minutes", "90");
	g_assert_cmpint(money_metric(flat_only, "support_cost"), ==, 7500);
	g_assert_nonnull(strstr(note(flat_only), "90 logged agent minutes cost nothing"));

	/* A third of an hour is 20 minutes, rounded once; at 60 an hour that is
	 * 20.00, not 19.98. */
	log_hours(f, d.t3, 0.333);
	rates(f, 60, 0, NULL);
	rounded = run_ok(f, "2026-08", NULL);
	assert_cell(rounded, row_named(rounded, "company", "Bolt"), "minutes", "20");
	assert_cell(rounded, row_named(rounded, "company", "Bolt"), "support_cost", "$20.00");
}

/* The options, and every refusal. */
static void
test_options(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	Desk d;

	desk(f, &d);
	rates(f, 60, 25, NULL);

	{
		g_autoptr(JsonObject) by_tickets = options("sort=-tickets", NULL);
		g_autoptr(JsonObject) by_fewest = options("sort=tickets", NULL);
		g_autoptr(JsonObject) by_name = options("sort=-company", NULL);
		g_autoptr(VentureReportResult) most = run_ok(f, "2026-08", by_tickets);
		g_autoptr(VentureReportResult) fewest = run_ok(f, "2026-08", by_fewest);
		g_autoptr(VentureReportResult) names = run_ok(f, "2026-08", by_name);
		assert_cell(most, 0, "company", "Acme");
		assert_cell(fewest, 2, "company", "Acme");
		assert_cell(names, 0, "company", "Bolt");
	}
	{
		g_autoptr(JsonObject) bogus = options("sort=-profit", NULL);
		g_autoptr(VentureReportResult) result = run(f, "2026-08", bogus, &error);
		g_assert_null(result);
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
		g_assert_nonnull(strstr(error->message, "no column called \"profit\""));
		g_clear_error(&error);
	}
	{
		g_autoptr(JsonObject) unknown = options("colour=red", NULL);
		g_autoptr(VentureReportResult) result = run(f, "2026-08", unknown, &error);
		g_assert_null(result);
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
		g_assert_nonnull(strstr(error->message, "\"colour\""));
		g_clear_error(&error);
	}
	{
		/* Hiding small customers changes the table, not the totals. */
		g_autoptr(JsonObject) minimum = options("min_tickets=2", NULL);
		g_autoptr(VentureReportResult) result = run_ok(f, "2026-08", minimum);
		g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 1);
		assert_cell(result, 0, "company", "Acme");
		g_assert_cmpint(count_metric(result, "companies"), ==, 1);
		g_assert_cmpint(count_metric(result, "tickets"), ==, 4);
		g_assert_cmpint(money_metric(result, "support_cost"), ==, 16500);
		g_assert_nonnull(strstr(note(result), "2 companies with fewer than 2 tickets are not listed"));
	}
	{
		g_autoptr(JsonObject) negative = options("min_tickets=-1", NULL);
		g_autoptr(JsonObject) words = options("min_tickets=many", NULL);
		g_autoptr(VentureReportResult) result = run(f, "2026-08", negative, &error);
		g_assert_null(result);
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
		g_clear_error(&error);
		result = run(f, "2026-08", words, &error);
		g_assert_null(result);
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
		g_clear_error(&error);
	}
	{
		/* One company: its rows and its total, which is what its page shows. */
		g_autofree gchar *pair = g_strdup_printf("company=%" G_GINT64_FORMAT, d.bolt);
		g_autoptr(JsonObject) one = options(pair, NULL);
		g_autoptr(JsonObject) text = options("company=bolt", NULL);
		g_autoptr(JsonObject) zero = options("company=0", NULL);
		g_autoptr(VentureReportResult) result = run_ok(f, "2026-08", one);
		g_autoptr(VentureReportResult) refused = NULL;
		g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 1);
		assert_cell(result, 0, "company", "Bolt");
		g_assert_cmpint(count_metric(result, "tickets"), ==, 1);
		g_assert_cmpint(money_metric(result, "support_cost"), ==, 2500);
		refused = run(f, "2026-08", text, &error);
		g_assert_null(refused);
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
		g_clear_error(&error);
		refused = run(f, "2026-08", zero, &error);
		g_assert_null(refused);
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
		g_clear_error(&error);
	}
	{
		g_autoptr(JsonObject) grouping = options("group_by=assignee", NULL);
		g_autoptr(VentureReportResult) result = run(f, "2026-08", grouping, &error);
		g_assert_null(result);
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
		g_assert_nonnull(strstr(error->message, "group_by must be company or product"));
		g_clear_error(&error);
	}
	{
		/* The scope every report takes still works: another organisation
		 * has no tickets, and a venture filter keeps only its tickets. */
		g_autoptr(JsonObject) elsewhere = options("organization_id=999999", NULL);
		g_autoptr(JsonObject) venture = options("venture_id=999999", NULL);
		g_autoptr(VentureReportResult) empty = run_ok(f, "2026-08", elsewhere);
		g_autoptr(VentureReportResult) none = run_ok(f, "2026-08", venture);
		g_assert_cmpuint(venture_report_result_get_row_count(empty), ==, 0);
		g_assert_cmpint(count_metric(none, "tickets"), ==, 0);
	}
}

/*
 * Ticket volume by product: a custom field called "product" on the
 * ticket, read through the custom fields service. group_by=product rows
 * the same figures per product; product=NAME keeps one product's tickets
 * in the per-company table.
 */
static void
test_products(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) definition = NULL;
	g_autoptr(JsonObject) by_product = options("group_by=product", NULL);
	g_autoptr(JsonObject) one_product = options("product=Widget", NULL);
	g_autoptr(JsonObject) both = options("group_by=product", "sort=product", NULL);
	g_autoptr(VentureReportResult) grouped = NULL;
	g_autoptr(VentureReportResult) filtered = NULL;
	g_autoptr(VentureReportResult) sorted = NULL;
	VentureCustomFieldsService *fields;
	Desk d;
	guint widget, gadget, none;

	desk(f, &d);
	rates(f, 60, 25, NULL);
	g_assert_true(venture_context_module_enabled(f->context, "custom_fields"));
	fields = venture_custom_fields_service_get(f->db);
	definition = venture_custom_fields_service_define(fields, f->org, "ticket", "product", "string", FALSE, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(definition);
	g_assert_true(venture_custom_fields_service_put_value(fields, f->org, "ticket", d.t1, "product", "Widget", NULL, &error));
	g_assert_no_error(error);
	g_assert_true(venture_custom_fields_service_put_value(fields, f->org, "ticket", d.t2, "product", "Gadget", NULL, &error));
	g_assert_no_error(error);
	g_assert_true(venture_custom_fields_service_put_value(fields, f->org, "ticket", d.t3, "product", "Widget", NULL, &error));
	g_assert_no_error(error);

	grouped = run_ok(f, "2026-08", by_product);
	g_assert_cmpuint(venture_report_result_get_row_count(grouped), ==, 3);
	widget = row_named(grouped, "product", "Widget");
	gadget = row_named(grouped, "product", "Gadget");
	none = row_named(grouped, "product", "(no product)");
	g_assert_cmpuint(widget, !=, G_MAXUINT);
	g_assert_cmpuint(gadget, !=, G_MAXUINT);
	g_assert_cmpuint(none, !=, G_MAXUINT);
	/* Widget: Acme's answered ticket and Bolt's: 90 minutes plus one flat. */
	assert_cell(grouped, widget, "tickets", "2");
	assert_cell(grouped, widget, "closed", "2");
	assert_cell(grouped, widget, "minutes", "90");
	assert_cell(grouped, widget, "support_cost", "$115.00");
	assert_cell(grouped, widget, "csat", "50.0%");
	assert_cell(grouped, gadget, "tickets", "1");
	assert_cell(grouped, gadget, "open", "1");
	assert_cell(grouped, gadget, "breaches", "1");
	assert_cell(grouped, none, "tickets", "1");
	g_assert_cmpint(count_metric(grouped, "products"), ==, 3);
	g_assert_cmpint(money_metric(grouped, "support_cost"), ==, 16500);

	/* One product, per company. */
	filtered = run_ok(f, "2026-08", one_product);
	g_assert_cmpuint(venture_report_result_get_row_count(filtered), ==, 2);
	g_assert_cmpuint(row_named(filtered, "company", "Acme"), !=, G_MAXUINT);
	g_assert_cmpuint(row_named(filtered, "company", "Bolt"), !=, G_MAXUINT);
	g_assert_cmpint(count_metric(filtered, "tickets"), ==, 2);
	g_assert_cmpint(money_metric(filtered, "support_cost"), ==, 11500);

	/* The group column is sortable under its own name. */
	sorted = run_ok(f, "2026-08", both);
	assert_cell(sorted, 0, "product", "(no product)");
	assert_cell(sorted, 1, "product", "Gadget");
	assert_cell(sorted, 2, "product", "Widget");
}

/*
 * One function for the number. The Support card's figure for the period is
 * venture_support_rollup_cost() for the organisation; the report's total
 * is the same metric; a company's share is the same function with the
 * company named.
 */
static void
test_one_function(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDateRange) period = NULL;
	g_autoptr(JsonObject) scope = json_object_new();
	g_autoptr(JsonObject) bolt_scope = json_object_new();
	g_autoptr(VentureMetric) cost = NULL;
	g_autoptr(VentureMetric) bolt = NULL;
	g_autoptr(VentureMetric) unpriced = NULL;
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(JsonNode) cards = NULL;
	g_autofree gchar *value = NULL;
	JsonArray *array;
	JsonObject *card = NULL;
	JsonArray *lines;
	guint i;
	Desk d;

	desk(f, &d);
	period = venture_context_parse_period(f->context, "2026-08", &error);
	g_assert_no_error(error);
	json_object_set_int_member(scope, "organization_id", f->org);

	/* Before a rate exists the card says n/a, as the report does. */
	unpriced = venture_support_rollup_cost(f->context, scope, period, &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_metric_get_kind(unpriced), ==, VENTURE_METRIC_KIND_TEXT);
	g_assert_cmpstr(venture_metric_get_text(unpriced), ==, "n/a");

	rates(f, 60, 25, NULL);
	cost = venture_support_rollup_cost(f->context, scope, period, &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_metric_get_kind(cost), ==, VENTURE_METRIC_KIND_MONEY);
	g_assert_cmpint(venture_money_get_amount(venture_metric_get_money(cost)), ==, 16500);
	result = run_ok(f, "2026-08", scope);
	g_assert_cmpint(money_metric(result, "support_cost"), ==, venture_money_get_amount(venture_metric_get_money(cost)));

	json_object_set_int_member(bolt_scope, "organization_id", f->org);
	json_object_set_int_member(bolt_scope, "company", d.bolt);
	bolt = venture_support_rollup_cost(f->context, bolt_scope, period, &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_money_get_amount(venture_metric_get_money(bolt)), ==, 2500);

	/* The card's number is the organisation's cost; the queue moved to
	 * the lines beneath it. */
	cards = venture_headline_home_cards(f->context, f->org, period, &error);
	g_assert_no_error(error);
	array = json_node_get_array(cards);
	for (i = 0; i < json_array_get_length(array); i++)
	{
		JsonObject *candidate = json_array_get_object_element(array, i);
		if (g_strcmp0(json_object_get_string_member(candidate, "key"), "support") == 0)
			card = candidate;
	}
	g_assert_nonnull(card);
	value = venture_metric_format_value(cost);
	g_assert_cmpstr(json_object_get_string_member(card, "value"), ==, value);
	g_assert_cmpstr(json_object_get_string_member(card, "value"), ==, "$165.00");
	g_assert_false(json_object_get_boolean_member(card, "higher_is_better"));
	g_assert_true(g_str_has_prefix(json_object_get_string_member(card, "link"), "/reports/support_rollup?"));
	lines = json_object_get_array_member(card, "lines");
	g_assert_cmpuint(json_array_get_length(lines), ==, 3);
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(lines, 0), "label"), ==, "Open tickets");
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(lines, 0), "value"), ==, "2");
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(lines, 2), "label"), ==, "Raised in period");
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(lines, 2), "value"), ==, "4");

	/* A refused option is an error, not a number. */
	{
		g_autoptr(JsonObject) bad = options("sort=nothing", NULL);
		g_autoptr(VentureMetric) refused = venture_support_rollup_cost(f->context, bad, period, &error);
		g_assert_null(refused);
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
		g_clear_error(&error);
	}
}

/* --- Over HTTP and the CLI ------------------------------------------------ */

typedef struct
{
	VentureDatabase *db;
	VentureConfig *config;
	VentureContext *context;
	VentureWebServer *server;
	SoupSession *session;
	gchar *state_dir;
	gchar *cookie;
	guint16 port;
	gint64 org;
} ServerFixture;

typedef struct
{
	gboolean done;
	GBytes *body;
	GError *error;
} RequestResult;

static void
request_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
	RequestResult *outcome = user_data;
	outcome->body = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &outcome->error);
	outcome->done = TRUE;
}

static guint
server_request(ServerFixture *f, const gchar *path, gchar **out_body, gchar **out_content_type)
{
	g_autoptr(SoupMessage) message = NULL;
	g_autofree gchar *url = g_strdup_printf("http://127.0.0.1:%u%s", f->port, path);
	RequestResult outcome = { FALSE, NULL, NULL };
	message = soup_message_new("GET", url);
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	if (f->cookie != NULL)
		soup_message_headers_append(soup_message_get_request_headers(message), "Cookie", f->cookie);
	soup_session_send_and_read_async(f->session, message, G_PRIORITY_DEFAULT, NULL, request_done, &outcome);
	while (!outcome.done)
		g_main_context_iteration(NULL, TRUE);
	if (outcome.error != NULL)
		g_error("GET %s: %s", path, outcome.error->message);
	if (out_body != NULL)
		*out_body = g_strndup(g_bytes_get_data(outcome.body, NULL), g_bytes_get_size(outcome.body));
	if (out_content_type != NULL)
		*out_content_type = g_strdup(soup_message_headers_get_content_type(soup_message_get_response_headers(message), NULL));
	g_clear_pointer(&outcome.body, g_bytes_unref);
	return soup_message_get_status(message);
}

/* Signs @username in and returns the session cookie. */
static gchar *
login(ServerFixture *f, const gchar *username, const gchar *password)
{
	g_autoptr(SoupMessage) message = NULL;
	g_autofree gchar *url = g_strdup_printf("http://127.0.0.1:%u/login", f->port);
	g_autofree gchar *form = g_strdup_printf("username=%s&password=%s", username, password);
	g_autoptr(GBytes) bytes = NULL;
	RequestResult outcome = { FALSE, NULL, NULL };
	gchar *cookie;
	gchar *semicolon;
	message = soup_message_new("POST", url);
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	bytes = g_bytes_new(form, strlen(form));
	soup_message_set_request_body_from_bytes(message, "application/x-www-form-urlencoded", bytes);
	soup_session_send_and_read_async(f->session, message, G_PRIORITY_DEFAULT, NULL, request_done, &outcome);
	while (!outcome.done)
		g_main_context_iteration(NULL, TRUE);
	g_clear_pointer(&outcome.body, g_bytes_unref);
	g_clear_error(&outcome.error);
	cookie = g_strdup(soup_message_headers_get_one(soup_message_get_response_headers(message), "Set-Cookie"));
	g_assert_nonnull(cookie);
	semicolon = strchr(cookie, ';');
	if (semicolon != NULL)
		*semicolon = '\0';
	return cookie;
}

static void
member(ServerFixture *f, const gchar *username, VentureUserRole user_role, gint role)
{
	g_autoptr(VentureUser) user = venture_user_new();
	g_autoptr(VentureEntity) membership = NULL;
	g_object_set(user, "username", username, "role", user_role, "active", TRUE, NULL);
	g_assert_true(venture_user_set_password(user, "member-password-1", 100000, NULL));
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(user), NULL, NULL));
	membership = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP, "organization-id", f->org,
		"user-id", venture_entity_get_id(VENTURE_ENTITY(user)), "active", TRUE, "role", role, NULL);
	g_assert_true(venture_database_save(f->db, membership, NULL, NULL));
}

static void
server_start(ServerFixture *f, gboolean require_auth)
{
	g_autoptr(GError) error = NULL;
	g_setenv("VENTURE_TEST_SESSION_SECRET", "support-rollup-test-secret", TRUE);
	f->state_dir = g_dir_make_tmp("venture-support-rollup-XXXXXX", NULL);
	f->port = (guint16)(20000 + ((getpid() + 14111) % 20000));
	f->config = venture_config_new();
	g_object_set(f->config, "state-dir", f->state_dir, "server-bind-address", "127.0.0.1",
		"server-port", (gint64)f->port, "security-session-secret-env", "VENTURE_TEST_SESSION_SECRET",
		"security-password-iterations", (gint64)100000, "security-require-auth", require_auth, NULL);
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->org = venture_context_get_default_organization_id(f->context);
	f->server = venture_web_server_new(f->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(f->server, &error));
	g_assert_no_error(error);
	f->session = soup_session_new();
}

/* A server that asks for a login: the gates are what is tested. */
static void
server_setup(ServerFixture *f, gconstpointer unused)
{
	server_start(f, TRUE);
	member(f, "viewer", VENTURE_USER_ROLE_VIEWER, VENTURE_ORGANIZATION_ROLE_FINANCE);
	f->cookie = login(f, "viewer", "member-password-1");
}

/* A server that lets the local owner straight in: the pages and the CLI. */
static void
open_setup(ServerFixture *f, gconstpointer unused)
{
	server_start(f, FALSE);
}

static void
server_teardown(ServerFixture *f, gconstpointer unused)
{
	if (f->server != NULL)
		venture_web_server_stop(f->server);
	g_clear_pointer(&f->cookie, g_free);
	g_clear_object(&f->session);
	g_clear_object(&f->server);
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
	if (f->state_dir != NULL)
	{
		venture_test_remove_tree(f->state_dir);
		g_clear_pointer(&f->state_dir, g_free);
	}
	g_unsetenv("VENTURE_TEST_SESSION_SECRET");
}

/* The plain fixture's helpers over the server's database. */
static Fixture
plain(ServerFixture *f)
{
	Fixture p;
	p.db = f->db;
	p.config = f->config;
	p.context = f->context;
	p.org = f->org;
	return p;
}

/*
 * Read like every report: a viewer with a session may read it, nobody
 * without one may, the CSV export is the report's own rendering, and with
 * the tickets module off the report is not there to be asked for.
 */
static void
test_gates(ServerFixture *f, gconstpointer unused)
{
	Fixture p = plain(f);
	g_autofree gchar *body = NULL;
	g_autofree gchar *csv = NULL;
	g_autofree gchar *content_type = NULL;
	g_autofree gchar *refused = NULL;
	g_autofree gchar *cookie = NULL;
	guint status;
	Desk d;

	desk(&p, &d);
	rates(&p, 60, 25, NULL);

	status = server_request(f, "/api/v1/reports/support_rollup?period=2026-08&sort=-tickets&min_tickets=1", &body, NULL);
	g_assert_cmpuint(status, ==, 200);
	g_assert_nonnull(strstr(body, "\"support_cost\""));
	g_assert_nonnull(strstr(body, "Acme"));

	/* The options reach the report: a bad column is refused, not ignored. */
	status = server_request(f, "/api/v1/reports/support_rollup?period=2026-08&sort=-profit", &refused, NULL);
	g_assert_cmpuint(status, >=, 400);
	g_assert_nonnull(strstr(refused, "no column called"));

	status = server_request(f, "/api/v1/reports/support_rollup?period=2026-08&format=csv", &csv, &content_type);
	g_assert_cmpuint(status, ==, 200);
	g_assert_cmpstr(content_type, ==, "text/csv");
	g_assert_true(g_str_has_prefix(csv, "Company,Tickets,Closed,Open now,Service levels missed,Median reply minutes,Median resolve minutes,Satisfaction,Agent minutes,Support cost\n"));
	g_assert_nonnull(strstr(csv, "Acme,2,1,1,1,30,120,100.0%,90,$115.00\n"));

	/* No session, no report. */
	cookie = g_steal_pointer(&f->cookie);
	status = server_request(f, "/api/v1/reports/support_rollup?period=2026-08", NULL, NULL);
	g_assert_cmpuint(status, ==, 401);
	f->cookie = g_steal_pointer(&cookie);

	/* The tickets module off takes the report with it, cleanly. */
	venture_config_set_module_enabled(f->config, "tickets", FALSE);
	g_assert_null(venture_report_registry_lookup(venture_context_get_report_registry(f->context), "support_rollup"));
	g_clear_pointer(&body, g_free);
	status = server_request(f, "/api/v1/reports/support_rollup?period=2026-08", &body, NULL);
	g_assert_cmpuint(status, ==, 404);
	g_assert_nonnull(strstr(body, "no report called"));
	venture_config_set_module_enabled(f->config, "tickets", TRUE);
	g_assert_nonnull(venture_report_registry_lookup(venture_context_get_report_registry(f->context), "support_rollup"));
}

/* The company page shows this month's cost from the same function. */
static void
test_company_page(ServerFixture *f, gconstpointer unused)
{
	Fixture p = plain(f);
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(VentureDateRange) period = NULL;
	g_autoptr(JsonObject) scope = json_object_new();
	g_autoptr(VentureMetric) cost = NULL;
	g_autofree gchar *today = g_date_time_format(now, "%Y-%m-%d");
	g_autofree gchar *path = NULL;
	g_autofree gchar *page = NULL;
	g_autofree gchar *expected = NULL;
	g_autofree gchar *value = NULL;
	gint64 acme = company(&p, "Acme");
	gint64 raised;
	guint status;

	raised = ticket(&p, acme, today, VENTURE_TICKET_STATUS_DONE, 10, 20, FALSE, VENTURE_SATISFACTION_GOOD);
	log_hours(&p, raised, 2.0);
	ticket(&p, acme, today, VENTURE_TICKET_STATUS_TODO, -1, -1, FALSE, VENTURE_SATISFACTION_UNRATED);
	rates(&p, 50, 10, NULL);

	period = venture_context_parse_period(f->context, "this_month", &error);
	g_assert_no_error(error);
	json_object_set_int_member(scope, "organization_id", f->org);
	json_object_set_int_member(scope, "company", acme);
	cost = venture_support_rollup_cost(f->context, scope, period, &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_money_get_amount(venture_metric_get_money(cost)), ==, 11000);
	value = venture_metric_format_value(cost);

	path = g_strdup_printf("/e/company/%" G_GINT64_FORMAT, acme);
	status = server_request(f, path, &page, NULL);
	g_assert_cmpuint(status, ==, 200);
	g_assert_nonnull(strstr(page, "data-block=\"support-cost\""));
	expected = g_strdup_printf("<dt>Support cost (this month)</dt><dd>%s</dd>", value);
	g_assert_nonnull(strstr(page, expected));
	g_clear_pointer(&expected, g_free);
	expected = g_strdup_printf("/reports/support_rollup?period=this_month&amp;company=%" G_GINT64_FORMAT, acme);
	g_assert_nonnull(strstr(page, expected));

	/* The home page's Support card carries the organisation's figure. */
	g_clear_pointer(&page, g_free);
	status = server_request(f, "/?period=this_month", &page, NULL);
	g_assert_cmpuint(status, ==, 200);
	g_assert_nonnull(strstr(page, "data-card=\"support\""));
	g_assert_nonnull(strstr(page, value));

	/* Somebody else's page carries no support block. */
	g_clear_pointer(&page, g_free);
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/e/ticket/%" G_GINT64_FORMAT, raised);
	status = server_request(f, path, &page, NULL);
	g_assert_cmpuint(status, ==, 200);
	g_assert_null(strstr(page, "data-block=\"support-cost\""));
}

typedef struct { gboolean done; GError *error; gchar *out; gchar *err; } CliResult;

static void
cli_done(GObject *source, GAsyncResult *result, gpointer data)
{
	CliResult *r = data;
	g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &r->out, &r->err, &r->error);
	r->done = TRUE;
}

static gboolean
cli_timeout(gpointer process)
{
	g_subprocess_force_exit(process);
	return G_SOURCE_CONTINUE;
}

/* Runs venturectl against the fixture's server; returns stdout. */
static gchar *
cli(ServerFixture *f, const gchar *format, const gchar *const *args, gboolean expect_success)
{
	g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
	g_autoptr(GSubprocess) process = NULL;
	g_autoptr(GPtrArray) argv = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GError) error = NULL;
	CliResult result;
	guint i, timeout;
	memset(&result, 0, sizeof(result));
	g_ptr_array_add(argv, g_canonicalize_filename("build/debug/venturectl", NULL));
	g_ptr_array_add(argv, g_strdup("--server"));
	g_ptr_array_add(argv, g_strdup(venture_web_server_get_base_url(f->server)));
	g_ptr_array_add(argv, g_strdup("-f"));
	g_ptr_array_add(argv, g_strdup(format));
	for (i = 0; args[i] != NULL; i++)
		g_ptr_array_add(argv, g_strdup(args[i]));
	g_ptr_array_add(argv, NULL);
	g_subprocess_launcher_setenv(launcher, "VENTURE_TOKEN", "support-rollup-fixture", TRUE);
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

/* venturectl support rollup --from --to [--sort]: the report route. */
static void
test_cli(ServerFixture *f, gconstpointer unused)
{
	Fixture p = plain(f);
	const gchar *rollup[] = { "support", "rollup", "--from", "2026-08-01", "--to", "2026-08-31", "--sort", "-tickets", NULL };
	const gchar *filtered[] = { "support", "rollup", "--from", "2026-08-01", "--to", "2026-08-31", "min_tickets=2", NULL };
	const gchar *no_span[] = { "support", "rollup", NULL };
	const gchar *no_verb[] = { "support", "cost", "--from", "2026-08-01", "--to", "2026-08-31", NULL };
	const gchar *bad_sort[] = { "support", "rollup", "--from", "2026-08-01", "--to", "2026-08-31", "--sort", "profit", NULL };
	g_autofree gchar *json = NULL;
	g_autofree gchar *csv = NULL;
	g_autofree gchar *narrowed = NULL;
	g_autofree gchar *usage = NULL;
	g_autofree gchar *wrong = NULL;
	g_autofree gchar *refused = NULL;
	g_autoptr(JsonNode) node = NULL;
	JsonObject *report;
	JsonArray *rows;
	Desk d;

	desk(&p, &d);
	rates(&p, 60, 25, NULL);

	json = cli(f, "json", rollup, TRUE);
	node = venture_json_parse(json, NULL);
	g_assert_nonnull(node);
	report = json_node_get_object(node);
	rows = json_object_get_array_member(report, "rows");
	g_assert_cmpuint(json_array_get_length(rows), ==, 3);
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(rows, 0), "company"), ==, "Acme");
	g_assert_nonnull(strstr(json, "\"support_cost\""));

	csv = cli(f, "csv", rollup, TRUE);
	g_assert_true(g_str_has_prefix(csv, "Company,Tickets,Closed,Open now,Service levels missed,Median reply minutes,Median resolve minutes,Satisfaction,Agent minutes,Support cost\n"));

	narrowed = cli(f, "json", filtered, TRUE);
	g_assert_nonnull(strstr(narrowed, "Acme"));
	g_assert_null(strstr(narrowed, "Bolt"));

	usage = cli(f, "json", no_span, FALSE);
	wrong = cli(f, "json", no_verb, FALSE);
	refused = cli(f, "json", bad_sort, FALSE);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	venture_entity_registry_register_builtins(venture_entity_registry_get_default());
	g_test_add("/support-rollup/rows", Fixture, NULL, setup, test_rows, teardown);
	g_test_add("/support-rollup/rates", Fixture, NULL, setup, test_rates, teardown);
	g_test_add("/support-rollup/options", Fixture, NULL, setup, test_options, teardown);
	g_test_add("/support-rollup/products", Fixture, NULL, setup, test_products, teardown);
	g_test_add("/support-rollup/one-function", Fixture, NULL, setup, test_one_function, teardown);
	g_test_add("/support-rollup/gates", ServerFixture, NULL, server_setup, test_gates, server_teardown);
	g_test_add("/support-rollup/company-page", ServerFixture, NULL, open_setup, test_company_page, server_teardown);
	g_test_add("/support-rollup/cli", ServerFixture, NULL, open_setup, test_cli, server_teardown);
	return g_test_run();
}
