/*
 * venture-query.h - Safe, composable record queries
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A #VentureQuery describes what to fetch: which record type, which filters,
 * what order, how many. It compiles to parameterised SQL.
 *
 * The reason this type exists rather than a function taking a WHERE clause
 * is that three of its four callers are not trustworthy in the relevant
 * sense. A REST client sends query parameters; the CLI passes flags through;
 * the AI writes filters from a natural-language request. None of them may be
 * allowed to contribute SQL text.
 *
 * So they cannot. A filter names a field, an operator from a closed
 * enumeration, and values. Field names are checked against the record type's
 * actual properties and then quoted; operators map to fixed SQL fragments;
 * values only ever become bound parameters. There is no path by which caller
 * input reaches the statement as text, which makes injection structurally
 * impossible rather than merely unlikely.
 */

#ifndef VENTURE_QUERY_H
#define VENTURE_QUERY_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>
#include <orm.h>

G_BEGIN_DECLS

#define VENTURE_TYPE_QUERY (venture_query_get_type())

G_DECLARE_FINAL_TYPE(VentureQuery, venture_query, VENTURE, QUERY, GObject)

/**
 * venture_query_new:
 * @entity_type: the record type to query
 *
 * Returns: (transfer full): a new query over @entity_type
 */
VentureQuery *
venture_query_new(GType entity_type);

/**
 * venture_query_new_for_name:
 * @registry: the registry to resolve @entity_name in
 * @entity_name: the record type name
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): a new query, or %NULL if the name is
 *   not a registered record type
 */
VentureQuery *
venture_query_new_for_name(
	VentureEntityRegistry	 *registry,
	const gchar		 *entity_name,
	GError			**error
);

/**
 * venture_query_get_entity_type:
 * @self: a #VentureQuery
 *
 * Returns: the record type being queried
 */
GType
venture_query_get_entity_type(VentureQuery *self);

/**
 * venture_query_add_filter:
 * @self: a #VentureQuery
 * @field: a field name, in either property or column spelling
 * @op: the comparison
 * @values: (element-type utf8): the operand values as text
 * @error: (out) (optional): return location for a #GError
 *
 * Adds a filter. The field must exist on the record type and the operand
 * count must match the operator's arity, both of which are checked here
 * rather than discovered as a SQL syntax error later.
 *
 * Returns: %TRUE if the filter was accepted
 */
gboolean
venture_query_add_filter(
	VentureQuery	 *self,
	const gchar	 *field,
	VentureFilterOp	  op,
	GPtrArray	 *values,
	GError		**error
);

/**
 * venture_query_add_filter_string:
 * @self: a #VentureQuery
 * @field: a field name
 * @op: the comparison
 * @value: (nullable): the single operand
 * @error: (out) (optional): return location for a #GError
 *
 * Convenience for the common single-operand filter.
 *
 * Returns: %TRUE if the filter was accepted
 */
gboolean
venture_query_add_filter_string(
	VentureQuery	 *self,
	const gchar	 *field,
	VentureFilterOp	  op,
	const gchar	 *value,
	GError		**error
);

/**
 * venture_query_add_filter_int:
 * @self: a #VentureQuery
 * @field: a field name
 * @op: the comparison
 * @value: the operand
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: %TRUE if the filter was accepted
 */
gboolean
venture_query_add_filter_int(
	VentureQuery	 *self,
	const gchar	 *field,
	VentureFilterOp	  op,
	gint64		  value,
	GError		**error
);

/**
 * venture_query_set_date_range:
 * @self: a #VentureQuery
 * @field: the timestamp field to bound
 * @range: (nullable): the period; %NULL clears any bound
 * @error: (out) (optional): return location for a #GError
 *
 * Restricts the query to records whose @field falls in @range, applying the
 * half-open rule so adjacent periods do not both claim a boundary record.
 *
 * Returns: %TRUE if the bound was accepted
 */
gboolean
venture_query_set_date_range(
	VentureQuery		 *self,
	const gchar		 *field,
	const VentureDateRange	 *range,
	GError			**error
);

/**
 * venture_query_set_search:
 * @self: a #VentureQuery
 * @text: (nullable): free text to match
 *
 * Matches @text case-insensitively against every field the record type
 * flagged searchable. Clearing it removes the constraint.
 */
void
venture_query_set_search(
	VentureQuery	*self,
	const gchar	*text
);

/**
 * venture_query_add_order:
 * @self: a #VentureQuery
 * @field: the field to sort by
 * @direction: ascending or descending
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: %TRUE if the field exists
 */
gboolean
venture_query_add_order(
	VentureQuery		 *self,
	const gchar		 *field,
	VentureSortDirection	  direction,
	GError			**error
);

/**
 * venture_query_set_limit:
 * @self: a #VentureQuery
 * @limit: the maximum number of rows, or 0 for no limit
 */
void
venture_query_set_limit(
	VentureQuery	*self,
	guint		 limit
);

/**
 * venture_query_set_offset:
 * @self: a #VentureQuery
 * @offset: how many rows to skip
 */
void
venture_query_set_offset(
	VentureQuery	*self,
	guint		 offset
);

/**
 * venture_query_set_organization:
 * @self: a #VentureQuery
 * @organization_id: the owning organisation, or 0 for every organisation
 *
 * Scopes the query to one organisation. This is applied to every query the
 * web and CLI layers issue, which is what keeps a personal entity's records
 * out of a business's reports.
 */
void
venture_query_set_organization(
	VentureQuery	*self,
	gint64		 organization_id
);

/**
 * venture_query_set_organization_tree:
 * @self: a #VentureQuery
 * @organization_ids: (array length=n_organizations): the entities to include,
 *   the first being the primary one
 * @n_organizations: how many
 *
 * Restricts the query to a set of entities rather than one.
 *
 * Entities nest: a business may hold sub-entities, and looking at the parent
 * should show everything beneath it rather than only what was filed directly
 * against the parent. Every identifier is bound as a parameter.
 */
void
venture_query_set_organization_tree(
	VentureQuery	*self,
	const gint64	*organization_ids,
	gsize		 n_organizations
);

/**
 * venture_query_set_include_deleted:
 * @self: a #VentureQuery
 * @include_deleted: whether soft-deleted records are returned
 *
 * Off by default. Records are never physically removed, so without this
 * every query would otherwise return the deleted ones too.
 */
void
venture_query_set_include_deleted(
	VentureQuery	*self,
	gboolean	 include_deleted
);

/**
 * venture_query_get_limit:
 * @self: a #VentureQuery
 *
 * Returns: the row limit, or 0 for none
 */
guint
venture_query_get_limit(VentureQuery *self);

/**
 * venture_query_apply_json:
 * @self: a #VentureQuery
 * @node: a JSON object describing filters, ordering and paging
 * @error: (out) (optional): return location for a #GError
 *
 * Applies a query description. This is the form the AI's query tool and the
 * REST API's request body both use:
 *
 * |[
 * {
 *   "filters": [ {"field": "status", "op": "eq", "value": "active"} ],
 *   "search":  "etsy",
 *   "order":   [ {"field": "occurred_at", "direction": "desc"} ],
 *   "limit":   50,
 *   "offset":  0,
 *   "period":  "this_month",
 *   "period_field": "occurred_at"
 * }
 * ]|
 *
 * Every part is validated against the record type, so a model that invents a
 * field name is told so rather than silently returning everything.
 *
 * Returns: %TRUE if the description was accepted
 */
gboolean
venture_query_apply_json(
	VentureQuery	 *self,
	JsonNode	 *node,
	GError		**error
);

/**
 * venture_query_apply_query_string:
 * @self: a #VentureQuery
 * @params: (element-type utf8 utf8): decoded URL query parameters
 * @error: (out) (optional): return location for a #GError
 *
 * Applies filters from URL query parameters. A bare `field=value` is an
 * equality test; `field__op=value` uses the named operator, so
 * `?occurred_at__gte=2026-01-01&status__in=active,paused` works.
 *
 * Returns: %TRUE if every parameter was accepted
 */
gboolean
venture_query_apply_query_string(
	VentureQuery	 *self,
	GHashTable	 *params,
	GError		**error
);

/**
 * venture_query_to_sql:
 * @self: a #VentureQuery
 * @dialect: the target SQL dialect
 * @count_only: build a `SELECT COUNT(*)` instead of selecting rows
 * @out_params: (out) (transfer full) (element-type OrmValue): the bound
 *   parameters, in placeholder order
 *
 * Compiles the query. Every caller-supplied value is a bound parameter; no
 * caller input is ever interpolated into the returned text.
 *
 * Returns: (transfer full): the SQL statement
 */
gchar *
venture_query_to_sql(
	VentureQuery	 *self,
	OrmDialectType	  dialect,
	gboolean	  count_only,
	GList		**out_params
);

/**
 * venture_query_describe:
 * @self: a #VentureQuery
 *
 * Produces a human-readable summary of what the query will fetch, used in
 * AI confirmations and in the CLI's dry-run output so it is obvious what a
 * destructive bulk operation is about to touch.
 *
 * Returns: (transfer full): the description
 */
gchar *
venture_query_describe(VentureQuery *self);

G_END_DECLS

#endif /* VENTURE_QUERY_H */
