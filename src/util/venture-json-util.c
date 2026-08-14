/*
 * venture-json-util.c - GValue and JSON interconversion
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

JsonNode *
venture_json_node_from_value(const GValue *value)
{
	GType value_type;

	if ((NULL == value) || (G_TYPE_INVALID == G_VALUE_TYPE(value)))
		return json_node_new(JSON_NODE_NULL);

	value_type = G_VALUE_TYPE(value);

	if (G_TYPE_STRING == value_type)
	{
		const gchar *text;

		text = g_value_get_string(value);

		if (NULL == text)
			return json_node_new(JSON_NODE_NULL);

		return json_node_init_string(json_node_alloc(), text);
	}

	if (G_TYPE_BOOLEAN == value_type)
		return json_node_init_boolean(json_node_alloc(), g_value_get_boolean(value));

	if (G_TYPE_INT == value_type)
		return json_node_init_int(json_node_alloc(), (gint64)g_value_get_int(value));

	if (G_TYPE_UINT == value_type)
		return json_node_init_int(json_node_alloc(), (gint64)g_value_get_uint(value));

	if (G_TYPE_INT64 == value_type)
		return json_node_init_int(json_node_alloc(), g_value_get_int64(value));

	if (G_TYPE_UINT64 == value_type)
		return json_node_init_int(json_node_alloc(), (gint64)g_value_get_uint64(value));

	if (G_TYPE_DOUBLE == value_type)
		return json_node_init_double(json_node_alloc(), g_value_get_double(value));

	if (G_TYPE_FLOAT == value_type)
		return json_node_init_double(json_node_alloc(), (gdouble)g_value_get_float(value));

	/* Enumerations travel by nick, never by number. The numbers are an
	 * implementation detail that would break the moment a value is
	 * inserted in the middle of an enum; the nicks are the contract. */
	if (G_TYPE_IS_ENUM(value_type))
	{
		const gchar *nick;

		nick = venture_enum_to_nick(value_type, g_value_get_enum(value));

		if (NULL == nick)
			return json_node_new(JSON_NODE_NULL);

		return json_node_init_string(json_node_alloc(), nick);
	}

	if (G_TYPE_IS_FLAGS(value_type))
		return json_node_init_int(json_node_alloc(), (gint64)g_value_get_flags(value));

	if (VENTURE_TYPE_MONEY == value_type)
	{
		const VentureMoney *money;

		money = g_value_get_boxed(value);

		if (NULL == money)
			return json_node_new(JSON_NODE_NULL);

		return venture_money_to_json(money);
	}

	if (VENTURE_TYPE_DATE_RANGE == value_type)
	{
		VentureDateRange *range;

		range = g_value_get_boxed(value);

		if (NULL == range)
			return json_node_new(JSON_NODE_NULL);

		return venture_date_range_to_json(range);
	}

	if (G_TYPE_DATE_TIME == value_type)
	{
		GDateTime *when;
		g_autofree gchar *text = NULL;

		when = g_value_get_boxed(value);

		if (NULL == when)
			return json_node_new(JSON_NODE_NULL);

		text = g_date_time_format_iso8601(when);

		return json_node_init_string(json_node_alloc(), text);
	}

	if (G_TYPE_STRV == value_type)
	{
		g_autoptr(JsonArray) array = NULL;
		const gchar * const *strings;
		gsize i;

		strings = g_value_get_boxed(value);

		if (NULL == strings)
			return json_node_new(JSON_NODE_NULL);

		array = json_array_new();

		for (i = 0; NULL != strings[i]; i++)
			json_array_add_string_element(array, strings[i]);

		return json_node_init_array(json_node_alloc(), g_steal_pointer(&array));
	}

	/* Anything else has no agreed encoding. Rendering it as null keeps a
	 * single unusual property from making the whole record unserialisable,
	 * and the warning says which one to look at. */
	g_debug("venture_json_node_from_value: no JSON encoding for %s",
	        g_type_name(value_type));

	return json_node_new(JSON_NODE_NULL);
}

/*
 * Pulls a string out of a node regardless of the node's own type, so a
 * quoted number or a bare number both work. Returns NULL for null nodes.
 */
static gchar *
venture_json_node_coerce_string(JsonNode *node)
{
	GType value_type;

	if ((NULL == node) || JSON_NODE_HOLDS_NULL(node))
		return NULL;

	if (!JSON_NODE_HOLDS_VALUE(node))
		return venture_json_to_string(node, FALSE);

	value_type = json_node_get_value_type(node);

	if (G_TYPE_STRING == value_type)
		return g_strdup(json_node_get_string(node));

	if (G_TYPE_INT64 == value_type)
		return g_strdup_printf("%" G_GINT64_FORMAT, json_node_get_int(node));

	if (G_TYPE_DOUBLE == value_type)
		return g_strdup_printf("%g", json_node_get_double(node));

	if (G_TYPE_BOOLEAN == value_type)
		return g_strdup(json_node_get_boolean(node) ? "true" : "false");

	return NULL;
}

/*
 * Coerces a node to an integer, accepting a quoted number. Returns FALSE if
 * the text is not a number at all rather than guessing zero, because
 * silently turning "abc" into 0 in a financial record is unacceptable.
 */
static gboolean
venture_json_node_coerce_int(
	JsonNode	*node,
	gint64		*out_value
){
	GType value_type;

	if ((NULL == node) || JSON_NODE_HOLDS_NULL(node))
	{
		*out_value = 0;
		return TRUE;
	}

	if (!JSON_NODE_HOLDS_VALUE(node))
		return FALSE;

	value_type = json_node_get_value_type(node);

	if (G_TYPE_INT64 == value_type)
	{
		*out_value = json_node_get_int(node);
		return TRUE;
	}

	if (G_TYPE_DOUBLE == value_type)
	{
		*out_value = (gint64)json_node_get_double(node);
		return TRUE;
	}

	if (G_TYPE_BOOLEAN == value_type)
	{
		*out_value = json_node_get_boolean(node) ? 1 : 0;
		return TRUE;
	}

	if (G_TYPE_STRING == value_type)
	{
		const gchar *text;
		gchar *end = NULL;
		gint64 parsed;

		text = json_node_get_string(node);

		if ((NULL == text) || ('\0' == text[0]))
		{
			*out_value = 0;
			return TRUE;
		}

		errno = 0;
		parsed = g_ascii_strtoll(text, &end, 10);

		if ((0 != errno) || (NULL == end) || ('\0' != *end))
			return FALSE;

		*out_value = parsed;
		return TRUE;
	}

	return FALSE;
}

/*
 * Coerces a node to a boolean across every spelling that reaches this
 * system. An HTML checkbox posts "on" and omits the field entirely when
 * unchecked; an AI writes true, "true", "yes" or 1 depending on its mood.
 */
static gboolean
venture_json_node_coerce_bool(
	JsonNode	*node,
	gboolean	*out_value
){
	GType value_type;

	if ((NULL == node) || JSON_NODE_HOLDS_NULL(node))
	{
		*out_value = FALSE;
		return TRUE;
	}

	if (!JSON_NODE_HOLDS_VALUE(node))
		return FALSE;

	value_type = json_node_get_value_type(node);

	if (G_TYPE_BOOLEAN == value_type)
	{
		*out_value = json_node_get_boolean(node);
		return TRUE;
	}

	if (G_TYPE_INT64 == value_type)
	{
		*out_value = (0 != json_node_get_int(node));
		return TRUE;
	}

	if (G_TYPE_STRING == value_type)
	{
		const gchar *text;

		text = json_node_get_string(node);

		if (NULL == text)
		{
			*out_value = FALSE;
			return TRUE;
		}

		if ((0 == g_ascii_strcasecmp(text, "true")) ||
		    (0 == g_ascii_strcasecmp(text, "yes")) ||
		    (0 == g_ascii_strcasecmp(text, "on")) ||
		    (0 == g_strcmp0(text, "1")))
		{
			*out_value = TRUE;
			return TRUE;
		}

		if ((0 == g_ascii_strcasecmp(text, "false")) ||
		    (0 == g_ascii_strcasecmp(text, "no")) ||
		    (0 == g_ascii_strcasecmp(text, "off")) ||
		    (0 == g_strcmp0(text, "0")) ||
		    ('\0' == text[0]))
		{
			*out_value = FALSE;
			return TRUE;
		}

		return FALSE;
	}

	return FALSE;
}

gboolean
venture_json_value_from_node(
	JsonNode	 *node,
	GType		  target_type,
	GValue		 *out_value,
	GError		**error
){
	g_return_val_if_fail(G_TYPE_INVALID != target_type, FALSE);
	g_return_val_if_fail(NULL != out_value, FALSE);

	g_value_init(out_value, target_type);

	/* A null or absent value leaves the GValue at its type default, which
	 * is the right meaning for "this field was not supplied". */
	if ((NULL == node) || JSON_NODE_HOLDS_NULL(node))
		return TRUE;

	if (G_TYPE_STRING == target_type)
	{
		g_autofree gchar *text = NULL;

		text = venture_json_node_coerce_string(node);
		g_value_take_string(out_value, g_steal_pointer(&text));

		return TRUE;
	}

	if ((G_TYPE_INT64 == target_type) || (G_TYPE_INT == target_type) ||
	    (G_TYPE_UINT == target_type) || (G_TYPE_UINT64 == target_type))
	{
		gint64 number;

		if (!venture_json_node_coerce_int(node, &number))
		{
			g_autofree gchar *text = NULL;

			text = venture_json_to_string(node, FALSE);
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_SERIALIZATION,
			            "%s is not a whole number", text);
			return FALSE;
		}

		if (G_TYPE_INT64 == target_type)
			g_value_set_int64(out_value, number);
		else if (G_TYPE_INT == target_type)
			g_value_set_int(out_value, (gint)number);
		else if (G_TYPE_UINT == target_type)
			g_value_set_uint(out_value, (guint)number);
		else
			g_value_set_uint64(out_value, (guint64)number);

		return TRUE;
	}

	if ((G_TYPE_DOUBLE == target_type) || (G_TYPE_FLOAT == target_type))
	{
		g_autofree gchar *text = NULL;
		gdouble number;

		if (JSON_NODE_HOLDS_VALUE(node) &&
		    (G_TYPE_DOUBLE == json_node_get_value_type(node)))
		{
			number = json_node_get_double(node);
		}
		else if (JSON_NODE_HOLDS_VALUE(node) &&
		         (G_TYPE_INT64 == json_node_get_value_type(node)))
		{
			number = (gdouble)json_node_get_int(node);
		}
		else
		{
			gchar *end = NULL;

			text = venture_json_node_coerce_string(node);

			if ((NULL == text) || ('\0' == text[0]))
				return TRUE;

			number = g_ascii_strtod(text, &end);

			if ((NULL == end) || ('\0' != *end))
			{
				g_set_error(error, VENTURE_ERROR,
				            VENTURE_ERROR_SERIALIZATION,
				            "\"%s\" is not a number", text);
				return FALSE;
			}
		}

		if (G_TYPE_DOUBLE == target_type)
			g_value_set_double(out_value, number);
		else
			g_value_set_float(out_value, (gfloat)number);

		return TRUE;
	}

	if (G_TYPE_BOOLEAN == target_type)
	{
		gboolean flag;

		if (!venture_json_node_coerce_bool(node, &flag))
		{
			g_autofree gchar *text = NULL;

			text = venture_json_to_string(node, FALSE);
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_SERIALIZATION,
			            "%s is not a boolean", text);
			return FALSE;
		}

		g_value_set_boolean(out_value, flag);

		return TRUE;
	}

	if (G_TYPE_IS_ENUM(target_type))
	{
		g_autofree gchar *text = NULL;
		gint enum_value;

		text = venture_json_node_coerce_string(node);

		if ((NULL == text) || ('\0' == text[0]))
			return TRUE;

		if (!venture_enum_from_nick(target_type, text, &enum_value))
		{
			g_auto(GStrv) nicks = NULL;
			g_autofree gchar *valid = NULL;

			nicks = venture_enum_list_nicks(target_type);
			valid = g_strjoinv(", ", nicks);

			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_SERIALIZATION,
			            "\"%s\" is not a valid value. Expected one of: %s",
			            text, valid);
			return FALSE;
		}

		g_value_set_enum(out_value, enum_value);

		return TRUE;
	}

	if (VENTURE_TYPE_MONEY == target_type)
	{
		g_autoptr(VentureMoney) money = NULL;

		money = venture_money_from_json(node,
		                                venture_money_get_default_currency(),
		                                error);

		if (NULL == money)
			return FALSE;

		g_value_take_boxed(out_value, g_steal_pointer(&money));

		return TRUE;
	}

	if (G_TYPE_DATE_TIME == target_type)
	{
		g_autofree gchar *text = NULL;
		g_autoptr(GDateTime) when = NULL;

		text = venture_json_node_coerce_string(node);

		if ((NULL == text) || ('\0' == text[0]))
			return TRUE;

		when = venture_time_from_string(text, error);

		if (NULL == when)
			return FALSE;

		g_value_set_boxed(out_value, when);

		return TRUE;
	}

	if (G_TYPE_STRV == target_type)
	{
		g_autoptr(GPtrArray) strings = NULL;
		JsonArray *array;
		guint i;

		if (!JSON_NODE_HOLDS_ARRAY(node))
		{
			/* A single value where a list was expected is treated as a
			 * one-element list: an HTML form with one selected option
			 * posts exactly that. */
			g_autofree gchar *text = NULL;

			text = venture_json_node_coerce_string(node);
			strings = g_ptr_array_new();

			if (NULL != text)
				g_ptr_array_add(strings, g_steal_pointer(&text));

			g_ptr_array_add(strings, NULL);
			g_value_take_boxed(out_value,
				g_ptr_array_free(g_steal_pointer(&strings), FALSE));

			return TRUE;
		}

		array = json_node_get_array(node);
		strings = g_ptr_array_new();

		for (i = 0; i < json_array_get_length(array); i++)
		{
			gchar *element;

			element = venture_json_node_coerce_string(
				json_array_get_element(array, i));

			if (NULL != element)
				g_ptr_array_add(strings, element);
		}

		g_ptr_array_add(strings, NULL);
		g_value_take_boxed(out_value,
			g_ptr_array_free(g_steal_pointer(&strings), FALSE));

		return TRUE;
	}

	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_SERIALIZATION,
	            "Cannot decode JSON into a value of type %s",
	            g_type_name(target_type));

	return FALSE;
}

gchar *
venture_json_to_string(
	JsonNode	*node,
	gboolean	 pretty
){
	g_autoptr(JsonGenerator) generator = NULL;

	if (NULL == node)
		return g_strdup("null");

	generator = json_generator_new();
	json_generator_set_root(generator, node);
	json_generator_set_pretty(generator, pretty);
	json_generator_set_indent(generator, 2);

	return json_generator_to_data(generator, NULL);
}

JsonNode *
venture_json_parse(
	const gchar	 *text,
	GError		**error
){
	g_autoptr(JsonParser) parser = NULL;
	g_autoptr(GError) local_error = NULL;
	JsonNode *root;

	g_return_val_if_fail(NULL != text, NULL);

	parser = json_parser_new();

	if (!json_parser_load_from_data(parser, text, -1, &local_error))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_SERIALIZATION,
		            "Invalid JSON: %s", local_error->message);
		return NULL;
	}

	root = json_parser_get_root(parser);

	if (NULL == root)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_SERIALIZATION,
		                    "Empty JSON document");
		return NULL;
	}

	/* The parser owns the root, so hand back a copy that outlives it. */
	return json_node_copy(root);
}

const gchar *
venture_json_object_get_string(
	JsonObject	*object,
	const gchar	*member,
	const gchar	*fallback
){
	JsonNode *node;

	if ((NULL == object) || !json_object_has_member(object, member))
		return fallback;

	node = json_object_get_member(object, member);

	if (!JSON_NODE_HOLDS_VALUE(node) ||
	    (G_TYPE_STRING != json_node_get_value_type(node)))
		return fallback;

	return json_node_get_string(node);
}

gint64
venture_json_object_get_int(
	JsonObject	*object,
	const gchar	*member,
	gint64		 fallback
){
	gint64 value;

	if ((NULL == object) || !json_object_has_member(object, member))
		return fallback;

	if (!venture_json_node_coerce_int(json_object_get_member(object, member),
	                                  &value))
		return fallback;

	return value;
}

gboolean
venture_json_object_get_bool(
	JsonObject	*object,
	const gchar	*member,
	gboolean	 fallback
){
	gboolean value;

	if ((NULL == object) || !json_object_has_member(object, member))
		return fallback;

	if (!venture_json_node_coerce_bool(json_object_get_member(object, member),
	                                   &value))
		return fallback;

	return value;
}

void
venture_json_builder_add_error(
	JsonBuilder	*builder,
	const GError	*error
){
	VentureError code;

	g_return_if_fail(NULL != builder);

	code = (NULL != error) ? (VentureError)error->code : VENTURE_ERROR_FAILED;

	/* Anything not in the VENTURE domain -- a GIO or libsoup failure that
	 * reached this far -- is reported as a generic failure rather than
	 * having its unrelated code number misread as one of ours. */
	if ((NULL != error) && (VENTURE_ERROR != error->domain))
		code = VENTURE_ERROR_FAILED;

	json_builder_set_member_name(builder, "error");
	json_builder_add_string_value(builder, venture_error_get_slug(code));

	json_builder_set_member_name(builder, "message");
	json_builder_add_string_value(builder,
		(NULL != error) ? error->message : "Unknown error");

	json_builder_set_member_name(builder, "code");
	json_builder_add_int_value(builder, (gint64)code);
}

gchar *
venture_json_error_to_string(
	const GError	*error,
	gboolean	 pretty
){
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) node = NULL;

	builder = json_builder_new();
	json_builder_begin_object(builder);
	venture_json_builder_add_error(builder, error);
	json_builder_end_object(builder);

	node = json_builder_get_root(builder);

	return venture_json_to_string(node, pretty);
}
