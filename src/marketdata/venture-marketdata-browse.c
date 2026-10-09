/*
 * venture-marketdata-browse.c - What the market pages, reports and widgets show
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Each public function here answers one page's question as JSON, and
 * every door -- the page, its /api/v1/market twin, the report and the
 * widget -- reads that one answer. See the header for the shapes.
 *
 * The reads are bounded by construction: every list the store is asked
 * for is a page of an indexed sort, the history behind a chart is two
 * weeks of one instrument at one venue, and the per-venue loops stop at
 * the store's own caps. Nothing here sorts or filters store rows in C.
 */

#include "venture.h"
#include "marketdata/venture-marketdata-private.h"

#include <math.h>
#include <string.h>

#define MD_DAY (86400)
#define MD_HOURLY_DAYS (14)
#define MD_HEAT_DAYS (7)
#define MD_DAILY_DAYS (60)
#define MD_MEDIAN_DAYS (14)
#define MD_TREND_DAYS (14)
#define MD_MAX_SOURCES (500)
#define MD_MAX_ENTRIES (500)
#define MD_MAX_RULES (500)
#define MD_MAX_PAGE_NUMBER (1000000)
#define MD_HEALTH_VENUES (2000)

static const gchar *const md_browse_sorts[] = {
	"min_price", "quantity", "market_value", "region_median", "pct_vs_region",
	"sale_rate", "sold_per_day", "deal_price", "listings", "name", "updated",
	"venue", NULL
};

/* --- JSON helpers ---------------------------------------------------------- */

static gint64
md_now(gint64 now)
{
	return (now > 0) ? now : g_get_real_time() / G_USEC_PER_SEC;
}

gint64
venture_marketdata_stale_seconds(VentureContext *context)
{
	VentureConfig *config;
	gint64 minutes = VENTURE_MARKETDATA_STALE_MINUTES_DEFAULT;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), minutes * 60);

	config = venture_context_get_config(context);

	if (NULL != config)
		g_object_get(config, "series-stale-minutes", &minutes, NULL);

	/* The configuration refuses anything else at startup; a test that
	 * sets the property directly is held to the same bounds. */
	return CLAMP(minutes, 1, 10080) * 60;
}

#ifdef VENTURE_HAVE_SQLITE

/* A price in minor units as a money object, or null when unknown. */
static void
md_set_money(
	JsonObject	*object,
	const gchar	*name,
	gint64		 minor,
	const gchar	*currency
){
	g_autoptr(VentureMoney) money = NULL;

	if ((VENTURE_SERIES_NONE == minor) || venture_string_is_empty(currency) ||
	    !venture_currency_is_valid(currency))
	{
		json_object_set_null_member(object, name);
		return;
	}

	money = venture_money_new_for_currency(minor, currency);

	if (NULL == money)
	{
		json_object_set_null_member(object, name);
		return;
	}

	json_object_set_member(object, name, venture_money_to_json(money));
}

static void
md_set_figure(
	JsonObject	*object,
	const gchar	*name,
	gint64		 value
){
	if (VENTURE_SERIES_NONE == value)
		json_object_set_null_member(object, name);
	else
		json_object_set_int_member(object, name, value);
}

static void
md_set_ratio(
	JsonObject	*object,
	const gchar	*name,
	gdouble		 value
){
	if (!isfinite(value))
		json_object_set_null_member(object, name);
	else
		json_object_set_double_member(object, name, value);
}

/* A Unix time as ISO 8601 UTC, or null for none. */
static void
md_set_time(
	JsonObject	*object,
	const gchar	*name,
	gint64		 when
){
	g_autoptr(GDateTime) moment = NULL;
	g_autofree gchar *text = NULL;

	if ((VENTURE_SERIES_NONE == when) || (when <= 0))
	{
		json_object_set_null_member(object, name);
		return;
	}

	moment = g_date_time_new_from_unix_utc(when);

	if (NULL == moment)
	{
		json_object_set_null_member(object, name);
		return;
	}

	text = venture_time_to_string(moment);
	json_object_set_string_member(object, name, text);
}

/*
 * How old a price is: @prefix "taken_at" (when its snapshot was taken),
 * "age_seconds" (now less that, null when unknown) and "stale" (older
 * than @stale_after). A price with no time is stale -- nothing says it is
 * current -- which is the side to be wrong on: a realm whose feed stopped
 * eight hours ago read as the cheapest place to buy, with nothing on the
 * page to say so.
 */
static void
md_set_age(
	JsonObject	*object,
	const gchar	*prefix,
	gint64		 taken_at,
	gint64		 now,
	gint64		 stale_after
){
	g_autofree gchar *at = g_strconcat(prefix, "taken_at", NULL);
	g_autofree gchar *age = g_strconcat(prefix, "age_seconds", NULL);
	g_autofree gchar *stale = g_strconcat(prefix, "stale", NULL);
	gboolean known = (VENTURE_SERIES_NONE != taken_at) && (taken_at > 0);

	md_set_time(object, at, taken_at);

	if (known)
		json_object_set_int_member(object, age, MAX((gint64)0, now - taken_at));
	else
		json_object_set_null_member(object, age);

	json_object_set_boolean_member(object, stale, !known || (now - taken_at > stale_after));
}

#endif /* VENTURE_HAVE_SQLITE */

static void
md_set_text(
	JsonObject	*object,
	const gchar	*name,
	const gchar	*value
){
	if (NULL == value)
		json_object_set_null_member(object, name);
	else
		json_object_set_string_member(object, name, value);
}

static void
md_note(
	JsonArray	*notes,
	const gchar	*text
){
	json_array_add_string_element(notes, text);
}

static JsonNode *
md_node(JsonObject *root)
{
	JsonNode *node;

	node = json_node_new(JSON_NODE_OBJECT);
	json_node_take_object(node, root);

	return node;
}

/*
 * The answer's `attribution`: the lines of the providers whose data it
 * shows. Every answer here gets the member, empty for none, so a reader
 * never has to ask whether it is there.
 */

/* An answer about one source, which shows that source's data only when
 * its store could be read. */
static void
md_attribute_source(
	VentureContext	*context,
	gint64		 organization_id,
	JsonObject	*root
){
	g_autoptr(GPtrArray) attributions = NULL;

	attributions = g_ptr_array_new_with_free_func(g_free);

	if (json_object_get_boolean_member_with_default(root, "available", FALSE))
		venture_marketdata_attribution_add_source(context, organization_id,
		                                          json_object_get_int_member_with_default(
		                                                  root, "data_source_id", 0),
		                                          attributions);

	venture_marketdata_attribution_set(root, attributions);
}

/* An answer whose @member lists rows from any number of sources. */
static void
md_attribute_member(
	VentureContext	*context,
	gint64		 organization_id,
	JsonObject	*root,
	const gchar	*member
){
	g_autoptr(GPtrArray) attributions = NULL;

	attributions = g_ptr_array_new_with_free_func(g_free);
	venture_marketdata_attribution_collect(context, organization_id,
	                                       json_object_get_member(root, member), attributions);
	venture_marketdata_attribution_set(root, attributions);
}

gchar *
venture_marketdata_instrument_path(
	gint64		 data_source_id,
	const gchar	*key,
	const gchar	*venue
){
	g_autofree gchar *escaped = NULL;
	g_autoptr(GString) path = NULL;

	/* Every reserved character escaped, "/" included: a key is one
	 * segment of the path whatever it contains. */
	escaped = g_uri_escape_string((NULL != key) ? key : "", NULL, FALSE);
	path = g_string_new(NULL);
	g_string_append_printf(path, "/market/i/%" G_GINT64_FORMAT "/%s", data_source_id, escaped);

	if (!venture_string_is_empty(venue))
	{
		g_autofree gchar *escaped_venue = NULL;

		escaped_venue = g_uri_escape_string(venue, NULL, FALSE);
		g_string_append_printf(path, "?venue=%s", escaped_venue);
	}

	return g_string_free(g_steal_pointer(&path), FALSE);
}

VentureMoney *
venture_marketdata_parse_amount(
	const gchar	 *text,
	GError		**error
){
	g_autoptr(VentureMoney) one = NULL;
	g_autoptr(VentureMoney) other = NULL;

	g_return_val_if_fail(NULL != text, NULL);

	/*
	 * The parser falls back on a default currency when the text names
	 * none. Read the text under two different defaults: an amount that
	 * names its currency reads the same both times, and one that does
	 * not is refused rather than read in whichever the install uses.
	 */
	one = venture_money_from_string(text, "USD", error);

	if (NULL == one)
		return NULL;

	other = venture_money_from_string(text, "EUR", NULL);

	if ((NULL == other) ||
	    (0 != g_strcmp0(venture_money_get_currency(one), venture_money_get_currency(other))))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "\"%s\" names no currency: write it with one, e.g. \"10.00 GOLD\"", text);
		return NULL;
	}

	return g_steal_pointer(&one);
}

/* --- Sources ------------------------------------------------------------- */

static gboolean
md_require_module(
	VentureContext	 *context,
	GError		**error
){
	if (!venture_context_module_enabled(context, "marketdata"))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		                    "The marketdata module is off");
		return FALSE;
	}

	return TRUE;
}

/*
 * Whether there is a store to read at all, and a note saying why not.
 * This is the "no data sources" state every page degrades to.
 */
static gboolean
md_series_ready(
	VentureContext	*context,
	JsonArray	*notes
){
#ifndef VENTURE_HAVE_SQLITE
	(void)context;
	md_note(notes, "No data sources: this build has no series store (it was built without SQLite).");
	return FALSE;
#else
	if (!venture_context_module_enabled(context, "feeds"))
	{
		md_note(notes, "No data sources: market data feeds are off (feeds.enabled).");
		return FALSE;
	}

	return TRUE;
#endif
}

#ifdef VENTURE_HAVE_SQLITE

/* The organization's live sources, by name. */
static GPtrArray *
md_sources(
	VentureContext	 *context,
	gint64		  organization_id,
	GError		**error
){
	g_autoptr(VentureQuery) query = NULL;

	query = venture_query_new(VENTURE_TYPE_DATA_SOURCE);
	venture_query_set_organization(query, organization_id);
	venture_query_add_order(query, "name", VENTURE_SORT_ASCENDING, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	venture_query_set_limit(query, MD_MAX_SOURCES);

	return venture_database_find(venture_context_get_database(context), query, error);
}

/* One source, which must be the organization's and live: NOT_FOUND says
 * nothing about whether another organization has one by that id. */
static VentureEntity *
md_source(
	VentureContext	 *context,
	gint64		  organization_id,
	gint64		  data_source_id,
	GError		**error
){
	g_autoptr(VentureEntity) source = NULL;

	if (data_source_id > 0)
		source = venture_database_get(venture_context_get_database(context),
		                              VENTURE_TYPE_DATA_SOURCE, data_source_id, NULL);

	if ((NULL == source) || venture_entity_is_deleted(source) ||
	    (venture_entity_get_organization_id(source) != organization_id))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "No data source #%" G_GINT64_FORMAT " in this organization", data_source_id);
		return NULL;
	}

	return g_steal_pointer(&source);
}

static gchar *
md_source_name(VentureEntity *source)
{
	gchar *name = NULL;

	g_object_get(source, "name", &name, NULL);

	if (venture_string_is_empty(name))
	{
		g_free(name);
		name = g_strdup_printf("Data source #%" G_GINT64_FORMAT, venture_entity_get_id(source));
	}

	return name;
}

static JsonArray *
md_sources_json(GPtrArray *sources)
{
	JsonArray *array;
	guint i;

	array = json_array_new();

	for (i = 0; (NULL != sources) && (i < sources->len); i++)
	{
		VentureEntity *source = g_ptr_array_index(sources, i);
		g_autofree gchar *name = NULL;
		JsonObject *object;

		name = md_source_name(source);
		object = json_object_new();
		json_object_set_int_member(object, "id", venture_entity_get_id(source));
		json_object_set_string_member(object, "name", name);
		json_array_add_object_element(array, object);
	}

	return array;
}

/*
 * A read handle on a source's store. A store that has stored nothing, or
 * cannot be read, is a note when @notes is given -- one bad store must not
 * take a page of several down -- and an error otherwise.
 */
static VentureSeriesStore *
md_reader(
	VentureContext	 *context,
	VentureEntity	 *source,
	JsonArray	 *notes,
	GError		**error
){
	g_autoptr(GError) local_error = NULL;
	g_autofree gchar *name = NULL;
	g_autofree gchar *text = NULL;
	VentureFeedsService *service;
	VentureSeriesStore *store;

	name = md_source_name(source);
	service = venture_context_get_feeds_service(context);

	if (NULL == service)
	{
		g_set_error_literal(&local_error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		                    "Market data feeds are off (feeds.enabled)");
	}
	else
	{
		store = venture_feeds_service_open_reader(service, venture_entity_get_id(source),
		                                          &local_error);

		if (NULL != store)
			return store;

		if (g_error_matches(local_error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND))
		{
			g_clear_error(&local_error);
			g_set_error(&local_error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
			            "%s has stored nothing yet", name);
		}
	}

	if (NULL == notes)
	{
		g_propagate_error(error, g_steal_pointer(&local_error));
		return NULL;
	}

	text = g_strdup_printf("%s: %s", name, local_error->message);
	md_note(notes, text);

	return NULL;
}

/* One current row as JSON: what browse, deals and the venue table show. */
/*
 * A variant's distinguishing part, readably: the instrument key past its
 * plain item ("19019:b1472,6646:m9=70" -> "bonuses 1472, 6646 · modifiers
 * 9=70"). NULL for a key with no variant part. Only the shape the
 * blizzard-auctions plugin writes is spelled out; any other suffix is
 * shown as it is rather than guessed at.
 */
static gchar *
md_variant_label(const gchar *key)
{
	g_autoptr(GString) label = NULL;
	g_auto(GStrv) parts = NULL;
	const gchar *colon;
	guint i;

	colon = (NULL != key) ? strchr(key, ':') : NULL;
	if (NULL == colon)
		return NULL;

	label = g_string_new(NULL);
	parts = g_strsplit(colon + 1, ":", -1);

	for (i = 0; NULL != parts[i]; i++)
	{
		g_auto(GStrv) items = NULL;
		const gchar *body = parts[i] + 1;

		if ('\0' == parts[i][0])
			continue;

		if (label->len > 0)
			g_string_append(label, " · ");

		if ('b' == parts[i][0])
		{
			g_autofree gchar *joined = NULL;

			items = g_strsplit(body, ",", -1);
			joined = g_strjoinv(", ", items);
			g_string_append_printf(label, "%s %s",
			                       (g_strv_length(items) > 1) ? "bonuses" : "bonus", joined);
		}
		else if ('m' == parts[i][0])
		{
			g_autofree gchar *joined = NULL;

			items = g_strsplit(body, ",", -1);
			joined = g_strjoinv(", ", items);
			g_string_append_printf(label, "%s %s",
			                       (g_strv_length(items) > 1) ? "modifiers" : "modifier", joined);
		}
		else if ('p' == parts[i][0])
		{
			const gchar *dot = strchr(body, '.');

			if (NULL != dot)
				g_string_append_printf(label, "pet species %.*s, breed %s",
				                       (gint)(dot - body), body, dot + 1);
			else
				g_string_append_printf(label, "pet species %s", body);
		}
		else
			g_string_append(label, parts[i]);
	}

	return (label->len > 0) ? g_string_free(g_steal_pointer(&label), FALSE) : NULL;
}

/* Parses an attrs JSON text; NULL for none or anything but an object. */
static JsonObject *
md_attrs_object(const gchar *attrs_json)
{
	g_autoptr(JsonParser) parser = NULL;

	if (NULL == attrs_json)
		return NULL;

	parser = json_parser_new();

	if (!json_parser_load_from_data(parser, attrs_json, -1, NULL) ||
	    !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
		return NULL;

	return json_object_ref(json_node_get_object(json_parser_get_root(parser)));
}

/*
 * Appends to @out the links an attrs object declares (attrs.links, each
 * {label, url}), with every "{name}" in a url replaced by @values' scalar
 * member of that name, URI-escaped. A link whose url names a value @values
 * lacks is left out rather than shown broken; only http(s) urls are kept.
 * This is how a provider says "this item on that site" without VENTURE
 * knowing any site: the item carries its own links, and a venue carries
 * templates the item fills in.
 */
static void
md_links_append(
	JsonArray	*out,
	JsonObject	*declaring,
	JsonObject	*values
){
	JsonArray *links;
	guint i;

	if ((NULL == declaring) || !json_object_has_member(declaring, "links") ||
	    !JSON_NODE_HOLDS_ARRAY(json_object_get_member(declaring, "links")))
		return;

	links = json_object_get_array_member(declaring, "links");

	for (i = 0; i < json_array_get_length(links); i++)
	{
		JsonNode *element = json_array_get_element(links, i);
		g_autoptr(GString) url = NULL;
		const gchar *label;
		const gchar *template;
		const gchar *p;
		gboolean complete = TRUE;
		JsonObject *link;

		if (!JSON_NODE_HOLDS_OBJECT(element))
			continue;

		label = json_object_get_string_member_with_default(json_node_get_object(element), "label", NULL);
		template = json_object_get_string_member_with_default(json_node_get_object(element), "url", NULL);

		if (venture_string_is_empty(label) || (NULL == template) ||
		    !(g_str_has_prefix(template, "https://") || g_str_has_prefix(template, "http://")))
			continue;

		url = g_string_new(NULL);

		for (p = template; complete && ('\0' != *p); p++)
		{
			const gchar *close;
			g_autofree gchar *name = NULL;
			g_autofree gchar *value = NULL;
			JsonNode *member;

			if ('{' != *p)
			{
				g_string_append_c(url, *p);
				continue;
			}

			close = strchr(p, '}');
			if (NULL == close)
			{
				complete = FALSE;
				break;
			}

			name = g_strndup(p + 1, close - p - 1);
			member = (NULL != values) ? json_object_get_member(values, name) : NULL;

			if ((NULL == member) || !JSON_NODE_HOLDS_VALUE(member))
				complete = FALSE;
			else if (G_TYPE_STRING == json_node_get_value_type(member))
				value = g_strdup(json_node_get_string(member));
			else if (G_TYPE_INT64 == json_node_get_value_type(member))
				value = g_strdup_printf("%" G_GINT64_FORMAT, json_node_get_int(member));
			else
				complete = FALSE;

			if (complete)
				g_string_append_uri_escaped(url, value, NULL, FALSE);

			p = close;
		}

		if (!complete)
			continue;

		link = json_object_new();
		json_object_set_string_member(link, "label", label);
		json_object_set_string_member(link, "url", url->str);
		json_array_add_object_element(out, link);
	}
}

/* "#rrggbb" and nothing else: a colour a provider sets reaches a style
 * attribute, so anything that could close it is refused. */
static gboolean
md_is_color(const gchar *text)
{
	guint i;

	if ((NULL == text) || ('#' != text[0]) || (7 != strlen(text)))
		return FALSE;

	for (i = 1; i < 7; i++)
		if (!g_ascii_isxdigit(text[i]))
			return FALSE;

	return TRUE;
}

/* An icon a page may load: this server's own path, or https. */
static gboolean
md_is_icon(const gchar *text)
{
	return (NULL != text) && (strlen(text) < 512) && (NULL == strpbrk(text, "\"'<> \\\\")) &&
	       ((('/' == text[0]) && ('/' != text[1])) || g_str_has_prefix(text, "https://"));
}

/*
 * The provider's "display" attribute, checked, as @object's "display":
 * {color, icon, lines: [{text, right, color}]}. How an instrument is drawn
 * -- its name's colour, its icon, a tooltip -- is the provider's to say
 * (blizzard-auctions draws an item as the game does); VENTURE only keeps
 * what is safe to put in a page: colours as #rrggbb, an icon of its own or
 * over https, at most 40 lines of bounded text. Text is escaped where it
 * is drawn like any other.
 */
static void
md_set_display(
	JsonObject	*object,
	JsonObject	*attrs
){
	JsonObject *display;
	JsonObject *checked;
	JsonArray *lines;
	JsonArray *kept;
	const gchar *text;
	guint i;

	if ((NULL == attrs) || !json_object_has_member(attrs, "display") ||
	    !JSON_NODE_HOLDS_OBJECT(json_object_get_member(attrs, "display")))
		return;

	display = json_object_get_object_member(attrs, "display");
	checked = json_object_new();

	text = json_object_get_string_member_with_default(display, "color", NULL);
	if (md_is_color(text))
		json_object_set_string_member(checked, "color", text);

	text = json_object_get_string_member_with_default(display, "icon", NULL);
	if (md_is_icon(text))
		json_object_set_string_member(checked, "icon", text);

	kept = json_array_new();
	lines = json_object_has_member(display, "lines") &&
	        JSON_NODE_HOLDS_ARRAY(json_object_get_member(display, "lines"))
		? json_object_get_array_member(display, "lines") : NULL;

	for (i = 0; (NULL != lines) && (i < json_array_get_length(lines)) && (json_array_get_length(kept) < 40); i++)
	{
		JsonNode *element = json_array_get_element(lines, i);
		JsonObject *line;
		JsonObject *copy;
		const gchar *right;
		const gchar *color;

		if (!JSON_NODE_HOLDS_OBJECT(element))
			continue;

		line = json_node_get_object(element);
		text = json_object_get_string_member_with_default(line, "text", NULL);
		right = json_object_get_string_member_with_default(line, "right", NULL);
		color = json_object_get_string_member_with_default(line, "color", NULL);

		if (venture_string_is_empty(text) || (strlen(text) > 1024))
			continue;

		copy = json_object_new();
		json_object_set_string_member(copy, "text", text);
		if (!venture_string_is_empty(right) && (strlen(right) <= 256))
			json_object_set_string_member(copy, "right", right);
		if (md_is_color(color))
			json_object_set_string_member(copy, "color", color);
		json_array_add_object_element(kept, copy);
	}

	json_object_set_array_member(checked, "lines", kept);
	json_object_set_object_member(object, "display", checked);
}

/* The display of @key in @reader's store, onto a row object. */
static void
md_row_set_display(
	JsonObject		*object,
	VentureSeriesStore	*reader,
	const gchar		*key
){
	g_autoptr(VentureSeriesInstrumentRow) instrument = NULL;
	g_autoptr(JsonObject) attrs = NULL;

	if (!venture_series_store_get_instrument(reader, key, &instrument, NULL) || (NULL == instrument))
		return;

	attrs = md_attrs_object(instrument->attrs_json);
	md_set_display(object, attrs);

	if ((NULL != attrs) && json_object_has_member(attrs, "level") &&
	    JSON_NODE_HOLDS_VALUE(json_object_get_member(attrs, "level")))
		json_object_set_int_member(object, "level", json_object_get_int_member(attrs, "level"));
}

/*
 * An instrument's stored attributes as an object, its item level hoisted
 * to "level" (the plain item's level: a variant's bonuses change it, and
 * the Game Data API does not say by how much), and its variant label.
 */
static void
md_set_attrs_and_variant(
	JsonObject	*object,
	const gchar	*key,
	const gchar	*parent_key,
	const gchar	*attrs_json
){
	g_autofree gchar *variant = NULL;

	if (NULL != attrs_json)
	{
		g_autoptr(JsonParser) parser = json_parser_new();

		if (json_parser_load_from_data(parser, attrs_json, -1, NULL) &&
		    JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
		{
			JsonObject *attrs = json_node_get_object(json_parser_get_root(parser));

			if (json_object_has_member(attrs, "level"))
				json_object_set_int_member(object, "level",
				                           json_object_get_int_member(attrs, "level"));

			md_set_display(object, attrs);
			json_object_set_object_member(object, "attrs", json_object_ref(attrs));
		}
	}

	variant = (NULL != parent_key) ? md_variant_label(key) : NULL;
	md_set_text(object, "variant", variant);
}

static JsonObject *
md_row_object(
	const VentureSeriesRow	*row,
	gint64			 data_source_id
){
	g_autofree gchar *url = NULL;
	JsonObject *object;

	object = json_object_new();
	json_object_set_string_member(object, "venue_key", row->venue_key);
	md_set_text(object, "venue_name", row->venue_name);
	json_object_set_string_member(object, "group_key", (NULL != row->group_key) ? row->group_key : "");
	json_object_set_string_member(object, "instrument_key", row->instrument_key);
	md_set_text(object, "instrument_name", row->instrument_name);
	{
		g_autofree gchar *variant = md_variant_label(row->instrument_key);

		md_set_text(object, "variant", variant);
	}
	md_set_text(object, "category", row->category);
	md_set_text(object, "kind", row->kind);
	json_object_set_string_member(object, "currency", row->currency);
	md_set_time(object, "taken_at", row->taken_at);
	md_set_time(object, "seen_at", row->seen_at);
	md_set_figure(object, "quantity", row->quantity);
	json_object_set_int_member(object, "listings", row->listings);
	md_set_money(object, "min_price", row->min_price, row->currency);
	md_set_money(object, "market_value", row->market_value, row->currency);
	md_set_money(object, "median", row->median, row->currency);
	md_set_money(object, "region_median", row->region_median, row->currency);
	md_set_money(object, "deal_price", row->deal_price, row->currency);
	md_set_ratio(object, "pct_vs_region", row->pct_vs_region);
	md_set_ratio(object, "sale_rate", row->sale_rate);
	md_set_ratio(object, "sold_per_day", row->sold_per_day);
	url = venture_marketdata_instrument_path(data_source_id, row->instrument_key, row->venue_key);
	json_object_set_string_member(object, "url", url);

	return object;
}

static JsonArray *
md_venues_json(
	GPtrArray	*venues,
	JsonArray	*groups
){
	g_autoptr(GHashTable) seen = NULL;
	JsonArray *array;
	guint i;

	array = json_array_new();
	seen = g_hash_table_new(g_str_hash, g_str_equal);

	for (i = 0; (NULL != venues) && (i < venues->len); i++)
	{
		VentureSeriesVenueRow *venue = g_ptr_array_index(venues, i);
		JsonObject *object;
		const gchar *group;

		group = (NULL != venue->group_key) ? venue->group_key : "";
		object = json_object_new();
		json_object_set_string_member(object, "key", venue->key);
		md_set_text(object, "name", venue->name);
		json_object_set_string_member(object, "group_key", group);
		md_set_text(object, "currency", venue->currency);
		json_array_add_object_element(array, object);

		if ((NULL != groups) && ('\0' != group[0]) && !g_hash_table_contains(seen, group))
		{
			g_hash_table_add(seen, (gpointer)group);
			json_array_add_string_element(groups, group);
		}
	}

	return array;
}

/* --- Categories ------------------------------------------------------------ */

/*
 * A store's categories, kept a while: listing them groups every
 * instrument (850,000 on a US auction house store), and every market page
 * draws the picker, but the list changes only when a provider files a new
 * kind of item. Keyed by the store's file; the pages are answered on one
 * thread.
 */
#define MD_CATEGORY_CACHE_SECONDS (600)

typedef struct
{
	gint64		 at;
	GPtrArray	*categories;	/* VentureSeriesCategoryRow */
} MdCategoryCache;

static void
md_category_cache_free(gpointer data)
{
	MdCategoryCache *entry = data;

	g_ptr_array_unref(entry->categories);
	g_free(entry);
}

static GPtrArray *
md_categories_of(VentureSeriesStore *reader)
{
	static GHashTable *cache = NULL;
	const gchar *path = venture_series_store_get_path(reader);
	gint64 now = g_get_monotonic_time() / G_USEC_PER_SEC;
	MdCategoryCache *entry;
	GPtrArray *fresh;

	if (NULL == cache)
		cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, md_category_cache_free);

	entry = (NULL != path) ? g_hash_table_lookup(cache, path) : NULL;

	if ((NULL != entry) && (now - entry->at < MD_CATEGORY_CACHE_SECONDS))
		return g_ptr_array_ref(entry->categories);

	fresh = venture_series_store_list_categories(reader, NULL);

	if ((NULL != fresh) && (NULL != path))
	{
		entry = g_new0(MdCategoryCache, 1);
		entry->at = now;
		entry->categories = g_ptr_array_ref(fresh);
		g_hash_table_replace(cache, g_strdup(path), entry);
	}

	return fresh;
}

/* Adds @reader's categories to @counts (path -> plain instruments). */
static void
md_categories_add(
	GHashTable		*counts,
	VentureSeriesStore	*reader
){
	g_autoptr(GPtrArray) categories = NULL;
	guint i;

	categories = md_categories_of(reader);

	for (i = 0; (NULL != categories) && (i < categories->len); i++)
	{
		VentureSeriesCategoryRow *row = g_ptr_array_index(categories, i);
		gint64 *count = g_hash_table_lookup(counts, row->path);

		if (NULL == count)
		{
			count = g_new0(gint64, 1);
			g_hash_table_insert(counts, g_strdup(row->path), count);
		}

		*count += row->instruments;
	}
}

static gint
md_compare_strings(gconstpointer a, gconstpointer b)
{
	return g_strcmp0(*(const gchar *const *)a, *(const gchar *const *)b);
}

/* @counts as the answer's "categories": [{path, instruments}], by path. */
static JsonArray *
md_categories_json(GHashTable *counts)
{
	g_autoptr(GPtrArray) paths = g_ptr_array_new();
	GHashTableIter iter;
	gpointer key;
	JsonArray *array;
	guint i;

	g_hash_table_iter_init(&iter, counts);
	while (g_hash_table_iter_next(&iter, &key, NULL))
		g_ptr_array_add(paths, key);

	g_ptr_array_sort(paths, md_compare_strings);
	array = json_array_new();

	for (i = 0; i < paths->len; i++)
	{
		JsonObject *object = json_object_new();
		const gchar *path = g_ptr_array_index(paths, i);

		json_object_set_string_member(object, "path", path);
		json_object_set_int_member(object, "instruments", *(gint64 *)g_hash_table_lookup(counts, path));
		json_array_add_object_element(array, object);
	}

	return array;
}

static GHashTable *
md_categories_new(void)
{
	return g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
}

/* --- Venue groups ---------------------------------------------------------- */

/* The pseudo-group of the realms the operator's characters are on. */
#define MD_GROUP_CHARACTERS "characters"

/* Lower-cased, trimmed text, for comparing a person's spelling of a name
 * with a store's. */
static gchar *
md_fold(const gchar *text)
{
	g_autofree gchar *trimmed = g_strstrip(g_strdup((NULL != text) ? text : ""));

	return g_utf8_casefold(trimmed, -1);
}

/*
 * Adds every venue of @venues that @wanted (folded keys and names) names
 * to @keys, once. A venue matches by its key, its whole name, or one
 * comma-separated part of its name: a connected realm is named for all of
 * its realms ("Silver Hand, Thorium Brotherhood, Farstriders"), and a
 * person means it when they say any one of them.
 */
static void
md_match_venues(
	GPtrArray	*venues,
	GHashTable	*wanted,
	GPtrArray	*keys
){
	guint i;

	for (i = 0; (NULL != venues) && (i < venues->len); i++)
	{
		VentureSeriesVenueRow *venue = g_ptr_array_index(venues, i);
		g_autofree gchar *key = md_fold(venue->key);
		g_autofree gchar *whole = md_fold(venue->name);
		g_auto(GStrv) parts = g_strsplit((NULL != venue->name) ? venue->name : "", ",", -1);
		gboolean hit;
		guint j;

		hit = g_hash_table_contains(wanted, key) || g_hash_table_contains(wanted, whole);

		for (j = 0; !hit && (NULL != parts[j]); j++)
		{
			g_autofree gchar *part = md_fold(parts[j]);

			hit = ('\0' != part[0]) && g_hash_table_contains(wanted, part);
		}

		if (hit)
			g_ptr_array_add(keys, g_strdup(venue->key));
	}
}

/*
 * The folded realm names (and realm slugs) the organization's characters
 * are on, from every push source's accounts less the ignored ones (see
 * venture-marketdata-ignore.c). A source that cannot be read
 * is skipped: a missing realm in a convenience filter is not worth failing
 * a page for.
 */
static GHashTable *
md_character_realms(
	VentureContext	*context,
	gint64		 organization_id,
	guint		*out_characters
){
	g_autoptr(GPtrArray) sources = NULL;
	g_autoptr(VentureMarketdataIgnores) ignores = NULL;
	GHashTable *realms;
	guint i;

	realms = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	*out_characters = 0;
	sources = md_sources(context, organization_id, NULL);
	ignores = venture_marketdata_ignores_load(context, organization_id);

	for (i = 0; (NULL != sources) && (i < sources->len); i++)
	{
		VentureEntity *source = g_ptr_array_index(sources, i);
		g_autofree gchar *provider = NULL;
		g_autoptr(VentureSeriesStore) reader = NULL;
		g_autoptr(GPtrArray) accounts = NULL;
		guint j;

		g_object_get(source, "provider", &provider, NULL);

		if (0 != g_strcmp0(provider, "push"))
			continue;

		reader = md_reader(context, source, NULL, NULL);
		if (NULL == reader)
			continue;

		accounts = venture_series_store_list_accounts(reader, "character", NULL, NULL, 0, NULL);

		for (j = 0; (NULL != accounts) && (j < accounts->len); j++)
		{
			VentureSeriesAccountRow *account = g_ptr_array_index(accounts, j);

			/* A character the organization ignores, or one on a realm
			 * it ignores, puts no realm in the group. */
			if (0 != venture_marketdata_ignores_match(ignores, account, NULL))
				continue;

			if (!venture_string_is_empty(account->group_key))
				g_hash_table_add(realms, md_fold(account->group_key));
			if (!venture_string_is_empty(account->venue_key))
				g_hash_table_add(realms, md_fold(account->venue_key));
			(*out_characters)++;
		}
	}

	return realms;
}

/*
 * The venue keys of @venues a venue group stands for, as a
 * NULL-terminated array; NULL when @spec asks for no group (every venue).
 * @spec is "" or NULL (none), "characters" (the realms the organization's
 * characters are on), or a venue group record's id. A group that matches
 * nothing is an empty array, which filters to nothing: an empty group
 * shows nothing rather than everything. *out_label names it for a page.
 */
static gboolean
md_venue_group_keys(
	VentureContext	 *context,
	gint64		  organization_id,
	GPtrArray	 *venues,
	const gchar	 *spec,
	JsonArray	 *notes,
	gchar		***out_keys,
	gchar		**out_label,
	GError		**error
){
	g_autoptr(GHashTable) wanted = NULL;
	g_autoptr(GPtrArray) keys = NULL;

	*out_keys = NULL;
	if (NULL != out_label)
		*out_label = NULL;

	if (venture_string_is_empty(spec))
		return TRUE;

	keys = g_ptr_array_new_with_free_func(g_free);

	if (0 == g_strcmp0(spec, MD_GROUP_CHARACTERS))
	{
		guint characters = 0;

		wanted = md_character_realms(context, organization_id, &characters);

		if ((0 == characters) && (NULL != notes))
			md_note(notes, "No characters yet: \"My characters\" is the realms a push source's "
			               "characters are on, and none has pushed any.");

		if (NULL != out_label)
			*out_label = g_strdup("My characters");
	}
	else
	{
		g_autoptr(VentureEntity) group = NULL;
		g_autofree gchar *list = NULL;
		g_auto(GStrv) entries = NULL;
		gint64 id = 0;
		guint i;

		if (!g_ascii_string_to_signed(spec, 10, 1, G_MAXINT64, &id, NULL))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "A venue group is \"%s\" or a venue group's id, not \"%s\"",
			            MD_GROUP_CHARACTERS, spec);
			return FALSE;
		}

		group = venture_database_get(venture_context_get_database(context),
		                             VENTURE_TYPE_VENUE_GROUP, id, NULL);

		if ((NULL == group) || venture_entity_is_deleted(group) ||
		    (venture_entity_get_organization_id(group) != organization_id))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
			            "No venue group #%" G_GINT64_FORMAT " in this organization", id);
			return FALSE;
		}

		g_object_get(group, "venues", &list, NULL);

		if (NULL != out_label)
			g_object_get(group, "name", out_label, NULL);

		wanted = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
		entries = g_strsplit((NULL != list) ? list : "", ",", -1);

		for (i = 0; NULL != entries[i]; i++)
		{
			gchar *folded = md_fold(entries[i]);

			if ('\0' != folded[0])
				g_hash_table_add(wanted, folded);
			else
				g_free(folded);
		}
	}

	md_match_venues(venues, wanted, keys);
	g_ptr_array_add(keys, NULL);
	*out_keys = (gchar **)g_ptr_array_free(g_steal_pointer(&keys), FALSE);
	return TRUE;
}

/*
 * The choices a venue group picker offers: none, "My characters" when a
 * push source exists, then the organization's saved groups by name.
 */
static JsonArray *
md_venue_groups_json(
	VentureContext	*context,
	gint64		 organization_id
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) groups = NULL;
	g_autoptr(GPtrArray) sources = NULL;
	JsonArray *array;
	JsonObject *object;
	gboolean pushed = FALSE;
	guint i;

	array = json_array_new();
	sources = md_sources(context, organization_id, NULL);

	for (i = 0; (NULL != sources) && (i < sources->len) && !pushed; i++)
	{
		g_autofree gchar *provider = NULL;

		g_object_get(g_ptr_array_index(sources, i), "provider", &provider, NULL);
		pushed = (0 == g_strcmp0(provider, "push"));
	}

	if (pushed)
	{
		object = json_object_new();
		json_object_set_string_member(object, "value", MD_GROUP_CHARACTERS);
		json_object_set_string_member(object, "name", "My characters");
		json_array_add_object_element(array, object);
	}

	query = venture_query_new(VENTURE_TYPE_VENUE_GROUP);
	venture_query_set_organization(query, organization_id);
	venture_query_add_order(query, "name", VENTURE_SORT_ASCENDING, NULL);
	venture_query_set_limit(query, MD_MAX_SOURCES);
	groups = venture_database_find(venture_context_get_database(context), query, NULL);

	for (i = 0; (NULL != groups) && (i < groups->len); i++)
	{
		VentureEntity *group = g_ptr_array_index(groups, i);
		g_autofree gchar *name = NULL;
		g_autofree gchar *value = g_strdup_printf("%" G_GINT64_FORMAT, venture_entity_get_id(group));

		g_object_get(group, "name", &name, NULL);
		object = json_object_new();
		json_object_set_string_member(object, "value", value);
		json_object_set_string_member(object, "name", venture_string_is_empty(name) ? value : name);
		json_array_add_object_element(array, object);
	}

	return array;
}

/* --- Connected realms ------------------------------------------------------ */

static void
md_unref_reader(gpointer reader)
{
	if (NULL != reader)
		g_object_unref(reader);
}

static void
md_unref_venues(gpointer venues)
{
	if (NULL != venues)
		g_ptr_array_unref(venues);
}

/*
 * One auction house as the sources between them know it. Sources key a
 * realm their own way: Blizzard by connected-realm id, named for all of
 * its realms ("Silver Hand, Thorium Brotherhood, Farstriders"); a TSM
 * push by one realm's slug, named for that realm alone ("Thorium
 * Brotherhood"). A connected realm shares one auction house, so every one
 * of those is the same place to buy and sell, and a picker listing them
 * separately offered four entries for one market and let a pick by one
 * member's name miss the source that only knows the connected realm.
 * Realms are matched by member name within a group (a region): a realm is
 * in exactly one connected realm of its region.
 */
typedef struct
{
	gint64	 data_source_id;
	gchar	*key;
	gint64	 last_taken_at;
} MdRealmVenue;

typedef struct
{
	gchar		*group_key;
	GPtrArray	*members;	/* gchar *, as spelt, in the connected realm's order */
	GHashTable	*alone;		/* folded members some source keeps as a venue of their own */
	GPtrArray	*venues;	/* MdRealmVenue */
	gchar		*label;
} MdRealm;

typedef struct
{
	GPtrArray	*realms;	/* MdRealm */
	GPtrArray	*pending;	/* MdPendingVenue, until md_realms_build() */
	GHashTable	*by_member;	/* "group\037folded member" -> MdRealm */
	GHashTable	*by_venue;	/* "source id\037key" -> MdRealm */
	GHashTable	*by_name;	/* folded label, name, member or key -> MdRealm */
} MdRealms;

typedef struct
{
	gint64	 data_source_id;
	gchar	*key;
	gchar	*group_key;
	gchar	**members;
	gint64	 last_taken_at;
} MdPendingVenue;

static void
md_realm_venue_free(gpointer data)
{
	MdRealmVenue *venue = data;

	g_free(venue->key);
	g_free(venue);
}

static void
md_realm_free(gpointer data)
{
	MdRealm *realm = data;

	g_free(realm->group_key);
	g_ptr_array_unref(realm->members);
	g_hash_table_unref(realm->alone);
	g_ptr_array_unref(realm->venues);
	g_free(realm->label);
	g_free(realm);
}

static void
md_pending_venue_free(gpointer data)
{
	MdPendingVenue *venue = data;

	g_free(venue->key);
	g_free(venue->group_key);
	g_strfreev(venue->members);
	g_free(venue);
}

static void
md_realms_free(MdRealms *realms)
{
	if (NULL == realms)
		return;

	g_ptr_array_unref(realms->realms);
	g_clear_pointer(&realms->pending, g_ptr_array_unref);
	g_hash_table_unref(realms->by_member);
	g_hash_table_unref(realms->by_venue);
	g_hash_table_unref(realms->by_name);
	g_free(realms);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(MdRealms, md_realms_free)

static MdRealms *
md_realms_new(void)
{
	MdRealms *realms = g_new0(MdRealms, 1);

	realms->realms = g_ptr_array_new_with_free_func(md_realm_free);
	realms->pending = g_ptr_array_new_with_free_func(md_pending_venue_free);
	realms->by_member = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	realms->by_venue = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	realms->by_name = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

	return realms;
}

/* A venue's realms: the comma-separated parts of its name, trimmed, or
 * its key when it has no name. */
static gchar **
md_realm_members(VentureSeriesVenueRow *venue)
{
	g_autoptr(GPtrArray) members = g_ptr_array_new_with_free_func(g_free);
	g_auto(GStrv) parts = NULL;
	guint i;

	parts = g_strsplit(venture_string_is_empty(venue->name) ? "" : venue->name, ",", -1);

	for (i = 0; NULL != parts[i]; i++)
	{
		g_strstrip(parts[i]);
		if ('\0' != parts[i][0])
			g_ptr_array_add(members, g_strdup(parts[i]));
	}

	if (0 == members->len)
		g_ptr_array_add(members, g_strdup(venue->key));

	g_ptr_array_add(members, NULL);
	return (gchar **)g_ptr_array_free(g_steal_pointer(&members), FALSE);
}

/* Remembers @venues (VentureSeriesVenueRow) of one source, copied. */
static void
md_realms_add(
	MdRealms	*realms,
	gint64		 data_source_id,
	GPtrArray	*venues
){
	guint i;

	for (i = 0; (NULL != venues) && (i < venues->len); i++)
	{
		VentureSeriesVenueRow *row = g_ptr_array_index(venues, i);
		MdPendingVenue *venue = g_new0(MdPendingVenue, 1);

		venue->data_source_id = data_source_id;
		venue->key = g_strdup(row->key);
		venue->group_key = g_strdup((NULL != row->group_key) ? row->group_key : "");
		venue->members = md_realm_members(row);
		venue->last_taken_at = row->last_taken_at;
		g_ptr_array_add(realms->pending, venue);
	}
}

/* Connected realms (the most members) first, so a realm known alone
 * joins the connected realm naming it rather than starting its own. */
static gint
md_pending_compare(
	gconstpointer	a,
	gconstpointer	b
){
	const MdPendingVenue *x = *(const MdPendingVenue *const *)a;
	const MdPendingVenue *y = *(const MdPendingVenue *const *)b;
	guint nx = g_strv_length(x->members);
	guint ny = g_strv_length(y->members);

	if (nx != ny)
		return (nx > ny) ? -1 : 1;

	if (x->data_source_id != y->data_source_id)
		return (x->data_source_id < y->data_source_id) ? -1 : 1;

	return g_strcmp0(x->key, y->key);
}

static gchar *
md_realm_member_key(
	const gchar	*group_key,
	const gchar	*member
){
	g_autofree gchar *folded = md_fold(member);

	return g_strdup_printf("%s\037%s", group_key, folded);
}

/* Names @realm by @text for a person's input, unless another has it. */
static void
md_realms_name(
	MdRealms	*realms,
	const gchar	*text,
	MdRealm		*realm
){
	gchar *folded = md_fold(text);

	if (('\0' == folded[0]) || g_hash_table_contains(realms->by_name, folded))
		g_free(folded);
	else
		g_hash_table_insert(realms->by_name, folded, realm);
}

/*
 * Groups what md_realms_add() was given into connected realms and labels
 * each: the realms some source keeps alone -- the ones a person plays and
 * pushes -- lead, the rest follow in brackets, so "Cenarius (+ Cairne,
 * Frostmane, ...)" is found by the name the person knows and still says
 * which market it is.
 */
static void
md_realms_build(MdRealms *realms)
{
	g_autoptr(GHashTable) labels = NULL;
	guint i;

	g_ptr_array_sort(realms->pending, md_pending_compare);

	for (i = 0; i < realms->pending->len; i++)
	{
		MdPendingVenue *venue = g_ptr_array_index(realms->pending, i);
		MdRealmVenue *place;
		MdRealm *realm = NULL;
		guint j;

		for (j = 0; (NULL == realm) && (NULL != venue->members[j]); j++)
		{
			g_autofree gchar *key = md_realm_member_key(venue->group_key, venue->members[j]);

			realm = g_hash_table_lookup(realms->by_member, key);
		}

		if (NULL == realm)
		{
			realm = g_new0(MdRealm, 1);
			realm->group_key = g_strdup(venue->group_key);
			realm->members = g_ptr_array_new_with_free_func(g_free);
			realm->alone = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
			realm->venues = g_ptr_array_new_with_free_func(md_realm_venue_free);
			g_ptr_array_add(realms->realms, realm);
		}

		for (j = 0; NULL != venue->members[j]; j++)
		{
			gchar *key = md_realm_member_key(venue->group_key, venue->members[j]);

			if (g_hash_table_contains(realms->by_member, key))
				g_free(key);
			else
			{
				g_hash_table_insert(realms->by_member, key, realm);
				g_ptr_array_add(realm->members, g_strdup(venue->members[j]));
			}
		}

		if (1 == g_strv_length(venue->members))
			g_hash_table_add(realm->alone, md_fold(venue->members[0]));

		place = g_new0(MdRealmVenue, 1);
		place->data_source_id = venue->data_source_id;
		place->key = g_strdup(venue->key);
		place->last_taken_at = venue->last_taken_at;
		g_ptr_array_add(realm->venues, place);
		g_hash_table_insert(realms->by_venue,
		                    g_strdup_printf("%" G_GINT64_FORMAT "\037%s", venue->data_source_id, venue->key),
		                    realm);
	}

	labels = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

	for (i = 0; i < realms->realms->len; i++)
	{
		MdRealm *realm = g_ptr_array_index(realms->realms, i);
		g_autoptr(GString) lead = g_string_new(NULL);
		g_autoptr(GString) rest = g_string_new(NULL);
		gboolean any_alone = FALSE;
		gchar *folded_label;
		guint j;

		for (j = 0; j < realm->members->len; j++)
		{
			g_autofree gchar *folded = md_fold(g_ptr_array_index(realm->members, j));

			any_alone = any_alone || g_hash_table_contains(realm->alone, folded);
		}

		for (j = 0; j < realm->members->len; j++)
		{
			const gchar *member = g_ptr_array_index(realm->members, j);
			g_autofree gchar *folded = md_fold(member);
			/* Nobody keeps one alone: the connected realm's first leads. */
			gboolean leads = any_alone ? g_hash_table_contains(realm->alone, folded) : (0 == j);
			GString *into = leads ? lead : rest;

			g_string_append_printf(into, "%s%s", (into->len > 0) ? ", " : "", member);
		}

		realm->label = (rest->len > 0) ? g_strdup_printf("%s (+ %s)", lead->str, rest->str)
		                               : g_strdup(lead->str);
		folded_label = md_fold(realm->label);
		g_hash_table_replace(labels, folded_label,
		                     GUINT_TO_POINTER(GPOINTER_TO_UINT(g_hash_table_lookup(labels, folded_label)) + 1));
	}

	/* The same realm name in two regions: say which is which. */
	for (i = 0; i < realms->realms->len; i++)
	{
		MdRealm *realm = g_ptr_array_index(realms->realms, i);
		g_autofree gchar *folded = md_fold(realm->label);

		if ((GPOINTER_TO_UINT(g_hash_table_lookup(labels, folded)) > 1) &&
		    !venture_string_is_empty(realm->group_key))
		{
			g_autofree gchar *group = g_ascii_strup(realm->group_key, -1);
			gchar *label = g_strdup_printf("%s [%s]", realm->label, group);

			g_free(realm->label);
			realm->label = label;
		}
	}

	/* What a person may type for one: its label first, then any member
	 * realm's name, then a source's whole name or key for it. */
	for (i = 0; i < realms->realms->len; i++)
	{
		MdRealm *realm = g_ptr_array_index(realms->realms, i);

		md_realms_name(realms, realm->label, realm);
	}

	for (i = 0; i < realms->pending->len; i++)
	{
		MdPendingVenue *venue = g_ptr_array_index(realms->pending, i);
		g_autofree gchar *key = g_strdup_printf("%" G_GINT64_FORMAT "\037%s", venue->data_source_id,
		                                        venue->key);
		MdRealm *realm = g_hash_table_lookup(realms->by_venue, key);
		g_autofree gchar *whole = g_strjoinv(", ", venue->members);
		guint j;

		for (j = 0; NULL != venue->members[j]; j++)
			md_realms_name(realms, venue->members[j], realm);
		md_realms_name(realms, whole, realm);
	}

	for (i = 0; i < realms->pending->len; i++)
	{
		MdPendingVenue *venue = g_ptr_array_index(realms->pending, i);
		g_autofree gchar *key = g_strdup_printf("%" G_GINT64_FORMAT "\037%s", venue->data_source_id,
		                                        venue->key);

		md_realms_name(realms, venue->key, g_hash_table_lookup(realms->by_venue, key));
	}

	g_clear_pointer(&realms->pending, g_ptr_array_unref);
}

/* The connected realm @wanted names (a label, a realm's name, a source's
 * whole name or key for it), or NULL. */
static MdRealm *
md_realms_find(
	MdRealms	*realms,
	const gchar	*wanted
){
	g_autofree gchar *folded = md_fold(wanted);

	return g_hash_table_lookup(realms->by_name, folded);
}

/* The connected realm one source's venue belongs to, or NULL. */
static MdRealm *
md_realms_of(
	MdRealms	*realms,
	gint64		 data_source_id,
	const gchar	*key
){
	g_autofree gchar *lookup = g_strdup_printf("%" G_GINT64_FORMAT "\037%s", data_source_id, key);

	return g_hash_table_lookup(realms->by_venue, lookup);
}

/* @realm's venues in one source, as a NULL-terminated array: NULL when
 * that source has none of it. */
static gchar **
md_realm_keys(
	MdRealm		*realm,
	gint64		 data_source_id
){
	g_autoptr(GPtrArray) keys = g_ptr_array_new_with_free_func(g_free);
	guint i;

	for (i = 0; (NULL != realm) && (i < realm->venues->len); i++)
	{
		MdRealmVenue *venue = g_ptr_array_index(realm->venues, i);

		if (venue->data_source_id == data_source_id)
			g_ptr_array_add(keys, g_strdup(venue->key));
	}

	if (0 == keys->len)
		return NULL;

	g_ptr_array_add(keys, NULL);
	return (gchar **)g_ptr_array_free(g_steal_pointer(&keys), FALSE);
}

/*
 * The connected realms a venue group names, in any source: a group
 * naming "Farstriders" means the connected realm one source keys as "12"
 * and another keeps as "silver-hand" and "thorium-brotherhood", though
 * only the first has the name. *out_realms is NULL when @spec asks for no
 * group. @venue_lists holds each of @sources' venues, NULL where a store
 * could not be read.
 */
static gboolean
md_realms_of_group(
	VentureContext	 *context,
	gint64		  organization_id,
	MdRealms	 *realms,
	GPtrArray	 *sources,
	GPtrArray	 *venue_lists,
	const gchar	 *spec,
	JsonArray	 *notes,
	GHashTable	**out_realms,
	gchar		**out_label,
	GError		**error
){
	g_autoptr(GHashTable) named = NULL;
	guint i;

	*out_realms = NULL;
	if (NULL != out_label)
		*out_label = NULL;

	if (venture_string_is_empty(spec))
		return TRUE;

	named = g_hash_table_new(g_direct_hash, g_direct_equal);

	for (i = 0; (NULL != sources) && (i < sources->len); i++)
	{
		gint64 source_id = venture_entity_get_id(g_ptr_array_index(sources, i));
		g_auto(GStrv) keys = NULL;
		g_autofree gchar *label = NULL;
		guint j;

		if (!md_venue_group_keys(context, organization_id, g_ptr_array_index(venue_lists, i), spec,
		                         (0 == i) ? notes : NULL, &keys, &label, error))
			return FALSE;

		if ((NULL != out_label) && (NULL == *out_label))
			*out_label = g_steal_pointer(&label);

		for (j = 0; (NULL != keys) && (NULL != keys[j]); j++)
		{
			MdRealm *realm = md_realms_of(realms, source_id, keys[j]);

			if (NULL != realm)
				g_hash_table_add(named, realm);
		}
	}

	*out_realms = g_steal_pointer(&named);
	return TRUE;
}

/* One source's venue keys in @named (MdRealm set) as a NULL-terminated
 * array, empty when it has none of them; NULL for no group. */
static gchar **
md_realms_group_keys(
	GHashTable	*named,
	gint64		 data_source_id
){
	g_autoptr(GPtrArray) keys = NULL;
	GHashTableIter iter;
	gpointer realm;

	if (NULL == named)
		return NULL;

	keys = g_ptr_array_new_with_free_func(g_free);
	g_hash_table_iter_init(&iter, named);

	while (g_hash_table_iter_next(&iter, &realm, NULL))
	{
		guint i;

		for (i = 0; i < ((MdRealm *)realm)->venues->len; i++)
		{
			MdRealmVenue *venue = g_ptr_array_index(((MdRealm *)realm)->venues, i);

			if (venue->data_source_id == data_source_id)
				g_ptr_array_add(keys, g_strdup(venue->key));
		}
	}

	g_ptr_array_add(keys, NULL);
	return (gchar **)g_ptr_array_free(g_steal_pointer(&keys), FALSE);
}

/* The label of the connected realm a source's venue is in, as the
 * pickers spell it, so a row and the picker that chose it agree; NULL for
 * none. Borrowed from @realms. */
static const gchar *
md_realm_label(
	MdRealms	*realms,
	gint64		 data_source_id,
	const gchar	*venue_key
){
	MdRealm *realm = md_realms_of(realms, data_source_id, venue_key);

	return (NULL != realm) ? realm->label : NULL;
}

/* A picker's choice for @realm: {name, value, members, venues:
 * [{data_source_id, venue_key, taken_at, age_seconds, stale}]}. */
static JsonObject *
md_realm_choice(
	MdRealm	*realm,
	gint64	 now,
	gint64	 stale_after
){
	JsonObject *object = json_object_new();
	JsonArray *members = json_array_new();
	JsonArray *venues = json_array_new();
	guint i;

	json_object_set_string_member(object, "name", realm->label);
	json_object_set_string_member(object, "value", realm->label);
	json_object_set_string_member(object, "group_key", realm->group_key);

	for (i = 0; i < realm->members->len; i++)
		json_array_add_string_element(members, g_ptr_array_index(realm->members, i));

	for (i = 0; i < realm->venues->len; i++)
	{
		MdRealmVenue *venue = g_ptr_array_index(realm->venues, i);
		JsonObject *one = json_object_new();

		json_object_set_int_member(one, "data_source_id", venue->data_source_id);
		json_object_set_string_member(one, "venue_key", venue->key);
		md_set_age(one, "", venue->last_taken_at, now, stale_after);
		json_array_add_object_element(venues, one);
	}

	json_object_set_array_member(object, "members", members);
	json_object_set_array_member(object, "venues", venues);

	return object;
}

#endif /* VENTURE_HAVE_SQLITE */

JsonNode *
venture_marketdata_venue_group_choices(
	VentureContext	*context,
	gint64		 organization_id
){
	JsonNode *node;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	node = json_node_new(JSON_NODE_ARRAY);
#ifdef VENTURE_HAVE_SQLITE
	/* The groups' choices read the data sources, whose table need not
	 * exist with feeds off. */
	if (venture_context_module_enabled(context, "feeds"))
	{
		json_node_take_array(node, md_venue_groups_json(context, organization_id));
		return node;
	}
#endif
	(void)organization_id;
	json_node_take_array(node, json_array_new());
	return node;
}

gboolean
venture_marketdata_venue_group_venue_keys(
	VentureContext	 *context,
	gint64		  organization_id,
	const gchar	 *spec,
	gchar		***out_keys,
	gchar		**out_label,
	GError		**error
){
	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), FALSE);
	g_return_val_if_fail(NULL != out_keys, FALSE);

	*out_keys = NULL;
	if (NULL != out_label)
		*out_label = NULL;

	if (venture_string_is_empty(spec))
		return TRUE;

#ifdef VENTURE_HAVE_SQLITE
	{
		g_autoptr(GPtrArray) sources = NULL;
		g_autoptr(GPtrArray) venue_lists = NULL;
		g_autoptr(MdRealms) realms = NULL;
		g_autoptr(GPtrArray) keys = NULL;
		g_autoptr(GHashTable) seen = NULL;
		g_autoptr(GHashTable) named = NULL;
		guint i;

		/* A group with feeds off names nothing it could be judged by. */
		sources = venture_context_module_enabled(context, "feeds")
			? md_sources(context, organization_id, NULL) : NULL;
		venue_lists = g_ptr_array_new_with_free_func(md_unref_venues);
		realms = md_realms_new();
		keys = g_ptr_array_new_with_free_func(g_free);
		seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

		for (i = 0; (NULL != sources) && (i < sources->len); i++)
		{
			g_autoptr(VentureSeriesStore) reader = md_reader(context, g_ptr_array_index(sources, i), NULL, NULL);
			GPtrArray *venues = (NULL != reader) ? venture_series_store_list_venues(reader, NULL) : NULL;

			g_ptr_array_add(venue_lists, venues);
			md_realms_add(realms, venture_entity_get_id(g_ptr_array_index(sources, i)), venues);
		}

		md_realms_build(realms);

		if (!md_realms_of_group(context, organization_id, realms, sources, venue_lists, spec, NULL,
		                        &named, out_label, error))
			return FALSE;

		for (i = 0; (NULL != sources) && (i < sources->len); i++)
		{
			g_auto(GStrv) group_keys = md_realms_group_keys(named,
			                                                venture_entity_get_id(g_ptr_array_index(sources, i)));
			guint j;

			for (j = 0; (NULL != group_keys) && (NULL != group_keys[j]); j++)
				if (g_hash_table_add(seen, g_strdup(group_keys[j])))
					g_ptr_array_add(keys, g_strdup(group_keys[j]));
		}

		/* Judged once more with no store at all, so a group that does
		 * not exist is refused even before any source has data. */
		if ((NULL == sources) || (0 == sources->len))
		{
			g_auto(GStrv) none = NULL;

			/* "characters" reads the data sources, whose table need
			 * not exist with feeds off; it names nothing then. */
			if (!venture_context_module_enabled(context, "feeds") &&
			    (0 == g_strcmp0(spec, MD_GROUP_CHARACTERS)))
			{
				if (NULL != out_label)
					*out_label = g_strdup("My characters");
			}
			else if (!md_venue_group_keys(context, organization_id, NULL, spec, NULL, &none,
			                              out_label, error))
				return FALSE;
		}

		g_ptr_array_add(keys, NULL);
		*out_keys = (gchar **)g_ptr_array_free(g_steal_pointer(&keys), FALSE);
	}
#else
	(void)organization_id;
	(void)error;
	*out_keys = g_new0(gchar *, 1);
#endif

	return TRUE;
}

/* --- A row's last seven days ----------------------------------------------- */

#ifdef VENTURE_HAVE_SQLITE

/*
 * Whether a price is a dip or the new normal, beside each row of a table:
 * the buy venue's hourly lowest prices over seven days, as a sparkline of
 * four-hour blocks and a median to compare the price with. Twelve hours
 * is the least a median is taken from: a row first seen this morning has
 * no "normal" yet, and a median of three hours would call its first
 * undercut a dip.
 */
#define MD_RECENT_BLOCK (4 * 3600)
#define MD_RECENT_BLOCKS (42)
#define MD_RECENT_MIN_HOURS (12)

/* The start of the oldest four-hour block a row's last seven days hold. */
static gint64
md_recent_first(gint64 now)
{
	gint64 last;

	last = now - (((now % MD_RECENT_BLOCK) + MD_RECENT_BLOCK) % MD_RECENT_BLOCK);

	return last - (MD_RECENT_BLOCKS - 1) * (gint64)MD_RECENT_BLOCK;
}

/* Sets trend_7d, median_7d, median_7d_hours and vs_median_7d_pct on
 * @object from @points, the row's hourly points from md_recent_first(). */
static void
md_recent_set(
	JsonObject		*object,
	GArray			*points,
	const VentureSeriesRow	*row,
	gint64			 now
){
	g_autoptr(GArray) prices = NULL;
	gint64 lows[MD_RECENT_BLOCKS];
	gint64 first;
	gint64 median;
	JsonArray *trend;
	guint i;

	first = md_recent_first(now);
	prices = g_array_new(FALSE, FALSE, sizeof(gint64));

	for (i = 0; i < MD_RECENT_BLOCKS; i++)
		lows[i] = VENTURE_SERIES_NONE;

	for (i = 0; (NULL != points) && (i < points->len); i++)
	{
		const VentureSeriesPoint *point = &g_array_index(points, VentureSeriesPoint, i);
		guint block;

		/* Another currency's price is not a price in this one. */
		if ((VENTURE_SERIES_NONE == point->min_price) || (point->at < first) || (point->at > now) ||
		    (0 != g_strcmp0(point->currency, row->currency)))
			continue;

		block = (guint)((point->at - first) / MD_RECENT_BLOCK);

		if ((VENTURE_SERIES_NONE == lows[block]) || (point->min_price < lows[block]))
			lows[block] = point->min_price;

		g_array_append_val(prices, point->min_price);
	}

	trend = json_array_new();

	for (i = 0; i < MD_RECENT_BLOCKS; i++)
	{
		if (VENTURE_SERIES_NONE == lows[i])
			json_array_add_null_element(trend);
		else
			json_array_add_int_element(trend, lows[i]);
	}

	json_object_set_array_member(object, "trend_7d", trend);
	json_object_set_int_member(object, "median_7d_hours", prices->len);

	median = (prices->len >= MD_RECENT_MIN_HOURS)
		? venture_series_math_median((const gint64 *)(gpointer)prices->data, prices->len)
		: VENTURE_SERIES_NONE;

	md_set_money(object, "median_7d", median, row->currency);

	/* A ratio for reading, not money: the double is never added up. */
	if ((VENTURE_SERIES_NONE != median) && (median > 0) && (VENTURE_SERIES_NONE != row->min_price))
		md_set_ratio(object, "vs_median_7d_pct",
		             100.0 * ((gdouble)row->min_price - (gdouble)median) / (gdouble)median);
	else
		json_object_set_null_member(object, "vs_median_7d_pct");
}

/*
 * The last seven days of @n rows of one store, in one statement: a table
 * of fifty rows is one read, not fifty. @objects[i] is the answer's object
 * for @rows[i].
 */
static gboolean
md_recent_rows(
	VentureSeriesStore	 *reader,
	VentureSeriesRow	**rows,
	JsonObject		**objects,
	guint			  n,
	gint64			  now,
	GError			**error
){
	g_autofree VentureSeriesPair *pairs = NULL;
	g_autoptr(GPtrArray) points = NULL;
	guint i;

	if (0 == n)
		return TRUE;

	pairs = g_new0(VentureSeriesPair, n);

	for (i = 0; i < n; i++)
	{
		pairs[i].venue_key = rows[i]->venue_key;
		pairs[i].instrument_key = rows[i]->instrument_key;
	}

	points = venture_series_store_hourly_many(reader, pairs, n, md_recent_first(now), error);

	if (NULL == points)
		return FALSE;

	for (i = 0; i < n; i++)
		md_recent_set(objects[i], g_ptr_array_index(points, i), rows[i], now);

	return TRUE;
}

#endif /* VENTURE_HAVE_SQLITE */

/* --- Browse ---------------------------------------------------------------- */

void
venture_marketdata_browse_query_init(VentureMarketdataBrowseQuery *query)
{
	g_return_if_fail(NULL != query);

	memset(query, 0, sizeof(*query));
	query->page = 1;
}

const gchar *const *
venture_marketdata_browse_sorts(void)
{
	return md_browse_sorts;
}

/* An echo of the question, so a page can draw its controls from it. */
static JsonObject *
md_browse_echo(const VentureMarketdataBrowseQuery *query)
{
	JsonObject *object;

	object = json_object_new();
	md_set_text(object, "search", query->search);
	md_set_text(object, "category", query->category);
	md_set_text(object, "venue", query->venue);
	md_set_text(object, "group_key", query->group_key);
	md_set_text(object, "venue_group", query->venue_group);
	json_object_set_string_member(object, "sort", (NULL != query->sort) ? query->sort : "min_price");
	json_object_set_boolean_member(object, "descending", query->descending);
	json_object_set_boolean_member(object, "in_stock_only", query->in_stock_only);

	return object;
}

JsonNode *
venture_marketdata_browse(
	VentureContext				 *context,
	const VentureMarketdataBrowseQuery	 *query,
	GError					**error
){
	JsonObject *root;
	JsonArray *notes;
	JsonArray *rows;
	guint page;
	guint per_page;
	gint64 total;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	g_return_val_if_fail(NULL != query, NULL);

	if (!md_require_module(context, error))
		return NULL;

	/* The sort is an allowlist: a name in a query string never reaches
	 * the store as anything but one of these. */
	if ((NULL != query->sort) && !g_strv_contains(md_browse_sorts, query->sort))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "\"%s\" is not a column browse sorts by", query->sort);
		return NULL;
	}

	per_page = (0 == query->per_page) ? 50 : query->per_page;
	page = (0 == query->page) ? 1 : query->page;

	if ((per_page > VENTURE_MARKETDATA_BROWSE_MAX_PAGE) || (page > MD_MAX_PAGE_NUMBER))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "A browse page holds at most %d rows, and there are at most %d pages",
		            VENTURE_MARKETDATA_BROWSE_MAX_PAGE, MD_MAX_PAGE_NUMBER);
		return NULL;
	}

	root = json_object_new();
	notes = json_array_new();
	rows = json_array_new();
	total = 0;
	json_object_set_object_member(root, "query", md_browse_echo(query));
	json_object_set_array_member(root, "sorts", json_array_new());

	{
		JsonArray *sorts = json_object_get_array_member(root, "sorts");
		guint i;

		for (i = 0; NULL != md_browse_sorts[i]; i++)
			json_array_add_string_element(sorts, md_browse_sorts[i]);
	}

	json_object_set_boolean_member(root, "available", FALSE);
	json_object_set_int_member(root, "data_source_id", 0);

	if (md_series_ready(context, notes))
	{
#ifdef VENTURE_HAVE_SQLITE
		g_autoptr(GPtrArray) sources = NULL;
		g_autoptr(VentureEntity) chosen = NULL;
		g_autoptr(VentureSeriesStore) reader = NULL;
		g_autoptr(GPtrArray) venues = NULL;
		g_autoptr(GPtrArray) found = NULL;
		g_autoptr(GError) local_error = NULL;
		const gchar *venue_keys[2];
		VentureSeriesFilter filter;
		JsonArray *groups;
		guint i;

		sources = md_sources(context, query->organization_id, error);

		if (NULL == sources)
		{
			json_object_unref(root);
			json_array_unref(notes);
			json_array_unref(rows);
			return NULL;
		}

		json_object_set_array_member(root, "sources", md_sources_json(sources));

		if (query->data_source_id > 0)
		{
			chosen = md_source(context, query->organization_id, query->data_source_id, error);

			if (NULL == chosen)
			{
				json_object_unref(root);
				json_array_unref(notes);
				json_array_unref(rows);
				return NULL;
			}
		}
		else if (sources->len > 0)
			chosen = g_object_ref(g_ptr_array_index(sources, 0));
		else
			md_note(notes, "No data sources yet: add one under Market data feeds.");

		if (NULL != chosen)
		{
			json_object_set_int_member(root, "data_source_id", venture_entity_get_id(chosen));
			reader = md_reader(context, chosen, notes, NULL);
		}

		groups = json_array_new();

		json_object_set_array_member(root, "venue_groups",
		                             md_venue_groups_json(context, query->organization_id));

		if (NULL != reader)
		{
			g_auto(GStrv) group_keys = NULL;
			g_autofree gchar *group_label = NULL;

			venues = venture_series_store_list_venues(reader, &local_error);

			{
				g_autoptr(GHashTable) counts = md_categories_new();

				md_categories_add(counts, reader);
				json_object_set_array_member(root, "categories", md_categories_json(counts));
			}

			venture_series_filter_init(&filter);

			if (!venture_string_is_empty(query->venue))
			{
				venue_keys[0] = query->venue;
				venue_keys[1] = NULL;
				filter.venue_keys = venue_keys;
			}
			else if (!md_venue_group_keys(context, query->organization_id, venues,
			                              query->venue_group, notes, &group_keys,
			                              &group_label, error))
			{
				json_object_unref(root);
				json_array_unref(notes);
				json_array_unref(rows);
				json_array_unref(groups);
				return NULL;
			}
			else if (NULL != group_keys)
			{
				filter.venue_keys = (const gchar **)group_keys;
				md_set_text(root, "venue_group_name", group_label);
				json_object_set_int_member(root, "venue_group_venues", g_strv_length(group_keys));
			}

			filter.group_key = venture_string_is_empty(query->group_key) ? NULL : query->group_key;
			filter.search = venture_string_is_empty(query->search) ? NULL : query->search;
			filter.category_prefix = venture_string_is_empty(query->category) ? NULL : query->category;
			filter.in_stock_only = query->in_stock_only;
			filter.descending = query->descending;
			filter.offset = (page - 1) * per_page;
			filter.count = per_page;

			if (!venture_series_sort_from_string((NULL != query->sort) ? query->sort : "min_price",
			                                     &filter.sort))
				filter.sort = VENTURE_SERIES_SORT_MIN_PRICE;

			if ((NULL != venues) &&
			    venture_series_store_count_current(reader, &filter, &total, &local_error))
				found = venture_series_store_list_current(reader, &filter, &local_error);

			if (NULL == found)
			{
				/* A store that refuses the question is a refusal, said
				 * with the store's reason (a bad search's UTF-8, say). */
				json_object_unref(root);
				json_array_unref(notes);
				json_array_unref(rows);
				json_array_unref(groups);
				g_propagate_error(error, g_steal_pointer(&local_error));
				return NULL;
			}

			json_object_set_boolean_member(root, "available", TRUE);

			{
				g_autofree JsonObject **objects = g_new0(JsonObject *, found->len + 1);

				for (i = 0; i < found->len; i++)
				{
					VentureSeriesRow *one = g_ptr_array_index(found, i);
					JsonObject *object = md_row_object(one, venture_entity_get_id(chosen));

					md_row_set_display(object, reader, one->instrument_key);
					json_array_add_object_element(rows, object);
					objects[i] = object;
				}

				/* The page's rows' last seven days, in one read. */
				if (!md_recent_rows(reader, (VentureSeriesRow **)found->pdata, objects, found->len,
				                    md_now(query->now), error))
				{
					json_object_unref(root);
					json_array_unref(notes);
					json_array_unref(rows);
					json_array_unref(groups);
					return NULL;
				}
			}
		}

		json_object_set_array_member(root, "venues", md_venues_json(venues, groups));
		json_object_set_array_member(root, "groups", groups);
#endif
	}

	if (!json_object_has_member(root, "sources"))
		json_object_set_array_member(root, "sources", json_array_new());
	if (!json_object_has_member(root, "venues"))
		json_object_set_array_member(root, "venues", json_array_new());
	if (!json_object_has_member(root, "groups"))
		json_object_set_array_member(root, "groups", json_array_new());

	json_object_set_int_member(root, "page", page);
	json_object_set_int_member(root, "per_page", per_page);
	json_object_set_int_member(root, "total", total);
	json_object_set_int_member(root, "pages", (total + per_page - 1) / per_page);
	json_object_set_array_member(root, "rows", rows);
	json_object_set_array_member(root, "notes", notes);
	md_attribute_source(context, query->organization_id, root);

	return md_node(root);
}

/* --- An instrument --------------------------------------------------------- */

#ifdef VENTURE_HAVE_SQLITE

/* The configured zone's offset from UTC at @now, in seconds. */
static gint64
md_zone_offset(
	VentureContext	*context,
	gint64		 now,
	gchar		**out_name
){
	GTimeZone *zone;
	gint interval;

	zone = venture_context_get_timezone(context);

	if (NULL == zone)
	{
		*out_name = g_strdup("UTC");
		return 0;
	}

	*out_name = g_strdup(g_time_zone_get_identifier(zone));
	interval = g_time_zone_find_interval(zone, G_TIME_TYPE_UNIVERSAL, now);

	if (interval < 0)
		return 0;

	return (gint64)g_time_zone_get_offset(zone, interval);
}

/* A 7 x 24 matrix as an array of arrays, NONE as null. */
static JsonArray *
md_heat_matrix(const VentureSeriesHeat *heat)
{
	JsonArray *days;
	guint d;
	guint h;

	days = json_array_new();

	for (d = 0; d < 7; d++)
	{
		JsonArray *hours = json_array_new();

		for (h = 0; h < 24; h++)
		{
			if (VENTURE_SERIES_NONE == heat->value[d][h])
				json_array_add_null_element(hours);
			else
				json_array_add_int_element(hours, heat->value[d][h]);
		}

		json_array_add_array_element(days, hours);
	}

	return days;
}

/*
 * The heat map of the last week: lowest price and quantity by weekday and
 * hour in the configured zone, from the hourly points in the newest
 * point's currency (a price map across two currencies would average two
 * units). The store's own heat reads only the price; the quantity is the
 * same arithmetic on the same points.
 */
static JsonObject *
md_heat(
	VentureContext	 *context,
	GArray		 *points,
	gint64		  now,
	GError		**error
){
	g_autoptr(GArray) times = NULL;
	g_autoptr(GArray) prices = NULL;
	g_autoptr(GArray) quantities = NULL;
	g_autofree gchar *zone_name = NULL;
	VentureSeriesHeat price_heat;
	VentureSeriesHeat quantity_heat;
	const gchar *currency;
	JsonObject *object;
	gint64 offset;
	gint64 since;
	guint i;
	static const gchar *const weekdays[] = {
		"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"
	};

	offset = md_zone_offset(context, now, &zone_name);
	since = now - MD_HEAT_DAYS * MD_DAY;
	currency = (points->len > 0)
		? g_array_index(points, VentureSeriesPoint, points->len - 1).currency : "";
	times = g_array_new(FALSE, FALSE, sizeof(gint64));
	prices = g_array_new(FALSE, FALSE, sizeof(gint64));
	quantities = g_array_new(FALSE, FALSE, sizeof(gint64));

	for (i = 0; i < points->len; i++)
	{
		const VentureSeriesPoint *point = &g_array_index(points, VentureSeriesPoint, i);

		if ((point->at < since) || (0 != g_strcmp0(point->currency, currency)))
			continue;

		g_array_append_val(times, point->at);
		g_array_append_val(prices, point->min_price);
		g_array_append_val(quantities, point->quantity);
	}

	if (!venture_series_math_heat((const gint64 *)(gpointer)times->data,
	                              (const gint64 *)(gpointer)prices->data, times->len, offset,
	                              &price_heat, error) ||
	    !venture_series_math_heat((const gint64 *)(gpointer)times->data,
	                              (const gint64 *)(gpointer)quantities->data, times->len, offset,
	                              &quantity_heat, error))
		return NULL;

	object = json_object_new();
	json_object_set_string_member(object, "zone", (NULL != zone_name) ? zone_name : "UTC");
	json_object_set_int_member(object, "zone_offset", offset);
	md_set_time(object, "since", since);
	json_object_set_string_member(object, "currency", currency);
	json_object_set_int_member(object, "samples", times->len);
	json_object_set_array_member(object, "weekdays", json_array_new());

	for (i = 0; i < G_N_ELEMENTS(weekdays); i++)
		json_array_add_string_element(json_object_get_array_member(object, "weekdays"), weekdays[i]);

	json_object_set_array_member(object, "price", md_heat_matrix(&price_heat));
	json_object_set_array_member(object, "quantity", md_heat_matrix(&quantity_heat));

	return object;
}

static gint
md_compare_gint64(
	gconstpointer	a,
	gconstpointer	b
){
	gint64 x = *(const gint64 *)a;
	gint64 y = *(const gint64 *)b;

	return (x < y) ? -1 : ((x > y) ? 1 : 0);
}

/*
 * The venue's median over two weeks: the median of each day's lowest
 * price in the row's currency, over the days that had one. A single
 * figure that a run of cheap hours cannot drag around the way today's
 * lowest can.
 */
static gint64
md_median_days(
	GArray		*days,
	const gchar	*currency,
	gint64		 since
){
	g_autoptr(GArray) mins = NULL;
	guint i;

	mins = g_array_new(FALSE, FALSE, sizeof(gint64));

	for (i = 0; i < days->len; i++)
	{
		const VentureSeriesDay *day = &g_array_index(days, VentureSeriesDay, i);

		if ((day->day_start < since) || (VENTURE_SERIES_NONE == day->min_price) ||
		    (0 != g_strcmp0(day->currency, currency)))
			continue;

		g_array_append_val(mins, day->min_price);
	}

	if (0 == mins->len)
		return VENTURE_SERIES_NONE;

	g_array_sort(mins, md_compare_gint64);

	return venture_series_math_median((const gint64 *)(gpointer)mins->data, mins->len);
}

/* The promoted record for a store key, or 0. */
static gint64
md_promoted_id(
	VentureContext	*context,
	GType		 type,
	gint64		 organization_id,
	const gchar	*row_namespace,
	const gchar	*source_namespace,
	const gchar	*key
){
	g_autoptr(VentureEntity) record = NULL;
	g_autofree gchar *ref = NULL;

	ref = venture_marketdata_external_ref(!venture_string_is_empty(row_namespace)
	                                      ? row_namespace : source_namespace, key);

	if (NULL == ref)
		return 0;

	record = venture_marketdata_find_by_ref(venture_context_get_database(context), type,
	                                        organization_id, ref, NULL);

	if ((NULL == record) || venture_entity_is_deleted(record))
		return 0;

	return venture_entity_get_id(record);
}

static JsonArray *
md_watchlists_brief(
	VentureContext	*context,
	gint64		 organization_id
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) lists = NULL;
	JsonArray *array;
	guint i;

	array = json_array_new();
	query = venture_query_new(VENTURE_TYPE_WATCHLIST);
	venture_query_set_organization(query, organization_id);
	venture_query_add_order(query, "name", VENTURE_SORT_ASCENDING, NULL);
	venture_query_set_limit(query, MD_MAX_SOURCES);
	lists = venture_database_find(venture_context_get_database(context), query, NULL);

	for (i = 0; (NULL != lists) && (i < lists->len); i++)
	{
		VentureEntity *list = g_ptr_array_index(lists, i);
		g_autofree gchar *name = NULL;
		JsonObject *object;

		g_object_get(list, "name", &name, NULL);
		object = json_object_new();
		json_object_set_int_member(object, "id", venture_entity_get_id(list));
		json_object_set_string_member(object, "name", (NULL != name) ? name : "");
		json_array_add_object_element(array, object);
	}

	return array;
}

/* --- Another source's view of an item -------------------------------------- */

/*
 * What a second source says of the same item: TSM's AuctionDB, as tsmctl
 * pushes it, beside Blizzard's live listings. Two stores, so two key
 * spaces joined only where both say they are the same namespace
 * (instrument-namespace, wow-item): an item key means nothing across
 * namespaces, and a match there would read as a price. Venues are joined
 * as the pickers join them (MdRealms): the region by its venue's
 * convention (venture_series_region_venue_key()), a realm by its
 * connected realm, whichever source keys it how.
 */
typedef struct
{
	gint64			 id;
	gchar			*name;
	gchar			*instrument_namespace;	/* "" for none */
	VentureSeriesStore	*reader;		/* NULL when unreadable */
} MdRefSource;

typedef struct
{
	GPtrArray	*sources;	/* MdRefSource, the organization's by name */
	MdRealms	*realms;
} MdRefs;

static void
md_ref_source_free(gpointer data)
{
	MdRefSource *source = data;

	g_free(source->name);
	g_free(source->instrument_namespace);
	g_clear_object(&source->reader);
	g_free(source);
}

static void
md_refs_free(MdRefs *refs)
{
	if (NULL == refs)
		return;

	g_ptr_array_unref(refs->sources);
	md_realms_free(refs->realms);
	g_free(refs);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(MdRefs, md_refs_free)

/* Every source of the organization, readable or not, and their realms.
 * A store that cannot be read has nothing to compare with; it is not a
 * failure of the page asking. */
static MdRefs *
md_refs_new(
	VentureContext	*context,
	gint64		 organization_id
){
	g_autoptr(GPtrArray) sources = md_sources(context, organization_id, NULL);
	MdRefs *refs = g_new0(MdRefs, 1);
	guint i;

	refs->sources = g_ptr_array_new_with_free_func(md_ref_source_free);
	refs->realms = md_realms_new();

	for (i = 0; (NULL != sources) && (i < sources->len); i++)
	{
		VentureEntity *entity = g_ptr_array_index(sources, i);
		MdRefSource *source = g_new0(MdRefSource, 1);
		g_autoptr(GPtrArray) venues = NULL;
		gchar *ns = NULL;

		g_object_get(entity, "instrument-namespace", &ns, NULL);
		source->id = venture_entity_get_id(entity);
		source->name = md_source_name(entity);
		source->instrument_namespace = (NULL != ns) ? ns : g_strdup("");
		source->reader = md_reader(context, entity, NULL, NULL);

		if (NULL != source->reader)
			venues = venture_series_store_list_venues(source->reader, NULL);

		md_realms_add(refs->realms, source->id, venues);
		g_ptr_array_add(refs->sources, source);
	}

	md_realms_build(refs->realms);

	return refs;
}

static MdRefSource *
md_refs_find(
	MdRefs	*refs,
	gint64	 id
){
	guint i;

	for (i = 0; i < refs->sources->len; i++)
	{
		MdRefSource *source = g_ptr_array_index(refs->sources, i);

		if (source->id == id)
			return source;
	}

	return NULL;
}

/* Whether @row is a figure to compare with: in @currency, with a market
 * value or the source's historical price. */
static gboolean
md_ref_row_usable(
	const VentureSeriesRow	*row,
	const gchar		*currency
){
	return (NULL != row) && (0 == g_strcmp0(row->currency, currency)) &&
	       ((VENTURE_SERIES_NONE != row->market_value) || (VENTURE_SERIES_NONE != row->source_historical));
}

/* @source's row for @row's instrument at @row's connected realm, the
 * newest of its member realms' when it keeps several apart (one auction
 * house, so one market); NULL when it has none. Transfer full. */
static VentureSeriesRow *
md_ref_realm_row(
	MdRefs			*refs,
	MdRefSource		*source,
	gint64			 own_id,
	const VentureSeriesRow	*row
){
	g_auto(GStrv) keys = NULL;
	VentureSeriesRow *best = NULL;
	guint i;

	keys = md_realm_keys(md_realms_of(refs->realms, own_id, row->venue_key), source->id);

	for (i = 0; (NULL != keys) && (NULL != keys[i]); i++)
	{
		VentureSeriesRow *found = NULL;

		if (!venture_series_store_get_current(source->reader, keys[i], row->instrument_key, &found, NULL) ||
		    !md_ref_row_usable(found, row->currency) || (VENTURE_SERIES_NONE == found->market_value) ||
		    ((NULL != best) && (found->taken_at <= best->taken_at)))
		{
			venture_series_row_free(found);
			continue;
		}

		venture_series_row_free(best);
		best = found;
	}

	return best;
}

/* @price against @reference as a percent difference: -20 is a fifth
 * under it. NAN when either is unknown or the reference is not above
 * zero. */
static gdouble
md_ref_pct(
	gint64	price,
	gint64	reference
){
	if ((VENTURE_SERIES_NONE == price) || (VENTURE_SERIES_NONE == reference) || (reference <= 0))
		return NAN;

	return 100.0 * ((gdouble)price - (gdouble)reference) / (gdouble)reference;
}

/*
 * The first source with anything to say of @buy's instrument -- the
 * deal's own source first (its own region venue, never its own realm,
 * which is the row itself), then the others of the same instrument
 * namespace by name -- as the "ref" object; NULL when none has anything,
 * so a page shows nothing rather than a zero. @sell may be NULL.
 */
static JsonObject *
md_ref_object(
	MdRefs			*refs,
	gint64			 own_id,
	const VentureSeriesRow	*buy,
	const VentureSeriesRow	*sell,
	gint64			 now,
	gint64			 stale_after
){
	g_autofree gchar *region_key = venture_series_region_venue_key(buy->group_key);
	MdRefSource *own = md_refs_find(refs, own_id);
	guint pass;
	guint i;

	for (pass = 0; pass < 2; pass++)
	{
		for (i = 0; i < refs->sources->len; i++)
		{
			MdRefSource *source = g_ptr_array_index(refs->sources, i);
			g_autoptr(VentureSeriesRow) region = NULL;
			g_autoptr(VentureSeriesRow) buy_realm = NULL;
			g_autoptr(VentureSeriesRow) sell_realm = NULL;
			gboolean is_own = (source == own);
			JsonObject *object;
			gint64 region_market;

			if (((0 == pass) != is_own) || (NULL == source->reader))
				continue;

			if (!is_own && ((NULL == own) || ('\0' == own->instrument_namespace[0]) ||
			                (0 != g_strcmp0(own->instrument_namespace, source->instrument_namespace))))
				continue;

			if ((NULL != region_key) &&
			    (!venture_series_store_get_current(source->reader, region_key, buy->instrument_key,
			                                       &region, NULL) ||
			     !md_ref_row_usable(region, buy->currency)))
				g_clear_pointer(&region, venture_series_row_free);

			if (!is_own)
			{
				buy_realm = md_ref_realm_row(refs, source, own_id, buy);
				if (NULL != sell)
					sell_realm = md_ref_realm_row(refs, source, own_id, sell);
			}

			if ((NULL == region) && (NULL == buy_realm) && (NULL == sell_realm))
				continue;

			region_market = (NULL != region) ? region->market_value : VENTURE_SERIES_NONE;
			object = json_object_new();
			json_object_set_int_member(object, "data_source_id", source->id);
			json_object_set_string_member(object, "source_name", source->name);
			md_set_text(object, "region_venue", (NULL != region) ? region_key : NULL);
			md_set_money(object, "region_market", region_market, buy->currency);
			md_set_money(object, "region_historical",
			             (NULL != region) ? region->source_historical : VENTURE_SERIES_NONE, buy->currency);
			/* The source's own rate where it sent one (TSM's), else
			 * what its store estimated. */
			md_set_ratio(object, "region_sale_rate",
			             (NULL == region) ? NAN
			             : !isnan(region->source_sale_rate) ? region->source_sale_rate : region->sale_rate);
			md_set_ratio(object, "region_sold_per_day",
			             (NULL == region) ? NAN
			             : !isnan(region->source_sold_per_day) ? region->source_sold_per_day
			             : region->sold_per_day);

			if (NULL != region)
				md_set_age(object, "region_", region->taken_at, now, stale_after);
			else
			{
				json_object_set_null_member(object, "region_taken_at");
				json_object_set_null_member(object, "region_age_seconds");
				json_object_set_null_member(object, "region_stale");
			}

			md_set_ratio(object, "buy_vs_region_pct", md_ref_pct(buy->min_price, region_market));
			md_set_money(object, "buy_realm_market",
			             (NULL != buy_realm) ? buy_realm->market_value : VENTURE_SERIES_NONE, buy->currency);
			md_set_ratio(object, "buy_vs_realm_pct",
			             md_ref_pct(buy->min_price,
			                        (NULL != buy_realm) ? buy_realm->market_value : VENTURE_SERIES_NONE));

			if (NULL != sell)
			{
				md_set_ratio(object, "sell_vs_region_pct", md_ref_pct(sell->min_price, region_market));
				md_set_money(object, "sell_realm_market",
				             (NULL != sell_realm) ? sell_realm->market_value : VENTURE_SERIES_NONE,
				             buy->currency);
				md_set_ratio(object, "sell_vs_realm_pct",
				             md_ref_pct(sell->min_price,
				                        (NULL != sell_realm) ? sell_realm->market_value : VENTURE_SERIES_NONE));
			}

			return object;
		}
	}

	return NULL;
}

/* --- The price history ------------------------------------------------------ */

/* The days of hourly history the store keeps (0: forever), as configured:
 * a range within it is read hour by hour, a longer one day by day. */
static gint64
md_hourly_days(VentureContext *context)
{
	VentureConfig *config;
	gint64 days = 14;

	config = venture_context_get_config(context);

	if (NULL != config)
		g_object_get(config, "series-hourly-days", &days, NULL);

	return MAX(days, (gint64)0);
}

/* A range's days, 0 for everything; -1 for a name that is not a range. */
static gint64
md_range_days(const gchar *range)
{
	if (0 == g_strcmp0(range, "24h"))
		return 1;
	if (0 == g_strcmp0(range, "7d"))
		return 7;
	if (0 == g_strcmp0(range, "14d"))
		return 14;
	if (0 == g_strcmp0(range, "90d"))
		return 90;
	if (0 == g_strcmp0(range, "all"))
		return 0;

	return -1;
}

/* The venue a chart is drawn for, as the instrument page chooses it: the
 * one asked for, else the cheapest with stock, else the first. NULL with
 * NOT_FOUND for a venue that never listed it; NULL and no error when no
 * venue has. */
static VentureSeriesRow *
md_chart_row(
	GPtrArray	 *venues,
	const gchar	 *venue,
	GError		**error
){
	VentureSeriesRow *row = NULL;
	guint i;

	for (i = 0; i < venues->len; i++)
	{
		VentureSeriesRow *candidate = g_ptr_array_index(venues, i);

		if (!venture_string_is_empty(venue))
		{
			if (0 == g_strcmp0(candidate->venue_key, venue))
				row = candidate;
		}
		else if ((NULL == row) && (VENTURE_SERIES_NONE != candidate->min_price) &&
		         (0 != candidate->quantity))
			row = candidate;
	}

	if (!venture_string_is_empty(venue) && (NULL == row))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "Venue \"%s\" has never listed this instrument", venue);
		return NULL;
	}

	if ((NULL == row) && (venues->len > 0))
		row = g_ptr_array_index(venues, 0);

	return row;
}

static JsonArray *
md_history_points(
	const VentureSeriesHistory	*history,
	gboolean			 full
){
	JsonArray *array;
	guint i;

	array = json_array_new();

	for (i = 0; i < history->points->len; i++)
	{
		const VentureSeriesHistoryPoint *point = &g_array_index(history->points, VentureSeriesHistoryPoint, i);
		JsonObject *object = json_object_new();

		md_set_time(object, "at", point->at);
		md_set_figure(object, "min_price", point->min_price);
		md_set_figure(object, "quantity", point->quantity);

		if (full)
		{
			md_set_figure(object, "market_value", point->market_value);
			json_object_set_int_member(object, "listings", point->listings);
		}
		else
			json_object_set_int_member(object, "venues", point->venues);

		json_array_add_object_element(array, object);
	}

	return array;
}

/*
 * The price history the instrument page charts and /api/v1/market/history
 * answers: @row's venue over @range, hour by hour within the hourly
 * window and day by day beyond it -- never both on one line, because an
 * hour's figure is its last snapshot and a day's its lowest all day, and
 * a line that changed measure partway would draw a step the market never
 * took. @compare adds a second line on the same slots and in the same
 * currency: another venue, or the region, the median of the group's
 * venues' lowest prices as they stood at each slot.
 */
static JsonObject *
md_history_object(
	VentureContext		 *context,
	VentureSeriesStore	 *reader,
	const VentureSeriesRow	 *row,
	GPtrArray		 *venues,
	const gchar		 *key,
	const gchar		 *range,
	const gchar		 *compare,
	gint64			  now,
	JsonArray		 *notes,
	GError			**error
){
	g_autoptr(VentureSeriesHistory) history = NULL;
	g_autoptr(VentureSeriesHistory) other = NULL;
	JsonObject *object;
	JsonArray *ranges;
	VentureSeriesResolution resolution;
	gint64 hourly_days;
	gint64 days;
	gint64 step;
	gint64 since;
	guint i;

	if (NULL == range)
		range = VENTURE_MARKETDATA_HISTORY_DEFAULT_RANGE;

	days = md_range_days(range);
	hourly_days = md_hourly_days(context);
	resolution = ((days > 0) && ((0 == hourly_days) || (days <= hourly_days)))
		? VENTURE_SERIES_RESOLUTION_HOUR : VENTURE_SERIES_RESOLUTION_DAY;
	step = (VENTURE_SERIES_RESOLUTION_HOUR == resolution) ? 3600 : MD_DAY;

	/* 24 hours is 24 slots, the newest the one holding now. */
	since = (days > 0) ? now - (days * MD_DAY / step - 1) * step : 0;

	history = venture_series_store_history(reader, row->venue_key, key, resolution, since, now,
	                                       row->currency, error);

	if (NULL == history)
		return NULL;

	object = json_object_new();
	json_object_set_string_member(object, "range", range);
	ranges = json_array_new();

	for (i = 0; NULL != venture_marketdata_history_ranges()[i]; i++)
		json_array_add_string_element(ranges, venture_marketdata_history_ranges()[i]);

	json_object_set_array_member(object, "ranges", ranges);
	json_object_set_string_member(object, "resolution",
	                              (VENTURE_SERIES_RESOLUTION_HOUR == resolution) ? "hour" : "day");
	md_set_time(object, "since", history->since);
	md_set_time(object, "until", history->until);
	json_object_set_string_member(object, "currency", row->currency);
	json_object_set_boolean_member(object, "truncated", history->truncated);
	json_object_set_array_member(object, "points", md_history_points(history, TRUE));

	if (history->truncated)
		md_note(notes, "The price history holds more than a chart draws; its oldest part is left out.");

	if (venture_string_is_empty(compare))
	{
		json_object_set_null_member(object, "compare");
		return object;
	}

	/* The same slots as the first line, or the two would not line up. */
	if (0 == history->points->len)
	{
		md_note(notes, "Nothing to compare with: this venue has no price history in the range.");
		json_object_set_null_member(object, "compare");
		return object;
	}

	if (0 == g_strcmp0(compare, "region"))
	{
		JsonObject *line;

		other = venture_series_store_group_history(reader, row->group_key, key, resolution,
		                                           history->since, now, row->currency, error);

		if (NULL == other)
		{
			json_object_unref(object);
			return NULL;
		}

		line = json_object_new();
		json_object_set_string_member(line, "kind", "region");
		json_object_set_null_member(line, "venue");
		json_object_set_null_member(line, "venue_name");
		json_object_set_string_member(line, "group_key", row->group_key);
		json_object_set_array_member(line, "points", md_history_points(other, FALSE));
		json_object_set_object_member(object, "compare", line);
		return object;
	}

	{
		VentureSeriesRow *with = NULL;
		JsonObject *line;

		for (i = 0; (NULL != venues) && (i < venues->len); i++)
			if (0 == g_strcmp0(((VentureSeriesRow *)g_ptr_array_index(venues, i))->venue_key, compare))
				with = g_ptr_array_index(venues, i);

		if (NULL == with)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
			            "Venue \"%s\" has never listed this instrument, so there is nothing to compare",
			            compare);
			json_object_unref(object);
			return NULL;
		}

		other = venture_series_store_history(reader, with->venue_key, key, resolution, history->since,
		                                     now, row->currency, error);

		if (NULL == other)
		{
			json_object_unref(object);
			return NULL;
		}

		line = json_object_new();
		json_object_set_string_member(line, "kind", "venue");
		json_object_set_string_member(line, "venue", with->venue_key);
		md_set_text(line, "venue_name", with->venue_name);
		json_object_set_string_member(line, "group_key", with->group_key);
		json_object_set_array_member(line, "points", md_history_points(other, FALSE));
		json_object_set_object_member(object, "compare", line);
	}

	return object;
}

#endif /* VENTURE_HAVE_SQLITE */

static const gchar *const md_history_ranges[] = { "24h", "7d", "14d", "90d", "all", NULL };

const gchar *const *
venture_marketdata_history_ranges(void)
{
	return md_history_ranges;
}

/* Refuses a range that is not one of md_history_ranges. */
static gboolean
md_check_range(
	const gchar	 *range,
	GError		**error
){
	g_autofree gchar *names = NULL;

	if ((NULL == range) || g_strv_contains(md_history_ranges, range))
		return TRUE;

	names = g_strjoinv(", ", (gchar **)md_history_ranges);
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
	            "A price history's range is one of %s, not \"%s\"", names, range);
	return FALSE;
}

JsonNode *
venture_marketdata_instrument(
	VentureContext				 *context,
	const VentureMarketdataInstrumentQuery	 *query,
	GError					**error
){
	JsonObject *root;
	JsonArray *notes;
	gint64 now;
	gint64 stale_after;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	g_return_val_if_fail(NULL != query, NULL);

	if (!md_require_module(context, error))
		return NULL;

	if (venture_string_is_empty(query->key))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "An instrument page needs the instrument's key");
		return NULL;
	}

	if ((query->units < 0) || (query->units > VENTURE_MARKETDATA_BULK_MAX_UNITS))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "The bulk calculator prices 1 to %d units", VENTURE_MARKETDATA_BULK_MAX_UNITS);
		return NULL;
	}

	if (!md_check_range(query->range, error))
		return NULL;

	now = md_now(query->now);
	stale_after = venture_marketdata_stale_seconds(context);
	(void)now;
	(void)stale_after;
	root = json_object_new();
	notes = json_array_new();
	json_object_set_boolean_member(root, "available", FALSE);
	json_object_set_int_member(root, "data_source_id", query->data_source_id);
	json_object_set_string_member(root, "key", query->key);
	json_object_set_int_member(root, "stale_after_seconds", stale_after);

	if (md_series_ready(context, notes))
	{
#ifdef VENTURE_HAVE_SQLITE
		g_autoptr(VentureEntity) source = NULL;
		g_autoptr(VentureSeriesStore) reader = NULL;
		g_autoptr(VentureSeriesInstrumentRow) instrument = NULL;
		g_autoptr(GPtrArray) venues = NULL;
		g_autoptr(GArray) hourly = NULL;
		g_autoptr(GArray) daily = NULL;
		g_autoptr(GArray) tiers = NULL;
		g_autofree gchar *source_name = NULL;
		g_autofree gchar *instrument_namespace = NULL;
		g_autofree gchar *venue_namespace = NULL;
		VentureSeriesRow *row;
		JsonObject *object;
		JsonArray *array;
		guint i;

		source = md_source(context, query->organization_id, query->data_source_id, error);

		if (NULL == source)
			goto fail;

		reader = md_reader(context, source, NULL, error);

		if (NULL == reader)
			goto fail;

		if (!venture_series_store_get_instrument(reader, query->key, &instrument, error))
			goto fail;

		if (NULL == instrument)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
			            "Data source #%" G_GINT64_FORMAT " knows no instrument \"%s\"",
			            query->data_source_id, query->key);
			goto fail;
		}

		source_name = md_source_name(source);
		g_object_get(source, "instrument-namespace", &instrument_namespace,
		             "venue-namespace", &venue_namespace, NULL);
		json_object_set_boolean_member(root, "available", TRUE);
		json_object_set_string_member(root, "source_name", source_name);

		object = json_object_new();
		json_object_set_string_member(object, "key", instrument->key);
		md_set_text(object, "name", instrument->name);
		md_set_text(object, "kind", instrument->kind);
		md_set_text(object, "category", instrument->category);
		md_set_text(object, "parent_key", instrument->parent_key);
		md_set_text(object, "namespace", instrument->namespace_);
		md_set_time(object, "first_seen", instrument->first_seen);
		md_set_time(object, "last_seen", instrument->last_seen);
		md_set_attrs_and_variant(object, instrument->key, instrument->parent_key,
		                         instrument->attrs_json);
		json_object_set_object_member(root, "instrument", object);
		json_object_set_int_member(root, "record_id",
			md_promoted_id(context, VENTURE_TYPE_INSTRUMENT, query->organization_id,
			               instrument->namespace_, instrument_namespace, instrument->key));

		/* Every venue's current row, cheapest first: the "other realms"
		 * table, and where the charted venue is chosen from. */
		venues = venture_series_store_other_venues(reader, query->key, NULL, error);

		if (NULL == venues)
			goto fail;

		{
			g_autoptr(GError) chart_error = NULL;

			row = md_chart_row(venues, query->venue, &chart_error);

			if (NULL != chart_error)
			{
				g_propagate_error(error, g_steal_pointer(&chart_error));
				goto fail;
			}
		}

		array = json_array_new();

		/* The table: optionally one venue group's venues, and dearest
		 * first on request. Venues with nothing offered stay last either
		 * way -- "most expensive" means among what can be bought. */
		{
			g_auto(GStrv) group_keys = NULL;
			g_autofree gchar *group_label = NULL;
			g_autoptr(GPtrArray) store_venues = NULL;
			g_autoptr(GPtrArray) shown = NULL;
			guint priced = 0;

			store_venues = venture_series_store_list_venues(reader, NULL);

			if (!md_venue_group_keys(context, query->organization_id, store_venues,
			                         query->venue_group, notes, &group_keys, &group_label,
			                         error))
				goto fail;

			json_object_set_array_member(root, "venue_groups",
			                             md_venue_groups_json(context, query->organization_id));
			md_set_text(root, "venue_group_name", group_label);

			shown = g_ptr_array_new();

			for (i = 0; i < venues->len; i++)
			{
				VentureSeriesRow *candidate = g_ptr_array_index(venues, i);

				if ((NULL != group_keys) &&
				    !g_strv_contains((const gchar *const *)group_keys, candidate->venue_key))
					continue;

				g_ptr_array_add(shown, candidate);
				if (VENTURE_SERIES_NONE != candidate->min_price)
					priced++;
			}

			/* The store answers cheapest first with unpriced venues
			 * last, so dearest first is the priced run reversed. */
			if (query->venues_descending)
			{
				guint a = 0;
				guint b = priced;

				while ((b > 0) && (a < b - 1))
				{
					gpointer swap = shown->pdata[a];

					shown->pdata[a] = shown->pdata[b - 1];
					shown->pdata[b - 1] = swap;
					a++;
					b--;
				}
			}

			{
				g_autoptr(JsonObject) instrument_attrs = md_attrs_object(instrument->attrs_json);
				g_autoptr(GHashTable) venue_attrs = g_hash_table_new_full(g_str_hash, g_str_equal,
				                                                          NULL, (GDestroyNotify)json_object_unref);
				JsonArray *links = json_array_new();
				guint v;

				for (v = 0; (NULL != store_venues) && (v < store_venues->len); v++)
				{
					VentureSeriesVenueRow *one = g_ptr_array_index(store_venues, v);
					JsonObject *parsed = md_attrs_object(one->attrs_json);

					if (NULL != parsed)
						g_hash_table_insert(venue_attrs, one->key, parsed);
				}

				/* The item's own links, then the charted venue's. */
				md_links_append(links, instrument_attrs, instrument_attrs);
				if (NULL != row)
					md_links_append(links, g_hash_table_lookup(venue_attrs, row->venue_key),
					                instrument_attrs);
				json_object_set_array_member(root, "links", links);

				for (i = 0; i < shown->len; i++)
				{
					VentureSeriesRow *one = g_ptr_array_index(shown, i);
					JsonObject *venue_row = md_row_object(one, query->data_source_id);
					JsonArray *row_links = json_array_new();

					md_set_age(venue_row, "", one->taken_at, now, stale_after);

					md_links_append(row_links, g_hash_table_lookup(venue_attrs, one->venue_key),
					                instrument_attrs);
					json_object_set_array_member(venue_row, "links", row_links);
					json_array_add_object_element(array, venue_row);
				}
			}

			json_object_set_boolean_member(root, "venues_descending", query->venues_descending);
		}

		json_object_set_array_member(root, "venues", array);

		if (NULL == row)
		{
			md_note(notes, "No venue has listed this instrument yet.");
			json_object_set_null_member(root, "venue");
		}
		else
		{
			VentureSeriesRegion region;
			VentureSeriesReference reference;
			VentureSeriesBulkCost bulk;
			gchar bulk_currency[VENTURE_MONEY_CURRENCY_LEN];
			g_autoptr(JsonObject) heat = NULL;

			json_object_set_string_member(root, "venue", row->venue_key);
			md_set_text(root, "venue_name", row->venue_name);
			json_object_set_string_member(root, "group_key", row->group_key);
			json_object_set_string_member(root, "currency", row->currency);
			json_object_set_int_member(root, "venue_record_id",
				md_promoted_id(context, VENTURE_TYPE_VENUE, query->organization_id, NULL,
				               venue_namespace, row->venue_key));

			hourly = venture_series_store_hourly(reader, row->venue_key, query->key,
			                                     now - MD_HOURLY_DAYS * MD_DAY, error);
			daily = (NULL != hourly)
				? venture_series_store_daily(reader, row->venue_key, query->key,
				                             now - MD_DAILY_DAYS * MD_DAY, error)
				: NULL;

			if ((NULL == hourly) || (NULL == daily) ||
			    !venture_series_store_get_region(reader, row->group_key, query->key,
			                                     row->currency, &region, error) ||
			    !venture_series_store_reference(reader, row->venue_key, NULL, query->key, now,
			                                    &reference, error))
				goto fail;

			/* Base stats: what Undermine shows at the top of an item. */
			object = json_object_new();
			md_set_money(object, "min_price", row->min_price, row->currency);
			md_set_figure(object, "quantity", row->quantity);
			json_object_set_int_member(object, "listings", row->listings);
			md_set_money(object, "market_value", row->market_value, row->currency);
			md_set_age(object, "", row->taken_at, now, stale_after);
			md_set_time(object, "last_seen", row->seen_at);
			md_set_money(object, "median_14d",
			             md_median_days(daily, row->currency, now - MD_MEDIAN_DAYS * MD_DAY),
			             row->currency);
			md_set_money(object, "region_median", region.found ? region.median_min : VENTURE_SERIES_NONE,
			             row->currency);
			md_set_money(object, "region_p33", region.found ? region.p33 : VENTURE_SERIES_NONE,
			             row->currency);
			md_set_money(object, "deal_price", region.found ? region.deal_price : VENTURE_SERIES_NONE,
			             row->currency);
			md_set_money(object, "region_market_avg",
			             region.found ? region.market_avg : VENTURE_SERIES_NONE, row->currency);
			json_object_set_int_member(object, "region_venues", region.found ? region.venues_offering : 0);
			md_set_figure(object, "region_quantity",
			              region.found ? region.total_quantity : VENTURE_SERIES_NONE);
			md_set_time(object, "region_computed_at", region.found ? region.computed_at : 0);
			md_set_ratio(object, "pct_vs_region", row->pct_vs_region);
			json_object_set_object_member(root, "base", object);

			/* Another source's view of it, as on Deals: the region's
			 * figures, and the charted venue's realm's. */
			{
				g_autoptr(MdRefs) refs = md_refs_new(context, query->organization_id);
				JsonObject *ref = md_ref_object(refs, query->data_source_id, row, NULL, now, stale_after);

				if (NULL != ref)
					json_object_set_object_member(root, "ref", ref);
			}

			/* The sale estimate and the slow prices, at this venue. */
			object = json_object_new();
			json_object_set_string_member(object, "currency", reference.currency);
			md_set_money(object, "market_14d", reference.market_14d, reference.currency);
			md_set_money(object, "historical_60d", reference.historical_60d, reference.currency);
			md_set_money(object, "sale_avg", reference.sale_avg, reference.currency);
			md_set_ratio(object, "sale_rate", reference.sale_rate);
			md_set_ratio(object, "sold_per_day", reference.sold_per_day);
			json_object_set_int_member(object, "days_14", reference.days_14);
			json_object_set_int_member(object, "days_60", reference.days_60);
			json_object_set_object_member(root, "reference", object);

			array = json_array_new();

			for (i = 0; i < hourly->len; i++)
			{
				const VentureSeriesPoint *point = &g_array_index(hourly, VentureSeriesPoint, i);

				object = json_object_new();
				md_set_time(object, "at", point->at);
				json_object_set_string_member(object, "currency", point->currency);
				md_set_figure(object, "min_price", point->min_price);
				md_set_figure(object, "quantity", point->quantity);
				md_set_figure(object, "market_value", point->market_value);
				md_set_figure(object, "listings", point->listings);
				json_array_add_object_element(array, object);
			}

			json_object_set_array_member(root, "hourly", array);

			heat = md_heat(context, hourly, now, error);

			if (NULL == heat)
				goto fail;

			json_object_set_object_member(root, "heat", g_steal_pointer(&heat));

			array = json_array_new();

			for (i = 0; i < daily->len; i++)
			{
				const VentureSeriesDay *day = &g_array_index(daily, VentureSeriesDay, i);

				object = json_object_new();
				md_set_time(object, "day", day->day_start);
				json_object_set_string_member(object, "currency", day->currency);
				json_object_set_int_member(object, "snapshots", day->snapshots);
				md_set_figure(object, "min_price", day->min_price);
				json_object_set_int_member(object, "max_quantity", day->max_quantity);
				md_set_figure(object, "price_at_max", day->price_at_max);
				md_set_figure(object, "market_value", day->market_value);
				json_object_set_int_member(object, "sold", day->sold_estimate);
				json_object_set_int_member(object, "expired", day->expired_estimate);
				md_set_figure(object, "sale_avg", day->sale_avg);
				json_array_add_object_element(array, object);
			}

			json_object_set_array_member(root, "daily", array);

			/* The chart a person reads to tell a dip from the new
			 * normal, over the range asked for. */
			object = md_history_object(context, reader, row, venues, query->key, query->range,
			                           query->compare, now, notes, error);

			if (NULL == object)
				goto fail;

			json_object_set_object_member(root, "history", object);

			/* The book, and what buying from it would cost. */
			tiers = venture_series_store_get_tiers(reader, row->venue_key, query->key, error);

			if (NULL == tiers)
				goto fail;

			array = json_array_new();

			for (i = 0; i < tiers->len; i++)
			{
				const VentureSeriesTier *tier = &g_array_index(tiers, VentureSeriesTier, i);

				object = json_object_new();
				json_object_set_int_member(object, "price", tier->price);
				json_object_set_int_member(object, "quantity", tier->quantity);
				json_array_add_object_element(array, object);
			}

			json_object_set_array_member(root, "tiers", array);

			if (query->units > 0)
			{
				if (!venture_series_store_bulk_cost(reader, row->venue_key, query->key, query->units,
				                                    &bulk, bulk_currency, error))
					goto fail;

				object = json_object_new();
				json_object_set_int_member(object, "requested", bulk.requested);
				json_object_set_int_member(object, "filled", bulk.filled);
				md_set_money(object, "cost", (bulk.filled > 0) ? bulk.cost : VENTURE_SERIES_NONE,
				             bulk_currency);
				md_set_money(object, "worst_price", bulk.worst_price, bulk_currency);
				md_set_money(object, "average", bulk.average, bulk_currency);
				json_object_set_boolean_member(object, "complete", bulk.complete);
				json_object_set_object_member(root, "bulk", object);
			}
			else
				json_object_set_null_member(root, "bulk");
		}

		json_object_set_array_member(root, "watchlists",
		                             md_watchlists_brief(context, query->organization_id));
#endif
	}

	json_object_set_array_member(root, "notes", notes);
	md_attribute_source(context, query->organization_id, root);

	return md_node(root);

#ifdef VENTURE_HAVE_SQLITE
fail:
	json_object_unref(root);
	json_array_unref(notes);

	return NULL;
#endif
}

/* --- A price history alone -------------------------------------------------- */

JsonNode *
venture_marketdata_history(
	VentureContext				 *context,
	const VentureMarketdataInstrumentQuery	 *query,
	GError					**error
){
	JsonObject *root;
	JsonArray *notes;
	gint64 now;
	gint64 stale_after;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	g_return_val_if_fail(NULL != query, NULL);

	if (!md_require_module(context, error))
		return NULL;

	if (venture_string_is_empty(query->key))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "A price history needs the instrument's key");
		return NULL;
	}

	if (!md_check_range(query->range, error))
		return NULL;

	now = md_now(query->now);
	stale_after = venture_marketdata_stale_seconds(context);
	(void)now;
	root = json_object_new();
	notes = json_array_new();
	json_object_set_boolean_member(root, "available", FALSE);
	json_object_set_int_member(root, "data_source_id", query->data_source_id);
	json_object_set_string_member(root, "key", query->key);
	json_object_set_int_member(root, "stale_after_seconds", stale_after);

	if (md_series_ready(context, notes))
	{
#ifdef VENTURE_HAVE_SQLITE
		g_autoptr(VentureEntity) source = NULL;
		g_autoptr(VentureSeriesStore) reader = NULL;
		g_autoptr(VentureSeriesInstrumentRow) instrument = NULL;
		g_autoptr(GPtrArray) venues = NULL;
		g_autoptr(GError) chart_error = NULL;
		g_autofree gchar *source_name = NULL;
		VentureSeriesRow *row;
		JsonObject *history;

		source = md_source(context, query->organization_id, query->data_source_id, error);

		if (NULL == source)
			goto fail;

		reader = md_reader(context, source, NULL, error);

		if (NULL == reader)
			goto fail;

		if (!venture_series_store_get_instrument(reader, query->key, &instrument, error))
			goto fail;

		if (NULL == instrument)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
			            "Data source #%" G_GINT64_FORMAT " knows no instrument \"%s\"",
			            query->data_source_id, query->key);
			goto fail;
		}

		source_name = md_source_name(source);
		json_object_set_boolean_member(root, "available", TRUE);
		json_object_set_string_member(root, "source_name", source_name);
		md_set_text(root, "name", instrument->name);

		venues = venture_series_store_other_venues(reader, query->key, NULL, error);

		if (NULL == venues)
			goto fail;

		row = md_chart_row(venues, query->venue, &chart_error);

		if (NULL != chart_error)
		{
			g_propagate_error(error, g_steal_pointer(&chart_error));
			goto fail;
		}

		if (NULL == row)
		{
			md_note(notes, "No venue has listed this instrument yet.");
			json_object_set_null_member(root, "venue");
			json_object_set_null_member(root, "history");
		}
		else
		{
			json_object_set_string_member(root, "venue", row->venue_key);
			md_set_text(root, "venue_name", row->venue_name);
			json_object_set_string_member(root, "group_key", row->group_key);
			json_object_set_string_member(root, "currency", row->currency);

			history = md_history_object(context, reader, row, venues, query->key, query->range,
			                            query->compare, now, notes, error);

			if (NULL == history)
				goto fail;

			json_object_set_object_member(root, "history", history);
		}
#endif
	}

	if (!json_object_has_member(root, "history"))
		json_object_set_null_member(root, "history");

	json_object_set_array_member(root, "notes", notes);
	md_attribute_source(context, query->organization_id, root);

	return md_node(root);

#ifdef VENTURE_HAVE_SQLITE
fail:
	json_object_unref(root);
	json_array_unref(notes);

	return NULL;
#endif
}

/* --- Deals ----------------------------------------------------------------- */

/* --- Venue group choices ----------------------------------------------------- */

JsonNode *
venture_marketdata_picker_choices(
	VentureContext	*context,
	gint64		 organization_id,
	gint64		 data_source_id
){
	JsonObject *root;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	root = json_object_new();
#ifdef VENTURE_HAVE_SQLITE
	{
		g_autoptr(GHashTable) counts = md_categories_new();
		g_autoptr(VentureEntity) source = NULL;
		g_autoptr(JsonArray) scratch = json_array_new();

		json_object_set_array_member(root, "venue_groups",
		                             md_venue_groups_json(context, organization_id));

		if (data_source_id > 0)
			source = md_source(context, organization_id, data_source_id, NULL);
		else
		{
			g_autoptr(GPtrArray) sources = md_sources(context, organization_id, NULL);

			if ((NULL != sources) && (sources->len > 0))
				source = g_object_ref(g_ptr_array_index(sources, 0));
		}

		if ((NULL != source) && md_series_ready(context, scratch))
		{
			g_autoptr(VentureSeriesStore) reader = md_reader(context, source, NULL, NULL);

			if (NULL != reader)
				md_categories_add(counts, reader);
		}

		json_object_set_array_member(root, "categories", md_categories_json(counts));
	}
#else
	(void)organization_id;
	(void)data_source_id;
	json_object_set_array_member(root, "venue_groups", json_array_new());
	json_object_set_array_member(root, "categories", json_array_new());
#endif
	return md_node(root);
}

/* --- Find ---------------------------------------------------------------- */

#ifdef VENTURE_HAVE_SQLITE

typedef struct
{
	VentureSeriesRow	*cheapest;
	VentureSeriesRow	*dearest;
	guint			 venues;
} MdFound;

#endif

JsonNode *
venture_marketdata_find(
	VentureContext				 *context,
	const VentureMarketdataFindQuery	 *query,
	GError					**error
){
	JsonObject *root;
	JsonArray *notes;
	JsonArray *items;
	JsonObject *echo;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	g_return_val_if_fail(NULL != query, NULL);

	if (!md_require_module(context, error))
		return NULL;

	if (venture_string_is_empty(query->search) && venture_string_is_empty(query->category))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "Say what to look for: part of an item's name or key, or a category");
		return NULL;
	}

	root = json_object_new();
	notes = json_array_new();
	items = json_array_new();
	echo = json_object_new();
	md_set_text(echo, "search", query->search);
	md_set_text(echo, "venue_group", query->venue_group);
	md_set_text(echo, "category", query->category);
	json_object_set_int_member(echo, "data_source_id", query->data_source_id);
	json_object_set_object_member(root, "query", echo);
	json_object_set_boolean_member(root, "available", FALSE);
	json_object_set_boolean_member(root, "truncated", FALSE);
	json_object_set_int_member(root, "data_source_id", 0);

	if (md_series_ready(context, notes))
	{
#ifdef VENTURE_HAVE_SQLITE
		g_autoptr(GPtrArray) sources = NULL;
		g_autoptr(VentureEntity) chosen = NULL;
		g_autoptr(VentureSeriesStore) reader = NULL;

		sources = md_sources(context, query->organization_id, error);
		if (NULL == sources)
			goto fail;

		json_object_set_array_member(root, "sources", md_sources_json(sources));
		json_object_set_array_member(root, "venue_groups",
		                             md_venue_groups_json(context, query->organization_id));

		if (query->data_source_id > 0)
		{
			chosen = md_source(context, query->organization_id, query->data_source_id, error);
			if (NULL == chosen)
				goto fail;
		}
		else if (sources->len > 0)
			chosen = g_object_ref(g_ptr_array_index(sources, 0));
		else
			md_note(notes, "No data sources yet: add one under Market data feeds.");

		if (NULL != chosen)
		{
			json_object_set_int_member(root, "data_source_id", venture_entity_get_id(chosen));
			reader = md_reader(context, chosen, notes, NULL);
		}

		if (NULL != reader)
		{
			g_autoptr(GPtrArray) venues = NULL;
			g_autoptr(GPtrArray) found = NULL;
			g_autoptr(GHashTable) by_key = NULL;
			g_autoptr(GPtrArray) order = NULL;
			g_auto(GStrv) group_keys = NULL;
			g_autofree gchar *group_label = NULL;
			VentureSeriesFilter filter;
			guint i;

			venues = venture_series_store_list_venues(reader, NULL);

			if (!md_venue_group_keys(context, query->organization_id, venues, query->venue_group,
			                         notes, &group_keys, &group_label, error))
				goto fail;

			md_set_text(root, "venue_group_name", group_label);

			{
				g_autoptr(GHashTable) counts = md_categories_new();

				md_categories_add(counts, reader);
				json_object_set_array_member(root, "categories", md_categories_json(counts));
			}

			venture_series_filter_init(&filter);
			filter.search = venture_string_is_empty(query->search) ? NULL : query->search;
			filter.category_prefix = venture_string_is_empty(query->category) ? NULL : query->category;
			filter.venue_keys = (const gchar **)group_keys;
			filter.in_stock_only = TRUE;
			filter.sort = VENTURE_SERIES_SORT_NAME;

			/* The store answers a page at a time; read pages until the
			 * search is exhausted or one row past the bound says there
			 * are more than one search reads. */
			found = g_ptr_array_new_with_free_func((GDestroyNotify)venture_series_row_free);

			while (found->len <= VENTURE_MARKETDATA_FIND_MAX_ROWS)
			{
				g_autoptr(GPtrArray) page = NULL;

				filter.offset = found->len;
				filter.count = MIN(VENTURE_SERIES_MAX_PAGE, VENTURE_MARKETDATA_FIND_MAX_ROWS + 1 - found->len);
				page = venture_series_store_list_current(reader, &filter, error);

				if (NULL == page)
					goto fail;

				g_ptr_array_extend_and_steal(found, g_steal_pointer(&page));

				if (filter.count > (guint)(found->len - filter.offset))
					break;
			}

			json_object_set_boolean_member(root, "available", TRUE);

			if (found->len > VENTURE_MARKETDATA_FIND_MAX_ROWS)
			{
				json_object_set_boolean_member(root, "truncated", TRUE);
				g_ptr_array_set_size(found, VENTURE_MARKETDATA_FIND_MAX_ROWS);
				md_note(notes, "More matches than one search reads: use more of the name.");
			}

			/* Group the venue rows by instrument, keeping the store's
			 * name order for the instruments' order. */
			by_key = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);
			order = g_ptr_array_new();

			for (i = 0; i < found->len; i++)
			{
				VentureSeriesRow *row = g_ptr_array_index(found, i);
				MdFound *entry;

				if (VENTURE_SERIES_NONE == row->min_price)
					continue;

				entry = g_hash_table_lookup(by_key, row->instrument_key);

				if (NULL == entry)
				{
					entry = g_new0(MdFound, 1);
					entry->cheapest = row;
					entry->dearest = row;
					g_hash_table_insert(by_key, row->instrument_key, entry);
					g_ptr_array_add(order, row->instrument_key);
				}

				if (row->min_price < entry->cheapest->min_price)
					entry->cheapest = row;
				if (row->min_price > entry->dearest->min_price)
					entry->dearest = row;
				entry->venues++;
			}

			for (i = 0; i < order->len; i++)
			{
				const gchar *key = g_ptr_array_index(order, i);
				MdFound *entry = g_hash_table_lookup(by_key, key);
				g_autoptr(VentureSeriesInstrumentRow) instrument = NULL;
				g_autofree gchar *url = NULL;
				JsonObject *object = json_object_new();

				venture_series_store_get_instrument(reader, key, &instrument, NULL);

				json_object_set_string_member(object, "key", key);
				md_set_text(object, "name", entry->cheapest->instrument_name);
				md_set_text(object, "category", entry->cheapest->category);
				md_set_attrs_and_variant(object, key,
				                         (NULL != instrument) ? instrument->parent_key : NULL,
				                         (NULL != instrument) ? instrument->attrs_json : NULL);
				json_object_set_int_member(object, "venues", entry->venues);
				json_object_set_object_member(object, "cheapest",
					md_row_object(entry->cheapest, venture_entity_get_id(chosen)));
				json_object_set_object_member(object, "dearest",
					md_row_object(entry->dearest, venture_entity_get_id(chosen)));
				url = venture_marketdata_instrument_path(venture_entity_get_id(chosen), key, NULL);
				json_object_set_string_member(object, "url", url);
				json_array_add_object_element(items, object);
			}
		}
#endif
	}

	if (!json_object_has_member(root, "sources"))
		json_object_set_array_member(root, "sources", json_array_new());
	if (!json_object_has_member(root, "venue_groups"))
		json_object_set_array_member(root, "venue_groups", json_array_new());
	if (!json_object_has_member(root, "categories"))
		json_object_set_array_member(root, "categories", json_array_new());

	json_object_set_array_member(root, "items", items);
	json_object_set_array_member(root, "notes", notes);
	return md_node(root);

#ifdef VENTURE_HAVE_SQLITE
fail:
	json_object_unref(root);
	json_array_unref(notes);
	json_array_unref(items);
	return NULL;
#endif
}

void
venture_marketdata_deals_query_init(VentureMarketdataDealsQuery *query)
{
	g_return_if_fail(NULL != query);

	memset(query, 0, sizeof(*query));
	query->max_pct = NAN;
	query->cut_pct = 5.0;
	query->min_sold_per_day = NAN;
	query->horizon_days = VENTURE_MARKETDATA_DEALS_HORIZON_DAYS;
}

#ifdef VENTURE_HAVE_SQLITE

typedef struct
{
	VentureSeriesRow	*row;
	gint64			 data_source_id;
	guint			 source_index;	/* in the answer's readers */
	gchar			*source_name;
	JsonObject		*display;
	VentureSeriesRow	*sell;		/* the dearest venue to sell at, or NULL */
	const gchar		*buy_label;	/* the connected realms' labels, borrowed */
	const gchar		*sell_label;	/* from the answer's MdRealms, or NULL */
	gint64			 profit;	/* sell less the cut less the buy */
	gdouble			 roi;		/* profit / buy, percent */
	gint64			 net;		/* one unit's sale after the cut */
	guint			 source_index;	/* into the answer's sources and readers */
	gboolean		 realised;	/* md_deal_realise() has run */
	gint64			 expected;	/* units the sell venue sells over the horizon, or NONE */
	VentureSeriesRealisable	 realisable;	/* meaningful when expected is not NONE */
} MdDeal;

static void
md_deal_free(gpointer data)
{
	MdDeal *deal = data;

	venture_series_row_free(deal->row);
	venture_series_row_free(deal->sell);
	g_free(deal->source_name);
	if (NULL != deal->display)
		json_object_unref(deal->display);
	g_free(deal);
}

/* Cheapest against the region first, then by source, venue, instrument:
 * the same order every time, whatever order the sources were read in. */
/* The columns deals sort by, "deal" being the deal index's own order. */
static const gchar *const md_deal_sorts[] = {
	"deal", "name", "ilvl", "buy_at", "buy", "sell_at", "sell", "profit", "roi", "region", "rate", "sold",
	"realisable", "qty", NULL
};

/* The merge's order: NULL for the deal index's own, else a column of
 * md_deal_sorts, and which way. Set for the length of one sort; the
 * server answers on one thread. */
static const gchar *md_deal_sort = NULL;
static gboolean md_deal_descending = FALSE;

/* The columns sorted as text rather than as figures. */
static const gchar *const md_deal_text_sorts[] = { "name", "buy_at", "sell_at", NULL };

/* Whether a column reads most-first unless asked otherwise. */
static gboolean
md_deal_sort_descends(const gchar *sort)
{
	static const gchar *const ascending[] = { "name", "buy_at", "sell_at", "buy", "region", NULL };

	return !g_strv_contains(ascending, sort);
}

/* The figure a deal is sorted by, NAN when it has none for that column. */
static gdouble
md_deal_figure(const MdDeal *deal, const gchar *sort)
{
	if (0 == g_strcmp0(sort, "ilvl"))
		return json_object_has_member(deal->display, "level")
			? (gdouble)json_object_get_int_member(deal->display, "level") : NAN;
	if (0 == g_strcmp0(sort, "buy"))
		return (gdouble)deal->row->min_price;
	if (0 == g_strcmp0(sort, "region"))
		return deal->row->pct_vs_region;
	if (0 == g_strcmp0(sort, "qty"))
		return (gdouble)deal->row->quantity;
	if (NULL == deal->sell)
		return NAN;
	if (0 == g_strcmp0(sort, "sell"))
		return (gdouble)deal->sell->min_price;
	if (0 == g_strcmp0(sort, "profit"))
		return (gdouble)deal->profit;
	if (0 == g_strcmp0(sort, "roi"))
		return deal->roi;
	if (0 == g_strcmp0(sort, "rate"))
		return deal->sell->sale_rate;
	if (0 == g_strcmp0(sort, "sold"))
		return deal->sell->sold_per_day;
	if (0 == g_strcmp0(sort, "realisable"))
		return (VENTURE_SERIES_NONE != deal->expected) ? (gdouble)deal->realisable.profit : NAN;
	return NAN;
}

/* The text a deal is sorted by, NULL when it has none for that column. */
static const gchar *
md_deal_text(const MdDeal *deal, const gchar *sort)
{
	if (0 == g_strcmp0(sort, "name"))
		return (NULL != deal->row->instrument_name) ? deal->row->instrument_name : deal->row->instrument_key;
	/* By the names the page shows: the connected realm's label. */
	if (0 == g_strcmp0(sort, "buy_at"))
		return (NULL != deal->buy_label) ? deal->buy_label
		     : (NULL != deal->row->venue_name) ? deal->row->venue_name : deal->row->venue_key;
	if ((0 == g_strcmp0(sort, "sell_at")) && (NULL != deal->sell))
		return (NULL != deal->sell_label) ? deal->sell_label
		     : (NULL != deal->sell->venue_name) ? deal->sell->venue_name : deal->sell->venue_key;
	return NULL;
}

/*
 * Where @deal is best sold: the dearest venue offering its instrument, in
 * @group_keys when given and else in the buy venue's group, other than
 * the buy venue, with stock (a market with nothing in it has no price to
 * sell at), where it has been selling -- at least @min_sold_per_day a day
 * when that is a number -- and whose price is not an outlier. Sets
 * deal->sell, net, profit and roi; leaves sell NULL when there is nowhere.
 */
static void
md_deal_sell_side(
	MdDeal			*deal,
	VentureSeriesStore	*reader,
	const gchar *const	*group_keys,
	gdouble			 cut_pct,
	gdouble			 min_sold_per_day
){
	g_autoptr(GPtrArray) venues = NULL;
	gint64 cut;
	gint64 net;
	guint best = G_MAXUINT;
	guint i;

	venues = venture_series_store_other_venues(reader, deal->row->instrument_key,
	                                           (NULL != group_keys) ? NULL : deal->row->group_key, NULL);

	for (i = 0; (NULL != venues) && (i < venues->len); i++)
	{
		VentureSeriesRow *candidate = g_ptr_array_index(venues, i);

		if ((0 == g_strcmp0(candidate->venue_key, deal->row->venue_key)) ||
		    (VENTURE_SERIES_NONE == candidate->min_price) || (0 == candidate->quantity) ||
		    (0 != g_strcmp0(candidate->currency, deal->row->currency)) ||
		    ((NULL != group_keys) && !g_strv_contains(group_keys, candidate->venue_key)))
			continue;

		if (!venture_series_row_sell_plausible(candidate, candidate->min_price))
			continue;

		/* A floor on how fast it sells is a floor on the store's own
		 * estimate: a venue it has none for is not known to sell at
		 * that rate, so it is not one. */
		if (!isnan(min_sold_per_day) &&
		    (isnan(candidate->sold_per_day) || (candidate->sold_per_day < min_sold_per_day)))
			continue;

		/* Over twenty times the buy price is a troll's listing, however
		 * "sold" it looks (VENTURE_SERIES_SELL_MAX_MARKUP). */
		if ((deal->row->min_price > 0) &&
		    (candidate->min_price > VENTURE_SERIES_SELL_MAX_MARKUP * deal->row->min_price))
			continue;

		if ((G_MAXUINT == best) ||
		    (candidate->min_price > ((VentureSeriesRow *)g_ptr_array_index(venues, best))->min_price))
			best = i;
	}

	if (G_MAXUINT == best)
		return;

	g_ptr_array_set_free_func(venues, (GDestroyNotify)venture_series_row_free);
	deal->sell = g_ptr_array_steal_index(venues, best);

	/* The cut is taken from the sale, in whole minor units, rounded the
	 * seller's way (down): a profit is never overstated by a fraction. */
	cut = (gint64)ceil((gdouble)deal->sell->min_price * CLAMP(cut_pct, 0.0, 100.0) / 100.0);
	net = deal->sell->min_price - cut;
	deal->net = net;
	deal->profit = net - deal->row->min_price;
	deal->roi = (deal->row->min_price > 0) ? (100.0 * (gdouble)deal->profit / (gdouble)deal->row->min_price) : 0.0;
}

/*
 * What @deal can really make over @horizon_days: the sell venue's units
 * sold a day times the horizon, rounded down, is how many it is expected
 * to sell; the buy venue's book, cheapest first and only while a unit
 * costs less than one sells for after the cut, is how many there are to
 * buy at a profit. The smaller of the two, each unit at its own margin
 * (venture_series_math_realisable()). A store that keeps no tiers (a
 * source's statistics) is one tier: its lowest price, its quantity. No
 * sell side, or no estimate of how fast it sells there, leaves it
 * unknown -- never zero, which is a real answer ("it does not sell").
 */
static void
md_deal_realise(
	MdDeal			*deal,
	VentureSeriesStore	*reader,
	gdouble			 horizon_days
){
	g_autoptr(GArray) tiers = NULL;

	if (deal->realised)
		return;

	deal->realised = TRUE;
	deal->expected = VENTURE_SERIES_NONE;

	if (NULL == deal->sell)
		return;

	deal->expected = venture_series_math_expected_sales(deal->sell->sold_per_day, horizon_days);

	if (VENTURE_SERIES_NONE == deal->expected)
		return;

	tiers = venture_series_store_get_tiers(reader, deal->row->venue_key, deal->row->instrument_key, NULL);

	if ((NULL != tiers) && (0 == tiers->len) && (VENTURE_SERIES_NONE != deal->row->min_price) &&
	    (VENTURE_SERIES_NONE != deal->row->quantity) && (deal->row->quantity > 0))
	{
		VentureSeriesTier one;

		one.price = deal->row->min_price;
		one.quantity = deal->row->quantity;
		g_array_append_val(tiers, one);
	}

	if ((NULL == tiers) ||
	    !venture_series_math_realisable((const VentureSeriesTier *)(gpointer)tiers->data, tiers->len,
	                                    deal->net, deal->expected, &deal->realisable, NULL))
		deal->expected = VENTURE_SERIES_NONE;
}

/* The deals pickers' venues, one per connected realm, in label order. */
static gint
md_compare_realms(
	gconstpointer	a,
	gconstpointer	b
){
	return g_utf8_collate((*(MdRealm *const *)a)->label, (*(MdRealm *const *)b)->label);
}

static JsonArray *
md_venue_choices_json(
	GHashTable	*choices,
	gint64		 now,
	gint64		 stale_after
){
	g_autoptr(GPtrArray) realms = g_ptr_array_new();
	JsonArray *array = json_array_new();
	GHashTableIter iter;
	gpointer realm;
	guint i;

	g_hash_table_iter_init(&iter, choices);

	while (g_hash_table_iter_next(&iter, &realm, NULL))
		g_ptr_array_add(realms, realm);

	g_ptr_array_sort(realms, md_compare_realms);

	for (i = 0; i < realms->len; i++)
		json_array_add_object_element(array, md_realm_choice(g_ptr_array_index(realms, i), now, stale_after));

	return array;
}

/* @keys less those outside @group_keys; NULL when none is left. With no
 * group, @keys as they are. Transfer full. */
static gchar **
md_keys_in_group(
	gchar		**keys,
	gchar		**group_keys
){
	g_autoptr(GPtrArray) kept = NULL;
	guint i;

	if ((NULL == keys) || (NULL == group_keys))
		return keys;

	kept = g_ptr_array_new_with_free_func(g_free);

	for (i = 0; NULL != keys[i]; i++)
		if (g_strv_contains((const gchar *const *)group_keys, keys[i]))
			g_ptr_array_add(kept, g_strdup(keys[i]));

	g_strfreev(keys);

	if (0 == kept->len)
		return NULL;

	g_ptr_array_add(kept, NULL);
	return (gchar **)g_ptr_array_free(g_steal_pointer(&kept), FALSE);
}

static gint
md_deal_compare(
	gconstpointer	a,
	gconstpointer	b
){
	const MdDeal *x = *(const MdDeal *const *)a;
	const MdDeal *y = *(const MdDeal *const *)b;
	gint order;

	if (NULL != md_deal_sort)
	{
		if (g_strv_contains(md_deal_text_sorts, md_deal_sort))
		{
			const gchar *a_text = md_deal_text(x, md_deal_sort);
			const gchar *b_text = md_deal_text(y, md_deal_sort);

			/* Nothing to compare by goes last, whichever way. */
			if ((NULL == a_text) != (NULL == b_text))
				return (NULL == a_text) ? 1 : -1;

			if (NULL != a_text)
			{
				g_autofree gchar *a_fold = g_utf8_casefold(a_text, -1);
				g_autofree gchar *b_fold = g_utf8_casefold(b_text, -1);

				order = g_utf8_collate(a_fold, b_fold);

				if (0 != order)
					return md_deal_descending ? -order : order;
			}
		}
		else
		{
			gdouble a_value = md_deal_figure(x, md_deal_sort);
			gdouble b_value = md_deal_figure(y, md_deal_sort);

			if (isnan(a_value) != isnan(b_value))
				return isnan(a_value) ? 1 : -1;

			if (!isnan(a_value) && (a_value != b_value))
				return ((a_value < b_value) != md_deal_descending) ? -1 : 1;
		}
	}

	if (x->row->pct_vs_region < y->row->pct_vs_region)
		return -1;
	if (x->row->pct_vs_region > y->row->pct_vs_region)
		return 1;
	if (x->data_source_id != y->data_source_id)
		return (x->data_source_id < y->data_source_id) ? -1 : 1;

	order = g_strcmp0(x->row->venue_key, y->row->venue_key);

	if (0 != order)
		return order;

	return g_strcmp0(x->row->instrument_key, y->row->instrument_key);
}

#endif

JsonNode *
venture_marketdata_deals(
	VentureContext				 *context,
	const VentureMarketdataDealsQuery	 *query,
	GError					**error
){
	JsonObject *root;
	JsonArray *notes;
	JsonArray *rows;
	guint count;
	gint64 now;
	gint64 stale_after;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	g_return_val_if_fail(NULL != query, NULL);

	if (!md_require_module(context, error))
		return NULL;

	count = (0 == query->count) ? 50 : query->count;
	now = md_now(query->now);
	stale_after = venture_marketdata_stale_seconds(context);
	(void)now;

	if (count > VENTURE_MARKETDATA_DEALS_MAX)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "The deals list holds at most %d rows", VENTURE_MARKETDATA_DEALS_MAX);
		return NULL;
	}

	if (!isnan(query->max_pct) && (!isfinite(query->max_pct) || (query->max_pct < 0.0)))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "max_pct is a percent of the region median, zero or more");
		return NULL;
	}

	if (!isfinite(query->cut_pct) || (query->cut_pct < 0.0) || (query->cut_pct >= 100.0))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "cut_pct is the marketplace's cut of a sale: a percent from 0 to under 100");
		return NULL;
	}

	if (!isnan(query->min_sold_per_day) && (!isfinite(query->min_sold_per_day) || (query->min_sold_per_day < 0.0)))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "min_sold_per_day is the fewest units the sell venue sells a day, zero or more");
		return NULL;
	}

	if (!isfinite(query->horizon_days) || (query->horizon_days <= 0.0) ||
	    (query->horizon_days > VENTURE_MARKETDATA_DEALS_MAX_HORIZON_DAYS))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "horizon_days is the days a realisable profit counts sales over: above 0, at most %g",
		            VENTURE_MARKETDATA_DEALS_MAX_HORIZON_DAYS);
		return NULL;
	}

	if ((NULL != query->sort) && !g_strv_contains(md_deal_sorts, query->sort))
	{
		g_autofree gchar *names = g_strjoinv(", ", (gchar **)md_deal_sorts);

		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "Deals sort by %s, not \"%s\"", names, query->sort);
		return NULL;
	}

	if ((NULL != query->dir) && (0 != g_strcmp0(query->dir, "asc")) && (0 != g_strcmp0(query->dir, "desc")))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "dir is asc or desc, not \"%s\"", query->dir);
		return NULL;
	}

	root = json_object_new();
	notes = json_array_new();
	rows = json_array_new();
	json_object_set_boolean_member(root, "available", FALSE);
	json_object_set_boolean_member(root, "truncated", FALSE);
	json_object_set_int_member(root, "stale_after_seconds", stale_after);

	if (md_series_ready(context, notes))
	{
#ifdef VENTURE_HAVE_SQLITE
		g_autoptr(GPtrArray) sources = NULL;
		g_autoptr(GPtrArray) deals = NULL;
		g_autoptr(GHashTable) category_counts = NULL;
		gboolean by_trade = (NULL != query->sort) && (0 != g_strcmp0(query->sort, "deal"));
		/* Selling at one venue, or only where it sells fast enough,
		 * throws away every deal that has no price there, so the pool
		 * is the wide one, as for a sort. */
		gboolean wide = by_trade || !venture_string_is_empty(query->sell_venue) ||
		                !isnan(query->min_sold_per_day);
		/* Sorting by it needs it for the whole pool; otherwise only the
		 * rows kept are realised, after the cut. */
		gboolean by_realisable = (0 == g_strcmp0(query->sort, "realisable"));
		g_autoptr(MdRefs) refs = NULL;
		gint64 totals_realisable = 0;
		gint64 totals_realisable_rows = 0;
		gboolean totals_realisable_overflow = FALSE;
		g_autoptr(GHashTable) choices = g_hash_table_new(g_direct_hash, g_direct_equal);
		g_autoptr(GPtrArray) readers = NULL;
		g_autoptr(GPtrArray) venue_lists = NULL;
		g_autoptr(MdRealms) realms = NULL;
		g_autoptr(GHashTable) group_realms = NULL;
		MdRealm *buy_realm = NULL;
		MdRealm *sell_realm = NULL;
		const gchar *buy_wanted = venture_string_is_empty(query->venue) ? NULL : query->venue;
		const gchar *sell_wanted = venture_string_is_empty(query->sell_venue) ? NULL : query->sell_venue;
		gboolean buy_aside = FALSE;
		gboolean sell_aside = FALSE;
		gboolean buy_used = FALSE;
		gboolean sell_used = FALSE;
		g_autofree gchar *totals_currency = NULL;
		gint64 totals_investment = 0;
		gint64 totals_profit = 0;
		gint64 totals_rows = 0;
		gboolean totals_mixed = FALSE;
		gboolean truncated;
		guint i;


		if (query->data_source_id > 0)
		{
			VentureEntity *one;

			one = md_source(context, query->organization_id, query->data_source_id, error);

			if (NULL == one)
				goto fail;

			sources = g_ptr_array_new_with_free_func(g_object_unref);
			g_ptr_array_add(sources, one);
		}
		else
		{
			sources = md_sources(context, query->organization_id, error);

			if (NULL == sources)
				goto fail;

			if (0 == sources->len)
				md_note(notes, "No data sources yet: add one under Market data feeds.");
		}

		json_object_set_array_member(root, "sources", md_sources_json(sources));
		json_object_set_array_member(root, "venue_groups",
		                             md_venue_groups_json(context, query->organization_id));
		deals = g_ptr_array_new_with_free_func(md_deal_free);
		truncated = FALSE;
		category_counts = md_categories_new();

		/*
		 * Every source's venues first, so the pickers and a picked realm
		 * see the connected realms all of them describe (MdRealm): the
		 * same auction house is a Blizzard connected-realm id in one
		 * store and a TSM realm slug in another.
		 */
		readers = g_ptr_array_new_with_free_func(md_unref_reader);
		venue_lists = g_ptr_array_new_with_free_func(md_unref_venues);
		realms = md_realms_new();

		for (i = 0; i < sources->len; i++)
		{
			VentureSeriesStore *one = md_reader(context, g_ptr_array_index(sources, i), notes, NULL);
			GPtrArray *venues = (NULL != one) ? venture_series_store_list_venues(one, NULL) : NULL;

			g_ptr_array_add(readers, one);
			g_ptr_array_add(venue_lists, venues);
			md_realms_add(realms, venture_entity_get_id(g_ptr_array_index(sources, i)), venues);
		}

		md_realms_build(realms);

		{
			g_autofree gchar *label = NULL;

			if (!md_realms_of_group(context, query->organization_id, realms, sources, venue_lists,
			                        query->venue_group, notes, &group_realms, &label, error))
				goto fail;

			if (NULL != group_realms)
				md_set_text(root, "venue_group_name", label);
		}

		/* A picked venue is its connected realm, whichever member's name
		 * or source's key named it. */
		if (NULL != buy_wanted)
			buy_realm = md_realms_find(realms, buy_wanted);
		if (NULL != sell_wanted)
			sell_realm = md_realms_find(realms, sell_wanted);

		/*
		 * Each source answers its own best count+1 deals from the deal
		 * index; the merge keeps the best count of all of them, and the
		 * extra one says whether anything was cut.
		 */
		for (i = 0; i < sources->len; i++)
		{
			VentureEntity *source = g_ptr_array_index(sources, i);
			gint64 source_id = venture_entity_get_id(source);
			VentureSeriesStore *reader = g_ptr_array_index(readers, i);
			GPtrArray *venues = g_ptr_array_index(venue_lists, i);
			g_autoptr(GPtrArray) found = NULL;
			g_autoptr(GError) local_error = NULL;
			g_auto(GStrv) group_keys = NULL;
			g_auto(GStrv) buy_keys = NULL;
			g_auto(GStrv) sell_keys = NULL;
			VentureSeriesFilter filter;
			guint j;

			if (NULL == reader)
				continue;

			json_object_set_boolean_member(root, "available", TRUE);
			md_categories_add(category_counts, reader);
			venture_series_filter_init(&filter);

			/* A group of realms is a set of venue keys in this store,
			 * not another: every one this source has for the
			 * connected realms the group names. */
			if (NULL != group_realms)
			{
				group_keys = md_realms_group_keys(group_realms, source_id);
				filter.venue_keys = (const gchar **)group_keys;
			}

			/* The pickers' choices: one per connected realm. */
			for (j = 0; (NULL != venues) && (j < venues->len); j++)
			{
				VentureSeriesVenueRow *venue = g_ptr_array_index(venues, j);
				MdRealm *realm = md_realms_of(realms, source_id, venue->key);

				if ((NULL != group_keys) && !g_strv_contains((const gchar *const *)group_keys, venue->key))
					continue;

				if (NULL != realm)
					g_hash_table_add(choices, realm);
			}

			/* The picked realms in this store's keys -- several when a
			 * source keeps each member realm apart. A store without the
			 * one to buy or sell at has no deal on that route; one
			 * outside the group (left over from the group picked
			 * before) is set aside rather than empty the page. */
			buy_keys = md_realm_keys(buy_realm, source_id);
			sell_keys = md_realm_keys(sell_realm, source_id);

			if (((NULL != buy_wanted) && (NULL == buy_keys)) || ((NULL != sell_wanted) && (NULL == sell_keys)))
				continue;

			if (NULL != buy_keys)
			{
				buy_keys = md_keys_in_group(buy_keys, group_keys);
				buy_aside = buy_aside || (NULL == buy_keys);
			}

			if (NULL != sell_keys)
			{
				sell_keys = md_keys_in_group(sell_keys, group_keys);
				sell_aside = sell_aside || (NULL == sell_keys);
			}

			filter.search = venture_string_is_empty(query->search) ? NULL : query->search;
			filter.deals_only = TRUE;
			filter.max_pct_vs_region = query->max_pct;
			filter.sort = VENTURE_SERIES_SORT_PCT_VS_REGION;

			/* By deal, the index's own best count+1 is the answer. By
			 * profit or return it is not: the cheapest against the
			 * region are mostly things that sell nowhere, so a wide
			 * pool of deals is priced and the best of those kept. */
			filter.count = wide ? VENTURE_SERIES_MAX_PAGE : count + 1;

			if (NULL != buy_keys)
				filter.venue_keys = (const gchar **)buy_keys;

			buy_used = buy_used || (NULL != buy_keys);
			sell_used = sell_used || (NULL != sell_keys);

			filter.group_key = venture_string_is_empty(query->group_key) ? NULL : query->group_key;
			filter.category_prefix = venture_string_is_empty(query->category) ? NULL : query->category;

			if (NULL != query->min_value)
			{
				filter.min_value = venture_money_get_amount(query->min_value);
				filter.min_value_currency = venture_money_get_currency(query->min_value);
			}

			found = venture_series_store_list_current(reader, &filter, &local_error);

			if (NULL == found)
			{
				g_propagate_error(error, g_steal_pointer(&local_error));
				goto fail;
			}

			if (found->len > count)
				truncated = TRUE;

			/* Moved, not copied; the merge sorts them anyway. */
			for (j = found->len; j > 0; j--)
			{
				MdDeal *deal;

				deal = g_new0(MdDeal, 1);
				deal->row = g_ptr_array_steal_index_fast(found, j - 1);
				deal->data_source_id = source_id;
				deal->source_index = i;
				deal->source_name = md_source_name(source);

				/* Read while this source's store is open: the merge
				 * outlives it. */
				deal->display = json_object_new();
				md_row_set_display(deal->display, reader, deal->row->instrument_key);
				deal->source_index = i;
				md_deal_sell_side(deal, reader,
				                  (const gchar *const *)((NULL != sell_keys) ? sell_keys : group_keys),
				                  query->cut_pct, query->min_sold_per_day);

				deal->buy_label = md_realm_label(realms, source_id, deal->row->venue_key);
				if (NULL != deal->sell)
					deal->sell_label = md_realm_label(realms, source_id, deal->sell->venue_key);

				/* Asked to sell at one venue, or only where it sells
				 * fast enough, a deal that cannot be sold there is not
				 * one. */
				if (((NULL != sell_keys) || !isnan(query->min_sold_per_day)) && (NULL == deal->sell))
				{
					md_deal_free(deal);
					continue;
				}

				if (by_realisable)
					md_deal_realise(deal, reader, query->horizon_days);

				g_ptr_array_add(deals, deal);
			}
		}

		md_deal_sort = by_trade ? query->sort : NULL;
		md_deal_descending = (NULL != query->dir) ? (0 == g_strcmp0(query->dir, "desc"))
		                                          : (by_trade && md_deal_sort_descends(query->sort));
		g_ptr_array_sort(deals, md_deal_compare);
		md_deal_sort = NULL;
		md_deal_descending = FALSE;

		if (deals->len > count)
		{
			truncated = TRUE;
			g_ptr_array_set_size(deals, count);
		}

		/* Another source's view of the kept rows only: a handful of
		 * indexed reads each, never the pool's. */
		if (deals->len > 0)
			refs = md_refs_new(context, query->organization_id);

		for (i = 0; i < deals->len; i++)
		{
			MdDeal *deal = g_ptr_array_index(deals, i);
			JsonObject *object;
			JsonObject *ref;
			gint64 discount;

			md_deal_realise(deal, g_ptr_array_index(readers, deal->source_index), query->horizon_days);

			object = md_row_object(deal->row, deal->data_source_id);
			if (json_object_has_member(deal->display, "display"))
				json_object_set_member(object, "display",
				                       json_node_copy(json_object_get_member(deal->display, "display")));
			if (json_object_has_member(deal->display, "level"))
				json_object_set_member(object, "level",
				                       json_node_copy(json_object_get_member(deal->display, "level")));
			json_object_set_int_member(object, "data_source_id", deal->data_source_id);
			json_object_set_string_member(object, "source_name", deal->source_name);

			/* Exact: both sides are minor units in the row's currency. */
			if (venture_series_math_add(deal->row->deal_price, -deal->row->min_price, &discount))
				md_set_money(object, "discount", discount, deal->row->currency);
			else
				json_object_set_null_member(object, "discount");

			md_set_ratio(object, "discount_pct", 100.0 - deal->row->pct_vs_region);

			/* Each side's price is as old as the snapshot it came from;
			 * a page must be able to say so beside the realm. */
			md_set_age(object, "buy_", deal->row->taken_at, now, stale_after);
			md_set_text(object, "realm", deal->buy_label);

			if (NULL != deal->sell)
			{
				JsonObject *sell = md_row_object(deal->sell, deal->data_source_id);

				md_set_age(sell, "", deal->sell->taken_at, now, stale_after);
				md_set_text(sell, "realm", deal->sell_label);
				json_object_set_object_member(object, "sell", sell);
				md_set_age(object, "sell_", deal->sell->taken_at, now, stale_after);
				md_set_money(object, "profit", deal->profit, deal->row->currency);
				md_set_ratio(object, "roi_pct", deal->roi);

				/* How fast it goes where it is sold, and what that
				 * leaves of the profit over the horizon. */
				md_set_ratio(object, "sell_sold_per_day", deal->sell->sold_per_day);
				md_set_ratio(object, "sell_sale_rate", deal->sell->sale_rate);
				md_set_figure(object, "expected_sales", deal->expected);

				if (VENTURE_SERIES_NONE != deal->expected)
				{
					md_set_money(object, "realisable_profit", deal->realisable.profit, deal->row->currency);
					md_set_money(object, "realisable_cost", deal->realisable.cost, deal->row->currency);
					json_object_set_int_member(object, "realisable_units", deal->realisable.units);
					json_object_set_int_member(object, "book_units", deal->realisable.book_units);
				}
				else
				{
					json_object_set_null_member(object, "realisable_profit");
					json_object_set_null_member(object, "realisable_cost");
					json_object_set_null_member(object, "realisable_units");
					json_object_set_null_member(object, "book_units");
				}

				/* One of each: what buying every row once costs, sells
				 * for after the cut, and makes. */
				if (NULL == totals_currency)
					totals_currency = g_strdup(deal->row->currency);

				if (0 == g_strcmp0(totals_currency, deal->row->currency))
				{
					totals_investment += deal->row->min_price;
					totals_profit += deal->profit;
					totals_rows++;

					if ((VENTURE_SERIES_NONE != deal->expected) &&
					    !venture_series_math_add(totals_realisable, deal->realisable.profit,
					                             &totals_realisable))
						totals_realisable_overflow = TRUE;
					else if (VENTURE_SERIES_NONE != deal->expected)
						totals_realisable_rows++;
				}
				else
					totals_mixed = TRUE;
			}

			ref = md_ref_object(refs, deal->data_source_id, deal->row, deal->sell, now, stale_after);

			if (NULL != ref)
				json_object_set_object_member(object, "ref", ref);

			json_array_add_object_element(rows, object);
		}

		/* Each row's last seven days at its buy venue: one read per
		 * source for every row it gave, after the merge, so only the
		 * rows shown are read. */
		for (i = 0; i < readers->len; i++)
		{
			g_autoptr(GPtrArray) store_rows = g_ptr_array_new();
			g_autoptr(GPtrArray) objects = g_ptr_array_new();
			guint j;

			for (j = 0; j < deals->len; j++)
			{
				MdDeal *deal = g_ptr_array_index(deals, j);

				if (deal->source_index != i)
					continue;

				g_ptr_array_add(store_rows, deal->row);
				g_ptr_array_add(objects, json_array_get_object_element(rows, j));
			}

			if ((store_rows->len > 0) &&
			    !md_recent_rows(g_ptr_array_index(readers, i), (VentureSeriesRow **)store_rows->pdata,
			                    (JsonObject **)objects->pdata, store_rows->len, now, error))
				goto fail;
		}

		json_object_set_double_member(root, "cut_pct", query->cut_pct);
		json_object_set_double_member(root, "horizon_days", query->horizon_days);
		md_set_ratio(root, "min_sold_per_day", query->min_sold_per_day);
		json_object_set_array_member(root, "venue_choices", md_venue_choices_json(choices, now, stale_after));

		/* Set aside only where no source could use it. */
		buy_aside = buy_aside && !buy_used;
		sell_aside = sell_aside && !sell_used;

		if (buy_aside)
			md_note(notes, "The buy venue is not in this venue group, so every venue of the group is "
			               "bought at.");

		if (sell_aside)
			md_note(notes, "The sell venue is not in this venue group, so the dearest venue of the "
			               "group is sold at.");

		/* Echoed as the picker spells it, so the page selects it
		 * whichever member's name or key was asked for. */
		md_set_text(root, "buy_venue", buy_aside ? NULL : (NULL != buy_realm) ? buy_realm->label : buy_wanted);
		md_set_text(root, "sell_venue", sell_aside ? NULL : (NULL != sell_realm) ? sell_realm->label : sell_wanted);

		if ((totals_rows > 0) && !totals_mixed)
		{
			JsonObject *totals = json_object_new();

			md_set_money(totals, "investment", totals_investment, totals_currency);
			md_set_money(totals, "sale", totals_investment + totals_profit, totals_currency);
			md_set_money(totals, "profit", totals_profit, totals_currency);
			json_object_set_int_member(totals, "rows", totals_rows);
			/* Over the rows that have one; a sum past 64 bits is no
			 * sum, not a wrapped one. */
			md_set_money(totals, "realisable_profit",
			             ((totals_realisable_rows > 0) && !totals_realisable_overflow)
			             ? totals_realisable : VENTURE_SERIES_NONE, totals_currency);
			json_object_set_int_member(totals, "realisable_rows", totals_realisable_rows);
			json_object_set_object_member(root, "totals", totals);
		}

		json_object_set_boolean_member(root, "truncated", truncated);
		json_object_set_array_member(root, "categories", md_categories_json(category_counts));
#endif
	}

	if (!json_object_has_member(root, "sources"))
		json_object_set_array_member(root, "sources", json_array_new());
	if (!json_object_has_member(root, "categories"))
		json_object_set_array_member(root, "categories", json_array_new());
	if (!json_object_has_member(root, "venue_choices"))
		json_object_set_array_member(root, "venue_choices", json_array_new());

	json_object_set_array_member(root, "rows", rows);
	json_object_set_array_member(root, "notes", notes);
	md_attribute_member(context, query->organization_id, root, "rows");

	return md_node(root);

#ifdef VENTURE_HAVE_SQLITE
fail:
	json_object_unref(root);
	json_array_unref(notes);
	json_array_unref(rows);

	return NULL;
#endif
}

/* --- The venue index ------------------------------------------------------- */

JsonNode *
venture_marketdata_venue_index(
	VentureContext	 *context,
	gint64		  organization_id,
	gint64		  data_source_id,
	const gchar	 *group_key,
	gint64		  now,
	GError		**error
){
	JsonObject *root;
	JsonArray *notes;
	JsonArray *venues;
	JsonArray *groups;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	if (!md_require_module(context, error))
		return NULL;

	now = md_now(now);
	root = json_object_new();
	notes = json_array_new();
	venues = json_array_new();
	groups = json_array_new();
	json_object_set_boolean_member(root, "available", FALSE);
	md_set_text(root, "group_key", venture_string_is_empty(group_key) ? NULL : group_key);

	if (md_series_ready(context, notes))
	{
#ifdef VENTURE_HAVE_SQLITE
		g_autoptr(GPtrArray) sources = NULL;
		g_autoptr(GHashTable) seen_groups = NULL;
		guint i;

		if (data_source_id > 0)
		{
			VentureEntity *one;

			one = md_source(context, organization_id, data_source_id, error);

			if (NULL == one)
				goto fail;

			sources = g_ptr_array_new_with_free_func(g_object_unref);
			g_ptr_array_add(sources, one);
		}
		else
		{
			sources = md_sources(context, organization_id, error);

			if (NULL == sources)
				goto fail;

			if (0 == sources->len)
				md_note(notes, "No data sources yet: add one under Market data feeds.");
		}

		json_object_set_array_member(root, "sources", md_sources_json(sources));
		seen_groups = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

		for (i = 0; i < sources->len; i++)
		{
			VentureEntity *source = g_ptr_array_index(sources, i);
			g_autoptr(VentureSeriesStore) reader = NULL;
			g_autoptr(GPtrArray) stats = NULL;
			g_autofree gchar *source_name = NULL;
			guint j;

			reader = md_reader(context, source, notes, NULL);

			if (NULL == reader)
				continue;

			json_object_set_boolean_member(root, "available", TRUE);
			source_name = md_source_name(source);
			stats = venture_series_store_venue_index(reader,
				venture_string_is_empty(group_key) ? NULL : group_key, error);

			if (NULL == stats)
				goto fail;

			for (j = 0; j < stats->len; j++)
			{
				VentureSeriesVenueStats *venue = g_ptr_array_index(stats, j);
				VentureSeriesVenueState state;
				JsonObject *object;
				gint64 age;

				if (!venture_series_store_get_venue_state(reader, venue->venue_key, &state, error))
					goto fail;

				object = json_object_new();
				json_object_set_int_member(object, "data_source_id", venture_entity_get_id(source));
				json_object_set_string_member(object, "source_name", source_name);
				json_object_set_string_member(object, "venue_key", venue->venue_key);
				md_set_text(object, "venue_name", venue->venue_name);
				json_object_set_string_member(object, "group_key",
				                              (NULL != venue->group_key) ? venue->group_key : "");
				json_object_set_int_member(object, "instruments", venue->instruments);
				json_object_set_int_member(object, "cheaper", venue->cheaper);
				json_object_set_int_member(object, "equal", venue->equal);
				json_object_set_int_member(object, "dearer", venue->dearer);

				/* Shares of the instruments that have a region to be
				 * compared with; none, and there is no share to give. */
				if (venue->instruments > 0)
				{
					json_object_set_double_member(object, "pct_cheaper",
						100.0 * (gdouble)venue->cheaper / (gdouble)venue->instruments);
					json_object_set_double_member(object, "pct_equal",
						100.0 * (gdouble)venue->equal / (gdouble)venue->instruments);
					json_object_set_double_member(object, "pct_dearer",
						100.0 * (gdouble)venue->dearer / (gdouble)venue->instruments);
				}
				else
				{
					json_object_set_null_member(object, "pct_cheaper");
					json_object_set_null_member(object, "pct_equal");
					json_object_set_null_member(object, "pct_dearer");
				}

				md_set_ratio(object, "avg_ratio", venue->avg_ratio);
				json_object_set_int_member(object, "listings", venue->listings);
				md_set_time(object, "last_taken_at", venue->last_taken_at);
				md_set_time(object, "last_fetched_at", state.found ? state.last_fetched_at
				                                                   : VENTURE_SERIES_NONE);
				json_object_set_int_member(object, "interval_seconds", venue->interval_seconds);
				json_object_set_int_member(object, "gaps", state.found ? state.gaps : 0);
				md_set_time(object, "next_expected", state.found ? state.next_expected
				                                                 : VENTURE_SERIES_NONE);

				age = (VENTURE_SERIES_NONE != venue->last_taken_at) ? now - venue->last_taken_at
				                                                    : VENTURE_SERIES_NONE;
				md_set_figure(object, "age_seconds", age);

				/* Late: twice its own rhythm without a new snapshot. */
				json_object_set_boolean_member(object, "overdue",
					(VENTURE_SERIES_NONE != age) && (venue->interval_seconds > 0) &&
					(age > 2 * venue->interval_seconds));
				json_array_add_object_element(venues, object);

				if ((NULL != venue->group_key) && ('\0' != venue->group_key[0]) &&
				    !g_hash_table_contains(seen_groups, venue->group_key))
				{
					g_hash_table_add(seen_groups, g_strdup(venue->group_key));
					json_array_add_string_element(groups, venue->group_key);
				}
			}
		}
#endif
	}

	if (!json_object_has_member(root, "sources"))
		json_object_set_array_member(root, "sources", json_array_new());

	json_object_set_array_member(root, "groups", groups);
	json_object_set_array_member(root, "venues", venues);
	json_object_set_array_member(root, "notes", notes);
	md_attribute_member(context, organization_id, root, "venues");

	return md_node(root);

#ifdef VENTURE_HAVE_SQLITE
fail:
	json_object_unref(root);
	json_array_unref(notes);
	json_array_unref(venues);
	json_array_unref(groups);

	return NULL;
#endif
}

/* --- Watchlists ------------------------------------------------------------ */

/* A record of @type in the organization, live, or NOT_FOUND naming it. */
static VentureEntity *
md_record(
	VentureContext	 *context,
	GType		  type,
	gint64		  organization_id,
	gint64		  id,
	const gchar	 *label,
	GError		**error
){
	g_autoptr(VentureEntity) record = NULL;

	if (id > 0)
		record = venture_database_get(venture_context_get_database(context), type, id, NULL);

	if ((NULL == record) || venture_entity_is_deleted(record) ||
	    (venture_entity_get_organization_id(record) != organization_id))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "No %s #%" G_GINT64_FORMAT " in this organization", label, id);
		return NULL;
	}

	return g_steal_pointer(&record);
}

JsonNode *
venture_marketdata_watchlists(
	VentureContext	 *context,
	gint64		  organization_id,
	GError		**error
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) lists = NULL;
	JsonObject *root;
	JsonArray *array;
	guint i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	if (!md_require_module(context, error))
		return NULL;

	query = venture_query_new(VENTURE_TYPE_WATCHLIST);
	venture_query_set_organization(query, organization_id);
	venture_query_add_order(query, "name", VENTURE_SORT_ASCENDING, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	venture_query_set_limit(query, MD_MAX_SOURCES);
	lists = venture_database_find(venture_context_get_database(context), query, error);

	if (NULL == lists)
		return NULL;

	root = json_object_new();
	array = json_array_new();

	for (i = 0; i < lists->len; i++)
	{
		VentureEntity *list = g_ptr_array_index(lists, i);
		g_autoptr(VentureQuery) entries = NULL;
		g_autofree gchar *name = NULL;
		g_autofree gchar *group = NULL;
		JsonObject *object;
		gint64 count = 0;

		g_object_get(list, "name", &name, "group-key", &group, NULL);
		entries = venture_query_new(VENTURE_TYPE_WATCHLIST_ENTRY);
		venture_query_set_organization(entries, organization_id);

		if (venture_query_add_filter_int(entries, "watchlist-id", VENTURE_FILTER_OP_EQ,
		                                 venture_entity_get_id(list), NULL))
			count = venture_database_count(venture_context_get_database(context), entries, NULL);

		object = json_object_new();
		json_object_set_int_member(object, "id", venture_entity_get_id(list));
		json_object_set_string_member(object, "name", (NULL != name) ? name : "");
		json_object_set_string_member(object, "group_key", (NULL != group) ? group : "");
		json_object_set_int_member(object, "entries", MAX(count, 0));
		json_array_add_object_element(array, object);
	}

	json_object_set_array_member(root, "watchlists", array);

	/* Names and counts of the organization's own lists: no source's
	 * data, so nothing to attribute. */
	venture_marketdata_attribution_set(root, NULL);

	return md_node(root);
}

#ifdef VENTURE_HAVE_SQLITE

/* The difference @price - @target in the same currency, or null. */
static void
md_set_delta(
	JsonObject		*object,
	const gchar		*name,
	gint64			 price,
	const gchar		*currency,
	const VentureMoney	*target
){
	g_autoptr(VentureMoney) here = NULL;
	g_autoptr(VentureMoney) delta = NULL;

	if ((NULL == target) || (VENTURE_SERIES_NONE == price) || venture_string_is_empty(currency))
	{
		json_object_set_null_member(object, name);
		return;
	}

	here = venture_money_new_for_currency(price, currency);

	/* A target in another currency is not compared: there is no rate
	 * here, and the oracle never converts. */
	if ((NULL == here) || (0 != g_strcmp0(venture_money_get_currency(target), currency)))
	{
		json_object_set_null_member(object, name);
		return;
	}

	delta = venture_money_subtract(here, target, NULL);

	if (NULL == delta)
		json_object_set_null_member(object, name);
	else
		json_object_set_member(object, name, venture_money_to_json(delta));
}

#endif

JsonNode *
venture_marketdata_watchlist_view(
	VentureContext	 *context,
	gint64		  organization_id,
	gint64		  watchlist_id,
	gint64		  now,
	GError		**error
){
	g_autoptr(VentureEntity) list = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) entries = NULL;
	g_autoptr(GHashTable) readers = NULL;
	g_autofree gchar *name = NULL;
	g_autofree gchar *group = NULL;
	JsonObject *root;
	JsonObject *object;
	JsonArray *notes;
	JsonArray *array;
	gboolean ready;
	guint i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	if (!md_require_module(context, error))
		return NULL;

	list = md_record(context, VENTURE_TYPE_WATCHLIST, organization_id, watchlist_id, "watchlist",
	                 error);

	if (NULL == list)
		return NULL;

	query = venture_query_new(VENTURE_TYPE_WATCHLIST_ENTRY);
	venture_query_set_organization(query, organization_id);

	if (!venture_query_add_filter_int(query, "watchlist-id", VENTURE_FILTER_OP_EQ, watchlist_id,
	                                  error))
		return NULL;

	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	venture_query_set_limit(query, MD_MAX_ENTRIES);
	entries = venture_database_find(venture_context_get_database(context), query, error);

	if (NULL == entries)
		return NULL;

	now = md_now(now);
	root = json_object_new();
	notes = json_array_new();
	array = json_array_new();
	g_object_get(list, "name", &name, "group-key", &group, NULL);

	object = json_object_new();
	json_object_set_int_member(object, "id", watchlist_id);
	json_object_set_string_member(object, "name", (NULL != name) ? name : "");
	json_object_set_string_member(object, "group_key", (NULL != group) ? group : "");
	json_object_set_object_member(root, "watchlist", object);

	ready = md_series_ready(context, notes);
	json_object_set_boolean_member(root, "available", ready);
	readers = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_object_unref);

	for (i = 0; i < entries->len; i++)
	{
		VentureEntity *entry = g_ptr_array_index(entries, i);
		g_autoptr(VentureEntity) instrument = NULL;
		g_autoptr(VentureMoney) target_buy = NULL;
		g_autoptr(VentureMoney) target_sell = NULL;
		g_autofree gchar *instrument_name = NULL;
		g_autofree gchar *key = NULL;
		gint64 instrument_id = 0;
		gint64 source_id = 0;
		JsonObject *item;

		g_object_get(entry, "instrument-id", &instrument_id, "target-buy", &target_buy,
		             "target-sell", &target_sell, NULL);
		instrument = md_record(context, VENTURE_TYPE_INSTRUMENT, organization_id, instrument_id,
		                       "instrument", NULL);

		if (NULL != instrument)
			g_object_get(instrument, "name", &instrument_name, "key", &key,
			             "data-source-id", &source_id, NULL);

		item = json_object_new();
		json_object_set_int_member(item, "id", venture_entity_get_id(entry));
		json_object_set_int_member(item, "instrument_id", instrument_id);
		md_set_text(item, "instrument_name", instrument_name);
		md_set_text(item, "key", key);
		json_object_set_int_member(item, "data_source_id", source_id);

		if (NULL != target_buy)
			json_object_set_member(item, "target_buy", venture_money_to_json(target_buy));
		else
			json_object_set_null_member(item, "target_buy");

		if (NULL != target_sell)
			json_object_set_member(item, "target_sell", venture_money_to_json(target_sell));
		else
			json_object_set_null_member(item, "target_sell");

		json_object_set_null_member(item, "best");
		json_object_set_null_member(item, "buy_delta");
		json_object_set_null_member(item, "sell_delta");
		json_object_set_boolean_member(item, "buy_now", FALSE);
		json_object_set_boolean_member(item, "sell_now", FALSE);
		json_object_set_array_member(item, "venues", json_array_new());
		json_object_set_array_member(item, "trend", json_array_new());
		json_object_set_null_member(item, "trend_currency");
		json_object_set_null_member(item, "url");
		json_object_set_null_member(item, "problem");

#ifdef VENTURE_HAVE_SQLITE
		if (ready && (source_id > 0) && !venture_string_is_empty(key))
		{
			VentureSeriesStore *reader;
			g_autoptr(GPtrArray) rows = NULL;
			g_autoptr(GError) local_error = NULL;
			VentureSeriesRow *best = NULL;
			JsonArray *venues;
			guint j;

			reader = g_hash_table_lookup(readers, &source_id);

			if (NULL == reader)
			{
				g_autoptr(VentureEntity) source = NULL;

				source = md_source(context, organization_id, source_id, &local_error);
				reader = (NULL != source) ? md_reader(context, source, NULL, &local_error) : NULL;

				if (NULL != reader)
				{
					gint64 *id = g_new(gint64, 1);

					*id = source_id;
					g_hash_table_insert(readers, id, reader);
				}
			}

			if (NULL != reader)
				rows = venture_series_store_other_venues(reader, key,
				                                         venture_string_is_empty(group) ? NULL : group,
				                                         &local_error);

			if (NULL == rows)
			{
				json_object_set_string_member(item, "problem",
				                              (NULL != local_error) ? local_error->message
				                                                    : "The store cannot be read");
			}
			else
			{
				g_autofree gchar *url = NULL;

				venues = json_object_get_array_member(item, "venues");

				for (j = 0; j < rows->len; j++)
				{
					VentureSeriesRow *row = g_ptr_array_index(rows, j);
					JsonObject *venue;

					if ((NULL == best) && (VENTURE_SERIES_NONE != row->min_price) &&
					    (0 != row->quantity))
						best = row;

					venue = json_object_new();
					json_object_set_string_member(venue, "venue_key", row->venue_key);
					md_set_text(venue, "venue_name", row->venue_name);
					md_set_money(venue, "min_price", row->min_price, row->currency);
					md_set_figure(venue, "quantity", row->quantity);
					md_set_time(venue, "taken_at", row->taken_at);
					json_array_add_object_element(venues, venue);
				}

				url = venture_marketdata_instrument_path(source_id, key,
				                                         (NULL != best) ? best->venue_key : NULL);
				json_object_set_string_member(item, "url", url);

				if (NULL != best)
				{
					g_autoptr(GArray) days = NULL;
					JsonObject *summary;
					JsonArray *trend;

					summary = json_object_new();
					json_object_set_string_member(summary, "venue_key", best->venue_key);
					md_set_text(summary, "venue_name", best->venue_name);
					md_set_money(summary, "min_price", best->min_price, best->currency);
					md_set_figure(summary, "quantity", best->quantity);
					md_set_time(summary, "taken_at", best->taken_at);
					json_object_set_object_member(item, "best", summary);

					md_set_delta(item, "buy_delta", best->min_price, best->currency, target_buy);
					md_set_delta(item, "sell_delta", best->min_price, best->currency, target_sell);

					/* At or under the buy target is a buy now; at or over
					 * the sell target, listing now undercuts nobody. */
					if ((NULL != target_buy) &&
					    (0 == g_strcmp0(venture_money_get_currency(target_buy), best->currency)))
					{
						g_autoptr(VentureMoney) here = venture_money_new_for_currency(best->min_price,
						                                                              best->currency);

						json_object_set_boolean_member(item, "buy_now",
							(NULL != here) && (venture_money_compare(here, target_buy) <= 0));
					}

					if ((NULL != target_sell) &&
					    (0 == g_strcmp0(venture_money_get_currency(target_sell), best->currency)))
					{
						g_autoptr(VentureMoney) here = venture_money_new_for_currency(best->min_price,
						                                                              best->currency);

						json_object_set_boolean_member(item, "sell_now",
							(NULL != here) && (venture_money_compare(here, target_sell) >= 0));
					}

					/* The sparkline: each day's lowest at the cheapest
					 * venue, in its currency, a null where there was none. */
					days = venture_series_store_daily(reader, best->venue_key, key,
					                                  now - (MD_TREND_DAYS - 1) * MD_DAY, NULL);
					trend = json_object_get_array_member(item, "trend");
					json_object_set_string_member(item, "trend_currency", best->currency);

					for (j = 0; (NULL != days) && (j < days->len); j++)
					{
						const VentureSeriesDay *day = &g_array_index(days, VentureSeriesDay, j);

						if ((VENTURE_SERIES_NONE == day->min_price) ||
						    (0 != g_strcmp0(day->currency, best->currency)))
							json_array_add_null_element(trend);
						else
							json_array_add_int_element(trend, day->min_price);
					}
				}
			}
		}
		else if (ready)
			json_object_set_string_member(item, "problem",
			                              "Not linked to a data source: promote it from a "
			                              "source's instrument page");
#endif

		json_array_add_object_element(array, item);
	}

	(void)now;
	json_object_set_array_member(root, "entries", array);
	json_object_set_array_member(root, "notes", notes);

	/* An entry shows its source's data only when the store gave it
	 * venues; one linked to a source with nothing stored shows none. */
	{
		g_autoptr(GPtrArray) attributions = g_ptr_array_new_with_free_func(g_free);

		for (i = 0; i < json_array_get_length(array); i++)
		{
			JsonObject *entry = json_array_get_object_element(array, i);
			JsonNode *venues = json_object_get_member(entry, "venues");

			if ((NULL != venues) && JSON_NODE_HOLDS_ARRAY(venues) &&
			    (json_array_get_length(json_node_get_array(venues)) > 0))
				venture_marketdata_attribution_add_source(
					context, organization_id,
					json_object_get_int_member_with_default(entry, "data_source_id", 0),
					attributions);
		}

		venture_marketdata_attribution_set(root, attributions);
	}

	return md_node(root);
}

/* --- Alerts ---------------------------------------------------------------- */

JsonNode *
venture_marketdata_alerts_overview(
	VentureContext	 *context,
	gint64		  organization_id,
	guint		  count,
	GError		**error
){
	g_autoptr(VentureQuery) rules_query = NULL;
	g_autoptr(VentureQuery) hits_query = NULL;
	g_autoptr(GPtrArray) rules = NULL;
	g_autoptr(GPtrArray) hits = NULL;
	g_autoptr(GHashTable) names = NULL;
	VentureDatabase *database;
	JsonObject *root;
	JsonArray *array;
	guint i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	if (!md_require_module(context, error))
		return NULL;

	count = (0 == count) ? 50 : count;

	if (count > 500)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "The alerts overview lists at most 500 hits");
		return NULL;
	}

	database = venture_context_get_database(context);
	rules_query = venture_query_new(VENTURE_TYPE_ALERT_RULE);
	venture_query_set_organization(rules_query, organization_id);
	venture_query_add_order(rules_query, "name", VENTURE_SORT_ASCENDING, NULL);
	venture_query_add_order(rules_query, "id", VENTURE_SORT_ASCENDING, NULL);
	venture_query_set_limit(rules_query, MD_MAX_RULES);
	rules = venture_database_find(database, rules_query, error);

	if (NULL == rules)
		return NULL;

	hits_query = venture_query_new(VENTURE_TYPE_ALERT_HIT);
	venture_query_set_organization(hits_query, organization_id);
	venture_query_add_order(hits_query, "observed-at", VENTURE_SORT_DESCENDING, NULL);
	venture_query_add_order(hits_query, "id", VENTURE_SORT_DESCENDING, NULL);
	venture_query_set_limit(hits_query, count);
	hits = venture_database_find(database, hits_query, error);

	if (NULL == hits)
		return NULL;

	root = json_object_new();
	names = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);
	array = json_array_new();

	for (i = 0; i < rules->len; i++)
	{
		VentureEntity *rule = g_ptr_array_index(rules, i);
		g_autoptr(VentureQuery) last_query = NULL;
		g_autoptr(VentureEntity) last = NULL;
		g_autofree gchar *rule_name = NULL;
		JsonNode *node;
		JsonObject *object;
		gint64 *id;

		g_object_get(rule, "name", &rule_name, NULL);
		id = g_new(gint64, 1);
		*id = venture_entity_get_id(rule);
		g_hash_table_insert(names, id, g_strdup((NULL != rule_name) ? rule_name : ""));

		node = venture_serializable_to_json(VENTURE_SERIALIZABLE(rule), FALSE);
		object = json_node_get_object(node);

		/* When it last fired: one indexed row per rule. */
		last_query = venture_query_new(VENTURE_TYPE_ALERT_HIT);
		venture_query_set_organization(last_query, organization_id);

		if (venture_query_add_filter_int(last_query, "rule-id", VENTURE_FILTER_OP_EQ, *id, NULL))
		{
			venture_query_add_order(last_query, "observed-at", VENTURE_SORT_DESCENDING, NULL);
			venture_query_set_limit(last_query, 1);
			last = venture_database_find_one(database, last_query, NULL);
		}

		if (NULL != last)
		{
			g_autoptr(GDateTime) observed = NULL;
			g_autofree gchar *text = NULL;

			g_object_get(last, "observed-at", &observed, NULL);
			text = (NULL != observed) ? venture_time_to_string(observed) : NULL;
			md_set_text(object, "last_hit_at", text);
		}
		else
			json_object_set_null_member(object, "last_hit_at");

		json_array_add_element(array, node);
	}

	json_object_set_array_member(root, "rules", array);
	array = json_array_new();

	for (i = 0; i < hits->len; i++)
	{
		VentureEntity *hit = g_ptr_array_index(hits, i);
		JsonNode *node;
		JsonObject *object;
		const gchar *rule_name;
		g_autofree gchar *key = NULL;
		gint64 rule_id = 0;
		gint64 source_id = 0;

		g_object_get(hit, "rule-id", &rule_id, "data-source-id", &source_id,
		             "instrument-key", &key, NULL);
		node = venture_serializable_to_json(VENTURE_SERIALIZABLE(hit), FALSE);
		object = json_node_get_object(node);
		rule_name = g_hash_table_lookup(names, &rule_id);
		md_set_text(object, "rule_name", rule_name);

		/* Where to look: the instrument's page, when the hit names one. */
		if ((source_id > 0) && !venture_string_is_empty(key))
		{
			g_autofree gchar *venue = NULL;
			g_autofree gchar *path = NULL;

			g_object_get(hit, "venue-key", &venue, NULL);
			path = venture_marketdata_instrument_path(source_id, key, venue);
			json_object_set_string_member(object, "page", path);
		}
		else
			json_object_set_null_member(object, "page");

		json_array_add_element(array, node);
	}

	json_object_set_array_member(root, "hits", array);

	/* A hit is a reading of its source's data; a rule naming a source is
	 * only a setting, so the rules are not walked. */
	md_attribute_member(context, organization_id, root, "hits");

	return md_node(root);
}

/* --- Source health ----------------------------------------------------------- */

JsonNode *
venture_marketdata_source_health(
	VentureContext	 *context,
	gint64		  organization_id,
	gint64		  now,
	GError		**error
){
	JsonObject *root;
	JsonArray *notes;
	JsonArray *array;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	now = md_now(now);
	root = json_object_new();
	notes = json_array_new();
	array = json_array_new();
	json_object_set_boolean_member(root, "available", FALSE);

	if (md_series_ready(context, notes))
	{
#ifdef VENTURE_HAVE_SQLITE
		g_autoptr(GPtrArray) sources = NULL;
		g_autoptr(JsonNode) status = NULL;
		VentureFeedsService *service;
		JsonArray *workers;
		guint i;

		sources = md_sources(context, organization_id, error);

		if (NULL == sources)
		{
			json_object_unref(root);
			json_array_unref(notes);
			json_array_unref(array);
			return NULL;
		}

		json_object_set_boolean_member(root, "available", TRUE);

		if (0 == sources->len)
			md_note(notes, "No data sources yet: add one under Market data feeds.");

		service = venture_context_get_feeds_service(context);
		status = (NULL != service) ? venture_feeds_service_dup_status(service) : NULL;
		workers = ((NULL != status) && JSON_NODE_HOLDS_OBJECT(status))
			? json_object_get_array_member(json_node_get_object(status), "sources") : NULL;

		for (i = 0; i < sources->len; i++)
		{
			VentureEntity *source = g_ptr_array_index(sources, i);
			g_autoptr(VentureQuery) runs = NULL;
			g_autoptr(VentureEntity) last = NULL;
			g_autoptr(VentureSeriesStore) reader = NULL;
			g_autoptr(GPtrArray) venues = NULL;
			g_autofree gchar *name = NULL;
			g_autofree gchar *provider = NULL;
			g_autofree gchar *schedule = NULL;
			JsonObject *object;
			JsonObject *worker = NULL;
			gboolean enabled = FALSE;
			gint64 next_check = 0;
			gint64 newest = VENTURE_SERIES_NONE;
			gint64 stale = 0;
			guint j;

			name = md_source_name(source);
			g_object_get(source, "provider", &provider, "schedule", &schedule, "enabled", &enabled,
			             NULL);
			object = json_object_new();
			json_object_set_int_member(object, "id", venture_entity_get_id(source));
			json_object_set_string_member(object, "name", name);
			md_set_text(object, "provider", provider);
			json_object_set_boolean_member(object, "enabled", enabled);
			json_object_set_string_member(object, "schedule",
			                              venture_string_is_empty(schedule) ? "auto" : schedule);

			runs = venture_query_new(VENTURE_TYPE_DATA_SOURCE_RUN);
			venture_query_set_organization(runs, organization_id);

			if (venture_query_add_filter_int(runs, "data-source-id", VENTURE_FILTER_OP_EQ,
			                                 venture_entity_get_id(source), NULL))
			{
				venture_query_add_order(runs, "finished-at", VENTURE_SORT_DESCENDING, NULL);
				venture_query_add_order(runs, "id", VENTURE_SORT_DESCENDING, NULL);
				venture_query_set_limit(runs, 1);
				last = venture_database_find_one(venture_context_get_database(context), runs, NULL);
			}

			if (NULL != last)
			{
				g_autoptr(GDateTime) finished = NULL;
				g_autofree gchar *run_error = NULL;
				VentureDataSourceRunStatus run_status = VENTURE_DATA_SOURCE_RUN_STATUS_OK;
				gint64 rows = 0;
				JsonObject *run;

				g_object_get(last, "status", &run_status, "finished-at", &finished, "rows", &rows,
				             "error", &run_error, NULL);
				run = json_object_new();
				json_object_set_int_member(run, "id", venture_entity_get_id(last));
				json_object_set_string_member(run, "status",
					venture_enum_to_nick(VENTURE_TYPE_DATA_SOURCE_RUN_STATUS, (gint)run_status));
				md_set_time(run, "finished_at", (NULL != finished) ? g_date_time_to_unix(finished) : 0);
				json_object_set_int_member(run, "rows", rows);
				md_set_text(run, "error", venture_string_is_empty(run_error) ? NULL : run_error);
				json_object_set_object_member(object, "last_run", run);
			}
			else
				json_object_set_null_member(object, "last_run");

			for (j = 0; (NULL != workers) && (j < json_array_get_length(workers)); j++)
			{
				JsonObject *candidate = json_array_get_object_element(workers, j);

				if (json_object_get_int_member(candidate, "id") == venture_entity_get_id(source))
					worker = candidate;
			}

			/* The earliest of the source's units' next checks. */
			if ((NULL != worker) && json_object_has_member(worker, "units"))
			{
				JsonArray *units = json_object_get_array_member(worker, "units");

				for (j = 0; (NULL != units) && (j < json_array_get_length(units)); j++)
				{
					JsonNode *next = json_object_get_member(json_array_get_object_element(units, j),
					                                        "next_check");

					if ((NULL != next) && JSON_NODE_HOLDS_VALUE(next) &&
					    ((0 == next_check) || (json_node_get_int(next) < next_check)))
						next_check = json_node_get_int(next);
				}
			}

			md_set_time(object, "next_check", next_check);
			json_object_set_boolean_member(object, "in_flight",
				(NULL != worker) && json_object_has_member(worker, "in_flight") &&
				json_object_get_boolean_member(worker, "in_flight"));

			reader = md_reader(context, source, notes, NULL);
			venues = (NULL != reader) ? venture_series_store_list_venues(reader, NULL) : NULL;

			for (j = 0; (NULL != venues) && (j < venues->len) && (j < MD_HEALTH_VENUES); j++)
			{
				VentureSeriesVenueRow *venue = g_ptr_array_index(venues, j);
				VentureSeriesVenueState state;

				if (!venture_series_store_get_venue_state(reader, venue->key, &state, NULL) ||
				    !state.found || (VENTURE_SERIES_NONE == state.last_taken_at))
					continue;

				if ((VENTURE_SERIES_NONE == newest) || (state.last_taken_at > newest))
					newest = state.last_taken_at;

				if ((state.interval_seconds > 0) &&
				    (now - state.last_taken_at > 2 * state.interval_seconds))
					stale++;
			}

			json_object_set_int_member(object, "venues", (NULL != venues) ? venues->len : 0);
			json_object_set_int_member(object, "stale_venues", stale);
			md_set_time(object, "newest_snapshot", newest);
			json_array_add_object_element(array, object);
		}
#endif
	}

	json_object_set_array_member(root, "sources", array);
	json_object_set_array_member(root, "notes", notes);

	/* Health is about each source as a whole -- its venues, its newest
	 * snapshot -- so every source listed is named by its provider. */
	{
		g_autoptr(GPtrArray) attributions = g_ptr_array_new_with_free_func(g_free);
		guint k;

		for (k = 0; k < json_array_get_length(array); k++)
			venture_marketdata_attribution_add_provider(
				context,
				json_object_get_string_member_with_default(
					json_array_get_object_element(array, k), "provider", NULL),
				attributions);

		venture_marketdata_attribution_set(root, attributions);
	}

	return md_node(root);
}
