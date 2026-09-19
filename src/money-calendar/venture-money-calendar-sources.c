/*
 * venture-money-calendar-sources.c - the six built-in money calendar sources
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Each source reads records the owning module already keeps and emits
 * events; none of them writes anything. Amounts are open balances where a
 * service can say what is still owed (bills, invoices), and template or
 * line sums where nothing has been issued yet (schedules, payroll, tax).
 * An item past due and still open is carried to today and flagged, so the
 * grid shows what must be paid, not what should have been.
 */

#include "venture.h"
#include "money-calendar/venture-money-calendar-private.h"

#include <string.h>

/* The recurring module's enums are registered as GEnums, not C constants;
 * these mirror venture-recurring-records.c in order. */
enum { KIND_INVOICE = 0, KIND_BILL = 1, KIND_EXPENSE = 2, KIND_JOURNAL = 3 };
enum { FREQUENCY_MONTHLY = 0, FREQUENCY_WEEKLY = 1, FREQUENCY_DAILY = 2, FREQUENCY_YEARLY = 3 };
enum { OCCURRENCE_GENERATED = 0 };

/* --- Shared plumbing ------------------------------------------------------- */

static gint64
number(VentureEntity *e, const gchar *field)
{
	gint64 value = 0;
	g_object_get(e, field, &value, NULL);
	return value;
}

static gchar *
text(VentureEntity *e, const gchar *field)
{
	gchar *value = NULL;
	g_object_get(e, field, &value, NULL);
	return value;
}

static gboolean
flag(VentureEntity *e, const gchar *field)
{
	gboolean value = FALSE;
	g_object_get(e, field, &value, NULL);
	return value;
}

static GDateTime *
when(VentureEntity *e, const gchar *field)
{
	GDateTime *value = NULL;
	g_object_get(e, field, &value, NULL);
	return value;
}

static VentureMoney *
money(VentureEntity *e, const gchar *field)
{
	VentureMoney *value = NULL;
	g_object_get(e, field, &value, NULL);
	return value;
}

static gchar *
day_text(GDateTime *date)
{
	return g_date_time_format(date, "%Y-%m-%d");
}

/* Days compare as text: the endpoints of a range are local midnights and an
 * event's date is whatever calendar day its source declared. */
static gboolean
in_range(GDateTime *date, const VentureDateRange *range)
{
	g_autofree gchar *day = day_text(date);
	GDateTime *start = venture_date_range_get_start(range);
	GDateTime *end = venture_date_range_get_end(range);
	if (start != NULL)
	{
		g_autofree gchar *first = day_text(start);
		if (g_strcmp0(day, first) < 0)
			return FALSE;
	}
	if (end != NULL)
	{
		g_autofree gchar *stop = day_text(end);
		if (g_strcmp0(day, stop) >= 0)
			return FALSE;
	}
	return TRUE;
}

static gboolean
before_today(GDateTime *date, GDateTime *today)
{
	g_autofree gchar *day = day_text(date);
	g_autofree gchar *now = day_text(today);
	return g_strcmp0(day, now) < 0;
}

/* The event for a dated open item: on its day, or carried to today and
 * flagged when the day has passed. NULL when it falls outside the range. */
static VentureMoneyCalendarEvent *
dated_event(const gchar *kind, GDateTime *due, GDateTime *today, const VentureDateRange *range,
	const gchar *title, const VentureMoney *amount, const gchar *direction, const gchar *record_type, gint64 record_id)
{
	VentureMoneyCalendarEvent *event;
	gboolean overdue = before_today(due, today);
	GDateTime *shown = overdue ? today : due;
	g_autofree gchar *anchor = day_text(due);
	if (!in_range(shown, range))
		return NULL;
	event = venture_money_calendar_event_new(kind, shown, title, amount, direction, record_type, record_id);
	g_object_set(event, "anchor-day", anchor, "overdue", overdue, NULL);
	return event;
}

static VentureEntity *
load(VentureDatabase *db, GType type, gint64 id, gint64 org)
{
	g_autoptr(VentureEntity) row = NULL;
	if (id <= 0 || type == G_TYPE_INVALID)
		return NULL;
	row = venture_database_get(db, type, id, NULL);
	if (row == NULL || venture_entity_is_deleted(row) || venture_entity_get_organization_id(row) != org)
		return NULL;
	return g_steal_pointer(&row);
}

static gchar *
company_name(VentureDatabase *db, gint64 id, gint64 org)
{
	g_autoptr(VentureEntity) company = load(db, VENTURE_TYPE_COMPANY, id, org);
	return company != NULL ? text(company, "name") : NULL;
}

static GPtrArray *
rows_with_status(VentureDatabase *db, GType type, gint64 org, const gchar *field, const gchar *status, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	venture_query_set_organization(query, org);
	if (status != NULL && !venture_query_add_filter_string(query, field, VENTURE_FILTER_OP_EQ, status, error))
		return NULL;
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	venture_query_set_limit(query, 0);
	return venture_database_find(db, query, error);
}

static GPtrArray *
rows_with_int(VentureDatabase *db, GType type, gint64 org, const gchar *field, gint64 value, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	venture_query_set_organization(query, org);
	if (!venture_query_add_filter_int(query, field, VENTURE_FILTER_OP_EQ, value, error))
		return NULL;
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	venture_query_set_limit(query, 0);
	return venture_database_find(db, query, error);
}

/* A decimal quantity ("2.5", "3", or a JSON number) times a price, exactly:
 * the digits become an integer rational, the money is multiplied once. */
static VentureMoney *
line_amount(JsonObject *line, const gchar *default_currency, GError **error)
{
	JsonNode *quantity_node = json_object_get_member(line, "quantity");
	const gchar *price_text = venture_json_object_get_string(line, "unit_price", NULL);
	g_autoptr(VentureMoney) price = NULL;
	g_autoptr(VentureMoney) total = NULL;
	g_autofree gchar *quantity_text = NULL;
	gint64 numerator = 0, denominator = 1;
	const gchar *p;
	if (price_text == NULL)
		price_text = venture_json_object_get_string(line, "unit-price", NULL);
	if (venture_string_is_empty(price_text))
		return venture_money_new_zero(default_currency);
	price = venture_money_from_string(price_text, default_currency, error);
	if (price == NULL)
		return NULL;
	if (quantity_node == NULL || JSON_NODE_HOLDS_NULL(quantity_node))
		quantity_text = g_strdup("1");
	else if (json_node_get_value_type(quantity_node) == G_TYPE_STRING)
		quantity_text = g_strdup(json_node_get_string(quantity_node));
	else if (json_node_get_value_type(quantity_node) == G_TYPE_INT64)
		quantity_text = g_strdup_printf("%" G_GINT64_FORMAT, json_node_get_int(quantity_node));
	else
		quantity_text = g_strdup_printf("%.3f", json_node_get_double(quantity_node));
	for (p = quantity_text; *p; p++)
	{
		if (*p == '.')
		{
			for (p++; *p; p++)
			{
				if (!g_ascii_isdigit(*p))
					break;
				numerator = numerator * 10 + (*p - '0');
				denominator *= 10;
			}
			break;
		}
		if (!g_ascii_isdigit(*p))
			break;
		numerator = numerator * 10 + (*p - '0');
	}
	total = venture_money_multiply_rational(price, numerator, denominator, error);
	if (total == NULL)
		return NULL;
	{
		const gchar *tax_text = venture_json_object_get_string(line, "tax_amount", NULL);
		if (tax_text == NULL)
			tax_text = venture_json_object_get_string(line, "tax-amount", NULL);
		if (!venture_string_is_empty(tax_text))
		{
			g_autoptr(VentureMoney) tax = venture_money_from_string(tax_text, venture_money_get_currency(price), error);
			if (tax == NULL)
				return NULL;
			return venture_money_add(total, tax, error);
		}
	}
	return g_steal_pointer(&total);
}

/* --- A tiny class per source ------------------------------------------------- */

#define DEFINE_SOURCE(Camel, snake, kind_text, module_text, expand_fn) \
	typedef struct { GObject parent; } Camel; \
	typedef struct { GObjectClass parent; } Camel##Class; \
	static GType snake##_get_type(void); \
	static const gchar *snake##_kind(VentureMoneyCalendarSource *s) { (void)s; return kind_text; } \
	static const gchar *snake##_module(VentureMoneyCalendarSource *s) { (void)s; return module_text; } \
	static void snake##_iface(VentureMoneyCalendarSourceInterface *iface) \
	{ iface->get_kind = snake##_kind; iface->get_module = snake##_module; iface->expand = expand_fn; } \
	G_DEFINE_FINAL_TYPE_WITH_CODE(Camel, snake, G_TYPE_OBJECT, \
		G_IMPLEMENT_INTERFACE(VENTURE_TYPE_MONEY_CALENDAR_SOURCE, snake##_iface)) \
	static void snake##_init(Camel *self) { (void)self; } \
	static void snake##_class_init(Camel##Class *klass) { (void)klass; }

/* --- Recurring schedules ------------------------------------------------------- */

/* The amount one occurrence of a schedule is worth, from its template:
 * invoice and bill lines summed, an expense's amount. */
static VentureMoney *
template_amount(VentureEntity *schedule, gint kind, const gchar *book_currency, GError **error)
{
	g_autofree gchar *json = text(schedule, "template");
	g_autoptr(JsonNode) node = NULL;
	JsonObject *object;
	if (venture_string_is_empty(json))
		return venture_money_new_zero(book_currency);
	node = venture_json_parse(json, error);
	if (node == NULL)
		return NULL;
	if (!JSON_NODE_HOLDS_OBJECT(node))
		return venture_money_new_zero(book_currency);
	object = json_node_get_object(node);
	if (kind == KIND_EXPENSE)
	{
		const gchar *amount = venture_json_object_get_string(object, "amount", NULL);
		if (venture_string_is_empty(amount))
			return venture_money_new_zero(book_currency);
		return venture_money_from_string(amount, book_currency, error);
	}
	{
		JsonNode *lines_node = json_object_get_member(object, "lines");
		const gchar *currency = venture_json_object_get_string(object, "currency", book_currency);
		g_autoptr(VentureMoney) total = NULL;
		JsonArray *lines;
		guint i;
		if (lines_node == NULL || !JSON_NODE_HOLDS_ARRAY(lines_node))
			return venture_money_new_zero(currency);
		lines = json_node_get_array(lines_node);
		for (i = 0; i < json_array_get_length(lines); i++)
		{
			JsonNode *line = json_array_get_element(lines, i);
			g_autoptr(VentureMoney) amount = NULL;
			if (!JSON_NODE_HOLDS_OBJECT(line))
				continue;
			amount = line_amount(json_node_get_object(line), currency, error);
			if (amount == NULL)
				return NULL;
			if (total == NULL)
				total = g_steal_pointer(&amount);
			else
			{
				VentureMoney *sum = venture_money_add(total, amount, error);
				if (sum == NULL)
					return NULL;
				venture_money_free(total);
				total = sum;
			}
		}
		return total != NULL ? g_steal_pointer(&total) : venture_money_new_zero(currency);
	}
}

/* Same arithmetic as the recurring service: the start in the schedule's own
 * timezone, plus N days, weeks, months or years. Adding a month to a local
 * time keeps the wall clock across a DST change and clamps to month end. */
static GDateTime *
cycle_date(GDateTime *start, gint frequency, gint64 index)
{
	if (start == NULL || index < 0 || index > G_MAXINT / 7)
		return NULL;
	if (frequency == FREQUENCY_WEEKLY)
		return g_date_time_add_days(start, 7 * (gint)index);
	if (frequency == FREQUENCY_DAILY)
		return g_date_time_add_days(start, (gint)index);
	if (frequency == FREQUENCY_YEARLY)
		return g_date_time_add_years(start, (gint)index);
	return g_date_time_add_months(start, (gint)index);
}

static GHashTable *
generated_days(VentureDatabase *db, VentureEntity *schedule, GError **error)
{
	g_autoptr(GPtrArray) rows = rows_with_int(db, VENTURE_TYPE_RECURRING_OCCURRENCE,
		venture_entity_get_organization_id(schedule), "schedule-id", venture_entity_get_id(schedule), error);
	GHashTable *days;
	guint i;
	if (rows == NULL)
		return NULL;
	days = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autofree gchar *key = text(row, "occurrence-key");
		gint status = 0;
		const gchar *colon;
		g_object_get(row, "status", &status, NULL);
		if (status != OCCURRENCE_GENERATED || key == NULL)
			continue;
		colon = strrchr(key, ':');
		if (colon != NULL)
			g_hash_table_add(days, g_strdup(colon + 1));
	}
	return days;
}

static gboolean
recurring_expand(VentureMoneyCalendarSource *self, VentureContext *context, gint64 org,
	const VentureDateRange *range, GDateTime *today, GPtrArray *events, GError **error)
{
	VentureDatabase *db = venture_context_get_database(context);
	g_autoptr(GPtrArray) schedules = rows_with_status(db, VENTURE_TYPE_RECURRING_SCHEDULE, org, NULL, NULL, error);
	g_autoptr(VentureEntity) organization = NULL;
	g_autofree gchar *book_currency = NULL;
	guint i;
	(void)self;
	(void)today;
	if (schedules == NULL)
		return FALSE;
	organization = venture_database_get(db, VENTURE_TYPE_ORGANIZATION, org, NULL);
	if (organization != NULL)
		book_currency = text(organization, "default-currency");
	if (venture_string_is_empty(book_currency))
	{
		g_free(book_currency);
		book_currency = g_strdup(venture_money_get_default_currency());
	}
	for (i = 0; i < schedules->len; i++)
	{
		VentureEntity *schedule = g_ptr_array_index(schedules, i);
		g_autoptr(GDateTime) start = when(schedule, "start-at");
		g_autoptr(GDateTime) end = when(schedule, "end-at");
		g_autoptr(GDateTime) local_start = NULL;
		g_autoptr(GTimeZone) zone = NULL;
		g_autofree gchar *zone_name = text(schedule, "timezone");
		g_autofree gchar *name = text(schedule, "name");
		g_autofree gchar *end_day = NULL;
		g_autoptr(VentureMoney) amount = NULL;
		g_autoptr(GHashTable) done = NULL;
		gint kind = 0, frequency = 0;
		gint64 index;
		guint n;
		if (flag(schedule, "paused") || start == NULL)
			continue;
		g_object_get(schedule, "kind", &kind, "frequency", &frequency, "cycle-index", &index, NULL);
		if (kind == KIND_JOURNAL)
			continue;
		zone = g_time_zone_new_identifier(venture_string_is_empty(zone_name) ? "UTC" : zone_name);
		if (zone == NULL)
			continue;
		local_start = g_date_time_to_timezone(start, zone);
		if (end != NULL)
		{
			g_autoptr(GDateTime) local_end = g_date_time_to_timezone(end, zone);
			end_day = day_text(local_end);
		}
		amount = template_amount(schedule, kind, book_currency, error);
		if (amount == NULL)
			return FALSE;
		done = generated_days(db, schedule, error);
		if (done == NULL)
			return FALSE;
		for (n = 0; n < 5000; n++, index++)
		{
			g_autoptr(GDateTime) at = cycle_date(local_start, frequency, index);
			g_autofree gchar *day = NULL;
			g_autofree gchar *title = NULL;
			VentureMoneyCalendarEvent *event;
			GDateTime *range_end = venture_date_range_get_end(range);
			if (at == NULL)
				break;
			day = day_text(at);
			if (end_day != NULL && g_strcmp0(day, end_day) > 0)
				break;
			if (range_end != NULL)
			{
				g_autofree gchar *stop = day_text(range_end);
				if (g_strcmp0(day, stop) >= 0)
					break;
			}
			if (!in_range(at, range) || g_hash_table_contains(done, day))
				continue;
			title = g_strdup_printf("%s %s", kind == KIND_INVOICE ? "Recurring invoice"
				: kind == KIND_BILL ? "Recurring bill" : "Recurring expense", name ? name : "");
			event = venture_money_calendar_event_new("recurring", at, title, amount,
				kind == KIND_INVOICE ? "in" : "out", "recurring_schedule",
				venture_entity_get_id(schedule));
			g_ptr_array_add(events, event);
		}
	}
	return TRUE;
}

DEFINE_SOURCE(RecurringSource, recurring_source, "recurring", "recurring", recurring_expand)

/* --- Bills ----------------------------------------------------------------------- */

static gboolean
bills_expand(VentureMoneyCalendarSource *self, VentureContext *context, gint64 org,
	const VentureDateRange *range, GDateTime *today, GPtrArray *events, GError **error)
{
	static const gchar *const statuses[] = { "approved", "partially_paid", NULL };
	VentureDatabase *db = venture_context_get_database(context);
	VenturePayablesService *payables = venture_payables_service_get(db);
	guint s, i;
	(void)self;
	for (s = 0; statuses[s] != NULL; s++)
	{
		g_autoptr(GPtrArray) bills = rows_with_status(db, VENTURE_TYPE_VENDOR_BILL, org, "status", statuses[s], error);
		if (bills == NULL)
			return FALSE;
		for (i = 0; i < bills->len; i++)
		{
			VentureEntity *bill = g_ptr_array_index(bills, i);
			g_autoptr(GDateTime) due = when(bill, "due-date");
			g_autoptr(VentureMoney) balance = NULL;
			g_autofree gchar *number_text = text(bill, "number");
			g_autofree gchar *vendor = company_name(db, number(bill, "company-id"), org);
			g_autofree gchar *title = NULL;
			VentureMoneyCalendarEvent *event;
			if (due == NULL)
				continue;
			balance = venture_payables_service_bill_balance(payables, venture_entity_get_id(bill), NULL, error);
			if (balance == NULL)
				return FALSE;
			if (venture_money_is_zero(balance) || venture_money_is_negative(balance))
				continue;
			title = g_strdup_printf("Bill %s %s", number_text ? number_text : "", vendor ? vendor : "");
			event = dated_event("bill", due, today, range, title, balance, "out", "vendor_bill", venture_entity_get_id(bill));
			if (event == NULL)
				continue;
			g_object_set(event, "counterparty", vendor, "counterparty-id", number(bill, "company-id"),
				"venture-id", number(bill, "venture-id"), NULL);
			g_ptr_array_add(events, event);
		}
	}
	return TRUE;
}

DEFINE_SOURCE(BillSource, bill_source, "bill", "payables", bills_expand)

/* --- Invoices ----------------------------------------------------------------------- */

static GPtrArray *
open_invoices(VentureDatabase *db, gint64 org, GError **error)
{
	static const gint statuses[] = { VENTURE_INVOICE_STATUS_SENT, VENTURE_INVOICE_STATUS_PARTIALLY_PAID };
	GPtrArray *all = g_ptr_array_new_with_free_func(g_object_unref);
	guint s, i;
	for (s = 0; s < G_N_ELEMENTS(statuses); s++)
	{
		g_autoptr(GPtrArray) rows = rows_with_int(db, VENTURE_TYPE_INVOICE, org, "status", statuses[s], error);
		if (rows == NULL)
		{
			g_ptr_array_unref(all);
			return NULL;
		}
		for (i = 0; i < rows->len; i++)
			g_ptr_array_add(all, g_object_ref(g_ptr_array_index(rows, i)));
	}
	return all;
}

static gboolean
invoices_expand(VentureMoneyCalendarSource *self, VentureContext *context, gint64 org,
	const VentureDateRange *range, GDateTime *today, GPtrArray *events, GError **error)
{
	VentureDatabase *db = venture_context_get_database(context);
	VentureSettlementService *settlement = venture_settlement_service_get(db);
	g_autoptr(GPtrArray) invoices = open_invoices(db, org, error);
	guint i;
	(void)self;
	if (invoices == NULL)
		return FALSE;
	for (i = 0; i < invoices->len; i++)
	{
		VentureEntity *invoice = g_ptr_array_index(invoices, i);
		g_autoptr(GDateTime) due = when(invoice, "due-at");
		g_autoptr(VentureMoney) balance = NULL;
		g_autofree gchar *number_text = text(invoice, "number");
		g_autofree gchar *customer = company_name(db, number(invoice, "company-id"), org);
		g_autofree gchar *title = NULL;
		VentureMoneyCalendarEvent *event;
		if (due == NULL)
			continue;
		balance = venture_settlement_service_invoice_balance(settlement, venture_entity_get_id(invoice), NULL, error);
		if (balance == NULL)
			return FALSE;
		if (venture_money_is_zero(balance) || venture_money_is_negative(balance))
			continue;
		title = g_strdup_printf("Invoice %s %s", number_text ? number_text : "", customer ? customer : "");
		event = dated_event("invoice", due, today, range, title, balance, "in", "invoice", venture_entity_get_id(invoice));
		if (event == NULL)
			continue;
		g_object_set(event, "counterparty", customer, "counterparty-id", number(invoice, "company-id"),
			"venture-id", number(invoice, "venture-id"), NULL);
		g_ptr_array_add(events, event);
	}
	return TRUE;
}

DEFINE_SOURCE(InvoiceSource, invoice_source, "invoice", "receivables", invoices_expand)

/* --- Dunning steps --------------------------------------------------------------------- */

/* The invoice's own policy, then its customer's, then the organization
 * default: the same resolution the dunning sweep makes. */
static VentureEntity *
resolve_policy(VentureDatabase *db, VentureEntity *invoice, VentureEntity *company, GError **error)
{
	gint64 org = venture_entity_get_organization_id(invoice);
	gint64 id = number(invoice, "dunning-policy-id");
	g_autoptr(GPtrArray) policies = NULL;
	guint i;
	if (id <= 0 && company != NULL)
		id = number(company, "dunning-policy-id");
	if (id > 0)
		return load(db, VENTURE_TYPE_DUNNING_POLICY, id, org);
	policies = rows_with_status(db, VENTURE_TYPE_DUNNING_POLICY, org, NULL, NULL, error);
	if (policies == NULL)
		return NULL;
	for (i = 0; i < policies->len; i++)
		if (flag(g_ptr_array_index(policies, i), "is-default"))
			return g_object_ref(g_ptr_array_index(policies, i));
	return NULL;
}

static GHashTable *
recorded_steps(VentureDatabase *db, VentureEntity *invoice, VentureEntity *policy, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_DUNNING_EVENT);
	g_autoptr(GPtrArray) rows = NULL;
	GHashTable *steps;
	guint i;
	venture_query_set_organization(query, venture_entity_get_organization_id(invoice));
	venture_query_set_include_deleted(query, TRUE);
	venture_query_add_filter_int(query, "invoice-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(invoice), NULL);
	venture_query_add_filter_int(query, "policy-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(policy), NULL);
	venture_query_set_limit(query, 0);
	rows = venture_database_find(db, query, error);
	if (rows == NULL)
		return NULL;
	steps = g_hash_table_new(g_direct_hash, g_direct_equal);
	for (i = 0; i < rows->len; i++)
		g_hash_table_add(steps, GINT_TO_POINTER((gint)number(g_ptr_array_index(rows, i), "step")));
	return steps;
}

static gboolean
dunning_expand(VentureMoneyCalendarSource *self, VentureContext *context, gint64 org,
	const VentureDateRange *range, GDateTime *today, GPtrArray *events, GError **error)
{
	VentureDatabase *db = venture_context_get_database(context);
	VentureSettlementService *settlement = venture_settlement_service_get(db);
	g_autoptr(GPtrArray) invoices = open_invoices(db, org, error);
	guint i;
	(void)self;
	if (invoices == NULL)
		return FALSE;
	for (i = 0; i < invoices->len; i++)
	{
		VentureEntity *invoice = g_ptr_array_index(invoices, i);
		g_autoptr(VentureEntity) company = load(db, VENTURE_TYPE_COMPANY, number(invoice, "company-id"), org);
		g_autoptr(VentureEntity) policy = NULL;
		g_autoptr(GDateTime) due = when(invoice, "due-at");
		g_autoptr(VentureMoney) balance = NULL;
		g_autoptr(GHashTable) done = NULL;
		g_autoptr(JsonNode) steps_node = NULL;
		g_autofree gchar *workflow = text(invoice, "workflow-state");
		g_autofree gchar *steps_json = NULL;
		g_autofree gchar *number_text = text(invoice, "number");
		g_autofree gchar *customer = company != NULL ? text(company, "name") : NULL;
		g_autofree gchar *policy_name = NULL;
		JsonArray *steps;
		guint step_no;
		if (due == NULL || flag(invoice, "dunning-disabled") || g_strcmp0(workflow, "disputed") == 0)
			continue;
		if (company != NULL && flag(company, "dunning-opt-out"))
			continue;
		policy = resolve_policy(db, invoice, company, error);
		if (policy == NULL)
		{
			if (error != NULL && *error != NULL)
				return FALSE;
			continue;
		}
		steps_json = text(policy, "steps");
		if (venture_string_is_empty(steps_json))
			continue;
		steps_node = venture_json_parse(steps_json, NULL);
		if (steps_node == NULL || !JSON_NODE_HOLDS_ARRAY(steps_node))
			continue;
		balance = venture_settlement_service_invoice_balance(settlement, venture_entity_get_id(invoice), NULL, error);
		if (balance == NULL)
			return FALSE;
		if (venture_money_is_zero(balance) || venture_money_is_negative(balance))
			continue;
		done = recorded_steps(db, invoice, policy, error);
		if (done == NULL)
			return FALSE;
		policy_name = text(policy, "name");
		steps = json_node_get_array(steps_node);
		for (step_no = 1; step_no <= json_array_get_length(steps); step_no++)
		{
			JsonNode *step = json_array_get_element(steps, step_no - 1);
			g_autoptr(GDateTime) at = NULL;
			g_autofree gchar *title = NULL;
			VentureMoneyCalendarEvent *event;
			gint64 offset;
			if (!JSON_NODE_HOLDS_OBJECT(step) || g_hash_table_contains(done, GINT_TO_POINTER((gint)step_no)))
				continue;
			offset = venture_json_object_get_int(json_node_get_object(step), "offset", 0);
			if (offset < -3650 || offset > 3650)
				continue;
			at = g_date_time_add_days(due, (gint)offset);
			title = g_strdup_printf("Reminder %u/%u %s %s (%s)", step_no, json_array_get_length(steps),
				number_text ? number_text : "", customer ? customer : "", policy_name ? policy_name : "");
			event = dated_event("dunning", at, today, range, title, balance, "in", "invoice", venture_entity_get_id(invoice));
			if (event == NULL)
				continue;
			{
				g_autofree gchar *slot = g_strdup_printf("step%u", step_no);
				g_object_set(event, "counts-in-net", FALSE, "slot", slot, "counterparty", customer,
					"counterparty-id", number(invoice, "company-id"), "venture-id", number(invoice, "venture-id"), NULL);
			}
			g_ptr_array_add(events, event);
		}
	}
	return TRUE;
}

DEFINE_SOURCE(DunningSource, dunning_source, "dunning", "dunning", dunning_expand)

/* --- Payroll ------------------------------------------------------------------------------- */

/* A pay run carries no pay date, so its period end is the day the money
 * goes out. Net pay and the liabilities are two events because they are
 * paid to different people, and disbursement clears each separately. */
static gboolean
payroll_expand(VentureMoneyCalendarSource *self, VentureContext *context, gint64 org,
	const VentureDateRange *range, GDateTime *today, GPtrArray *events, GError **error)
{
	VentureDatabase *db = venture_context_get_database(context);
	g_autoptr(GPtrArray) runs = rows_with_status(db, VENTURE_TYPE_PAYROLL_RUN, org, "status", "imported", error);
	guint i;
	(void)self;
	if (runs == NULL)
		return FALSE;
	for (i = 0; i < runs->len; i++)
	{
		VentureEntity *run = g_ptr_array_index(runs, i);
		g_autoptr(GDateTime) period_end = when(run, "period-end");
		g_autoptr(GPtrArray) lines = NULL;
		g_autofree gchar *key = text(run, "run-key");
		g_autofree gchar *currency = text(run, "currency");
		g_autoptr(VentureMoney) net = NULL;
		g_autoptr(VentureMoney) liabilities = NULL;
		guint j;
		if (period_end == NULL)
			continue;
		lines = rows_with_int(db, VENTURE_TYPE_PAYROLL_LINE, org, "run-id", venture_entity_get_id(run), error);
		if (lines == NULL)
			return FALSE;
		net = venture_money_new_zero(currency);
		liabilities = venture_money_new_zero(currency);
		for (j = 0; j < lines->len; j++)
		{
			VentureEntity *line = g_ptr_array_index(lines, j);
			g_autoptr(VentureMoney) line_net = money(line, "net");
			g_autoptr(VentureMoney) line_liabilities = money(line, "liabilities");
			VentureMoney *sum;
			if (line_net != NULL)
			{
				sum = venture_money_add(net, line_net, error);
				if (sum == NULL)
					return FALSE;
				venture_money_free(net);
				net = sum;
			}
			if (line_liabilities != NULL)
			{
				sum = venture_money_add(liabilities, line_liabilities, error);
				if (sum == NULL)
					return FALSE;
				venture_money_free(liabilities);
				liabilities = sum;
			}
		}
		if (!flag(run, "net-disbursed") && !venture_money_is_zero(net))
		{
			g_autofree gchar *title = g_strdup_printf("Payroll %s net pay", key ? key : "");
			VentureMoneyCalendarEvent *event = dated_event("payroll", period_end, today, range, title, net, "out",
				"payroll_run", venture_entity_get_id(run));
			if (event != NULL)
			{
				g_object_set(event, "slot", "net", NULL);
				g_ptr_array_add(events, event);
			}
		}
		if (!flag(run, "tax-disbursed") && !venture_money_is_zero(liabilities))
		{
			g_autofree gchar *title = g_strdup_printf("Payroll %s liabilities", key ? key : "");
			VentureMoneyCalendarEvent *event = dated_event("payroll", period_end, today, range, title, liabilities, "out",
				"payroll_run", venture_entity_get_id(run));
			if (event != NULL)
			{
				g_object_set(event, "slot", "liabilities", NULL);
				g_ptr_array_add(events, event);
			}
		}
	}
	return TRUE;
}

DEFINE_SOURCE(PayrollSource, payroll_source, "payroll", "payroll", payroll_expand)

/* --- Tax filings ------------------------------------------------------------------------------ */

/* A pack records no filing deadline; the end of the period it covers is
 * when it becomes due. Once submitted it has left the calendar. */
static gboolean
tax_expand(VentureMoneyCalendarSource *self, VentureContext *context, gint64 org,
	const VentureDateRange *range, GDateTime *today, GPtrArray *events, GError **error)
{
	static const gchar *const statuses[] = { "draft", "reviewed", NULL };
	VentureDatabase *db = venture_context_get_database(context);
	guint s, i;
	(void)self;
	for (s = 0; statuses[s] != NULL; s++)
	{
		g_autoptr(GPtrArray) filings = rows_with_status(db, VENTURE_TYPE_TAX_FILING, org, "status", statuses[s], error);
		if (filings == NULL)
			return FALSE;
		for (i = 0; i < filings->len; i++)
		{
			VentureEntity *filing = g_ptr_array_index(filings, i);
			g_autoptr(GDateTime) period_end = when(filing, "period-end");
			g_autoptr(VentureMoney) tax = money(filing, "tax");
			g_autofree gchar *name = text(filing, "name");
			g_autofree gchar *jurisdiction = text(filing, "jurisdiction");
			g_autofree gchar *title = NULL;
			VentureMoneyCalendarEvent *event;
			if (period_end == NULL || tax == NULL)
				continue;
			title = g_strdup_printf("Tax filing %s %s", jurisdiction ? jurisdiction : "", name ? name : "");
			event = dated_event("tax", period_end, today, range, title, tax, "out", "tax_filing", venture_entity_get_id(filing));
			if (event != NULL)
				g_ptr_array_add(events, event);
		}
	}
	return TRUE;
}

DEFINE_SOURCE(TaxSource, tax_source, "tax", "tax_filing", tax_expand)

/* --- Registration ---------------------------------------------------------------------------------- */

void
venture_money_calendar_add_builtin_sources(VentureMoneyCalendarExpander *expander)
{
	GType types[] = { recurring_source_get_type(), bill_source_get_type(), invoice_source_get_type(),
		dunning_source_get_type(), payroll_source_get_type(), tax_source_get_type() };
	guint i;
	g_return_if_fail(VENTURE_IS_MONEY_CALENDAR_EXPANDER(expander));
	for (i = 0; i < G_N_ELEMENTS(types); i++)
	{
		g_autoptr(GObject) source = g_object_new(types[i], NULL);
		venture_money_calendar_expander_add(expander, VENTURE_MONEY_CALENDAR_SOURCE(source));
	}
}
