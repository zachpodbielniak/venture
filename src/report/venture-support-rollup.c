/*
 * venture-support-rollup.c - Support cost and ticket volume per customer
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The support report says how the desk is doing, per person holding
 * tickets. This one turns the same tickets round to face the customer: one
 * row per company with what they raised, what is still open, what was
 * promised and missed, how long they waited, what they thought of it, and
 * what it cost. Cost is agent minutes from the worklogs at an hourly rate,
 * and a flat rate per ticket that had no minutes logged, so a desk that
 * logs time and a desk that does not both get a number.
 *
 * Money is VentureMoney throughout. Hours are stored as a double by the
 * ticket module; each worklog is rounded once to whole minutes and from
 * then on every step is integer arithmetic, one rounding per row.
 */
#include "venture.h"

#include <math.h>
#include <string.h>

/* --- The shape ------------------------------------------------------------ */

/* One row: a company, or a product when grouped that way. Medians are
 * kept as the samples they are taken from, because a median cannot be
 * accumulated. */
typedef struct
{
	gchar		*name;
	gint64		 tickets;
	gint64		 closed;
	gint64		 open;
	gint64		 breaches;
	gint64		 minutes;
	gint64		 unlogged;
	gint64		 rated;
	gint64		 good;
	GArray		*response;
	GArray		*resolution;
	gint64		 response_median;
	gint64		 resolution_median;
	VentureMoney	*cost;
} RollupRow;

static void
rollup_row_free(gpointer data)
{
	RollupRow *row = data;

	if (NULL == row)
		return;

	g_free(row->name);
	g_clear_pointer(&row->response, g_array_unref);
	g_clear_pointer(&row->resolution, g_array_unref);
	g_clear_pointer(&row->cost, venture_money_free);
	g_free(row);
}

static RollupRow *
rollup_row_new(const gchar *name)
{
	RollupRow *row;

	row = g_new0(RollupRow, 1);
	row->name = g_strdup(name);
	row->response = g_array_new(FALSE, FALSE, sizeof(gint64));
	row->resolution = g_array_new(FALSE, FALSE, sizeof(gint64));

	return row;
}

/* What the options asked for, checked once. */
typedef struct
{
	gint64		 organization_id;
	gint64		 venture_id;
	gint64		 company;
	const gchar	*product;
	gint64		 min_tickets;
	gboolean	 by_product;
	const gchar	*sort_key;
	gboolean	 sort_descending;
} RollupOptions;

/* The two rates, already in the book currency, or absent. */
typedef struct
{
	gchar		*currency;
	VentureMoney	*hourly;
	VentureMoney	*per_ticket;
	gboolean	 hourly_is_worklog_rate;
	gchar		*foreign_rate;
} RollupRates;

static void
rollup_rates_clear(RollupRates *rates)
{
	g_clear_pointer(&rates->currency, g_free);
	g_clear_pointer(&rates->hourly, venture_money_free);
	g_clear_pointer(&rates->per_ticket, venture_money_free);
	g_clear_pointer(&rates->foreign_rate, g_free);
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(RollupRates, rollup_rates_clear)

typedef struct
{
	const gchar			*key;
	const gchar			*label;
	VentureReportColumnKind		 kind;
} RollupColumn;

/* The first column is the group: "company", or "product" when grouped by
 * product. Every key here is a column a caller may sort by. */
static const RollupColumn rollup_columns[] = {
	{ "company", "Company", VENTURE_REPORT_COLUMN_TEXT },
	{ "tickets", "Tickets", VENTURE_REPORT_COLUMN_NUMBER },
	{ "closed", "Closed", VENTURE_REPORT_COLUMN_NUMBER },
	{ "open", "Open now", VENTURE_REPORT_COLUMN_NUMBER },
	{ "breaches", "Service levels missed", VENTURE_REPORT_COLUMN_NUMBER },
	{ "response", "Median reply minutes", VENTURE_REPORT_COLUMN_NUMBER },
	{ "resolution", "Median resolve minutes", VENTURE_REPORT_COLUMN_NUMBER },
	{ "csat", "Satisfaction", VENTURE_REPORT_COLUMN_PERCENT },
	{ "minutes", "Agent minutes", VENTURE_REPORT_COLUMN_NUMBER },
	{ "support_cost", "Support cost", VENTURE_REPORT_COLUMN_MONEY }
};

/* The options this report reads. organization_id, venture_id, as_of,
 * currency and dimension are the scope every report is handed by the
 * home page, saved reports and the report pack runner. */
static const gchar *const rollup_known_options[] = {
	"organization_id", "venture_id", "as_of", "currency", "dimension",
	"sort", "min_tickets", "company", "product", "group_by", NULL
};

/* --- Options -------------------------------------------------------------- */

static const gchar *
rollup_group_key(const RollupOptions *options)
{
	return options->by_product ? "product" : "company";
}

/* Whether @key names a column of this report, as it is grouped. */
static gboolean
rollup_column_exists(const RollupOptions *options, const gchar *key)
{
	gsize i;

	if (0 == g_strcmp0(key, rollup_group_key(options)))
		return TRUE;

	for (i = 1; i < G_N_ELEMENTS(rollup_columns); i++)
		if (0 == g_strcmp0(key, rollup_columns[i].key))
			return TRUE;

	return FALSE;
}

static gchar *
rollup_column_list(const RollupOptions *options)
{
	GString *list = g_string_new(rollup_group_key(options));
	gsize i;

	for (i = 1; i < G_N_ELEMENTS(rollup_columns); i++)
		g_string_append_printf(list, ", %s", rollup_columns[i].key);

	return g_string_free(list, FALSE);
}

/* An integer option that may arrive as a JSON number or as the text a
 * query string carries. Anything else is refused by name. */
static gboolean
rollup_option_int(
	JsonObject	 *options,
	const gchar	 *name,
	gint64		  fallback,
	gint64		 *out_value,
	GError		**error
){
	JsonNode *node;

	*out_value = fallback;

	if ((NULL == options) || !json_object_has_member(options, name))
		return TRUE;

	node = json_object_get_member(options, name);

	if (JSON_NODE_HOLDS_VALUE(node) &&
	    (G_TYPE_STRING == json_node_get_value_type(node)))
	{
		const gchar *text = json_node_get_string(node);
		gchar *end = NULL;

		if (!venture_string_is_empty(text))
		{
			*out_value = g_ascii_strtoll(text, &end, 10);

			if ((NULL != end) && ('\0' == *end))
				return TRUE;
		}
	}
	else if (JSON_NODE_HOLDS_VALUE(node) &&
	         (G_TYPE_INT64 == json_node_get_value_type(node)))
	{
		*out_value = json_node_get_int(node);
		return TRUE;
	}
	else if (JSON_NODE_HOLDS_VALUE(node) &&
	         (G_TYPE_DOUBLE == json_node_get_value_type(node)) &&
	         (json_node_get_double(node) == (gdouble)(gint64)json_node_get_double(node)))
	{
		*out_value = (gint64)json_node_get_double(node);
		return TRUE;
	}

	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
	            "support_rollup: %s must be a whole number", name);
	return FALSE;
}

static gboolean
rollup_options_parse(
	VentureContext	 *context,
	JsonObject	 *options,
	RollupOptions	 *parsed,
	GError		**error
){
	const gchar *sort;
	const gchar *group_by;

	memset(parsed, 0, sizeof(*parsed));

	if (NULL != options)
	{
		GList *members;
		GList *l;

		members = json_object_get_members(options);

		for (l = members; NULL != l; l = l->next)
		{
			if (!g_strv_contains(rollup_known_options, l->data))
			{
				g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
				            "support_rollup does not take an option called "
				            "\"%s\"; it takes sort, min_tickets, company, "
				            "product and group_by",
				            (const gchar *)l->data);
				g_list_free(members);
				return FALSE;
			}
		}

		g_list_free(members);
	}

	if (!rollup_option_int(options, "organization_id", 0,
	                       &parsed->organization_id, error) ||
	    !rollup_option_int(options, "venture_id", 0, &parsed->venture_id,
	                       error) ||
	    !rollup_option_int(options, "company", 0, &parsed->company, error) ||
	    !rollup_option_int(options, "min_tickets", 0, &parsed->min_tickets,
	                       error))
		return FALSE;

	if (parsed->organization_id <= 0)
		parsed->organization_id =
			venture_context_get_default_organization_id(context);

	if ((NULL != options) && json_object_has_member(options, "company") &&
	    (parsed->company <= 0))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "support_rollup: company must be a company id");
		return FALSE;
	}

	if (parsed->min_tickets < 0)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "support_rollup: min_tickets cannot be negative");
		return FALSE;
	}

	parsed->product = venture_json_object_get_string(options, "product", NULL);

	if (venture_string_is_empty(parsed->product))
		parsed->product = NULL;

	group_by = venture_json_object_get_string(options, "group_by", NULL);

	if (!venture_string_is_empty(group_by))
	{
		if (0 == g_strcmp0(group_by, "product"))
			parsed->by_product = TRUE;
		else if (0 != g_strcmp0(group_by, "company"))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "support_rollup: group_by must be company or product, "
			            "not \"%s\"", group_by);
			return FALSE;
		}
	}

	/* The default order: the customers costing the most first. */
	parsed->sort_key = "support_cost";
	parsed->sort_descending = TRUE;
	sort = venture_json_object_get_string(options, "sort", NULL);

	if (!venture_string_is_empty(sort))
	{
		parsed->sort_descending = ('-' == sort[0]);
		parsed->sort_key = parsed->sort_descending ? sort + 1 : sort;

		if (!rollup_column_exists(parsed, parsed->sort_key))
		{
			g_autofree gchar *columns = rollup_column_list(parsed);

			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "support_rollup has no column called \"%s\"; sort by "
			            "one of %s, with a leading - for descending",
			            parsed->sort_key, columns);
			return FALSE;
		}
	}

	return TRUE;
}

/* --- Reads ---------------------------------------------------------------- */

/* A query over the organisation's rows of @type, hiding what as_of hides. */
static GPtrArray *
rollup_find(
	VentureContext		 *context,
	const RollupOptions	 *parsed,
	JsonObject		 *options,
	GType			  type,
	const gchar		 *date_field,
	VentureDateRange	 *period,
	GError			**error
){
	g_autoptr(VentureQuery) query = NULL;

	query = venture_query_new(type);
	venture_query_set_organization(query, parsed->organization_id);
	venture_query_set_limit(query, 0);

	if ((NULL != date_field) && (NULL != period) &&
	    !venture_query_set_date_range(query, date_field, period, error))
		return NULL;

	if (!venture_period_report_scope(query, options, error))
		return NULL;

	return venture_database_find(venture_context_get_database(context),
	                             query, error);
}

static gint64
rollup_int(VentureEntity *record, const gchar *property)
{
	gint64 value = 0;

	g_object_get(record, property, &value, NULL);

	return value;
}

static gchar *
rollup_string(VentureEntity *record, const gchar *property)
{
	gchar *value = NULL;

	g_object_get(record, property, &value, NULL);

	return value;
}

/* The book currency: the organisation's, else the process default. */
static gchar *
rollup_currency(VentureContext *context, gint64 organization_id)
{
	g_autoptr(VentureEntity) organization = NULL;
	g_autofree gchar *currency = NULL;

	organization = venture_database_get(venture_context_get_database(context),
	                                    VENTURE_TYPE_ORGANIZATION,
	                                    organization_id, NULL);

	if (NULL != organization)
		g_object_get(organization, "default-currency", &currency, NULL);

	if (venture_string_is_empty(currency))
		return g_strdup(venture_money_get_default_currency());

	return g_ascii_strup(currency, -1);
}

/* @units whole units of @currency as money: 45 -> 45.00 USD. */
static VentureMoney *
rollup_money_from_units(gint64 units, const gchar *currency)
{
	gint64 minor = units;
	guint8 exponent = venture_currency_get_exponent(currency);
	guint8 i;

	for (i = 0; i < exponent; i++)
		minor *= 10;

	return venture_money_new_for_currency(minor, currency);
}

/*
 * The rates from the organisation's headline_setting row. An integer
 * support_hourly_rate wins; zero falls back to the worklog hourly_rate the
 * LTV gross margin already prices hours at, so a rate set once serves
 * both. A per-ticket rate of zero means a ticket without minutes costs
 * nothing. With the headline module off there is no settings row to read.
 */
static void
rollup_load_rates(
	VentureContext	*context,
	gint64		 organization_id,
	RollupRates	*rates
){
	g_autoptr(VentureEntity) setting = NULL;
	g_autoptr(VentureMoney) worklog_rate = NULL;
	gint64 hourly = 0;
	gint64 per_ticket = 0;

	rates->currency = rollup_currency(context, organization_id);

	if (!venture_context_module_enabled(context, "headline"))
		return;

	setting = venture_headline_setting_find(
		venture_context_get_database(context), organization_id);

	if (NULL == setting)
		return;

	g_object_get(setting, "support-hourly-rate", &hourly,
	             "support-ticket-rate", &per_ticket,
	             "hourly-rate", &worklog_rate, NULL);

	if (hourly > 0)
		rates->hourly = rollup_money_from_units(hourly, rates->currency);
	else if ((NULL != worklog_rate) && !venture_money_is_zero(worklog_rate))
	{
		if (0 == g_strcmp0(venture_money_get_currency(worklog_rate),
		                   rates->currency))
		{
			rates->hourly = venture_money_copy(worklog_rate);
			rates->hourly_is_worklog_rate = TRUE;
		}
		else
			rates->foreign_rate = venture_money_to_string(worklog_rate);
	}

	if (per_ticket > 0)
		rates->per_ticket = rollup_money_from_units(per_ticket, rates->currency);
}

/*
 * Minutes logged per ticket: every worklog rounded once to whole minutes.
 * Tickets not in @wanted are skipped, so a worklog on last year's ticket
 * never lands in this period's cost.
 */
static GHashTable *
rollup_minutes(
	VentureContext		 *context,
	const RollupOptions	 *parsed,
	JsonObject		 *options,
	GHashTable		 *wanted,
	GError			**error
){
	g_autoptr(GPtrArray) worklogs = NULL;
	GHashTable *minutes;
	guint i;

	worklogs = rollup_find(context, parsed, options, VENTURE_TYPE_WORKLOG,
	                       NULL, NULL, error);

	if (NULL == worklogs)
		return NULL;

	minutes = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free,
	                                g_free);

	for (i = 0; i < worklogs->len; i++)
	{
		VentureEntity *worklog = g_ptr_array_index(worklogs, i);
		gdouble hours = 0.0;
		gint64 ticket_id;
		gint64 *total;
		gint64 logged;

		ticket_id = rollup_int(worklog, "ticket-id");

		if (!g_hash_table_contains(wanted, &ticket_id))
			continue;

		g_object_get(worklog, "hours", &hours, NULL);

		if (hours <= 0.0)
			continue;

		logged = (gint64)llround(hours * 60.0);
		total = g_hash_table_lookup(minutes, &ticket_id);

		if (NULL == total)
		{
			gint64 *key = g_new(gint64, 1);

			total = g_new0(gint64, 1);
			*key = ticket_id;
			g_hash_table_insert(minutes, key, total);
		}

		*total += logged;
	}

	return minutes;
}

/*
 * The product a ticket is about, when anything says: a product-id field on
 * the ticket type (a plugin's, or a later version's -- the core ticket has
 * none) resolved to the product's name, else a custom field called
 * "product" on the ticket. Returns ticket id -> product name.
 */
static GHashTable *
rollup_products(
	VentureContext		 *context,
	const RollupOptions	 *parsed,
	JsonObject		 *options,
	GPtrArray		 *tickets,
	GError			**error
){
	GHashTable *products;
	gboolean has_field = FALSE;
	guint i;

	products = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free,
	                                 g_free);

	if (tickets->len > 0)
	{
		GPtrArray *specs;

		specs = venture_entity_get_field_specs(g_ptr_array_index(tickets, 0));

		for (i = 0; i < specs->len; i++)
			if (0 == g_strcmp0(venture_field_spec_get_name(
			                       g_ptr_array_index(specs, i)), "product-id"))
				has_field = TRUE;

		g_ptr_array_unref(specs);
	}

	/* The products table belongs to the sales module; a ticket type that
	 * points at products while that module is off has nothing to name. */
	if (has_field && venture_context_module_enabled(context, "sales"))
	{
		g_autoptr(GHashTable) names = NULL;
		g_autoptr(GPtrArray) catalogue = NULL;

		catalogue = rollup_find(context, parsed, options, VENTURE_TYPE_PRODUCT,
		                        NULL, NULL, error);

		if (NULL == catalogue)
		{
			g_hash_table_unref(products);
			return NULL;
		}

		names = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free,
		                              g_free);

		for (i = 0; i < catalogue->len; i++)
		{
			VentureEntity *product = g_ptr_array_index(catalogue, i);
			gint64 *key = g_new(gint64, 1);

			*key = venture_entity_get_id(product);
			g_hash_table_insert(names, key, rollup_string(product, "name"));
		}

		for (i = 0; i < tickets->len; i++)
		{
			VentureEntity *ticket = g_ptr_array_index(tickets, i);
			gint64 product_id = rollup_int(ticket, "product-id");
			const gchar *name = g_hash_table_lookup(names, &product_id);

			if ((product_id > 0) && !venture_string_is_empty(name))
			{
				gint64 *key = g_new(gint64, 1);

				*key = venture_entity_get_id(ticket);
				g_hash_table_insert(products, key, g_strdup(name));
			}
		}
	}

	/* A custom field named "product" on tickets. The table only exists
	 * once the module has been on, so it is read only while it is. */
	if (venture_context_module_enabled(context, "custom_fields"))
	{
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(GPtrArray) values = NULL;

		query = venture_query_new(VENTURE_TYPE_CUSTOM_FIELD_VALUE);
		venture_query_set_organization(query, parsed->organization_id);
		venture_query_set_limit(query, 0);

		if (!venture_query_add_filter_string(query, "record-type",
		                                     VENTURE_FILTER_OP_EQ, "ticket",
		                                     error) ||
		    !venture_query_add_filter_string(query, "name",
		                                     VENTURE_FILTER_OP_EQ, "product",
		                                     error))
		{
			g_hash_table_unref(products);
			return NULL;
		}

		values = venture_database_find(venture_context_get_database(context),
		                               query, error);

		if (NULL == values)
		{
			g_hash_table_unref(products);
			return NULL;
		}

		for (i = 0; i < values->len; i++)
		{
			VentureEntity *value = g_ptr_array_index(values, i);
			g_autofree gchar *text = rollup_string(value, "value");
			gint64 record_id = rollup_int(value, "record-id");

			if (venture_string_is_empty(text) ||
			    g_hash_table_contains(products, &record_id))
				continue;

			{
				gint64 *key = g_new(gint64, 1);

				*key = record_id;
				g_hash_table_insert(products, key, g_steal_pointer(&text));
			}
		}
	}

	return products;
}

/* Company id -> name, for the rows' labels. */
static GHashTable *
rollup_company_names(
	VentureContext		 *context,
	const RollupOptions	 *parsed,
	JsonObject		 *options,
	GError			**error
){
	g_autoptr(GPtrArray) companies = NULL;
	GHashTable *names;
	guint i;

	companies = rollup_find(context, parsed, options, VENTURE_TYPE_COMPANY,
	                        NULL, NULL, error);

	if (NULL == companies)
		return NULL;

	names = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);

	for (i = 0; i < companies->len; i++)
	{
		VentureEntity *company = g_ptr_array_index(companies, i);
		gint64 *key = g_new(gint64, 1);

		*key = venture_entity_get_id(company);
		g_hash_table_insert(names, key, venture_entity_get_display_name(company));
	}

	return names;
}

/* --- Arithmetic ----------------------------------------------------------- */

/* Whole minutes from @from to @to, nearest, never negative. */
static gint64
rollup_minutes_between(GDateTime *from, GDateTime *to)
{
	GTimeSpan span = g_date_time_difference(to, from);

	if (span <= 0)
		return 0;

	return (gint64)((span + (G_TIME_SPAN_MINUTE / 2)) / G_TIME_SPAN_MINUTE);
}

static gint
rollup_compare_int64(gconstpointer a, gconstpointer b)
{
	const gint64 *x = a;
	const gint64 *y = b;

	return (*x > *y) - (*x < *y);
}

/* The median of whole minutes: the middle sample, or the mean of the two
 * middle ones rounded half up. -1 when there is no sample. */
static gint64
rollup_median(GArray *samples)
{
	gint64 low;
	gint64 high;

	if (0 == samples->len)
		return -1;

	g_array_sort(samples, rollup_compare_int64);

	if (1 == (samples->len % 2))
		return g_array_index(samples, gint64, samples->len / 2);

	low = g_array_index(samples, gint64, samples->len / 2 - 1);
	high = g_array_index(samples, gint64, samples->len / 2);

	return (low + high + 1) / 2;
}

/*
 * A row's cost: its minutes at the hourly rate, multiplied then divided
 * once, plus the flat rate for each ticket with no minutes. NULL when no
 * rate applies to anything in the row.
 */
static VentureMoney *
rollup_row_cost(const RollupRow *row, const RollupRates *rates)
{
	g_autoptr(VentureMoney) timed = NULL;
	g_autoptr(VentureMoney) flat = NULL;

	if ((NULL == rates->hourly) && (NULL == rates->per_ticket))
		return NULL;

	if (NULL != rates->hourly)
		timed = venture_money_multiply_rational(rates->hourly, row->minutes, 60,
		                                        NULL);

	if (NULL != rates->per_ticket)
		flat = venture_money_multiply_int(rates->per_ticket, row->unlogged, NULL);

	if ((NULL != timed) && (NULL != flat))
		return venture_money_add(timed, flat, NULL);

	if (NULL != timed)
		return g_steal_pointer(&timed);

	if (NULL != flat)
		return g_steal_pointer(&flat);

	return venture_money_new_zero(rates->currency);
}

/* --- Sorting -------------------------------------------------------------- */

typedef struct
{
	const gchar	*key;
	gboolean	 descending;
} RollupSort;

static gint
rollup_compare_rows(gconstpointer a, gconstpointer b, gpointer user_data)
{
	const RollupRow *x = *(RollupRow *const *)a;
	const RollupRow *y = *(RollupRow *const *)b;
	const RollupSort *sort = user_data;
	const gchar *key = sort->key;
	gint order = 0;

	if ((0 == g_strcmp0(key, "company")) || (0 == g_strcmp0(key, "product")))
		order = g_utf8_collate(x->name, y->name);
	else if (0 == g_strcmp0(key, "tickets"))
		order = (x->tickets > y->tickets) - (x->tickets < y->tickets);
	else if (0 == g_strcmp0(key, "closed"))
		order = (x->closed > y->closed) - (x->closed < y->closed);
	else if (0 == g_strcmp0(key, "open"))
		order = (x->open > y->open) - (x->open < y->open);
	else if (0 == g_strcmp0(key, "breaches"))
		order = (x->breaches > y->breaches) - (x->breaches < y->breaches);
	else if (0 == g_strcmp0(key, "minutes"))
		order = (x->minutes > y->minutes) - (x->minutes < y->minutes);
	else if (0 == g_strcmp0(key, "response"))
		order = (x->response_median > y->response_median) -
		        (x->response_median < y->response_median);
	else if (0 == g_strcmp0(key, "resolution"))
		order = (x->resolution_median > y->resolution_median) -
		        (x->resolution_median < y->resolution_median);
	else if (0 == g_strcmp0(key, "csat"))
	{
		/* good over rated, compared as cross products so no double is
		 * involved; an unrated row sorts below every rated one. */
		gint64 p = (x->rated > 0) ? x->good * MAX(y->rated, 1) : -1;
		gint64 q = (y->rated > 0) ? y->good * MAX(x->rated, 1) : -1;

		order = (p > q) - (p < q);
	}
	else if (0 == g_strcmp0(key, "support_cost"))
	{
		gint64 p = (NULL != x->cost) ? venture_money_get_amount(x->cost) : -1;
		gint64 q = (NULL != y->cost) ? venture_money_get_amount(y->cost) : -1;

		order = (p > q) - (p < q);
	}

	if (sort->descending)
		order = -order;

	/* Ties fall back to the name, so the order is stable between runs. */
	if (0 == order)
		order = g_utf8_collate(x->name, y->name);

	return order;
}

/* --- The report ----------------------------------------------------------- */

static VentureReportResult *
venture_report_support_rollup(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(GPtrArray) tickets = NULL;
	g_autoptr(GPtrArray) ordered = NULL;
	g_autoptr(GHashTable) wanted = NULL;
	g_autoptr(GHashTable) minutes = NULL;
	g_autoptr(GHashTable) products = NULL;
	g_autoptr(GHashTable) names = NULL;
	g_autoptr(GHashTable) rows = NULL;
	g_autoptr(GArray) all_response = NULL;
	g_autoptr(GArray) all_resolution = NULL;
	g_autoptr(VentureMoney) total = NULL;
	g_auto(RollupRates) rates = { NULL, NULL, NULL, FALSE, NULL };
	RollupOptions parsed;
	RollupSort sort;
	RollupRow whole;
	guint i;
	gint64 shown = 0;
	gint64 hidden = 0;
	gint64 without_company = 0;
	gint64 unpriced_minutes = 0;
	gboolean costed = FALSE;

	if (!rollup_options_parse(context, options, &parsed, error))
		return NULL;

	tickets = rollup_find(context, &parsed, options, VENTURE_TYPE_TICKET,
	                      "created-at", period, error);

	if (NULL == tickets)
		return NULL;

	wanted = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);

	for (i = 0; i < tickets->len; i++)
	{
		gint64 *key = g_new(gint64, 1);

		*key = venture_entity_get_id(g_ptr_array_index(tickets, i));
		g_hash_table_add(wanted, key);
	}

	minutes = rollup_minutes(context, &parsed, options, wanted, error);

	if (NULL == minutes)
		return NULL;

	products = rollup_products(context, &parsed, options, tickets, error);

	if (NULL == products)
		return NULL;

	names = rollup_company_names(context, &parsed, options, error);

	if (NULL == names)
		return NULL;

	rollup_load_rates(context, parsed.organization_id, &rates);
	costed = (NULL != rates.hourly) || (NULL != rates.per_ticket);

	rows = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                             rollup_row_free);
	memset(&whole, 0, sizeof(whole));
	all_response = g_array_new(FALSE, FALSE, sizeof(gint64));
	all_resolution = g_array_new(FALSE, FALSE, sizeof(gint64));
	whole.response = all_response;
	whole.resolution = all_resolution;

	for (i = 0; i < tickets->len; i++)
	{
		g_autoptr(GDateTime) responded_at = NULL;
		g_autoptr(GDateTime) resolved_at = NULL;
		g_autofree gchar *group = NULL;
		VentureEntity *ticket = g_ptr_array_index(tickets, i);
		VentureTicketStatus status;
		VentureSatisfaction satisfaction;
		GDateTime *created;
		RollupRow *row;
		const gchar *product;
		const gchar *label;
		gboolean breached = FALSE;
		gboolean open;
		gint64 ticket_id;
		gint64 company_id;
		gint64 *logged;

		ticket_id = venture_entity_get_id(ticket);
		company_id = rollup_int(ticket, "company-id");

		if ((0 != parsed.venture_id) &&
		    (rollup_int(ticket, "venture-id") != parsed.venture_id))
			continue;

		if ((0 != parsed.company) && (company_id != parsed.company))
			continue;

		product = g_hash_table_lookup(products, &ticket_id);

		if ((NULL != parsed.product) &&
		    (0 != g_strcmp0(product, parsed.product)))
			continue;

		g_object_get(ticket, "status", &status, "sla-breached", &breached,
		             "first-responded-at", &responded_at,
		             "resolved-at", &resolved_at,
		             "satisfaction", &satisfaction, NULL);
		created = venture_entity_get_created_at(ticket);

		if (parsed.by_product)
		{
			label = (NULL != product) ? product : "(no product)";
			group = g_strdup(label);
			row = g_hash_table_lookup(rows, group);

			if (NULL == row)
			{
				row = rollup_row_new(label);
				g_hash_table_insert(rows, g_strdup(group), row);
			}
		}
		else
		{
			const gchar *name = g_hash_table_lookup(names, &company_id);

			if (company_id <= 0)
			{
				label = "(no company)";
				without_company++;
			}
			else if (NULL != name)
				label = name;
			else
				label = NULL;

			group = g_strdup_printf("%" G_GINT64_FORMAT, MAX(company_id, 0));
			row = g_hash_table_lookup(rows, group);

			if (NULL == row)
			{
				g_autofree gchar *fallback = NULL;

				if (NULL == label)
				{
					fallback = g_strdup_printf("company #%" G_GINT64_FORMAT,
					                           company_id);
					label = fallback;
				}

				row = rollup_row_new(label);
				g_hash_table_insert(rows, g_strdup(group), row);
			}
		}

		open = (VENTURE_TICKET_STATUS_DONE != status) &&
		       (VENTURE_TICKET_STATUS_CANCELLED != status);
		logged = g_hash_table_lookup(minutes, &ticket_id);

		row->tickets++;
		whole.tickets++;

		if (open)
		{
			row->open++;
			whole.open++;
		}
		else
		{
			row->closed++;
			whole.closed++;
		}

		if (breached)
		{
			row->breaches++;
			whole.breaches++;
		}

		if ((NULL != logged) && (*logged > 0))
		{
			row->minutes += *logged;
			whole.minutes += *logged;

			if (NULL == rates.hourly)
				unpriced_minutes += *logged;
		}
		else
		{
			row->unlogged++;
			whole.unlogged++;
		}

		if ((NULL != responded_at) && (NULL != created))
		{
			gint64 wait = rollup_minutes_between(created, responded_at);

			g_array_append_val(row->response, wait);
			g_array_append_val(all_response, wait);
		}

		if ((NULL != resolved_at) && (NULL != created))
		{
			gint64 wait = rollup_minutes_between(created, resolved_at);

			g_array_append_val(row->resolution, wait);
			g_array_append_val(all_resolution, wait);
		}

		if (VENTURE_SATISFACTION_UNRATED != satisfaction)
		{
			row->rated++;
			whole.rated++;

			if (VENTURE_SATISFACTION_GOOD == satisfaction)
			{
				row->good++;
				whole.good++;
			}
		}
	}

	/* Cost each row once, and the total from the rows, so the total is
	 * exactly the sum of what the table shows; the medians once too, so
	 * sorting never re-sorts a row's samples. */
	ordered = g_ptr_array_new();
	total = venture_money_new_zero(rates.currency);

	{
		GHashTableIter iter;
		gpointer value;

		g_hash_table_iter_init(&iter, rows);

		while (g_hash_table_iter_next(&iter, NULL, &value))
		{
			RollupRow *row = value;

			row->response_median = rollup_median(row->response);
			row->resolution_median = rollup_median(row->resolution);
			row->cost = rollup_row_cost(row, &rates);

			if (NULL != row->cost)
			{
				VentureMoney *sum = venture_money_add(total, row->cost, NULL);

				if (NULL != sum)
				{
					venture_money_free(total);
					total = sum;
				}
			}

			g_ptr_array_add(ordered, row);
		}
	}

	sort.key = parsed.sort_key;
	sort.descending = parsed.sort_descending;
	g_ptr_array_sort_with_data(ordered, rollup_compare_rows, &sort);

	result = venture_report_result_new(parsed.by_product
	                                   ? "Support rollup by product"
	                                   : "Support rollup", period);

	for (i = 0; i < G_N_ELEMENTS(rollup_columns); i++)
		venture_report_result_add_column(result,
			(0 == i) ? rollup_group_key(&parsed) : rollup_columns[i].key,
			(0 == i) ? (parsed.by_product ? "Product" : "Company")
			         : rollup_columns[i].label,
			rollup_columns[i].kind);

	for (i = 0; i < ordered->len; i++)
	{
		RollupRow *row = g_ptr_array_index(ordered, i);

		if (row->tickets < parsed.min_tickets)
		{
			hidden++;
			continue;
		}

		shown++;
		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, rollup_group_key(&parsed),
		                               row->name);
		venture_report_result_set_number(result, "tickets", (gdouble)row->tickets);
		venture_report_result_set_number(result, "closed", (gdouble)row->closed);
		venture_report_result_set_number(result, "open", (gdouble)row->open);
		venture_report_result_set_number(result, "breaches",
		                                 (gdouble)row->breaches);
		if (row->response_median >= 0)
			venture_report_result_set_number(result, "response",
			                                 (gdouble)row->response_median);

		if (row->resolution_median >= 0)
			venture_report_result_set_number(result, "resolution",
			                                 (gdouble)row->resolution_median);

		if (row->rated > 0)
			venture_report_result_set_number(result, "csat",
				(gdouble)row->good / (gdouble)row->rated);

		venture_report_result_set_number(result, "minutes", (gdouble)row->minutes);

		if (NULL != row->cost)
			venture_report_result_set_money(result, "support_cost", row->cost);
	}

	/* The figures above the table: the whole scope, whatever min_tickets
	 * hid, so hiding small customers never changes the total. */
	{
		VentureMetric *metric;
		gint64 median;

		venture_report_result_add_metric(result,
			venture_metric_new_count(parsed.by_product ? "products" : "companies",
			                         parsed.by_product ? "Products" : "Companies",
			                         shown));
		venture_report_result_add_metric(result,
			venture_metric_new_count("tickets", "Tickets raised", whole.tickets));
		venture_report_result_add_metric(result,
			venture_metric_new_count("closed", "Closed", whole.closed));
		metric = venture_metric_new_count("open", "Open now", whole.open);
		venture_metric_set_higher_is_better(metric, FALSE);
		venture_report_result_add_metric(result, metric);
		metric = venture_metric_new_count("breaches", "Service levels missed",
		                                  whole.breaches);
		venture_metric_set_higher_is_better(metric, FALSE);
		venture_report_result_add_metric(result, metric);

		median = rollup_median(all_response);
		metric = (median >= 0)
			? venture_metric_new_number("response", "Median reply minutes",
			                            (gdouble)median)
			: venture_metric_new_text("response", "Median reply minutes", "n/a");
		venture_metric_set_higher_is_better(metric, FALSE);
		venture_report_result_add_metric(result, metric);

		median = rollup_median(all_resolution);
		metric = (median >= 0)
			? venture_metric_new_number("resolution", "Median resolve minutes",
			                            (gdouble)median)
			: venture_metric_new_text("resolution", "Median resolve minutes",
			                          "n/a");
		venture_metric_set_higher_is_better(metric, FALSE);
		venture_report_result_add_metric(result, metric);

		if (whole.rated > 0)
			venture_report_result_add_metric(result,
				venture_metric_new_ratio("csat", "Satisfaction",
					(gdouble)whole.good / (gdouble)whole.rated));
		else
			venture_report_result_add_metric(result,
				venture_metric_new_text("csat", "Satisfaction", "n/a"));

		venture_report_result_add_metric(result,
			venture_metric_new_count("minutes", "Agent minutes", whole.minutes));

		if (costed)
			metric = venture_metric_new_money("support_cost", "Support cost", total);
		else
			metric = venture_metric_new_text("support_cost", "Support cost", "n/a");

		venture_metric_set_higher_is_better(metric, FALSE);
		venture_report_result_add_metric(result, metric);
	}

	if (!costed)
		venture_report_result_append_note(result,
			"No support rate is configured, so nothing is priced. Set "
			"support_hourly_rate (per agent hour, whole units of the book "
			"currency) or support_ticket_rate (per ticket without logged "
			"minutes) on the organisation's headline_setting; "
			"support_hourly_rate of zero uses the worklog hourly_rate.");
	else if ((NULL == rates.hourly) && (unpriced_minutes > 0))
	{
		g_autofree gchar *note = g_strdup_printf(
			"%" G_GINT64_FORMAT " logged agent minutes cost nothing because no "
			"hourly rate is configured; only tickets without minutes are "
			"priced, at the per-ticket rate.", unpriced_minutes);

		venture_report_result_append_note(result, note);
	}
	else if ((NULL == rates.per_ticket) && (whole.unlogged > 0))
	{
		g_autofree gchar *note = g_strdup_printf(
			"%" G_GINT64_FORMAT " tickets have no logged minutes and cost "
			"nothing; set support_ticket_rate to price them at a flat rate.",
			whole.unlogged);

		venture_report_result_append_note(result, note);
	}

	if (NULL != rates.foreign_rate)
	{
		g_autofree gchar *note = g_strdup_printf(
			"The worklog hourly rate is %s, not the book currency %s, so it "
			"was not used; set support_hourly_rate in the book currency.",
			rates.foreign_rate, rates.currency);

		venture_report_result_append_note(result, note);
	}

	if (rates.hourly_is_worklog_rate)
		venture_report_result_append_note(result,
			"Agent minutes are priced at the worklog hourly_rate, the rate "
			"the LTV gross margin uses; support_hourly_rate is zero.");

	if (without_company > 0)
	{
		g_autofree gchar *note = g_strdup_printf(
			"%" G_GINT64_FORMAT " tickets name no company and are counted "
			"under (no company), so the total is the whole desk.",
			without_company);

		venture_report_result_append_note(result, note);
	}

	if (hidden > 0)
	{
		g_autofree gchar *note = g_strdup_printf(
			"%" G_GINT64_FORMAT " %s with fewer than %" G_GINT64_FORMAT
			" tickets are not listed; the figures above the table still "
			"include them.", hidden,
			parsed.by_product ? "products" : "companies", parsed.min_tickets);

		venture_report_result_append_note(result, note);
	}

	if (parsed.by_product && (0 == g_hash_table_size(products)) &&
	    (tickets->len > 0))
		venture_report_result_append_note(result,
			"No ticket names a product: give tickets a custom field called "
			"product, or a product_id field, to break support down by "
			"product.");

	return g_steal_pointer(&result);
}

/* --- The shared figure ---------------------------------------------------- */

VentureMetric *
venture_support_rollup_cost(
	VentureContext		 *context,
	JsonObject		 *options,
	VentureDateRange	 *period,
	GError			**error
){
	g_autoptr(VentureReportResult) result = NULL;
	GPtrArray *metrics;
	guint i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	g_return_val_if_fail(NULL != period, NULL);
	g_return_val_if_fail((NULL == error) || (NULL == *error), NULL);

	result = venture_report_support_rollup(context, period, options, error);

	if (NULL == result)
		return NULL;

	metrics = venture_report_result_get_metrics(result);

	for (i = 0; i < metrics->len; i++)
	{
		VentureMetric *metric = g_ptr_array_index(metrics, i);

		if (0 == g_strcmp0(venture_metric_get_key(metric), "support_cost"))
			return venture_metric_copy(metric);
	}

	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_FAILED,
	                    "The support rollup produced no support_cost figure");
	return NULL;
}

void
venture_support_rollup_register_reports(VentureReportRegistry *registry)
{
	g_return_if_fail(VENTURE_IS_REPORT_REGISTRY(registry));

	venture_report_registry_add(registry,
		VENTURE_REPORT(venture_func_report_new("support_rollup",
			"Support rollup",
			"Support cost and ticket volume per customer: tickets raised, "
			"closed and open, service levels missed, median minutes to the "
			"first reply and to resolution, satisfaction, agent minutes and "
			"what they cost; group_by=product breaks the same figures down "
			"by product",
			venture_report_support_rollup)));
}
