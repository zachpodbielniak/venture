/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * venture-lql-source.c - `fetch TYPE`: one record type as an LQL source
 *
 * Copyright (C) 2026 Zach Podbielniak
 *
 * A VentureLqlSource reads one record type the way GET /api/v1/<type>
 * does, and nothing else (docs/lql.org):
 *
 *   rows    venture_database_find() on a VentureQuery, inside the caller's
 *           own reader (venture_access_policy_enter_snapshot()), so the
 *           access policy decides every row exactly as it does for the
 *           list API: organization membership, role-scoped visibility
 *           (sales, support and viewer see assigned records only),
 *           personal records. An unqualified fetch is scoped to the
 *           selected business and everything beneath it, as the list is.
 *   fields  venture_serializable_to_json(record, FALSE): the list API's
 *           own serialisation, so a sensitive field is not merely empty
 *           but absent, and the schema LQL sees (get_columns) is built
 *           from the same properties, minus the same sensitive ones.
 *           Money stays the API's {amount, currency, exponent, formatted}
 *           record -- amount in minor units, never a double -- dates and
 *           times become Timestamps, and every reference also gets a
 *           <ref>_label: the referenced record's display name, read
 *           through the same reader (NULL where the caller may not read
 *           it).
 *   filters the conjuncts LQL offers (the filter stages right after the
 *           fetch) that mean exactly what a VENTURE filter means become
 *           filters of the query (EXACT); a few that a VENTURE filter can
 *           only narrow become filters LQL still checks (HINT); the rest
 *           -- OR across fields, NOT of a group, other functions -- stay
 *           LQL's. See venture_lql_source_map() for the exact table.
 *   bound   query.max_rows: a fetch that would read more is an error that
 *           says so, never a silently shorter list.
 */

#include "lql/venture-lql-private.h"

#include <math.h>
#include <string.h>

/* How many ids one label look-up asks for at once. */
#define VENTURE_LQL_LABEL_BATCH (200)

/* --- The schema ----------------------------------------------------------- */

/*
 * What a field of a row is.
 *
 * PLAIN      a persistent property, valued from the list API's JSON
 * LABEL      <ref>_label, the display name of what a reference points at
 * DISPLAY    display_name, which the list API sends with every record
 * ATTRIBUTES the record's custom attributes, as a Record
 */
typedef enum
{
	VENTURE_LQL_FIELD_PLAIN = 0,
	VENTURE_LQL_FIELD_LABEL,
	VENTURE_LQL_FIELD_DISPLAY,
	VENTURE_LQL_FIELD_ATTRIBUTES
} VentureLqlFieldRole;

/*
 * One field of the rows a source produces.
 *
 * @name: the field's name in LQL, the list API's member name (snake_case)
 * @property: (nullable): the GObject property, for a PLAIN field
 * @role: what the field is
 * @value_type: the property's GType (G_TYPE_INVALID for computed fields)
 * @lql_type: the LQL type its non-NULL values have
 * @description: what get_columns tells an editor
 * @reference_type: for a reference, the GType it points at, or
 *   G_TYPE_INVALID when that type's module is off
 * @label: (nullable): for a reference, the name of its label field
 * @reference: (nullable): for a LABEL, the reference field it labels
 */
typedef struct
{
	gchar			*name;
	gchar			*property;
	VentureLqlFieldRole	 role;
	GType			 value_type;
	LqlValueType		 lql_type;
	gchar			*description;
	GType			 reference_type;
	gchar			*label;
	gchar			*reference;
} VentureLqlField;

static void
venture_lql_field_free(gpointer data)
{
	VentureLqlField *field = data;

	g_free(field->name);
	g_free(field->property);
	g_free(field->description);
	g_free(field->label);
	g_free(field->reference);
	g_free(field);
}

struct _VentureLqlSource
{
	GObject			 parent_instance;

	VentureLqlCaller	*caller;	/* owned reference */
	GType			 entity_type;
	gchar			*name;

	/* Built on first use: most sources of an environment are never read. */
	GPtrArray		*fields;	/* VentureLqlField, in row order */
	GHashTable		*by_name;	/* name -> VentureLqlField (borrowed) */
};

static void venture_lql_source_iface_init(LqlSourceInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(VentureLqlSource, venture_lql_source, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(LQL_TYPE_SOURCE, venture_lql_source_iface_init))

static void
venture_lql_source_finalize(GObject *object)
{
	VentureLqlSource *self = VENTURE_LQL_SOURCE(object);

	g_clear_pointer(&self->caller, venture_lql_caller_unref);
	g_clear_pointer(&self->name, g_free);
	g_clear_pointer(&self->fields, g_ptr_array_unref);
	g_clear_pointer(&self->by_name, g_hash_table_unref);

	G_OBJECT_CLASS(venture_lql_source_parent_class)->finalize(object);
}

static void
venture_lql_source_class_init(VentureLqlSourceClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = venture_lql_source_finalize;
}

static void
venture_lql_source_init(VentureLqlSource *self)
{
	(void)self;
}

VentureLqlSource *
venture_lql_source_new(
	VentureLqlCaller	*caller,
	GType			 entity_type,
	const gchar		*name
){
	VentureLqlSource *self;

	self = g_object_new(VENTURE_TYPE_LQL_SOURCE, NULL);
	self->caller = venture_lql_caller_ref(caller);
	self->entity_type = entity_type;
	self->name = g_strdup(name);

	return self;
}

GType
venture_lql_source_get_entity_type(VentureLqlSource *self)
{
	g_return_val_if_fail(VENTURE_IS_LQL_SOURCE(self), G_TYPE_INVALID);

	return self->entity_type;
}

/* The LQL type a property's values have, from the GType the list API
 * serialises it from (venture_json_node_from_value()). */
static LqlValueType
venture_lql_type_for(GType value_type)
{
	if (VENTURE_TYPE_MONEY == value_type)
		return LQL_VALUE_TYPE_RECORD;
	if (G_TYPE_DATE_TIME == value_type)
		return LQL_VALUE_TYPE_TIMESTAMP;
	if (G_TYPE_BOOLEAN == value_type)
		return LQL_VALUE_TYPE_BOOLEAN;
	if ((G_TYPE_INT64 == value_type) || (G_TYPE_INT == value_type) ||
	    (G_TYPE_UINT == value_type) || (G_TYPE_UINT64 == value_type) ||
	    G_TYPE_IS_FLAGS(value_type))
		return LQL_VALUE_TYPE_INT;
	if ((G_TYPE_DOUBLE == value_type) || (G_TYPE_FLOAT == value_type))
		return LQL_VALUE_TYPE_DOUBLE;
	if ((G_TYPE_STRING == value_type) || G_TYPE_IS_ENUM(value_type))
		return LQL_VALUE_TYPE_STRING;
	if (G_TYPE_STRV == value_type)
		return LQL_VALUE_TYPE_ARRAY;

	return LQL_VALUE_TYPE_ANY;
}

/* Adds a field to the schema; the schema owns it. */
static VentureLqlField *
venture_lql_source_add_field(
	VentureLqlSource	*self,
	const gchar		*name,
	VentureLqlFieldRole	 role,
	LqlValueType		 lql_type,
	const gchar		*description
){
	VentureLqlField *field;

	field = g_new0(VentureLqlField, 1);
	field->name = g_strdup(name);
	field->role = role;
	field->value_type = G_TYPE_INVALID;
	field->lql_type = lql_type;
	field->description = g_strdup(description);
	field->reference_type = G_TYPE_INVALID;

	g_ptr_array_add(self->fields, field);
	g_hash_table_insert(self->by_name, field->name, field);

	return field;
}

/*
 * Builds the schema: the properties the list API serialises, in its
 * order, with each reference followed by its label, then display_name and
 * attributes. A sensitive property is not a field at all.
 */
static void
venture_lql_source_ensure_fields(VentureLqlSource *self)
{
	g_autoptr(VentureEntityClass) klass = NULL;
	g_autofree GParamSpec **properties = NULL;
	g_autoptr(GHashTable) labels = NULL;
	g_autoptr(GPtrArray) specs = NULL;
	VentureEntity *prototype;
	guint n_properties;
	guint i;

	if (NULL != self->fields)
		return;

	self->fields = g_ptr_array_new_with_free_func(venture_lql_field_free);
	self->by_name = g_hash_table_new(g_str_hash, g_str_equal);

	/* The labels `describe` shows, as each column's description. */
	labels = g_hash_table_new(g_str_hash, g_str_equal);
	prototype = venture_entity_registry_get_prototype(self->caller->registry, self->name);

	if (NULL != prototype)
	{
		specs = venture_entity_get_field_specs(prototype);

		for (i = 0; i < specs->len; i++)
		{
			VentureFieldSpec *spec = g_ptr_array_index(specs, i);

			g_hash_table_insert(labels, (gpointer)venture_field_spec_get_name(spec),
			                    (gpointer)venture_field_spec_get_label(spec));
		}
	}

	klass = g_type_class_ref(self->entity_type);
	properties = venture_entity_class_list_persistent_properties(klass, &n_properties);

	for (i = 0; i < n_properties; i++)
	{
		GParamSpec *pspec = properties[i];
		VentureColumnFlags flags;
		VentureLqlField *field;
		g_autofree gchar *column = NULL;
		const gchar *description;
		const gchar *reference;

		flags = venture_entity_class_get_column_flags(klass, pspec->name);

		/* Exactly what venture_serializable_to_json(..., FALSE) leaves
		 * out: a sensitive field, and the attributes bag's own string. */
		if ((0 != (flags & VENTURE_COLUMN_FLAG_SENSITIVE)) ||
		    (0 == g_strcmp0(pspec->name, "attributes")))
			continue;

		column = venture_entity_property_to_column(pspec->name);
		description = g_hash_table_lookup(labels, pspec->name);
		field = venture_lql_source_add_field(self, column, VENTURE_LQL_FIELD_PLAIN,
			venture_lql_type_for(pspec->value_type),
			(NULL != description) ? description : g_param_spec_get_nick(pspec));
		field->property = g_strdup(pspec->name);
		field->value_type = pspec->value_type;

		/* A reference is an id plus <ref>_label, unless the type
		 * already has a field of that name. */
		reference = venture_entity_class_get_reference(klass, pspec->name);

		if ((NULL != reference) && (LQL_VALUE_TYPE_INT == field->lql_type))
		{
			g_autofree gchar *base = NULL;
			g_autofree gchar *label = NULL;
			g_autofree gchar *label_property = NULL;

			base = g_str_has_suffix(column, "_id")
				? g_strndup(column, strlen(column) - 3) : g_strdup(column);
			label = g_strconcat(base, "_label", NULL);
			label_property = venture_entity_column_to_property(label);

			field->reference_type = venture_entity_registry_lookup(self->caller->registry, reference);

			if (!g_hash_table_contains(self->by_name, label) &&
			    (NULL == g_object_class_find_property(G_OBJECT_CLASS(klass), label_property)))
			{
				g_autofree gchar *text = g_strdup_printf("The name of the %s it refers to", reference);
				VentureLqlField *label_field;

				field->label = g_strdup(label);
				label_field = venture_lql_source_add_field(self, label,
					VENTURE_LQL_FIELD_LABEL, LQL_VALUE_TYPE_STRING, text);
				label_field->reference = g_strdup(column);
				label_field->reference_type = field->reference_type;
			}
		}
	}

	if (!g_hash_table_contains(self->by_name, "display_name"))
		venture_lql_source_add_field(self, "display_name", VENTURE_LQL_FIELD_DISPLAY,
			LQL_VALUE_TYPE_STRING, "The record's name, as lists show it");

	if (!g_hash_table_contains(self->by_name, "attributes"))
		venture_lql_source_add_field(self, "attributes", VENTURE_LQL_FIELD_ATTRIBUTES,
			LQL_VALUE_TYPE_RECORD, "Custom attributes; NULL when there are none");
}

/* --- JSON to LQL ---------------------------------------------------------- */

/* A JSON value as the LQL value of the same shape. */
static LqlValue *
venture_lql_from_json(JsonNode *node)
{
	if ((NULL == node) || JSON_NODE_HOLDS_NULL(node))
		return lql_value_new_null();

	if (JSON_NODE_HOLDS_OBJECT(node))
	{
		g_autoptr(LqlRecord) record = lql_record_new();
		g_autoptr(GList) members = json_object_get_members(json_node_get_object(node));
		GList *iter;

		for (iter = members; NULL != iter; iter = iter->next)
		{
			lql_record_take(record, iter->data, venture_lql_from_json(
				json_object_get_member(json_node_get_object(node), iter->data)));
		}

		return lql_value_new_record(record);
	}

	if (JSON_NODE_HOLDS_ARRAY(node))
	{
		g_autoptr(GPtrArray) items = g_ptr_array_new_with_free_func((GDestroyNotify)lql_value_unref);
		JsonArray *array = json_node_get_array(node);
		guint i;

		for (i = 0; i < json_array_get_length(array); i++)
			g_ptr_array_add(items, venture_lql_from_json(json_array_get_element(array, i)));

		return lql_value_new_array(items);
	}

	switch (json_node_get_value_type(node))
	{
	case G_TYPE_INT64:
		return lql_value_new_int(json_node_get_int(node));
	case G_TYPE_DOUBLE:
		return lql_value_new_double(json_node_get_double(node));
	case G_TYPE_BOOLEAN:
		return lql_value_new_boolean(json_node_get_boolean(node));
	case G_TYPE_STRING:
		return lql_value_new_string(json_node_get_string(node));
	default:
		return lql_value_new_null();
	}
}

/* One member of a record's JSON as the value of @field. A time is sent as
 * the text VENTURE stores; LQL gets a Timestamp. */
static LqlValue *
venture_lql_field_value(
	const VentureLqlField	*field,
	JsonNode		*member
){
	if ((NULL != member) && (LQL_VALUE_TYPE_TIMESTAMP == field->lql_type) &&
	    JSON_NODE_HOLDS_VALUE(member) && (G_TYPE_STRING == json_node_get_value_type(member)))
	{
		g_autoptr(GDateTime) when = venture_time_from_string(json_node_get_string(member), NULL);

		if (NULL == when)
			return lql_value_new_null();

		return lql_value_new_timestamp(g_date_time_to_unix_usec(when));
	}

	return venture_lql_from_json(member);
}

/* One record as a row: the list API's JSON of it, field by field. Labels
 * are NULL here; venture_lql_source_fill_labels() sets them. */
static LqlRecord *
venture_lql_source_row(
	VentureLqlSource	*self,
	VentureEntity		*entity
){
	g_autoptr(JsonNode) json = NULL;
	JsonObject *object;
	LqlRecord *row;
	guint i;

	json = venture_serializable_to_json(VENTURE_SERIALIZABLE(entity), FALSE);
	object = json_node_get_object(json);
	row = lql_record_new();

	for (i = 0; i < self->fields->len; i++)
	{
		VentureLqlField *field = g_ptr_array_index(self->fields, i);
		JsonNode *member = NULL;

		if ((VENTURE_LQL_FIELD_LABEL != field->role) && json_object_has_member(object, field->name))
			member = json_object_get_member(object, field->name);

		lql_record_take(row, field->name, venture_lql_field_value(field, member));
	}

	return row;
}

/*
 * Sets every <ref>_label of @rows: one look-up per referenced type for up
 * to VENTURE_LQL_LABEL_BATCH ids at a time, made in the caller's reader
 * (the caller must already be inside it). A record the caller may not
 * read, or of a type they may not open, leaves its label NULL: a label
 * never tells anybody more than the record would.
 */
static gboolean
venture_lql_source_fill_labels(
	VentureLqlSource	 *self,
	GPtrArray		 *rows,
	GError			**error
){
	guint f;

	for (f = 0; f < self->fields->len; f++)
	{
		VentureLqlField *field = g_ptr_array_index(self->fields, f);
		g_autoptr(GHashTable) names = NULL;
		g_autoptr(GPtrArray) wanted = NULL;
		guint i;

		if ((VENTURE_LQL_FIELD_LABEL != field->role) ||
		    (G_TYPE_INVALID == field->reference_type) ||
		    !venture_lql_caller_can_read(self->caller, field->reference_type))
			continue;

		/* The distinct ids the rows refer to. */
		names = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);
		wanted = g_ptr_array_new_with_free_func(g_free);

		for (i = 0; i < rows->len; i++)
		{
			LqlValue *id = lql_record_get(g_ptr_array_index(rows, i), field->reference);
			gint64 *key;

			if ((LQL_VALUE_TYPE_INT != lql_value_get_value_type(id)) ||
			    (lql_value_get_int(id) <= 0))
				continue;

			key = g_new(gint64, 1);
			*key = lql_value_get_int(id);

			if (g_hash_table_contains(names, key))
			{
				g_free(key);
				continue;
			}

			g_hash_table_insert(names, key, NULL);
			g_ptr_array_add(wanted, g_strdup_printf("%" G_GINT64_FORMAT, *key));
		}

		/* Their display names, a batch at a time. */
		for (i = 0; i < wanted->len; i += VENTURE_LQL_LABEL_BATCH)
		{
			g_autoptr(VentureQuery) query = venture_query_new(field->reference_type);
			g_autoptr(GPtrArray) batch = g_ptr_array_new();
			g_autoptr(GPtrArray) found = NULL;
			guint j;

			for (j = i; (j < wanted->len) && (j < i + VENTURE_LQL_LABEL_BATCH); j++)
				g_ptr_array_add(batch, g_ptr_array_index(wanted, j));

			if (!venture_query_add_filter(query, "id", VENTURE_FILTER_OP_IN, batch, error))
				return FALSE;

			venture_query_set_limit(query, batch->len);
			found = venture_database_find(self->caller->database, query, error);

			if (NULL == found)
				return FALSE;

			for (j = 0; j < found->len; j++)
			{
				VentureEntity *entity = g_ptr_array_index(found, j);
				gint64 key = venture_entity_get_id(entity);

				g_hash_table_replace(names, g_memdup2(&key, sizeof key),
				                     venture_entity_get_display_name(entity));
			}
		}

		for (i = 0; i < rows->len; i++)
		{
			LqlRecord *row = g_ptr_array_index(rows, i);
			LqlValue *id = lql_record_get(row, field->reference);
			gint64 key;

			if (LQL_VALUE_TYPE_INT != lql_value_get_value_type(id))
				continue;

			key = lql_value_get_int(id);
			lql_record_take(row, field->name,
				lql_value_new_string(g_hash_table_lookup(names, &key)));
		}
	}

	return TRUE;
}

/* --- Pushdown ------------------------------------------------------------- */

/*
 * What a predicate's field side names: a field of the schema, or the
 * amount or currency of a money field (`total.amount`, `total.currency`).
 */
typedef struct
{
	VentureLqlField	*field;
	gboolean	 amount;
	gboolean	 currency;
} VentureLqlTarget;

static gboolean
venture_lql_source_resolve(
	VentureLqlSource	*self,
	LqlNode			*node,
	VentureLqlTarget	*out
){
	const gchar * const *path;
	guint n;

	memset(out, 0, sizeof *out);

	if (LQL_NODE_FIELD != lql_node_get_kind(node))
		return FALSE;

	path = lql_node_get_path(node);
	n = (NULL != path) ? g_strv_length((gchar **)path) : 0;

	if ((n < 1) || (n > 2))
		return FALSE;

	out->field = g_hash_table_lookup(self->by_name, path[0]);

	/* Only a stored property can be a filter; a label, display_name or
	 * an attribute is computed and stays LQL's. */
	if ((NULL == out->field) || (VENTURE_LQL_FIELD_PLAIN != out->field->role))
		return FALSE;

	if (1 == n)
		return TRUE;

	if (VENTURE_TYPE_MONEY != out->field->value_type)
		return FALSE;

	out->amount = (0 == g_strcmp0(path[1], "amount"));
	out->currency = (0 == g_strcmp0(path[1], "currency"));

	return out->amount || out->currency;
}

/*
 * Whether @node can be evaluated once, without a row: no field, no
 * sub-pipeline, no function that reads the row. now() and ago() are
 * marked volatile but fixed for one execution, so they qualify, as do
 * VENTURE's own functions.
 */
static gboolean
venture_lql_is_constant(
	LqlEnvironment	*environment,
	LqlNode		*node
){
	guint i;

	switch (lql_node_get_kind(node))
	{
	case LQL_NODE_FIELD:
	case LQL_NODE_PIPELINE:
		return FALSE;

	case LQL_NODE_CALL:
	{
		LqlFunction *function = lql_environment_lookup_function(environment, lql_node_get_name(node));

		if ((NULL == function) ||
		    (0 != (lql_function_get_flags(function) & LQL_FUNCTION_FLAG_NEEDS_ROW)))
			return FALSE;
		break;
	}

	default:
		break;
	}

	for (i = 0; i < lql_node_get_n_children(node); i++)
	{
		if (!venture_lql_is_constant(environment, lql_node_get_child(node, i)))
			return FALSE;
	}

	return TRUE;
}

/* The value of a constant operand, or NULL when it is not one or fails. */
static LqlValue *
venture_lql_operand(
	LqlFetchRequest	*request,
	LqlNode		*node
){
	g_autoptr(GError) error = NULL;

	if (!venture_lql_is_constant(lql_fetch_request_get_environment(request), node))
		return NULL;

	return lql_fetch_request_evaluate(request, node, &error);
}

/*
 * The text a VENTURE filter on @target compares with, for an LQL operand
 * @value; NULL when the comparison would not mean the same thing:
 *
 *   Int field, money amount   an Int operand, any comparison
 *   Double field              an Int or finite Double operand, any comparison
 *   Boolean field             a Boolean operand, == and != only
 *   String or enum field      a non-empty String, == and != only (an empty
 *                             one means "NULL or empty" to VENTURE, and
 *                             strings order by collation in PostgreSQL)
 *   money currency            a non-empty String, == only
 *   Timestamp field           a Timestamp, < <= > >= only (stored text
 *                             compares by instant only through the range
 *                             operators, which pad the fraction)
 *
 * A value of another type is never pushed: LQL answers == between types
 * FALSE and != TRUE, which no SQL comparison reproduces.
 */
static gchar *
venture_lql_operand_text(
	const VentureLqlTarget	*target,
	LqlValue		*value,
	gboolean		 ordering
){
	LqlValueType type = lql_value_get_value_type(value);
	GType value_type = target->field->value_type;

	if (target->currency)
	{
		if (ordering || (LQL_VALUE_TYPE_STRING != type) ||
		    ('\0' == lql_value_get_string(value)[0]))
			return NULL;
		return g_strdup(lql_value_get_string(value));
	}

	if (target->amount || (G_TYPE_INT64 == value_type) || (G_TYPE_INT == value_type))
	{
		if (LQL_VALUE_TYPE_INT != type)
			return NULL;
		return g_strdup_printf("%" G_GINT64_FORMAT, lql_value_get_int(value));
	}

	if (G_TYPE_DOUBLE == value_type)
	{
		gchar buffer[G_ASCII_DTOSTR_BUF_SIZE];
		gdouble number;

		if ((LQL_VALUE_TYPE_INT != type) && (LQL_VALUE_TYPE_DOUBLE != type))
			return NULL;
		if (!lql_value_get_number(value, &number) || !isfinite(number))
			return NULL;
		return g_strdup(g_ascii_dtostr(buffer, sizeof buffer, number));
	}

	if (G_TYPE_BOOLEAN == value_type)
	{
		if (ordering || (LQL_VALUE_TYPE_BOOLEAN != type))
			return NULL;
		return g_strdup(lql_value_get_boolean(value) ? "true" : "false");
	}

	if ((G_TYPE_STRING == value_type) || G_TYPE_IS_ENUM(value_type))
	{
		if (ordering || (LQL_VALUE_TYPE_STRING != type) ||
		    ('\0' == lql_value_get_string(value)[0]))
			return NULL;
		return g_strdup(lql_value_get_string(value));
	}

	if (G_TYPE_DATE_TIME == value_type)
	{
		g_autoptr(GDateTime) when = NULL;

		if (!ordering || (LQL_VALUE_TYPE_TIMESTAMP != type))
			return NULL;
		when = g_date_time_new_from_unix_utc_usec(lql_value_get_timestamp(value));
		return (NULL != when) ? g_date_time_format(when, "%Y-%m-%dT%H:%M:%S.%fZ") : NULL;
	}

	return NULL;
}

/* LQL's comparison operators as VENTURE's, with the field on the left. */
static gboolean
venture_lql_filter_op(
	LqlOperator		 op,
	gboolean		 mirrored,
	VentureFilterOp		*out
){
	switch (op)
	{
	case LQL_OP_EQ: *out = VENTURE_FILTER_OP_EQ; return TRUE;
	case LQL_OP_NE: *out = VENTURE_FILTER_OP_NE; return TRUE;
	case LQL_OP_LT: *out = mirrored ? VENTURE_FILTER_OP_GT : VENTURE_FILTER_OP_LT; return TRUE;
	case LQL_OP_LE: *out = mirrored ? VENTURE_FILTER_OP_GTE : VENTURE_FILTER_OP_LTE; return TRUE;
	case LQL_OP_GT: *out = mirrored ? VENTURE_FILTER_OP_LT : VENTURE_FILTER_OP_GT; return TRUE;
	case LQL_OP_GE: *out = mirrored ? VENTURE_FILTER_OP_LTE : VENTURE_FILTER_OP_GTE; return TRUE;
	default: return FALSE;
	}
}

/* The property a filter on @target names (VENTURE resolves a money
 * property to its amount column). */
static const gchar *
venture_lql_target_property(const VentureLqlTarget *target)
{
	return target->field->property;
}

/* field OP constant, either way round. */
static LqlPushdown
venture_lql_map_comparison(
	VentureLqlSource	*self,
	LqlFetchRequest		*request,
	LqlNode			*node,
	VentureQuery		*query,
	gint64			*out_organization
){
	g_autoptr(LqlValue) value = NULL;
	g_autofree gchar *text = NULL;
	VentureLqlTarget target;
	VentureFilterOp op;
	LqlNode *other;
	gboolean mirrored = FALSE;

	if (venture_lql_source_resolve(self, lql_node_get_child(node, 0), &target))
		other = lql_node_get_child(node, 1);
	else if (venture_lql_source_resolve(self, lql_node_get_child(node, 1), &target))
	{
		other = lql_node_get_child(node, 0);
		mirrored = TRUE;
	}
	else
		return LQL_PUSHDOWN_NONE;

	if (!venture_lql_filter_op(lql_node_get_operator(node), mirrored, &op))
		return LQL_PUSHDOWN_NONE;

	/* A whole money value is a record: it compares only as one. */
	if ((VENTURE_TYPE_MONEY == target.field->value_type) && !target.amount && !target.currency)
		return LQL_PUSHDOWN_NONE;

	value = venture_lql_operand(request, other);

	if (NULL == value)
		return LQL_PUSHDOWN_NONE;

	text = venture_lql_operand_text(&target, value,
		(VENTURE_FILTER_OP_EQ != op) && (VENTURE_FILTER_OP_NE != op));

	if (NULL == text)
		return LQL_PUSHDOWN_NONE;

	if (target.currency)
	{
		if ((VENTURE_FILTER_OP_EQ != op) ||
		    !venture_query_add_money_currency_as_read(query, venture_lql_target_property(&target), text, NULL))
			return LQL_PUSHDOWN_NONE;
		return LQL_PUSHDOWN_EXACT;
	}

	if (!venture_query_add_filter_string_as_read(query, venture_lql_target_property(&target), op, text, NULL))
		return LQL_PUSHDOWN_NONE;

	/* `organization_id == N` is what ?organization_id=N is to the list
	 * API: it replaces the selected-business scope. */
	if ((NULL != out_organization) && (VENTURE_FILTER_OP_EQ == op) &&
	    (0 == g_strcmp0(target.field->name, "organization_id")))
		*out_organization = lql_value_get_int(value);

	return LQL_PUSHDOWN_EXACT;
}

/* field in [constants] and field not in [constants]. */
static LqlPushdown
venture_lql_map_in(
	VentureLqlSource	*self,
	LqlFetchRequest		*request,
	LqlNode			*node,
	VentureQuery		*query
){
	g_autoptr(LqlValue) list = NULL;
	g_autoptr(GPtrArray) texts = NULL;
	VentureLqlTarget target;
	GPtrArray *items;
	guint i;

	if (!venture_lql_source_resolve(self, lql_node_get_child(node, 0), &target) ||
	    target.currency ||
	    ((VENTURE_TYPE_MONEY == target.field->value_type) && !target.amount))
		return LQL_PUSHDOWN_NONE;

	list = venture_lql_operand(request, lql_node_get_child(node, 1));

	if ((NULL == list) || (LQL_VALUE_TYPE_ARRAY != lql_value_get_value_type(list)))
		return LQL_PUSHDOWN_NONE;

	items = lql_value_get_array(list);

	if ((NULL == items) || (0 == items->len))
		return LQL_PUSHDOWN_NONE;

	texts = g_ptr_array_new_with_free_func(g_free);

	for (i = 0; i < items->len; i++)
	{
		gchar *text = venture_lql_operand_text(&target, g_ptr_array_index(items, i), FALSE);

		/* A NULL, or a value of another type, anywhere in the list
		 * gives LQL an answer IN does not; the whole conjunct stays. */
		if (NULL == text)
			return LQL_PUSHDOWN_NONE;

		g_ptr_array_add(texts, text);
	}

	if (!venture_query_add_filter_as_read(query, venture_lql_target_property(&target),
	        lql_node_get_negated(node) ? VENTURE_FILTER_OP_NOT_IN : VENTURE_FILTER_OP_IN,
	        texts, NULL))
		return LQL_PUSHDOWN_NONE;

	return LQL_PUSHDOWN_EXACT;
}

/* Whether @node is the constant FALSE (an explicit "case-sensitive"). */
static gboolean
venture_lql_is_false(
	LqlFetchRequest	*request,
	LqlNode		*node
){
	g_autoptr(LqlValue) value = venture_lql_operand(request, node);

	return (NULL != value) && (LQL_VALUE_TYPE_BOOLEAN == lql_value_get_value_type(value)) &&
	       !lql_value_get_boolean(value);
}

/*
 * The function forms: is_null(f), is_not_null(f), between(f, lo, hi), and
 * contains / starts_with / ends_with / like on a string field.
 *
 * The text matches are pushed as a HINT, never EXACT: VENTURE's "like" is
 * SQL LIKE, which is case-insensitive for ASCII on SQLite and treats %
 * and _ in the text as wildcards, so it can only keep more rows than LQL
 * would; LQL checks every row it returns. A pattern with a backslash is
 * not pushed at all (LQL escapes with it, SQLite does not), nor is a
 * case-insensitive match (SQL folds case differently from LQL).
 */
static LqlPushdown
venture_lql_map_call(
	VentureLqlSource	*self,
	LqlFetchRequest		*request,
	LqlNode			*node,
	VentureQuery		*query
){
	const gchar *name = lql_node_get_name(node);
	guint n = lql_node_get_n_children(node);
	VentureLqlTarget target;

	if ((n < 1) || !venture_lql_source_resolve(self, lql_node_get_child(node, 0), &target) ||
	    target.currency)
		return LQL_PUSHDOWN_NONE;

	/* is_null(f), is_not_null(f): a money value is NULL in all three of
	 * its columns at once, so the amount column answers for it. */
	if ((1 == n) && ((0 == g_strcmp0(name, "is_null")) || (0 == g_strcmp0(name, "is_not_null"))))
	{
		if (!venture_query_add_filter_as_read(query, venture_lql_target_property(&target),
		        (0 == g_strcmp0(name, "is_null")) ? VENTURE_FILTER_OP_IS_NULL : VENTURE_FILTER_OP_NOT_NULL,
		        NULL, NULL))
			return LQL_PUSHDOWN_NONE;
		return LQL_PUSHDOWN_EXACT;
	}

	/* between(f, lo, hi), both ends included, as SQL's BETWEEN. */
	if ((3 == n) && (0 == g_strcmp0(name, "between")))
	{
		g_autoptr(LqlValue) low = NULL;
		g_autoptr(LqlValue) high = NULL;
		g_autoptr(GPtrArray) texts = NULL;
		gchar *low_text;
		gchar *high_text;

		if ((VENTURE_TYPE_MONEY == target.field->value_type) && !target.amount)
			return LQL_PUSHDOWN_NONE;

		low = venture_lql_operand(request, lql_node_get_child(node, 1));
		high = venture_lql_operand(request, lql_node_get_child(node, 2));

		if ((NULL == low) || (NULL == high))
			return LQL_PUSHDOWN_NONE;

		texts = g_ptr_array_new_with_free_func(g_free);
		low_text = venture_lql_operand_text(&target, low, TRUE);
		high_text = venture_lql_operand_text(&target, high, TRUE);

		if ((NULL == low_text) || (NULL == high_text))
		{
			g_free(low_text);
			g_free(high_text);
			return LQL_PUSHDOWN_NONE;
		}

		g_ptr_array_add(texts, low_text);
		g_ptr_array_add(texts, high_text);

		if (!venture_query_add_filter_as_read(query, venture_lql_target_property(&target),
		                                      VENTURE_FILTER_OP_BETWEEN, texts, NULL))
			return LQL_PUSHDOWN_NONE;
		return LQL_PUSHDOWN_EXACT;
	}

	/* The text matches, case-sensitive only. */
	if (((2 == n) || ((3 == n) && venture_lql_is_false(request, lql_node_get_child(node, 2)))) &&
	    ((0 == g_strcmp0(name, "contains")) || (0 == g_strcmp0(name, "starts_with")) ||
	     (0 == g_strcmp0(name, "ends_with")) || (0 == g_strcmp0(name, "like"))))
	{
		g_autoptr(LqlValue) part = NULL;
		g_autofree gchar *pattern = NULL;
		const gchar *text;

		if (target.amount || (G_TYPE_STRING != target.field->value_type))
			return LQL_PUSHDOWN_NONE;

		part = venture_lql_operand(request, lql_node_get_child(node, 1));

		if ((NULL == part) || (LQL_VALUE_TYPE_STRING != lql_value_get_value_type(part)))
			return LQL_PUSHDOWN_NONE;

		text = lql_value_get_string(part);

		if (NULL != strchr(text, '\\'))
			return LQL_PUSHDOWN_NONE;

		if (0 == g_strcmp0(name, "contains"))
			pattern = g_strconcat("%", text, "%", NULL);
		else if (0 == g_strcmp0(name, "starts_with"))
			pattern = g_strconcat(text, "%", NULL);
		else if (0 == g_strcmp0(name, "ends_with"))
			pattern = g_strconcat("%", text, NULL);
		else
			pattern = g_strdup(text);

		if (!venture_query_add_filter_string_as_read(query, venture_lql_target_property(&target),
		                                             VENTURE_FILTER_OP_LIKE, pattern, NULL))
			return LQL_PUSHDOWN_NONE;
		return LQL_PUSHDOWN_HINT;
	}

	return LQL_PUSHDOWN_NONE;
}

/*
 * venture_lql_source_map:
 * @self: the source
 * @request: the fetch, for evaluating operands
 * @predicate: one conjunct of the filters after the fetch
 * @query: the query to add the filter to (a scratch one while planning)
 * @out_organization: (nullable): set to N for `organization_id == N`
 *
 * The one table of what is pushed down, used both to classify a conjunct
 * while the query is planned and to apply it when it runs, so the two
 * cannot disagree:
 *
 *   f == c, f != c, f < c, f <= c, f > c, f >= c    eq ne lt lte gt gte (EXACT)
 *   f in [c, ...], f not in [c, ...]                in, not_in (EXACT)
 *   is_null(f), is_not_null(f)                      is_null, not_null (EXACT)
 *   between(f, lo, hi)                              between (EXACT)
 *   m.amount OP c, m.currency == "X"                a money amount / currency (EXACT)
 *   contains / starts_with / ends_with / like       like (HINT)
 *
 * where f is a stored field, c an expression without a field (a literal,
 * a $parameter, now() - 7d, me(), period("this_month").start, ...) whose
 * value is of f's own type (venture_lql_operand_text()). Everything else
 * -- `or`, `not`, other functions, labels, display_name -- is NONE.
 *
 * Every filter is added as read (venture_query_add_filter_as_read()): it
 * compares what the row holds, not the stored column. They differ where a
 * column is NULL but the record reads a value -- a column a later
 * migration added, NULL in every older row, on an enum (its zero member),
 * an integer (0) or a boolean (FALSE). Then is_null(f) matches nothing,
 * is_not_null(f) everything, and a comparison the read value passes keeps
 * the NULL rows too; m.currency == the default currency keeps a money
 * whose currency column is NULL. Where the read value is not fixed, or
 * the database cannot decide a comparison of it (LIKE on a text field
 * that reads a value), the conjunct is NONE.
 *
 * Returns: how the conjunct was taken
 */
static LqlPushdown
venture_lql_source_map(
	VentureLqlSource	*self,
	LqlFetchRequest		*request,
	LqlNode			*predicate,
	VentureQuery		*query,
	gint64			*out_organization
){
	venture_lql_source_ensure_fields(self);

	switch (lql_node_get_kind(predicate))
	{
	case LQL_NODE_BINARY:
		if (2 != lql_node_get_n_children(predicate))
			return LQL_PUSHDOWN_NONE;
		return venture_lql_map_comparison(self, request, predicate, query, out_organization);

	case LQL_NODE_IN:
		return venture_lql_map_in(self, request, predicate, query);

	case LQL_NODE_CALL:
		return venture_lql_map_call(self, request, predicate, query);

	default:
		return LQL_PUSHDOWN_NONE;
	}
}

/* --- Building the query --------------------------------------------------- */

/*
 * The VentureQuery a fetch runs: the pushed filters, the selected-business
 * scope the list API applies, soft deletion, and the row bound. Conjuncts
 * LQL was promised (EXACT) but that no longer map when the query runs --
 * an operand of another type than when it was planned -- are returned in
 * @out_local for the source to check row by row itself.
 */
static VentureQuery *
venture_lql_source_build_query(
	VentureLqlSource	 *self,
	LqlFetchRequest		 *request,
	GPtrArray		**out_local,
	gboolean		 *out_bounded,
	GError			**error
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) local = NULL;
	g_auto(GStrv) params = NULL;
	LqlValue *include_deleted;
	gint64 organization = 0;
	gint64 limit;
	guint i;

	venture_lql_source_ensure_fields(self);

	/* --- arguments ----------------------------------------------------- */
	if (lql_fetch_request_get_n_args(request) > 0)
	{
		g_set_error(error, LQL_ERROR, LQL_ERROR_ARGUMENT,
		            "fetch %s takes no arguments; filter its rows with `| filter`",
		            self->name);
		return NULL;
	}

	params = lql_fetch_request_list_params(request);

	for (i = 0; (NULL != params) && (NULL != params[i]); i++)
	{
		if (0 != g_strcmp0(params[i], VENTURE_LQL_PARAM_INCLUDE_DELETED))
		{
			g_set_error(error, LQL_ERROR, LQL_ERROR_ARGUMENT,
			            "fetch %s has no parameter \"%s\"; it takes %s: TRUE",
			            self->name, params[i], VENTURE_LQL_PARAM_INCLUDE_DELETED);
			return NULL;
		}
	}

	include_deleted = lql_fetch_request_get_param(request, VENTURE_LQL_PARAM_INCLUDE_DELETED);

	if ((NULL != include_deleted) &&
	    (LQL_VALUE_TYPE_BOOLEAN != lql_value_get_value_type(include_deleted)))
	{
		g_set_error(error, LQL_ERROR, LQL_ERROR_ARGUMENT,
		            "%s must be TRUE or FALSE", VENTURE_LQL_PARAM_INCLUDE_DELETED);
		return NULL;
	}

	/* --- the pushed filters --------------------------------------------- */
	query = venture_query_new(self->entity_type);
	local = g_ptr_array_new();

	for (i = 0; i < lql_fetch_request_get_n_predicates(request); i++)
	{
		LqlPushdown promised = lql_fetch_request_get_pushdown(request, i);
		LqlNode *predicate = lql_fetch_request_get_predicate(request, i);
		LqlPushdown taken;

		if (LQL_PUSHDOWN_NONE == promised)
			continue;

		taken = venture_lql_source_map(self, request, predicate, query, &organization);

		if ((LQL_PUSHDOWN_EXACT == promised) && (LQL_PUSHDOWN_EXACT != taken))
			g_ptr_array_add(local, predicate);
	}

	/* --- the selected business ------------------------------------------ */
	/* What venture_web_scope_to_active_organization() does to a list: the
	 * selected business and everything beneath it, unless the query names
	 * one business itself, and never for businesses or workspace
	 * administration records, which are not filed against a business. */
	if ((0 == organization) && (self->caller->organizations->len > 0) &&
	    (VENTURE_TYPE_ORGANIZATION != self->entity_type) &&
	    (VENTURE_DATA_CLASS_TENANT_ADMIN != venture_data_class_for_type(self->entity_type)))
	{
		venture_query_set_organization_tree(query,
			(const gint64 *)self->caller->organizations->data,
			self->caller->organizations->len);
	}

	if ((NULL != include_deleted) && lql_value_get_boolean(include_deleted))
		venture_query_set_include_deleted(query, TRUE);

	/* --- the bound -------------------------------------------------------- */
	/* A limit LQL offered (every filter before it EXACT) is honoured when
	 * the source checks nothing itself; otherwise one row past the bound is
	 * read, to tell "exactly the bound" from "more than the bound". */
	limit = lql_fetch_request_get_limit(request);

	if ((0 == local->len) && (limit >= 0) && (limit <= self->caller->max_rows))
	{
		venture_query_set_limit(query, (guint)limit);
		*out_bounded = FALSE;
	}
	else
	{
		venture_query_set_limit(query, (guint)self->caller->max_rows + 1);
		*out_bounded = TRUE;
	}

	*out_local = g_steal_pointer(&local);

	return g_steal_pointer(&query);
}

/* --- LqlSource -------------------------------------------------------------- */

static const gchar *
venture_lql_source_get_name(LqlSource *source)
{
	return VENTURE_LQL_SOURCE(source)->name;
}

static GPtrArray *
venture_lql_source_get_columns(LqlSource *source)
{
	VentureLqlSource *self = VENTURE_LQL_SOURCE(source);
	GPtrArray *columns;
	guint i;

	venture_lql_source_ensure_fields(self);
	columns = g_ptr_array_new_with_free_func((GDestroyNotify)lql_column_unref);

	for (i = 0; i < self->fields->len; i++)
	{
		VentureLqlField *field = g_ptr_array_index(self->fields, i);

		g_ptr_array_add(columns, lql_column_new(field->name, field->lql_type, field->description));
	}

	return columns;
}

static GPtrArray *
venture_lql_source_get_parameters(LqlSource *source)
{
	GPtrArray *parameters;

	(void)source;
	parameters = g_ptr_array_new_with_free_func((GDestroyNotify)lql_column_unref);
	g_ptr_array_add(parameters, lql_column_new(VENTURE_LQL_PARAM_INCLUDE_DELETED,
		LQL_VALUE_TYPE_BOOLEAN, "TRUE to read soft-deleted records too"));

	return parameters;
}

static LqlPushdown
venture_lql_source_classify_predicate(
	LqlSource	*source,
	LqlFetchRequest	*request,
	LqlNode		*predicate
){
	VentureLqlSource *self = VENTURE_LQL_SOURCE(source);
	g_autoptr(VentureQuery) scratch = venture_query_new(self->entity_type);

	return venture_lql_source_map(self, request, predicate, scratch, NULL);
}

static gboolean
venture_lql_source_accepts_limit(
	LqlSource	*source,
	LqlFetchRequest	*request
){
	(void)source;
	(void)request;

	/* Honoured in venture_lql_source_build_query() whenever it can be. */
	return TRUE;
}

static gchar *
venture_lql_source_explain(
	LqlSource	*source,
	LqlFetchRequest	*request
){
	VentureLqlSource *self = VENTURE_LQL_SOURCE(source);
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) local = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *described = NULL;
	gboolean bounded = TRUE;

	query = venture_lql_source_build_query(self, request, &local, &bounded, &error);

	if (NULL == query)
		return g_strdup_printf("venture %s: %s", self->name, error->message);

	described = venture_query_describe(query);

	return g_strdup_printf("venture %s: %s, read as the caller; %s",
		self->name, described,
		bounded ? "more than query.max_rows rows is an error" : "limit taken by the database");
}

/*
 * The whole read, synchronously: the query, the rows through the caller's
 * reader, the labels, and any conjunct the source has to check itself.
 */
static LqlTable *
venture_lql_source_read(
	VentureLqlSource	 *self,
	LqlFetchRequest		 *request,
	GError			**error
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) local = NULL;
	g_autoptr(GPtrArray) entities = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(LqlEvaluator) evaluator = NULL;
	LqlTable *table;
	VentureAccessPolicy *policy;
	gboolean bounded = TRUE;
	guint i;
	guint j;

	query = venture_lql_source_build_query(self, request, &local, &bounded, error);

	if (NULL == query)
		return NULL;

	table = lql_table_new();

	for (i = 0; i < self->fields->len; i++)
	{
		VentureLqlField *field = g_ptr_array_index(self->fields, i);

		lql_table_add_column(table, field->name, field->lql_type);
	}

	/* `limit 0`: VENTURE reads 0 as "no limit", so nothing is read. */
	if (!bounded && (0 == venture_query_get_limit(query)))
		return table;

	/* --- the rows, as the caller -------------------------------------- */
	rows = g_ptr_array_new_with_free_func((GDestroyNotify)lql_record_unref);
	policy = venture_database_get_access_policy(self->caller->database);
	{
		g_autoptr(VentureAccessScope) scope = NULL;

		scope = venture_access_policy_enter_snapshot(policy, self->caller->reader);
		entities = venture_database_find(self->caller->database, query, error);

		if (NULL == entities)
		{
			lql_table_unref(table);
			return NULL;
		}

		if (bounded && (entities->len > (guint)self->caller->max_rows))
		{
			g_set_error(error, LQL_ERROR, LQL_ERROR_LIMIT,
			            "fetch %s would read more than %" G_GINT64_FORMAT " rows "
			            "(query.max_rows); filter on stored fields right after "
			            "the fetch so the database narrows it, or add `| limit N`",
			            self->name, self->caller->max_rows);
			lql_table_unref(table);
			return NULL;
		}

		for (i = 0; i < entities->len; i++)
			g_ptr_array_add(rows, venture_lql_source_row(self, g_ptr_array_index(entities, i)));

		if (!venture_lql_source_fill_labels(self, rows, error))
		{
			lql_table_unref(table);
			return NULL;
		}
	}

	/* --- what the source promised but could not push ------------------ */
	if (local->len > 0)
		evaluator = lql_fetch_request_new_evaluator(request);

	for (i = 0; i < rows->len; i++)
	{
		LqlRecord *row = g_ptr_array_index(rows, i);
		gboolean keep = TRUE;

		for (j = 0; keep && (j < local->len); j++)
			keep = lql_evaluator_test(evaluator, g_ptr_array_index(local, j), row);

		if (keep)
			lql_table_add_row(table, row);
	}

	return table;
}

static void
venture_lql_source_fetch_async(
	LqlSource		*source,
	LqlFetchRequest		*request,
	GCancellable		*cancellable,
	GAsyncReadyCallback	 callback,
	gpointer		 user_data
){
	VentureLqlSource *self = VENTURE_LQL_SOURCE(source);
	g_autoptr(GTask) task = NULL;
	GError *error = NULL;
	LqlTable *table;

	task = g_task_new(source, cancellable, callback, user_data);
	g_task_set_source_tag(task, venture_lql_source_fetch_async);

	if (g_task_return_error_if_cancelled(task))
		return;

	/* The database is read on the calling thread, as every list is. */
	table = venture_lql_source_read(self, request, &error);

	if (NULL == table)
		g_task_return_error(task, error);
	else
		g_task_return_pointer(task, table, (GDestroyNotify)lql_table_unref);
}

static LqlTable *
venture_lql_source_fetch_finish(
	LqlSource	 *source,
	GAsyncResult	 *result,
	GError		**error
){
	g_return_val_if_fail(g_task_is_valid(result, source), NULL);

	return g_task_propagate_pointer(G_TASK(result), error);
}

static void
venture_lql_source_iface_init(LqlSourceInterface *iface)
{
	iface->get_name = venture_lql_source_get_name;
	iface->get_columns = venture_lql_source_get_columns;
	iface->get_parameters = venture_lql_source_get_parameters;
	iface->classify_predicate = venture_lql_source_classify_predicate;
	iface->accepts_limit = venture_lql_source_accepts_limit;
	iface->explain = venture_lql_source_explain;
	iface->fetch_async = venture_lql_source_fetch_async;
	iface->fetch_finish = venture_lql_source_fetch_finish;
}
