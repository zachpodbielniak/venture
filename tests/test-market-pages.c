/*
 * test-market-pages.c - The Trading pages, their API twins, the market
 * reports and the market widgets
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Each page is drawn from one answer (venture-marketdata-browse.c) that
 * its /api/v1/market twin returns verbatim, so most assertions here read
 * the JSON and compare it with what the series store itself says, and the
 * page tests check what a person sees: escaping, the empty states, the
 * buttons. The store is seeded through its writer, as test-marketdata
 * does; the server runs on a kernel-chosen port with authentication off,
 * so these tests are about what the pages say, and test-auth about who
 * may see them.
 */

#include <venture.h>
#include <glib/gstdio.h>
#include <libsoup/soup.h>
#include <math.h>
#include <string.h>

#include "venture-test-util.h"

#ifdef VENTURE_HAVE_SQLITE

#define ID(record) (venture_entity_get_id(VENTURE_ENTITY(record)))

static const gchar hostile_name[] = "<script>alert(\"pwn\")</script>";
static const gchar hostile_key[] = "evil/key 1";

typedef struct
{
	gchar			*state_dir;
	VentureConfig		*config;
	VentureDatabase		*database;
	VentureContext		*context;
	VentureWebServer	*server;
	SoupSession		*session;
	gint64			 org;
	gint64			 source_id;
	const gchar		*cookie;	/* sent when set: the sidebar's pick */
} Fixture;

/* --- Helpers ---------------------------------------------------------------- */

static void
save(
	Fixture		*fixture,
	gpointer	 record
){
	g_autoptr(GError) error = NULL;

	if (!venture_database_save(fixture->database, VENTURE_ENTITY(record), NULL, &error))
		g_error("save refused: %s", error->message);
}

static VentureMoney *
money_of(const gchar *text)
{
	VentureMoney *money;

	money = venture_money_from_string(text, NULL, NULL);
	g_assert_nonnull(money);

	return money;
}

typedef struct
{
	gboolean	 done;
	GBytes		*body;
	GError		*error;
} Reply;

static void
reply_done(
	GObject		*source,
	GAsyncResult	*result,
	gpointer	 data
){
	Reply *reply = data;

	reply->body = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &reply->error);
	reply->done = TRUE;
}

/*
 * One request on this thread's main context, which the server answering
 * it runs on too. Redirects are not followed, so a 302 and its Location
 * can be asserted.
 */
static guint
http(
	Fixture		 *fixture,
	const gchar	 *method,
	const gchar	 *path,
	const gchar	 *form,
	gchar		**out_body,
	gchar		**out_location
){
	g_autoptr(SoupMessage) message = NULL;
	g_autofree gchar *url = NULL;
	Reply reply;

	memset(&reply, 0, sizeof(reply));
	url = g_strconcat(venture_web_server_get_base_url(fixture->server), path, NULL);
	message = soup_message_new(method, url);
	g_assert_nonnull(message);

	if (NULL != form)
	{
		g_autoptr(GBytes) bytes = g_bytes_new(form, strlen(form));

		soup_message_set_request_body_from_bytes(message, "application/x-www-form-urlencoded", bytes);
	}

	if (NULL != fixture->cookie)
		soup_message_headers_append(soup_message_get_request_headers(message), "Cookie", fixture->cookie);

	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	soup_session_send_and_read_async(fixture->session, message, G_PRIORITY_DEFAULT, NULL, reply_done,
	                                 &reply);

	while (!reply.done)
		g_main_context_iteration(NULL, TRUE);

	g_assert_no_error(reply.error);

	if (NULL != out_body)
		*out_body = g_strndup(g_bytes_get_data(reply.body, NULL), g_bytes_get_size(reply.body));

	if (NULL != out_location)
		*out_location = g_strdup(soup_message_headers_get_one(
			soup_message_get_response_headers(message), "Location"));

	g_bytes_unref(reply.body);

	return soup_message_get_status(message);
}

/* GETs @path, asserting @status, and parses the body as JSON. */
static JsonNode *
get_json(
	Fixture		*fixture,
	const gchar	*path,
	guint		 status
){
	g_autofree gchar *body = NULL;
	g_autoptr(GError) error = NULL;
	JsonNode *node;

	g_assert_cmpuint(http(fixture, "GET", path, NULL, &body, NULL), ==, status);
	node = json_from_string(body, &error);
	g_assert_no_error(error);
	g_assert_nonnull(node);

	return node;
}

static gchar *
get_page(
	Fixture		*fixture,
	const gchar	*path
){
	gchar *body = NULL;
	guint status;

	status = http(fixture, "GET", path, NULL, &body, NULL);

	if (200 != status)
		g_error("GET %s answered %u: %s", path, status, body);

	return body;
}

/* Every button says what it does (the record-view walk, page by page). */
static void
assert_buttons_named(
	const gchar	*page,
	const gchar	*path
){
	const gchar *cursor = page;

	while (NULL != (cursor = strstr(cursor, "<button")))
	{
		const gchar *open_end = strchr(cursor, '>');
		const gchar *close = strstr(cursor, "</button>");
		g_autofree gchar *tag = NULL;
		gboolean words = FALSE;
		gboolean in_tag = FALSE;
		const gchar *p;

		g_assert_nonnull(open_end);
		g_assert_nonnull(close);
		tag = g_strndup(cursor, open_end - cursor);

		for (p = open_end + 1; p < close; p++)
		{
			if ('<' == *p)
				in_tag = TRUE;
			else if ('>' == *p)
				in_tag = FALSE;
			else if (!in_tag && g_ascii_isalnum(*p))
				words = TRUE;
		}

		if (!words && (NULL == strstr(tag, "aria-label=\"")))
			g_error("%s: a button with no name: %s>", path, tag);

		cursor = close;
	}
}

static JsonObject *
root_of(JsonNode *node)
{
	g_assert_true(JSON_NODE_HOLDS_OBJECT(node));

	return json_node_get_object(node);
}

static gint64
money_amount(
	JsonObject	*object,
	const gchar	*member
){
	JsonNode *node = json_object_get_member(object, member);

	g_assert_nonnull(node);
	g_assert_true(JSON_NODE_HOLDS_OBJECT(node));

	return json_object_get_int_member(json_node_get_object(node), "amount");
}

/* A read handle on the fixture's store, to compare the pages with. */
static VentureSeriesStore *
reader(Fixture *fixture)
{
	g_autoptr(GError) error = NULL;
	VentureSeriesStore *store;

	store = venture_feeds_service_open_reader(venture_context_get_feeds_service(fixture->context),
	                                          fixture->source_id, &error);
	g_assert_no_error(error);
	g_assert_nonnull(store);

	return store;
}

/* --- The store ---------------------------------------------------------------- */

typedef struct
{
	const gchar	*instrument;
	guint64		 id;
	gint64		 price;
	gint64		 quantity;
} Offer;

static void
add_venue(
	VentureSeriesStore	*store,
	const gchar		*key,
	const gchar		*group,
	const gchar		*currency,
	gint64			 seen_at
){
	g_autoptr(GError) error = NULL;
	VentureSeriesVenue venue;

	memset(&venue, 0, sizeof(venue));
	venue.key = key;
	venue.namespace_ = "realm";
	venue.name = key;
	venue.kind = "auction_house";
	venue.group_key = group;
	venue.currency = currency;
	g_assert_true(venture_series_store_upsert_venue(store, &venue, seen_at, &error));
	g_assert_no_error(error);
}

static void
add_instrument(
	VentureSeriesStore	*store,
	const gchar		*key,
	const gchar		*name,
	const gchar		*category,
	gint64			 seen_at
){
	g_autoptr(GError) error = NULL;
	VentureSeriesInstrument instrument;

	memset(&instrument, 0, sizeof(instrument));
	instrument.key = key;
	instrument.namespace_ = "wow-item";
	instrument.name = name;
	instrument.kind = "item";
	instrument.category = category;
	g_assert_true(venture_series_store_upsert_instrument(store, &instrument, seen_at, NULL, NULL,
	                                                     &error));
	g_assert_no_error(error);
}

static void
snapshot(
	VentureSeriesStore	*store,
	const gchar		*venue,
	const gchar		*currency,
	gint64			 taken_at,
	const Offer		*offers,
	guint			 n_offers
){
	g_autoptr(GError) error = NULL;
	VentureSeriesSnapshot *snap;
	VentureSeriesCommitResult result;
	guint i;

	snap = venture_series_store_begin_snapshot(store, venue, currency, taken_at, taken_at + 60, TRUE,
	                                           &error);
	g_assert_no_error(error);

	for (i = 0; i < n_offers; i++)
	{
		VentureSeriesListing listing;

		memset(&listing, 0, sizeof(listing));
		listing.instrument_key = offers[i].instrument;
		listing.listing_id = offers[i].id;
		listing.unit_price = offers[i].price;
		listing.quantity = offers[i].quantity;
		listing.side = VENTURE_SERIES_SIDE_SELL;
		listing.expires_in_min = -1;
		g_assert_true(venture_series_snapshot_add_listing(snap, &listing, &error));
		g_assert_no_error(error);
	}

	g_assert_true(venture_series_store_commit_snapshot(store, snap, &result, &error));
	g_assert_no_error(error);
}

/*
 * Three venues: two EU ones in USD and a US one in EUR. Copper ore seen
 * three times at realm-a (a listing sells between snapshots), once
 * elsewhere; peacebloom at both EU venues; a bar; and an instrument whose
 * name is a script and whose key holds a slash and a space.
 */
static void
seed_store(Fixture *fixture)
{
	g_autoptr(VentureEntity) source = NULL;
	g_autoptr(VentureSeriesStore) store = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *dir = NULL;
	gint64 now;
	gint64 t0;
	gint64 t1;
	gint64 t2;
	static const Offer a0[] = {
		{ "2770", 1, 125, 10 }, { "2770", 2, 140, 5 }, { "2447", 3, 75, 12 }
	};
	static const Offer a1[] = {
		{ "2770", 1, 125, 10 }, { "2770", 4, 130, 3 }, { "2447", 3, 75, 12 }
	};
	static const Offer a2[] = {
		{ "2770", 4, 130, 3 }, { "2770", 5, 120, 2 }, { "2447", 3, 75, 12 },
		{ "2840", 6, 500, 1 }, { hostile_key, 7, 999, 1 }
	};
	static const Offer b2[] = { { "2770", 10, 110, 4 }, { "2447", 11, 80, 1 } };
	static const Offer c2[] = { { "2770", 20, 100, 7 } };

	source = venture_database_get(fixture->database, VENTURE_TYPE_DATA_SOURCE, fixture->source_id, NULL);
	dir = venture_feeds_store_dir(fixture->config, venture_entity_get_uuid(source));
	store = venture_series_store_open(dir, &error);
	g_assert_no_error(error);

	now = g_get_real_time() / G_USEC_PER_SEC;
	t0 = now - 2 * 86400;
	t1 = now - 86400;
	t2 = now - 3600;

	add_venue(store, "realm-a", "eu", "USD", t0);
	add_venue(store, "realm-b", "eu", "USD", t0);
	add_venue(store, "realm-c", "us", "EUR", t0);
	add_instrument(store, "2770", "Copper Ore", "Materials/Ore", t0);
	add_instrument(store, "2447", "Peacebloom", "Herbs", t0);
	add_instrument(store, "2840", "Copper Bar", "Materials/Bars", t0);
	add_instrument(store, hostile_key, hostile_name, "Materials/<b>", t0);

	snapshot(store, "realm-a", "USD", t0, a0, G_N_ELEMENTS(a0));
	snapshot(store, "realm-a", "USD", t1, a1, G_N_ELEMENTS(a1));
	snapshot(store, "realm-a", "USD", t2, a2, G_N_ELEMENTS(a2));
	snapshot(store, "realm-b", "USD", t2, b2, G_N_ELEMENTS(b2));
	snapshot(store, "realm-c", "EUR", t2, c2, G_N_ELEMENTS(c2));

	g_assert_true(venture_series_store_recompute_region(store, NULL, now, VENTURE_SERIES_NONE, NULL,
	                                                    NULL, &error));
	g_assert_no_error(error);
}

/* A data source in another organization, for the NOT_FOUND checks. */
static gint64
foreign_source(Fixture *fixture)
{
	g_autoptr(VentureEntity) organization = NULL;
	g_autoptr(VentureDataSource) source = NULL;

	organization = VENTURE_ENTITY(venture_organization_new());
	g_object_set(organization, "name", "Elsewhere", "slug", "elsewhere", NULL);
	save(fixture, organization);
	source = venture_data_source_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(source), ID(organization));
	g_object_set(source, "name", "Theirs", "provider", "file_jsonl", "settings", "file: y.jsonl",
	             "schedule", "manual", NULL);
	save(fixture, source);

	return ID(source);
}

/* --- The fixture ------------------------------------------------------------ */

static void
fixture_set_up(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDataSource) source = NULL;

	(void)user_data;

	fixture->state_dir = g_dir_make_tmp("venture-market-pages-XXXXXX", &error);
	g_assert_no_error(error);

	fixture->config = venture_config_new();
	g_object_set(fixture->config, "state-dir", fixture->state_dir, "feeds-enabled", TRUE,
	             "server-bind-address", "127.0.0.1", "server-port", (gint64)0,
	             "security-require-auth", FALSE, NULL);

	fixture->database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(fixture->database, venture_entity_registry_get_default(),
	                                       &error));
	g_assert_no_error(error);
	fixture->context = venture_context_new(fixture->config, fixture->database);
	fixture->org = venture_context_get_default_organization_id(fixture->context);

	/* The registry is process-wide; with this context's switches applied
	 * the feeds and marketdata tables exist now. */
	g_assert_true(venture_database_migrate(fixture->database, venture_entity_registry_get_default(),
	                                       &error));
	g_assert_no_error(error);

	/* Manual, so nothing starts a worker behind the test's back. */
	source = venture_data_source_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(source), fixture->org);
	g_object_set(source, "name", "Auctions", "provider", "file_jsonl", "settings", "file: x.jsonl",
	             "schedule", "manual", "currency", "USD", "instrument-namespace", "wow-item",
	             "venue-namespace", "realm", NULL);
	save(fixture, source);
	fixture->source_id = ID(source);

	fixture->server = venture_web_server_new(fixture->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(fixture->server, &error));
	g_assert_no_error(error);
	fixture->session = soup_session_new_with_options("timeout", 30, NULL);
}

static void
fixture_tear_down(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureConfig) everything = NULL;
	g_autoptr(VentureModuleRegistry) registry = NULL;

	(void)user_data;

	venture_web_server_stop(fixture->server);
	g_clear_object(&fixture->session);
	g_clear_object(&fixture->server);
	g_clear_object(&fixture->context);
	g_clear_object(&fixture->database);
	g_clear_object(&fixture->config);

	if (NULL != fixture->state_dir)
	{
		venture_test_remove_tree(fixture->state_dir);
		g_clear_pointer(&fixture->state_dir, g_free);
	}

	/* The entity registry is process-wide; a test that switched a module
	 * off must not leave it off for the next. */
	everything = venture_config_new();
	registry = venture_module_registry_new();
	venture_module_registry_register_builtins(registry);
	venture_module_registry_configure(registry, everything, NULL);
	venture_module_registry_apply(registry, venture_entity_registry_get_default());
}

/* --- The tests -------------------------------------------------------------- */

/*
 * With feeds off every page is still a page: "no data sources", empty
 * lists, the records (watchlists, alerts) still listed. What breaks if
 * this regresses: the Trading section, which shows by default, answers
 * every click with an error page on an install that never turned feeds
 * on.
 */
static void
test_no_sources(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	static const gchar *const pages[] = {
		"/market/browse", "/market/deals", "/market/venues", "/market/watchlists",
		"/market/alerts", "/market/i/1/2770", NULL
	};
	g_autoptr(JsonNode) answer = NULL;
	guint i;

	(void)user_data;

	g_object_set(fixture->config, "feeds-enabled", FALSE, NULL);

	for (i = 0; NULL != pages[i]; i++)
	{
		g_autofree gchar *page = get_page(fixture, pages[i]);

		assert_buttons_named(page, pages[i]);

		if ((NULL != strstr(pages[i], "browse")) || (NULL != strstr(pages[i], "deals")) ||
		    (NULL != strstr(pages[i], "venues")) || (NULL != strstr(pages[i], "/i/")))
			g_assert_nonnull(strstr(page, "No data sources: market data feeds are off"));
	}

	answer = get_json(fixture, "/api/v1/market/browse", 200);
	g_assert_false(json_object_get_boolean_member(root_of(answer), "available"));
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(root_of(answer), "rows")), ==, 0);
}

/*
 * A source that has stored nothing yet says so, page by page; asking for
 * one of its instruments is NOT_FOUND with the reason.
 */
static void
test_empty_store(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(JsonNode) answer = NULL;
	g_autofree gchar *page = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *path = NULL;

	(void)user_data;

	page = get_page(fixture, "/market/browse");
	g_assert_nonnull(strstr(page, "has stored nothing yet"));
	g_assert_nonnull(strstr(page, "Nothing matches."));

	answer = get_json(fixture, "/api/v1/market/deals", 200);
	g_assert_false(json_object_get_boolean_member(root_of(answer), "available"));
	g_clear_pointer(&answer, json_node_unref);
	answer = get_json(fixture, "/api/v1/market/venues", 200);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(root_of(answer), "venues")), ==, 0);

	path = g_strdup_printf("/api/v1/market/i/%" G_GINT64_FORMAT "/2770", fixture->source_id);
	g_assert_cmpuint(http(fixture, "GET", path, NULL, &body, NULL), ==, 404);
	g_assert_nonnull(strstr(body, "has stored nothing yet"));
}

/*
 * The catalogue: the total is the store's own count, the default order is
 * cheapest first with unknowns last, a sort is an allowlisted column, and
 * paging, search, category and venue narrow the store's query. What breaks
 * if this regresses: a sort name from the URL reaching SQL, a page that
 * claims more rows than exist, or a filter that narrows nothing.
 */
static void
test_browse(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureSeriesStore) store = NULL;
	g_autoptr(JsonNode) answer = NULL;
	g_autofree gchar *page = NULL;
	g_autofree gchar *path = NULL;
	VentureSeriesFilter filter;
	JsonArray *rows;
	gint64 expected;
	gint64 previous;
	guint i;

	(void)user_data;

	seed_store(fixture);
	store = reader(fixture);
	venture_series_filter_init(&filter);
	g_assert_true(venture_series_store_count_current(store, &filter, &expected, NULL));

	answer = get_json(fixture, "/api/v1/market/browse", 200);
	g_assert_true(json_object_get_boolean_member(root_of(answer), "available"));
	g_assert_cmpint(json_object_get_int_member(root_of(answer), "total"), ==, expected);
	g_assert_cmpint(json_object_get_int_member(root_of(answer), "data_source_id"), ==, fixture->source_id);
	rows = json_object_get_array_member(root_of(answer), "rows");
	g_assert_cmpuint(json_array_get_length(rows), ==, (guint)expected);

	/* Cheapest first; rows with no price (out of stock) last. */
	previous = G_MININT64;

	for (i = 0; i < json_array_get_length(rows); i++)
	{
		JsonObject *row = json_array_get_object_element(rows, i);
		JsonNode *price = json_object_get_member(row, "min_price");

		if (JSON_NODE_HOLDS_NULL(price))
		{
			previous = G_MAXINT64;
			continue;
		}

		g_assert_cmpint(previous, !=, G_MAXINT64);
		g_assert_cmpint(money_amount(row, "min_price"), >=, previous);
		previous = money_amount(row, "min_price");
	}

	g_clear_pointer(&answer, json_node_unref);

	/* Paging: two a page, the second page. */
	answer = get_json(fixture, "/api/v1/market/browse?per_page=2&page=2", 200);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(root_of(answer), "rows")), ==, 2);
	g_assert_cmpint(json_object_get_int_member(root_of(answer), "pages"), ==, (expected + 1) / 2);
	g_clear_pointer(&answer, json_node_unref);

	/* Search, category and venue each narrow. */
	answer = get_json(fixture, "/api/v1/market/browse?search=copper", 200);
	rows = json_object_get_array_member(root_of(answer), "rows");
	g_assert_cmpuint(json_array_get_length(rows), >, 0);

	for (i = 0; i < json_array_get_length(rows); i++)
		g_assert_nonnull(strstr(json_object_get_string_member(json_array_get_object_element(rows, i),
		                                                      "instrument_name"), "Copper"));

	g_clear_pointer(&answer, json_node_unref);
	answer = get_json(fixture, "/api/v1/market/browse?category=Materials%2FOre", 200);
	rows = json_object_get_array_member(root_of(answer), "rows");
	g_assert_cmpuint(json_array_get_length(rows), ==, 3);

	for (i = 0; i < json_array_get_length(rows); i++)
		g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(rows, i),
		                                              "instrument_key"), ==, "2770");

	g_clear_pointer(&answer, json_node_unref);
	answer = get_json(fixture, "/api/v1/market/browse?venue=realm-b&sort=pct_vs_region&dir=desc", 200);
	rows = json_object_get_array_member(root_of(answer), "rows");
	g_assert_cmpuint(json_array_get_length(rows), ==, 2);
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(rows, 0), "venue_key"),
	                ==, "realm-b");
	g_clear_pointer(&answer, json_node_unref);

	/* A sort is a column from the allowlist, or a refusal. */
	g_assert_cmpuint(http(fixture, "GET", "/api/v1/market/browse?sort=min_price%3BDROP", NULL, NULL, NULL),
	                 ==, 400);
	g_assert_cmpuint(http(fixture, "GET", "/api/v1/market/browse?per_page=500", NULL, NULL, NULL), ==, 400);
	g_assert_cmpuint(http(fixture, "GET", "/api/v1/market/browse?page=x", NULL, NULL, NULL), ==, 400);

	/*
	 * Every allowlisted sort answers, both ways: each is a column the
	 * store keeps, so none is computed here.
	 */
	for (i = 0; NULL != venture_marketdata_browse_sorts()[i]; i++)
	{
		g_autofree gchar *sorted = NULL;

		sorted = g_strdup_printf("/api/v1/market/browse?sort=%s&dir=desc",
		                         venture_marketdata_browse_sorts()[i]);
		g_assert_cmpuint(http(fixture, "GET", sorted, NULL, NULL, NULL), ==, 200);
	}

	/* The page: sortable headings, links to the instrument pages, and
	 * the hostile name escaped. */
	page = get_page(fixture, "/market/browse?sort=min_price&dir=asc");
	g_assert_nonnull(strstr(page, "aria-sort=\"ascending\""));
	path = g_strdup_printf("href=\"/market/i/%" G_GINT64_FORMAT "/2770?venue=realm-", fixture->source_id);
	g_assert_nonnull(strstr(page, path));
	g_assert_nonnull(strstr(page, "&lt;script&gt;alert(&quot;pwn&quot;)&lt;/script&gt;"));
	g_assert_null(strstr(page, hostile_name));
	g_assert_nonnull(strstr(page, "Materials/&lt;b&gt;"));
	{
		g_autofree gchar *evil = venture_marketdata_instrument_path(fixture->source_id, hostile_key, "realm-a");

		g_assert_true(g_str_has_suffix(evil, "/evil%2Fkey%201?venue=realm-a"));
		g_assert_nonnull(strstr(page, evil));
	}
	assert_buttons_named(page, "/market/browse");
}

/*
 * An instrument page: the base stats are the store's current row and
 * region, the heat map is seven weekdays by twenty-four hours, the bulk
 * calculator walks the book, and every venue is listed cheapest first.
 * What breaks if this regresses: a page whose "lowest price" is another
 * venue's, a heat map of the wrong shape, a bulk cost that does not walk
 * the tiers.
 */
static void
test_instrument(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureSeriesStore) store = NULL;
	g_autoptr(VentureSeriesRow) row = NULL;
	g_autoptr(JsonNode) answer = NULL;
	g_autoptr(GArray) hourly = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *page = NULL;
	VentureSeriesRegion region;
	VentureSeriesBulkCost bulk;
	gchar currency[VENTURE_MONEY_CURRENCY_LEN];
	JsonObject *root;
	JsonObject *base;
	JsonObject *heat;
	JsonArray *venues;
	JsonArray *days;
	guint i;

	(void)user_data;

	seed_store(fixture);
	store = reader(fixture);
	g_assert_true(venture_series_store_get_current(store, "realm-a", "2770", &row, NULL));
	g_assert_true(venture_series_store_get_region(store, "eu", "2770", "USD", &region, NULL));
	g_assert_true(venture_series_store_bulk_cost(store, "realm-a", "2770", 4, &bulk, currency, NULL));

	path = g_strdup_printf("/api/v1/market/i/%" G_GINT64_FORMAT "/2770?venue=realm-a&units=4",
	                       fixture->source_id);
	answer = get_json(fixture, path, 200);
	root = root_of(answer);
	g_assert_cmpstr(json_object_get_string_member(root, "venue"), ==, "realm-a");
	g_assert_cmpstr(json_object_get_string_member(root, "group_key"), ==, "eu");
	g_assert_cmpstr(json_object_get_string_member(json_object_get_object_member(root, "instrument"), "name"),
	                ==, "Copper Ore");

	base = json_object_get_object_member(root, "base");
	g_assert_cmpint(money_amount(base, "min_price"), ==, row->min_price);
	g_assert_cmpint(money_amount(base, "min_price"), ==, 120);
	g_assert_cmpint(json_object_get_int_member(base, "quantity"), ==, row->quantity);
	g_assert_cmpint(money_amount(base, "region_median"), ==, region.median_min);
	g_assert_cmpint(money_amount(base, "deal_price"), ==, region.deal_price);
	g_assert_cmpint(json_object_get_int_member(base, "region_venues"), ==, region.venues_offering);

	/* The heat map: a matrix per figure, Monday first. */
	heat = json_object_get_object_member(root, "heat");
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(heat, "price")), ==, 7);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(heat, "quantity")), ==, 7);

	for (i = 0; i < 7; i++)
		g_assert_cmpuint(json_array_get_length(json_array_get_array_element(
			json_object_get_array_member(heat, "price"), i)), ==, 24);

	g_assert_cmpstr(json_array_get_string_element(json_object_get_array_member(heat, "weekdays"), 0), ==,
	                "Mon");

	/* The snapshots chart reads the store's hourly points, all of them. */
	hourly = venture_series_store_hourly(store, "realm-a", "2770",
	                                     g_get_real_time() / G_USEC_PER_SEC - 14 * 86400, NULL);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(root, "hourly")), ==, hourly->len);
	days = json_object_get_array_member(root, "daily");
	g_assert_cmpuint(json_array_get_length(days), >, 0);

	/* Bulk: what the store's own walk says. */
	g_assert_cmpint(json_object_get_int_member(json_object_get_object_member(root, "bulk"), "filled"), ==,
	                bulk.filled);
	g_assert_cmpint(money_amount(json_object_get_object_member(root, "bulk"), "cost"), ==, bulk.cost);

	/* Other venues: all three, cheapest first (within a currency the
	 * store sorts by price). */
	venues = json_object_get_array_member(root, "venues");
	g_assert_cmpuint(json_array_get_length(venues), ==, 3);

	/* The page: charts with their tables, the forms, named buttons. */
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/market/i/%" G_GINT64_FORMAT "/2770?units=4", fixture->source_id);
	page = get_page(fixture, path);
	g_assert_nonnull(strstr(page, "<figure class=\"chart chart-line\">"));
	g_assert_nonnull(strstr(page, "<figure class=\"chart chart-heat\">"));
	g_assert_nonnull(strstr(page, "chart-data"));
	g_assert_nonnull(strstr(page, "Bulk pricing"));
	g_assert_nonnull(strstr(page, "Promote to a record"));
	g_assert_nonnull(strstr(page, "Add to watchlist"));
	g_assert_nonnull(strstr(page, "Create an alert"));
	g_assert_nonnull(strstr(page, "Every venue"));
	assert_buttons_named(page, path);

	/* Unknown key, unknown venue, a broken escape: not found. */
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/api/v1/market/i/%" G_GINT64_FORMAT "/nope", fixture->source_id);
	g_assert_cmpuint(http(fixture, "GET", path, NULL, NULL, NULL), ==, 404);
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/api/v1/market/i/%" G_GINT64_FORMAT "/2770?venue=realm-z", fixture->source_id);
	g_assert_cmpuint(http(fixture, "GET", path, NULL, NULL, NULL), ==, 404);
	g_clear_pointer(&path, g_free);
	/* A NUL in a key is refused, by the transport or here. */
	path = g_strdup_printf("/api/v1/market/i/%" G_GINT64_FORMAT "/%%00x", fixture->source_id);
	g_assert_cmpuint(http(fixture, "GET", path, NULL, NULL, NULL), >=, 400);
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/api/v1/market/i/%" G_GINT64_FORMAT "/2770?units=-1", fixture->source_id);
	g_assert_cmpuint(http(fixture, "GET", path, NULL, NULL, NULL), ==, 400);
}

/*
 * The hostile instrument: a key with a slash and a space is one path
 * segment, and its name -- a script -- is text in every place a page draws
 * it, the SVG's <title>, aria-label and data table included. What breaks
 * if this regresses: a feed runs script in the operator's browser.
 */
static void
test_instrument_escaping(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *path = NULL;
	g_autofree gchar *page = NULL;
	g_autoptr(JsonNode) answer = NULL;

	(void)user_data;

	seed_store(fixture);
	path = venture_marketdata_instrument_path(fixture->source_id, hostile_key, NULL);
	g_assert_true(g_str_has_suffix(path, "/evil%2Fkey%201"));
	page = get_page(fixture, path);
	g_assert_null(strstr(page, hostile_name));
	g_assert_null(strstr(page, "<script>alert"));
	g_assert_nonnull(strstr(page, "<h1>&lt;script&gt;alert(&quot;pwn&quot;)&lt;/script&gt;</h1>"));
	g_assert_nonnull(strstr(page, "aria-label=\"&lt;script&gt;alert(&quot;pwn&quot;)&lt;/script&gt;: "));
	g_assert_nonnull(strstr(page, "<title>&lt;script&gt;"));

	/* The API twin answers the same key, unescaped. */
	{
		g_autofree gchar *api = g_strdup_printf("/api/v1%s", path);

		answer = get_json(fixture, api, 200);
		g_assert_cmpstr(json_object_get_string_member(
			json_object_get_object_member(root_of(answer), "instrument"), "key"), ==, hostile_key);
		g_assert_cmpstr(json_object_get_string_member(
			json_object_get_object_member(root_of(answer), "instrument"), "name"), ==, hostile_name);
	}
}

/*
 * Another organization's source is NOT_FOUND on every door, never
 * FORBIDDEN, so whether it exists is not told.
 */
static void
test_other_organization(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *path = NULL;
	gint64 theirs;

	(void)user_data;

	seed_store(fixture);
	theirs = foreign_source(fixture);

	path = g_strdup_printf("/api/v1/market/i/%" G_GINT64_FORMAT "/2770", theirs);
	g_assert_cmpuint(http(fixture, "GET", path, NULL, NULL, NULL), ==, 404);
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/market/i/%" G_GINT64_FORMAT "/2770", theirs);
	g_assert_cmpuint(http(fixture, "GET", path, NULL, NULL, NULL), ==, 404);
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/api/v1/market/browse?source=%" G_GINT64_FORMAT, theirs);
	g_assert_cmpuint(http(fixture, "GET", path, NULL, NULL, NULL), ==, 404);
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/api/v1/market/deals?source=%" G_GINT64_FORMAT, theirs);
	g_assert_cmpuint(http(fixture, "GET", path, NULL, NULL, NULL), ==, 404);
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/api/v1/market/venues?source=%" G_GINT64_FORMAT, theirs);
	g_assert_cmpuint(http(fixture, "GET", path, NULL, NULL, NULL), ==, 404);
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/market/i/%" G_GINT64_FORMAT "/2770", theirs);
	g_assert_cmpuint(http(fixture, "POST", path, "action=promote", NULL, NULL), ==, 404);
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/api/v1/reports/market_deals?period=all&data_source_id=%" G_GINT64_FORMAT, theirs);
	g_assert_cmpuint(http(fixture, "GET", path, NULL, NULL, NULL), ==, 404);
	g_clear_pointer(&path, g_free);

	/* The sidebar's "all" answers for the default organization, as the
	 * scan does: the browse page lists its own source only, and the
	 * instrument link it draws opens. Passed on as zero, the list read
	 * every organization and every detail page answered NOT_FOUND. */
	fixture->cookie = "venture_entity=all";
	{
		g_autofree gchar *page = get_page(fixture, "/market/browse");
		g_autofree gchar *link = NULL;
		g_autofree gchar *detail = NULL;

		link = g_strdup_printf("href=\"/market/i/%" G_GINT64_FORMAT "/", theirs);
		g_assert_null(strstr(page, link));
		g_clear_pointer(&link, g_free);
		link = g_strdup_printf("/market/i/%" G_GINT64_FORMAT "/2770", fixture->source_id);
		g_assert_nonnull(strstr(page, link));
		detail = get_page(fixture, link);
		g_assert_nonnull(detail);
	}
	fixture->cookie = NULL;
}

/*
 * Deals are exactly the rows in stock at or under their group's deal
 * price -- checked against every current row of the store -- and the
 * filters narrow them: a percent bound, and a minimum value that says
 * nothing about another currency. What breaks if this regresses: a deal
 * list with a venue dearer than the deal price, or a dollar bound that
 * lets euro rows through.
 */
static void
test_deals(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureSeriesStore) store = NULL;
	g_autoptr(GPtrArray) all = NULL;
	g_autoptr(GHashTable) expected = NULL;
	g_autoptr(JsonNode) answer = NULL;
	g_autofree gchar *page = NULL;
	VentureSeriesFilter filter;
	JsonArray *rows;
	gdouble previous;
	guint deals;
	guint i;

	(void)user_data;

	seed_store(fixture);
	store = reader(fixture);
	venture_series_filter_init(&filter);
	filter.count = 1000;
	all = venture_series_store_list_current(store, &filter, NULL);
	expected = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

	for (i = 0; i < all->len; i++)
	{
		VentureSeriesRow *row = g_ptr_array_index(all, i);

		if ((VENTURE_SERIES_NONE != row->deal_price) && (VENTURE_SERIES_NONE != row->min_price) &&
		    (row->min_price <= row->deal_price) && (0 != row->quantity))
			g_hash_table_add(expected, g_strdup_printf("%s|%s", row->venue_key, row->instrument_key));
	}

	g_assert_cmpuint(g_hash_table_size(expected), >, 0);
	g_assert_cmpuint(g_hash_table_size(expected), <, all->len);

	answer = get_json(fixture, "/api/v1/market/deals", 200);
	rows = json_object_get_array_member(root_of(answer), "rows");
	deals = json_array_get_length(rows);
	g_assert_cmpuint(deals, ==, g_hash_table_size(expected));
	previous = -1.0;

	for (i = 0; i < deals; i++)
	{
		JsonObject *row = json_array_get_object_element(rows, i);
		g_autofree gchar *pair = g_strdup_printf("%s|%s", json_object_get_string_member(row, "venue_key"),
		                                         json_object_get_string_member(row, "instrument_key"));

		g_assert_true(g_hash_table_contains(expected, pair));
		g_assert_cmpint(money_amount(row, "min_price"), <=, money_amount(row, "deal_price"));
		g_assert_cmpint(money_amount(row, "discount"), ==,
		                money_amount(row, "deal_price") - money_amount(row, "min_price"));
		g_assert_cmpfloat(json_object_get_double_member(row, "pct_vs_region"), >=, previous);
		previous = json_object_get_double_member(row, "pct_vs_region");
		g_assert_cmpint(json_object_get_int_member(row, "data_source_id"), ==, fixture->source_id);
	}

	g_clear_pointer(&answer, json_node_unref);

	/* A euro bound: only euro rows can pass it. */
	answer = get_json(fixture, "/api/v1/market/deals?min_value=0.01%20EUR", 200);
	rows = json_object_get_array_member(root_of(answer), "rows");

	for (i = 0; i < json_array_get_length(rows); i++)
		g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(rows, i), "currency"),
		                ==, "EUR");

	g_clear_pointer(&answer, json_node_unref);

	/* At most 1%: nothing is that cheap. */
	answer = get_json(fixture, "/api/v1/market/deals?max_pct=1", 200);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(root_of(answer), "rows")), ==, 0);
	g_clear_pointer(&answer, json_node_unref);

	/* One row and it says it cut some. */
	answer = get_json(fixture, "/api/v1/market/deals?top=1", 200);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(root_of(answer), "rows")), ==, 1);
	g_assert_true(json_object_get_boolean_member(root_of(answer), "truncated") || (1 == deals));

	g_assert_cmpuint(http(fixture, "GET", "/api/v1/market/deals?max_pct=lots", NULL, NULL, NULL), ==, 400);
	/* A bound with no currency is refused, never read in a default. */
	g_assert_cmpuint(http(fixture, "GET", "/api/v1/market/deals?min_value=10", NULL, NULL, NULL), ==, 400);
	g_assert_cmpuint(http(fixture, "GET", "/api/v1/reports/market_deals?period=all&min_value=10", NULL, NULL,
	                      NULL), ==, 400);

	/* The report says the same, through the report door. */
	g_clear_pointer(&answer, json_node_unref);
	answer = get_json(fixture, "/api/v1/reports/market_deals?period=all", 200);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(root_of(answer), "rows")), ==, deals);
	g_clear_pointer(&answer, json_node_unref);
	answer = get_json(fixture, "/api/v1/reports/market_deals?period=all&max_pct=1", 200);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(root_of(answer), "rows")), ==, 0);
	g_clear_pointer(&answer, json_node_unref);
	answer = get_json(fixture, "/api/v1/reports/market_deals?period=all&group_key=us&top=5", 200);
	rows = json_object_get_array_member(root_of(answer), "rows");

	for (i = 0; i < json_array_get_length(rows); i++)
		g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(rows, i), "venue"),
		                ==, "realm-c");

	/* The report page offers its questions and keeps them on the
	 * period links. */
	page = get_page(fixture, "/reports/market_deals?period=all&max_pct=95&group_key=eu");
	g_assert_nonnull(strstr(page, "name=\"max_pct\""));
	g_assert_nonnull(strstr(page, "name=\"min_value\""));
	g_assert_nonnull(strstr(page, "&amp;max_pct=95"));
	g_assert_nonnull(strstr(page, "&amp;group_key=eu"));

	g_clear_pointer(&page, g_free);
	page = get_page(fixture, "/market/deals?group=eu");
	g_assert_nonnull(strstr(page, "Deal price"));
	assert_buttons_named(page, "/market/deals");
}

/*
 * The venue index is the store's own per-venue figures, with shares that
 * add up, and the upload timer it learned. What breaks if this
 * regresses: a "cheaper than region" share that is not the store's, or a
 * venue without its interval.
 */
static void
test_venue_index(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureSeriesStore) store = NULL;
	g_autoptr(GPtrArray) stats = NULL;
	g_autoptr(JsonNode) answer = NULL;
	g_autoptr(JsonNode) report = NULL;
	g_autofree gchar *page = NULL;
	JsonArray *venues;
	guint i;

	(void)user_data;

	seed_store(fixture);
	store = reader(fixture);
	stats = venture_series_store_venue_index(store, NULL, NULL);

	answer = get_json(fixture, "/api/v1/market/venues", 200);
	venues = json_object_get_array_member(root_of(answer), "venues");
	g_assert_cmpuint(json_array_get_length(venues), ==, stats->len);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(root_of(answer), "groups")), ==, 2);

	for (i = 0; i < stats->len; i++)
	{
		VentureSeriesVenueStats *one = g_ptr_array_index(stats, i);
		JsonObject *venue = json_array_get_object_element(venues, i);

		g_assert_cmpstr(json_object_get_string_member(venue, "venue_key"), ==, one->venue_key);
		g_assert_cmpint(json_object_get_int_member(venue, "cheaper"), ==, one->cheaper);
		g_assert_cmpint(json_object_get_int_member(venue, "equal"), ==, one->equal);
		g_assert_cmpint(json_object_get_int_member(venue, "dearer"), ==, one->dearer);
		g_assert_cmpint(json_object_get_int_member(venue, "listings"), ==, one->listings);
		g_assert_cmpint(json_object_get_int_member(venue, "interval_seconds"), ==, one->interval_seconds);

		if (one->instruments > 0)
			g_assert_cmpfloat_with_epsilon(json_object_get_double_member(venue, "pct_cheaper") +
			                               json_object_get_double_member(venue, "pct_equal") +
			                               json_object_get_double_member(venue, "pct_dearer"),
			                               100.0, 1e-9);
		else
			g_assert_true(JSON_NODE_HOLDS_NULL(json_object_get_member(venue, "pct_cheaper")));
	}

	g_clear_pointer(&answer, json_node_unref);
	answer = get_json(fixture, "/api/v1/market/venues?group=us", 200);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(root_of(answer), "venues")), ==, 1);

	report = get_json(fixture, "/api/v1/reports/venue_index?period=all&group_key=eu", 200);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(root_of(report), "rows")), ==, 2);

	page = get_page(fixture, "/market/venues");
	g_assert_nonnull(strstr(page, "Updates every"));
	g_assert_nonnull(strstr(page, "realm-c"));
}

/*
 * A watchlist priced across its group: the cheapest in-stock venue, the
 * difference from the targets in the same currency, and a sparkline. The
 * buttons on an instrument page promote and hand over to the generic
 * forms, prefilled. What breaks if this regresses: a list priced at a
 * venue outside its group, a "buy" signal from a target in another
 * currency, or a watch button that creates a second instrument record.
 */
static void
test_watchlist_and_actions(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) list = NULL;
	g_autoptr(VentureEntity) entry = NULL;
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(VentureEntity) again = NULL;
	g_autoptr(VentureMoney) buy = NULL;
	g_autoptr(VentureMoney) sell = NULL;
	g_autoptr(JsonNode) answer = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *location = NULL;
	g_autofree gchar *expected = NULL;
	g_autofree gchar *page = NULL;
	JsonArray *entries;
	JsonObject *item;
	gint64 instrument_id;

	(void)user_data;

	seed_store(fixture);

	list = VENTURE_ENTITY(venture_watchlist_new());
	venture_entity_set_organization_id(list, fixture->org);
	g_object_set(list, "name", "Ores", "group-key", "eu", NULL);
	save(fixture, list);

	/* Promote: once, however many times it is asked. */
	path = g_strdup_printf("/market/i/%" G_GINT64_FORMAT "/2770", fixture->source_id);
	g_assert_cmpuint(http(fixture, "POST", path, "action=promote", NULL, &location), ==, 302);
	g_assert_cmpstr(location, ==, path);
	g_clear_pointer(&location, g_free);
	g_assert_cmpuint(http(fixture, "POST", path, "action=promote", NULL, NULL), ==, 302);
	g_assert_cmpuint(http(fixture, "POST", path, "action=sell-everything", NULL, NULL), ==, 422);
	record = venture_marketdata_find_by_ref(fixture->database, VENTURE_TYPE_INSTRUMENT, fixture->org,
	                                        "wow-item:2770", NULL);
	g_assert_nonnull(record);
	instrument_id = ID(record);

	/* Watch: promotes, then the generic entry form, prefilled. */
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/market/i/%" G_GINT64_FORMAT "/2770", fixture->source_id);
	{
		g_autofree gchar *form = g_strdup_printf("action=watch&watchlist_id=%" G_GINT64_FORMAT, ID(list));

		g_assert_cmpuint(http(fixture, "POST", path, form, NULL, &location), ==, 302);
	}
	expected = g_strdup_printf("/e/watchlist_entry/new?instrument_id=%" G_GINT64_FORMAT
	                           "&watchlist_id=%" G_GINT64_FORMAT, instrument_id, ID(list));
	g_assert_cmpstr(location, ==, expected);
	page = get_page(fixture, location);
	g_clear_pointer(&page, g_free);
	g_clear_pointer(&location, g_free);
	again = venture_marketdata_find_by_ref(fixture->database, VENTURE_TYPE_INSTRUMENT, fixture->org,
	                                       "wow-item:2770", NULL);
	g_assert_cmpint(ID(again), ==, instrument_id);

	/* Alert: the rule form, with the instrument, the source, a kind and
	 * the venue's group. */
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/market/i/%" G_GINT64_FORMAT "/2770", fixture->source_id);
	g_assert_cmpuint(http(fixture, "POST", path, "action=alert&venue=realm-a", NULL, &location), ==, 302);
	g_clear_pointer(&expected, g_free);
	expected = g_strdup_printf("/e/alert_rule/new?instrument_id=%" G_GINT64_FORMAT "&data_source_id=%"
	                           G_GINT64_FORMAT "&kind=below&group_key=eu", instrument_id,
	                           fixture->source_id);
	g_assert_cmpstr(location, ==, expected);

	/* The entry, with a dollar buy target over the cheapest EU price
	 * (1.10 at realm-b) and a sell target in euros. */
	buy = money_of("1.15 USD");
	sell = money_of("2.00 EUR");
	entry = VENTURE_ENTITY(venture_watchlist_entry_new());
	venture_entity_set_organization_id(entry, fixture->org);
	g_object_set(entry, "watchlist-id", ID(list), "instrument-id", instrument_id, "target-buy", buy,
	             NULL);
	save(fixture, entry);

	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/api/v1/market/watchlists/%" G_GINT64_FORMAT, ID(list));
	answer = get_json(fixture, path, 200);
	entries = json_object_get_array_member(root_of(answer), "entries");
	g_assert_cmpuint(json_array_get_length(entries), ==, 1);
	item = json_array_get_object_element(entries, 0);
	g_assert_cmpstr(json_object_get_string_member(json_object_get_object_member(item, "best"), "venue_key"),
	                ==, "realm-b");
	g_assert_cmpint(money_amount(json_object_get_object_member(item, "best"), "min_price"), ==, 110);
	g_assert_cmpint(money_amount(item, "buy_delta"), ==, -5);
	g_assert_true(json_object_get_boolean_member(item, "buy_now"));

	/* Only the group's venues: realm-c is in "us". */
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(item, "venues")), ==, 2);
	g_clear_pointer(&answer, json_node_unref);

	/* A target in euros against a dollar price is not compared. */
	g_object_set(entry, "target-buy", NULL, "target-sell", sell, NULL);
	save(fixture, entry);
	answer = get_json(fixture, path, 200);
	item = json_array_get_object_element(json_object_get_array_member(root_of(answer), "entries"), 0);
	g_assert_true(JSON_NODE_HOLDS_NULL(json_object_get_member(item, "sell_delta")));
	g_assert_false(json_object_get_boolean_member(item, "sell_now"));
	g_assert_false(json_object_get_boolean_member(item, "buy_now"));
	g_clear_pointer(&answer, json_node_unref);

	/* The page: a sparkline per entry, named. */
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/market/watchlists/%" G_GINT64_FORMAT, ID(list));
	page = get_page(fixture, path);
	g_assert_nonnull(strstr(page, "class=\"sparkline"));
	g_assert_nonnull(strstr(page, "Copper Ore: lowest price each day, 14 days"));
	g_clear_pointer(&page, g_free);
	page = get_page(fixture, "/market/watchlists");
	g_assert_nonnull(strstr(page, ">Ores</a>"));

	/* The report door. */
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/api/v1/reports/watchlist?period=all&watchlist_id=%" G_GINT64_FORMAT, ID(list));
	answer = get_json(fixture, path, 200);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(root_of(answer), "rows")), ==, 1);
	g_assert_cmpuint(http(fixture, "GET", "/api/v1/reports/watchlist?period=all", NULL, NULL, NULL), ==, 400);

	/* Another organization's list is not found. */
	g_assert_cmpuint(http(fixture, "GET", "/api/v1/market/watchlists/999999", NULL, NULL, NULL), ==, 404);
}

/*
 * The alerts page lists recent hits and the rules; "evaluate now" runs a
 * rule and records what fires, as a feed run would. What breaks if this
 * regresses: the button evaluates without recording, or the page cannot
 * show what fired.
 */
static void
test_alerts(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(VentureEntity) rule = NULL;
	g_autoptr(VentureMoney) threshold = NULL;
	g_autoptr(JsonNode) answer = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *page = NULL;
	g_autofree gchar *body = NULL;
	JsonObject *root;

	(void)user_data;

	seed_store(fixture);
	g_assert_true(venture_marketdata_promote_instrument(fixture->context, fixture->org, fixture->source_id,
	                                                    "2770", NULL, &record, NULL));

	threshold = money_of("1.15 USD");
	rule = VENTURE_ENTITY(venture_alert_rule_new());
	venture_entity_set_organization_id(rule, fixture->org);
	g_object_set(rule, "name", "Cheap <ore>", "kind", VENTURE_ALERT_KIND_BELOW, "instrument-id", ID(record),
	             "threshold", threshold, NULL);
	save(fixture, rule);

	/* A dry run says what would fire and writes nothing. */
	path = g_strdup_printf("/api/v1/market/alerts/%" G_GINT64_FORMAT "/evaluate?record=false", ID(rule));
	g_assert_cmpuint(http(fixture, "POST", path, "{}", &body, NULL), ==, 200);
	answer = json_from_string(body, NULL);
	g_assert_cmpint(json_object_get_int_member(root_of(answer), "candidates"), ==, 1);
	g_assert_cmpint(json_object_get_int_member(root_of(answer), "written"), ==, 0);
	g_clear_pointer(&answer, json_node_unref);

	/* The button records. */
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/market/alerts/%" G_GINT64_FORMAT "/evaluate", ID(rule));
	g_assert_cmpuint(http(fixture, "POST", path, "", &page, NULL), ==, 200);
	g_assert_nonnull(strstr(page, "1 recorded"));
	g_assert_nonnull(strstr(page, "Cheap &lt;ore&gt;"));
	g_assert_null(strstr(page, "Cheap <ore>"));
	assert_buttons_named(page, path);

	answer = get_json(fixture, "/api/v1/market/alerts", 200);
	root = root_of(answer);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(root, "hits")), ==, 1);
	/* file_jsonl asks to be named by nobody: present, and empty. */
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(root, "attribution")), ==, 0);
	g_assert_cmpstr(json_object_get_string_member(
		json_array_get_object_element(json_object_get_array_member(root, "hits"), 0), "rule_name"),
		==, "Cheap <ore>");
	g_assert_false(JSON_NODE_HOLDS_NULL(json_object_get_member(
		json_array_get_object_element(json_object_get_array_member(root, "rules"), 0), "last_hit_at")));

	g_clear_pointer(&page, g_free);
	page = get_page(fixture, "/market/alerts");
	g_assert_nonnull(strstr(page, "Evaluate now"));
	g_assert_nonnull(strstr(page, "Recent hits"));
	g_assert_cmpuint(http(fixture, "POST", "/market/alerts/999999/evaluate", "", NULL, NULL), ==, 404);
}

/*
 * The three widget kinds say the same thing in their JSON and on their
 * card, read from the answers the pages read. What breaks if this
 * regresses: a card that shows one price while the assistant reads
 * another, or a widget that takes the dashboard down when its list is
 * gone.
 */
static void
test_widgets(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureDashboard) dashboard = NULL;
	g_autoptr(VentureDashboardWidget) watch = NULL;
	g_autoptr(VentureDashboardWidget) hits = NULL;
	g_autoptr(VentureDashboardWidget) health = NULL;
	g_autoptr(VentureDashboardWidget) missing = NULL;
	g_autoptr(VentureWidgetResult) result = NULL;
	g_autoptr(VentureEntity) list = NULL;
	g_autoptr(VentureEntity) entry = NULL;
	g_autoptr(VentureEntity) record = NULL;
	JsonArray *data;
	JsonObject *first;
	VentureWidgetScope scope;
	gint64 orgs[1];

	(void)user_data;

	seed_store(fixture);
	g_assert_true(venture_marketdata_promote_instrument(fixture->context, fixture->org, fixture->source_id,
	                                                    "2770", NULL, &record, NULL));
	list = VENTURE_ENTITY(venture_watchlist_new());
	venture_entity_set_organization_id(list, fixture->org);
	g_object_set(list, "name", "Ores", "group-key", "eu", NULL);
	save(fixture, list);
	entry = VENTURE_ENTITY(venture_watchlist_entry_new());
	venture_entity_set_organization_id(entry, fixture->org);
	g_object_set(entry, "watchlist-id", ID(list), "instrument-id", ID(record), NULL);
	save(fixture, entry);

	dashboard = venture_dashboard_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(dashboard), fixture->org);
	g_object_set(dashboard, "name", "Trading", "slug", "trading", NULL);
	save(fixture, dashboard);

	watch = venture_dashboard_widget_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(watch), fixture->org);
	g_object_set(watch, "dashboard-id", ID(dashboard), "kind", "watchlist", "record-id", ID(list), NULL);
	save(fixture, watch);
	hits = venture_dashboard_widget_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(hits), fixture->org);
	g_object_set(hits, "dashboard-id", ID(dashboard), "kind", "market_alerts", NULL);
	save(fixture, hits);
	health = venture_dashboard_widget_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(health), fixture->org);
	g_object_set(health, "dashboard-id", ID(dashboard), "kind", "source_health", NULL);
	save(fixture, health);

	orgs[0] = fixture->org;
	memset(&scope, 0, sizeof(scope));
	scope.organization_ids = orgs;
	scope.n_organizations = 1;

	/* The watchlist card and its data name the same price. */
	result = venture_dashboard_render_widget(fixture->context, watch, &scope);
	g_assert_null(result->error);
	g_assert_cmpstr(result->title, ==, "Ores");
	data = json_node_get_array(result->data);
	g_assert_cmpuint(json_array_get_length(data), ==, 1);
	first = json_array_get_object_element(data, 0);
	g_assert_cmpstr(json_object_get_string_member(first, "venue"), ==, "realm-b");
	g_assert_cmpint(money_amount(first, "price"), ==, 110);
	g_assert_nonnull(strstr(result->html, json_object_get_string_member(first, "price_formatted")));
	g_assert_nonnull(strstr(result->html, ">Copper Ore</a>"));
	g_clear_pointer(&result, venture_widget_result_free);

	result = venture_dashboard_render_widget(fixture->context, hits, &scope);
	g_assert_null(result->error);
	g_assert_cmpuint(json_array_get_length(json_node_get_array(result->data)), ==, 0);
	g_assert_nonnull(strstr(result->html, "Nothing has fired yet."));
	g_clear_pointer(&result, venture_widget_result_free);

	result = venture_dashboard_render_widget(fixture->context, health, &scope);
	g_assert_null(result->error);
	data = json_node_get_array(result->data);
	g_assert_cmpuint(json_array_get_length(data), ==, 1);
	first = json_array_get_object_element(data, 0);
	g_assert_cmpstr(json_object_get_string_member(first, "name"), ==, "Auctions");
	g_assert_cmpstr(json_object_get_string_member(first, "status"), ==, "never");
	g_assert_nonnull(strstr(result->html, ">Auctions</a>"));
	g_assert_nonnull(strstr(result->html, ">never</span>"));
	g_clear_pointer(&result, venture_widget_result_free);

	/* A list that is gone is an error in place, not a crash. */
	missing = venture_dashboard_widget_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(missing), fixture->org);
	g_object_set(missing, "dashboard-id", ID(dashboard), "kind", "watchlist", "record-id",
	             (gint64)999999, NULL);
	result = venture_dashboard_render_widget(fixture->context, missing, &scope);
	g_assert_nonnull(result->error);
	g_clear_pointer(&result, venture_widget_result_free);

	/* Feeds off: the health card says so, the module way. */
	g_object_set(fixture->config, "feeds-enabled", FALSE, NULL);
	result = venture_dashboard_render_widget(fixture->context, health, &scope);
	g_assert_nonnull(result->error);
}


/* --- Source attribution ------------------------------------------------------ */

/* A provider's line written to break out of the page, and what it must
 * become on one. */
static const gchar hostile_credit[] = "Data from <script>alert(\"x\")</script> & 'Co'";
static const gchar escaped_credit[] =
	"<p class=\"source-attribution\" role=\"note\"><span class=\"source-attribution-label\">Source:</span> "
	"Data from &lt;script&gt;alert(&quot;x&quot;)&lt;/script&gt; &amp; &#39;Co&#39;</p>";
static const gchar exchange_credit[] = "Prices courtesy of Example Exchange.";

/* The providers here never fetch: their stores are written directly. */
static VentureFeedBatch *
attribution_no_fetch(
	VentureFeedRequest	 *request,
	gpointer		  user_data,
	GError			**error
){
	(void)request;
	(void)user_data;

	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN, "not in this test");

	return NULL;
}

static void
attribution_provider(
	Fixture		*fixture,
	const gchar	*name,
	const gchar	*credit
){
	g_autoptr(VentureDataSourceProvider) provider = NULL;
	g_autoptr(GError) error = NULL;

	provider = venture_func_data_source_provider_new(name, NULL, NULL, attribution_no_fetch, NULL, NULL);
	g_assert_true(venture_func_data_source_provider_set_attribution(provider, credit, &error));
	g_assert_no_error(error);
	g_assert_true(venture_data_source_provider_registry_add(
		venture_context_get_data_source_providers(fixture->context), provider, &error));
	g_assert_no_error(error);
}

/* Another source of the organization, its store holding one venue that
 * offers copper ore. */
static gint64
attribution_source(
	Fixture		*fixture,
	const gchar	*name,
	const gchar	*provider,
	const gchar	*venue
){
	g_autoptr(VentureDataSource) source = NULL;
	g_autoptr(VentureSeriesStore) store = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *dir = NULL;
	const Offer offers[] = { { "2770", 90, 105, 3 } };
	gint64 now;

	source = venture_data_source_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(source), fixture->org);
	g_object_set(source, "name", name, "provider", provider, "schedule", "manual", "currency", "USD",
	             NULL);
	save(fixture, source);

	dir = venture_feeds_store_dir(fixture->config, venture_entity_get_uuid(VENTURE_ENTITY(source)));
	store = venture_series_store_open(dir, &error);
	g_assert_no_error(error);
	now = g_get_real_time() / G_USEC_PER_SEC;
	add_venue(store, venue, "eu", "USD", now - 7200);
	add_instrument(store, "2770", "Copper Ore", "Materials/Ore", now - 7200);
	snapshot(store, venue, "USD", now - 3600, offers, G_N_ELEMENTS(offers));

	return ID(source);
}

/* The answer's attribution is exactly @expected, in any order. */
static void
assert_attribution(
	JsonNode		*answer,
	const gchar *const	*expected
){
	JsonArray *lines;
	guint n;
	guint i;
	guint j;

	lines = json_object_get_array_member(root_of(answer), "attribution");
	g_assert_nonnull(lines);
	n = (NULL != expected) ? g_strv_length((gchar **)expected) : 0;
	g_assert_cmpuint(json_array_get_length(lines), ==, n);

	for (i = 0; i < n; i++)
	{
		gboolean found = FALSE;

		for (j = 0; j < json_array_get_length(lines); j++)
			found = found || (0 == g_strcmp0(json_array_get_string_element(lines, j), expected[i]));

		if (!found)
			g_error("the answer does not name \"%s\"", expected[i]);
	}
}

/*
 * A provider that declares an attribution line is named on every page,
 * API answer and card that shows its data -- once, escaped, and only
 * there: a page drawing on two providers names both, a page drawing on a
 * provider with no line (or on nothing) shows no line at all.
 *
 * What breaks if this regresses: Blizzard's terms (section 2.13) require
 * the source be named clearly and conspicuously wherever its data is
 * shown; a page that drops the line, or a plugin whose line injects a
 * script into every Trading page, is a breach or a hole.
 */
static void
test_attribution(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	static const gchar *const credited[] = { hostile_credit, NULL };
	static const gchar *const exchange[] = { exchange_credit, NULL };
	static const gchar *const both[] = { hostile_credit, exchange_credit, NULL };
	g_autoptr(VentureEntity) source = NULL;
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(VentureEntity) list = NULL;
	g_autoptr(VentureEntity) entry = NULL;
	g_autoptr(VentureDashboard) dashboard = NULL;
	g_autoptr(VentureDashboardWidget) health = NULL;
	g_autoptr(VentureDashboardWidget) watch = NULL;
	g_autoptr(VentureWidgetResult) result = NULL;
	g_autoptr(JsonNode) answer = NULL;
	g_autofree gchar *page = NULL;
	g_autofree gchar *path = NULL;
	VentureWidgetScope scope;
	JsonArray *sources;
	gint64 orgs[1];
	gint64 exchange_id;
	gint64 plain_id;
	guint i;

	(void)user_data;

	attribution_provider(fixture, "credited_feed", hostile_credit);
	attribution_provider(fixture, "exchange_feed", exchange_credit);
	attribution_provider(fixture, "plain_feed", NULL);
	seed_store(fixture);

	/* file_jsonl declares no line: the data is shown, and nothing names it. */
	answer = get_json(fixture, "/api/v1/market/browse", 200);
	g_assert_true(json_object_get_boolean_member(root_of(answer), "available"));
	assert_attribution(answer, NULL);
	g_clear_pointer(&answer, json_node_unref);
	page = get_page(fixture, "/market/browse");
	g_assert_null(strstr(page, "class=\"source-attribution\""));
	g_clear_pointer(&page, g_free);

	/* The same source behind a provider that asks to be named. */
	source = venture_database_get(fixture->database, VENTURE_TYPE_DATA_SOURCE, fixture->source_id, NULL);
	g_object_set(source, "provider", "credited_feed", "settings", "", NULL);
	save(fixture, source);

	answer = get_json(fixture, "/api/v1/market/browse", 200);
	assert_attribution(answer, credited);
	g_clear_pointer(&answer, json_node_unref);
	page = get_page(fixture, "/market/browse");
	g_assert_nonnull(strstr(page, escaped_credit));
	g_assert_null(strstr(page, hostile_credit));
	g_assert_null(strstr(page, "<script>alert(\"x\")"));
	/* Once, however many rows it supplied. */
	g_assert_null(strstr(strstr(page, escaped_credit) + strlen(escaped_credit),
	                   "class=\"source-attribution\""));
	g_clear_pointer(&page, g_free);

	path = g_strdup_printf("/market/i/%" G_GINT64_FORMAT "/2770", fixture->source_id);
	page = get_page(fixture, path);
	g_assert_nonnull(strstr(page, escaped_credit));
	g_clear_pointer(&page, g_free);
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/api/v1/market/i/%" G_GINT64_FORMAT "/2770", fixture->source_id);
	answer = get_json(fixture, path, 200);
	assert_attribution(answer, credited);
	g_clear_pointer(&answer, json_node_unref);

	answer = get_json(fixture, "/api/v1/market/deals", 200);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(root_of(answer), "rows")), >, 0);
	assert_attribution(answer, credited);
	g_clear_pointer(&answer, json_node_unref);
	page = get_page(fixture, "/market/deals");
	g_assert_nonnull(strstr(page, escaped_credit));
	g_clear_pointer(&page, g_free);

	/* Two more sources: one named by its provider, one not. */
	exchange_id = attribution_source(fixture, "Exchange", "exchange_feed", "x-venue");
	plain_id = attribution_source(fixture, "Plain", "plain_feed", "p-venue");

	/* Every source's venues on one page: both lines, never a third. */
	answer = get_json(fixture, "/api/v1/market/venues", 200);
	assert_attribution(answer, both);
	g_clear_pointer(&answer, json_node_unref);
	page = get_page(fixture, "/market/venues");
	g_assert_nonnull(strstr(page, escaped_credit));
	g_assert_nonnull(strstr(page, exchange_credit));
	g_clear_pointer(&page, g_free);

	/* One source at a time names that source only. */
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/api/v1/market/browse?source=%" G_GINT64_FORMAT, exchange_id);
	answer = get_json(fixture, path, 200);
	assert_attribution(answer, exchange);
	g_clear_pointer(&answer, json_node_unref);

	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/api/v1/market/browse?source=%" G_GINT64_FORMAT, plain_id);
	answer = get_json(fixture, path, 200);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(root_of(answer), "rows")), >, 0);
	assert_attribution(answer, NULL);
	g_clear_pointer(&answer, json_node_unref);
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/market/browse?source=%" G_GINT64_FORMAT, plain_id);
	page = get_page(fixture, path);
	g_assert_null(strstr(page, "class=\"source-attribution\""));
	g_clear_pointer(&page, g_free);

	/* A list of the organization's watchlists shows nobody's data. */
	answer = get_json(fixture, "/api/v1/market/watchlists", 200);
	assert_attribution(answer, NULL);
	g_clear_pointer(&answer, json_node_unref);

	/* The feeds: each source with its own line, every distinct one once. */
	answer = get_json(fixture, "/api/v1/feeds", 200);
	assert_attribution(answer, both);
	sources = json_object_get_array_member(root_of(answer), "sources");

	for (i = 0; i < json_array_get_length(sources); i++)
	{
		JsonObject *one = json_array_get_object_element(sources, i);
		gint64 id = json_object_get_int_member(one, "id");

		if (id == plain_id)
			g_assert_true(JSON_NODE_HOLDS_NULL(json_object_get_member(one, "attribution")));
		else if (id == exchange_id)
			g_assert_cmpstr(json_object_get_string_member(one, "attribution"), ==, exchange_credit);
		else
			g_assert_cmpstr(json_object_get_string_member(one, "attribution"), ==, hostile_credit);
	}

	g_clear_pointer(&answer, json_node_unref);
	page = get_page(fixture, "/feeds");
	g_assert_nonnull(strstr(page, escaped_credit));
	g_assert_nonnull(strstr(page, exchange_credit));
	g_assert_null(strstr(page, hostile_credit));
	g_clear_pointer(&page, g_free);

	/* The data_source record page says what its provider will show. */
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/e/data_source/%" G_GINT64_FORMAT, fixture->source_id);
	page = get_page(fixture, path);
	g_assert_nonnull(strstr(page, escaped_credit));
	g_clear_pointer(&page, g_free);
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/e/data_source/%" G_GINT64_FORMAT, plain_id);
	page = get_page(fixture, path);
	g_assert_null(strstr(page, "class=\"source-attribution\""));
	g_clear_pointer(&page, g_free);

	/* The cards: the health card lists every source, so names both; a
	 * watchlist of the credited source's ore names that one. */
	g_assert_true(venture_marketdata_promote_instrument(fixture->context, fixture->org, fixture->source_id,
	                                                    "2770", NULL, &record, NULL));
	list = VENTURE_ENTITY(venture_watchlist_new());
	venture_entity_set_organization_id(list, fixture->org);
	g_object_set(list, "name", "Ores", "group-key", "eu", NULL);
	save(fixture, list);
	entry = VENTURE_ENTITY(venture_watchlist_entry_new());
	venture_entity_set_organization_id(entry, fixture->org);
	g_object_set(entry, "watchlist-id", ID(list), "instrument-id", ID(record), NULL);
	save(fixture, entry);

	dashboard = venture_dashboard_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(dashboard), fixture->org);
	g_object_set(dashboard, "name", "Trading", "slug", "trading", NULL);
	save(fixture, dashboard);
	health = venture_dashboard_widget_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(health), fixture->org);
	g_object_set(health, "dashboard-id", ID(dashboard), "kind", "source_health", NULL);
	save(fixture, health);
	watch = venture_dashboard_widget_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(watch), fixture->org);
	g_object_set(watch, "dashboard-id", ID(dashboard), "kind", "watchlist", "record-id", ID(list), NULL);
	save(fixture, watch);

	orgs[0] = fixture->org;
	memset(&scope, 0, sizeof(scope));
	scope.organization_ids = orgs;
	scope.n_organizations = 1;

	result = venture_dashboard_render_widget(fixture->context, health, &scope);
	g_assert_null(result->error);
	g_assert_nonnull(result->attribution);
	g_assert_cmpuint(g_strv_length(result->attribution), ==, 2);
	g_assert_true(g_strv_contains((const gchar *const *)result->attribution, hostile_credit));
	g_assert_true(g_strv_contains((const gchar *const *)result->attribution, exchange_credit));
	g_assert_nonnull(strstr(result->html, escaped_credit));
	g_clear_pointer(&result, venture_widget_result_free);

	result = venture_dashboard_render_widget(fixture->context, watch, &scope);
	g_assert_null(result->error);
	g_assert_cmpuint(g_strv_length(result->attribution), ==, 1);
	g_assert_cmpstr(result->attribution[0], ==, hostile_credit);
	g_assert_nonnull(strstr(result->html, escaped_credit));
	g_assert_null(strstr(result->html, exchange_credit));
	g_clear_pointer(&result, venture_widget_result_free);

	/* An alert's hits are readings of the credited source: the evaluation,
	 * the overview, its page and the hits card all name it. */
	{
		g_autoptr(VentureEntity) rule = NULL;
		g_autoptr(VentureMoney) threshold = NULL;
		g_autoptr(VentureDashboardWidget) hits = NULL;
		g_autofree gchar *body = NULL;

		threshold = money_of("1.15 USD");
		rule = VENTURE_ENTITY(venture_alert_rule_new());
		venture_entity_set_organization_id(rule, fixture->org);
		g_object_set(rule, "name", "Cheap ore", "kind", VENTURE_ALERT_KIND_BELOW, "instrument-id", ID(record),
		             "threshold", threshold, NULL);
		save(fixture, rule);

		g_clear_pointer(&path, g_free);
		path = g_strdup_printf("/api/v1/market/alerts/%" G_GINT64_FORMAT "/evaluate", ID(rule));
		g_assert_cmpuint(http(fixture, "POST", path, "{}", &body, NULL), ==, 200);
		answer = json_from_string(body, NULL);
		g_assert_cmpint(json_object_get_int_member(root_of(answer), "written"), ==, 1);
		assert_attribution(answer, credited);
		g_clear_pointer(&answer, json_node_unref);

		answer = get_json(fixture, "/api/v1/market/alerts", 200);
		assert_attribution(answer, credited);
		g_clear_pointer(&answer, json_node_unref);
		page = get_page(fixture, "/market/alerts");
		g_assert_nonnull(strstr(page, escaped_credit));
		g_clear_pointer(&page, g_free);

		hits = venture_dashboard_widget_new();
		venture_entity_set_organization_id(VENTURE_ENTITY(hits), fixture->org);
		g_object_set(hits, "dashboard-id", ID(dashboard), "kind", "market_alerts", NULL);
		save(fixture, hits);
		result = venture_dashboard_render_widget(fixture->context, hits, &scope);
		g_assert_null(result->error);
		g_assert_cmpuint(g_strv_length(result->attribution), ==, 1);
		g_assert_nonnull(strstr(result->html, escaped_credit));
		g_clear_pointer(&result, venture_widget_result_free);
	}

	/* The watchlist page and its twin name the same source the card does. */
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/api/v1/market/watchlists/%" G_GINT64_FORMAT, ID(list));
	answer = get_json(fixture, path, 200);
	assert_attribution(answer, credited);
	g_clear_pointer(&answer, json_node_unref);

	/* The dashboard's JSON carries each card's lines beside its data. */
	{
		g_autoptr(JsonNode) dumped = NULL;
		g_autofree gchar *text = NULL;
		g_autoptr(GError) error = NULL;

		dumped = venture_dashboard_describe(fixture->context, dashboard, &scope, TRUE, &error);
		g_assert_no_error(error);
		text = json_to_string(dumped, FALSE);
		g_assert_nonnull(strstr(text, "\"attribution\":["));
		g_assert_nonnull(strstr(text, "Prices courtesy of Example Exchange."));
	}
}

typedef struct
{
	gboolean	 done;
	gchar		*out;
	gchar		*err;
	GError		*error;
} CliRun;

static void
cli_done(
	GObject		*source,
	GAsyncResult	*result,
	gpointer	 data
){
	CliRun *run = data;

	g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &run->out, &run->err, &run->error);
	run->done = TRUE;
}

/*
 * The market reports reach every door: the MCP catalogue offers their
 * options, venturectl passes them, and both looks style the new pages.
 * What breaks if this regresses: an option silently dropped on one door,
 * the report answering a different question there, or a chart drawn
 * unstyled in one look.
 */
static void
test_doors_and_looks(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	static const gchar *const options[] = {
		"data_source_id", "venue", "group_key", "category_path", "min_value", "max_pct", "top",
		"watchlist_id", NULL
	};
	static const gchar *const classes[] = {
		".chart-line-1", ".chart-line-2", ".chart-heat-cell", ".chart-grid", ".chart-axis",
		".sparkline", ".market-action-row", ".form-inline", ".chart-legend", ".chart-caption",
		".source-attribution", ".source-attribution-label", NULL
	};
	g_autoptr(VentureMcpCatalog) catalog = NULL;
	g_autoptr(JsonNode) schema = NULL;
	g_autoptr(JsonNode) tools = NULL;
	g_autoptr(GSubprocessLauncher) launcher = NULL;
	g_autoptr(GSubprocess) process = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *out = NULL;
	g_autofree gchar *err = NULL;
	g_autofree gchar *industrial = NULL;
	g_autofree gchar *classic = NULL;
	JsonObject *properties = NULL;
	guint i;

	(void)user_data;

	seed_store(fixture);

	/* MCP: the report tool declares every option. */
	schema = json_from_string("[{\"name\":\"sale\"}]", NULL);
	catalog = venture_mcp_catalog_new_from_schema(schema, &error);
	g_assert_no_error(error);
	tools = venture_mcp_catalog_get_tools(catalog);

	for (i = 0; i < json_array_get_length(json_node_get_array(tools)); i++)
	{
		JsonObject *tool = json_array_get_object_element(json_node_get_array(tools), i);

		if (0 == g_strcmp0(json_object_get_string_member(tool, "name"), "venture_report"))
			properties = json_object_get_object_member(json_object_get_object_member(tool, "inputSchema"),
			                                           "properties");
	}

	g_assert_nonnull(properties);

	for (i = 0; NULL != options[i]; i++)
		if (!json_object_has_member(properties, options[i]))
			g_error("the MCP venture_report tool does not offer %s", options[i]);

	/* The CLI: the options pass its allow-list and reach the report. */
	if (g_file_test(VENTURE_TEST_BIN_DIR "/venturectl", G_FILE_TEST_IS_EXECUTABLE))
	{
		const gchar *argv[] = { VENTURE_TEST_BIN_DIR "/venturectl", "--server",
			venture_web_server_get_base_url(fixture->server), "--format", "json", "report",
			"market_deals", "all", "group_key=us", "max_pct=100", "top=5", "min_value=0.01 EUR", NULL };

		launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
		                                     G_SUBPROCESS_FLAGS_STDERR_PIPE);
		g_subprocess_launcher_unsetenv(launcher, "VENTURE_TOKEN");
		process = g_subprocess_launcher_spawnv(launcher, argv, &error);
		g_assert_no_error(error);

		/* The server answering it runs on this thread's loop. */
		{
			CliRun run;

			memset(&run, 0, sizeof(run));
			g_subprocess_communicate_utf8_async(process, NULL, NULL, cli_done, &run);

			while (!run.done)
				g_main_context_iteration(NULL, TRUE);

			g_assert_no_error(run.error);

			if (!g_subprocess_get_successful(process))
				g_error("venturectl report market_deals failed: %s", run.err);

			/* Only the US venue, in euros, under the bound: realm-c. */
			g_assert_nonnull(strstr(run.out, "realm-c"));
			g_assert_null(strstr(run.out, "realm-a"));
			out = g_steal_pointer(&run.out);
			err = g_steal_pointer(&run.err);
		}
	}

	/* Both looks carry the market rules. */
	industrial = get_page(fixture, "/market/browse");
	g_assert_nonnull(strstr(industrial, "instrument panel"));
	g_object_set(fixture->config, "ui-look", "classic", NULL);
	classic = get_page(fixture, "/market/browse");
	g_assert_nonnull(strstr(classic, "warm monochrome"));

	for (i = 0; NULL != classes[i]; i++)
	{
		if ((NULL == strstr(industrial, classes[i])) || (NULL == strstr(classic, classes[i])))
			g_error("%s is not styled in both looks", classes[i]);
	}

	(void)out;
	(void)err;
}

#define ADD(path, func) \
	g_test_add("/market-pages/" path, Fixture, NULL, fixture_set_up, func, fixture_tear_down)

gint
main(
	gint	 argc,
	gchar	**argv
){
	g_test_init(&argc, &argv, NULL);

	ADD("no-sources", test_no_sources);
	ADD("empty-store", test_empty_store);
	ADD("browse", test_browse);
	ADD("instrument", test_instrument);
	ADD("instrument-escaping", test_instrument_escaping);
	ADD("other-organization", test_other_organization);
	ADD("deals", test_deals);
	ADD("venue-index", test_venue_index);
	ADD("watchlist-and-actions", test_watchlist_and_actions);
	ADD("alerts", test_alerts);
	ADD("widgets", test_widgets);
	ADD("attribution", test_attribution);
	ADD("doors-and-looks", test_doors_and_looks);

	return g_test_run();
}

#else /* !VENTURE_HAVE_SQLITE */

gint
main(
	gint	 argc,
	gchar	**argv
){
	g_test_init(&argc, &argv, NULL);
	return g_test_run();
}

#endif /* VENTURE_HAVE_SQLITE */
