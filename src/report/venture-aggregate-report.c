/*
 * venture-aggregate-report.c - Sum, count, average, minimum and maximum
 *                              over any record type
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The report is a plan and a loop. The plan is the options, checked
 * against the field table before a row is read: the type exists and may be
 * read, every named field exists and is not sensitive, the aggregate makes
 * sense for the measure. The loop reads the matching rows once and drops
 * each into a cell keyed by its bucket, its group values and -- for money
 * -- its currency, so two currencies are two rows and never one sum.
 *
 * Everything is read through the ordinary query, which is what carries the
 * organisation, the caller's access scope and the module mask; nothing
 * here writes SQL. That is also why it cannot GROUP BY: the query layer has
 * no aggregation, on purpose, because the arithmetic that matters here has
 * to be exact and currency-aware.
 */

#include "venture.h"

#include <string.h>

/* The separator between the parts of a cell's key. A unit separator
 * cannot occur in a nick, an id or an ISO date, and a label never makes
 * it into a key. */
#define AGGREGATE_KEY_SEPARATOR "\x1f"

/* What a missing group value reads as, in every door. */
#define AGGREGATE_NONE_LABEL "(none)"

typedef enum
{
	AGGREGATE_BUCKET_NONE = 0,
	AGGREGATE_BUCKET_DAY,
	AGGREGATE_BUCKET_WEEK,
	AGGREGATE_BUCKET_MONTH,
	AGGREGATE_BUCKET_QUARTER,
	AGGREGATE_BUCKET_YEAR
} AggregateBucket;

static const struct
{
	const gchar	*name;
	const gchar	*label;
} aggregate_buckets[] = {
	{ "", "" },
	{ "day", "Day" },
	{ "week", "Week" },
	{ "month", "Month" },
	{ "quarter", "Quarter" },
	{ "year", "Year" }
};

/*
 * One group_by. A dotted name ("product_id.category_id") follows a
 * reference once and groups by a field of what it points at; @hop is that
 * reference, NULL for a plain field.
 */
typedef struct
{
	gchar			*name;
	gchar			*column;
	gchar			*label;
	VentureFieldSpec	*hop;
	gboolean		 hop_custom;
	gchar			*hop_type;
	VentureFieldSpec	*spec;
	gboolean		 custom;
	gchar			*owner_type;
} AggregateGroup;

typedef struct
{
	VentureContext		*context;
	VentureDatabase		*database;
	gchar			*entity_name;
	GType			 type;
	gint64			 organization_id;

	/* NULL when the measure is the records themselves. */
	VentureFieldSpec	*measure;
	gboolean		 measure_custom;
	VentureAggregate	 aggregate;

	AggregateGroup		 groups[VENTURE_AGGREGATE_MAX_GROUPS];
	guint			 n_groups;
	gint			 category_depth;

	VentureFieldSpec	*date_field;
	AggregateBucket		 bucket;

	gint64			 per_seconds;
	const gchar		*per_name;

	/* Referenced records and their labels, fetched once each: a month
	 * of sales names the same few products over and over. */
	GHashTable		*targets;
	GHashTable		*labels;

	guint			 absent;
	guint			 unreadable;
} AggregatePlan;

/*
 * One row of the answer, before it is written out.
 */
typedef struct
{
	gchar		**keys;
	gchar		**labels;
	gchar		 *bucket_key;
	gchar		 *bucket_label;
	GDateTime	 *bucket_start;
	GDateTime	 *bucket_end;
	gchar		 *currency;

	guint		  count;
	VentureMoney	 *money_sum;
	VentureMoney	 *money_min;
	VentureMoney	 *money_max;
	gint64		  int_sum;
	gint64		  int_min;
	gint64		  int_max;
	gdouble		  dbl_sum;
	gdouble		  dbl_min;
	gdouble		  dbl_max;
	GHashTable	 *distinct;
} AggregateCell;

/* ==========================================================================
 * Housekeeping
 * ========================================================================== */

void
venture_aggregate_total_free(VentureAggregateTotal *self)
{
	if (NULL == self)
		return;

	g_free(self->currency);
	g_clear_pointer(&self->money, venture_money_free);
	g_free(self);
}

static void
aggregate_group_clear(AggregateGroup *group)
{
	g_clear_pointer(&group->name, g_free);
	g_clear_pointer(&group->column, g_free);
	g_clear_pointer(&group->label, g_free);
	g_clear_pointer(&group->hop, venture_field_spec_free);
	g_clear_pointer(&group->hop_type, g_free);
	g_clear_pointer(&group->spec, venture_field_spec_free);
	g_clear_pointer(&group->owner_type, g_free);
}

static void
aggregate_plan_free(AggregatePlan *plan)
{
	guint i;

	if (NULL == plan)
		return;

	for (i = 0; i < plan->n_groups; i++)
		aggregate_group_clear(&plan->groups[i]);

	g_free(plan->entity_name);
	g_clear_pointer(&plan->measure, venture_field_spec_free);
	g_clear_pointer(&plan->date_field, venture_field_spec_free);
	g_clear_pointer(&plan->targets, g_hash_table_unref);
	g_clear_pointer(&plan->labels, g_hash_table_unref);
	g_free(plan);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AggregatePlan, aggregate_plan_free)

/* A cached target, which is NULL when it could not be read. */
static void
aggregate_target_free(gpointer data)
{
	if (NULL != data)
		g_object_unref(data);
}

static void
aggregate_cell_free(gpointer data)
{
	AggregateCell *cell;

	cell = data;

	g_strfreev(cell->keys);
	g_strfreev(cell->labels);
	g_free(cell->bucket_key);
	g_free(cell->bucket_label);
	g_clear_pointer(&cell->bucket_start, g_date_time_unref);
	g_clear_pointer(&cell->bucket_end, g_date_time_unref);
	g_free(cell->currency);
	g_clear_pointer(&cell->money_sum, venture_money_free);
	g_clear_pointer(&cell->money_min, venture_money_free);
	g_clear_pointer(&cell->money_max, venture_money_free);
	g_clear_pointer(&cell->distinct, g_hash_table_unref);
	g_free(cell);
}

/*
 * "sale_price" or "sale-price" to "Sale price", for a custom field, which
 * has a name and no label.
 */
static gchar *
aggregate_humanise(const gchar *name)
{
	gchar *label;
	gchar *cursor;

	label = g_strdup((NULL != name) ? name : "");

	for (cursor = label; '\0' != *cursor; cursor++)
	{
		if (('_' == *cursor) || ('-' == *cursor))
			*cursor = ' ';
	}

	if (g_ascii_islower(label[0]))
		label[0] = g_ascii_toupper(label[0]);

	return label;
}

/*
 * The wire spelling of a property name, which is what a person typed and
 * what an error should quote back.
 */
static gchar *
aggregate_wire_name(const gchar *property)
{
	gchar *wire;
	gchar *cursor;

	wire = g_strdup(property);

	for (cursor = wire; '\0' != *cursor; cursor++)
	{
		if ('-' == *cursor)
			*cursor = '_';
	}

	return wire;
}

/* ==========================================================================
 * Fields
 * ========================================================================== */

VentureFieldSpec *
venture_aggregate_find_field(
	VentureDatabase	 *database,
	gint64		  organization_id,
	const gchar	 *entity_name,
	const gchar	 *name,
	gboolean	 *out_custom,
	GError		**error
){
	g_autoptr(GPtrArray) specs = NULL;
	g_autofree gchar *property = NULL;
	VentureFieldSpec *found;
	VentureEntity *prototype;
	guint i;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	g_return_val_if_fail(NULL != entity_name, NULL);

	if (NULL != out_custom)
		*out_custom = FALSE;

	if (venture_string_is_empty(name))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "A field name is empty");
		return NULL;
	}

	prototype = venture_entity_registry_get_prototype(
		venture_entity_registry_get_default(), entity_name);

	if (NULL == prototype)
	{
		venture_entity_registry_set_unknown_type_error(
			venture_entity_registry_get_default(), entity_name, error);
		return NULL;
	}

	property = venture_entity_column_to_property(name);
	specs = venture_entity_get_field_specs(prototype);
	found = NULL;

	for (i = 0; (NULL != specs) && (i < specs->len); i++)
	{
		VentureFieldSpec *spec;

		spec = g_ptr_array_index(specs, i);

		if (0 == g_strcmp0(venture_field_spec_get_name(spec), property))
		{
			found = spec;
			break;
		}
	}

	if (NULL != found)
	{
		/* A total is a way of reading every value it adds, so a field
		 * that may never reach a response may not be aggregated either
		 * -- nor grouped by, which prints the values outright. */
		if (0 != (venture_field_spec_get_flags(found) &
		          VENTURE_COLUMN_FLAG_SENSITIVE))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED,
			            "%s.%s is sensitive and cannot be aggregated",
			            entity_name, name);
			return NULL;
		}

		return venture_field_spec_copy(found);
	}

	/* The spine's timestamps are on every record but in no field table;
	 * "created this month" is too common a question to refuse. */
	if ((0 == g_strcmp0(property, "created-at")) ||
	    (0 == g_strcmp0(property, "updated-at")))
	{
		return venture_field_spec_new(property,
			(0 == g_strcmp0(property, "created-at")) ? "Created" : "Updated",
			VENTURE_FIELD_KIND_DATETIME);
	}

	/* Custom fields live in the attributes, keyed by the name as it was
	 * defined, so the spelling is matched as given and as wire. */
	{
		g_autoptr(GPtrArray) custom = NULL;
		g_autoptr(GError) local = NULL;
		g_autofree gchar *wire = NULL;

		custom = venture_custom_fields_form_specs(database, organization_id,
		                                          entity_name, NULL, &local);

		if (NULL == custom)
		{
			g_propagate_error(error, g_steal_pointer(&local));
			return NULL;
		}

		wire = aggregate_wire_name(name);

		for (i = 0; i < custom->len; i++)
		{
			VentureFieldSpec *spec;
			const gchar *custom_name;

			spec = g_ptr_array_index(custom, i);
			custom_name = venture_field_spec_get_name(spec);

			if ((0 == g_strcmp0(custom_name, name)) ||
			    (0 == g_strcmp0(custom_name, wire)))
			{
				if (NULL != out_custom)
					*out_custom = TRUE;

				return venture_field_spec_copy(spec);
			}
		}
	}

	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
	            "%s has no field called \"%s\"; venturectl describe %s "
	            "lists them", entity_name, name, entity_name);
	return NULL;
}

gboolean
venture_aggregate_field_is_numeric(const VentureFieldSpec *spec)
{
	VentureFieldKind kind;

	g_return_val_if_fail(NULL != spec, FALSE);

	kind = venture_field_spec_get_kind(spec);

	return (VENTURE_FIELD_KIND_MONEY == kind) ||
	       (VENTURE_FIELD_KIND_INTEGER == kind) ||
	       (VENTURE_FIELD_KIND_DOUBLE == kind);
}

/*
 * Reads a numeric field. Returns 1 with the value in the out parameter the
 * kind uses, 0 when the record carries none (an unset money field, a
 * custom field never filled), and -1 when a custom value no longer parses
 * -- which the caller counts rather than failing the whole report for one
 * record.
 */
static gint
aggregate_read_number(
	VentureEntity		 *row,
	const VentureFieldSpec	 *spec,
	gboolean		  custom,
	VentureMoney		**out_money,
	gint64			 *out_int,
	gdouble			 *out_double
){
	VentureFieldKind kind;

	*out_money = NULL;
	*out_int = 0;
	*out_double = 0.0;
	kind = venture_field_spec_get_kind(spec);

	if (custom)
	{
		const gchar *text;
		gchar *end;

		text = venture_entity_get_attribute(row, venture_field_spec_get_name(spec));

		if (venture_string_is_empty(text))
			return 0;

		if (VENTURE_FIELD_KIND_MONEY == kind)
		{
			/* The custom field validator parsed it with no default
			 * currency, so a stored value names its own. */
			*out_money = venture_money_from_string(text, NULL, NULL);

			return (NULL != *out_money) ? 1 : -1;
		}

		if (VENTURE_FIELD_KIND_INTEGER == kind)
		{
			*out_int = g_ascii_strtoll(text, &end, 10);

			return ((end != text) && ('\0' == *end)) ? 1 : -1;
		}

		*out_double = g_ascii_strtod(text, &end);

		return ((end != text) && ('\0' == *end)) ? 1 : -1;
	}

	{
		g_auto(GValue) value = G_VALUE_INIT;

		if (!venture_entity_get_field(row, venture_field_spec_get_name(spec),
		                              &value))
			return 0;

		if (G_VALUE_HOLDS(&value, VENTURE_TYPE_MONEY))
		{
			const VentureMoney *money;

			money = g_value_get_boxed(&value);

			if (NULL == money)
				return 0;

			*out_money = venture_money_copy(money);
			return 1;
		}

		if (G_VALUE_HOLDS_INT64(&value))
		{
			*out_int = g_value_get_int64(&value);
			return 1;
		}

		if (G_VALUE_HOLDS_INT(&value))
		{
			*out_int = g_value_get_int(&value);
			return 1;
		}

		if (G_VALUE_HOLDS_UINT(&value))
		{
			*out_int = g_value_get_uint(&value);
			return 1;
		}

		if (G_VALUE_HOLDS_DOUBLE(&value))
		{
			*out_double = g_value_get_double(&value);
			return 1;
		}
	}

	return -1;
}

/*
 * Adds @b to @a, refusing rather than wrapping: an integer total that
 * overflowed would be a confident nonsense figure.
 */
static gboolean
aggregate_add_int(
	gint64	 *a,
	gint64	  b
){
	if (((b > 0) && (*a > G_MAXINT64 - b)) ||
	    ((b < 0) && (*a < G_MININT64 - b)))
		return FALSE;

	*a += b;

	return TRUE;
}

/* ==========================================================================
 * Cells
 * ========================================================================== */

/*
 * Folds one value into a cell. Every statistic is kept, not just the one
 * asked for: they cost nothing, and it keeps this one function for every
 * aggregate.
 */
static gboolean
aggregate_cell_add(
	AggregateCell		 *cell,
	VentureFieldKind	  kind,
	const VentureMoney	 *money,
	gint64			  integer,
	gdouble			  number,
	const gchar		 *distinct_key,
	GError			**error
){
	gboolean first;

	first = (0 == cell->count);
	cell->count++;

	if (NULL != distinct_key)
	{
		if (NULL == cell->distinct)
			cell->distinct = g_hash_table_new_full(g_str_hash, g_str_equal,
			                                       g_free, NULL);

		g_hash_table_add(cell->distinct, g_strdup(distinct_key));
	}

	if (NULL != money)
	{
		VentureMoney *next;

		if (first)
		{
			cell->money_sum = venture_money_copy(money);
			cell->money_min = venture_money_copy(money);
			cell->money_max = venture_money_copy(money);
			return TRUE;
		}

		next = venture_money_add(cell->money_sum, money, error);

		if (NULL == next)
		{
			g_prefix_error(error, "A total cannot be added up: ");
			return FALSE;
		}

		venture_money_free(cell->money_sum);
		cell->money_sum = next;

		if (venture_money_compare(money, cell->money_min) < 0)
		{
			venture_money_free(cell->money_min);
			cell->money_min = venture_money_copy(money);
		}

		if (venture_money_compare(money, cell->money_max) > 0)
		{
			venture_money_free(cell->money_max);
			cell->money_max = venture_money_copy(money);
		}

		return TRUE;
	}

	if (VENTURE_FIELD_KIND_INTEGER == kind)
	{
		if (first)
		{
			cell->int_sum = integer;
			cell->int_min = integer;
			cell->int_max = integer;
			return TRUE;
		}

		if (!aggregate_add_int(&cell->int_sum, integer))
		{
			g_set_error_literal(error, VENTURE_ERROR,
			                    VENTURE_ERROR_INVALID_ARGUMENT,
			                    "A total overflows a 64-bit integer; "
			                    "narrow the period or the filter");
			return FALSE;
		}

		cell->int_min = MIN(cell->int_min, integer);
		cell->int_max = MAX(cell->int_max, integer);
		return TRUE;
	}

	if (VENTURE_FIELD_KIND_DOUBLE == kind)
	{
		if (first)
		{
			cell->dbl_sum = number;
			cell->dbl_min = number;
			cell->dbl_max = number;
			return TRUE;
		}

		cell->dbl_sum += number;
		cell->dbl_min = MIN(cell->dbl_min, number);
		cell->dbl_max = MAX(cell->dbl_max, number);
	}

	return TRUE;
}

/*
 * The cell under @key, created with its identity on first sight.
 */
static AggregateCell *
aggregate_cell_lookup(
	GHashTable	 *cells,
	const gchar	 *key,
	gchar		**keys,
	gchar		**labels,
	const gchar	 *bucket_key,
	const gchar	 *bucket_label,
	GDateTime	 *bucket_start,
	GDateTime	 *bucket_end,
	const gchar	 *currency
){
	AggregateCell *cell;

	cell = g_hash_table_lookup(cells, key);

	if (NULL != cell)
		return cell;

	cell = g_new0(AggregateCell, 1);
	cell->keys = g_strdupv(keys);
	cell->labels = g_strdupv(labels);
	cell->bucket_key = g_strdup(bucket_key);
	cell->bucket_label = g_strdup(bucket_label);
	cell->bucket_start = (NULL != bucket_start) ? g_date_time_ref(bucket_start) : NULL;
	cell->bucket_end = (NULL != bucket_end) ? g_date_time_ref(bucket_end) : NULL;
	cell->currency = g_strdup(currency);
	g_hash_table_insert(cells, g_strdup(key), cell);

	return cell;
}

/*
 * Chronological by bucket, then by group label, then by currency: the
 * order a person reads a table in, and the same every run.
 */
static gint
aggregate_cell_compare(
	gconstpointer	a,
	gconstpointer	b
){
	const AggregateCell *left;
	const AggregateCell *right;
	gint order;
	guint i;

	left = *(AggregateCell * const *)a;
	right = *(AggregateCell * const *)b;

	order = g_strcmp0(left->bucket_key, right->bucket_key);

	if (0 != order)
		return order;

	for (i = 0; (NULL != left->labels[i]) && (NULL != right->labels[i]); i++)
	{
		order = g_strcmp0(left->labels[i], right->labels[i]);

		if (0 != order)
			return order;

		order = g_strcmp0(left->keys[i], right->keys[i]);

		if (0 != order)
			return order;
	}

	return g_strcmp0(left->currency, right->currency);
}

GPtrArray *
venture_aggregate_sum(
	GPtrArray		 *rows,
	const VentureFieldSpec	 *spec,
	gboolean		  custom,
	GError			**error
){
	g_autoptr(GHashTable) cells = NULL;
	g_autoptr(GPtrArray) sorted = NULL;
	GPtrArray *totals;
	GHashTableIter iter;
	gpointer value;
	gchar *no_keys[] = { NULL };
	VentureFieldKind kind;
	guint i;

	g_return_val_if_fail(NULL != rows, NULL);
	g_return_val_if_fail(NULL != spec, NULL);

	if (!venture_aggregate_field_is_numeric(spec))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "%s is not a number or an amount, so it cannot be "
		            "summed", venture_field_spec_get_name(spec));
		return NULL;
	}

	kind = venture_field_spec_get_kind(spec);
	cells = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                              aggregate_cell_free);

	for (i = 0; i < rows->len; i++)
	{
		g_autoptr(VentureMoney) money = NULL;
		AggregateCell *cell;
		const gchar *currency;
		gint64 integer;
		gdouble number;

		if (1 != aggregate_read_number(g_ptr_array_index(rows, i), spec, custom,
		                               &money, &integer, &number))
			continue;

		currency = (NULL != money) ? venture_money_get_currency(money) : "";
		cell = aggregate_cell_lookup(cells, currency, no_keys, no_keys, NULL,
		                             NULL, NULL, NULL,
		                             (NULL != money) ? currency : NULL);

		if (!aggregate_cell_add(cell, kind, money, integer, number, NULL,
		                        error))
			return NULL;
	}

	sorted = g_ptr_array_new();
	g_hash_table_iter_init(&iter, cells);

	while (g_hash_table_iter_next(&iter, NULL, &value))
		g_ptr_array_add(sorted, value);

	g_ptr_array_sort(sorted, aggregate_cell_compare);
	totals = g_ptr_array_new_with_free_func(
		(GDestroyNotify)venture_aggregate_total_free);

	/* A number always has a total, even over nothing: zero is the
	 * answer. Money over nothing has no currency to be zero in. */
	if ((0 == sorted->len) && (VENTURE_FIELD_KIND_MONEY != kind))
		g_ptr_array_add(totals, g_new0(VentureAggregateTotal, 1));

	for (i = 0; i < sorted->len; i++)
	{
		AggregateCell *cell;
		VentureAggregateTotal *total;

		cell = g_ptr_array_index(sorted, i);
		total = g_new0(VentureAggregateTotal, 1);
		total->currency = g_strdup(cell->currency);
		total->money = (NULL != cell->money_sum)
			? venture_money_copy(cell->money_sum) : NULL;
		total->number = (VENTURE_FIELD_KIND_INTEGER == kind)
			? (gdouble)cell->int_sum : cell->dbl_sum;
		total->count = cell->count;
		g_ptr_array_add(totals, total);
	}

	return totals;
}

/* ==========================================================================
 * Group values
 * ========================================================================== */

/*
 * A referenced record, fetched once. %NULL when it cannot be read: gone,
 * or outside what the caller may see -- either way its name is not ours
 * to print.
 */
static VentureEntity *
aggregate_target(
	AggregatePlan	*plan,
	const gchar	*type_name,
	gint64		 id
){
	g_autofree gchar *key = NULL;
	VentureEntity *target;
	GType type;

	key = g_strdup_printf("%s#%" G_GINT64_FORMAT, type_name, id);

	if (g_hash_table_contains(plan->targets, key))
		return g_hash_table_lookup(plan->targets, key);

	type = venture_entity_registry_lookup_any(
		venture_entity_registry_get_default(), type_name);
	target = (G_TYPE_INVALID != type)
		? venture_database_get(plan->database, type, id, NULL) : NULL;

	g_hash_table_insert(plan->targets, g_steal_pointer(&key), target);

	return target;
}

/*
 * What a reference groups under: its target's display name, or for a tree
 * (category, location) the path -- rolled up to category_depth when one
 * was asked for, so "Materials / Herbs / Rare" and "Materials / Herbs /
 * Common" both land in "Materials / Herbs" at depth 1. The key is the id
 * the row is grouped by, so two targets that share a name stay apart.
 */
static void
aggregate_reference_value(
	AggregatePlan	 *plan,
	const gchar	 *type_name,
	gint64		  id,
	gchar		**out_key,
	gchar		**out_label
){
	g_autofree gchar *cache_key = NULL;
	const gchar *cached;
	GType type;

	if (id <= 0)
	{
		*out_key = g_strdup("");
		*out_label = g_strdup(AGGREGATE_NONE_LABEL);
		return;
	}

	type = venture_entity_registry_lookup_any(
		venture_entity_registry_get_default(), type_name);

	if (((VENTURE_TYPE_CATEGORY == type) || (VENTURE_TYPE_LOCATION == type)) &&
	    (plan->category_depth >= 0))
	{
		gint64 ancestor;

		ancestor = venture_category_ancestor_at_depth(plan->database, type,
			id, (guint)plan->category_depth, NULL);

		/* An unreadable node keeps its own id rather than vanishing
		 * into another group. */
		if (ancestor > 0)
			id = ancestor;
	}

	*out_key = g_strdup_printf("%" G_GINT64_FORMAT, id);
	cache_key = g_strdup_printf("%s#%" G_GINT64_FORMAT, type_name, id);
	cached = g_hash_table_lookup(plan->labels, cache_key);

	if (NULL != cached)
	{
		*out_label = g_strdup(cached);
		return;
	}

	{
		gchar *label = NULL;

		if ((VENTURE_TYPE_CATEGORY == type) || (VENTURE_TYPE_LOCATION == type))
		{
			/* The path is computed on every call and never stored, so
			 * a renamed parent renames the group with no cascade. */
			if (NULL != aggregate_target(plan, type_name, id))
				label = venture_category_path(plan->database, type, id, NULL);
		}
		else
		{
			VentureEntity *target;

			target = aggregate_target(plan, type_name, id);

			if (NULL != target)
				label = venture_entity_get_display_name(target);
		}

		if (venture_string_is_empty(label))
		{
			g_free(label);
			label = g_strdup_printf("#%" G_GINT64_FORMAT, id);
		}

		g_hash_table_insert(plan->labels, g_steal_pointer(&cache_key),
		                    g_strdup(label));
		*out_label = label;
	}
}

/*
 * The id a reference field holds, built-in or custom.
 */
static gint64
aggregate_reference_id(
	VentureEntity		*row,
	const VentureFieldSpec	*spec,
	gboolean		 custom
){
	g_auto(GValue) value = G_VALUE_INIT;

	if (custom)
	{
		const gchar *text;

		text = venture_entity_get_attribute(row, venture_field_spec_get_name(spec));

		return venture_string_is_empty(text) ? 0 : g_ascii_strtoll(text, NULL, 10);
	}

	if (!venture_entity_get_field(row, venture_field_spec_get_name(spec), &value))
		return 0;

	return G_VALUE_HOLDS_INT64(&value) ? g_value_get_int64(&value) : 0;
}

/*
 * A field's value as a key to group by and a label to show. An enum groups
 * by its nick and shows its declared label; a date by the UTC day it is
 * (a stored calendar date is midnight UTC, and bucket is the tool for
 * coarser); a reference as above.
 */
static void
aggregate_describe(
	AggregatePlan		 *plan,
	VentureEntity		 *row,
	const VentureFieldSpec	 *spec,
	gboolean		  custom,
	gchar			**out_key,
	gchar			**out_label
){
	g_auto(GValue) value = G_VALUE_INIT;
	VentureFieldKind kind;
	gchar *text;
	gchar *shown;

	kind = venture_field_spec_get_kind(spec);
	text = NULL;
	shown = NULL;

	if (VENTURE_FIELD_KIND_REFERENCE == kind)
	{
		aggregate_reference_value(plan, venture_field_spec_get_reference_type(spec),
			aggregate_reference_id(row, spec, custom), out_key, out_label);
		return;
	}

	if (custom)
	{
		text = g_strdup(venture_entity_get_attribute(row,
			venture_field_spec_get_name(spec)));
	}
	else if (venture_entity_get_field(row, venture_field_spec_get_name(spec),
	                                  &value))
	{
		if (G_VALUE_HOLDS(&value, VENTURE_TYPE_MONEY))
		{
			const VentureMoney *money;

			money = g_value_get_boxed(&value);

			/* The key stays the plain decimal so it groups and parses;
			 * the label is what a person reads, "7g 7s 20c" rather
			 * than "7.0720 GOLD". */
			if (NULL != money)
			{
				text = venture_money_to_string(money);
				shown = venture_money_to_display_string(money, TRUE);
			}
		}
		else if (G_VALUE_HOLDS(&value, G_TYPE_DATE_TIME))
		{
			GDateTime *when;

			when = g_value_get_boxed(&value);
			text = (NULL != when) ? venture_time_to_date_string(when, NULL) : NULL;
		}
		else if (G_VALUE_HOLDS_ENUM(&value))
		{
			text = g_strdup(venture_enum_to_nick(G_VALUE_TYPE(&value),
			                                     g_value_get_enum(&value)));
		}
		else if (G_VALUE_HOLDS_BOOLEAN(&value))
		{
			text = g_strdup(g_value_get_boolean(&value) ? "yes" : "no");
		}
		else if (G_VALUE_HOLDS_INT64(&value))
		{
			text = g_strdup_printf("%" G_GINT64_FORMAT, g_value_get_int64(&value));
		}
		else if (G_VALUE_HOLDS_DOUBLE(&value))
		{
			gchar buffer[G_ASCII_DTOSTR_BUF_SIZE];

			text = g_strdup(g_ascii_dtostr(buffer, sizeof(buffer),
			                               g_value_get_double(&value)));
		}
		else if (G_VALUE_HOLDS_STRING(&value))
		{
			text = g_value_dup_string(&value);
		}
	}

	if (venture_string_is_empty(text))
	{
		g_free(text);
		g_free(shown);
		*out_key = g_strdup("");
		*out_label = g_strdup(AGGREGATE_NONE_LABEL);
		return;
	}

	*out_key = text;

	if (VENTURE_FIELD_KIND_ENUM == kind)
	{
		const gchar *declared;

		declared = venture_field_spec_get_choice_label(spec, text);
		*out_label = (NULL != declared) ? g_strdup(declared) : aggregate_humanise(text);
		return;
	}

	*out_label = (NULL != shown) ? shown : g_strdup(text);
}

/*
 * A group's key and label for one row: follow the hop if there is one,
 * then describe the field on whatever that reached.
 */
static void
aggregate_group_value(
	AggregatePlan	 *plan,
	AggregateGroup	 *group,
	VentureEntity	 *row,
	gchar		**out_key,
	gchar		**out_label
){
	VentureEntity *source;

	source = row;

	if (NULL != group->hop)
	{
		gint64 id;

		id = aggregate_reference_id(row, group->hop, group->hop_custom);

		if (id <= 0)
		{
			*out_key = g_strdup("");
			*out_label = g_strdup(AGGREGATE_NONE_LABEL);
			return;
		}

		source = aggregate_target(plan, group->hop_type, id);

		if (NULL == source)
		{
			/* Unreadable is not the same as none: the row points at
			 * something, and the table says which. */
			*out_key = g_strdup_printf("#%" G_GINT64_FORMAT, id);
			*out_label = g_strdup_printf("%s #%" G_GINT64_FORMAT,
			                             group->hop_type, id);
			return;
		}
	}

	aggregate_describe(plan, source, group->spec, group->custom, out_key,
	                   out_label);
}

/* ==========================================================================
 * Buckets
 * ========================================================================== */

/*
 * The bucket an instant falls in, as its start and end at midnight UTC and
 * a label. UTC and not the configured zone, for the reason every period
 * boundary is UTC: a calendar date is stored as midnight UTC on that day,
 * and in any zone west of UTC local time would put the first of the month
 * in the month before.
 */
static void
aggregate_bucket_of(
	AggregateBucket	  bucket,
	GDateTime	 *when,
	GDateTime	**out_start,
	GDateTime	**out_end,
	gchar		**out_key,
	gchar		**out_label
){
	g_autoptr(GDateTime) utc = NULL;
	g_autoptr(GDateTime) day = NULL;
	GDateTime *start;
	GDateTime *end;
	gint year;
	gint month;
	gint dom;

	utc = g_date_time_to_utc(when);
	g_date_time_get_ymd(utc, &year, &month, &dom);
	day = g_date_time_new_utc(year, month, dom, 0, 0, 0);

	switch (bucket)
	{
	case AGGREGATE_BUCKET_WEEK:
		/* ISO weeks start on Monday. */
		start = g_date_time_add_days(day, 1 - g_date_time_get_day_of_week(day));
		end = g_date_time_add_weeks(start, 1);
		*out_label = g_date_time_format(start, "%G-W%V");
		break;
	case AGGREGATE_BUCKET_MONTH:
		start = g_date_time_new_utc(year, month, 1, 0, 0, 0);
		end = g_date_time_add_months(start, 1);
		*out_label = g_date_time_format(start, "%Y-%m");
		break;
	case AGGREGATE_BUCKET_QUARTER:
		start = g_date_time_new_utc(year, ((month - 1) / 3) * 3 + 1, 1, 0, 0, 0);
		end = g_date_time_add_months(start, 3);
		*out_label = g_strdup_printf("%d-Q%d", year, ((month - 1) / 3) + 1);
		break;
	case AGGREGATE_BUCKET_YEAR:
		start = g_date_time_new_utc(year, 1, 1, 0, 0, 0);
		end = g_date_time_add_years(start, 1);
		*out_label = g_strdup_printf("%d", year);
		break;
	case AGGREGATE_BUCKET_DAY:
	case AGGREGATE_BUCKET_NONE:
	default:
		start = g_date_time_ref(day);
		end = g_date_time_add_days(start, 1);
		*out_label = g_date_time_format(start, "%Y-%m-%d");
		break;
	}

	*out_key = g_date_time_format(start, "%Y-%m-%d");
	*out_start = start;
	*out_end = end;
}

/* ==========================================================================
 * The plan
 * ========================================================================== */

/*
 * Resolves one group_by entry, following a dotted name through one
 * reference.
 */
static gboolean
aggregate_plan_group(
	AggregatePlan	 *plan,
	AggregateGroup	 *group,
	const gchar	 *name,
	GError		**error
){
	g_auto(GStrv) parts = NULL;
	const gchar *field_name;
	const gchar *owner;

	group->name = g_strdup(name);
	parts = g_strsplit(name, ".", -1);
	owner = plan->entity_name;
	field_name = name;

	if (g_strv_length(parts) > 2)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "group_by \"%s\" follows more than one reference; one "
		            "hop (reference.field) is the limit", name);
		return FALSE;
	}

	if (2 == g_strv_length(parts))
	{
		group->hop = venture_aggregate_find_field(plan->database,
			plan->organization_id, plan->entity_name, parts[0],
			&group->hop_custom, error);

		if (NULL == group->hop)
			return FALSE;

		if (VENTURE_FIELD_KIND_REFERENCE != venture_field_spec_get_kind(group->hop))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "group_by \"%s\": %s is not a reference, so there "
			            "is nothing to follow", name, parts[0]);
			return FALSE;
		}

		group->hop_type = g_strdup(venture_field_spec_get_reference_type(group->hop));
		owner = group->hop_type;
		field_name = parts[1];
	}

	group->spec = venture_aggregate_find_field(plan->database,
		plan->organization_id, owner, field_name, &group->custom, error);

	if (NULL == group->spec)
		return FALSE;

	if ((VENTURE_FIELD_KIND_TEXT == venture_field_spec_get_kind(group->spec)) ||
	    (VENTURE_FIELD_KIND_JSON == venture_field_spec_get_kind(group->spec)))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "group_by \"%s\" is long text; every record would be "
		            "its own group", name);
		return FALSE;
	}

	group->owner_type = g_strdup(owner);

	{
		const gchar *spec_label;

		spec_label = venture_field_spec_get_label(group->spec);
		group->label = (group->custom || venture_string_is_empty(spec_label))
			? aggregate_humanise(field_name) : g_strdup(spec_label);

		if (NULL != group->hop)
		{
			g_autofree gchar *hop_label = NULL;
			const gchar *hop_spec_label;

			hop_spec_label = venture_field_spec_get_label(group->hop);
			hop_label = (group->hop_custom || venture_string_is_empty(hop_spec_label))
				? aggregate_humanise(parts[0]) : g_strdup(hop_spec_label);
			g_free(group->label);
			group->label = g_strdup_printf("%s %s", hop_label,
				(group->custom || venture_string_is_empty(spec_label))
					? field_name : spec_label);
		}
	}

	return TRUE;
}

/*
 * Whether any group reads a tree, which is what category_depth needs to
 * mean anything.
 */
static gboolean
aggregate_plan_has_tree_group(AggregatePlan *plan)
{
	guint i;

	for (i = 0; i < plan->n_groups; i++)
	{
		const VentureFieldSpec *spec;
		const gchar *target;

		spec = plan->groups[i].spec;

		if (VENTURE_FIELD_KIND_REFERENCE != venture_field_spec_get_kind(spec))
			continue;

		target = venture_field_spec_get_reference_type(spec);

		if ((0 == g_strcmp0(target, "category")) ||
		    (0 == g_strcmp0(target, "location")))
			return TRUE;
	}

	return FALSE;
}

/*
 * Reads the options into a plan, refusing whatever cannot be answered
 * before any row is read.
 */
static AggregatePlan *
aggregate_plan_new(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	g_autoptr(AggregatePlan) plan = NULL;
	VentureEntityRegistry *registry;
	VentureDataClass classification;
	const gchar *type_name;
	const gchar *measure;
	const gchar *aggregate;
	const gchar *group_by;
	const gchar *date_field;
	const gchar *bucket;
	const gchar *per;

	plan = g_new0(AggregatePlan, 1);
	plan->context = context;
	plan->database = venture_context_get_database(context);
	plan->category_depth = -1;
	plan->targets = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                      aggregate_target_free);
	plan->labels = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

	type_name = (NULL != options) ? venture_json_object_get_string(options, "type", NULL) : NULL;

	if (venture_string_is_empty(type_name))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "The aggregate report needs a type: the record "
		                    "type to read, for example type=sale");
		return NULL;
	}

	/* --- The type: registered, its module on, and business data --- */

	registry = venture_context_get_entity_registry(context);
	plan->type = venture_entity_registry_lookup(registry, type_name);

	if (G_TYPE_INVALID == plan->type)
	{
		venture_entity_registry_set_unknown_type_error(registry, type_name, error);
		return NULL;
	}

	/* Derived from the type's declared authority, not a list: users,
	 * tokens, chat and the inbox are personal, forges and webhooks are
	 * platform machinery, and a total over either is a way of reading
	 * rows this report's callers are not given. A plugin's type is
	 * covered the day it declares itself. */
	classification = venture_data_class_for_type(plan->type);

	if ((VENTURE_DATA_CLASS_TENANT != classification) &&
	    (VENTURE_DATA_CLASS_REFERENCE != classification))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED,
		            "%s is not business data; the aggregate report does not "
		            "read it", type_name);
		return NULL;
	}

	plan->entity_name = g_strdup(venture_entity_get_entity_name(
		venture_entity_registry_get_prototype(registry, type_name)));
	plan->organization_id = venture_json_object_get_int(options, "organization_id", 0);

	if (plan->organization_id <= 0)
		plan->organization_id = venture_context_get_default_organization_id(context);

	/* --- The measure and what is done to it --- */

	measure = venture_json_object_get_string(options, "measure", NULL);
	aggregate = venture_json_object_get_string(options, "aggregate", NULL);

	if (!venture_string_is_empty(measure) && (0 != g_strcmp0(measure, "count")))
	{
		plan->measure = venture_aggregate_find_field(plan->database,
			plan->organization_id, plan->entity_name, measure,
			&plan->measure_custom, error);

		if (NULL == plan->measure)
			return NULL;
	}

	if (venture_string_is_empty(aggregate))
	{
		plan->aggregate = ((NULL != plan->measure) &&
		                   venture_aggregate_field_is_numeric(plan->measure))
			? VENTURE_AGGREGATE_SUM : VENTURE_AGGREGATE_COUNT;
	}
	else
	{
		gint value;

		if (!venture_enum_from_nick(VENTURE_TYPE_AGGREGATE, aggregate, &value) ||
		    (VENTURE_AGGREGATE_NONE == value))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "aggregate \"%s\" is not one of sum, avg, min, max, "
			            "count, count_distinct", aggregate);
			return NULL;
		}

		plan->aggregate = value;
	}

	switch (plan->aggregate)
	{
	case VENTURE_AGGREGATE_SUM:
	case VENTURE_AGGREGATE_AVG:
	case VENTURE_AGGREGATE_MIN:
	case VENTURE_AGGREGATE_MAX:
		if ((NULL == plan->measure) ||
		    !venture_aggregate_field_is_numeric(plan->measure))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "aggregate=%s needs a measure that is money, an "
			            "integer or a number; %s is not",
			            venture_enum_to_nick(VENTURE_TYPE_AGGREGATE,
			                                 (gint)plan->aggregate),
			            (NULL != plan->measure) ? measure : "count");
			return NULL;
		}
		break;
	case VENTURE_AGGREGATE_COUNT_DISTINCT:
		if (NULL == plan->measure)
		{
			g_set_error_literal(error, VENTURE_ERROR,
			                    VENTURE_ERROR_INVALID_ARGUMENT,
			                    "aggregate=count_distinct needs a measure: "
			                    "the field whose distinct values are counted");
			return NULL;
		}
		break;
	case VENTURE_AGGREGATE_COUNT:
	case VENTURE_AGGREGATE_NONE:
	default:
		break;
	}

	/* --- The groups --- */

	group_by = venture_json_object_get_string(options, "group_by", NULL);

	if (!venture_string_is_empty(group_by))
	{
		g_auto(GStrv) names = NULL;
		guint i;

		names = g_strsplit(group_by, ",", -1);

		for (i = 0; NULL != names[i]; i++)
		{
			g_strstrip(names[i]);

			if ('\0' == names[i][0])
				continue;

			if (plan->n_groups == VENTURE_AGGREGATE_MAX_GROUPS)
			{
				g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
				            "group_by takes at most %d fields",
				            VENTURE_AGGREGATE_MAX_GROUPS);
				return NULL;
			}

			/* Counted before the check can fail, so the free walks
			 * whatever the group already holds. */
			plan->n_groups++;

			if (!aggregate_plan_group(plan, &plan->groups[plan->n_groups - 1],
			                          names[i], error))
				return NULL;
		}
	}

	if ((NULL != options) && json_object_has_member(options, "category_depth"))
	{
		gint64 depth;

		depth = venture_json_object_get_int(options, "category_depth", -1);

		if ((depth < 0) || (depth >= VENTURE_CATEGORY_MAX_DEPTH))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "category_depth must be 0 (the top of the tree) to %d",
			            VENTURE_CATEGORY_MAX_DEPTH - 1);
			return NULL;
		}

		if (!aggregate_plan_has_tree_group(plan))
		{
			g_set_error_literal(error, VENTURE_ERROR,
			                    VENTURE_ERROR_INVALID_ARGUMENT,
			                    "category_depth rolls up a group_by that "
			                    "points at a category or a location, and "
			                    "none does");
			return NULL;
		}

		plan->category_depth = (gint)depth;
	}

	/* --- Time --- */

	date_field = venture_json_object_get_string(options, "date_field", NULL);
	bucket = venture_json_object_get_string(options, "bucket", NULL);

	if (!venture_string_is_empty(date_field))
	{
		gboolean custom;
		VentureFieldKind kind;

		plan->date_field = venture_aggregate_find_field(plan->database,
			plan->organization_id, plan->entity_name, date_field, &custom,
			error);

		if (NULL == plan->date_field)
			return NULL;

		kind = venture_field_spec_get_kind(plan->date_field);

		if (((VENTURE_FIELD_KIND_DATE != kind) &&
		     (VENTURE_FIELD_KIND_DATETIME != kind)) || custom)
		{
			/* A custom date lives in the attributes, where the query
			 * cannot bound it; refusing beats a period that silently
			 * bounds nothing. */
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "date_field \"%s\" is not a declared date or time "
			            "field of %s", date_field, plan->entity_name);
			return NULL;
		}
	}

	if (!venture_string_is_empty(bucket))
	{
		gsize i;

		for (i = 1; i < G_N_ELEMENTS(aggregate_buckets); i++)
		{
			if (0 == g_ascii_strcasecmp(bucket, aggregate_buckets[i].name))
				plan->bucket = (AggregateBucket)i;
		}

		if (AGGREGATE_BUCKET_NONE == plan->bucket)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "bucket \"%s\" is not one of day, week, month, "
			            "quarter, year", bucket);
			return NULL;
		}

		if (NULL == plan->date_field)
		{
			g_set_error_literal(error, VENTURE_ERROR,
			                    VENTURE_ERROR_INVALID_ARGUMENT,
			                    "bucket needs a date_field to bucket by");
			return NULL;
		}
	}

	/* --- Rates --- */

	per = venture_json_object_get_string(options, "per", NULL);

	if (!venture_string_is_empty(per))
	{
		if (0 == g_ascii_strcasecmp(per, "hour"))
		{
			plan->per_seconds = 3600;
			plan->per_name = "hour";
		}
		else if (0 == g_ascii_strcasecmp(per, "day"))
		{
			plan->per_seconds = 86400;
			plan->per_name = "day";
		}
		else
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "per \"%s\" is not hour or day", per);
			return NULL;
		}

		if ((VENTURE_AGGREGATE_SUM != plan->aggregate) &&
		    (VENTURE_AGGREGATE_COUNT != plan->aggregate))
		{
			g_set_error_literal(error, VENTURE_ERROR,
			                    VENTURE_ERROR_INVALID_ARGUMENT,
			                    "per is a rate of a sum or a count; an "
			                    "average, minimum, maximum or distinct count "
			                    "over time is not one");
			return NULL;
		}

		/* The denominator is the elapsed length of the window; a
		 * window with no start has none. A bucket always has one. */
		if ((AGGREGATE_BUCKET_NONE == plan->bucket) &&
		    ((NULL == period) || (NULL == venture_date_range_get_start(period))))
		{
			g_set_error_literal(error, VENTURE_ERROR,
			                    VENTURE_ERROR_INVALID_ARGUMENT,
			                    "per needs a period with a start, or a "
			                    "bucket: all time has no length to divide by");
			return NULL;
		}
	}

	return g_steal_pointer(&plan);
}

/*
 * The query: the type, the organisation, the venture, the period on the
 * date field and the filter. The row bound is checked by the caller.
 */
static VentureQuery *
aggregate_build_query(
	AggregatePlan		 *plan,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	g_autoptr(VentureQuery) query = NULL;
	VentureEntity *prototype;
	const gchar *filter;
	gint64 venture_id;

	query = venture_query_new(plan->type);
	prototype = venture_entity_registry_get_prototype(
		venture_entity_registry_get_default(), plan->entity_name);

	/* An organisation is the scope, not in one. */
	if (VENTURE_TYPE_ORGANIZATION != plan->type)
		venture_query_set_organization(query, plan->organization_id);

	venture_id = venture_json_object_get_int(options, "venture_id", 0);

	if (0 != venture_id)
	{
		if (NULL == g_object_class_find_property(G_OBJECT_GET_CLASS(prototype),
		                                         "venture-id"))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "%s records do not belong to a venture, so "
			            "venture_id cannot narrow them", plan->entity_name);
			return NULL;
		}

		if (!venture_query_add_filter_int(query, "venture-id",
		                                  VENTURE_FILTER_OP_EQ, venture_id,
		                                  error))
			return NULL;
	}

	if ((NULL != plan->date_field) && (NULL != period))
	{
		if (!venture_query_set_date_range(query,
			venture_field_spec_get_name(plan->date_field), period, error))
			return NULL;
	}

	filter = venture_json_object_get_string(options, "filter", NULL);

	if (!venture_string_is_empty(filter))
	{
		g_autoptr(GHashTable) params = NULL;
		GHashTableIter iter;
		gpointer key;

		params = g_uri_parse_params(filter, -1, "&", G_URI_PARAMS_WWW_FORM,
		                            error);

		if (NULL == params)
		{
			g_prefix_error(error, "The filter cannot be read: ");
			return NULL;
		}

		g_hash_table_iter_init(&iter, params);

		while (g_hash_table_iter_next(&iter, &key, NULL))
		{
			g_autofree gchar *base = NULL;
			g_autoptr(VentureFieldSpec) spec = NULL;
			g_autoptr(GError) ignored = NULL;
			gchar *operator;

			/* Paging would quietly total a page; ordering means
			 * nothing to a total. */
			if ((0 == g_strcmp0(key, "limit")) || (0 == g_strcmp0(key, "offset")) ||
			    (0 == g_strcmp0(key, "page")) || (0 == g_strcmp0(key, "order")))
			{
				g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
				            "The filter may not set %s: the aggregate reads "
				            "every matching record", (const gchar *)key);
				return NULL;
			}

			if (0 == g_strcmp0(key, "search"))
				continue;

			/* A filter on a sensitive field is a way of reading it,
			 * one comparison at a time. */
			base = g_strdup(key);
			operator = strstr(base, "__");

			if (NULL != operator)
				*operator = '\0';

			spec = venture_aggregate_find_field(plan->database,
				plan->organization_id, plan->entity_name, base, NULL,
				&ignored);

			if ((NULL != ignored) &&
			    g_error_matches(ignored, VENTURE_ERROR,
			                    VENTURE_ERROR_PERMISSION_DENIED))
			{
				g_propagate_error(error, g_steal_pointer(&ignored));
				return NULL;
			}
		}

		if (!venture_query_apply_query_string(query, params, error))
			return NULL;
	}

	if (!venture_period_report_scope(query, options, error))
		return NULL;

	venture_query_set_limit(query, 0);

	return g_steal_pointer(&query);
}

/* ==========================================================================
 * Writing the answer
 * ========================================================================== */

/*
 * The seconds a row's rate divides by: its window -- the bucket clipped to
 * the period, or the period -- clipped again to now, so this month's rate
 * is over the days gone, not the days it will have. Zero or less when the
 * window has not started.
 */
static gint64
aggregate_elapsed_seconds(
	VentureDateRange	*period,
	AggregateCell		*cell,
	GDateTime		*now
){
	GDateTime *start;
	GDateTime *end;

	start = cell->bucket_start;
	end = cell->bucket_end;

	if (NULL != period)
	{
		GDateTime *period_start;
		GDateTime *period_end;

		period_start = venture_date_range_get_start(period);
		period_end = venture_date_range_get_end(period);

		if ((NULL == start) ||
		    ((NULL != period_start) && (g_date_time_compare(period_start, start) > 0)))
			start = period_start;

		if ((NULL == end) ||
		    ((NULL != period_end) && (g_date_time_compare(period_end, end) < 0)))
			end = period_end;
	}

	if ((NULL == end) || (g_date_time_compare(now, end) < 0))
		end = now;

	if (NULL == start)
		return 0;

	return g_date_time_difference(end, start) / G_TIME_SPAN_SECOND;
}

/*
 * Writes a cell's value, and its rate when asked for one.
 */
static gboolean
aggregate_write_value(
	AggregatePlan		 *plan,
	VentureReportResult	 *result,
	VentureDateRange	 *period,
	AggregateCell		 *cell,
	GDateTime		 *now,
	GError			**error
){
	g_autoptr(VentureMoney) money = NULL;
	VentureFieldKind kind;
	gdouble number;
	gboolean is_money;

	kind = (NULL != plan->measure)
		? venture_field_spec_get_kind(plan->measure) : VENTURE_FIELD_KIND_INTEGER;
	is_money = FALSE;
	number = 0.0;

	switch (plan->aggregate)
	{
	case VENTURE_AGGREGATE_COUNT_DISTINCT:
		number = (NULL != cell->distinct) ? g_hash_table_size(cell->distinct) : 0;
		break;
	case VENTURE_AGGREGATE_SUM:
		is_money = (NULL != cell->money_sum);
		money = is_money ? venture_money_copy(cell->money_sum) : NULL;
		number = (VENTURE_FIELD_KIND_INTEGER == kind) ? (gdouble)cell->int_sum : cell->dbl_sum;
		break;
	case VENTURE_AGGREGATE_AVG:
		if (NULL != cell->money_sum)
		{
			/* One exact division, rounded half to even: never through a
			 * double, which is how a cent goes missing. */
			is_money = TRUE;
			money = venture_money_multiply_rational(cell->money_sum, 1,
			                                        (gint64)cell->count, error);

			if (NULL == money)
				return FALSE;
		}
		else
		{
			number = ((VENTURE_FIELD_KIND_INTEGER == kind)
				? (gdouble)cell->int_sum : cell->dbl_sum) / (gdouble)cell->count;
		}
		break;
	case VENTURE_AGGREGATE_MIN:
		is_money = (NULL != cell->money_min);
		money = is_money ? venture_money_copy(cell->money_min) : NULL;
		number = (VENTURE_FIELD_KIND_INTEGER == kind) ? (gdouble)cell->int_min : cell->dbl_min;
		break;
	case VENTURE_AGGREGATE_MAX:
		is_money = (NULL != cell->money_max);
		money = is_money ? venture_money_copy(cell->money_max) : NULL;
		number = (VENTURE_FIELD_KIND_INTEGER == kind) ? (gdouble)cell->int_max : cell->dbl_max;
		break;
	case VENTURE_AGGREGATE_COUNT:
	case VENTURE_AGGREGATE_NONE:
	default:
		number = cell->count;
		break;
	}

	if (is_money)
		venture_report_result_set_money(result, "value", money);
	else
		venture_report_result_set_number(result, "value", number);

	if (0 != plan->per_seconds)
	{
		gint64 elapsed;

		elapsed = aggregate_elapsed_seconds(period, cell, now);

		/* A window that has not begun has no rate, which is not the
		 * same as a rate of zero. */
		if (elapsed <= 0)
			return TRUE;

		if (is_money)
		{
			g_autoptr(VentureMoney) rate = NULL;

			rate = venture_money_multiply_rational(money, plan->per_seconds,
			                                       elapsed, error);

			if (NULL == rate)
				return FALSE;

			venture_report_result_set_money(result, "rate", rate);
		}
		else
		{
			venture_report_result_set_number(result, "rate",
				number * (gdouble)plan->per_seconds / (gdouble)elapsed);
		}
	}

	return TRUE;
}

/*
 * What the value column is called: "Sum of gross", "Records".
 */
static gchar *
aggregate_value_label(AggregatePlan *plan)
{
	static const gchar *const names[] = {
		"", "Sum", "Average", "Minimum", "Maximum", "Count", "Distinct"
	};
	g_autofree gchar *measure = NULL;

	if (NULL == plan->measure)
		return g_strdup("Records");

	measure = venture_string_is_empty(venture_field_spec_get_label(plan->measure))
		? aggregate_humanise(venture_field_spec_get_name(plan->measure))
		: g_ascii_strdown(venture_field_spec_get_label(plan->measure), -1);

	return g_strdup_printf("%s of %s", names[plan->aggregate], measure);
}

/*
 * A column key for a group: its name, unless that would collide with one
 * of the report's own columns.
 */
static gchar *
aggregate_group_column(
	const gchar	*name,
	guint		 index
){
	static const gchar *const reserved[] = {
		"bucket", "currency", "records", "value", "rate", NULL
	};

	if (g_strv_contains(reserved, name))
		return g_strdup_printf("group_%u", index + 1);

	return g_strdup(name);
}

VentureReportResult *
venture_aggregate_report(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	g_autoptr(AggregatePlan) plan = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GHashTable) cells = NULL;
	g_autoptr(GPtrArray) sorted = NULL;
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autoptr(GString) title = NULL;
	g_autofree gchar *value_label = NULL;
	VentureFieldKind measure_kind;
	GHashTableIter iter;
	gpointer value;
	gboolean money_measure;
	gint64 matched;
	guint i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	plan = aggregate_plan_new(context, period, options, error);

	if (NULL == plan)
		return NULL;

	query = aggregate_build_query(plan, period, options, error);

	if (NULL == query)
		return NULL;

	/* Counted first, so an over-broad question is refused before its rows
	 * are loaded rather than after. */
	matched = venture_database_count(plan->database, query, error);

	if (matched < 0)
		return NULL;

	if (matched > VENTURE_AGGREGATE_MAX_ROWS)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "%" G_GINT64_FORMAT " %s records match; the aggregate "
		            "report reads at most %d. Narrow the period or the "
		            "filter", matched, plan->entity_name,
		            VENTURE_AGGREGATE_MAX_ROWS);
		return NULL;
	}

	rows = venture_database_find(plan->database, query, error);

	if (NULL == rows)
		return NULL;

	measure_kind = (NULL != plan->measure)
		? venture_field_spec_get_kind(plan->measure) : VENTURE_FIELD_KIND_INTEGER;
	money_measure = (VENTURE_FIELD_KIND_MONEY == measure_kind);
	cells = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                              aggregate_cell_free);

	/* --- One pass over the rows --- */

	for (i = 0; i < rows->len; i++)
	{
		g_autoptr(VentureMoney) money = NULL;
		g_autoptr(GString) key = NULL;
		g_autoptr(GPtrArray) keys = NULL;
		g_autoptr(GPtrArray) labels = NULL;
		g_autoptr(GDateTime) bucket_start = NULL;
		g_autoptr(GDateTime) bucket_end = NULL;
		g_autofree gchar *bucket_key = NULL;
		g_autofree gchar *bucket_label = NULL;
		g_autofree gchar *distinct = NULL;
		VentureEntity *row;
		AggregateCell *cell;
		const gchar *currency;
		gint64 integer;
		gdouble number;
		guint g;

		row = g_ptr_array_index(rows, i);
		integer = 0;
		number = 0.0;

		/* The measure first: a record with nothing to add is in no
		 * row, and is counted so the note can say so. */
		if ((NULL != plan->measure) &&
		    venture_aggregate_field_is_numeric(plan->measure))
		{
			gint read;

			read = aggregate_read_number(row, plan->measure,
				plan->measure_custom, &money, &integer, &number);

			if (read < 0)
			{
				plan->unreadable++;
				continue;
			}

			if (0 == read)
			{
				plan->absent++;
				continue;
			}

			if (VENTURE_AGGREGATE_COUNT_DISTINCT == plan->aggregate)
			{
				if (NULL != money)
					distinct = venture_money_to_string(money);
				else if (VENTURE_FIELD_KIND_INTEGER == measure_kind)
					distinct = g_strdup_printf("%" G_GINT64_FORMAT, integer);
				else
					distinct = g_strdup_printf("%.17g", number);
			}
		}
		else if (NULL != plan->measure)
		{
			g_autofree gchar *label = NULL;

			aggregate_describe(plan, row, plan->measure, plan->measure_custom,
			                   &distinct, &label);

			if ('\0' == distinct[0])
			{
				plan->absent++;
				continue;
			}
		}

		key = g_string_new(NULL);
		keys = g_ptr_array_new_with_free_func(g_free);
		labels = g_ptr_array_new_with_free_func(g_free);

		if (AGGREGATE_BUCKET_NONE != plan->bucket)
		{
			g_auto(GValue) when = G_VALUE_INIT;
			GDateTime *instant;

			instant = NULL;

			if (venture_entity_get_field(row,
				venture_field_spec_get_name(plan->date_field), &when) &&
			    G_VALUE_HOLDS(&when, G_TYPE_DATE_TIME))
				instant = g_value_get_boxed(&when);

			if (NULL != instant)
			{
				aggregate_bucket_of(plan->bucket, instant, &bucket_start,
				                    &bucket_end, &bucket_key, &bucket_label);
			}
			else
			{
				/* Sorted last: "~" follows every digit. */
				bucket_key = g_strdup("~");
				bucket_label = g_strdup("(no date)");
			}

			g_string_append(key, bucket_key);
		}

		for (g = 0; g < plan->n_groups; g++)
		{
			gchar *group_key;
			gchar *group_label;

			aggregate_group_value(plan, &plan->groups[g], row, &group_key,
			                      &group_label);
			g_string_append(key, AGGREGATE_KEY_SEPARATOR);
			g_string_append(key, group_key);
			g_ptr_array_add(keys, group_key);
			g_ptr_array_add(labels, group_label);
		}

		g_ptr_array_add(keys, NULL);
		g_ptr_array_add(labels, NULL);

		/* Money is split by currency here, at the key: two currencies
		 * are two cells, so nothing below can ever add across them. */
		currency = (NULL != money) ? venture_money_get_currency(money) : NULL;

		if (NULL != currency)
		{
			g_string_append(key, AGGREGATE_KEY_SEPARATOR);
			g_string_append(key, currency);
		}

		cell = aggregate_cell_lookup(cells, key->str, (gchar **)keys->pdata,
		                             (gchar **)labels->pdata, bucket_key,
		                             bucket_label, bucket_start, bucket_end,
		                             currency);

		if (!aggregate_cell_add(cell, measure_kind, money, integer, number,
		                        distinct, error))
			return NULL;
	}

	/* --- The table --- */

	title = g_string_new(NULL);
	value_label = aggregate_value_label(plan);

	{
		g_autofree gchar *type_label = NULL;

		type_label = venture_entity_type_dup_label(plan->type, TRUE);
		g_string_append_printf(title, "%s: %s", (NULL != type_label)
			? type_label : plan->entity_name, value_label);
	}

	for (i = 0; i < plan->n_groups; i++)
		g_string_append_printf(title, "%s%s", (0 == i) ? " by " : ", ",
		                       plan->groups[i].label);

	if (AGGREGATE_BUCKET_NONE != plan->bucket)
		g_string_append_printf(title, " per %s",
			aggregate_buckets[plan->bucket].name);

	result = venture_report_result_new(title->str, period);

	if (AGGREGATE_BUCKET_NONE != plan->bucket)
		venture_report_result_add_column(result, "bucket",
			aggregate_buckets[plan->bucket].label, VENTURE_REPORT_COLUMN_TEXT);

	for (i = 0; i < plan->n_groups; i++)
	{
		plan->groups[i].column = aggregate_group_column(plan->groups[i].name, i);
		venture_report_result_add_column(result, plan->groups[i].column,
			plan->groups[i].label, VENTURE_REPORT_COLUMN_TEXT);
	}

	if (money_measure)
		venture_report_result_add_column(result, "currency", "Currency",
		                                 VENTURE_REPORT_COLUMN_TEXT);

	venture_report_result_add_column(result, "records", "Records",
	                                 VENTURE_REPORT_COLUMN_NUMBER);

	/* The count of records is already the records column. */
	if (NULL != plan->measure)
	{
		gboolean value_is_money;

		value_is_money = money_measure &&
			(VENTURE_AGGREGATE_COUNT != plan->aggregate) &&
			(VENTURE_AGGREGATE_COUNT_DISTINCT != plan->aggregate);
		venture_report_result_add_column(result, "value", value_label,
			value_is_money ? VENTURE_REPORT_COLUMN_MONEY
			               : VENTURE_REPORT_COLUMN_NUMBER);

		if (0 != plan->per_seconds)
		{
			g_autofree gchar *rate_label = NULL;

			rate_label = g_strdup_printf("Per %s", plan->per_name);
			venture_report_result_add_column(result, "rate", rate_label,
				value_is_money ? VENTURE_REPORT_COLUMN_MONEY
				               : VENTURE_REPORT_COLUMN_NUMBER);
		}
	}
	else if (0 != plan->per_seconds)
	{
		g_autofree gchar *rate_label = NULL;

		rate_label = g_strdup_printf("Per %s", plan->per_name);
		venture_report_result_add_column(result, "rate", rate_label,
		                                 VENTURE_REPORT_COLUMN_NUMBER);
	}

	sorted = g_ptr_array_new();
	g_hash_table_iter_init(&iter, cells);

	while (g_hash_table_iter_next(&iter, NULL, &value))
		g_ptr_array_add(sorted, value);

	g_ptr_array_sort(sorted, aggregate_cell_compare);
	now = g_date_time_new_now_utc();

	for (i = 0; i < sorted->len; i++)
	{
		AggregateCell *cell;
		guint g;

		cell = g_ptr_array_index(sorted, i);
		venture_report_result_begin_row(result);

		if (AGGREGATE_BUCKET_NONE != plan->bucket)
			venture_report_result_set_text(result, "bucket", cell->bucket_label);

		for (g = 0; g < plan->n_groups; g++)
			venture_report_result_set_text(result, plan->groups[g].column,
			                               cell->labels[g]);

		if (money_measure)
			venture_report_result_set_text(result, "currency", cell->currency);

		venture_report_result_set_number(result, "records", cell->count);

		if (NULL != plan->measure)
		{
			if (!aggregate_write_value(plan, result, period, cell, now, error))
				return NULL;
		}
		else if (0 != plan->per_seconds)
		{
			gint64 elapsed;

			elapsed = aggregate_elapsed_seconds(period, cell, now);

			if (elapsed > 0)
				venture_report_result_set_number(result, "rate",
					(gdouble)cell->count * (gdouble)plan->per_seconds /
					(gdouble)elapsed);
		}
	}

	venture_report_result_add_metric(result,
		venture_metric_new_count("records", "Records", (gint64)rows->len));
	venture_report_result_add_metric(result,
		venture_metric_new_count("rows", "Rows", (gint64)sorted->len));

	/* --- What the figures leave out, said outright --- */

	if ((NULL == plan->date_field) && (NULL != period) &&
	    (NULL != venture_date_range_get_start(period)))
	{
		venture_report_result_append_note(result,
			"No date_field was named, so the period does not bound these "
			"records: every matching record is included. Name a date "
			"field (date_field=occurred_at) to read one period.");
	}

	if (plan->absent > 0)
	{
		g_autofree gchar *note = NULL;
		g_autofree gchar *wire = NULL;

		wire = aggregate_wire_name(venture_field_spec_get_name(plan->measure));
		note = g_strdup_printf("%u record%s had no %s and %s in no row.",
		                       plan->absent, (1 == plan->absent) ? "" : "s",
		                       wire, (1 == plan->absent) ? "is" : "are");
		venture_report_result_append_note(result, note);
	}

	if (plan->unreadable > 0)
	{
		g_autofree gchar *note = NULL;

		note = g_strdup_printf("%u record%s held a value that could not be "
		                       "read as a number or an amount and %s left out.",
		                       plan->unreadable,
		                       (1 == plan->unreadable) ? "" : "s",
		                       (1 == plan->unreadable) ? "is" : "are");
		venture_report_result_append_note(result, note);
	}

	if (0 != plan->per_seconds)
	{
		g_autofree gchar *note = NULL;

		note = g_strdup_printf("Per %s is each row's value divided by the "
		                       "%ss elapsed in its window: the %s, clipped "
		                       "to now.", plan->per_name, plan->per_name,
		                       (AGGREGATE_BUCKET_NONE != plan->bucket)
		                        ? "bucket within the period" : "period");
		venture_report_result_append_note(result, note);
	}

	return g_steal_pointer(&result);
}

/* ==========================================================================
 * The report object
 * ========================================================================== */

#define VENTURE_TYPE_AGGREGATE_REPORT (venture_aggregate_report_object_get_type())

G_DECLARE_FINAL_TYPE(VentureAggregateReport, venture_aggregate_report_object,
                     VENTURE, AGGREGATE_REPORT, VentureReport)

struct _VentureAggregateReport
{
	VentureReport parent_instance;
};

G_DEFINE_TYPE(VentureAggregateReport, venture_aggregate_report_object,
              VENTURE_TYPE_REPORT)

static VentureReportResult *
venture_aggregate_report_object_generate(
	VentureReport		 *self,
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	(void)self;

	return venture_aggregate_report(context, period, options, error);
}

/*
 * Every option, described, because this schema is what the assistant's
 * report tool and the CLI help read: a report whose options are
 * undocumented is one only its author can run.
 */
static JsonNode *
venture_aggregate_report_object_parameters(VentureReport *self)
{
	(void)self;

	return venture_json_parse(
		"{\"type\":\"object\",\"required\":[\"type\"],\"properties\":{"
		"\"type\":{\"type\":\"string\",\"description\":\"The record type to "
		"read, e.g. sale, ticket, expense; any business type whose module is "
		"on\"},"
		"\"measure\":{\"type\":\"string\",\"description\":\"The field to "
		"aggregate (money, integer or double for sum/avg/min/max; any field "
		"for count/count_distinct), a custom field by name, or count "
		"(default) for the records themselves\"},"
		"\"aggregate\":{\"type\":\"string\",\"enum\":[\"sum\",\"avg\",\"min\","
		"\"max\",\"count\",\"count_distinct\"],\"description\":\"What to do "
		"to the measure; sum for a numeric measure, count otherwise. Money "
		"is aggregated per currency, one row each\"},"
		"\"group_by\":{\"type\":\"string\",\"description\":\"Up to 3 "
		"comma-separated fields: plain fields, enums (shown by label), "
		"references (shown by name; categories and locations by path), "
		"custom fields, or reference.field to group by a field of what a "
		"reference points at (product_id.category_id)\"},"
		"\"category_depth\":{\"type\":\"integer\",\"description\":\"Roll "
		"category and location groups up to this level of their tree; 0 is "
		"the top\"},"
		"\"date_field\":{\"type\":\"string\",\"description\":\"The date or "
		"time field the period bounds and bucket reads, e.g. occurred_at; "
		"without it the period bounds nothing\"},"
		"\"bucket\":{\"type\":\"string\",\"enum\":[\"day\",\"week\",\"month\","
		"\"quarter\",\"year\"],\"description\":\"Split by date_field into "
		"UTC calendar buckets\"},"
		"\"filter\":{\"type\":\"string\",\"description\":\"A list-page query "
		"string: status=open&priority__in=high,urgent\"},"
		"\"per\":{\"type\":\"string\",\"enum\":[\"hour\",\"day\"],"
		"\"description\":\"Add a rate: the sum or count divided by the hours "
		"or days elapsed in the period (or bucket), clipped to now\"},"
		"\"period\":{\"type\":\"string\",\"description\":\"The period, e.g. "
		"this_month, 2026-03, last_30_days, all\"},"
		"\"organization_id\":{\"type\":\"integer\",\"description\":\"The "
		"legal entity; defaults to the default organization\"},"
		"\"venture_id\":{\"type\":\"integer\",\"description\":\"Narrow to "
		"one venture, for types that belong to one\"}}}", NULL);
}

static void
venture_aggregate_report_object_class_init(VentureAggregateReportClass *klass)
{
	VentureReportClass *report_class;

	report_class = VENTURE_REPORT_CLASS(klass);
	report_class->generate = venture_aggregate_report_object_generate;
	report_class->describe_parameters = venture_aggregate_report_object_parameters;
}

static void
venture_aggregate_report_object_init(VentureAggregateReport *self)
{
	(void)self;
}

void
venture_aggregate_register_report(VentureReportRegistry *registry)
{
	VentureReport *report;

	g_return_if_fail(VENTURE_IS_REPORT_REGISTRY(registry));

	report = g_object_new(VENTURE_TYPE_AGGREGATE_REPORT,
		"name", "aggregate",
		"title", "Aggregate",
		"description", "Sum, average, minimum, maximum, count or distinct "
		"count of any field of any record type, grouped by up to three "
		"fields and bucketed by a date, money per currency",
		NULL);
	venture_data_class_declare_resource(G_OBJECT(report), VENTURE_DATA_CLASS_TENANT);
	venture_report_registry_add(registry, report);
}
