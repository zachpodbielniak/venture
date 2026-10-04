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

		if (NULL != reader)
		{
			venues = venture_series_store_list_venues(reader, &local_error);

			venture_series_filter_init(&filter);

			if (!venture_string_is_empty(query->venue))
			{
				venue_keys[0] = query->venue;
				venue_keys[1] = NULL;
				filter.venue_keys = venue_keys;
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

			for (i = 0; i < found->len; i++)
				json_array_add_object_element(rows, md_row_object(g_ptr_array_index(found, i),
				                                                  venture_entity_get_id(chosen)));
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

#endif /* VENTURE_HAVE_SQLITE */

JsonNode *
venture_marketdata_instrument(
	VentureContext				 *context,
	const VentureMarketdataInstrumentQuery	 *query,
	GError					**error
){
	JsonObject *root;
	JsonArray *notes;
	gint64 now;

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

	now = md_now(query->now);
	(void)now;
	root = json_object_new();
	notes = json_array_new();
	json_object_set_boolean_member(root, "available", FALSE);
	json_object_set_int_member(root, "data_source_id", query->data_source_id);
	json_object_set_string_member(root, "key", query->key);

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
		json_object_set_object_member(root, "instrument", object);
		json_object_set_int_member(root, "record_id",
			md_promoted_id(context, VENTURE_TYPE_INSTRUMENT, query->organization_id,
			               instrument->namespace_, instrument_namespace, instrument->key));

		/* Every venue's current row, cheapest first: the "other realms"
		 * table, and where the charted venue is chosen from. */
		venues = venture_series_store_other_venues(reader, query->key, NULL, error);

		if (NULL == venues)
			goto fail;

		row = NULL;

		for (i = 0; i < venues->len; i++)
		{
			VentureSeriesRow *candidate = g_ptr_array_index(venues, i);

			if (!venture_string_is_empty(query->venue))
			{
				if (0 == g_strcmp0(candidate->venue_key, query->venue))
					row = candidate;
			}
			else if ((NULL == row) && (VENTURE_SERIES_NONE != candidate->min_price) &&
			         (0 != candidate->quantity))
				row = candidate;
		}

		if (!venture_string_is_empty(query->venue) && (NULL == row))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
			            "Venue \"%s\" has never listed this instrument", query->venue);
			goto fail;
		}

		if ((NULL == row) && (venues->len > 0))
			row = g_ptr_array_index(venues, 0);

		array = json_array_new();

		for (i = 0; i < venues->len; i++)
			json_array_add_object_element(array, md_row_object(g_ptr_array_index(venues, i),
			                                                   query->data_source_id));

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
			md_set_time(object, "taken_at", row->taken_at);
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

/* --- Deals ----------------------------------------------------------------- */

void
venture_marketdata_deals_query_init(VentureMarketdataDealsQuery *query)
{
	g_return_if_fail(NULL != query);

	memset(query, 0, sizeof(*query));
	query->max_pct = NAN;
}

#ifdef VENTURE_HAVE_SQLITE

typedef struct
{
	VentureSeriesRow	*row;
	gint64			 data_source_id;
	gchar			*source_name;
} MdDeal;

static void
md_deal_free(gpointer data)
{
	MdDeal *deal = data;

	venture_series_row_free(deal->row);
	g_free(deal->source_name);
	g_free(deal);
}

/* Cheapest against the region first, then by source, venue, instrument:
 * the same order every time, whatever order the sources were read in. */
static gint
md_deal_compare(
	gconstpointer	a,
	gconstpointer	b
){
	const MdDeal *x = *(const MdDeal *const *)a;
	const MdDeal *y = *(const MdDeal *const *)b;
	gint order;

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

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	g_return_val_if_fail(NULL != query, NULL);

	if (!md_require_module(context, error))
		return NULL;

	count = (0 == query->count) ? 50 : query->count;

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

	root = json_object_new();
	notes = json_array_new();
	rows = json_array_new();
	json_object_set_boolean_member(root, "available", FALSE);
	json_object_set_boolean_member(root, "truncated", FALSE);

	if (md_series_ready(context, notes))
	{
#ifdef VENTURE_HAVE_SQLITE
		g_autoptr(GPtrArray) sources = NULL;
		g_autoptr(GPtrArray) deals = NULL;
		const gchar *venue_keys[2];
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
		deals = g_ptr_array_new_with_free_func(md_deal_free);
		truncated = FALSE;

		/*
		 * Each source answers its own best count+1 deals from the deal
		 * index; the merge keeps the best count of all of them, and the
		 * extra one says whether anything was cut.
		 */
		for (i = 0; i < sources->len; i++)
		{
			VentureEntity *source = g_ptr_array_index(sources, i);
			g_autoptr(VentureSeriesStore) reader = NULL;
			g_autoptr(GPtrArray) found = NULL;
			g_autoptr(GError) local_error = NULL;
			VentureSeriesFilter filter;
			guint j;

			reader = md_reader(context, source, notes, NULL);

			if (NULL == reader)
				continue;

			json_object_set_boolean_member(root, "available", TRUE);
			venture_series_filter_init(&filter);
			filter.deals_only = TRUE;
			filter.max_pct_vs_region = query->max_pct;
			filter.sort = VENTURE_SERIES_SORT_PCT_VS_REGION;
			filter.count = count + 1;

			if (!venture_string_is_empty(query->venue))
			{
				venue_keys[0] = query->venue;
				venue_keys[1] = NULL;
				filter.venue_keys = venue_keys;
			}

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
				deal->data_source_id = venture_entity_get_id(source);
				deal->source_name = md_source_name(source);
				g_ptr_array_add(deals, deal);
			}
		}

		g_ptr_array_sort(deals, md_deal_compare);

		if (deals->len > count)
		{
			truncated = TRUE;
			g_ptr_array_set_size(deals, count);
		}

		for (i = 0; i < deals->len; i++)
		{
			MdDeal *deal = g_ptr_array_index(deals, i);
			JsonObject *object;
			gint64 discount;

			object = md_row_object(deal->row, deal->data_source_id);
			json_object_set_int_member(object, "data_source_id", deal->data_source_id);
			json_object_set_string_member(object, "source_name", deal->source_name);

			/* Exact: both sides are minor units in the row's currency. */
			if (venture_series_math_add(deal->row->deal_price, -deal->row->min_price, &discount))
				md_set_money(object, "discount", discount, deal->row->currency);
			else
				json_object_set_null_member(object, "discount");

			md_set_ratio(object, "discount_pct", 100.0 - deal->row->pct_vs_region);
			json_array_add_object_element(rows, object);
		}

		json_object_set_boolean_member(root, "truncated", truncated);
#endif
	}

	if (!json_object_has_member(root, "sources"))
		json_object_set_array_member(root, "sources", json_array_new());

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
