/*
 * venture-dashboard.c - Dashboards: pages of widgets, as many as you like
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The widget kinds live here, one function each. Every kind follows the
 * same shape: read the widget's settings, build a query or run a report
 * within the scope, then write the answer twice from the same loop -- once
 * as JSON into a builder, once as HTML into a string. The two are produced
 * together so that they cannot disagree; a widget the assistant reads
 * through the API is the widget the operator sees on the page.
 */

#include "venture.h"

#include <string.h>

/* ==========================================================================
 * Results
 * ========================================================================== */

VentureWidgetResult *
venture_widget_result_new(void)
{
	return g_new0(VentureWidgetResult, 1);
}

void
venture_widget_result_free(VentureWidgetResult *self)
{
	if (NULL == self)
		return;

	g_free(self->title);
	g_free(self->html);
	g_clear_pointer(&self->data, json_node_unref);
	g_free(self->link);
	g_free(self->link_label);
	g_free(self->error);
	g_free(self);
}

/*
 * A result that says why, for the page to show in place of a body.
 */
static VentureWidgetResult *
venture_widget_result_new_error(
	const gchar	*title,
	const gchar	*message
){
	VentureWidgetResult *result;

	result = venture_widget_result_new();
	result->title = g_strdup(title);
	result->error = g_strdup(message);
	result->data = json_node_new(JSON_NODE_NULL);

	return result;
}

/*
 * "in_progress" as "In progress": nicks and field names, made readable.
 * ASCII only, deliberately -- these are identifiers the type system
 * already constrains.
 */
static gchar *
venture_widget_humanise(const gchar *name)
{
	gchar *label;
	gchar *cursor;

	if (venture_string_is_empty(name))
		return g_strdup("");

	label = g_strdup(name);

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
 * A JSON scalar as text, the way a form would have sent it.
 */
static gchar *
venture_widget_node_to_text(JsonNode *node)
{
	if (JSON_NODE_HOLDS_VALUE(node))
	{
		switch (json_node_get_value_type(node))
		{
		case G_TYPE_STRING:
			return g_strdup(json_node_get_string(node));
		case G_TYPE_BOOLEAN:
			return g_strdup(json_node_get_boolean(node) ? "true" : "false");
		case G_TYPE_INT64:
			return g_strdup_printf("%" G_GINT64_FORMAT,
			                       json_node_get_int(node));
		case G_TYPE_DOUBLE:
			return g_strdup_printf("%g", json_node_get_double(node));
		default:
			break;
		}
	}

	/* An object or an array, as its JSON text: what the options field
	 * holds when a definition writes it as a real object rather than a
	 * string. */
	return venture_json_to_string(node, FALSE);
}

/* ==========================================================================
 * Reading a widget
 * ========================================================================== */

/*
 * A string setting, or %NULL when blank. Callers own the copy.
 */
static gchar *
venture_widget_get_string(
	VentureDashboardWidget	*widget,
	const gchar		*property
){
	gchar *text = NULL;

	g_object_get(widget, property, &text, NULL);

	if (venture_string_is_empty(text))
	{
		g_free(text);
		return NULL;
	}

	g_strstrip(text);

	return text;
}

static gint64
venture_widget_get_int(
	VentureDashboardWidget	*widget,
	const gchar		*property
){
	gint64 value = 0;

	g_object_get(widget, property, &value, NULL);

	return value;
}

/*
 * The widget's options, parsed. %NULL when blank; an error when the text
 * is not a JSON object, which the save validator refuses so it should not
 * reach here.
 */
static JsonNode *
venture_widget_get_options(
	VentureDashboardWidget	 *widget,
	GError			**error
){
	g_autofree gchar *text = NULL;
	g_autoptr(JsonNode) node = NULL;

	text = venture_widget_get_string(widget, "options");

	if (NULL == text)
		return NULL;

	node = venture_json_parse(text, error);

	if (NULL == node)
		return NULL;

	if (!JSON_NODE_HOLDS_OBJECT(node))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "The widget's options must be a JSON object");
		return NULL;
	}

	return g_steal_pointer(&node);
}

static gint64
venture_widget_option_int(
	JsonNode	*options,
	const gchar	*key,
	gint64		 fallback
){
	if ((NULL == options) || !JSON_NODE_HOLDS_OBJECT(options))
		return fallback;

	return venture_json_object_get_int(json_node_get_object(options), key,
	                                   fallback);
}

static gboolean
venture_widget_option_bool(
	JsonNode	*options,
	const gchar	*key,
	gboolean	 fallback
){
	if ((NULL == options) || !JSON_NODE_HOLDS_OBJECT(options))
		return fallback;

	return venture_json_object_get_bool(json_node_get_object(options), key,
	                                    fallback);
}

/*
 * The limit, or the kind's default when the widget left it at zero.
 */
static guint
venture_widget_get_limit(
	VentureDashboardWidget	*widget,
	guint			 fallback
){
	gint64 limit;

	limit = venture_widget_get_int(widget, "limit");

	if (limit <= 0)
		return fallback;

	if (limit > 500)
		return 500;

	return (guint)limit;
}

/*
 * Resolves the widget's record type, refusing one that is off or unknown
 * with the registry's own message, so "the crm module is disabled" and
 * "no such type" come out different.
 */
static gboolean
venture_widget_resolve_type(
	VentureContext		 *context,
	const gchar		 *entity_type,
	GType			 *out_type,
	VentureEntity		**out_prototype,
	GError			**error
){
	VentureEntityRegistry *registry;

	if (venture_string_is_empty(entity_type))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "This widget needs a record type");
		return FALSE;
	}

	registry = venture_context_get_entity_registry(context);
	*out_type = venture_entity_registry_lookup(registry, entity_type);

	if (G_TYPE_INVALID == *out_type)
	{
		venture_entity_registry_set_unknown_type_error(registry, entity_type,
		                                               error);
		return FALSE;
	}

	if (NULL != out_prototype)
		*out_prototype = venture_entity_registry_get_prototype(registry,
		                                                       entity_type);

	return TRUE;
}

/*
 * Substitutes the viewer into a filter: `{me}` is their name, `{user_id}`
 * their id, `{venture_id}` the dashboard's venture. A work dashboard's
 * "my tickets" is `assignee={me}`, and the same widget answers for whoever
 * is looking.
 */
static gchar *
venture_widget_expand_placeholders(
	const gchar			*text,
	const VentureWidgetScope	*scope
){
	g_autoptr(GString) out = NULL;
	const gchar *cursor;

	out = g_string_new(NULL);

	for (cursor = text; '\0' != *cursor; cursor++)
	{
		if (g_str_has_prefix(cursor, "{me}"))
		{
			g_autofree gchar *escaped = NULL;

			escaped = g_uri_escape_string(
				(NULL != scope->username) ? scope->username : "", NULL,
				FALSE);
			g_string_append(out, escaped);
			cursor += strlen("{me}") - 1;
		}
		else if (g_str_has_prefix(cursor, "{user_id}"))
		{
			g_string_append_printf(out, "%" G_GINT64_FORMAT,
			                       scope->user_id);
			cursor += strlen("{user_id}") - 1;
		}
		else if (g_str_has_prefix(cursor, "{venture_id}"))
		{
			g_string_append_printf(out, "%" G_GINT64_FORMAT,
			                       scope->venture_id);
			cursor += strlen("{venture_id}") - 1;
		}
		else
		{
			g_string_append_c(out, *cursor);
		}
	}

	return g_string_free(g_steal_pointer(&out), FALSE);
}

/*
 * Whether a type carries a venture-id field, so a dashboard narrowed to a
 * venture can narrow this widget too.
 */
static gboolean
venture_widget_type_has_field(
	VentureEntity	*prototype,
	const gchar	*property
){
	return (NULL != prototype) &&
	       (NULL != g_object_class_find_property(G_OBJECT_GET_CLASS(prototype),
	                                             property));
}

/*
 * Applies the scope to a query: the viewer's entities, and the dashboard's
 * venture when the type has one.
 */
static void
venture_widget_scope_query(
	VentureQuery			*query,
	VentureEntity			*prototype,
	const VentureWidgetScope	*scope
){
	if (VENTURE_TYPE_ORGANIZATION == venture_query_get_entity_type(query))
		return;

	if ((NULL != scope->organization_ids) && (scope->n_organizations > 0))
		venture_query_set_organization_tree(query, scope->organization_ids,
		                                    scope->n_organizations);

	if ((0 != scope->venture_id) &&
	    venture_widget_type_has_field(prototype, "venture-id"))
	{
		venture_query_add_filter_int(query, "venture-id",
		                             VENTURE_FILTER_OP_EQ, scope->venture_id,
		                             NULL);
	}
}

/*
 * Builds the query most kinds share: the type, the filter with the viewer
 * substituted in, the order, the limit, the scope. The filter is the same
 * syntax a list page's URL takes, applied by the same parser, so what a
 * widget shows is what that URL shows.
 */
static VentureQuery *
venture_widget_build_query(
	VentureContext			 *context,
	VentureDashboardWidget		 *widget,
	const VentureWidgetScope	 *scope,
	GType				  entity_type,
	VentureEntity			 *prototype,
	guint				  default_limit,
	GError				**error
){
	g_autoptr(VentureQuery) query = NULL;
	g_autofree gchar *filter = NULL;
	g_autofree gchar *order = NULL;

	(void)context;

	query = venture_query_new(entity_type);
	filter = venture_widget_get_string(widget, "filter");

	if (NULL != filter)
	{
		g_autofree gchar *expanded = NULL;
		g_autoptr(GHashTable) params = NULL;

		expanded = venture_widget_expand_placeholders(filter, scope);
		params = g_uri_parse_params(expanded, -1, "&",
		                            G_URI_PARAMS_WWW_FORM, error);

		if (NULL == params)
		{
			g_prefix_error(error, "The widget's filter cannot be read: ");
			return NULL;
		}

		if (!venture_query_apply_query_string(query, params, error))
			return NULL;
	}

	order = venture_widget_get_string(widget, "order");

	if (NULL != order)
	{
		g_autofree gchar *property = NULL;
		const gchar *name;
		VentureSortDirection direction;

		direction = ('-' == order[0]) ? VENTURE_SORT_DESCENDING
		                              : VENTURE_SORT_ASCENDING;
		name = ('-' == order[0]) ? order + 1 : order;
		property = venture_entity_column_to_property(name);

		if (!venture_query_add_order(query, property, direction, error))
			return NULL;
	}

	venture_query_set_limit(query, venture_widget_get_limit(widget,
	                                                        default_limit));
	venture_widget_scope_query(query, prototype, scope);

	return g_steal_pointer(&query);
}

/*
 * The list page that shows what a widget counts or lists, so a figure is
 * one click from its rows. The filter goes on verbatim: it is already a
 * query string.
 */
static gchar *
venture_widget_list_path(
	VentureDashboardWidget		*widget,
	const gchar			*entity_type,
	const VentureWidgetScope	*scope
){
	g_autofree gchar *filter = NULL;
	g_autofree gchar *order = NULL;
	g_autoptr(GString) path = NULL;
	gboolean first;

	path = g_string_new("/e/");
	g_string_append(path, entity_type);
	first = TRUE;

	filter = venture_widget_get_string(widget, "filter");

	if (NULL != filter)
	{
		g_autofree gchar *expanded = NULL;

		expanded = venture_widget_expand_placeholders(filter, scope);
		g_string_append_c(path, first ? '?' : '&');
		g_string_append(path, expanded);
		first = FALSE;
	}

	order = venture_widget_get_string(widget, "order");

	if (NULL != order)
	{
		g_autofree gchar *escaped = NULL;

		escaped = g_uri_escape_string(order, NULL, FALSE);
		g_string_append_c(path, first ? '?' : '&');
		g_string_append(path, "order=");
		g_string_append(path, escaped);
	}

	return g_string_free(g_steal_pointer(&path), FALSE);
}

/*
 * Formats one field of a record for a widget: text for the JSON, and for
 * the HTML the same text, linked when the field points at another record.
 * References are resolved to their display name, because "#42" tells the
 * reader nothing and the whole point of a reference is following it.
 */
static gchar *
venture_widget_field_text(
	VentureContext		 *context,
	VentureEntity		 *record,
	VentureFieldSpec	 *spec,
	gchar			**out_link
){
	g_auto(GValue) value = G_VALUE_INIT;
	const gchar *name;

	if (NULL != out_link)
		*out_link = NULL;

	name = venture_field_spec_get_name(spec);

	if (!venture_entity_get_field(record, name, &value))
		return g_strdup("");

	if (G_VALUE_HOLDS(&value, VENTURE_TYPE_MONEY))
	{
		const VentureMoney *money;

		money = g_value_get_boxed(&value);

		return (NULL != money) ? venture_money_to_display_string(money, TRUE)
		                       : g_strdup("");
	}

	if (G_VALUE_HOLDS(&value, G_TYPE_DATE_TIME))
	{
		GDateTime *when;

		when = g_value_get_boxed(&value);

		if (NULL == when)
			return g_strdup("");

		if (VENTURE_FIELD_KIND_DATE == venture_field_spec_get_kind(spec))
			return venture_time_to_date_string(when,
				venture_context_get_timezone(context));

		return venture_time_to_relative_string(when);
	}

	if (G_VALUE_HOLDS_BOOLEAN(&value))
		return g_strdup(g_value_get_boolean(&value) ? "yes" : "no");

	if (G_VALUE_HOLDS_ENUM(&value))
	{
		return venture_widget_humanise(venture_enum_to_nick(
			G_VALUE_TYPE(&value), g_value_get_enum(&value)));
	}

	if (VENTURE_FIELD_KIND_REFERENCE == venture_field_spec_get_kind(spec))
	{
		g_autoptr(VentureEntity) target = NULL;
		const gchar *type_name;
		GType target_type;
		gint64 id;

		id = G_VALUE_HOLDS_INT64(&value) ? g_value_get_int64(&value) : 0;

		if (0 == id)
			return g_strdup("");

		type_name = venture_field_spec_get_reference_type(spec);
		target_type = venture_entity_registry_lookup(
			venture_context_get_entity_registry(context), type_name);

		if (G_TYPE_INVALID != target_type)
			target = venture_database_get(
				venture_context_get_database(context), target_type, id,
				NULL);

		if (NULL == target)
			return g_strdup_printf("#%" G_GINT64_FORMAT, id);

		if (NULL != out_link)
			*out_link = g_strdup_printf("/e/%s/%" G_GINT64_FORMAT,
			                            type_name, id);

		return venture_entity_get_display_name(target);
	}

	if (G_VALUE_HOLDS_STRING(&value))
	{
		const gchar *text;

		text = g_value_get_string(&value);

		return g_strdup((NULL != text) ? text : "");
	}

	if (G_VALUE_HOLDS_INT64(&value))
		return g_strdup_printf("%" G_GINT64_FORMAT, g_value_get_int64(&value));

	if (G_VALUE_HOLDS_DOUBLE(&value))
	{
		gdouble number;

		number = g_value_get_double(&value);

		if (number == (gdouble)(gint64)number)
			return g_strdup_printf("%" G_GINT64_FORMAT, (gint64)number);

		return g_strdup_printf("%.2f", number);
	}

	{
		g_autoptr(JsonNode) node = NULL;
		g_autofree gchar *text = NULL;

		node = venture_json_node_from_value(&value);
		text = venture_json_to_string(node, FALSE);

		if ((NULL == text) || (0 == g_strcmp0(text, "null")))
			return g_strdup("");

		return g_steal_pointer(&text);
	}
}

/*
 * Finds a field spec by its wire or property spelling.
 */
static VentureFieldSpec *
venture_widget_find_spec(
	GPtrArray	*specs,
	const gchar	*name
){
	g_autofree gchar *property = NULL;
	guint i;

	property = venture_entity_column_to_property(name);

	for (i = 0; i < specs->len; i++)
	{
		VentureFieldSpec *spec;

		spec = g_ptr_array_index(specs, i);

		if (0 == g_strcmp0(venture_field_spec_get_name(spec), property))
			return spec;
	}

	return NULL;
}

/*
 * The columns a list shows: the widget's, or the type's first few list
 * columns after the name. Never the sensitive ones, whatever was asked.
 *
 * Returns: (transfer container) (element-type VentureFieldSpec): the specs
 */
static GPtrArray *
venture_widget_choose_columns(
	VentureDashboardWidget	*widget,
	GPtrArray		*specs,
	guint			 max_columns
){
	g_autofree gchar *columns = NULL;
	GPtrArray *chosen;
	guint i;

	chosen = g_ptr_array_new();
	columns = venture_widget_get_string(widget, "columns");

	if (NULL != columns)
	{
		g_auto(GStrv) names = NULL;

		names = g_strsplit(columns, ",", -1);

		for (i = 0; NULL != names[i]; i++)
		{
			VentureFieldSpec *spec;

			g_strstrip(names[i]);
			spec = venture_widget_find_spec(specs, names[i]);

			if ((NULL != spec) &&
			    (0 == (venture_field_spec_get_flags(spec) &
			           VENTURE_COLUMN_FLAG_SENSITIVE)))
				g_ptr_array_add(chosen, spec);
		}

		return chosen;
	}

	for (i = 0; (i < specs->len) && (chosen->len < max_columns); i++)
	{
		VentureFieldSpec *spec;
		VentureFieldKind kind;

		spec = g_ptr_array_index(specs, i);
		kind = venture_field_spec_get_kind(spec);

		if (!venture_field_spec_get_show_in_list(spec))
			continue;

		if (0 != (venture_field_spec_get_flags(spec) &
		          VENTURE_COLUMN_FLAG_SENSITIVE))
			continue;

		/* Long text and JSON do not fit a card's row. */
		if ((VENTURE_FIELD_KIND_TEXT == kind) ||
		    (VENTURE_FIELD_KIND_JSON == kind))
			continue;

		/* The first searchable string is the display name, which the
		 * row's link already carries. */
		if ((0 == i) && (VENTURE_FIELD_KIND_STRING == kind))
			continue;

		g_ptr_array_add(chosen, spec);
	}

	return chosen;
}

/*
 * The heading the kind gives a widget that has none: what it reads, made
 * readable. A count of "ticket" filtered on status is "Tickets"; the
 * filter is not spelled out because the card, not the caption, says what
 * matched.
 */
static gchar *
venture_widget_type_label(
	VentureContext	*context,
	const gchar	*entity_type,
	gboolean	 plural
){
	g_autofree gchar *label = NULL;
	VentureEntity *prototype;

	prototype = venture_entity_registry_get_prototype(
		venture_context_get_entity_registry(context), entity_type);

	if (NULL == prototype)
		return venture_widget_humanise(entity_type);

	label = venture_widget_humanise(venture_entity_get_entity_name(
		prototype));

	if (!plural)
		return g_steal_pointer(&label);

	return venture_pluralise(label);
}

/* ==========================================================================
 * The kinds
 * ========================================================================== */

/*
 * An HTML "see the rows" link is built by the page from result->link; the
 * body only ever carries the answer.
 */

/* --- count: one figure ---------------------------------------------------- */

static VentureWidgetResult *
venture_widget_kind_count(
	VentureContext			 *context,
	VentureDashboardWidget		 *widget,
	const VentureWidgetScope	 *scope,
	gpointer			  user_data,
	GError				**error
){
	g_autoptr(VentureWidgetResult) result = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(GString) html = NULL;
	g_autofree gchar *entity_type = NULL;
	g_autofree gchar *period_text = NULL;
	g_autofree gchar *field = NULL;
	VentureEntity *prototype;
	GType gtype;
	gint64 count;

	(void)user_data;

	entity_type = venture_widget_get_string(widget, "entity-type");

	if (!venture_widget_resolve_type(context, entity_type, &gtype, &prototype,
	                                 error))
		return NULL;

	query = venture_widget_build_query(context, widget, scope, gtype,
	                                   prototype, 0, error);

	if (NULL == query)
		return NULL;

	/* A period bounds a date field: "sales this month", "builds failed in
	 * the last seven days". Without a field the period is meaningless
	 * and is said so, rather than silently counting everything. */
	period_text = venture_widget_get_string(widget, "period");
	field = venture_widget_get_string(widget, "field");

	if (NULL != period_text)
	{
		g_autoptr(VentureDateRange) period = NULL;
		g_autofree gchar *property = NULL;

		if (NULL == field)
		{
			g_set_error_literal(error, VENTURE_ERROR,
			                    VENTURE_ERROR_VALIDATION,
			                    "A period needs a date field to bound");
			return NULL;
		}

		period = venture_context_parse_period(context, period_text, error);

		if (NULL == period)
			return NULL;

		property = venture_entity_column_to_property(field);

		if (!venture_query_set_date_range(query, property, period, error))
			return NULL;
	}

	count = venture_database_count(venture_context_get_database(context),
	                               query, error);

	if (count < 0)
		return NULL;

	result = venture_widget_result_new();
	result->title = venture_widget_type_label(context, entity_type, TRUE);
	result->link = venture_widget_list_path(widget, entity_type, scope);

	/*
	 * The link says what is on the other end of it. "Rows" was true and
	 * useless: a card titled "Missed a promise" showing 3 with a button
	 * marked Rows tells the reader nothing about where the button goes,
	 * and every count card on a page said the same word.
	 */
	result->link_label = venture_widget_type_label(context, entity_type, TRUE);

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "count");
	json_builder_add_int_value(builder, count);
	json_builder_set_member_name(builder, "entity_type");
	json_builder_add_string_value(builder, entity_type);
	json_builder_end_object(builder);
	result->data = json_builder_get_root(builder);

	html = g_string_new("<div class=\"widget-figure\"><a class=\"figure\" href=\"");
	{
		g_autofree gchar *escaped = NULL;

		escaped = venture_attribute_escape(result->link);
		g_string_append(html, escaped);
	}
	g_string_append_printf(html, "\">%" G_GINT64_FORMAT "</a>", count);

	if (NULL != period_text)
	{
		g_string_append(html, "<span class=\"figure-note\">");
		venture_html_escape_append(html, period_text);
		g_string_append(html, "</span>");
	}

	g_string_append(html, "</div>");
	result->html = g_string_free(g_steal_pointer(&html), FALSE);

	return g_steal_pointer(&result);
}

/* --- list: rows of a type ------------------------------------------------- */

static VentureWidgetResult *
venture_widget_kind_list(
	VentureContext			 *context,
	VentureDashboardWidget		 *widget,
	const VentureWidgetScope	 *scope,
	gpointer			  user_data,
	GError				**error
){
	g_autoptr(VentureWidgetResult) result = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) records = NULL;
	g_autoptr(GPtrArray) specs = NULL;
	g_autoptr(GPtrArray) columns = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(GString) html = NULL;
	g_autofree gchar *entity_type = NULL;
	VentureEntity *prototype;
	GType gtype;
	guint i;
	guint c;

	(void)user_data;

	entity_type = venture_widget_get_string(widget, "entity-type");

	if (!venture_widget_resolve_type(context, entity_type, &gtype, &prototype,
	                                 error))
		return NULL;

	query = venture_widget_build_query(context, widget, scope, gtype,
	                                   prototype, 8, error);

	if (NULL == query)
		return NULL;

	records = venture_database_find(venture_context_get_database(context),
	                                query, error);

	if (NULL == records)
		return NULL;

	specs = venture_entity_get_field_specs(prototype);
	g_ptr_array_sort_values(specs, venture_field_spec_compare_display_order);
	columns = venture_widget_choose_columns(widget, specs, 3);

	result = venture_widget_result_new();
	result->title = venture_widget_type_label(context, entity_type, TRUE);
	result->link = venture_widget_list_path(widget, entity_type, scope);
	result->link_label = g_strdup("All");

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "entity_type");
	json_builder_add_string_value(builder, entity_type);
	json_builder_set_member_name(builder, "rows");
	json_builder_begin_array(builder);

	html = g_string_new(NULL);

	if (0 == records->len)
	{
		g_string_append(html, "<p class=\"muted\">Nothing matches.</p>");
	}
	else
	{
		g_string_append(html, "<div class=\"table-wrap\"><table class=\"data "
		                      "widget-table\"><tbody>");
	}

	for (i = 0; i < records->len; i++)
	{
		VentureEntity *record;
		g_autofree gchar *label = NULL;

		record = g_ptr_array_index(records, i);
		label = venture_entity_get_display_name(record);

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "id");
		json_builder_add_int_value(builder, venture_entity_get_id(record));
		json_builder_set_member_name(builder, "label");
		json_builder_add_string_value(builder, label);
		json_builder_set_member_name(builder, "path");
		{
			g_autofree gchar *path = NULL;

			path = g_strdup_printf("/e/%s/%" G_GINT64_FORMAT, entity_type,
			                       venture_entity_get_id(record));
			json_builder_add_string_value(builder, path);

			g_string_append(html, "<tr><td><a href=\"");
			g_string_append(html, path);
			g_string_append(html, "\">");
			venture_html_escape_append(html, label);
			g_string_append(html, "</a></td>");
		}

		for (c = 0; c < columns->len; c++)
		{
			VentureFieldSpec *spec;
			g_autofree gchar *text = NULL;
			g_autofree gchar *link = NULL;
			g_autofree gchar *key = NULL;

			spec = g_ptr_array_index(columns, c);
			text = venture_widget_field_text(context, record, spec, &link);
			key = venture_entity_property_to_column(
				venture_field_spec_get_name(spec));

			json_builder_set_member_name(builder, key);
			json_builder_add_string_value(builder, text);

			g_string_append(html, "<td>");

			if (NULL != link)
			{
				g_string_append(html, "<a href=\"");
				g_string_append(html, link);
				g_string_append(html, "\">");
				venture_html_escape_append(html, text);
				g_string_append(html, "</a>");
			}
			else if (VENTURE_FIELD_KIND_ENUM == venture_field_spec_get_kind(spec))
			{
				g_string_append(html, "<span class=\"badge\">");
				venture_html_escape_append(html, text);
				g_string_append(html, "</span>");
			}
			else
			{
				venture_html_escape_append(html, text);
			}

			g_string_append(html, "</td>");
		}

		g_string_append(html, "</tr>");
		json_builder_end_object(builder);
	}

	if (records->len > 0)
		g_string_append(html, "</tbody></table></div>");

	json_builder_end_array(builder);
	json_builder_end_object(builder);
	result->data = json_builder_get_root(builder);
	result->html = g_string_free(g_steal_pointer(&html), FALSE);

	return g_steal_pointer(&result);
}

/* --- breakdown: counts per value of an enum field ------------------------- */

static VentureWidgetResult *
venture_widget_kind_breakdown(
	VentureContext			 *context,
	VentureDashboardWidget		 *widget,
	const VentureWidgetScope	 *scope,
	gpointer			  user_data,
	GError				**error
){
	g_autoptr(VentureWidgetResult) result = NULL;
	g_autoptr(GPtrArray) specs = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(GString) html = NULL;
	g_autoptr(GArray) counts = NULL;
	g_autofree gchar *entity_type = NULL;
	g_autofree gchar *field = NULL;
	g_autofree gchar *list_path = NULL;
	VentureFieldSpec *spec;
	VentureEntity *prototype;
	const gchar *const *choices;
	GType gtype;
	gint64 total;
	gint64 largest;
	gsize i;

	(void)user_data;

	entity_type = venture_widget_get_string(widget, "entity-type");

	if (!venture_widget_resolve_type(context, entity_type, &gtype, &prototype,
	                                 error))
		return NULL;

	field = venture_widget_get_string(widget, "field");
	specs = venture_entity_get_field_specs(prototype);
	spec = venture_widget_find_spec(specs, (NULL != field) ? field : "status");

	if ((NULL == spec) ||
	    (VENTURE_FIELD_KIND_ENUM != venture_field_spec_get_kind(spec)))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "A breakdown needs a field with a fixed set of values; "
		            "%s has no such field called \"%s\"", entity_type,
		            (NULL != field) ? field : "status");
		return NULL;
	}

	choices = venture_field_spec_get_choices(spec);
	counts = g_array_new(FALSE, TRUE, sizeof(gint64));
	total = 0;
	largest = 0;

	/* One count per value. A GROUP BY would be one query, but the query
	 * layer has no aggregation and an enum has a handful of values; the
	 * clarity is worth more than the round trips. */
	for (i = 0; (NULL != choices) && (NULL != choices[i]); i++)
	{
		g_autoptr(VentureQuery) query = NULL;
		gint64 count;

		query = venture_widget_build_query(context, widget, scope, gtype,
		                                   prototype, 0, error);

		if (NULL == query)
			return NULL;

		if (!venture_query_add_filter_string(query,
		                                     venture_field_spec_get_name(spec),
		                                     VENTURE_FILTER_OP_EQ, choices[i],
		                                     error))
			return NULL;

		count = venture_database_count(venture_context_get_database(context),
		                               query, error);

		if (count < 0)
			return NULL;

		g_array_append_val(counts, count);
		total += count;

		if (count > largest)
			largest = count;
	}

	result = venture_widget_result_new();
	{
		g_autofree gchar *type_label = NULL;

		type_label = venture_widget_type_label(context, entity_type, TRUE);
		result->title = g_strdup_printf("%s by %s", type_label,
			venture_field_spec_get_label(spec));
	}
	list_path = venture_widget_list_path(widget, entity_type, scope);
	result->link = g_strdup(list_path);
	result->link_label = g_strdup("All");

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "entity_type");
	json_builder_add_string_value(builder, entity_type);
	json_builder_set_member_name(builder, "field");
	json_builder_add_string_value(builder, venture_field_spec_get_name(spec));
	json_builder_set_member_name(builder, "total");
	json_builder_add_int_value(builder, total);
	json_builder_set_member_name(builder, "groups");
	json_builder_begin_array(builder);

	html = g_string_new("<ul class=\"bar-list\">");

	for (i = 0; (NULL != choices) && (NULL != choices[i]); i++)
	{
		g_autofree gchar *label = NULL;
		g_autofree gchar *href = NULL;
		gint64 count;
		gint percent;

		count = g_array_index(counts, gint64, i);
		label = venture_widget_humanise(choices[i]);
		percent = (largest > 0) ? (gint)((count * 100) / largest) : 0;

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "value");
		json_builder_add_string_value(builder, choices[i]);
		json_builder_set_member_name(builder, "label");
		json_builder_add_string_value(builder, label);
		json_builder_set_member_name(builder, "count");
		json_builder_add_int_value(builder, count);
		json_builder_end_object(builder);

		/* Each bar links to its rows: the filter plus this value. */
		href = g_strdup_printf("%s%c%s=%s", list_path,
		                       (NULL != strchr(list_path, '?')) ? '&' : '?',
		                       (NULL != field) ? field : "status", choices[i]);

		g_string_append(html, "<li><a class=\"bar-row\" href=\"");
		{
			g_autofree gchar *escaped = NULL;

			escaped = venture_attribute_escape(href);
			g_string_append(html, escaped);
		}
		g_string_append(html, "\"><span class=\"bar-label\">");
		venture_html_escape_append(html, label);
		g_string_append_printf(html,
			"</span><span class=\"bar-track\"><span class=\"bar-fill\" "
			"style=\"width:%d%%\"></span></span>"
			"<span class=\"bar-value\">%" G_GINT64_FORMAT "</span></a></li>",
			percent, count);
	}

	g_string_append(html, "</ul>");

	if (0 == total)
		g_string_append(html, "<p class=\"muted\">Nothing matches.</p>");

	json_builder_end_array(builder);
	json_builder_end_object(builder);
	result->data = json_builder_get_root(builder);
	result->html = g_string_free(g_steal_pointer(&html), FALSE);

	return g_steal_pointer(&result);
}

/* --- upcoming: what is due soon, and what is overdue ---------------------- */

/*
 * Picks the date field: the widget's, or the type's first date field.
 */
static VentureFieldSpec *
venture_widget_find_date_field(
	GPtrArray	*specs,
	const gchar	*field
){
	guint i;

	if (NULL != field)
	{
		VentureFieldSpec *spec;

		spec = venture_widget_find_spec(specs, field);

		if (NULL == spec)
			return NULL;

		switch (venture_field_spec_get_kind(spec))
		{
		case VENTURE_FIELD_KIND_DATE:
		case VENTURE_FIELD_KIND_DATETIME:
			return spec;
		default:
			return NULL;
		}
	}

	for (i = 0; i < specs->len; i++)
	{
		VentureFieldSpec *spec;

		spec = g_ptr_array_index(specs, i);

		switch (venture_field_spec_get_kind(spec))
		{
		case VENTURE_FIELD_KIND_DATE:
		case VENTURE_FIELD_KIND_DATETIME:
			return spec;
		default:
			break;
		}
	}

	return NULL;
}

static VentureWidgetResult *
venture_widget_kind_upcoming(
	VentureContext			 *context,
	VentureDashboardWidget		 *widget,
	const VentureWidgetScope	 *scope,
	gpointer			  user_data,
	GError				**error
){
	g_autoptr(VentureWidgetResult) result = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) records = NULL;
	g_autoptr(GPtrArray) specs = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) options = NULL;
	g_autoptr(GString) html = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autoptr(GDateTime) horizon = NULL;
	g_autofree gchar *entity_type = NULL;
	g_autofree gchar *field = NULL;
	g_autofree gchar *horizon_text = NULL;
	VentureFieldSpec *spec;
	VentureEntity *prototype;
	GType gtype;
	gint64 days;
	guint i;

	(void)user_data;

	entity_type = venture_widget_get_string(widget, "entity-type");

	if (!venture_widget_resolve_type(context, entity_type, &gtype, &prototype,
	                                 error))
		return NULL;

	options = venture_widget_get_options(widget, error);

	if ((NULL == options) && (NULL != *error))
		return NULL;

	field = venture_widget_get_string(widget, "field");
	specs = venture_entity_get_field_specs(prototype);
	spec = venture_widget_find_date_field(specs, field);

	if (NULL == spec)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "%s has no date field%s%s to look ahead on", entity_type,
		            (NULL != field) ? " called " : "",
		            (NULL != field) ? field : "");
		return NULL;
	}

	days = venture_widget_option_int(options, "days", 14);

	if (days < 1)
		days = 1;

	query = venture_widget_build_query(context, widget, scope, gtype,
	                                   prototype, 10, error);

	if (NULL == query)
		return NULL;

	/* Everything dated up to the horizon, overdue first. The lower bound
	 * is deliberately open: an overdue item is the one that matters
	 * most, and a widget that hid it would be lying by omission. */
	now = g_date_time_new_now_utc();
	horizon = g_date_time_add_days(now, (gint)days);
	horizon_text = venture_time_to_string(horizon);

	if (!venture_query_add_filter_string(query, venture_field_spec_get_name(spec),
	                                     VENTURE_FILTER_OP_LTE, horizon_text,
	                                     error))
		return NULL;

	if (!venture_query_add_order(query, venture_field_spec_get_name(spec),
	                             VENTURE_SORT_ASCENDING, error))
		return NULL;

	records = venture_database_find(venture_context_get_database(context),
	                                query, error);

	if (NULL == records)
		return NULL;

	result = venture_widget_result_new();
	{
		g_autofree gchar *type_label = NULL;

		type_label = venture_widget_type_label(context, entity_type, TRUE);
		result->title = g_strdup_printf("%s due", type_label);
	}
	result->link = venture_widget_list_path(widget, entity_type, scope);
	result->link_label = g_strdup("All");

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "entity_type");
	json_builder_add_string_value(builder, entity_type);
	json_builder_set_member_name(builder, "field");
	json_builder_add_string_value(builder, venture_field_spec_get_name(spec));
	json_builder_set_member_name(builder, "days");
	json_builder_add_int_value(builder, days);
	json_builder_set_member_name(builder, "rows");
	json_builder_begin_array(builder);

	html = g_string_new(NULL);

	if (0 == records->len)
		g_string_append_printf(html,
			"<p class=\"muted\">Nothing due in the next %" G_GINT64_FORMAT
			" days.</p>", days);
	else
		g_string_append(html, "<ul class=\"relation-list\">");

	for (i = 0; i < records->len; i++)
	{
		VentureEntity *record;
		g_auto(GValue) value = G_VALUE_INIT;
		g_autofree gchar *label = NULL;
		g_autofree gchar *when_text = NULL;
		GDateTime *when = NULL;
		gboolean overdue;

		record = g_ptr_array_index(records, i);
		label = venture_entity_get_display_name(record);

		if (venture_entity_get_field(record, venture_field_spec_get_name(spec),
		                             &value) &&
		    G_VALUE_HOLDS(&value, G_TYPE_DATE_TIME))
			when = g_value_get_boxed(&value);

		overdue = (NULL != when) && (g_date_time_compare(when, now) < 0);
		when_text = (NULL != when) ? venture_time_to_relative_string(when)
		                           : g_strdup("");

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "id");
		json_builder_add_int_value(builder, venture_entity_get_id(record));
		json_builder_set_member_name(builder, "label");
		json_builder_add_string_value(builder, label);
		json_builder_set_member_name(builder, "when");
		{
			g_autofree gchar *iso = NULL;

			iso = (NULL != when) ? venture_time_to_string(when) : NULL;

			if (NULL != iso)
				json_builder_add_string_value(builder, iso);
			else
				json_builder_add_null_value(builder);
		}
		json_builder_set_member_name(builder, "overdue");
		json_builder_add_boolean_value(builder, overdue);
		json_builder_end_object(builder);

		g_string_append_printf(html,
			"<li class=\"%s\"><a href=\"/e/%s/%" G_GINT64_FORMAT "\">",
			overdue ? "overdue" : "", entity_type,
			venture_entity_get_id(record));
		venture_html_escape_append(html, label);
		g_string_append(html, "</a> <span class=\"muted\">");
		venture_html_escape_append(html, when_text);
		g_string_append(html, "</span></li>");
	}

	if (records->len > 0)
		g_string_append(html, "</ul>");

	json_builder_end_array(builder);
	json_builder_end_object(builder);
	result->data = json_builder_get_root(builder);
	result->html = g_string_free(g_steal_pointer(&html), FALSE);

	return g_steal_pointer(&result);
}

/* --- record: one record's fields ------------------------------------------ */

static VentureEntity *
venture_widget_load_record(
	VentureContext	 *context,
	const gchar	 *entity_type,
	gint64		  record_id,
	GType		 *out_type,
	GError		**error
){
	g_autoptr(VentureEntity) record = NULL;

	if (!venture_widget_resolve_type(context, entity_type, out_type, NULL,
	                                 error))
		return NULL;

	if (0 == record_id)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "This widget needs a record id");
		return NULL;
	}

	record = venture_database_get(venture_context_get_database(context),
	                              *out_type, record_id, error);

	if (NULL == record)
		return NULL;

	if (venture_entity_is_deleted(record))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "%s #%" G_GINT64_FORMAT " has been deleted", entity_type,
		            record_id);
		return NULL;
	}

	return g_steal_pointer(&record);
}

static VentureWidgetResult *
venture_widget_kind_record(
	VentureContext			 *context,
	VentureDashboardWidget		 *widget,
	const VentureWidgetScope	 *scope,
	gpointer			  user_data,
	GError				**error
){
	g_autoptr(VentureWidgetResult) result = NULL;
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(GPtrArray) specs = NULL;
	g_autoptr(GPtrArray) columns = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(GString) html = NULL;
	g_autofree gchar *entity_type = NULL;
	GType gtype;
	guint i;

	(void)scope;
	(void)user_data;

	entity_type = venture_widget_get_string(widget, "entity-type");
	record = venture_widget_load_record(context, entity_type,
		venture_widget_get_int(widget, "record-id"), &gtype, error);

	if (NULL == record)
		return NULL;

	specs = venture_entity_get_field_specs(record);
	g_ptr_array_sort_values(specs, venture_field_spec_compare_display_order);
	columns = venture_widget_choose_columns(widget, specs, 6);

	result = venture_widget_result_new();
	result->title = venture_entity_get_display_name(record);
	result->link = g_strdup_printf("/e/%s/%" G_GINT64_FORMAT, entity_type,
	                               venture_entity_get_id(record));
	result->link_label = g_strdup("Open");

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "entity_type");
	json_builder_add_string_value(builder, entity_type);
	json_builder_set_member_name(builder, "id");
	json_builder_add_int_value(builder, venture_entity_get_id(record));
	json_builder_set_member_name(builder, "label");
	json_builder_add_string_value(builder, result->title);
	json_builder_set_member_name(builder, "fields");
	json_builder_begin_object(builder);

	html = g_string_new("<dl class=\"widget-fields\">");

	for (i = 0; i < columns->len; i++)
	{
		VentureFieldSpec *spec;
		g_autofree gchar *text = NULL;
		g_autofree gchar *link = NULL;
		g_autofree gchar *key = NULL;

		spec = g_ptr_array_index(columns, i);
		text = venture_widget_field_text(context, record, spec, &link);
		key = venture_entity_property_to_column(venture_field_spec_get_name(spec));

		json_builder_set_member_name(builder, key);
		json_builder_add_string_value(builder, text);

		g_string_append(html, "<dt>");
		venture_html_escape_append(html, venture_field_spec_get_label(spec));
		g_string_append(html, "</dt><dd>");

		if (venture_string_is_empty(text))
		{
			g_string_append(html, "<span class=\"muted\">\xe2\x80\x94</span>");
		}
		else if (NULL != link)
		{
			g_string_append(html, "<a href=\"");
			g_string_append(html, link);
			g_string_append(html, "\">");
			venture_html_escape_append(html, text);
			g_string_append(html, "</a>");
		}
		else
		{
			venture_html_escape_append(html, text);
		}

		g_string_append(html, "</dd>");
	}

	g_string_append(html, "</dl>");

	json_builder_end_object(builder);
	json_builder_end_object(builder);
	result->data = json_builder_get_root(builder);
	result->html = g_string_free(g_steal_pointer(&html), FALSE);

	return g_steal_pointer(&result);
}

/* --- links: everything joined to one record ------------------------------- */

static VentureWidgetResult *
venture_widget_kind_links(
	VentureContext			 *context,
	VentureDashboardWidget		 *widget,
	const VentureWidgetScope	 *scope,
	gpointer			  user_data,
	GError				**error
){
	g_autoptr(VentureWidgetResult) result = NULL;
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(JsonNode) links = NULL;
	g_autoptr(GString) html = NULL;
	g_autofree gchar *entity_type = NULL;
	g_autofree gchar *label = NULL;
	JsonArray *array;
	GType gtype;
	gint64 record_id;
	guint i;

	(void)scope;
	(void)user_data;

	entity_type = venture_widget_get_string(widget, "entity-type");
	record_id = venture_widget_get_int(widget, "record-id");
	record = venture_widget_load_record(context, entity_type, record_id,
	                                    &gtype, error);

	if (NULL == record)
		return NULL;

	links = venture_record_link_describe_for(
		venture_context_get_database(context), entity_type, record_id, error);

	if (NULL == links)
		return NULL;

	label = venture_entity_get_display_name(record);

	result = venture_widget_result_new();
	result->title = g_strdup_printf("Linked to %s", label);
	result->link = g_strdup_printf("/e/%s/%" G_GINT64_FORMAT, entity_type,
	                               record_id);
	result->link_label = g_strdup("Open");
	result->data = g_steal_pointer(&links);

	array = json_node_get_array(result->data);
	html = g_string_new(NULL);

	if (0 == json_array_get_length(array))
	{
		g_string_append(html, "<p class=\"muted\">No links yet.</p>");
	}
	else
	{
		g_string_append(html, "<ul class=\"relation-list\">");

		for (i = 0; i < json_array_get_length(array); i++)
		{
			JsonObject *link;
			const gchar *other_type;
			const gchar *other_label;
			const gchar *note;
			gint64 other_id;
			gboolean exists;

			link = json_array_get_object_element(array, i);
			other_type = venture_json_object_get_string(link, "other_type",
			                                            "?");
			other_label = venture_json_object_get_string(link,
			                                             "other_label", "");
			other_id = venture_json_object_get_int(link, "other_id", 0);
			exists = venture_json_object_get_bool(link, "other_exists",
			                                      TRUE);
			note = venture_json_object_get_string(link, "note", NULL);

			g_string_append(html, "<li><span class=\"muted\">");
			venture_html_escape_append(html,
				venture_json_object_get_string(link, "kind_label", ""));
			g_string_append(html, "</span> ");

			if (exists)
			{
				g_string_append_printf(html,
					"<a href=\"/e/%s/%" G_GINT64_FORMAT "\">", other_type,
					other_id);
				venture_html_escape_append(html, other_label);
				g_string_append(html, "</a>");
			}
			else
			{
				venture_html_escape_append(html, other_label);
				g_string_append(html, " <span class=\"muted\">"
				                      "(unavailable)</span>");
			}

			if (!venture_string_is_empty(note))
			{
				g_string_append(html, " <span class=\"muted\">\xe2\x80\x94 ");
				venture_html_escape_append(html, note);
				g_string_append(html, "</span>");
			}

			g_string_append(html, "</li>");
		}

		g_string_append(html, "</ul>");
	}

	result->html = g_string_free(g_steal_pointer(&html), FALSE);

	return g_steal_pointer(&result);
}

/* --- metric: one headline figure from a report ---------------------------- */

/*
 * Runs the widget's report for its period. Shared by the report, metric
 * and chart kinds.
 */
static VentureReportResult *
venture_widget_run_report(
	VentureContext		 *context,
	VentureDashboardWidget	 *widget,
	VentureReport		**out_report,
	GError			**error
){
	g_autoptr(VentureDateRange) period = NULL;
	g_autofree gchar *name = NULL;
	g_autofree gchar *period_text = NULL;
	VentureReport *report;

	name = venture_widget_get_string(widget, "report-name");

	if (NULL == name)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "This widget needs a report name");
		return NULL;
	}

	report = venture_report_registry_lookup(
		venture_context_get_report_registry(context), name);

	if (NULL == report)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "There is no report called \"%s\", or its module is off",
		            name);
		return NULL;
	}

	period_text = venture_widget_get_string(widget, "period");
	period = venture_context_parse_period(context, period_text, error);

	if (NULL == period)
		return NULL;

	if (NULL != out_report)
		*out_report = report;

	return venture_report_generate(report, context, period, NULL, error);
}

static VentureWidgetResult *
venture_widget_kind_metric(
	VentureContext			 *context,
	VentureDashboardWidget		 *widget,
	const VentureWidgetScope	 *scope,
	gpointer			  user_data,
	GError				**error
){
	g_autoptr(VentureWidgetResult) result = NULL;
	g_autoptr(VentureReportResult) report_result = NULL;
	g_autoptr(GString) html = NULL;
	g_autofree gchar *key = NULL;
	g_autofree gchar *formatted = NULL;
	g_autofree gchar *change = NULL;
	VentureReport *report = NULL;
	VentureMetric *metric = NULL;
	GPtrArray *metrics;
	guint i;

	(void)scope;
	(void)user_data;

	report_result = venture_widget_run_report(context, widget, &report, error);

	if (NULL == report_result)
		return NULL;

	metrics = venture_report_result_get_metrics(report_result);
	key = venture_widget_get_string(widget, "field");

	for (i = 0; i < metrics->len; i++)
	{
		VentureMetric *candidate;

		candidate = g_ptr_array_index(metrics, i);

		if ((NULL == key) ||
		    (0 == g_strcmp0(venture_metric_get_key(candidate), key)))
		{
			metric = candidate;
			break;
		}
	}

	if (NULL == metric)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "The %s report has no headline figure%s%s",
		            venture_report_get_name(report),
		            (NULL != key) ? " called " : "",
		            (NULL != key) ? key : "");
		return NULL;
	}

	formatted = venture_metric_format_value(metric);
	change = venture_metric_format_change(metric);

	result = venture_widget_result_new();
	result->title = g_strdup(venture_metric_get_label(metric));
	result->link = g_strdup_printf("/reports/%s", venture_report_get_name(report));
	result->link_label = g_strdup("Report");
	result->data = venture_metric_to_json(metric);

	html = g_string_new("<div class=\"widget-figure\"><span class=\"figure\">");
	venture_html_escape_append(html, formatted);
	g_string_append(html, "</span>");

	if (NULL != change)
	{
		gint direction;

		direction = venture_metric_get_direction(metric);
		g_string_append_printf(html, "<span class=\"stat-delta %s\">",
			(direction > 0) ? "up" : ((direction < 0) ? "down" : "flat"));
		venture_html_escape_append(html, change);
		g_string_append(html, "</span>");
	}

	{
		g_autofree gchar *period_text = NULL;

		period_text = venture_widget_get_string(widget, "period");
		g_string_append(html, "<span class=\"figure-note\">");
		venture_html_escape_append(html,
			(NULL != period_text) ? period_text : "this_month");
		g_string_append(html, "</span>");
	}

	g_string_append(html, "</div>");
	result->html = g_string_free(g_steal_pointer(&html), FALSE);

	return g_steal_pointer(&result);
}

/* --- report: the tiles and the table -------------------------------------- */

static VentureWidgetResult *
venture_widget_kind_report(
	VentureContext			 *context,
	VentureDashboardWidget		 *widget,
	const VentureWidgetScope	 *scope,
	gpointer			  user_data,
	GError				**error
){
	g_autoptr(VentureWidgetResult) result = NULL;
	g_autoptr(VentureReportResult) report_result = NULL;
	g_autoptr(JsonNode) options = NULL;
	VentureReport *report = NULL;
	gboolean tiles;
	gboolean table;

	(void)scope;
	(void)user_data;

	options = venture_widget_get_options(widget, error);

	if ((NULL == options) && (NULL != *error))
		return NULL;

	report_result = venture_widget_run_report(context, widget, &report, error);

	if (NULL == report_result)
		return NULL;

	tiles = venture_widget_option_bool(options, "tiles", TRUE);
	table = venture_widget_option_bool(options, "table", TRUE);

	result = venture_widget_result_new();
	result->title = g_strdup(venture_report_result_get_title(report_result));
	result->link = g_strdup_printf("/reports/%s", venture_report_get_name(report));
	result->link_label = g_strdup("Report");
	result->data = venture_report_result_to_json(report_result);
	result->html = venture_report_result_render_html_body(report_result, tiles,
		table, venture_widget_get_limit(widget, 0));

	return g_steal_pointer(&result);
}

/* --- chart: a report's rows as bars --------------------------------------- */

/*
 * Reads a cell as a number: a double as itself, money as its major units,
 * anything else as nothing.
 */
static gboolean
venture_widget_cell_number(
	const GValue	*value,
	gdouble		*out_number
){
	if (NULL == value)
		return FALSE;

	if (G_VALUE_HOLDS_DOUBLE(value))
	{
		*out_number = g_value_get_double(value);
		return TRUE;
	}

	if (G_VALUE_HOLDS(value, VENTURE_TYPE_MONEY))
	{
		const VentureMoney *money;

		money = g_value_get_boxed(value);

		if (NULL == money)
			return FALSE;

		*out_number = venture_money_to_double(money);
		return TRUE;
	}

	if (G_VALUE_HOLDS_INT64(value))
	{
		*out_number = (gdouble)g_value_get_int64(value);
		return TRUE;
	}

	return FALSE;
}

static VentureWidgetResult *
venture_widget_kind_chart(
	VentureContext			 *context,
	VentureDashboardWidget		 *widget,
	const VentureWidgetScope	 *scope,
	gpointer			  user_data,
	GError				**error
){
	g_autoptr(VentureWidgetResult) result = NULL;
	g_autoptr(VentureReportResult) report_result = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(GString) html = NULL;
	g_auto(GStrv) keys = NULL;
	g_autofree gchar *value_key = NULL;
	g_autofree gchar *label_key = NULL;
	VentureReport *report = NULL;
	gdouble largest;
	guint rows;
	guint limit;
	guint i;

	(void)scope;
	(void)user_data;

	report_result = venture_widget_run_report(context, widget, &report, error);

	if (NULL == report_result)
		return NULL;

	keys = venture_report_result_get_column_keys(report_result);

	if ((NULL == keys) || (NULL == keys[0]))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "The %s report has no table to chart",
		            venture_report_get_name(report));
		return NULL;
	}

	/* The label is the first column and the value the first numeric one
	 * unless the widget says otherwise: `field` names the value column,
	 * `columns` the label column. */
	label_key = venture_widget_get_string(widget, "columns");
	value_key = venture_widget_get_string(widget, "field");

	if (NULL == label_key)
		label_key = g_strdup(keys[0]);

	rows = venture_report_result_get_row_count(report_result);

	if (NULL == value_key)
	{
		for (i = 0; (NULL != keys[i]) && (NULL == value_key); i++)
		{
			gdouble number;

			if ((rows > 0) && venture_widget_cell_number(
				venture_report_result_get_cell(report_result, 0, keys[i]),
				&number))
				value_key = g_strdup(keys[i]);
		}

		if (NULL == value_key)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			            "The %s report has no numeric column to chart",
			            venture_report_get_name(report));
			return NULL;
		}
	}

	if (!g_strv_contains((const gchar *const *)keys, value_key) ||
	    !g_strv_contains((const gchar *const *)keys, label_key))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "The %s report has no column called \"%s\"",
		            venture_report_get_name(report),
		            g_strv_contains((const gchar *const *)keys, value_key)
		                ? label_key : value_key);
		return NULL;
	}

	limit = venture_widget_get_limit(widget, 10);
	largest = 0.0;

	for (i = 0; (i < rows) && (i < limit); i++)
	{
		gdouble number;

		if (venture_widget_cell_number(
			venture_report_result_get_cell(report_result, i, value_key),
			&number) && (ABS(number) > largest))
			largest = ABS(number);
	}

	result = venture_widget_result_new();
	result->title = g_strdup(venture_report_result_get_title(report_result));
	result->link = g_strdup_printf("/reports/%s", venture_report_get_name(report));
	result->link_label = g_strdup("Report");

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "report");
	json_builder_add_string_value(builder, venture_report_get_name(report));
	json_builder_set_member_name(builder, "label_column");
	json_builder_add_string_value(builder, label_key);
	json_builder_set_member_name(builder, "value_column");
	json_builder_add_string_value(builder, value_key);
	json_builder_set_member_name(builder, "bars");
	json_builder_begin_array(builder);

	html = g_string_new("<ul class=\"bar-list\">");

	for (i = 0; (i < rows) && (i < limit); i++)
	{
		g_autofree gchar *label = NULL;
		g_autofree gchar *formatted = NULL;
		gdouble number = 0.0;
		gint percent;

		label = venture_report_result_format_cell(report_result, i, label_key);
		formatted = venture_report_result_format_cell(report_result, i,
		                                              value_key);
		venture_widget_cell_number(
			venture_report_result_get_cell(report_result, i, value_key),
			&number);
		percent = (largest > 0.0) ? (gint)((ABS(number) * 100.0) / largest)
		                          : 0;

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "label");
		json_builder_add_string_value(builder, label);
		json_builder_set_member_name(builder, "value");
		json_builder_add_double_value(builder, number);
		json_builder_set_member_name(builder, "formatted");
		json_builder_add_string_value(builder, formatted);
		json_builder_end_object(builder);

		g_string_append(html, "<li><span class=\"bar-row\"><span class=\"bar-label\">");
		venture_html_escape_append(html, label);
		g_string_append_printf(html,
			"</span><span class=\"bar-track\"><span class=\"bar-fill%s\" "
			"style=\"width:%d%%\"></span></span><span class=\"bar-value\">",
			(number < 0.0) ? " negative" : "", percent);
		venture_html_escape_append(html, formatted);
		g_string_append(html, "</span></span></li>");
	}

	g_string_append(html, "</ul>");

	if (0 == rows)
		g_string_append(html, "<p class=\"muted\">Nothing in this period.</p>");

	json_builder_end_array(builder);
	json_builder_end_object(builder);
	result->data = json_builder_get_root(builder);
	result->html = g_string_free(g_steal_pointer(&html), FALSE);

	return g_steal_pointer(&result);
}

/* --- activity: the audit trail -------------------------------------------- */

static VentureWidgetResult *
venture_widget_kind_activity(
	VentureContext			 *context,
	VentureDashboardWidget		 *widget,
	const VentureWidgetScope	 *scope,
	gpointer			  user_data,
	GError				**error
){
	g_autoptr(VentureWidgetResult) result = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) entries = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(GString) html = NULL;
	g_autofree gchar *entity_type = NULL;
	guint i;

	(void)scope;
	(void)user_data;

	query = venture_query_new(VENTURE_TYPE_AUDIT_ENTRY);
	entity_type = venture_widget_get_string(widget, "entity-type");

	/* Narrowed to one type when asked: "what changed on releases". */
	if (NULL != entity_type)
	{
		if (!venture_query_add_filter_string(query, "target-type",
		                                     VENTURE_FILTER_OP_EQ,
		                                     entity_type, error))
			return NULL;
	}

	venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, NULL);
	venture_query_set_limit(query, venture_widget_get_limit(widget, 8));

	entries = venture_database_find(venture_context_get_database(context),
	                                query, error);

	if (NULL == entries)
		return NULL;

	result = venture_widget_result_new();
	result->title = g_strdup("Recent activity");
	result->link = g_strdup("/e/audit_entry");
	result->link_label = g_strdup("Audit log");

	builder = json_builder_new();
	json_builder_begin_array(builder);
	html = g_string_new("<ul class=\"activity\">");

	for (i = 0; i < entries->len; i++)
	{
		VentureEntity *entry;
		g_autofree gchar *actor = NULL;
		g_autofree gchar *target_type = NULL;
		g_autofree gchar *target_label = NULL;
		g_autofree gchar *when = NULL;
		g_autoptr(GDateTime) occurred = NULL;
		VentureAuditAction action;
		const gchar *action_nick;
		gint64 target_id = 0;

		entry = g_ptr_array_index(entries, i);
		g_object_get(entry,
		             "actor", &actor,
		             "action", &action,
		             "target-type", &target_type,
		             "target-id", &target_id,
		             "target-label", &target_label,
		             "occurred-at", &occurred,
		             NULL);

		action_nick = venture_enum_to_nick(VENTURE_TYPE_AUDIT_ACTION,
		                                   (gint)action);
		when = (NULL != occurred) ? venture_time_to_relative_string(occurred)
		                          : NULL;

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "actor");
		json_builder_add_string_value(builder,
			venture_string_is_empty(actor) ? "The system" : actor);
		json_builder_set_member_name(builder, "action");
		json_builder_add_string_value(builder, action_nick);
		json_builder_set_member_name(builder, "target_type");
		json_builder_add_string_value(builder,
			(NULL != target_type) ? target_type : "");
		json_builder_set_member_name(builder, "target_id");
		json_builder_add_int_value(builder, target_id);
		json_builder_set_member_name(builder, "target_label");
		json_builder_add_string_value(builder,
			(NULL != target_label) ? target_label : "");
		json_builder_set_member_name(builder, "when");
		json_builder_add_string_value(builder, (NULL != when) ? when : "");
		json_builder_end_object(builder);

		g_string_append(html, "<li><span class=\"activity-actor\">");
		venture_html_escape_append(html,
			venture_string_is_empty(actor) ? "The system" : actor);
		g_string_append(html, "</span> ");
		venture_html_escape_append(html, action_nick);
		g_string_append(html, " ");

		if (!venture_string_is_empty(target_type) && (target_id > 0))
		{
			g_string_append_printf(html,
				"<a href=\"/e/%s/%" G_GINT64_FORMAT "\">", target_type,
				target_id);
			venture_html_escape_append(html,
				!venture_string_is_empty(target_label) ? target_label
				                                       : target_type);
			g_string_append(html, "</a>");
		}
		else
		{
			venture_html_escape_append(html,
				!venture_string_is_empty(target_label) ? target_label
				                                       : "something");
		}

		if (NULL != when)
		{
			g_string_append(html, " <span class=\"activity-when\">");
			venture_html_escape_append(html, when);
			g_string_append(html, "</span>");
		}

		g_string_append(html, "</li>");
	}

	if (0 == entries->len)
		g_string_append(html, "<li class=\"muted\">Nothing has happened "
		                      "yet.</li>");

	g_string_append(html, "</ul>");
	json_builder_end_array(builder);
	result->data = json_builder_get_root(builder);
	result->html = g_string_free(g_steal_pointer(&html), FALSE);

	return g_steal_pointer(&result);
}

/* --- confirmations: what is waiting for a person -------------------------- */

static VentureWidgetResult *
venture_widget_kind_confirmations(
	VentureContext			 *context,
	VentureDashboardWidget		 *widget,
	const VentureWidgetScope	 *scope,
	gpointer			  user_data,
	GError				**error
){
	g_autoptr(VentureWidgetResult) result = NULL;
	g_autoptr(GPtrArray) pending = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(GString) html = NULL;
	guint limit;
	guint i;

	(void)scope;
	(void)user_data;
	(void)error;

	pending = venture_confirmation_store_list_pending(
		venture_context_get_confirmations(context));
	limit = venture_widget_get_limit(widget, 8);

	result = venture_widget_result_new();
	result->title = g_strdup("Awaiting approval");
	result->link = g_strdup("/chat");
	result->link_label = g_strdup("Decide");

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "pending");
	json_builder_add_int_value(builder, (gint64)pending->len);
	json_builder_set_member_name(builder, "items");
	json_builder_begin_array(builder);

	html = g_string_new(NULL);

	if (0 == pending->len)
	{
		g_string_append(html, "<p class=\"muted\">Nothing is waiting on "
		                      "you.</p>");
	}
	else
	{
		g_string_append_printf(html,
			"<p class=\"widget-lead\"><strong>%u</strong> staged change%s "
			"waiting</p><ul class=\"relation-list\">", pending->len,
			(1 == pending->len) ? "" : "s");
	}

	for (i = 0; (i < pending->len) && (i < limit); i++)
	{
		VentureConfirmation *confirmation;

		confirmation = g_ptr_array_index(pending, i);

		json_builder_add_value(builder,
			venture_confirmation_to_json(confirmation));

		g_string_append(html, "<li>");
		venture_html_escape_append(html,
			venture_confirmation_get_summary(confirmation));
		g_string_append(html, "</li>");
	}

	if (pending->len > 0)
		g_string_append(html, "</ul>");

	json_builder_end_array(builder);
	json_builder_end_object(builder);
	result->data = json_builder_get_root(builder);
	result->html = g_string_free(g_steal_pointer(&html), FALSE);

	return g_steal_pointer(&result);
}

/* --- note: words --------------------------------------------------------- */

/*
 * A note is plain text with two conveniences: a line starting "- " is a
 * bullet, and a site-relative path or an http(s) URL becomes a link. No
 * Markdown engine, because a runbook of four lines does not need one and
 * the escaping rules of a full one are a place for mistakes to live.
 */
static void
venture_widget_append_note_line(
	GString		*html,
	const gchar	*line
){
	const gchar *cursor;

	cursor = line;

	while ('\0' != *cursor)
	{
		const gchar *start;
		const gchar *end;

		start = NULL;

		if (g_str_has_prefix(cursor, "http://") ||
		    g_str_has_prefix(cursor, "https://") ||
		    ((cursor == line || ' ' == cursor[-1] || '(' == cursor[-1]) &&
		     ('/' == cursor[0]) && ('/' != cursor[1]) && ('\0' != cursor[1]) &&
		     (' ' != cursor[1])))
			start = cursor;

		if (NULL == start)
		{
			g_autofree gchar *one = NULL;

			one = g_strndup(cursor, (gsize)(g_utf8_next_char(cursor) - cursor));
			venture_html_escape_append(html, one);
			cursor = g_utf8_next_char(cursor);
			continue;
		}

		end = start;

		while (('\0' != *end) && !g_ascii_isspace(*end))
			end++;

		/* Trailing punctuation belongs to the sentence, not the URL. */
		while ((end > start) && (NULL != strchr(".,;:)!?", end[-1])))
			end--;

		{
			g_autofree gchar *href = NULL;
			g_autofree gchar *escaped = NULL;

			href = g_strndup(start, (gsize)(end - start));
			escaped = venture_attribute_escape(href);
			g_string_append(html, "<a href=\"");
			g_string_append(html, escaped);
			g_string_append(html, "\">");
			venture_html_escape_append(html, href);
			g_string_append(html, "</a>");
		}

		cursor = end;
	}
}

static VentureWidgetResult *
venture_widget_kind_note(
	VentureContext			 *context,
	VentureDashboardWidget		 *widget,
	const VentureWidgetScope	 *scope,
	gpointer			  user_data,
	GError				**error
){
	g_autoptr(VentureWidgetResult) result = NULL;
	g_autoptr(GString) html = NULL;
	g_auto(GStrv) lines = NULL;
	g_autofree gchar *body = NULL;
	gboolean in_list;
	gboolean in_paragraph;
	guint i;

	(void)context;
	(void)scope;
	(void)user_data;
	(void)error;

	body = venture_widget_get_string(widget, "body");

	result = venture_widget_result_new();
	result->title = g_strdup("Note");
	result->data = json_node_new(JSON_NODE_VALUE);
	json_node_set_string(result->data, (NULL != body) ? body : "");

	html = g_string_new("<div class=\"note-body\">");

	if (NULL == body)
	{
		g_string_append(html, "<p class=\"muted\">An empty note. Edit the "
		                      "widget to write one.</p>");
	}

	lines = g_strsplit((NULL != body) ? body : "", "\n", -1);
	in_list = FALSE;
	in_paragraph = FALSE;

	for (i = 0; NULL != lines[i]; i++)
	{
		gchar *line;

		line = g_strstrip(lines[i]);

		if ('\0' == *line)
		{
			if (in_list)
				g_string_append(html, "</ul>");

			if (in_paragraph)
				g_string_append(html, "</p>");

			in_list = FALSE;
			in_paragraph = FALSE;
			continue;
		}

		if (g_str_has_prefix(line, "- ") || g_str_has_prefix(line, "* "))
		{
			if (in_paragraph)
				g_string_append(html, "</p>");

			in_paragraph = FALSE;

			if (!in_list)
				g_string_append(html, "<ul>");

			in_list = TRUE;
			g_string_append(html, "<li>");
			venture_widget_append_note_line(html, line + 2);
			g_string_append(html, "</li>");
			continue;
		}

		if (in_list)
			g_string_append(html, "</ul>");

		in_list = FALSE;

		if (!in_paragraph)
			g_string_append(html, "<p>");
		else
			g_string_append(html, "<br>");

		in_paragraph = TRUE;
		venture_widget_append_note_line(html, line);
	}

	if (in_list)
		g_string_append(html, "</ul>");

	if (in_paragraph)
		g_string_append(html, "</p>");

	g_string_append(html, "</div>");
	result->html = g_string_free(g_steal_pointer(&html), FALSE);

	return g_steal_pointer(&result);
}

/* --- actions: buttons ----------------------------------------------------- */

/*
 * Only site-relative paths and http(s) URLs are honoured. A javascript:
 * href typed into a widget by an editor would run for every viewer, which
 * is a cross-site script by another name.
 */
static gboolean
venture_widget_action_href_is_safe(const gchar *href)
{
	if (venture_string_is_empty(href))
		return FALSE;

	if (('/' == href[0]) && ('/' != href[1]))
		return TRUE;

	return g_str_has_prefix(href, "http://") ||
	       g_str_has_prefix(href, "https://");
}

static VentureWidgetResult *
venture_widget_kind_actions(
	VentureContext			 *context,
	VentureDashboardWidget		 *widget,
	const VentureWidgetScope	 *scope,
	gpointer			  user_data,
	GError				**error
){
	g_autoptr(VentureWidgetResult) result = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(GString) html = NULL;
	g_auto(GStrv) lines = NULL;
	g_autofree gchar *body = NULL;
	guint shown;
	guint i;

	(void)context;
	(void)user_data;
	(void)error;

	body = venture_widget_get_string(widget, "body");

	result = venture_widget_result_new();
	result->title = g_strdup("Actions");

	builder = json_builder_new();
	json_builder_begin_array(builder);
	html = g_string_new("<div class=\"actions-body\">");
	lines = g_strsplit((NULL != body) ? body : "", "\n", -1);
	shown = 0;

	for (i = 0; NULL != lines[i]; i++)
	{
		g_autofree gchar *expanded = NULL;
		g_autofree gchar *escaped = NULL;
		gchar *line;
		gchar *bar;
		const gchar *label;
		const gchar *href;

		line = g_strstrip(lines[i]);

		if ('\0' == *line)
			continue;

		/* "Label | /path"; a bare path is its own label. */
		bar = strchr(line, '|');

		if (NULL != bar)
		{
			*bar = '\0';
			label = g_strstrip(line);
			href = g_strstrip(bar + 1);
		}
		else
		{
			label = line;
			href = line;
		}

		expanded = venture_widget_expand_placeholders(href, scope);

		if (!venture_widget_action_href_is_safe(expanded))
			continue;

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "label");
		json_builder_add_string_value(builder, label);
		json_builder_set_member_name(builder, "href");
		json_builder_add_string_value(builder, expanded);
		json_builder_end_object(builder);

		escaped = venture_attribute_escape(expanded);
		g_string_append(html, "<a class=\"btn\" href=\"");
		g_string_append(html, escaped);
		g_string_append(html, "\">");
		venture_html_escape_append(html, label);
		g_string_append(html, "</a>");
		shown++;
	}

	if (0 == shown)
		g_string_append(html, "<p class=\"muted\">No actions yet. Add one "
		                      "per line as <code>Label | /path</code>.</p>");

	g_string_append(html, "</div>");
	json_builder_end_array(builder);
	result->data = json_builder_get_root(builder);
	result->html = g_string_free(g_steal_pointer(&html), FALSE);

	return g_steal_pointer(&result);
}

/* --- search: a box -------------------------------------------------------- */

static VentureWidgetResult *
venture_widget_kind_search(
	VentureContext			 *context,
	VentureDashboardWidget		 *widget,
	const VentureWidgetScope	 *scope,
	gpointer			  user_data,
	GError				**error
){
	g_autoptr(VentureWidgetResult) result = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(GString) html = NULL;
	g_autofree gchar *entity_type = NULL;
	g_autofree gchar *action = NULL;
	const gchar *param;

	(void)scope;
	(void)user_data;

	entity_type = venture_widget_get_string(widget, "entity-type");

	/* Scoped to a type it searches that type's list; otherwise it is the
	 * global search, which reaches every type at once. */
	if (NULL != entity_type)
	{
		GType gtype;

		if (!venture_widget_resolve_type(context, entity_type, &gtype, NULL,
		                                 error))
			return NULL;

		action = g_strdup_printf("/e/%s", entity_type);
		param = "search";
	}
	else
	{
		action = g_strdup("/search");
		param = "q";
	}

	result = venture_widget_result_new();

	if (NULL != entity_type)
	{
		g_autofree gchar *label = NULL;

		label = venture_widget_type_label(context, entity_type, TRUE);
		result->title = g_strdup_printf("Search %s", label);
	}
	else
	{
		result->title = g_strdup("Search");
	}

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "action");
	json_builder_add_string_value(builder, action);
	json_builder_set_member_name(builder, "parameter");
	json_builder_add_string_value(builder, param);
	json_builder_end_object(builder);
	result->data = json_builder_get_root(builder);

	html = g_string_new("<form class=\"search-form\" method=\"get\" action=\"");
	g_string_append(html, action);
	g_string_append_printf(html,
		"\"><input type=\"search\" name=\"%s\" placeholder=\"Search\xe2\x80\xa6\">"
		"<button class=\"btn btn-primary\" type=\"submit\">Go</button></form>",
		param);
	result->html = g_string_free(g_steal_pointer(&html), FALSE);

	return g_steal_pointer(&result);
}

/* --- environments: what each is running (factory) ------------------------- */

/*
 * The latest deployment that succeeded into an environment.
 */
static VentureEntity *
venture_widget_current_deployment(
	VentureContext	*context,
	gint64		 environment_id
){
	g_autoptr(VentureQuery) query = NULL;

	query = venture_query_new(VENTURE_TYPE_DEPLOYMENT);

	if (!venture_query_add_filter_int(query, "environment-id",
	                                  VENTURE_FILTER_OP_EQ, environment_id,
	                                  NULL) ||
	    !venture_query_add_filter_string(query, "status", VENTURE_FILTER_OP_EQ,
	                                     "succeeded", NULL))
		return NULL;

	venture_query_add_order(query, "deployed-at", VENTURE_SORT_DESCENDING,
	                        NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, NULL);
	venture_query_set_limit(query, 1);

	return venture_database_find_one(venture_context_get_database(context),
	                                 query, NULL);
}

static VentureWidgetResult *
venture_widget_kind_environments(
	VentureContext			 *context,
	VentureDashboardWidget		 *widget,
	const VentureWidgetScope	 *scope,
	gpointer			  user_data,
	GError				**error
){
	g_autoptr(VentureWidgetResult) result = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) environments = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(GString) html = NULL;
	VentureEntity *prototype;
	guint i;

	(void)user_data;

	prototype = venture_entity_registry_get_prototype(
		venture_context_get_entity_registry(context), "environment");
	query = venture_widget_build_query(context, widget, scope,
	                                   VENTURE_TYPE_ENVIRONMENT, prototype, 12,
	                                   error);

	if (NULL == query)
		return NULL;

	{
		g_autofree gchar *order = NULL;

		order = venture_widget_get_string(widget, "order");

		if (NULL == order)
		{
			venture_query_add_order(query, "kind", VENTURE_SORT_DESCENDING,
			                        NULL);
			venture_query_add_order(query, "name", VENTURE_SORT_ASCENDING,
			                        NULL);
		}
	}

	environments = venture_database_find(venture_context_get_database(context),
	                                     query, error);

	if (NULL == environments)
		return NULL;

	result = venture_widget_result_new();
	result->title = g_strdup("Environments");
	result->link = g_strdup("/e/environment");
	result->link_label = g_strdup("All");

	builder = json_builder_new();
	json_builder_begin_array(builder);
	html = g_string_new(NULL);

	if (0 == environments->len)
		g_string_append(html, "<p class=\"muted\">No environments yet.</p>");
	else
		g_string_append(html, "<ul class=\"relation-list\">");

	for (i = 0; i < environments->len; i++)
	{
		VentureEntity *environment;
		g_autoptr(VentureEntity) deployment = NULL;
		g_autoptr(VentureEntity) release = NULL;
		g_autofree gchar *name = NULL;
		g_autofree gchar *release_label = NULL;
		g_autoptr(GDateTime) deployed_at = NULL;
		VentureEnvironmentKind kind;
		gint64 release_id = 0;

		environment = g_ptr_array_index(environments, i);
		g_object_get(environment, "name", &name, "kind", &kind, NULL);

		deployment = venture_widget_current_deployment(context,
			venture_entity_get_id(environment));

		if (NULL != deployment)
		{
			g_object_get(deployment, "release-id", &release_id,
			             "deployed-at", &deployed_at, NULL);

			if (0 != release_id)
				release = venture_database_get(
					venture_context_get_database(context),
					VENTURE_TYPE_RELEASE, release_id, NULL);
		}

		if (NULL != release)
			release_label = venture_entity_get_display_name(release);

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "id");
		json_builder_add_int_value(builder, venture_entity_get_id(environment));
		json_builder_set_member_name(builder, "name");
		json_builder_add_string_value(builder, name);
		json_builder_set_member_name(builder, "kind");
		json_builder_add_string_value(builder,
			venture_enum_to_nick(VENTURE_TYPE_ENVIRONMENT_KIND, (gint)kind));
		json_builder_set_member_name(builder, "release_id");
		json_builder_add_int_value(builder, release_id);
		json_builder_set_member_name(builder, "release");

		if (NULL != release_label)
			json_builder_add_string_value(builder, release_label);
		else
			json_builder_add_null_value(builder);

		json_builder_set_member_name(builder, "deployed_at");

		if (NULL != deployed_at)
		{
			g_autofree gchar *iso = NULL;

			iso = venture_time_to_string(deployed_at);
			json_builder_add_string_value(builder, iso);
		}
		else
		{
			json_builder_add_null_value(builder);
		}

		json_builder_end_object(builder);

		g_string_append_printf(html,
			"<li><a href=\"/e/environment/%" G_GINT64_FORMAT "\">",
			venture_entity_get_id(environment));
		venture_html_escape_append(html, name);
		g_string_append(html, "</a> <span class=\"badge\">");
		venture_html_escape_append(html,
			venture_enum_to_nick(VENTURE_TYPE_ENVIRONMENT_KIND, (gint)kind));
		g_string_append(html, "</span> ");

		if (NULL != release_label)
		{
			g_string_append_printf(html,
				"<a href=\"/e/release/%" G_GINT64_FORMAT "\">", release_id);
			venture_html_escape_append(html, release_label);
			g_string_append(html, "</a>");

			if (NULL != deployed_at)
			{
				g_autofree gchar *when = NULL;

				when = venture_time_to_relative_string(deployed_at);
				g_string_append(html, " <span class=\"muted\">");
				venture_html_escape_append(html, when);
				g_string_append(html, "</span>");
			}
		}
		else
		{
			g_string_append(html, "<span class=\"muted\">nothing deployed"
			                      "</span>");
		}

		g_string_append(html, "</li>");
	}

	if (environments->len > 0)
		g_string_append(html, "</ul>");

	json_builder_end_array(builder);
	result->data = json_builder_get_root(builder);
	result->html = g_string_free(g_steal_pointer(&html), FALSE);

	return g_steal_pointer(&result);
}

/* --- milestone: one milestone's progress (factory) ------------------------ */

static VentureWidgetResult *
venture_widget_kind_milestone(
	VentureContext			 *context,
	VentureDashboardWidget		 *widget,
	const VentureWidgetScope	 *scope,
	gpointer			  user_data,
	GError				**error
){
	g_autoptr(VentureWidgetResult) result = NULL;
	g_autoptr(VentureEntity) milestone = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(GString) html = NULL;
	g_autofree gchar *name = NULL;
	g_autoptr(GDateTime) due = NULL;
	VentureMilestoneStatus status;
	GType gtype;
	gint64 record_id;
	gint64 total;
	gint64 done;
	gint percent;

	(void)scope;
	(void)user_data;

	record_id = venture_widget_get_int(widget, "record-id");

	/* No id: the soonest-due active milestone, so the widget on a
	 * template lands on something without editing. */
	if (0 == record_id)
	{
		g_autoptr(VentureQuery) query = NULL;

		query = venture_query_new(VENTURE_TYPE_MILESTONE);
		venture_query_add_filter_string(query, "status", VENTURE_FILTER_OP_NE,
		                                "completed", NULL);
		venture_query_add_filter_string(query, "status", VENTURE_FILTER_OP_NE,
		                                "cancelled", NULL);
		venture_query_add_order(query, "due-on", VENTURE_SORT_ASCENDING, NULL);
		venture_query_set_limit(query, 1);
		milestone = venture_database_find_one(
			venture_context_get_database(context), query, error);

		if (NULL == milestone)
		{
			if (NULL != *error)
				return NULL;

			result = venture_widget_result_new();
			result->title = g_strdup("Milestone");
			result->link = g_strdup("/e/milestone");
			result->link_label = g_strdup("All");
			result->data = json_node_new(JSON_NODE_NULL);
			result->html = g_strdup("<p class=\"muted\">No open milestone. "
			                        "Plan one, or point the widget at a "
			                        "specific one.</p>");
			return g_steal_pointer(&result);
		}
	}
	else
	{
		milestone = venture_widget_load_record(context, "milestone", record_id,
		                                       &gtype, error);

		if (NULL == milestone)
			return NULL;
	}

	g_object_get(milestone, "name", &name, "status", &status, "due-on", &due,
	             NULL);

	/* Progress is tickets done over tickets planned, the same figure the
	 * milestone's own page shows. */
	{
		g_autoptr(VentureQuery) all = NULL;
		g_autoptr(VentureQuery) finished = NULL;

		all = venture_query_new(VENTURE_TYPE_TICKET);
		venture_query_add_filter_int(all, "milestone-id", VENTURE_FILTER_OP_EQ,
		                             venture_entity_get_id(milestone), NULL);
		total = venture_database_count(venture_context_get_database(context),
		                               all, NULL);

		finished = venture_query_new(VENTURE_TYPE_TICKET);
		venture_query_add_filter_int(finished, "milestone-id",
		                             VENTURE_FILTER_OP_EQ,
		                             venture_entity_get_id(milestone), NULL);
		venture_query_add_filter_string(finished, "status",
		                                VENTURE_FILTER_OP_EQ, "done", NULL);
		done = venture_database_count(venture_context_get_database(context),
		                              finished, NULL);
	}

	if (total < 0)
		total = 0;

	if (done < 0)
		done = 0;

	percent = (total > 0) ? (gint)((done * 100) / total) : 0;

	result = venture_widget_result_new();
	result->title = g_strdup_printf("Milestone %s", name);
	result->link = g_strdup_printf("/e/milestone/%" G_GINT64_FORMAT,
	                               venture_entity_get_id(milestone));
	result->link_label = g_strdup("Open");

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "id");
	json_builder_add_int_value(builder, venture_entity_get_id(milestone));
	json_builder_set_member_name(builder, "name");
	json_builder_add_string_value(builder, name);
	json_builder_set_member_name(builder, "status");
	json_builder_add_string_value(builder,
		venture_enum_to_nick(VENTURE_TYPE_MILESTONE_STATUS, (gint)status));
	json_builder_set_member_name(builder, "tickets");
	json_builder_add_int_value(builder, total);
	json_builder_set_member_name(builder, "done");
	json_builder_add_int_value(builder, done);
	json_builder_set_member_name(builder, "percent");
	json_builder_add_int_value(builder, percent);
	json_builder_set_member_name(builder, "due_on");

	if (NULL != due)
	{
		g_autofree gchar *iso = NULL;

		iso = venture_time_to_date_string(due,
			venture_context_get_timezone(context));
		json_builder_add_string_value(builder, iso);
	}
	else
	{
		json_builder_add_null_value(builder);
	}

	json_builder_end_object(builder);
	result->data = json_builder_get_root(builder);

	html = g_string_new("<div class=\"widget-progress\">");
	g_string_append_printf(html,
		"<div class=\"progress\"><div class=\"progress-fill\" "
		"style=\"width:%d%%\"></div></div>"
		"<p><strong>%" G_GINT64_FORMAT "</strong> of <strong>%" G_GINT64_FORMAT
		"</strong> tickets done (%d%%) <span class=\"badge\">", percent, done,
		total, percent);
	venture_html_escape_append(html,
		venture_enum_to_nick(VENTURE_TYPE_MILESTONE_STATUS, (gint)status));
	g_string_append(html, "</span>");

	if (NULL != due)
	{
		g_autofree gchar *when = NULL;

		when = venture_time_to_relative_string(due);
		g_string_append(html, " <span class=\"muted\">due ");
		venture_html_escape_append(html, when);
		g_string_append(html, "</span>");
	}

	g_string_append_printf(html,
		"</p><p><a href=\"/e/ticket?milestone_id=%" G_GINT64_FORMAT
		"\">Its tickets</a></p></div>", venture_entity_get_id(milestone));
	result->html = g_string_free(g_steal_pointer(&html), FALSE);

	return g_steal_pointer(&result);
}

/* ==========================================================================
 * The registry
 * ========================================================================== */

typedef struct
{
	const VentureWidgetKindInfo	*info;
	gpointer			 user_data;
	GDestroyNotify			 destroy;
} VentureWidgetKindEntry;

static void
venture_widget_kind_entry_free(gpointer data)
{
	VentureWidgetKindEntry *entry;

	entry = data;

	if (NULL != entry->destroy)
		entry->destroy(entry->user_data);

	g_free(entry);
}

struct _VentureWidgetKindRegistry
{
	GObject parent_instance;

	GHashTable	*kinds;		/* name -> VentureWidgetKindEntry */
	GPtrArray	*order;		/* names, in registration order */
};

G_DEFINE_FINAL_TYPE(VentureWidgetKindRegistry, venture_widget_kind_registry,
                    G_TYPE_OBJECT)

static void
venture_widget_kind_registry_finalize(GObject *object)
{
	VentureWidgetKindRegistry *self;

	self = VENTURE_WIDGET_KIND_REGISTRY(object);

	g_clear_pointer(&self->kinds, g_hash_table_unref);
	g_clear_pointer(&self->order, g_ptr_array_unref);

	G_OBJECT_CLASS(venture_widget_kind_registry_parent_class)->finalize(object);
}

static void
venture_widget_kind_registry_class_init(VentureWidgetKindRegistryClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = venture_widget_kind_registry_finalize;
}

static void
venture_widget_kind_registry_init(VentureWidgetKindRegistry *self)
{
	self->kinds = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                    venture_widget_kind_entry_free);
	self->order = g_ptr_array_new_with_free_func(g_free);
}

#define VENTURE_WIDGET_USES(...) \
	((const gchar *const []){ __VA_ARGS__, NULL })

/*
 * What each kind reads, in the wire spelling the editor and the API use.
 * A kind is described by this table and nothing else: the editor shows
 * these fields for it, the documentation is generated from it, and a
 * widget of the kind is refused nothing it does not read.
 */
static const gchar *const venture_widget_uses_count[] = {
	"entity_type", "filter", "period", "field", NULL
};
static const gchar *const venture_widget_uses_list[] = {
	"entity_type", "filter", "order", "limit", "columns", NULL
};
static const gchar *const venture_widget_uses_breakdown[] = {
	"entity_type", "field", "filter", NULL
};
static const gchar *const venture_widget_uses_upcoming[] = {
	"entity_type", "field", "filter", "limit", "options", NULL
};
static const gchar *const venture_widget_uses_record[] = {
	"entity_type", "record_id", "columns", NULL
};
static const gchar *const venture_widget_uses_links[] = {
	"entity_type", "record_id", NULL
};
static const gchar *const venture_widget_uses_metric[] = {
	"report_name", "period", "field", NULL
};
static const gchar *const venture_widget_uses_report[] = {
	"report_name", "period", "limit", "options", NULL
};
static const gchar *const venture_widget_uses_chart[] = {
	"report_name", "period", "field", "columns", "limit", NULL
};
static const gchar *const venture_widget_uses_activity[] = {
	"entity_type", "limit", NULL
};
static const gchar *const venture_widget_uses_confirmations[] = {
	"limit", NULL
};
static const gchar *const venture_widget_uses_body[] = { "body", NULL };
static const gchar *const venture_widget_uses_search[] = {
	"entity_type", NULL
};
static const gchar *const venture_widget_uses_environments[] = {
	"filter", "order", "limit", NULL
};
static const gchar *const venture_widget_uses_milestone[] = {
	"record_id", NULL
};

static const VentureWidgetKindInfo venture_widget_builtin_kinds[] = {
	{
		"count", "Count",
		"One figure: how many records of a type match a filter, "
		"optionally within a period on a date field.",
		NULL, venture_widget_uses_count, venture_widget_kind_count
	},
	{
		"list", "List",
		"The newest few records of a type that match a filter, with the "
		"columns you choose, each linking to its page.",
		NULL, venture_widget_uses_list, venture_widget_kind_list
	},
	{
		"breakdown", "Breakdown",
		"Counts per value of a field with a fixed set of values -- "
		"tickets by status, deals by stage -- as bars that link to "
		"their rows.",
		NULL, venture_widget_uses_breakdown, venture_widget_kind_breakdown
	},
	{
		"upcoming", "Upcoming",
		"What is due within the next so many days on a date field, "
		"overdue first.",
		NULL, venture_widget_uses_upcoming, venture_widget_kind_upcoming
	},
	{
		"record", "Record",
		"One record's fields, for the thing this page is about.",
		NULL, venture_widget_uses_record, venture_widget_kind_record
	},
	{
		"links", "Links",
		"Everything linked to one record, read from it.",
		NULL, venture_widget_uses_links, venture_widget_kind_links
	},
	{
		"metric", "Metric",
		"One headline figure from a report, with its change against the "
		"previous period.",
		NULL, venture_widget_uses_metric, venture_widget_kind_metric
	},
	{
		"report", "Report",
		"A whole report for a period: the headline tiles, the table, or "
		"both.",
		NULL, venture_widget_uses_report, venture_widget_kind_report
	},
	{
		"chart", "Chart",
		"A report's rows as bars: one column for the labels, one for "
		"the lengths.",
		NULL, venture_widget_uses_chart, venture_widget_kind_chart
	},
	{
		"activity", "Activity",
		"The latest changes from the audit trail, optionally for one "
		"record type.",
		NULL, venture_widget_uses_activity, venture_widget_kind_activity
	},
	{
		"confirmations", "Awaiting approval",
		"Staged changes waiting for a person to decide.",
		NULL, venture_widget_uses_confirmations,
		venture_widget_kind_confirmations
	},
	{
		"note", "Note",
		"Words: a runbook, a reminder, a definition of done. Lines "
		"starting with - are bullets; paths and URLs become links.",
		NULL, venture_widget_uses_body, venture_widget_kind_note
	},
	{
		"actions", "Actions",
		"Buttons, one per line as Label | /path -- the things you do "
		"from this page.",
		NULL, venture_widget_uses_body, venture_widget_kind_actions
	},
	{
		"search", "Search",
		"A search box, for everything or for one record type.",
		NULL, venture_widget_uses_search, venture_widget_kind_search
	},
	{
		"environments", "Environments",
		"Each environment and the release it is running now.",
		"factory", venture_widget_uses_environments,
		venture_widget_kind_environments
	},
	{
		"milestone", "Milestone progress",
		"One milestone's tickets done over planned; the soonest-due "
		"open one when no id is given.",
		"factory", venture_widget_uses_milestone,
		venture_widget_kind_milestone
	}
};

static gboolean
venture_widget_kind_name_is_valid(const gchar *name)
{
	const gchar *cursor;

	if (venture_string_is_empty(name))
		return FALSE;

	for (cursor = name; '\0' != *cursor; cursor++)
	{
		if (!g_ascii_islower(*cursor) && !g_ascii_isdigit(*cursor) &&
		    ('_' != *cursor))
			return FALSE;
	}

	return TRUE;
}

gboolean
venture_widget_kind_registry_add(
	VentureWidgetKindRegistry	 *self,
	const VentureWidgetKindInfo	 *info,
	gpointer			  user_data,
	GDestroyNotify			  destroy,
	GError				**error
){
	VentureWidgetKindEntry *entry;

	g_return_val_if_fail(VENTURE_IS_WIDGET_KIND_REGISTRY(self), FALSE);
	g_return_val_if_fail(NULL != info, FALSE);

	if (!venture_widget_kind_name_is_valid(info->name))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "\"%s\" is not a widget kind name: use lowercase "
		            "letters, digits and underscores",
		            (NULL != info->name) ? info->name : "");
		return FALSE;
	}

	if (NULL == info->func)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "Widget kind \"%s\" has no implementation", info->name);
		return FALSE;
	}

	if (g_hash_table_contains(self->kinds, info->name))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_ALREADY_EXISTS,
		            "A widget kind called \"%s\" is already registered",
		            info->name);
		return FALSE;
	}

	entry = g_new0(VentureWidgetKindEntry, 1);
	entry->info = info;
	entry->user_data = user_data;
	entry->destroy = destroy;

	g_hash_table_insert(self->kinds, g_strdup(info->name), entry);
	g_ptr_array_add(self->order, g_strdup(info->name));

	return TRUE;
}

VentureWidgetKindRegistry *
venture_widget_kind_registry_get_default(void)
{
	static VentureWidgetKindRegistry *instance = NULL;
	static gsize once = 0;

	if (g_once_init_enter(&once))
	{
		gsize i;

		instance = g_object_new(VENTURE_TYPE_WIDGET_KIND_REGISTRY, NULL);

		for (i = 0; i < G_N_ELEMENTS(venture_widget_builtin_kinds); i++)
		{
			g_autoptr(GError) local_error = NULL;

			if (!venture_widget_kind_registry_add(instance,
				&venture_widget_builtin_kinds[i], NULL, NULL,
				&local_error))
				g_error("Cannot register built-in widget kind: %s",
				        local_error->message);
		}

		g_once_init_leave(&once, 1);
	}

	return instance;
}

const VentureWidgetKindInfo *
venture_widget_kind_registry_lookup(
	VentureWidgetKindRegistry	*self,
	const gchar			*name
){
	VentureWidgetKindEntry *entry;

	g_return_val_if_fail(VENTURE_IS_WIDGET_KIND_REGISTRY(self), NULL);

	if (NULL == name)
		return NULL;

	entry = g_hash_table_lookup(self->kinds, name);

	return (NULL != entry) ? entry->info : NULL;
}

gchar **
venture_widget_kind_registry_list_names(VentureWidgetKindRegistry *self)
{
	GPtrArray *names;
	guint i;

	g_return_val_if_fail(VENTURE_IS_WIDGET_KIND_REGISTRY(self), NULL);

	names = g_ptr_array_new_with_free_func(g_free);

	for (i = 0; i < self->order->len; i++)
		g_ptr_array_add(names, g_strdup(g_ptr_array_index(self->order, i)));

	g_ptr_array_add(names, NULL);

	return (gchar **)g_ptr_array_free(names, FALSE);
}

JsonNode *
venture_widget_kind_registry_describe(
	VentureWidgetKindRegistry	*self,
	VentureContext			*context
){
	g_autoptr(JsonBuilder) builder = NULL;
	guint i;

	g_return_val_if_fail(VENTURE_IS_WIDGET_KIND_REGISTRY(self), NULL);

	builder = json_builder_new();
	json_builder_begin_array(builder);

	for (i = 0; i < self->order->len; i++)
	{
		const VentureWidgetKindInfo *info;
		gboolean enabled;
		gsize u;

		info = venture_widget_kind_registry_lookup(self,
			g_ptr_array_index(self->order, i));
		enabled = (NULL == info->module) || (NULL == context) ||
		          venture_context_module_enabled(context, info->module);

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "name");
		json_builder_add_string_value(builder, info->name);
		json_builder_set_member_name(builder, "label");
		json_builder_add_string_value(builder, info->label);
		json_builder_set_member_name(builder, "description");
		json_builder_add_string_value(builder,
			(NULL != info->description) ? info->description : "");
		json_builder_set_member_name(builder, "module");

		if (NULL != info->module)
			json_builder_add_string_value(builder, info->module);
		else
			json_builder_add_null_value(builder);

		json_builder_set_member_name(builder, "enabled");
		json_builder_add_boolean_value(builder, enabled);
		json_builder_set_member_name(builder, "uses");
		json_builder_begin_array(builder);

		for (u = 0; (NULL != info->uses) && (NULL != info->uses[u]); u++)
			json_builder_add_string_value(builder, info->uses[u]);

		json_builder_end_array(builder);
		json_builder_end_object(builder);
	}

	json_builder_end_array(builder);

	return json_builder_get_root(builder);
}

/* ==========================================================================
 * Rendering
 * ========================================================================== */

gchar *
venture_dashboard_widget_get_default_title(
	VentureContext		*context,
	VentureDashboardWidget	*widget
){
	g_autofree gchar *kind_name = NULL;
	g_autofree gchar *entity_type = NULL;
	g_autofree gchar *report_name = NULL;
	const VentureWidgetKindInfo *info;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	g_return_val_if_fail(VENTURE_IS_DASHBOARD_WIDGET(widget), NULL);

	kind_name = venture_widget_get_string(widget, "kind");
	info = venture_widget_kind_registry_lookup(
		venture_widget_kind_registry_get_default(), kind_name);
	entity_type = venture_widget_get_string(widget, "entity-type");
	report_name = venture_widget_get_string(widget, "report-name");

	if (NULL != report_name)
	{
		VentureReport *report;

		report = venture_report_registry_lookup(
			venture_context_get_report_registry(context), report_name);

		if (NULL != report)
			return g_strdup(venture_report_get_title(report));

		return venture_widget_humanise(report_name);
	}

	if (NULL != entity_type)
		return venture_widget_type_label(context, entity_type, TRUE);

	if (NULL != info)
		return g_strdup(info->label);

	return venture_widget_humanise(
		(NULL != kind_name) ? kind_name : "widget");
}

VentureWidgetResult *
venture_dashboard_render_widget(
	VentureContext			*context,
	VentureDashboardWidget		*widget,
	const VentureWidgetScope	*scope
){
	static const VentureWidgetScope nobody = { NULL, 0, 0, NULL, 0 };
	g_autoptr(GError) error = NULL;
	g_autofree gchar *kind_name = NULL;
	g_autofree gchar *title = NULL;
	VentureWidgetKindEntry *entry;
	VentureWidgetKindRegistry *registry;
	VentureWidgetResult *result;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	g_return_val_if_fail(VENTURE_IS_DASHBOARD_WIDGET(widget), NULL);

	if (NULL == scope)
		scope = &nobody;

	title = venture_widget_get_string(widget, "title");
	kind_name = venture_widget_get_string(widget, "kind");
	registry = venture_widget_kind_registry_get_default();
	entry = g_hash_table_lookup(registry->kinds,
	                            (NULL != kind_name) ? kind_name : "");

	if (NULL == entry)
	{
		g_autofree gchar *message = NULL;

		message = g_strdup_printf("There is no widget kind called \"%s\"",
		                          (NULL != kind_name) ? kind_name : "");
		return venture_widget_result_new_error(
			(NULL != title) ? title : "Unknown widget", message);
	}

	if ((NULL != entry->info->module) &&
	    !venture_context_module_enabled(context, entry->info->module))
	{
		g_autofree gchar *message = NULL;

		message = g_strdup_printf("This widget belongs to the %s module, "
		                          "which is off (modules.%s.enabled)",
		                          entry->info->module, entry->info->module);
		return venture_widget_result_new_error(
			(NULL != title) ? title : entry->info->label, message);
	}

	result = entry->info->func(context, widget, scope, entry->user_data,
	                           &error);

	if (NULL == result)
	{
		return venture_widget_result_new_error(
			(NULL != title) ? title : entry->info->label,
			(NULL != error) ? error->message : "The widget failed");
	}

	/* The widget's own title wins over the kind's. */
	if (NULL != title)
	{
		g_free(result->title);
		result->title = g_steal_pointer(&title);
	}
	else if (NULL == result->title)
	{
		result->title = g_strdup(entry->info->label);
	}

	if (NULL == result->data)
		result->data = json_node_new(JSON_NODE_NULL);

	return result;
}

/* ==========================================================================
 * Dashboards
 * ========================================================================== */

VentureDashboard *
venture_dashboard_find_by_slug(
	VentureDatabase	 *database,
	const gchar	 *slug,
	GError		**error
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) found = NULL;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);

	if (venture_string_is_empty(slug))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		                    "No dashboard named");
		return NULL;
	}

	query = venture_query_new(VENTURE_TYPE_DASHBOARD);

	if (!venture_query_add_filter_string(query, "slug", VENTURE_FILTER_OP_EQ,
	                                     slug, error))
		return NULL;

	venture_query_set_limit(query, 1);
	found = venture_database_find_one(database, query, error);

	if (NULL == found)
	{
		if ((NULL != error) && (NULL == *error))
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
			            "There is no dashboard called \"%s\"", slug);

		return NULL;
	}

	return VENTURE_DASHBOARD(g_steal_pointer(&found));
}

gboolean
venture_dashboard_is_visible_to(
	VentureDashboard	*dashboard,
	gint64			 user_id
){
	gboolean personal = FALSE;
	gint64 owner = 0;

	g_return_val_if_fail(VENTURE_IS_DASHBOARD(dashboard), FALSE);

	g_object_get(dashboard, "personal", &personal, "owner-user-id", &owner,
	             NULL);

	if (!personal)
		return TRUE;

	/* A personal dashboard with no owner is nobody's: an import with no
	 * user behind it. Shown, because hiding it from everyone would make
	 * it impossible to fix. */
	if (0 == owner)
		return TRUE;

	return owner == user_id;
}

VentureDashboard *
venture_dashboard_find_home(
	VentureDatabase	*database,
	gint64		 user_id
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) homes = NULL;
	guint i;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);

	query = venture_query_new(VENTURE_TYPE_DASHBOARD);
	venture_query_add_filter_string(query, "home", VENTURE_FILTER_OP_EQ,
	                                "true", NULL);
	venture_query_add_order(query, "position", VENTURE_SORT_ASCENDING, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	venture_query_set_limit(query, 10);

	homes = venture_database_find(database, query, NULL);

	for (i = 0; (NULL != homes) && (i < homes->len); i++)
	{
		VentureDashboard *dashboard;

		dashboard = g_ptr_array_index(homes, i);

		if (venture_dashboard_is_visible_to(dashboard, user_id))
			return g_object_ref(dashboard);
	}

	return NULL;
}

static gint
venture_dashboard_compare(
	gconstpointer	a,
	gconstpointer	b
){
	VentureDashboard *left;
	VentureDashboard *right;
	g_autofree gchar *left_name = NULL;
	g_autofree gchar *right_name = NULL;
	VentureDashboardPurpose left_purpose;
	VentureDashboardPurpose right_purpose;
	gint64 left_position;
	gint64 right_position;

	left = *(VentureDashboard *const *)a;
	right = *(VentureDashboard *const *)b;

	g_object_get(left, "purpose", &left_purpose, "position", &left_position,
	             "name", &left_name, NULL);
	g_object_get(right, "purpose", &right_purpose, "position", &right_position,
	             "name", &right_name, NULL);

	if (left_purpose != right_purpose)
		return (left_purpose < right_purpose) ? -1 : 1;

	if (left_position != right_position)
		return (left_position < right_position) ? -1 : 1;

	return g_strcmp0(left_name, right_name);
}

GPtrArray *
venture_dashboard_list_visible(
	VentureDatabase	 *database,
	gint64		  user_id,
	GError		**error
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) all = NULL;
	GPtrArray *visible;
	guint i;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);

	query = venture_query_new(VENTURE_TYPE_DASHBOARD);
	venture_query_set_limit(query, 500);
	all = venture_database_find(database, query, error);

	if (NULL == all)
		return NULL;

	visible = g_ptr_array_new_with_free_func(g_object_unref);

	for (i = 0; i < all->len; i++)
	{
		VentureDashboard *dashboard;

		dashboard = g_ptr_array_index(all, i);

		if (venture_dashboard_is_visible_to(dashboard, user_id))
			g_ptr_array_add(visible, g_object_ref(dashboard));
	}

	g_ptr_array_sort(visible, venture_dashboard_compare);

	return visible;
}

GPtrArray *
venture_dashboard_list_widgets(
	VentureDatabase	 *database,
	gint64		  dashboard_id,
	GError		**error
){
	g_autoptr(VentureQuery) query = NULL;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);

	query = venture_query_new(VENTURE_TYPE_DASHBOARD_WIDGET);

	if (!venture_query_add_filter_int(query, "dashboard-id",
	                                  VENTURE_FILTER_OP_EQ, dashboard_id,
	                                  error))
		return NULL;

	venture_query_add_order(query, "position", VENTURE_SORT_ASCENDING, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	venture_query_set_limit(query, 200);

	return venture_database_find(database, query, error);
}

/* ==========================================================================
 * The grid
 * ========================================================================== */

#define VENTURE_GRID_MAX_HEIGHT (8)
#define VENTURE_GRID_MAX_ROWS   (400)

void
venture_widget_placement_free(VentureWidgetPlacement *placement)
{
	if (NULL == placement)
		return;

	g_clear_object(&placement->widget);
	g_free(placement);
}

/*
 * A widget's wanted size, from its grid width or, failing that, its span:
 * "wide" is two columns and "full" is every column, on whatever layout the
 * dashboard has today. Always at least one cell and never wider than the
 * grid, so a layout that lost a column still shows every widget.
 */
static void
venture_grid_wanted_size(
	VentureDashboardWidget	*widget,
	guint			 columns,
	guint			*out_width,
	guint			*out_height
){
	VentureWidgetSpan span;
	gint64 width = 0;
	gint64 height = 0;

	g_object_get(widget, "span", &span, "grid-width", &width,
	             "grid-height", &height, NULL);

	if (width <= 0)
	{
		switch (span)
		{
		case VENTURE_WIDGET_SPAN_WIDE: width = 2; break;
		case VENTURE_WIDGET_SPAN_FULL: width = columns; break;
		case VENTURE_WIDGET_SPAN_NORMAL:
		default: width = 1; break;
		}
	}

	*out_width = (guint)CLAMP(width, 1, (gint64)columns);
	*out_height = (guint)CLAMP(height, 1, VENTURE_GRID_MAX_HEIGHT);
}

/*
 * The occupancy map: one byte per cell, rows growing as needed.
 */
typedef struct
{
	GArray	*cells;		/* guint8, row-major */
	guint	 columns;
	guint	 rows;
} VentureGrid;

static void
venture_grid_init(
	VentureGrid	*grid,
	guint		 columns
){
	grid->cells = g_array_new(FALSE, TRUE, sizeof(guint8));
	grid->columns = MAX(columns, 1);
	grid->rows = 0;
}

static void
venture_grid_clear(VentureGrid *grid)
{
	g_clear_pointer(&grid->cells, g_array_unref);
}

static void
venture_grid_ensure_rows(
	VentureGrid	*grid,
	guint		 rows
){
	if (rows <= grid->rows)
		return;

	g_array_set_size(grid->cells, rows * grid->columns);
	grid->rows = rows;
}

static gboolean
venture_grid_is_free(
	VentureGrid	*grid,
	guint		 col,
	guint		 row,
	guint		 width,
	guint		 height
){
	guint r;
	guint c;

	if ((col < 1) || (row < 1) || (col + width - 1 > grid->columns))
		return FALSE;

	for (r = row; r < row + height; r++)
	{
		if (r > grid->rows)
			continue;

		for (c = col; c < col + width; c++)
		{
			if (0 != g_array_index(grid->cells, guint8,
			                       (r - 1) * grid->columns + (c - 1)))
				return FALSE;
		}
	}

	return TRUE;
}

static void
venture_grid_take(
	VentureGrid	*grid,
	guint		 col,
	guint		 row,
	guint		 width,
	guint		 height
){
	guint r;
	guint c;

	venture_grid_ensure_rows(grid, row + height - 1);

	for (r = row; r < row + height; r++)
		for (c = col; c < col + width; c++)
			g_array_index(grid->cells, guint8,
			              (r - 1) * grid->columns + (c - 1)) = 1;
}

/*
 * The first free spot in reading order for a block of this size.
 */
static void
venture_grid_find_free(
	VentureGrid	*grid,
	guint		 width,
	guint		 height,
	guint		*out_col,
	guint		*out_row
){
	guint row;

	for (row = 1; row < VENTURE_GRID_MAX_ROWS; row++)
	{
		guint col;

		for (col = 1; col + width - 1 <= grid->columns; col++)
		{
			if (venture_grid_is_free(grid, col, row, width, height))
			{
				*out_col = col;
				*out_row = row;
				return;
			}
		}
	}

	/* Four hundred rows of widgets is not a dashboard; put it at the
	 * end rather than loop for ever. */
	*out_col = 1;
	*out_row = grid->rows + 1;
}

static gint
venture_placement_compare(
	gconstpointer	a,
	gconstpointer	b
){
	const VentureWidgetPlacement *left;
	const VentureWidgetPlacement *right;

	left = *(const VentureWidgetPlacement *const *)a;
	right = *(const VentureWidgetPlacement *const *)b;

	if (left->row != right->row)
		return (left->row < right->row) ? -1 : 1;

	if (left->col != right->col)
		return (left->col < right->col) ? -1 : 1;

	return 0;
}

/*
 * Lays the widgets out, with @skip left off the grid -- for asking where
 * a widget could go without it standing in its own way.
 */
static GPtrArray *
venture_dashboard_layout_without(
	VentureDatabase		 *database,
	VentureDashboard	 *dashboard,
	gint64			  skip_id,
	guint			 *out_columns,
	GError			**error
){
	g_autoptr(GPtrArray) widgets = NULL;
	g_autoptr(GPtrArray) placements = NULL;
	VentureDashboardLayout layout;
	VentureGrid grid;
	guint columns;
	guint i;

	widgets = venture_dashboard_list_widgets(database,
		venture_entity_get_id(VENTURE_ENTITY(dashboard)), error);

	if (NULL == widgets)
		return NULL;

	g_object_get(dashboard, "layout", &layout, NULL);
	columns = venture_dashboard_layout_get_columns(layout);

	if (NULL != out_columns)
		*out_columns = columns;

	placements = g_ptr_array_new_with_free_func(
		(GDestroyNotify)venture_widget_placement_free);
	venture_grid_init(&grid, columns);

	/*
	 * Two passes. The placed widgets claim their cells first, in page
	 * order, so that when two ask for the same spot the earlier one keeps
	 * it; then everything else flows into what is left. Doing both in one
	 * pass would let an unplaced early widget take the cell a placed late
	 * one asked for, which is the wrong way round: an explicit placement
	 * is a decision, a flow is a default.
	 */
	for (i = 0; i < widgets->len; i++)
	{
		VentureDashboardWidget *widget;
		VentureWidgetPlacement *placement;
		gint64 col = 0;
		gint64 row = 0;

		widget = g_ptr_array_index(widgets, i);

		if (venture_entity_get_id(VENTURE_ENTITY(widget)) == skip_id)
			continue;

		placement = g_new0(VentureWidgetPlacement, 1);
		placement->widget = g_object_ref(widget);
		venture_grid_wanted_size(widget, columns, &placement->width,
		                         &placement->height);
		g_object_get(widget, "grid-col", &col, "grid-row", &row, NULL);

		if ((col > 0) && (row > 0) && (row < VENTURE_GRID_MAX_ROWS) &&
		    venture_grid_is_free(&grid, (guint)col, (guint)row,
		                         placement->width, placement->height))
		{
			placement->col = (guint)col;
			placement->row = (guint)row;
			placement->placed = TRUE;
			venture_grid_take(&grid, placement->col, placement->row,
			                  placement->width, placement->height);
		}

		g_ptr_array_add(placements, placement);
	}

	for (i = 0; i < placements->len; i++)
	{
		VentureWidgetPlacement *placement;

		placement = g_ptr_array_index(placements, i);

		if (placement->placed)
			continue;

		venture_grid_find_free(&grid, placement->width, placement->height,
		                       &placement->col, &placement->row);
		venture_grid_take(&grid, placement->col, placement->row,
		                  placement->width, placement->height);
	}

	venture_grid_clear(&grid);
	g_ptr_array_sort(placements, venture_placement_compare);

	return g_steal_pointer(&placements);
}

GPtrArray *
venture_dashboard_layout(
	VentureDatabase		 *database,
	VentureDashboard	 *dashboard,
	GError			**error
){
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	g_return_val_if_fail(VENTURE_IS_DASHBOARD(dashboard), NULL);

	return venture_dashboard_layout_without(database, dashboard, 0, NULL,
	                                        error);
}

/*
 * Finds where a widget sits in a layout.
 */
static VentureWidgetPlacement *
venture_dashboard_find_placement(
	GPtrArray	*placements,
	gint64		 widget_id
){
	guint i;

	for (i = 0; i < placements->len; i++)
	{
		VentureWidgetPlacement *placement;

		placement = g_ptr_array_index(placements, i);

		if (venture_entity_get_id(VENTURE_ENTITY(placement->widget)) ==
		    widget_id)
			return placement;
	}

	return NULL;
}

gboolean
venture_dashboard_place_widget(
	VentureDatabase		 *database,
	VentureDashboard	 *dashboard,
	VentureDashboardWidget	 *widget,
	guint			  col,
	guint			  row,
	guint			  width,
	guint			  height,
	const VentureActor	 *actor,
	GError			**error
){
	g_autoptr(GPtrArray) others = NULL;
	guint columns;
	guint i;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), FALSE);
	g_return_val_if_fail(VENTURE_IS_DASHBOARD(dashboard), FALSE);
	g_return_val_if_fail(VENTURE_IS_DASHBOARD_WIDGET(widget), FALSE);

	others = venture_dashboard_layout_without(database, dashboard,
		venture_entity_get_id(VENTURE_ENTITY(widget)), &columns, error);

	if (NULL == others)
		return FALSE;

	if ((width < 1) || (width > columns))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "A widget spans 1 to %u columns on this layout", columns);
		return FALSE;
	}

	if ((height < 1) || (height > VENTURE_GRID_MAX_HEIGHT))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "A widget spans 1 to %d rows", VENTURE_GRID_MAX_HEIGHT);
		return FALSE;
	}

	/* Placed somewhere in particular: it has to fit, and the cells have
	 * to be free of everything else as it is laid out now. */
	if ((col > 0) || (row > 0))
	{
		if ((col < 1) || (row < 1) || (col + width - 1 > columns) ||
		    (row >= VENTURE_GRID_MAX_ROWS))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			            "Column %u, row %u with width %u does not fit a "
			            "%u-column grid", col, row, width, columns);
			return FALSE;
		}

		for (i = 0; i < others->len; i++)
		{
			VentureWidgetPlacement *other;

			other = g_ptr_array_index(others, i);

			/* Only a widget that asked for its cells stands in the
			 * way; one that merely flowed there will flow around this
			 * one once it is placed. */
			if (!other->placed)
				continue;

			if ((col < other->col + other->width) &&
			    (other->col < col + width) &&
			    (row < other->row + other->height) &&
			    (other->row < row + height))
			{
				g_autofree gchar *title = NULL;

				g_object_get(other->widget, "title", &title, NULL);
				g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
				            "That spot is taken by widget #%" G_GINT64_FORMAT
				            "%s%s (column %u, row %u)",
				            venture_entity_get_id(VENTURE_ENTITY(other->widget)),
				            venture_string_is_empty(title) ? "" : " ",
				            venture_string_is_empty(title) ? "" : title,
				            other->col, other->row);
				return FALSE;
			}
		}
	}

	g_object_set(widget,
	             "grid-col", (gint64)col,
	             "grid-row", (gint64)row,
	             "grid-width", (gint64)width,
	             "grid-height", (gint64)height,
	             NULL);

	return venture_database_save(database, VENTURE_ENTITY(widget), actor,
	                             error);
}

gboolean
venture_dashboard_swap_widgets(
	VentureDatabase		 *database,
	VentureDashboard	 *dashboard,
	VentureDashboardWidget	 *first,
	VentureDashboardWidget	 *second,
	const VentureActor	 *actor,
	GError			**error
){
	g_autoptr(GPtrArray) placements = NULL;
	VentureWidgetPlacement *one = NULL;
	VentureWidgetPlacement *two = NULL;
	gint64 first_id;
	gint64 second_id;
	guint i;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), FALSE);
	g_return_val_if_fail(VENTURE_IS_DASHBOARD(dashboard), FALSE);
	g_return_val_if_fail(VENTURE_IS_DASHBOARD_WIDGET(first), FALSE);
	g_return_val_if_fail(VENTURE_IS_DASHBOARD_WIDGET(second), FALSE);

	first_id = venture_entity_get_id(VENTURE_ENTITY(first));
	second_id = venture_entity_get_id(VENTURE_ENTITY(second));

	if (first_id == second_id)
	{
		g_set_error_literal(error, VENTURE_ERROR,
		                    VENTURE_ERROR_INVALID_ARGUMENT,
		                    "A widget cannot swap with itself");
		return FALSE;
	}

	/*
	 * Resolved rather than read off the rows: a widget that has never
	 * been placed has zeroes stored and a real position on the page, and
	 * swapping with its stored zeroes would move it to the top left
	 * rather than to where the other card is.
	 */
	placements = venture_dashboard_layout(database, dashboard, error);

	if (NULL == placements)
		return FALSE;

	for (i = 0; i < placements->len; i++)
	{
		VentureWidgetPlacement *placement;
		gint64 id;

		placement = g_ptr_array_index(placements, i);
		id = venture_entity_get_id(VENTURE_ENTITY(placement->widget));

		if (id == first_id)
			one = placement;
		else if (id == second_id)
			two = placement;
	}

	if ((NULL == one) || (NULL == two))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		                    "Both widgets have to be on this dashboard");
		return FALSE;
	}

	if ((one->width != two->width) || (one->height != two->height))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
		            "Only two widgets of the same size can trade places; "
		            "these are %ux%u and %ux%u. Move the other one out of "
		            "the way first.",
		            one->width, one->height, two->width, two->height);
		return FALSE;
	}

	if (!venture_database_begin(database, error))
		return FALSE;

	/*
	 * Both written before either is checked against the other, which is
	 * the whole reason this is one transaction: saving the first alone
	 * would land it on cells the second still holds, and the validator
	 * has no way to know a second save is coming.
	 */
	g_object_set(first,
	             "grid-col", (gint64)two->col,
	             "grid-row", (gint64)two->row,
	             "grid-width", (gint64)two->width,
	             "grid-height", (gint64)two->height,
	             NULL);
	g_object_set(second,
	             "grid-col", (gint64)one->col,
	             "grid-row", (gint64)one->row,
	             "grid-width", (gint64)one->width,
	             "grid-height", (gint64)one->height,
	             NULL);

	if (!venture_database_save(database, VENTURE_ENTITY(first), actor, error) ||
	    !venture_database_save(database, VENTURE_ENTITY(second), actor, error))
	{
		venture_database_rollback(database);
		return FALSE;
	}

	return venture_database_commit(database, error);
}

gboolean
venture_dashboard_nudge_widget(
	VentureDatabase		 *database,
	VentureDashboard	 *dashboard,
	VentureDashboardWidget	 *widget,
	const gchar		 *direction,
	const VentureActor	 *actor,
	GError			**error
){
	g_autoptr(GPtrArray) placements = NULL;
	VentureWidgetPlacement *here;
	guint col;
	guint row;
	guint width;
	guint height;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), FALSE);
	g_return_val_if_fail(VENTURE_IS_DASHBOARD(dashboard), FALSE);
	g_return_val_if_fail(VENTURE_IS_DASHBOARD_WIDGET(widget), FALSE);

	placements = venture_dashboard_layout(database, dashboard, error);

	if (NULL == placements)
		return FALSE;

	here = venture_dashboard_find_placement(placements,
		venture_entity_get_id(VENTURE_ENTITY(widget)));

	if (NULL == here)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		                    "The widget is not on its dashboard");
		return FALSE;
	}

	col = here->col;
	row = here->row;
	width = here->width;
	height = here->height;

	if (0 == g_strcmp0(direction, "up"))
		row = (row > 1) ? row - 1 : row;
	else if (0 == g_strcmp0(direction, "down"))
		row = row + 1;
	else if (0 == g_strcmp0(direction, "left"))
		col = (col > 1) ? col - 1 : col;
	else if (0 == g_strcmp0(direction, "right"))
		col = col + 1;
	else if (0 == g_strcmp0(direction, "wider"))
		width = width + 1;
	else if (0 == g_strcmp0(direction, "narrower"))
		width = (width > 1) ? width - 1 : width;
	else if (0 == g_strcmp0(direction, "taller"))
		height = height + 1;
	else if (0 == g_strcmp0(direction, "shorter"))
		height = (height > 1) ? height - 1 : height;
	else
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "\"%s\" is not a direction: up, down, left, right, "
		            "wider, narrower, taller or shorter",
		            (NULL != direction) ? direction : "");
		return FALSE;
	}

	/* Nothing to do is not an error: the button at the edge is inert. */
	if ((col == here->col) && (row == here->row) && (width == here->width) &&
	    (height == here->height))
		return TRUE;

	return venture_dashboard_place_widget(database, dashboard, widget, col,
	                                      row, width, height, actor, error);
}

gboolean
venture_dashboard_arrange(
	VentureDatabase		 *database,
	VentureDashboard	 *dashboard,
	const VentureActor	 *actor,
	GError			**error
){
	g_autoptr(GPtrArray) widgets = NULL;
	VentureDashboardLayout layout;
	VentureGrid grid;
	guint columns;
	guint i;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), FALSE);
	g_return_val_if_fail(VENTURE_IS_DASHBOARD(dashboard), FALSE);

	widgets = venture_dashboard_list_widgets(database,
		venture_entity_get_id(VENTURE_ENTITY(dashboard)), error);

	if (NULL == widgets)
		return FALSE;

	g_object_get(dashboard, "layout", &layout, NULL);
	columns = venture_dashboard_layout_get_columns(layout);
	venture_grid_init(&grid, columns);

	/* Page order, first free cell each: the flow with every placement
	 * forgotten, then written down so it stays. */
	for (i = 0; i < widgets->len; i++)
	{
		VentureDashboardWidget *widget;
		gint64 old_col = 0;
		gint64 old_row = 0;
		gint64 old_width = 0;
		gint64 old_height = 0;
		guint width;
		guint height;
		guint col;
		guint row;

		widget = g_ptr_array_index(widgets, i);
		venture_grid_wanted_size(widget, columns, &width, &height);
		venture_grid_find_free(&grid, width, height, &col, &row);
		venture_grid_take(&grid, col, row, width, height);

		g_object_get(widget, "grid-col", &old_col, "grid-row", &old_row,
		             "grid-width", &old_width, "grid-height", &old_height,
		             NULL);

		if ((old_col == (gint64)col) && (old_row == (gint64)row) &&
		    (old_width == (gint64)width) && (old_height == (gint64)height))
			continue;

		g_object_set(widget,
		             "grid-col", (gint64)col, "grid-row", (gint64)row,
		             "grid-width", (gint64)width, "grid-height", (gint64)height,
		             NULL);

		if (!venture_database_save(database, VENTURE_ENTITY(widget), actor,
		                           error))
		{
			venture_grid_clear(&grid);
			return FALSE;
		}
	}

	venture_grid_clear(&grid);

	return TRUE;
}

gboolean
venture_dashboard_move_widget(
	VentureDatabase		 *database,
	VentureDashboardWidget	 *widget,
	gint			  direction,
	const VentureActor	 *actor,
	GError			**error
){
	g_autoptr(GPtrArray) widgets = NULL;
	gint64 dashboard_id = 0;
	guint index;
	guint i;
	gboolean found;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), FALSE);
	g_return_val_if_fail(VENTURE_IS_DASHBOARD_WIDGET(widget), FALSE);

	if (0 == direction)
		return TRUE;

	g_object_get(widget, "dashboard-id", &dashboard_id, NULL);
	widgets = venture_dashboard_list_widgets(database, dashboard_id, error);

	if (NULL == widgets)
		return FALSE;

	found = FALSE;
	index = 0;

	for (i = 0; i < widgets->len; i++)
	{
		if (venture_entity_get_id(g_ptr_array_index(widgets, i)) ==
		    venture_entity_get_id(VENTURE_ENTITY(widget)))
		{
			found = TRUE;
			index = i;
			break;
		}
	}

	if (!found)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		                    "The widget is not on its dashboard");
		return FALSE;
	}

	if (((direction < 0) && (0 == index)) ||
	    ((direction > 0) && (index + 1 >= widgets->len)))
		return TRUE;

	/*
	 * Normalise first. Positions default to zero, and swapping two zeros
	 * changes nothing; tens leave room for an insert between two without
	 * renumbering. Only rows whose position changes are written.
	 */
	{
		gint64 expected;

		expected = 10;

		for (i = 0; i < widgets->len; i++)
		{
			VentureEntity *row;
			gint64 position = 0;

			row = g_ptr_array_index(widgets, i);
			g_object_get(row, "position", &position, NULL);

			if (position != expected)
			{
				g_object_set(row, "position", expected, NULL);

				if (!venture_database_save(database, row, actor, error))
					return FALSE;
			}

			expected += 10;
		}
	}

	{
		VentureEntity *here;
		VentureEntity *there;
		gint64 here_position = 0;
		gint64 there_position = 0;

		here = g_ptr_array_index(widgets, index);
		there = g_ptr_array_index(widgets,
			(direction < 0) ? index - 1 : index + 1);

		g_object_get(here, "position", &here_position, NULL);
		g_object_get(there, "position", &there_position, NULL);
		g_object_set(here, "position", there_position, NULL);
		g_object_set(there, "position", here_position, NULL);

		if (!venture_database_save(database, here, actor, error) ||
		    !venture_database_save(database, there, actor, error))
			return FALSE;
	}

	return TRUE;
}

/*
 * A widget's settings as JSON, in the wire spelling, without the spine.
 * Shared by describe and export.
 */
static void
venture_dashboard_widget_add_settings(
	JsonBuilder		*builder,
	VentureDashboardWidget	*widget,
	gboolean		 with_identity
){
	static const gchar *const strings[] = {
		"title", "kind", "entity-type", "report-name", "period", "filter",
		"order", "field", "columns", "body", "options", NULL
	};
	static const gchar *const integers[] = {
		"position", "record-id", "limit", "refresh-seconds",
		"grid-col", "grid-row", "grid-width", "grid-height", NULL
	};
	VentureWidgetSpan span;
	gsize i;

	if (with_identity)
	{
		json_builder_set_member_name(builder, "id");
		json_builder_add_int_value(builder,
			venture_entity_get_id(VENTURE_ENTITY(widget)));
	}

	for (i = 0; NULL != strings[i]; i++)
	{
		g_autofree gchar *value = NULL;
		g_autofree gchar *key = NULL;

		value = venture_widget_get_string(widget, strings[i]);
		key = venture_entity_property_to_column(strings[i]);

		json_builder_set_member_name(builder, key);

		if (NULL != value)
			json_builder_add_string_value(builder, value);
		else
			json_builder_add_null_value(builder);
	}

	for (i = 0; NULL != integers[i]; i++)
	{
		g_autofree gchar *key = NULL;

		key = venture_entity_property_to_column(integers[i]);
		json_builder_set_member_name(builder, key);
		json_builder_add_int_value(builder,
			venture_widget_get_int(widget, integers[i]));
	}

	g_object_get(widget, "span", &span, NULL);
	json_builder_set_member_name(builder, "span");
	json_builder_add_string_value(builder,
		venture_enum_to_nick(VENTURE_TYPE_WIDGET_SPAN, (gint)span));
}

static void
venture_dashboard_add_settings(
	JsonBuilder		*builder,
	VentureDashboard	*dashboard,
	gboolean		 with_identity
){
	g_autofree gchar *name = NULL;
	g_autofree gchar *slug = NULL;
	g_autofree gchar *description = NULL;
	VentureDashboardPurpose purpose;
	VentureDashboardLayout layout;
	gboolean home = FALSE;
	gboolean personal = FALSE;
	gint64 owner = 0;
	gint64 venture_id = 0;
	gint64 position = 0;

	g_object_get(dashboard,
	             "name", &name, "slug", &slug, "description", &description,
	             "purpose", &purpose, "layout", &layout, "home", &home,
	             "personal", &personal, "owner-user-id", &owner,
	             "venture-id", &venture_id, "position", &position,
	             NULL);

	if (with_identity)
	{
		json_builder_set_member_name(builder, "id");
		json_builder_add_int_value(builder,
			venture_entity_get_id(VENTURE_ENTITY(dashboard)));
	}

	json_builder_set_member_name(builder, "name");
	json_builder_add_string_value(builder, (NULL != name) ? name : "");
	json_builder_set_member_name(builder, "slug");
	json_builder_add_string_value(builder, (NULL != slug) ? slug : "");
	json_builder_set_member_name(builder, "description");
	json_builder_add_string_value(builder,
		(NULL != description) ? description : "");
	json_builder_set_member_name(builder, "purpose");
	json_builder_add_string_value(builder,
		venture_enum_to_nick(VENTURE_TYPE_DASHBOARD_PURPOSE, (gint)purpose));
	json_builder_set_member_name(builder, "layout");
	json_builder_add_string_value(builder,
		venture_enum_to_nick(VENTURE_TYPE_DASHBOARD_LAYOUT, (gint)layout));
	json_builder_set_member_name(builder, "home");
	json_builder_add_boolean_value(builder, home);
	json_builder_set_member_name(builder, "personal");
	json_builder_add_boolean_value(builder, personal);
	json_builder_set_member_name(builder, "position");
	json_builder_add_int_value(builder, position);

	if (with_identity)
	{
		json_builder_set_member_name(builder, "owner_user_id");
		json_builder_add_int_value(builder, owner);
		json_builder_set_member_name(builder, "venture_id");
		json_builder_add_int_value(builder, venture_id);
	}
}

JsonNode *
venture_dashboard_describe(
	VentureContext			 *context,
	VentureDashboard		 *dashboard,
	const VentureWidgetScope	 *scope,
	gboolean			  with_data,
	GError				**error
){
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(GPtrArray) widgets = NULL;
	guint i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	g_return_val_if_fail(VENTURE_IS_DASHBOARD(dashboard), NULL);

	widgets = venture_dashboard_list_widgets(
		venture_context_get_database(context),
		venture_entity_get_id(VENTURE_ENTITY(dashboard)), error);

	if (NULL == widgets)
		return NULL;

	builder = json_builder_new();
	json_builder_begin_object(builder);
	venture_dashboard_add_settings(builder, dashboard, TRUE);
	json_builder_set_member_name(builder, "path");
	{
		g_autofree gchar *slug = NULL;
		g_autofree gchar *path = NULL;

		g_object_get(dashboard, "slug", &slug, NULL);
		path = g_strdup_printf("/dashboards/%s", (NULL != slug) ? slug : "");
		json_builder_add_string_value(builder, path);
	}
	json_builder_set_member_name(builder, "widgets");
	json_builder_begin_array(builder);

	for (i = 0; i < widgets->len; i++)
	{
		VentureDashboardWidget *widget;

		widget = g_ptr_array_index(widgets, i);

		json_builder_begin_object(builder);
		venture_dashboard_widget_add_settings(builder, widget, TRUE);

		if (with_data)
		{
			g_autoptr(VentureWidgetResult) result = NULL;

			result = venture_dashboard_render_widget(context, widget, scope);

			json_builder_set_member_name(builder, "resolved_title");
			json_builder_add_string_value(builder, result->title);
			json_builder_set_member_name(builder, "link");

			if (NULL != result->link)
				json_builder_add_string_value(builder, result->link);
			else
				json_builder_add_null_value(builder);

			json_builder_set_member_name(builder, "error");

			if (NULL != result->error)
				json_builder_add_string_value(builder, result->error);
			else
				json_builder_add_null_value(builder);

			json_builder_set_member_name(builder, "data");
			json_builder_add_value(builder, json_node_ref(result->data));
		}

		json_builder_end_object(builder);
	}

	json_builder_end_array(builder);
	json_builder_end_object(builder);

	return json_builder_get_root(builder);
}

JsonNode *
venture_dashboard_export(
	VentureDatabase	 *database,
	VentureDashboard *dashboard,
	GError		**error
){
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(GPtrArray) widgets = NULL;
	guint i;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	g_return_val_if_fail(VENTURE_IS_DASHBOARD(dashboard), NULL);

	widgets = venture_dashboard_list_widgets(database,
		venture_entity_get_id(VENTURE_ENTITY(dashboard)), error);

	if (NULL == widgets)
		return NULL;

	builder = json_builder_new();
	json_builder_begin_object(builder);
	venture_dashboard_add_settings(builder, dashboard, FALSE);
	json_builder_set_member_name(builder, "widgets");
	json_builder_begin_array(builder);

	for (i = 0; i < widgets->len; i++)
	{
		json_builder_begin_object(builder);
		venture_dashboard_widget_add_settings(builder,
			g_ptr_array_index(widgets, i), FALSE);
		json_builder_end_object(builder);
	}

	json_builder_end_array(builder);
	json_builder_end_object(builder);

	return json_builder_get_root(builder);
}

/*
 * A slug nobody has. "factory" taken becomes "factory-2", then "factory-3".
 */
static gchar *
venture_dashboard_free_slug(
	VentureDatabase	*database,
	const gchar	*wanted
){
	guint n;

	for (n = 1; n < 1000; n++)
	{
		g_autofree gchar *candidate = NULL;
		g_autoptr(VentureDashboard) taken = NULL;

		candidate = (1 == n) ? g_strdup(wanted)
		                     : g_strdup_printf("%s-%u", wanted, n);
		taken = venture_dashboard_find_by_slug(database, candidate, NULL);

		if (NULL == taken)
			return g_steal_pointer(&candidate);
	}

	return g_strdup_printf("%s-%" G_GINT64_FORMAT, wanted,
	                       g_get_real_time());
}

/*
 * Copies a definition's members onto a record by field name, through the
 * same string setter the forms use, so a definition is checked the way a
 * form is. Members the type does not have are refused: a misspelt setting
 * in a template would otherwise silently do nothing.
 */
static gboolean
venture_dashboard_apply_definition(
	VentureEntity		 *record,
	JsonObject		 *object,
	const gchar *const	 *skip,
	GError			**error
){
	g_autoptr(GList) members = NULL;
	GList *cursor;

	members = json_object_get_members(object);

	for (cursor = members; NULL != cursor; cursor = cursor->next)
	{
		const gchar *member;
		g_autofree gchar *property = NULL;
		g_autofree gchar *text = NULL;
		JsonNode *value;

		member = cursor->data;

		if ((NULL != skip) && g_strv_contains(skip, member))
			continue;

		property = venture_entity_column_to_property(member);

		if (NULL == g_object_class_find_property(G_OBJECT_GET_CLASS(record),
		                                         property))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			            "%s has no setting called \"%s\"",
			            venture_entity_get_entity_name(record), member);
			return FALSE;
		}

		value = json_object_get_member(object, member);

		if (JSON_NODE_HOLDS_NULL(value))
			continue;

		text = venture_widget_node_to_text(value);

		if (!venture_entity_set_field_from_string(record, property, text,
		                                          error))
			return FALSE;
	}

	return TRUE;
}

VentureDashboard *
venture_dashboard_import(
	VentureContext		 *context,
	JsonNode		 *definition,
	gint64			  owner_user_id,
	const VentureActor	 *actor,
	GError			**error
){
	static const gchar *const skip_dashboard[] = {
		"id", "widgets", "path", "owner_user_id", "slug", NULL
	};
	static const gchar *const skip_widget[] = {
		"id", "dashboard_id", "resolved_title", "link", "error", "data", NULL
	};
	g_autoptr(VentureDashboard) dashboard = NULL;
	g_autoptr(GPtrArray) widgets = NULL;
	VentureDatabase *database;
	JsonObject *object;
	JsonArray *array;
	guint i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	g_return_val_if_fail(NULL != definition, NULL);

	if (!JSON_NODE_HOLDS_OBJECT(definition))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "A dashboard definition is a JSON object with "
		                    "a name and a list of widgets");
		return NULL;
	}

	database = venture_context_get_database(context);
	object = json_node_get_object(definition);
	dashboard = venture_dashboard_new();

	if (!venture_dashboard_apply_definition(VENTURE_ENTITY(dashboard), object,
	                                        skip_dashboard, error))
		return NULL;

	{
		g_autofree gchar *name = NULL;
		g_autofree gchar *wanted = NULL;
		g_autofree gchar *slug = NULL;
		const gchar *given;

		g_object_get(dashboard, "name", &name, NULL);

		if (venture_string_is_empty(name))
		{
			g_set_error_literal(error, VENTURE_ERROR,
			                    VENTURE_ERROR_VALIDATION,
			                    "A dashboard definition needs a name");
			return NULL;
		}

		given = venture_json_object_get_string(object, "slug", NULL);
		wanted = venture_slugify(
			!venture_string_is_empty(given) ? given : name);
		slug = venture_dashboard_free_slug(database, wanted);
		g_object_set(dashboard, "slug", slug, NULL);
	}

	if (0 != owner_user_id)
		g_object_set(dashboard, "owner-user-id", owner_user_id, NULL);

	venture_entity_set_organization_id(VENTURE_ENTITY(dashboard),
		venture_context_get_default_organization_id(context));

	/* The widgets are built and checked before anything is written, so
	 * a bad one leaves no dashboard behind. */
	widgets = g_ptr_array_new_with_free_func(g_object_unref);
	array = json_object_has_member(object, "widgets") &&
	        JSON_NODE_HOLDS_ARRAY(json_object_get_member(object, "widgets"))
		? json_object_get_array_member(object, "widgets") : NULL;

	for (i = 0; (NULL != array) && (i < json_array_get_length(array)); i++)
	{
		g_autoptr(VentureDashboardWidget) widget = NULL;
		JsonNode *element;
		gint64 position = 0;

		element = json_array_get_element(array, i);

		if (!JSON_NODE_HOLDS_OBJECT(element))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			            "Widget %u is not an object", i + 1);
			return NULL;
		}

		widget = venture_dashboard_widget_new();

		if (!venture_dashboard_apply_definition(VENTURE_ENTITY(widget),
			json_node_get_object(element), skip_widget, error))
		{
			g_prefix_error(error, "Widget %u: ", i + 1);
			return NULL;
		}

		/* Order in the file is order on the page unless the file says
		 * otherwise. */
		g_object_get(widget, "position", &position, NULL);

		if (0 == position)
			g_object_set(widget, "position", (gint64)((i + 1) * 10), NULL);

		venture_entity_set_organization_id(VENTURE_ENTITY(widget),
			venture_context_get_default_organization_id(context));
		g_ptr_array_add(widgets, g_steal_pointer(&widget));
	}

	if (!venture_database_begin(database, error))
		return NULL;

	if (!venture_database_save(database, VENTURE_ENTITY(dashboard), actor,
	                           error))
	{
		venture_database_rollback(database);
		return NULL;
	}

	for (i = 0; i < widgets->len; i++)
	{
		VentureEntity *widget;

		widget = g_ptr_array_index(widgets, i);
		g_object_set(widget, "dashboard-id",
		             venture_entity_get_id(VENTURE_ENTITY(dashboard)), NULL);

		if (!venture_database_save(database, widget, actor, error))
		{
			g_prefix_error(error, "Widget %u: ", i + 1);
			venture_database_rollback(database);
			return NULL;
		}
	}

	if (!venture_database_commit(database, error))
		return NULL;

	return g_steal_pointer(&dashboard);
}

/* ==========================================================================
 * Templates
 * ========================================================================== */

/*
 * Written in the export format, so a template is exactly what an export
 * of the dashboard it makes would be. A widget naming a type or a report
 * whose module is off still imports -- the validator accepts a type that
 * exists but is hidden -- and says so on the page, so a template built for
 * the factory is usable before the factory is turned on and complete
 * after.
 */
static const VentureDashboardTemplate venture_dashboard_templates[] = {
	{
		"factory", "Software factory",
		"The loop at a glance: what is planned, building, shipping and "
		"running, the incidents open, the runs in flight, and the "
		"knowledge and actions beside them.",
		"factory",
		"{"
		"\"name\": \"Factory\","
		"\"slug\": \"factory\","
		"\"description\": \"From a ticket to a running release, and back.\","
		"\"purpose\": \"work\","
		"\"layout\": \"three_columns\","
		"\"widgets\": ["
		" {\"kind\": \"count\", \"title\": \"Open incidents\","
		"  \"entity_type\": \"incident\","
		"  \"filter\": \"status__in=open,mitigated\"},"
		" {\"kind\": \"count\", \"title\": \"Builds failed, 7 days\","
		"  \"entity_type\": \"build\", \"filter\": \"status=failed\","
		"  \"period\": \"last_7_days\", \"field\": \"finished_at\"},"
		" {\"kind\": \"metric\", \"title\": \"Released this month\","
		"  \"report_name\": \"releases\", \"period\": \"this_month\","
		"  \"field\": \"releases\"},"
		" {\"kind\": \"milestone\", \"title\": \"Next milestone\"},"
		" {\"kind\": \"environments\"},"
		" {\"kind\": \"breakdown\", \"title\": \"Tickets by status\","
		"  \"entity_type\": \"ticket\", \"field\": \"status\","
		"  \"filter\": \"status__not_in=done,cancelled\"},"
		" {\"kind\": \"list\", \"title\": \"Latest builds\","
		"  \"entity_type\": \"build\", \"order\": \"-id\", \"limit\": 8,"
		"  \"columns\": \"status,ref,workflow\", \"span\": \"wide\"},"
		" {\"kind\": \"list\", \"title\": \"Recent releases\","
		"  \"entity_type\": \"release\", \"order\": \"-released_at\","
		"  \"limit\": 6, \"columns\": \"status,released_at\"},"
		" {\"kind\": \"list\", \"title\": \"In progress\","
		"  \"entity_type\": \"ticket\","
		"  \"filter\": \"status__in=in_progress,review,blocked\","
		"  \"order\": \"-updated_at\", \"limit\": 8,"
		"  \"columns\": \"status,assignee,milestone_id\", \"span\": \"wide\"},"
		" {\"kind\": \"list\", \"title\": \"Coding runs\","
		"  \"entity_type\": \"forge_run\", \"order\": \"-id\", \"limit\": 5,"
		"  \"columns\": \"state,ticket_id\"},"
		" {\"kind\": \"upcoming\", \"title\": \"Milestones due\","
		"  \"entity_type\": \"milestone\", \"field\": \"due_on\","
		"  \"filter\": \"status__not_in=completed,cancelled\","
		"  \"options\": \"{\\\"days\\\": 30}\"},"
		" {\"kind\": \"list\", \"title\": \"Recently written up\","
		"  \"entity_type\": \"kb_article\", \"order\": \"-updated_at\","
		"  \"limit\": 5, \"columns\": \"kb_id,status\"},"
		" {\"kind\": \"search\", \"title\": \"Search the knowledge bases\","
		"  \"entity_type\": \"kb_article\"},"
		" {\"kind\": \"actions\", \"title\": \"Do\","
		"  \"body\": \"New ticket | /e/ticket/new\\nNew release | "
		"/e/release/new\\nRecord an incident | /e/incident/new\\n"
		"Ticket board | /tickets\\nFactory page | /factory\\n"
		"Lead time | /reports/lead_time\\nReleases report | "
		"/reports/releases\"},"
		" {\"kind\": \"confirmations\"},"
		" {\"kind\": \"activity\", \"limit\": 10, \"span\": \"full\"}"
		"]}"
	},
	{
		"reporting", "Month end",
		"The figures: profit and loss, revenue by venture, receivables, "
		"the pipeline, and the trend.",
		"finance",
		"{"
		"\"name\": \"Month end\","
		"\"slug\": \"month-end\","
		"\"description\": \"How the month is going, across every venture.\","
		"\"purpose\": \"reporting\","
		"\"layout\": \"three_columns\","
		"\"widgets\": ["
		" {\"kind\": \"metric\", \"title\": \"Revenue\","
		"  \"report_name\": \"pnl\", \"field\": \"revenue\"},"
		" {\"kind\": \"metric\", \"title\": \"Expenses\","
		"  \"report_name\": \"pnl\", \"field\": \"expenses\"},"
		" {\"kind\": \"metric\", \"title\": \"Profit\","
		"  \"report_name\": \"pnl\", \"field\": \"profit\"},"
		" {\"kind\": \"chart\", \"title\": \"Revenue by venture\","
		"  \"report_name\": \"ventures\", \"columns\": \"venture\","
		"  \"field\": \"revenue\", \"span\": \"wide\"},"
		" {\"kind\": \"report\", \"title\": \"Receivables\","
		"  \"report_name\": \"receivables\","
		"  \"options\": \"{\\\"table\\\": false}\"},"
		" {\"kind\": \"chart\", \"title\": \"Profit by month\","
		"  \"report_name\": \"monthly\", \"period\": \"ytd\","
		"  \"columns\": \"month\", \"field\": \"profit\", \"limit\": 12,"
		"  \"span\": \"wide\"},"
		" {\"kind\": \"report\", \"title\": \"Pipeline\","
		"  \"report_name\": \"pipeline\"},"
		" {\"kind\": \"report\", \"title\": \"Profit and loss\","
		"  \"report_name\": \"pnl\", \"options\": \"{\\\"tiles\\\": false}\","
		"  \"span\": \"full\"},"
		" {\"kind\": \"list\", \"title\": \"Expenses awaiting review\","
		"  \"entity_type\": \"expense\","
		"  \"filter\": \"deductibility=review\", \"order\": \"-occurred_at\","
		"  \"limit\": 8, \"span\": \"wide\"},"
		" {\"kind\": \"actions\", \"title\": \"Exports\","
		"  \"body\": \"P&L as CSV | /api/v1/reports/pnl?format=csv\\n"
		"Tax summary | /reports/tax\\nAll reports | /reports\"}"
		"]}"
	},
	{
		"work", "My work",
		"What is on your plate: your tickets, what is due, what is "
		"waiting for a decision, and what just happened.",
		"tickets",
		"{"
		"\"name\": \"My work\","
		"\"slug\": \"my-work\","
		"\"description\": \"Everything with your name on it.\","
		"\"purpose\": \"work\","
		"\"layout\": \"three_columns\","
		"\"personal\": true,"
		"\"widgets\": ["
		" {\"kind\": \"count\", \"title\": \"Mine, open\","
		"  \"entity_type\": \"ticket\","
		"  \"filter\": \"assignee={me}&status__not_in=done,cancelled\"},"
		" {\"kind\": \"count\", \"title\": \"To triage\","
		"  \"entity_type\": \"ticket\", \"filter\": \"status=triage\"},"
		" {\"kind\": \"count\", \"title\": \"Blocked\","
		"  \"entity_type\": \"ticket\", \"filter\": \"status=blocked\"},"
		" {\"kind\": \"list\", \"title\": \"My tickets\","
		"  \"entity_type\": \"ticket\","
		"  \"filter\": \"assignee={me}&status__not_in=done,cancelled\","
		"  \"order\": \"-updated_at\", \"limit\": 10,"
		"  \"columns\": \"status,priority,due_at\", \"span\": \"wide\"},"
		" {\"kind\": \"upcoming\", \"title\": \"Due soon\","
		"  \"entity_type\": \"ticket\", \"field\": \"due_at\","
		"  \"filter\": \"status__not_in=done,cancelled\"},"
		" {\"kind\": \"confirmations\"},"
		" {\"kind\": \"breakdown\", \"title\": \"Everything by status\","
		"  \"entity_type\": \"ticket\", \"field\": \"status\"},"
		" {\"kind\": \"actions\", \"title\": \"Do\","
		"  \"body\": \"New ticket | /e/ticket/new\\nBoard | /tickets\\n"
		"My board | /tickets?assignee={me}\\nNew idea | /e/idea/new\"},"
		" {\"kind\": \"note\", \"title\": \"Working agreement\","
		"  \"body\": \"Triage daily. Nothing sits in review longer than "
		"a day.\\n\\n- Blocked means somebody else has the next move; "
		"say who.\\n- Done means released, not merged.\"},"
		" {\"kind\": \"activity\", \"limit\": 8, \"span\": \"full\"}"
		"]}"
	},
	{
		"overview", "Overview",
		"A general home page: the month's figures, the tickets, the "
		"pipeline, and what just happened -- the built-in dashboard, as "
		"widgets you can rearrange.",
		NULL,
		"{"
		"\"name\": \"Overview\","
		"\"slug\": \"overview\","
		"\"description\": \"This month across every venture.\","
		"\"purpose\": \"overview\","
		"\"layout\": \"three_columns\","
		"\"widgets\": ["
		" {\"kind\": \"metric\", \"title\": \"Revenue\","
		"  \"report_name\": \"pnl\", \"field\": \"revenue\"},"
		" {\"kind\": \"metric\", \"title\": \"Profit\","
		"  \"report_name\": \"pnl\", \"field\": \"profit\"},"
		" {\"kind\": \"count\", \"title\": \"Open tickets\","
		"  \"entity_type\": \"ticket\","
		"  \"filter\": \"status__not_in=done,cancelled\"},"
		" {\"kind\": \"breakdown\", \"title\": \"Tickets\","
		"  \"entity_type\": \"ticket\", \"field\": \"status\"},"
		" {\"kind\": \"report\", \"title\": \"Pipeline\","
		"  \"report_name\": \"pipeline\", \"options\": \"{\\\"table\\\": false}\"},"
		" {\"kind\": \"list\", \"title\": \"Ventures\","
		"  \"entity_type\": \"venture\", \"limit\": 8,"
		"  \"columns\": \"status,venture_type\"},"
		" {\"kind\": \"search\"},"
		" {\"kind\": \"confirmations\"},"
		" {\"kind\": \"activity\", \"limit\": 8, \"span\": \"full\"}"
		"]}"
	}
};

const VentureDashboardTemplate *
venture_dashboard_get_templates(gsize *n_templates)
{
	if (NULL != n_templates)
		*n_templates = G_N_ELEMENTS(venture_dashboard_templates);

	return venture_dashboard_templates;
}

VentureDashboard *
venture_dashboard_create_from_template(
	VentureContext		 *context,
	const gchar		 *template_name,
	gint64			  owner_user_id,
	const VentureActor	 *actor,
	GError			**error
){
	g_autoptr(JsonNode) definition = NULL;
	gsize i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	for (i = 0; i < G_N_ELEMENTS(venture_dashboard_templates); i++)
	{
		if (0 == g_strcmp0(venture_dashboard_templates[i].name, template_name))
		{
			definition = venture_json_parse(
				venture_dashboard_templates[i].definition, error);

			if (NULL == definition)
				return NULL;

			return venture_dashboard_import(context, definition,
			                                owner_user_id, actor, error);
		}
	}

	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
	            "There is no dashboard template called \"%s\"",
	            (NULL != template_name) ? template_name : "");

	return NULL;
}

/* ==========================================================================
 * Validators
 * ========================================================================== */

/*
 * A widget must name a kind that exists, a record type that exists (even
 * one whose module is off -- the widget outlives the switch), a report
 * that exists, and options that are a JSON object. Run at the save, so
 * the same rules hold for the editor, the API, an import and the
 * assistant.
 */
static gboolean
venture_dashboard_validate_widget(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	gpointer	  user_data,
	GError		**error
){
	VentureContext *context;
	VentureDashboardWidget *widget;
	g_autofree gchar *kind = NULL;
	g_autofree gchar *entity_type = NULL;
	g_autofree gchar *report_name = NULL;
	g_autoptr(JsonNode) options = NULL;
	const VentureWidgetKindInfo *info;

	(void)database;
	(void)previous;

	context = user_data;
	widget = VENTURE_DASHBOARD_WIDGET(entity);
	kind = venture_widget_get_string(widget, "kind");
	info = venture_widget_kind_registry_lookup(
		venture_widget_kind_registry_get_default(), kind);

	if (NULL == info)
	{
		g_autofree gchar *names = NULL;
		g_auto(GStrv) list = NULL;

		list = venture_widget_kind_registry_list_names(
			venture_widget_kind_registry_get_default());
		names = g_strjoinv(", ", list);
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "There is no widget kind called \"%s\"; the kinds are %s",
		            (NULL != kind) ? kind : "", names);
		return FALSE;
	}

	entity_type = venture_widget_get_string(widget, "entity-type");

	if (NULL != entity_type)
	{
		VentureEntityRegistry *registry;

		registry = venture_context_get_entity_registry(context);

		if (G_TYPE_INVALID == venture_entity_registry_lookup_any(registry,
		                                                         entity_type))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			            "There is no record type called \"%s\"", entity_type);
			return FALSE;
		}
	}

	report_name = venture_widget_get_string(widget, "report-name");

	/* Hidden reports are accepted for the reason hidden types are: a
	 * report is hidden by a module switch and comes back with it. */
	if ((NULL != report_name) &&
	    (NULL == venture_report_registry_lookup(
			venture_context_get_report_registry(context), report_name)))
	{
		VentureModule *module;
		gboolean owned;
		guint i;

		owned = FALSE;

		{
			g_autoptr(GPtrArray) modules = NULL;

			modules = venture_module_registry_list(
				venture_context_get_modules(context));

			for (i = 0; (i < modules->len) && !owned; i++)
			{
				module = g_ptr_array_index(modules, i);
				owned = g_strv_contains(venture_module_get_reports(module),
				                        report_name);
			}
		}

		if (!owned)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			            "There is no report called \"%s\"", report_name);
			return FALSE;
		}
	}

	options = venture_widget_get_options(widget, error);

	if ((NULL == options) && (NULL != error) && (NULL != *error))
		return FALSE;

	/* The grid hints must at least be sane numbers; whether they fit the
	 * layout is decided when the page is laid out, because the layout
	 * can change after the widget was placed. */
	{
		gint64 col = 0;
		gint64 row = 0;
		gint64 width = 0;
		gint64 height = 0;

		g_object_get(widget, "grid-col", &col, "grid-row", &row,
		             "grid-width", &width, "grid-height", &height, NULL);

		if ((col < 0) || (row < 0) || (width < 0) || (height < 0) ||
		    (col > 4) || (width > 4) || (height > VENTURE_GRID_MAX_HEIGHT) ||
		    (row >= VENTURE_GRID_MAX_ROWS))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			            "The grid placement is out of range: columns run "
			            "1 to 4, widths 1 to 4, heights 1 to %d, and 0 "
			            "means automatic", VENTURE_GRID_MAX_HEIGHT);
			return FALSE;
		}
	}

	return TRUE;
}

/*
 * A dashboard's slug is its address, so two living dashboards may not
 * share one; and only one dashboard is home, so marking this one unmarks
 * the others. The second is a write from inside a validator, which is
 * allowed -- the lock is recursive and the nested save is audited like any
 * other -- and is here rather than in the web handler so the API and an
 * import behave the same.
 */
static gboolean
venture_dashboard_validate_dashboard(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	gpointer	  user_data,
	GError		**error
){
	g_autofree gchar *slug = NULL;
	gboolean home = FALSE;
	gboolean was_home = FALSE;

	(void)user_data;

	g_object_get(entity, "slug", &slug, "home", &home, NULL);

	{
		g_autoptr(VentureDashboard) other = NULL;

		other = venture_dashboard_find_by_slug(database, slug, NULL);

		if ((NULL != other) &&
		    (venture_entity_get_id(VENTURE_ENTITY(other)) !=
		     venture_entity_get_id(entity)))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_ALREADY_EXISTS,
			            "Another dashboard is already at /dashboards/%s",
			            slug);
			return FALSE;
		}
	}

	if (NULL != previous)
		g_object_get(previous, "home", &was_home, NULL);

	if (home && !was_home)
	{
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(GPtrArray) homes = NULL;
		guint i;

		query = venture_query_new(VENTURE_TYPE_DASHBOARD);
		venture_query_add_filter_string(query, "home", VENTURE_FILTER_OP_EQ,
		                                "true", NULL);
		venture_query_set_limit(query, 100);
		homes = venture_database_find(database, query, NULL);

		for (i = 0; (NULL != homes) && (i < homes->len); i++)
		{
			VentureEntity *other;

			other = g_ptr_array_index(homes, i);

			if (venture_entity_get_id(other) == venture_entity_get_id(entity))
				continue;

			g_object_set(other, "home", FALSE, NULL);

			if (!venture_database_save(database, other, NULL, error))
				return FALSE;
		}
	}

	return TRUE;
}

void
venture_dashboard_install_validators(VentureContext *context)
{
	VentureDatabase *database;

	g_return_if_fail(VENTURE_IS_CONTEXT(context));

	database = venture_context_get_database(context);

	/* The context outlives the database's validator table only if the
	 * database outlives the context, which it does not: the context holds
	 * a reference. No reference is taken here to avoid a cycle. */
	venture_database_add_save_validator(database, VENTURE_TYPE_DASHBOARD_WIDGET,
	                                    venture_dashboard_validate_widget,
	                                    context, NULL);
	venture_database_add_save_validator(database, VENTURE_TYPE_DASHBOARD,
	                                    venture_dashboard_validate_dashboard,
	                                    context, NULL);
}
