/*
 * venture-query.c - Safe, composable record queries
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The invariant this file maintains: no caller-supplied string ever reaches
 * the compiled SQL as text. Field names are resolved against the record
 * type's real properties and then quoted by the schema layer; operators come
 * from a closed enumeration and map to fixed fragments; values are always
 * bound parameters. Anything that fails to resolve is an error, never a
 * pass-through.
 */

#include "venture.h"

#include <string.h>

typedef struct
{
	gchar		*column;
	GType		 value_type;
	VentureFilterOp	 op;
	GPtrArray	*values;
} VentureQueryFilterEntry;

typedef struct
{
	gchar			*column;
	VentureSortDirection	 direction;
} VentureQueryOrderEntry;

struct _VentureQuery
{
	GObject parent_instance;

	GType		 entity_type;
	GPtrArray	*filters;
	GPtrArray	*orders;
	gchar		*search;
	guint		 limit;
	guint		 offset;
	gint64		 organization_id;

	/*
	 * Additional organisations to include alongside organization_id.
	 * Entities nest -- a business may hold sub-entities -- and looking at
	 * a parent means looking at everything beneath it.
	 */
	GArray		*organization_tree;
	gboolean	 include_deleted;
};

G_DEFINE_FINAL_TYPE(VentureQuery, venture_query, G_TYPE_OBJECT)

static void
venture_query_filter_entry_free(gpointer data)
{
	VentureQueryFilterEntry *entry;

	entry = data;

	g_free(entry->column);
	g_clear_pointer(&entry->values, g_ptr_array_unref);
	g_free(entry);
}

static void
venture_query_order_entry_free(gpointer data)
{
	VentureQueryOrderEntry *entry;

	entry = data;

	g_free(entry->column);
	g_free(entry);
}

static void
venture_query_finalize(GObject *object)
{
	VentureQuery *self;

	self = VENTURE_QUERY(object);

	g_clear_pointer(&self->filters, g_ptr_array_unref);
	g_clear_pointer(&self->orders, g_ptr_array_unref);
	g_clear_pointer(&self->organization_tree, g_array_unref);
	g_clear_pointer(&self->search, g_free);

	G_OBJECT_CLASS(venture_query_parent_class)->finalize(object);
}

static void
venture_query_class_init(VentureQueryClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = venture_query_finalize;
}

static void
venture_query_init(VentureQuery *self)
{
	self->filters = g_ptr_array_new_with_free_func(
		venture_query_filter_entry_free);
	self->orders = g_ptr_array_new_with_free_func(
		venture_query_order_entry_free);
	self->include_deleted = FALSE;
}

VentureQuery *
venture_query_new(GType entity_type)
{
	VentureQuery *self;

	g_return_val_if_fail(g_type_is_a(entity_type, VENTURE_TYPE_ENTITY), NULL);

	self = g_object_new(VENTURE_TYPE_QUERY, NULL);
	self->entity_type = entity_type;

	return self;
}

VentureQuery *
venture_query_new_for_name(
	VentureEntityRegistry	 *registry,
	const gchar		 *entity_name,
	GError			**error
){
	GType entity_type;

	g_return_val_if_fail(VENTURE_IS_ENTITY_REGISTRY(registry), NULL);
	g_return_val_if_fail(NULL != entity_name, NULL);

	entity_type = venture_entity_registry_lookup(registry, entity_name);

	if (G_TYPE_INVALID == entity_type)
	{
		g_auto(GStrv) known = NULL;
		g_autofree gchar *list = NULL;

		known = venture_entity_registry_list_names(registry);
		list = g_strjoinv(", ", known);

		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "There is no record type called \"%s\". Known types: %s",
		            entity_name, list);
		return NULL;
	}

	return venture_query_new(entity_type);
}

GType
venture_query_get_entity_type(VentureQuery *self)
{
	g_return_val_if_fail(VENTURE_IS_QUERY(self), G_TYPE_INVALID);

	return self->entity_type;
}

/* --- Field resolution ---------------------------------------------------- */

/*
 * Resolves a caller-supplied field name to a real column.
 *
 * This is the gate. A name that does not correspond to a property of the
 * record type is rejected outright, which is what stops a REST parameter or
 * an AI-invented field from becoming SQL text. Both spellings are accepted
 * because callers arrive holding either.
 */
static gboolean
venture_query_resolve_field(
	VentureQuery	 *self,
	const gchar	 *field,
	gchar		**out_column,
	GType		 *out_value_type,
	GError		**error
){
	g_autoptr(VentureEntityClass) klass = NULL;
	g_autofree gchar *property_name = NULL;
	GParamSpec *pspec;

	property_name = venture_entity_column_to_property(field);
	klass = g_type_class_ref(self->entity_type);

	pspec = g_object_class_find_property(G_OBJECT_CLASS(klass), property_name);

	if (NULL == pspec)
	{
		g_autoptr(VentureEntity) prototype = NULL;
		g_auto(GStrv) columns = NULL;
		g_autofree gchar *list = NULL;

		prototype = g_object_new(self->entity_type, NULL);
		columns = venture_schema_get_columns(self->entity_type);
		list = g_strjoinv(", ", columns);

		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "%s has no field \"%s\". Available fields: %s",
		            venture_entity_get_entity_name(prototype), field, list);
		return FALSE;
	}

	/* A money property is three columns; filtering and sorting act on the
	 * amount, which is the only one an ordering or comparison means. */
	if (VENTURE_TYPE_MONEY == pspec->value_type)
	{
		g_autofree gchar *base = NULL;

		base = venture_entity_property_to_column(pspec->name);
		*out_column = g_strconcat(base,
			VENTURE_SCHEMA_MONEY_AMOUNT_SUFFIX, NULL);
		*out_value_type = G_TYPE_INT64;

		return TRUE;
	}

	*out_column = venture_entity_property_to_column(pspec->name);
	*out_value_type = pspec->value_type;

	return TRUE;
}

/* --- Filters ------------------------------------------------------------- */

gboolean
venture_query_add_filter(
	VentureQuery	 *self,
	const gchar	 *field,
	VentureFilterOp	  op,
	GPtrArray	 *values,
	GError		**error
){
	VentureQueryFilterEntry *entry;
	g_autofree gchar *column = NULL;
	GType value_type;
	guint arity;
	guint supplied;

	g_return_val_if_fail(VENTURE_IS_QUERY(self), FALSE);
	g_return_val_if_fail(NULL != field, FALSE);

	if (!venture_query_resolve_field(self, field, &column, &value_type, error))
		return FALSE;

	arity = venture_filter_op_arity(op);
	supplied = (NULL != values) ? values->len : 0;

	/* Checked here so a mistake surfaces as a clear message rather than a
	 * SQL syntax error from three layers down. IN takes a list, so any
	 * non-zero count is fine for it. */
	if ((VENTURE_FILTER_OP_IN != op) && (VENTURE_FILTER_OP_NOT_IN != op) &&
	    (supplied != arity))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "The \"%s\" comparison on %s takes %u value%s, not %u",
		            venture_enum_to_nick(VENTURE_TYPE_FILTER_OP, (gint)op),
		            field, arity, (1 == arity) ? "" : "s", supplied);
		return FALSE;
	}

	if (((VENTURE_FILTER_OP_IN == op) || (VENTURE_FILTER_OP_NOT_IN == op)) &&
	    (0 == supplied))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "The \"%s\" comparison on %s needs at least one value",
		            venture_enum_to_nick(VENTURE_TYPE_FILTER_OP, (gint)op),
		            field);
		return FALSE;
	}

	entry = g_new0(VentureQueryFilterEntry, 1);
	entry->column = g_steal_pointer(&column);
	entry->value_type = value_type;
	entry->op = op;
	entry->values = g_ptr_array_new_with_free_func(g_free);

	if (NULL != values)
	{
		guint i;

		for (i = 0; i < values->len; i++)
		{
			g_ptr_array_add(entry->values,
			                g_strdup(g_ptr_array_index(values, i)));
		}
	}

	g_ptr_array_add(self->filters, entry);

	return TRUE;
}

gboolean
venture_query_add_filter_string(
	VentureQuery	 *self,
	const gchar	 *field,
	VentureFilterOp	  op,
	const gchar	 *value,
	GError		**error
){
	g_autoptr(GPtrArray) values = NULL;

	values = g_ptr_array_new();

	if (NULL != value)
		g_ptr_array_add(values, (gpointer)value);

	return venture_query_add_filter(self, field, op, values, error);
}

gboolean
venture_query_add_filter_int(
	VentureQuery	 *self,
	const gchar	 *field,
	VentureFilterOp	  op,
	gint64		  value,
	GError		**error
){
	g_autofree gchar *text = NULL;

	text = g_strdup_printf("%" G_GINT64_FORMAT, value);

	return venture_query_add_filter_string(self, field, op, text, error);
}

gboolean
venture_query_set_date_range(
	VentureQuery		 *self,
	const gchar		 *field,
	const VentureDateRange	 *range,
	GError			**error
){
	GDateTime *start;
	GDateTime *end;

	g_return_val_if_fail(VENTURE_IS_QUERY(self), FALSE);
	g_return_val_if_fail(NULL != field, FALSE);

	if (NULL == range)
		return TRUE;

	start = venture_date_range_get_start(range);
	end = venture_date_range_get_end(range);

	if (NULL != start)
	{
		g_autofree gchar *text = NULL;

		text = venture_time_to_string(start);

		if (!venture_query_add_filter_string(self, field,
		                                     VENTURE_FILTER_OP_GTE,
		                                     text, error))
			return FALSE;
	}

	if (NULL != end)
	{
		g_autofree gchar *text = NULL;

		text = venture_time_to_string(end);

		/* Strictly less than the end: the range is half open, so a
		 * record exactly on the boundary belongs to the next period
		 * and consecutive periods tile without double counting. */
		if (!venture_query_add_filter_string(self, field,
		                                     VENTURE_FILTER_OP_LT,
		                                     text, error))
			return FALSE;
	}

	return TRUE;
}

void
venture_query_set_search(
	VentureQuery	*self,
	const gchar	*text
){
	g_return_if_fail(VENTURE_IS_QUERY(self));

	g_free(self->search);
	self->search = venture_string_is_empty(text) ? NULL : g_strdup(text);
}

gboolean
venture_query_add_order(
	VentureQuery		 *self,
	const gchar		 *field,
	VentureSortDirection	  direction,
	GError			**error
){
	VentureQueryOrderEntry *entry;
	g_autofree gchar *column = NULL;
	GType value_type;

	g_return_val_if_fail(VENTURE_IS_QUERY(self), FALSE);

	if (!venture_query_resolve_field(self, field, &column, &value_type, error))
		return FALSE;

	entry = g_new0(VentureQueryOrderEntry, 1);
	entry->column = g_steal_pointer(&column);
	entry->direction = direction;

	g_ptr_array_add(self->orders, entry);

	return TRUE;
}

void
venture_query_set_limit(
	VentureQuery	*self,
	guint		 limit
){
	g_return_if_fail(VENTURE_IS_QUERY(self));

	self->limit = limit;
}

void
venture_query_set_offset(
	VentureQuery	*self,
	guint		 offset
){
	g_return_if_fail(VENTURE_IS_QUERY(self));

	self->offset = offset;
}

void
venture_query_set_organization(
	VentureQuery	*self,
	gint64		 organization_id
){
	g_return_if_fail(VENTURE_IS_QUERY(self));

	self->organization_id = organization_id;
}

void
venture_query_set_organization_tree(
	VentureQuery	*self,
	const gint64	*organization_ids,
	gsize		 n_organizations
){
	g_return_if_fail(VENTURE_IS_QUERY(self));

	g_clear_pointer(&self->organization_tree, g_array_unref);

	if ((NULL == organization_ids) || (0 == n_organizations))
		return;

	/* The first is the primary; the rest widen it. Keeping the primary in
	 * organization_id means every existing caller and every code path
	 * that reads it keeps working. */
	self->organization_id = organization_ids[0];

	if (n_organizations < 2)
		return;

	self->organization_tree = g_array_sized_new(FALSE, FALSE, sizeof(gint64),
	                                            (guint)(n_organizations - 1));
	g_array_append_vals(self->organization_tree, organization_ids + 1,
	                    (guint)(n_organizations - 1));
}

void
venture_query_set_include_deleted(
	VentureQuery	*self,
	gboolean	 include_deleted
){
	g_return_if_fail(VENTURE_IS_QUERY(self));

	self->include_deleted = include_deleted;
}

guint
venture_query_get_limit(VentureQuery *self)
{
	g_return_val_if_fail(VENTURE_IS_QUERY(self), 0);

	return self->limit;
}

/* --- JSON and query strings ---------------------------------------------- */

gboolean
venture_query_apply_json(
	VentureQuery	 *self,
	JsonNode	 *node,
	GError		**error
){
	JsonObject *object;

	g_return_val_if_fail(VENTURE_IS_QUERY(self), FALSE);

	if ((NULL == node) || JSON_NODE_HOLDS_NULL(node))
		return TRUE;

	if (!JSON_NODE_HOLDS_OBJECT(node))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "A query description must be an object");
		return FALSE;
	}

	object = json_node_get_object(node);

	if (json_object_has_member(object, "filters"))
	{
		JsonArray *filters;
		guint i;

		filters = json_object_get_array_member(object, "filters");

		for (i = 0; i < json_array_get_length(filters); i++)
		{
			g_autoptr(GPtrArray) values = NULL;
			JsonObject *filter;
			const gchar *field;
			const gchar *op_nick;
			gint op_value;

			filter = json_array_get_object_element(filters, i);
			field = venture_json_object_get_string(filter, "field", NULL);
			op_nick = venture_json_object_get_string(filter, "op", "eq");

			if (NULL == field)
			{
				g_set_error_literal(error, VENTURE_ERROR,
				                    VENTURE_ERROR_INVALID_ARGUMENT,
				                    "Every filter needs a \"field\"");
				return FALSE;
			}

			if (!venture_enum_from_nick(VENTURE_TYPE_FILTER_OP,
			                            op_nick, &op_value))
			{
				g_auto(GStrv) nicks = NULL;
				g_autofree gchar *list = NULL;

				nicks = venture_enum_list_nicks(VENTURE_TYPE_FILTER_OP);
				list = g_strjoinv(", ", nicks);

				g_set_error(error, VENTURE_ERROR,
				            VENTURE_ERROR_INVALID_ARGUMENT,
				            "\"%s\" is not a comparison. Use one of: %s",
				            op_nick, list);
				return FALSE;
			}

			values = g_ptr_array_new_with_free_func(g_free);

			/* Accept a single "value" or a "values" array, since
			 * both read naturally depending on the operator. */
			if (json_object_has_member(filter, "values"))
			{
				JsonArray *list;
				guint j;

				list = json_object_get_array_member(filter, "values");

				for (j = 0; j < json_array_get_length(list); j++)
				{
					g_ptr_array_add(values,
						venture_json_to_string(
							json_array_get_element(list, j),
							FALSE));
				}
			}
			else if (json_object_has_member(filter, "value"))
			{
				JsonNode *value_node;

				value_node = json_object_get_member(filter, "value");

				if (JSON_NODE_HOLDS_VALUE(value_node) &&
				    (G_TYPE_STRING == json_node_get_value_type(value_node)))
				{
					g_ptr_array_add(values,
						g_strdup(json_node_get_string(value_node)));
				}
				else
				{
					g_ptr_array_add(values,
						venture_json_to_string(value_node, FALSE));
				}
			}

			if (!venture_query_add_filter(self, field,
			                              (VentureFilterOp)op_value,
			                              values, error))
				return FALSE;
		}
	}

	if (json_object_has_member(object, "search"))
	{
		venture_query_set_search(self,
			venture_json_object_get_string(object, "search", NULL));
	}

	/* A period is expressed in the same vocabulary the CLI and the AI use
	 * everywhere else, so "this_month" means one thing in this system. */
	if (json_object_has_member(object, "period"))
	{
		g_autoptr(VentureDateRange) range = NULL;
		const gchar *period;
		const gchar *period_field;

		period = venture_json_object_get_string(object, "period", NULL);
		period_field = venture_json_object_get_string(object, "period_field",
		                                              "occurred_at");

		range = venture_date_range_parse(period, NULL, 1, error);

		if (NULL == range)
			return FALSE;

		if (!venture_query_set_date_range(self, period_field, range, error))
			return FALSE;
	}

	if (json_object_has_member(object, "order"))
	{
		JsonArray *orders;
		guint i;

		orders = json_object_get_array_member(object, "order");

		for (i = 0; i < json_array_get_length(orders); i++)
		{
			JsonObject *order;
			const gchar *field;
			const gchar *direction_nick;
			gint direction_value;

			order = json_array_get_object_element(orders, i);
			field = venture_json_object_get_string(order, "field", NULL);
			direction_nick = venture_json_object_get_string(order,
			                                                "direction", "asc");

			if (NULL == field)
				continue;

			if (!venture_enum_from_nick(VENTURE_TYPE_SORT_DIRECTION,
			                            direction_nick, &direction_value))
				direction_value = VENTURE_SORT_ASCENDING;

			if (!venture_query_add_order(self, field,
			                             (VentureSortDirection)direction_value,
			                             error))
				return FALSE;
		}
	}

	venture_query_set_limit(self,
		(guint)venture_json_object_get_int(object, "limit",
		                                   (gint64)self->limit));
	venture_query_set_offset(self,
		(guint)venture_json_object_get_int(object, "offset",
		                                   (gint64)self->offset));
	venture_query_set_include_deleted(self,
		venture_json_object_get_bool(object, "include_deleted",
		                             self->include_deleted));

	return TRUE;
}

gboolean
venture_query_apply_query_string(
	VentureQuery	 *self,
	GHashTable	 *params,
	GError		**error
){
	GHashTableIter iter;
	gpointer key;
	gpointer value;

	g_return_val_if_fail(VENTURE_IS_QUERY(self), FALSE);

	if (NULL == params)
		return TRUE;

	g_hash_table_iter_init(&iter, params);

	while (g_hash_table_iter_next(&iter, &key, &value))
	{
		g_autofree gchar *field = NULL;
		g_autoptr(GPtrArray) values = NULL;
		const gchar *name;
		const gchar *separator;
		const gchar *text;
		VentureFilterOp op;
		gint op_value;

		name = key;
		text = value;

		/* Paging and ordering are reserved names rather than filters. */
		if (0 == g_strcmp0(name, "limit"))
		{
			venture_query_set_limit(self,
				(guint)g_ascii_strtoull(text, NULL, 10));
			continue;
		}

		if (0 == g_strcmp0(name, "offset"))
		{
			venture_query_set_offset(self,
				(guint)g_ascii_strtoull(text, NULL, 10));
			continue;
		}

		if (0 == g_strcmp0(name, "search"))
		{
			venture_query_set_search(self, text);
			continue;
		}

		if (0 == g_strcmp0(name, "include_deleted"))
		{
			venture_query_set_include_deleted(self,
				(0 == g_strcmp0(text, "true")) ||
				(0 == g_strcmp0(text, "1")));
			continue;
		}

		if (0 == g_strcmp0(name, "order"))
		{
			g_auto(GStrv) fields = NULL;
			gsize i;

			fields = g_strsplit(text, ",", -1);

			for (i = 0; NULL != fields[i]; i++)
			{
				VentureSortDirection direction;
				const gchar *order_field;

				/* A leading minus reverses, which is the
				 * convention every REST API uses. */
				direction = ('-' == fields[i][0])
					? VENTURE_SORT_DESCENDING
					: VENTURE_SORT_ASCENDING;
				order_field = ('-' == fields[i][0])
					? (fields[i] + 1) : fields[i];

				if (!venture_query_add_order(self, order_field,
				                             direction, error))
					return FALSE;
			}

			continue;
		}

		if (0 == g_strcmp0(name, "period"))
		{
			g_autoptr(VentureDateRange) range = NULL;
			const gchar *period_field;

			period_field = g_hash_table_lookup(params, "period_field");

			range = venture_date_range_parse(text, NULL, 1, error);

			if (NULL == range)
				return FALSE;

			if (!venture_query_set_date_range(self,
				(NULL != period_field) ? period_field : "occurred_at",
				range, error))
				return FALSE;

			continue;
		}

		if (0 == g_strcmp0(name, "period_field"))
			continue;

		/* Everything else is a filter: `field=value` or
		 * `field__op=value`. */
		separator = strstr(name, "__");
		op = VENTURE_FILTER_OP_EQ;

		if (NULL != separator)
		{
			field = g_strndup(name, (gsize)(separator - name));

			if (!venture_enum_from_nick(VENTURE_TYPE_FILTER_OP,
			                            separator + 2, &op_value))
			{
				g_set_error(error, VENTURE_ERROR,
				            VENTURE_ERROR_INVALID_ARGUMENT,
				            "\"%s\" is not a comparison", separator + 2);
				return FALSE;
			}

			op = (VentureFilterOp)op_value;
		}
		else
		{
			field = g_strdup(name);
		}

		values = g_ptr_array_new_with_free_func(g_free);

		if ((VENTURE_FILTER_OP_IN == op) || (VENTURE_FILTER_OP_NOT_IN == op))
		{
			g_auto(GStrv) parts = NULL;
			gsize i;

			parts = g_strsplit(text, ",", -1);

			for (i = 0; NULL != parts[i]; i++)
				g_ptr_array_add(values, g_strdup(parts[i]));
		}
		else if (0 != venture_filter_op_arity(op))
		{
			g_ptr_array_add(values, g_strdup(text));
		}

		if (!venture_query_add_filter(self, field, op, values, error))
			return FALSE;
	}

	return TRUE;
}

/* --- Compilation --------------------------------------------------------- */

/*
 * Converts one operand to a bound parameter of the right SQL type. Values
 * arrive as text from every caller, so the column's declared type is what
 * decides how to bind them.
 */
static OrmValue *
venture_query_bind_value(
	GType		 value_type,
	const gchar	*text
){
	if (NULL == text)
		return orm_value_new_null();

	if (G_TYPE_BOOLEAN == value_type)
	{
		return orm_value_new_boolean((0 == g_ascii_strcasecmp(text, "true")) ||
		                             (0 == g_strcmp0(text, "1")) ||
		                             (0 == g_ascii_strcasecmp(text, "yes")));
	}

	if ((G_TYPE_INT64 == value_type) || (G_TYPE_INT == value_type))
		return orm_value_new_integer(g_ascii_strtoll(text, NULL, 10));

	if (G_TYPE_DOUBLE == value_type)
		return orm_value_new_float(g_ascii_strtod(text, NULL));

	/* Timestamps and enumerations are stored as text, and so are their
	 * comparison operands. */
	return orm_value_new_string(text);
}

/*
 * The SQL fragment for an operator. Fixed strings, chosen by enumeration
 * value, never assembled from caller input.
 */
static const gchar *
venture_query_op_sql(VentureFilterOp op)
{
	switch (op)
	{
	case VENTURE_FILTER_OP_NE:       return "<>";
	case VENTURE_FILTER_OP_LT:       return "<";
	case VENTURE_FILTER_OP_LTE:      return "<=";
	case VENTURE_FILTER_OP_GT:       return ">";
	case VENTURE_FILTER_OP_GTE:      return ">=";
	case VENTURE_FILTER_OP_LIKE:     return "LIKE";
	case VENTURE_FILTER_OP_ILIKE:    return "LIKE";
	case VENTURE_FILTER_OP_EQ:
	default:                         return "=";
	}
}

/*
 * Appends the placeholder for the next bound parameter. PostgreSQL numbers
 * them and SQLite does not, which is the one place the two dialects differ
 * in generated statements here.
 */
static void
venture_query_append_placeholder(
	GString		*sql,
	OrmDialectType	 dialect,
	guint		*index
){
	if (ORM_DIALECT_POSTGRES == dialect)
		g_string_append_printf(sql, "$%u", *index);
	else
		g_string_append_c(sql, '?');

	*index += 1;
}

static void
venture_query_append_filter(
	VentureQuery			*self,
	const VentureQueryFilterEntry	*entry,
	GString				*sql,
	OrmDialectType			 dialect,
	guint				*index,
	GList				**params
){
	g_autofree gchar *quoted = NULL;

	quoted = venture_schema_quote_identifier(entry->column);

	switch (entry->op)
	{
	case VENTURE_FILTER_OP_IS_NULL:
		g_string_append_printf(sql, "%s IS NULL", quoted);
		return;

	case VENTURE_FILTER_OP_NOT_NULL:
		g_string_append_printf(sql, "%s IS NOT NULL", quoted);
		return;

	case VENTURE_FILTER_OP_BETWEEN:
		g_string_append_printf(sql, "%s BETWEEN ", quoted);
		venture_query_append_placeholder(sql, dialect, index);
		g_string_append(sql, " AND ");
		venture_query_append_placeholder(sql, dialect, index);

		*params = g_list_append(*params,
			venture_query_bind_value(entry->value_type,
			                         g_ptr_array_index(entry->values, 0)));
		*params = g_list_append(*params,
			venture_query_bind_value(entry->value_type,
			                         g_ptr_array_index(entry->values, 1)));
		return;

	case VENTURE_FILTER_OP_IN:
	case VENTURE_FILTER_OP_NOT_IN:
	{
		guint i;

		g_string_append_printf(sql, "%s %sIN (", quoted,
			(VENTURE_FILTER_OP_NOT_IN == entry->op) ? "NOT " : "");

		for (i = 0; i < entry->values->len; i++)
		{
			if (i > 0)
				g_string_append(sql, ", ");

			venture_query_append_placeholder(sql, dialect, index);
			*params = g_list_append(*params,
				venture_query_bind_value(entry->value_type,
					g_ptr_array_index(entry->values, i)));
		}

		g_string_append_c(sql, ')');
		return;
	}

	case VENTURE_FILTER_OP_ILIKE:
	{
		const gchar *pattern;

		/* SQLite's LIKE is case insensitive for ASCII already;
		 * PostgreSQL needs ILIKE. Lowering both sides would defeat any
		 * index, so the dialect's own operator is used. */
		pattern = g_ptr_array_index(entry->values, 0);

		if (ORM_DIALECT_POSTGRES == dialect)
			g_string_append_printf(sql, "%s ILIKE ", quoted);
		else
			g_string_append_printf(sql, "%s LIKE ", quoted);

		venture_query_append_placeholder(sql, dialect, index);
		*params = g_list_append(*params, orm_value_new_string(pattern));
		return;
	}

	default:
		g_string_append_printf(sql, "%s %s ", quoted,
		                       venture_query_op_sql(entry->op));
		venture_query_append_placeholder(sql, dialect, index);
		*params = g_list_append(*params,
			venture_query_bind_value(entry->value_type,
				(entry->values->len > 0)
					? g_ptr_array_index(entry->values, 0)
					: NULL));
		return;
	}
}

/*
 * Appends the free-text search: an OR across every column the record type
 * flagged searchable. A type with none simply contributes nothing, which is
 * better than matching everything.
 */
static void
venture_query_append_search(
	VentureQuery	 *self,
	GString		 *sql,
	OrmDialectType	  dialect,
	guint		 *index,
	GList		**params,
	gboolean	 *first
){
	g_autoptr(VentureEntityClass) klass = NULL;
	g_autofree GParamSpec **properties = NULL;
	g_autofree gchar *pattern = NULL;
	guint n_properties;
	guint n_searchable;
	guint i;

	if (NULL == self->search)
		return;

	klass = g_type_class_ref(self->entity_type);
	properties = venture_entity_class_list_persistent_properties(klass,
	                                                             &n_properties);
	pattern = g_strdup_printf("%%%s%%", self->search);
	n_searchable = 0;

	for (i = 0; i < n_properties; i++)
	{
		VentureColumnFlags flags;
		g_autofree gchar *column = NULL;
		g_autofree gchar *quoted = NULL;

		flags = venture_entity_class_get_column_flags(klass,
		                                              properties[i]->name);

		if (0 == (flags & VENTURE_COLUMN_FLAG_SEARCHABLE))
			continue;

		if (G_TYPE_STRING != properties[i]->value_type)
			continue;

		if (0 == n_searchable)
		{
			g_string_append(sql, *first ? " WHERE (" : " AND (");
			*first = FALSE;
		}
		else
		{
			g_string_append(sql, " OR ");
		}

		column = venture_entity_property_to_column(properties[i]->name);
		quoted = venture_schema_quote_identifier(column);

		if (ORM_DIALECT_POSTGRES == dialect)
			g_string_append_printf(sql, "%s ILIKE ", quoted);
		else
			g_string_append_printf(sql, "%s LIKE ", quoted);

		venture_query_append_placeholder(sql, dialect, index);
		*params = g_list_append(*params, orm_value_new_string(pattern));

		n_searchable++;
	}

	if (n_searchable > 0)
		g_string_append_c(sql, ')');
}

gchar *
venture_query_to_sql(
	VentureQuery	 *self,
	OrmDialectType	  dialect,
	gboolean	  count_only,
	GList		**out_params
){
	g_autoptr(VentureEntity) prototype = NULL;
	g_autoptr(GString) sql = NULL;
	g_autofree gchar *table = NULL;
	GList *params = NULL;
	gboolean first;
	guint index;
	guint i;

	g_return_val_if_fail(VENTURE_IS_QUERY(self), NULL);
	g_return_val_if_fail(NULL != out_params, NULL);

	prototype = g_object_new(self->entity_type, NULL);
	table = venture_schema_quote_identifier(
		venture_entity_get_table_name(prototype));

	sql = g_string_new(NULL);
	g_string_append_printf(sql, "SELECT %s FROM %s",
	                       count_only ? "COUNT(*)" : "*", table);

	first = TRUE;
	index = 1;

	/* Soft deletion is invisible by default. Without this every list view
	 * would show the records the operator thought they had removed. */
	if (!self->include_deleted)
	{
		g_string_append(sql, " WHERE \"deleted_at\" IS NULL");
		first = FALSE;
	}

	if (0 != self->organization_id)
	{
		g_string_append(sql, first ? " WHERE " : " AND ");

		if ((NULL == self->organization_tree) ||
		    (0 == self->organization_tree->len))
		{
			g_string_append(sql, "\"organization_id\" = ");
			venture_query_append_placeholder(sql, dialect, &index);
			params = g_list_append(params,
				orm_value_new_integer(self->organization_id));
		}
		else
		{
			guint tree_index;

			/* Every identifier is bound, never interpolated, so a set
			 * of them is no more of an injection surface than one. */
			g_string_append(sql, "\"organization_id\" IN (");
			venture_query_append_placeholder(sql, dialect, &index);
			params = g_list_append(params,
				orm_value_new_integer(self->organization_id));

			for (tree_index = 0;
			     tree_index < self->organization_tree->len;
			     tree_index++)
			{
				g_string_append(sql, ", ");
				venture_query_append_placeholder(sql, dialect, &index);
				params = g_list_append(params,
					orm_value_new_integer(g_array_index(
						self->organization_tree, gint64, tree_index)));
			}

			g_string_append_c(sql, ')');
		}

		first = FALSE;
	}

	for (i = 0; i < self->filters->len; i++)
	{
		g_string_append(sql, first ? " WHERE " : " AND ");
		venture_query_append_filter(self,
		                            g_ptr_array_index(self->filters, i),
		                            sql, dialect, &index, &params);
		first = FALSE;
	}

	venture_query_append_search(self, sql, dialect, &index, &params, &first);

	if (!count_only)
	{
		if (self->orders->len > 0)
		{
			g_string_append(sql, " ORDER BY ");

			for (i = 0; i < self->orders->len; i++)
			{
				const VentureQueryOrderEntry *order;
				g_autofree gchar *quoted = NULL;

				order = g_ptr_array_index(self->orders, i);

				if (i > 0)
					g_string_append(sql, ", ");

				quoted = venture_schema_quote_identifier(order->column);
				g_string_append_printf(sql, "%s %s", quoted,
					(VENTURE_SORT_DESCENDING == order->direction)
						? "DESC" : "ASC");
			}
		}
		else
		{
			/* A stable default order matters for paging: without
			 * one, two requests for the same page can legitimately
			 * return different rows. */
			g_string_append(sql, " ORDER BY \"id\" DESC");
		}

		/* The limit and offset are integers held in the query object,
		 * never caller text, so formatting them is safe. */
		if (self->limit > 0)
			g_string_append_printf(sql, " LIMIT %u", self->limit);

		if (self->offset > 0)
		{
			/* SQLite requires a LIMIT before an OFFSET; -1 means
			 * unlimited and is the idiomatic way to say so. */
			if (0 == self->limit)
				g_string_append(sql, " LIMIT -1");

			g_string_append_printf(sql, " OFFSET %u", self->offset);
		}
	}

	*out_params = params;

	return g_string_free(g_steal_pointer(&sql), FALSE);
}

gchar *
venture_query_describe(VentureQuery *self)
{
	g_autoptr(VentureEntity) prototype = NULL;
	g_autoptr(GString) text = NULL;
	guint i;

	g_return_val_if_fail(VENTURE_IS_QUERY(self), NULL);

	prototype = g_object_new(self->entity_type, NULL);
	text = g_string_new(NULL);

	g_string_append_printf(text, "%s records",
	                       venture_entity_get_entity_name(prototype));

	for (i = 0; i < self->filters->len; i++)
	{
		const VentureQueryFilterEntry *entry;

		entry = g_ptr_array_index(self->filters, i);

		g_string_append_printf(text, "%s %s %s",
			(0 == i) ? " where" : " and",
			entry->column,
			venture_enum_to_nick(VENTURE_TYPE_FILTER_OP, (gint)entry->op));

		if (entry->values->len > 0)
		{
			g_autofree gchar *joined = NULL;

			g_ptr_array_add(entry->values, NULL);
			joined = g_strjoinv(", ", (gchar **)entry->values->pdata);
			g_ptr_array_remove_index(entry->values,
			                         entry->values->len - 1);

			g_string_append_printf(text, " %s", joined);
		}
	}

	if (NULL != self->search)
		g_string_append_printf(text, " matching \"%s\"", self->search);

	if (self->limit > 0)
		g_string_append_printf(text, ", at most %u", self->limit);

	return g_string_free(g_steal_pointer(&text), FALSE);
}
