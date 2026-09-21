/*
 * venture-json-util.h - GValue and JSON interconversion
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The generic serialisation in #VentureEntity, the REST payload decoder, the
 * AI tool-argument binder and the CLI output formatter all need to move
 * values between GObject's type system and JSON. Doing that in one place
 * means a #VentureMoney is encoded identically no matter which of them is
 * asking, and a fix to date parsing fixes all four.
 */

#ifndef VENTURE_JSON_UTIL_H
#define VENTURE_JSON_UTIL_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/**
 * venture_json_node_from_value:
 * @value: the value to encode
 *
 * Encodes a #GValue as JSON. Understands the numeric and string
 * fundamentals, booleans, enumerations (encoded by nick, never by number),
 * #GDateTime (ISO 8601), #VentureMoney and #VentureDateRange. An unset or
 * unsupported value encodes as null rather than failing, so one odd property
 * cannot make a whole record unserialisable.
 *
 * Returns: (transfer full): a new #JsonNode
 */
JsonNode *
venture_json_node_from_value(const GValue *value);

/**
 * venture_json_value_from_node:
 * @node: (nullable): the JSON to decode
 * @target_type: the #GType to produce
 * @out_value: (out caller-allocates): an uninitialised #GValue to fill
 * @error: (out) (optional): return location for a #GError
 *
 * Decodes JSON into a #GValue of @target_type.
 *
 * The decoder is deliberately lenient about the input's own type, because
 * the same function handles a strict REST client, a web form where every
 * value is a string, and an AI that has decided today is the day it sends
 * numbers as strings. A JSON string "42" decodes into an integer property;
 * "true", "yes" and 1 all decode into a boolean; a date accepts an ISO
 * timestamp, a bare date or an epoch second count. What it will not do is
 * silently accept something meaningless -- that raises
 * %VENTURE_ERROR_SERIALIZATION naming the value.
 *
 * Returns: %TRUE on success, with @out_value initialised
 */
gboolean
venture_json_value_from_node(
	JsonNode	 *node,
	GType		  target_type,
	GValue		 *out_value,
	GError		**error
);

/**
 * venture_json_to_string:
 * @node: (nullable): the node to render
 * @pretty: whether to indent
 *
 * Returns: (transfer full) (nullable): the rendered JSON
 */
gchar *
venture_json_to_string(
	JsonNode	*node,
	gboolean	 pretty
);

/**
 * venture_json_parse:
 * @text: the JSON text
 * @error: (out) (optional): return location for a #GError
 *
 * Parses JSON, raising %VENTURE_ERROR_SERIALIZATION on failure.
 *
 * Returns: (transfer full) (nullable): the parsed root node
 */
JsonNode *
venture_json_parse(
	const gchar	 *text,
	GError		**error
);

/**
 * venture_json_object_get_string:
 * @object: a #JsonObject
 * @member: the member name
 * @fallback: (nullable): the value to return when absent or not a string
 *
 * Returns: (transfer none) (nullable): the member value or @fallback
 */
const gchar *
venture_json_object_get_string(
	JsonObject	*object,
	const gchar	*member,
	const gchar	*fallback
);

/**
 * venture_json_object_get_int:
 * @object: a #JsonObject
 * @member: the member name
 * @fallback: the value to return when absent or not a number
 *
 * Accepts a numeric member or a numeric string, since form encodings and
 * some AI outputs quote their integers.
 *
 * Returns: the member value or @fallback
 */
gint64
venture_json_object_get_int(
	JsonObject	*object,
	const gchar	*member,
	gint64		 fallback
);

/**
 * venture_json_object_get_bool:
 * @object: a #JsonObject
 * @member: the member name
 * @fallback: the value to return when absent
 *
 * Accepts a boolean, a number, or the strings "true", "false", "yes", "no",
 * "on", "off", "1" and "0" -- an HTML checkbox posts "on", so this matters.
 *
 * Returns: the member value or @fallback
 */
gboolean
venture_json_object_get_bool(
	JsonObject	*object,
	const gchar	*member,
	gboolean	 fallback
);

/**
 * venture_json_builder_add_error:
 * @builder: a #JsonBuilder positioned to begin an object
 * @error: the error to describe
 *
 * Writes the standard error body: a machine-readable slug, the message, and
 * the numeric code. Every REST error response goes through this so clients
 * only ever have to learn one shape.
 */
void
venture_json_builder_add_error(
	JsonBuilder	*builder,
	const GError	*error
);

/**
 * venture_json_error_to_string:
 * @error: the error to describe
 * @pretty: whether to indent
 *
 * Returns: (transfer full): the rendered error body
 */
gchar *
venture_json_error_to_string(
	const GError	*error,
	gboolean	 pretty
);

/**
 * venture_json_append_canonical:
 * @output: destination fingerprint material
 * @node: bounded parsed JSON value
 *
 * Appends deterministic hash material, sorting object keys and preserving
 * array order. The encoding retains accounting-approval fingerprint bytes,
 * including trailing delimiters; it is not a JSON wire representation.
 * Callers bound untrusted input size and nesting before this synchronous walk.
 */
void venture_json_append_canonical(GString *output, JsonNode *node);

G_END_DECLS

#endif /* VENTURE_JSON_UTIL_H */
