/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * venture-web-reference-lookup.inc - Reference pickers past a <select>
 *
 * Copyright (C) 2026 Zach Podbielniak
 *
 * Included by venture-web-server.c. A reference picker -- a record form's
 * company, a list's Filters panel company -- lists its candidates as a
 * <select> while there are at most VENTURE_WEB_REFERENCE_PICKER_MAX of
 * them. Past that it is a lookup (docs/interface.org, "Pickers"):
 *
 *   - a hidden input under the field's own name, holding the id, so a
 *     form posts and a list filters by exactly what it always did
 *     (?company_id=12);
 *   - a text box, <name>__lookup, showing the chosen record as
 *     "#12 Acme Anvils". The page script searches GET /ui/records/search
 *     as somebody types and fills both in. With scripting off the box is
 *     simply typed into: a name or a #id, resolved by the server on submit
 *     by venture_web_reference_resolve(), which refuses an unknown or an
 *     ambiguous one by name.
 *
 * Whenever the box is submitted it decides, and the hidden id is only
 * what it said last; the script drops the box's name once a match is
 * picked, so a filter's URL carries the id alone.
 *
 * Every read here is an ordinary find under the request's access scope,
 * so the lookup offers, and resolves to, only records the caller may read
 * -- in hosted mode, only the workspace's own.
 */

#include "web/venture-web-picker-private.h"

/* How many rows a name is matched against before it is called ambiguous. */
#define VENTURE_WEB_REFERENCE_RESOLVE_SCAN (25)

/* How many candidates an ambiguous name's refusal names. */
#define VENTURE_WEB_REFERENCE_RESOLVE_NAMED (5)

/* Defined further down with the rest of the record handling. */
static gchar *venture_web_type_label(GType type, gboolean plural);
static gchar *venture_web_reference_option_label(VentureWebServer *self,
	VentureEntity *record, VentureEntity *option);

/*
 * Whether @text names a record by its id: "12", "#12", or "#12" and the
 * label the lookup box shows after it ("#12 Acme Anvils"). A name that
 * merely starts with digits ("3M", "1999 Ltd") is a name.
 */
static gboolean
venture_web_reference_parse_id(
	const gchar	*text,
	gint64		*out_id
){
	const gchar *digits;
	gchar *end = NULL;
	gboolean hashed;
	gint64 id;

	hashed = ('#' == text[0]);
	digits = hashed ? text + 1 : text;

	if (!g_ascii_isdigit(digits[0]))
		return FALSE;

	id = g_ascii_strtoll(digits, &end, 10);

	/* Bare digits must be all there is; after a '#' a label may follow. */
	if ((id <= 0) || (('\0' != *end) && !(hashed && g_ascii_isspace(*end))))
		return FALSE;

	*out_id = id;
	return TRUE;
}

/* What @option is called in a lookup: "#12 Acme Anvils". */
static gchar *
venture_web_reference_lookup_text(
	VentureWebServer	*self,
	VentureEntity		*record,
	VentureEntity		*option
){
	g_autofree gchar *label = NULL;

	label = venture_web_reference_option_label(self, record, option);

	return g_strdup_printf("#%" G_GINT64_FORMAT "%s%s", venture_entity_get_id(option),
	                       venture_string_is_empty(label) ? "" : " ",
	                       venture_string_is_empty(label) ? "" : label);
}

/* Whether @type stores a text column called "name". */
static gboolean
venture_web_reference_has_name(GType type)
{
	g_autoptr(GTypeClass) klass = NULL;
	GParamSpec *property;

	klass = g_type_class_ref(type);
	property = g_object_class_find_property(G_OBJECT_CLASS(klass), "name");

	return (NULL != property) && (G_TYPE_STRING == property->value_type);
}

/*
 * The records of @target whose name is @folded (casefolded), case aside,
 * read through the name column; nothing for a type without one, or for a
 * name with a LIKE wildcard in it, which the column cannot be asked for
 * exactly.
 */
static GPtrArray *
venture_web_reference_named(
	VentureWebServer	 *self,
	GType			  target,
	gint64			  organization,
	const gchar		 *wanted,
	const gchar		 *folded,
	GError			**error
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	GPtrArray *named;
	guint i;

	named = g_ptr_array_new_with_free_func(g_object_unref);

	if (!venture_web_reference_has_name(target) || (NULL != strpbrk(wanted, "%_")))
		return named;

	query = venture_query_new(target);

	if (!venture_query_add_filter_string(query, "name", VENTURE_FILTER_OP_ILIKE, wanted, NULL))
		return named;

	if (organization > 0)
		venture_query_set_organization(query, organization);

	venture_query_set_limit(query, VENTURE_WEB_REFERENCE_RESOLVE_SCAN);
	rows = venture_database_find(venture_context_get_database(self->context), query, error);

	if (NULL == rows)
	{
		g_ptr_array_unref(named);
		return NULL;
	}

	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autofree gchar *name = NULL;
		g_autofree gchar *name_folded = NULL;

		g_object_get(row, "name", &name, NULL);
		name_folded = g_utf8_casefold((NULL != name) ? name : "", -1);

		if (0 == g_strcmp0(name_folded, folded))
			g_ptr_array_add(named, g_object_ref(row));
	}

	return named;
}

/*
 * venture_web_reference_resolve:
 * @self: the server
 * @target: the referenced type
 * @organization: the organization the picker offered, or 0 for every one
 *   the caller may read
 * @field: the field's name, which the refusal is filed against
 * @text: what the lookup box said
 * @out_id: (out): the record it names; 0 when @text is blank
 * @error: (out) (optional): a validation error naming @field
 *
 * Resolves a bounded picker's text, as submitted with scripting off, to
 * one record: "#12" (or "12", or "#12 Acme Anvils") by id; anything else
 * by name -- the one record called exactly that, case aside, or failing
 * that the one record the search finds. No match, or more than one, is
 * refused with a message that says which, and names the candidates by id
 * so the next attempt can be exact. Every read is under the request's
 * access scope: a record the caller may not read is "no such record",
 * the same answer as one that does not exist.
 *
 * Returns: %TRUE when @text named one record, or nothing
 */
static gboolean
venture_web_reference_resolve(
	VentureWebServer	 *self,
	GType			  target,
	gint64			  organization,
	const gchar		 *field,
	const gchar		 *text,
	gint64			 *out_id,
	GError			**error
){
	VentureDatabase *database;
	g_autofree gchar *wanted = NULL;
	g_autofree gchar *folded = NULL;
	g_autofree gchar *singular = NULL;
	g_autofree gchar *plural = NULL;
	g_autofree gchar *lower = NULL;
	g_autofree gchar *lower_plural = NULL;
	g_autoptr(GPtrArray) exact = NULL;
	g_autoptr(GPtrArray) found = NULL;
	g_autoptr(GString) named = NULL;
	GPtrArray *candidates;
	gint64 id;
	guint i;

	*out_id = 0;
	database = venture_context_get_database(self->context);
	wanted = g_strstrip(g_strdup((NULL != text) ? text : ""));

	/* An emptied box clears the field, as an emptied select does. */
	if ('\0' == wanted[0])
		return TRUE;

	singular = venture_web_type_label(target, FALSE);
	plural = venture_web_type_label(target, TRUE);
	lower = g_ascii_strdown(singular, -1);
	lower_plural = g_ascii_strdown(plural, -1);

	/* By id: the record must exist and be the caller's to read. */
	if (venture_web_reference_parse_id(wanted, &id))
	{
		g_autoptr(VentureQuery) query = venture_query_new(target);
		g_autoptr(GPtrArray) rows = NULL;

		venture_query_add_filter_int(query, "id", VENTURE_FILTER_OP_EQ, id, NULL);

		if (organization > 0)
			venture_query_set_organization(query, organization);

		venture_query_set_limit(query, 1);
		rows = venture_database_find(database, query, error);

		if (NULL == rows)
			return FALSE;

		if (0 == rows->len)
		{
			venture_set_error_validation(error, field, "there is no %s #%" G_GINT64_FORMAT,
			                             lower, id);
			return FALSE;
		}

		*out_id = id;
		return TRUE;
	}

	/* By name: exactly that name first, through the column... */
	folded = g_utf8_casefold(wanted, -1);
	exact = venture_web_reference_named(self, target, organization, wanted, folded, error);

	if (NULL == exact)
		return FALSE;

	/* ...then the search the lookup itself runs, for a type whose label
	 * is not a name column, or a name typed only in part. */
	if (0 == exact->len)
	{
		g_autoptr(VentureQuery) query = venture_query_new(target);

		venture_query_set_search(query, wanted);

		if (organization > 0)
			venture_query_set_organization(query, organization);

		venture_query_set_limit(query, VENTURE_WEB_REFERENCE_RESOLVE_SCAN);
		found = venture_database_find(database, query, error);

		if (NULL == found)
			return FALSE;

		for (i = 0; i < found->len; i++)
		{
			VentureEntity *row = g_ptr_array_index(found, i);
			g_autofree gchar *display = venture_entity_get_display_name(row);
			g_autofree gchar *display_folded = g_utf8_casefold((NULL != display) ? display : "", -1);

			if (0 == g_strcmp0(display_folded, folded))
				g_ptr_array_add(exact, g_object_ref(row));
		}

		/* One record and only one matches what was typed: that one. */
		if ((0 == exact->len) && (1 == found->len))
			g_ptr_array_add(exact, g_object_ref(g_ptr_array_index(found, 0)));
	}

	if (1 == exact->len)
	{
		*out_id = venture_entity_get_id(g_ptr_array_index(exact, 0));
		return TRUE;
	}

	if ((0 == exact->len) && ((NULL == found) || (0 == found->len)))
	{
		venture_set_error_validation(error, field,
			"no %s matches \"%s\"; type a name or a #id from the %s list",
			lower, wanted, lower_plural);
		return FALSE;
	}

	/* More than one: name some of them by id, so the next try is exact. */
	candidates = (exact->len > 1) ? exact : found;
	named = g_string_new(NULL);

	for (i = 0; (i < candidates->len) && (i < VENTURE_WEB_REFERENCE_RESOLVE_NAMED); i++)
	{
		g_autofree gchar *shown = venture_web_reference_lookup_text(self, NULL,
			g_ptr_array_index(candidates, i));

		if (0 != i)
			g_string_append(named, ", ");

		g_string_append(named, shown);
	}

	if (candidates->len > VENTURE_WEB_REFERENCE_RESOLVE_NAMED)
		g_string_append(named, ", \xe2\x80\xa6");

	venture_set_error_validation(error, field,
		"\"%s\" matches more than one %s (%s); type the #id of the one you mean",
		wanted, lower, named->str);
	return FALSE;
}

/* venture_web_reference_resolve() for @spec's target. */
static gboolean
venture_web_reference_resolve_spec(
	VentureWebServer	 *self,
	VentureFieldSpec	 *spec,
	gint64			  organization,
	const gchar		 *text,
	gint64			 *out_id,
	GError			**error
){
	GType target;

	target = venture_entity_registry_lookup(venture_context_get_entity_registry(self->context),
	                                        venture_field_spec_get_reference_type(spec));

	if (G_TYPE_INVALID == target)
	{
		venture_set_error_validation(error, venture_field_spec_get_name(spec),
		                             "refers to a record type this install does not have");
		return FALSE;
	}

	return venture_web_reference_resolve(self, target, organization,
	                                     venture_field_spec_get_name(spec), text, out_id, error);
}

/*
 * The lookup a picker is drawn as past VENTURE_WEB_REFERENCE_PICKER_MAX
 * candidates: the id under @name, hidden, and the box beside it showing
 * @current ("#12 Acme Anvils"). @record is what the form fills, for the
 * label; @organization narrows the script's search as it narrowed the
 * select, and is left out when 0.
 */
static void
venture_web_append_reference_lookup(
	VentureWebServer	*self,
	GString			*content,
	const gchar		*name,
	VentureEntity		*record,
	GType			 target,
	gint64			 organization,
	const gchar		*current,
	gboolean		 required
){
	g_autofree gchar *type_name = NULL;
	g_autofree gchar *plural = NULL;
	g_autofree gchar *lower = NULL;
	g_autofree gchar *shown = NULL;
	gint64 id;

	type_name = venture_web_type_name(self, target);
	plural = venture_web_type_label(target, TRUE);
	lower = g_ascii_strdown(plural, -1);
	id = (NULL != current) ? g_ascii_strtoll(current, NULL, 10) : 0;

	/* The chosen record by name, if the caller may read it; else its id. */
	if (id > 0)
	{
		g_autoptr(VentureQuery) query = venture_query_new(target);
		g_autoptr(GPtrArray) rows = NULL;

		venture_query_add_filter_int(query, "id", VENTURE_FILTER_OP_EQ, id, NULL);
		venture_query_set_limit(query, 1);
		rows = venture_database_find(venture_context_get_database(self->context), query, NULL);

		shown = ((NULL != rows) && (rows->len > 0))
			? venture_web_reference_lookup_text(self, record, g_ptr_array_index(rows, 0))
			: g_strdup_printf("#%" G_GINT64_FORMAT, id);
	}

	g_string_append(content, "<span class=\"record-pick reference-lookup\" data-record-pick data-type=\"");
	venture_html_escape_append(content, type_name);
	g_string_append(content, "\"");

	if (organization > 0)
		g_string_append_printf(content, " data-organization=\"%" G_GINT64_FORMAT "\"", organization);

	g_string_append(content, "><input type=\"hidden\" name=\"");
	venture_html_escape_append(content, name);
	g_string_append(content, "\" value=\"");

	if (id > 0)
		g_string_append_printf(content, "%" G_GINT64_FORMAT, id);

	g_string_append(content, "\"><input type=\"text\" name=\"");
	venture_html_escape_append(content, name);
	g_string_append(content, VENTURE_WEB_REFERENCE_LOOKUP_SUFFIX "\" value=\"");
	venture_html_escape_append(content, shown);
	g_string_append(content, "\" placeholder=\"Type a name or #id\" autocomplete=\"off\" "
	                         "data-record-lookup aria-label=\"Search ");
	venture_html_escape_append(content, lower);
	g_string_append_printf(content, "\"%s></span>", required ? " required" : "");
}

/*
 * Applies the lookups a list request carries (<field>__lookup) to its
 * @filters: each is resolved, as the Filters panel's picker offered it --
 * within the active organization -- and replaces whatever the field's
 * hidden id said; a blank one drops the field's filter, which is "any" in
 * that panel. A lookup on anything but a reference field is refused, as
 * any unknown filter is.
 *
 * Returns: %FALSE with @error set when a lookup names no one record
 */
static gboolean
venture_web_list_resolve_lookups(
	VentureWebServer	 *self,
	HtmxRequest		 *request,
	GType			  entity_type,
	GHashTable		 *filters,
	GError			**error
){
	g_autoptr(VentureEntity) prototype = NULL;
	g_autoptr(GPtrArray) specs = NULL;
	GHashTable *url_params;
	GHashTableIter iter;
	gpointer key, value;

	url_params = htmx_request_get_query_params(request);

	if ((NULL == url_params) || (NULL == filters))
		return TRUE;

	g_hash_table_iter_init(&iter, url_params);

	while (g_hash_table_iter_next(&iter, &key, &value))
	{
		g_autofree gchar *base = NULL;
		g_autofree gchar *wire = NULL;
		VentureFieldSpec *spec = NULL;
		gint64 id = 0;
		guint i;

		if (!g_str_has_suffix(key, VENTURE_WEB_REFERENCE_LOOKUP_SUFFIX))
			continue;

		base = g_strndup(key, strlen(key) - strlen(VENTURE_WEB_REFERENCE_LOOKUP_SUFFIX));
		wire = g_strdelimit(g_strdup(base), "-", '_');

		if (NULL == specs)
		{
			prototype = g_object_new(entity_type, NULL);
			specs = venture_entity_get_field_specs(prototype);
		}

		/* company-id or company_id: the form's name, or the column's. */
		for (i = 0; (NULL == spec) && (i < specs->len); i++)
		{
			VentureFieldSpec *candidate = g_ptr_array_index(specs, i);
			g_autofree gchar *name = g_strdelimit(
				g_strdup(venture_field_spec_get_name(candidate)), "-", '_');

			if (0 == g_strcmp0(name, wire))
				spec = candidate;
		}

		if ((NULL == spec) || (VENTURE_FIELD_KIND_REFERENCE != venture_field_spec_get_kind(spec)))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "There is no reference field \"%s\" to look up", base);
			return FALSE;
		}

		if (!venture_web_reference_resolve_spec(self, spec,
			venture_web_active_organization(self, request), value, &id, error))
			return FALSE;

		/* Under either spelling, so no stale id filters beside it. */
		{
			g_autofree gchar *dashed = g_strdelimit(g_strdup(wire), "_", '-');

			g_hash_table_remove(filters, base);
			g_hash_table_remove(filters, wire);
			g_hash_table_remove(filters, dashed);
		}

		if (id > 0)
			g_hash_table_insert(filters, g_strdup(base),
			                    g_strdup_printf("%" G_GINT64_FORMAT, id));
	}

	return TRUE;
}
