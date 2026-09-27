/*
 * venture-aggregate-report.h - One report that sums, counts and averages
 *                              any record type
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Every other report answers one question about one kind of record. The
 * aggregate report answers the shape of question they all share -- how
 * much, how many, grouped by what, over which days -- for any record type
 * the caller may see, a plugin's included, from the field table alone.
 *
 * The query layer deliberately has no GROUP BY: a SUM over a money column
 * adds cents to gold pieces. So rows are fetched through the ordinary
 * query (which carries the organisation, the access policy and the module
 * mask) and bucketed here in C, money per currency, never mixed. The same
 * accumulator backs the dashboard's `sum` and `progress` widgets, so a
 * figure on a card and the report behind it are one computation.
 */

#ifndef VENTURE_AGGREGATE_REPORT_H
#define VENTURE_AGGREGATE_REPORT_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/**
 * VENTURE_AGGREGATE_MAX_ROWS:
 *
 * The most records one aggregation reads. Totals are computed in C over
 * fetched rows, so an unbounded read is an unbounded request; a question
 * that matches more is refused -- naming the count -- rather than answered
 * from a silently truncated set, which would be a smaller number presented
 * as the same one.
 */
#define VENTURE_AGGREGATE_MAX_ROWS (20000)

/**
 * VENTURE_AGGREGATE_MAX_GROUPS:
 *
 * How many `group_by` fields one aggregation takes. Past three a table is
 * a cross-tab nobody reads, and each level multiplies the rows.
 */
#define VENTURE_AGGREGATE_MAX_GROUPS (3)

/**
 * VentureAggregateTotal:
 * @currency: (nullable): the currency of @money; %NULL for a plain number
 * @money: (nullable): the total of a money field in @currency
 * @number: the total of an integer or double field
 * @count: how many records carried a value
 *
 * One total: of a money field in one currency, or of a number. A money
 * field yields one of these per currency seen -- totals are never added
 * across currencies.
 */
typedef struct
{
	gchar		*currency;
	VentureMoney	*money;
	gdouble		 number;
	guint		 count;
} VentureAggregateTotal;

/**
 * venture_aggregate_total_free:
 * @self: (nullable): a #VentureAggregateTotal
 *
 * Frees @self. Safe to call with %NULL.
 */
void
venture_aggregate_total_free(VentureAggregateTotal *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(VentureAggregateTotal, venture_aggregate_total_free)

/**
 * venture_aggregate_find_field:
 * @database: the database, for the organisation's custom fields
 * @organization_id: the organisation whose custom fields count
 * @entity_name: the registered record type
 * @name: the field, in the wire (`occurred_at`) or property spelling
 * @out_custom: (out) (optional): whether the field is a custom field,
 *   whose value lives in the record's attributes
 * @error: (out) (optional): return location for a #GError
 *
 * Resolves a field for aggregation: a declared field of the type, the
 * spine's `created_at`/`updated_at`, or a custom field defined for the
 * type in @organization_id. A sensitive field is refused: a total is a
 * way of reading a value, and sensitive values reach no response.
 *
 * Returns: (transfer full) (nullable): a copy of the field's spec, or
 *   %NULL with @error set naming the type and the field
 */
VentureFieldSpec *
venture_aggregate_find_field(
	VentureDatabase	 *database,
	gint64		  organization_id,
	const gchar	 *entity_name,
	const gchar	 *name,
	gboolean	 *out_custom,
	GError		**error
);

/**
 * venture_aggregate_field_is_numeric:
 * @spec: a field spec
 *
 * Returns: %TRUE when the field is money, an integer or a double -- the
 *   kinds a sum, an average, a minimum and a maximum are defined on
 */
gboolean
venture_aggregate_field_is_numeric(const VentureFieldSpec *spec);

/**
 * venture_aggregate_sum:
 * @rows: (element-type VentureEntity): the records to total
 * @spec: a numeric field of theirs, from venture_aggregate_find_field()
 * @custom: whether @spec is a custom field
 * @error: (out) (optional): return location for a #GError
 *
 * Totals @spec across @rows: one #VentureAggregateTotal per currency for a
 * money field, sorted by code, and exactly one for a number. A record
 * with no value (an unset money field or custom field) is not counted.
 * Integers are added exactly and an overflow is refused.
 *
 * Returns: (transfer full) (element-type VentureAggregateTotal) (nullable):
 *   the totals, empty for a money field no record set; %NULL on error
 */
GPtrArray *
venture_aggregate_sum(
	GPtrArray		 *rows,
	const VentureFieldSpec	 *spec,
	gboolean		  custom,
	GError			**error
);

/**
 * venture_aggregate_report:
 * @context: the wiring
 * @period: (nullable): the period, bounding `date_field` when one is named
 * @options: (nullable): `type`, `measure`, `aggregate`, `group_by`,
 *   `category_depth`, `date_field`, `bucket`, `filter`, `per`,
 *   `organization_id`, `venture_id`, `as_of`; see docs/reporting.org
 * @error: (out) (optional): return location for a #GError
 *
 * Runs the aggregation @options describe over the records of one type.
 *
 * Returns: (transfer full) (nullable): the result, or %NULL with @error
 *   set when the options are refused
 */
VentureReportResult *
venture_aggregate_report(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
);

/**
 * venture_aggregate_register_report:
 * @registry: the report registry
 *
 * Registers the report as "aggregate". It belongs to the core module, so
 * no module switch hides it; the record types it reads are hidden by
 * their own modules instead.
 */
void
venture_aggregate_register_report(VentureReportRegistry *registry);

G_END_DECLS

#endif /* VENTURE_AGGREGATE_REPORT_H */
