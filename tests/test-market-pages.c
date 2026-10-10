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
#include <sqlite3.h>
#endif

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

	guint answered;

	answered = http(fixture, "GET", path, NULL, &body, NULL);

	/* Say which question and what came back: "400 == 200" alone names
	 * neither. */
	if (answered != status)
		g_test_message("GET %s answered %u: %s", path, answered, body);

	g_assert_cmpuint(answered, ==, status);
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
	/* Onto a list in one step: a list named or a new name, here. */
	g_assert_nonnull(strstr(page, "class=\"list-add\"><input type=\"hidden\" name=\"action\" value=\"watch\">"));
	g_assert_nonnull(strstr(page, "list=\"watchlist-names\""));
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
	g_assert_nonnull(strstr(page, "<h1 class=\"item-title\"><span>&lt;script&gt;alert(&quot;pwn&quot;)&lt;/script&gt;</span></h1>"));
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
/*
 * Where to buy and where to sell, picked: sell_venue keeps the deals that
 * sell there and says so on every row; venue and sell_venue together are
 * one route; the pickers offer only the venue group's venues, and a
 * venue left over from another group is set aside with a note rather
 * than emptying the page.
 */
static void
test_deals_buy_sell_venues(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(JsonNode) answer = NULL;
	g_autoptr(VentureEntity) group = NULL;
	g_autofree gchar *buy = NULL;
	g_autofree gchar *sell = NULL;
	g_autofree gchar *sell_name = NULL;
	g_autofree gchar *escaped = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *page = NULL;
	g_autofree gchar *venues = NULL;
	JsonArray *rows;
	JsonArray *choices;
	gboolean found = FALSE;
	guint i;

	(void)user_data;
	seed_store(fixture);

	/* A route the data has: the first deal with somewhere to sell. */
	answer = get_json(fixture, "/api/v1/market/deals?sort=profit", 200);
	rows = json_object_get_array_member(root_of(answer), "rows");

	for (i = 0; (NULL == sell) && (i < json_array_get_length(rows)); i++)
	{
		JsonObject *row = json_array_get_object_element(rows, i);

		if (!json_object_has_member(row, "sell"))
			continue;

		buy = g_strdup(json_object_get_string_member(row, "venue_key"));
		sell = g_strdup(json_object_get_string_member(json_object_get_object_member(row, "sell"), "venue_key"));
		sell_name = g_strdup(json_object_get_string_member(json_object_get_object_member(row, "sell"),
		                                                   "venue_name"));
	}

	g_assert_nonnull(sell);

	/* With no group, every venue is offered. */
	choices = json_object_get_array_member(root_of(answer), "venue_choices");
	g_assert_cmpuint(json_array_get_length(choices), >, 2);
	g_clear_pointer(&answer, json_node_unref);

	/* Sell there -- picked by name, as the page picks it: every row
	 * sells there, and that route is among them. */
	escaped = g_uri_escape_string(sell_name, NULL, FALSE);
	path = g_strdup_printf("/api/v1/market/deals?sell_venue=%s", escaped);
	answer = get_json(fixture, path, 200);
	rows = json_object_get_array_member(root_of(answer), "rows");
	g_assert_cmpuint(json_array_get_length(rows), >, 0);
	g_assert_cmpstr(json_object_get_string_member(root_of(answer), "sell_venue"), ==, sell_name);

	for (i = 0; i < json_array_get_length(rows); i++)
	{
		JsonObject *row = json_array_get_object_element(rows, i);

		g_assert_true(json_object_has_member(row, "sell"));
		g_assert_cmpstr(json_object_get_string_member(json_object_get_object_member(row, "sell"),
		                                              "venue_key"), ==, sell);
		found = found || (0 == g_strcmp0(json_object_get_string_member(row, "venue_key"), buy));
	}

	g_assert_true(found);
	g_clear_pointer(&answer, json_node_unref);
	g_clear_pointer(&path, g_free);

	/* Buy here, sell there: one route. */
	path = g_strdup_printf("/api/v1/market/deals?venue=%s&sell_venue=%s", buy, sell);
	answer = get_json(fixture, path, 200);
	rows = json_object_get_array_member(root_of(answer), "rows");
	g_assert_cmpuint(json_array_get_length(rows), >, 0);

	for (i = 0; i < json_array_get_length(rows); i++)
	{
		JsonObject *row = json_array_get_object_element(rows, i);

		g_assert_cmpstr(json_object_get_string_member(row, "venue_key"), ==, buy);
		g_assert_cmpstr(json_object_get_string_member(json_object_get_object_member(row, "sell"),
		                                              "venue_key"), ==, sell);
	}

	g_clear_pointer(&answer, json_node_unref);
	g_clear_pointer(&path, g_free);

	/* A group of the buy venue alone: the pickers offer it alone, and
	 * the sell venue, outside it, is set aside with a note. */
	group = VENTURE_ENTITY(venture_venue_group_new());
	venture_entity_set_organization_id(group, fixture->org);
	g_object_set(group, "name", "Just one", "venues", buy, NULL);
	save(fixture, group);
	path = g_strdup_printf("/api/v1/market/deals?venue_group=%" G_GINT64_FORMAT "&sell_venue=%s", ID(group), sell);
	answer = get_json(fixture, path, 200);
	choices = json_object_get_array_member(root_of(answer), "venue_choices");
	g_assert_cmpuint(json_array_get_length(choices), ==, 1);
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(choices, 0), "name"), ==,
	                json_object_get_string_member(json_array_get_object_element(json_object_get_array_member(
	                	root_of(answer), "rows"), 0), "venue_name"));
	g_assert_null(json_object_get_string_member_with_default(root_of(answer), "sell_venue", NULL));
	venues = json_to_string(json_object_get_member(root_of(answer), "notes"), FALSE);
	g_assert_nonnull(strstr(venues, "The sell venue is not in this venue group"));
	g_clear_pointer(&answer, json_node_unref);
	g_clear_pointer(&path, g_free);

	/* The page: two pickers, the chosen ones selected. */
	path = g_strdup_printf("/market/deals?venue=%s&sell_venue=%s", buy, escaped);
	page = get_page(fixture, path);
	g_assert_nonnull(strstr(page, "<select name=\"venue\">"));
	g_assert_nonnull(strstr(page, "<select name=\"sell_venue\">"));
	{
		g_autofree gchar *chosen = g_strdup_printf("<option value=\"%s\" selected>", sell_name);
		const gchar *picker = strstr(page, "<select name=\"sell_venue\">");

		g_assert_nonnull(strstr(picker, chosen));
	}
}

/*
 * Every price on Deals and an item's page says how old it is, and one past
 * series.stale_minutes is marked stale in the answer and on the page.
 * What breaks if this regresses: a realm whose feed stopped hours ago
 * reads as the cheapest place to buy, with nothing to say its price is
 * old -- which is how Area 52 was called cheapest when Zul'jin was.
 */
static void
test_deals_freshness(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(JsonNode) answer = NULL;
	g_autofree gchar *page = NULL;
	g_autofree gchar *path = NULL;
	JsonArray *rows;
	JsonArray *venues;
	gboolean sold = FALSE;
	guint i;

	(void)user_data;
	seed_store(fixture);

	/* Every snapshot is an hour old: fresh under the default two hours. */
	answer = get_json(fixture, "/api/v1/market/deals?sort=profit", 200);
	g_assert_cmpint(json_object_get_int_member(root_of(answer), "stale_after_seconds"), ==, 7200);
	rows = json_object_get_array_member(root_of(answer), "rows");
	g_assert_cmpuint(json_array_get_length(rows), >, 0);

	for (i = 0; i < json_array_get_length(rows); i++)
	{
		JsonObject *row = json_array_get_object_element(rows, i);
		gint64 age = json_object_get_int_member(row, "buy_age_seconds");

		g_assert_cmpstr(json_object_get_string_member(row, "buy_taken_at"), ==,
		                json_object_get_string_member(row, "taken_at"));
		g_assert_cmpint(age, >=, 3600);
		g_assert_cmpint(age, <, 3600 + 300);
		g_assert_false(json_object_get_boolean_member(row, "buy_stale"));

		if (json_object_has_member(row, "sell"))
		{
			JsonObject *sell = json_object_get_object_member(row, "sell");

			sold = TRUE;
			g_assert_cmpstr(json_object_get_string_member(row, "sell_taken_at"), ==,
			                json_object_get_string_member(sell, "taken_at"));
			g_assert_false(json_object_get_boolean_member(row, "sell_stale"));
			g_assert_false(json_object_get_boolean_member(sell, "stale"));
			g_assert_cmpint(json_object_get_int_member(sell, "age_seconds"), >=, 3600);
		}
	}

	g_assert_true(sold);
	g_clear_pointer(&answer, json_node_unref);

	page = get_page(fixture, "/market/deals?sort=profit");
	g_assert_nonnull(strstr(page, "<span class=\"price-age\"><time datetime=\""));
	g_assert_nonnull(strstr(page, "title=\"Price as of "));
	g_assert_nonnull(strstr(page, ">1h</time>"));
	/* (The page inlines the stylesheet, which names the class.) */
	g_assert_null(strstr(page, "<span class=\"price-age is-stale\">"));
	g_assert_null(strstr(page, "<td class=\"is-stale\""));
	g_clear_pointer(&page, g_free);

	/* Half an hour: the same prices are stale now, both sides. */
	g_object_set(fixture->config, "series-stale-minutes", (gint64)30, NULL);
	answer = get_json(fixture, "/api/v1/market/deals?sort=profit", 200);
	g_assert_cmpint(json_object_get_int_member(root_of(answer), "stale_after_seconds"), ==, 1800);
	rows = json_object_get_array_member(root_of(answer), "rows");

	for (i = 0; i < json_array_get_length(rows); i++)
	{
		JsonObject *row = json_array_get_object_element(rows, i);

		g_assert_true(json_object_get_boolean_member(row, "buy_stale"));
		if (json_object_has_member(row, "sell"))
			g_assert_true(json_object_get_boolean_member(row, "sell_stale"));
	}

	g_clear_pointer(&answer, json_node_unref);
	page = get_page(fixture, "/market/deals?sort=profit");
	g_assert_nonnull(strstr(page, "<td class=\"is-stale\""));
	g_assert_nonnull(strstr(page, "<span class=\"price-age is-stale\"><time datetime=\""));
	g_assert_nonnull(strstr(page, "<span class=\"price-age-flag\">stale</span>"));
	g_clear_pointer(&page, g_free);

	/* An item's venues and its "Now" card, the same way. */
	path = g_strdup_printf("/api/v1/market/i/%" G_GINT64_FORMAT "/2770", fixture->source_id);
	answer = get_json(fixture, path, 200);
	g_assert_true(json_object_get_boolean_member(json_object_get_object_member(root_of(answer), "base"),
	                                             "stale"));
	venues = json_object_get_array_member(root_of(answer), "venues");
	g_assert_cmpuint(json_array_get_length(venues), >, 0);

	for (i = 0; i < json_array_get_length(venues); i++)
	{
		JsonObject *venue = json_array_get_object_element(venues, i);

		g_assert_true(json_object_get_boolean_member(venue, "stale"));
		g_assert_cmpint(json_object_get_int_member(venue, "age_seconds"), >=, 3600);
	}

	g_clear_pointer(&answer, json_node_unref);
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/market/i/%" G_GINT64_FORMAT "/2770", fixture->source_id);
	page = get_page(fixture, path);
	g_assert_nonnull(strstr(page, "Price as of"));
	g_assert_nonnull(strstr(page, "<span class=\"price-age-flag\">stale</span>"));
}

/* A venue named as a source names it, not by its key. */
static void
add_named_venue(
	VentureSeriesStore	*store,
	const gchar		*key,
	const gchar		*name,
	gint64			 seen_at
){
	g_autoptr(GError) error = NULL;
	VentureSeriesVenue venue;

	memset(&venue, 0, sizeof(venue));
	venue.key = key;
	venue.namespace_ = "realm";
	venue.name = name;
	venue.kind = "auction_house";
	venue.group_key = "eu";
	venue.currency = "USD";
	g_assert_true(venture_series_store_upsert_venue(store, &venue, seen_at, &error));
	g_assert_no_error(error);
}

/*
 * One connected realm, two sources' keys for it: the fixture's source
 * (Blizzard's way) knows it as venue "12", named for its three realms;
 * a second source (a TSM push's way) keeps two of the realms apart, by
 * slug. Copper ore is cheap there in both.
 */
static gint64
seed_connected_realm(Fixture *fixture)
{
	g_autoptr(VentureDataSource) second = NULL;
	g_autoptr(VentureEntity) source = NULL;
	g_autoptr(VentureSeriesStore) store = NULL;
	g_autoptr(VentureSeriesStore) other = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *dir = NULL;
	g_autofree gchar *other_dir = NULL;
	gint64 now = g_get_real_time() / G_USEC_PER_SEC;
	gint64 t = now - 3600;
	static const Offer cheap[] = { { "2770", 40, 60, 9 } };
	static const Offer slug_a[] = { { "2770", 50, 60, 9 } };
	static const Offer slug_b[] = { { "2770", 51, 60, 9 } };
	static const Offer slug_c[] = { { "2770", 52, 150, 2 } };

	source = venture_database_get(fixture->database, VENTURE_TYPE_DATA_SOURCE, fixture->source_id, NULL);
	dir = venture_feeds_store_dir(fixture->config, venture_entity_get_uuid(source));
	store = venture_series_store_open(dir, &error);
	g_assert_no_error(error);
	add_named_venue(store, "12", "Silver Hand, Thorium Brotherhood, Farstriders", t);
	snapshot(store, "12", "USD", t, cheap, G_N_ELEMENTS(cheap));
	g_assert_true(venture_series_store_recompute_region(store, NULL, now, VENTURE_SERIES_NONE, NULL,
	                                                    NULL, &error));
	g_assert_no_error(error);

	second = venture_data_source_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(second), fixture->org);
	g_object_set(second, "name", "Pushes", "provider", "file_jsonl", "settings", "file: z.jsonl",
	             "schedule", "manual", "currency", "USD", "instrument-namespace", "wow-item",
	             "venue-namespace", "realm", NULL);
	save(fixture, second);
	other_dir = venture_feeds_store_dir(fixture->config, venture_entity_get_uuid(VENTURE_ENTITY(second)));
	other = venture_series_store_open(other_dir, &error);
	g_assert_no_error(error);
	add_named_venue(other, "silver-hand", "Silver Hand", t);
	add_named_venue(other, "thorium-brotherhood", "Thorium Brotherhood", t);
	add_named_venue(other, "elsewhere", "Elsewhere", t);
	add_instrument(other, "2770", "Copper Ore", "Materials/Ore", t);
	snapshot(other, "silver-hand", "USD", t, slug_a, G_N_ELEMENTS(slug_a));
	snapshot(other, "thorium-brotherhood", "USD", t, slug_b, G_N_ELEMENTS(slug_b));
	snapshot(other, "elsewhere", "USD", t, slug_c, G_N_ELEMENTS(slug_c));
	g_assert_true(venture_series_store_recompute_region(other, NULL, now, VENTURE_SERIES_NONE, NULL,
	                                                    NULL, &error));
	g_assert_no_error(error);

	return ID(second);
}

/*
 * The Deals pickers offer a connected realm once, however many sources
 * key it and however many of its realms one source keeps apart, labelled
 * by the realms a source keeps alone; any member's name or any source's
 * key picks it, in every source. What breaks if it regresses: "Silver
 * Hand", "Thorium Brotherhood" and "Silver Hand, Thorium Brotherhood,
 * Farstriders" offered as three markets, and a pick by one name finding
 * nothing in the source that knows only the connected realm.
 */
static void
test_deals_connected_realms(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	static const gchar *const asked[] = {
		"Farstriders", "thorium-brotherhood", "12", "silver%20hand",
		"Silver%20Hand%2C%20Thorium%20Brotherhood%20(%2B%20Farstriders)",
	};
	static const gchar label[] = "Silver Hand, Thorium Brotherhood (+ Farstriders)";
	g_autoptr(VentureEntity) group = NULL;
	g_autoptr(JsonNode) answer = NULL;
	g_autofree gchar *page = NULL;
	g_autofree gchar *path = NULL;
	JsonArray *choices;
	JsonObject *realm = NULL;
	gint64 second;
	guint found = 0;
	guint i;

	(void)user_data;
	seed_store(fixture);
	second = seed_connected_realm(fixture);

	answer = get_json(fixture, "/api/v1/market/deals", 200);
	choices = json_object_get_array_member(root_of(answer), "venue_choices");

	for (i = 0; i < json_array_get_length(choices); i++)
	{
		JsonObject *choice = json_array_get_object_element(choices, i);
		const gchar *name = json_object_get_string_member(choice, "name");

		/* No member realm is offered on its own beside it. */
		g_assert_cmpstr(name, !=, "Silver Hand");
		g_assert_cmpstr(name, !=, "Thorium Brotherhood");
		g_assert_cmpstr(name, !=, "Silver Hand, Thorium Brotherhood, Farstriders");

		if (0 == g_strcmp0(name, label))
		{
			realm = choice;
			found++;
		}
	}

	g_assert_cmpuint(found, ==, 1);
	g_assert_cmpstr(json_object_get_string_member(realm, "value"), ==, label);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(realm, "members")), ==, 3);
	/* Three venues across two sources, each with its own age. */
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(realm, "venues")), ==, 3);
	g_assert_false(json_object_get_boolean_member(
		json_array_get_object_element(json_object_get_array_member(realm, "venues"), 0), "stale"));
	g_clear_pointer(&answer, json_node_unref);

	/* Any name or key for it buys there, in both sources. */
	for (i = 0; i < G_N_ELEMENTS(asked); i++)
	{
		JsonArray *rows;
		gboolean first = FALSE;
		gboolean other = FALSE;
		guint j;

		g_clear_pointer(&path, g_free);
		path = g_strdup_printf("/api/v1/market/deals?search=copper%%20ore&venue=%s", asked[i]);
		answer = get_json(fixture, path, 200);
		g_assert_cmpstr(json_object_get_string_member(root_of(answer), "buy_venue"), ==, label);
		rows = json_object_get_array_member(root_of(answer), "rows");

		for (j = 0; j < json_array_get_length(rows); j++)
		{
			JsonObject *row = json_array_get_object_element(rows, j);
			const gchar *venue = json_object_get_string_member(row, "venue_key");

			g_assert_true((0 == g_strcmp0(venue, "12")) || (0 == g_strcmp0(venue, "silver-hand")) ||
			              (0 == g_strcmp0(venue, "thorium-brotherhood")));
			g_assert_cmpstr(json_object_get_string_member(row, "realm"), ==, label);
			first = first || (json_object_get_int_member(row, "data_source_id") == fixture->source_id);
			other = other || (json_object_get_int_member(row, "data_source_id") == second);
		}

		if (!first || !other)
			g_error("venue=%s: deals from %s", asked[i], first ? "the first source only"
			                                               : other ? "the second source only" : "neither");
		g_clear_pointer(&answer, json_node_unref);
	}

	/* A saved group naming one realm takes the whole connected realm,
	 * and its picker offers that realm once. */
	group = VENTURE_ENTITY(venture_venue_group_new());
	venture_entity_set_organization_id(group, fixture->org);
	g_object_set(group, "name", "Bank", "venues", "Farstriders", NULL);
	save(fixture, group);
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/api/v1/market/deals?search=copper%%20ore&venue_group=%" G_GINT64_FORMAT, ID(group));
	answer = get_json(fixture, path, 200);
	choices = json_object_get_array_member(root_of(answer), "venue_choices");
	g_assert_cmpuint(json_array_get_length(choices), ==, 1);
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(choices, 0), "name"), ==, label);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(root_of(answer), "rows")), >=, 2);
	g_clear_pointer(&answer, json_node_unref);

	/* The page: one option for it, selected when picked by a member. */
	page = get_page(fixture, "/market/deals?venue=Farstriders");
	{
		const gchar *picker = strstr(page, "<select name=\"venue\">");
		const gchar *end = (NULL != picker) ? strstr(picker, "</select>") : NULL;
		g_autofree gchar *options = (NULL != end) ? g_strndup(picker, (gsize)(end - picker)) : NULL;
		g_autofree gchar *selected = g_strdup_printf("<option value=\"Silver Hand, Thorium Brotherhood "
		                                             "(+ Farstriders)\" selected>");

		g_assert_nonnull(options);
		g_assert_nonnull(strstr(options, selected));
		g_assert_null(strstr(options, "<option value=\"Thorium Brotherhood\""));
	}
}

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

	/* Where to sell: never the venue it is bought at, in its currency, and
	 * the profit exactly the sell price less the cut (rounded up, so never
	 * overstated) less the buy price; most profit first when asked. */
	g_clear_pointer(&answer, json_node_unref);
	answer = get_json(fixture, "/api/v1/market/deals?sort=profit", 200);
	rows = json_object_get_array_member(root_of(answer), "rows");
	g_assert_cmpfloat(json_object_get_double_member(root_of(answer), "cut_pct"), ==, 5.0);

	{
		gint64 last_profit = G_MAXINT64;
		gboolean any_sell = FALSE;

		for (i = 0; i < json_array_get_length(rows); i++)
		{
			JsonObject *row = json_array_get_object_element(rows, i);
			JsonObject *sell;
			gint64 sell_price;
			gint64 profit;

			if (!json_object_has_member(row, "sell"))
			{
				last_profit = G_MININT64;
				continue;
			}

			any_sell = TRUE;
			sell = json_object_get_object_member(row, "sell");
			g_assert_cmpstr(json_object_get_string_member(sell, "venue_key"), !=,
			                json_object_get_string_member(row, "venue_key"));
			g_assert_cmpstr(json_object_get_string_member(sell, "currency"), ==,
			                json_object_get_string_member(row, "currency"));
			sell_price = money_amount(sell, "min_price");
			profit = money_amount(row, "profit");
			g_assert_cmpint(profit, ==, sell_price - (gint64)ceil(sell_price * 0.05) - money_amount(row, "min_price"));
			g_assert_cmpfloat(fabs(json_object_get_double_member(row, "roi_pct") -
			                       100.0 * profit / money_amount(row, "min_price")), <, 0.001);
			g_assert_cmpint(profit, <=, last_profit);
			last_profit = profit;
		}

		g_assert_true(any_sell);
	}

	g_clear_pointer(&answer, json_node_unref);

	/* No cut: the profit is the plain difference. */
	answer = get_json(fixture, "/api/v1/market/deals?cut=0", 200);
	rows = json_object_get_array_member(root_of(answer), "rows");

	for (i = 0; i < json_array_get_length(rows); i++)
	{
		JsonObject *row = json_array_get_object_element(rows, i);

		if (json_object_has_member(row, "sell"))
			g_assert_cmpint(money_amount(row, "profit"), ==,
			                money_amount(json_object_get_object_member(row, "sell"), "min_price") -
			                money_amount(row, "min_price"));
	}

	g_assert_cmpuint(http(fixture, "GET", "/api/v1/market/deals?cut=100", NULL, NULL, NULL), ==, 400);
	g_assert_cmpuint(http(fixture, "GET", "/api/v1/market/deals?cut=five", NULL, NULL, NULL), ==, 400);
	g_assert_cmpuint(http(fixture, "GET", "/api/v1/market/deals?sort=price", NULL, NULL, NULL), ==, 400);
	g_assert_cmpuint(http(fixture, "GET", "/api/v1/market/deals?sort=buy&dir=up", NULL, NULL, NULL), ==, 400);

	/* Every column sorts, either way: buy price cheapest first by default,
	 * dearest first when turned round; names A to Z, case-folded. */
	g_clear_pointer(&answer, json_node_unref);
	answer = get_json(fixture, "/api/v1/market/deals?sort=buy", 200);
	rows = json_object_get_array_member(root_of(answer), "rows");

	for (i = 1; i < json_array_get_length(rows); i++)
		g_assert_cmpint(money_amount(json_array_get_object_element(rows, i - 1), "min_price"), <=,
		                money_amount(json_array_get_object_element(rows, i), "min_price"));

	g_clear_pointer(&answer, json_node_unref);
	answer = get_json(fixture, "/api/v1/market/deals?sort=buy&dir=desc", 200);
	rows = json_object_get_array_member(root_of(answer), "rows");

	for (i = 1; i < json_array_get_length(rows); i++)
		g_assert_cmpint(money_amount(json_array_get_object_element(rows, i - 1), "min_price"), >=,
		                money_amount(json_array_get_object_element(rows, i), "min_price"));

	g_clear_pointer(&answer, json_node_unref);
	answer = get_json(fixture, "/api/v1/market/deals?sort=name", 200);
	rows = json_object_get_array_member(root_of(answer), "rows");

	for (i = 1; i < json_array_get_length(rows); i++)
	{
		g_autofree gchar *x = g_utf8_casefold(json_object_get_string_member(json_array_get_object_element(rows, i - 1),
		                                                                     "instrument_name"), -1);
		g_autofree gchar *y = g_utf8_casefold(json_object_get_string_member(json_array_get_object_element(rows, i),
		                                                                     "instrument_name"), -1);

		g_assert_cmpint(g_utf8_collate(x, y), <=, 0);
	}

	/* The headings sort, keeping the question; the active one turns
	 * round and says which way it is sorted. */
	g_clear_pointer(&page, g_free);
	page = get_page(fixture, "/market/deals?group=eu&sort=buy");
	g_assert_nonnull(strstr(page, "href=\"/market/deals?group=eu&amp;sort=ilvl\""));
	g_assert_nonnull(strstr(page, "aria-sort=\"ascending\" data-sort-type=\"num\" data-sort-default=\"asc\">"
	                              "<a href=\"/market/deals?group=eu&amp;sort=buy&amp;dir=desc\""));

	/* And they sort in place: the table says so, and every cell carries
	 * the raw figure (minor units for money) the page's script sorts by. */
	g_assert_nonnull(strstr(page, "data-client-sort"));
	g_assert_nonnull(strstr(page, "data-sort-value=\""));

	g_clear_pointer(&page, g_free);
	page = get_page(fixture, "/market/deals?group=eu");
	g_assert_nonnull(strstr(page, "Buy at"));
	g_assert_nonnull(strstr(page, "Sell at"));
	g_assert_nonnull(strstr(page, "tone-chip"));
	assert_buttons_named(page, "/market/deals");
}

/*
 * A listing far above what an item goes for is an asking price, not a
 * market: a venue whose lowest price is over twice the region's median is
 * never where a deal is sold, however dear.
 */
static void
test_deals_ignore_asking_prices(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) source = NULL;
	g_autoptr(VentureSeriesStore) store = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) answer = NULL;
	g_autofree gchar *dir = NULL;
	JsonArray *rows;
	gint64 now;
	guint i;
	static const Offer d2[] = { { "2770", 40, 100000, 1 }, { "2447", 41, 90000, 1 } };

	(void)user_data;
	seed_store(fixture);

	source = venture_database_get(fixture->database, VENTURE_TYPE_DATA_SOURCE, fixture->source_id, NULL);
	dir = venture_feeds_store_dir(fixture->config, venture_entity_get_uuid(source));
	store = venture_series_store_open(dir, &error);
	g_assert_no_error(error);
	now = g_get_real_time() / G_USEC_PER_SEC;
	add_venue(store, "realm-troll", "eu", "USD", now - 3600);
	snapshot(store, "realm-troll", "USD", now - 3600, d2, G_N_ELEMENTS(d2));
	g_assert_true(venture_series_store_recompute_region(store, NULL, now, VENTURE_SERIES_NONE, NULL,
	                                                    NULL, &error));
	g_assert_no_error(error);
	g_clear_object(&store);

	answer = get_json(fixture, "/api/v1/market/deals?sort=profit", 200);
	rows = json_object_get_array_member(root_of(answer), "rows");

	for (i = 0; i < json_array_get_length(rows); i++)
	{
		JsonObject *row = json_array_get_object_element(rows, i);

		if (json_object_has_member(row, "sell"))
			g_assert_cmpstr(json_object_get_string_member(json_object_get_object_member(row, "sell"),
			                                              "venue_key"), !=, "realm-troll");
	}
}

/* --- Velocity, realisable profit and another source's view ----------------- */

typedef struct
{
	const gchar	*instrument;
	guint64		 id;
	gint64		 price;
	gint64		 quantity;
	gint64		 expires;	/* expires_in_min: seconds, -1 for unknown */
} Timed;

static void
snapshot_timed(
	VentureSeriesStore	*store,
	const gchar		*venue,
	gint64			 taken_at,
	const Timed		*offers,
	guint			 n_offers
){
	g_autoptr(GError) error = NULL;
	VentureSeriesSnapshot *snap;
	VentureSeriesCommitResult result;
	guint i;

	snap = venture_series_store_begin_snapshot(store, venue, "USD", taken_at, taken_at + 30, TRUE, &error);
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
		listing.expires_in_min = offers[i].expires;
		g_assert_true(venture_series_snapshot_add_listing(snap, &listing, &error));
		g_assert_no_error(error);
	}

	g_assert_true(venture_series_store_commit_snapshot(store, snap, &result, &error));
	g_assert_no_error(error);
}

/*
 * Three EU realms, two snapshots four minutes apart inside one UTC day,
 * so every sale lands on one day of history:
 *
 *   9001  realm-cheap 1.00 x5 and 1.20 x5 (the book a buyer walks)
 *         realm-fast  3.00 x3 stays; 14 more vanish with no expiry: sold
 *         realm-slow  4.00 x2 stays; one vanishes with no expiry (sold),
 *                     ten vanish with a minute left (may have expired)
 *   9003  realm-cheap 1.00 x1; realm-slow 10.00 x1 stays, one sold
 *
 * So realm-slow sells 9001 one a day at a sale rate of 1/11 -- ten of
 * the eleven listings that vanished could simply have run out -- and
 * realm-fast fourteen a day at a rate of one.
 */
static void
seed_velocity(Fixture *fixture)
{
	g_autoptr(VentureEntity) source = NULL;
	g_autoptr(VentureSeriesStore) store = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *dir = NULL;
	gint64 now = g_get_real_time() / G_USEC_PER_SEC;
	gint64 day = now - now % 86400;
	gint64 t0;
	gint64 t1;
	static const Timed cheap[] = {
		{ "9001", 100, 100, 5, -1 }, { "9001", 101, 120, 5, -1 }, { "9003", 102, 100, 1, -1 }
	};
	static const Timed fast0[] = { { "9001", 200, 300, 3, -1 }, { "9001", 201, 300, 14, -1 } };
	static const Timed fast1[] = { { "9001", 200, 300, 3, -1 } };
	static const Timed slow0[] = {
		{ "9001", 300, 400, 2, -1 }, { "9001", 301, 400, 1, -1 },
		{ "9001", 302, 400, 1, 60 }, { "9001", 303, 400, 1, 60 }, { "9001", 304, 400, 1, 60 },
		{ "9001", 305, 400, 1, 60 }, { "9001", 306, 400, 1, 60 }, { "9001", 307, 400, 1, 60 },
		{ "9001", 308, 400, 1, 60 }, { "9001", 309, 400, 1, 60 }, { "9001", 310, 400, 1, 60 },
		{ "9001", 311, 400, 1, 60 },
		{ "9003", 320, 1000, 1, -1 }, { "9003", 321, 1000, 1, -1 }
	};
	static const Timed slow1[] = { { "9001", 300, 400, 2, -1 }, { "9003", 320, 1000, 1, -1 } };

	/* Two snapshots on one UTC day, whatever the hour: the day's first
	 * ten minutes would put them in the past few seconds' day before. */
	if (now - day < 600)
		day -= 86400;
	t0 = day + 60;
	t1 = day + 300;

	source = venture_database_get(fixture->database, VENTURE_TYPE_DATA_SOURCE, fixture->source_id, NULL);
	dir = venture_feeds_store_dir(fixture->config, venture_entity_get_uuid(source));
	store = venture_series_store_open(dir, &error);
	g_assert_no_error(error);

	add_venue(store, "realm-cheap", "eu", "USD", t0);
	add_venue(store, "realm-fast", "eu", "USD", t0);
	add_venue(store, "realm-slow", "eu", "USD", t0);
	add_instrument(store, "9001", "Velvet Thread", "Trade Goods", t0);
	add_instrument(store, "9003", "Gilded Clasp", "Trade Goods", t0);

	snapshot_timed(store, "realm-fast", t0, fast0, G_N_ELEMENTS(fast0));
	snapshot_timed(store, "realm-slow", t0, slow0, G_N_ELEMENTS(slow0));
	snapshot_timed(store, "realm-cheap", t1, cheap, G_N_ELEMENTS(cheap));
	snapshot_timed(store, "realm-fast", t1, fast1, G_N_ELEMENTS(fast1));
	snapshot_timed(store, "realm-slow", t1, slow1, G_N_ELEMENTS(slow1));

	g_assert_true(venture_series_store_recompute_region(store, NULL, now, VENTURE_SERIES_NONE, NULL,
	                                                    NULL, &error));
	g_assert_no_error(error);
}

/* The row of @rows bought at @venue for @key, or NULL. */
static JsonObject *
deal_row(
	JsonArray	*rows,
	const gchar	*key,
	const gchar	*venue
){
	guint i;

	for (i = 0; i < json_array_get_length(rows); i++)
	{
		JsonObject *row = json_array_get_object_element(rows, i);

		if ((0 == g_strcmp0(json_object_get_string_member(row, "instrument_key"), key)) &&
		    (0 == g_strcmp0(json_object_get_string_member(row, "venue_key"), venue)))
			return row;
	}

	return NULL;
}

static const gchar *
sell_venue_of(JsonObject *row)
{
	return json_object_get_string_member(json_object_get_object_member(row, "sell"), "venue_key");
}

/*
 * How fast a deal sells where it is sold, and what that leaves of its
 * profit. Vanished listings that could have run out are expiries, not
 * sales, so realm-slow's sale rate is 1/11 and it sells one a day; the
 * realisable profit walks the buy realm's book only as far as realm-slow
 * will sell in a week (7 units: 5 x 2.80 + 2 x 2.60 = 19.20). The floor
 * on units a day moves the sale to realm-fast, which sells all ten the
 * book has at a profit (5 x 1.85 + 5 x 1.65 = 17.50), and leaves out the
 * deals that sell nowhere that fast. What breaks if it regresses: an
 * expiry counted as a sale -- the bias that made every slow realm look
 * liquid -- or a profit counted on units nobody will buy.
 */
static void
test_deals_velocity(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureSeriesStore) store = NULL;
	g_autoptr(VentureSeriesRow) slow = NULL;
	g_autoptr(VentureSeriesRow) fast = NULL;
	g_autoptr(JsonNode) answer = NULL;
	g_autofree gchar *page = NULL;
	JsonArray *rows;
	JsonObject *row;
	JsonObject *totals;

	(void)user_data;
	seed_velocity(fixture);

	/* The store's own estimate first: sold and expired kept apart. */
	store = reader(fixture);
	g_assert_true(venture_series_store_get_current(store, "realm-slow", "9001", &slow, NULL));
	g_assert_true(venture_series_store_get_current(store, "realm-fast", "9001", &fast, NULL));
	g_assert_cmpfloat_with_epsilon(slow->sale_rate, 1.0 / 11.0, 1e-9);
	g_assert_cmpfloat_with_epsilon(slow->sold_per_day, 1.0, 1e-9);
	g_assert_cmpfloat_with_epsilon(fast->sale_rate, 1.0, 1e-9);
	g_assert_cmpfloat_with_epsilon(fast->sold_per_day, 14.0, 1e-9);

	answer = get_json(fixture, "/api/v1/market/deals", 200);
	g_assert_cmpfloat_with_epsilon(json_object_get_double_member(root_of(answer), "horizon_days"), 7.0, 1e-9);
	g_assert_true(JSON_NODE_HOLDS_NULL(json_object_get_member(root_of(answer), "min_sold_per_day")));
	row = deal_row(json_object_get_array_member(root_of(answer), "rows"), "9001", "realm-cheap");
	g_assert_nonnull(row);
	g_assert_cmpstr(sell_venue_of(row), ==, "realm-slow");
	g_assert_cmpfloat_with_epsilon(json_object_get_double_member(row, "sell_sold_per_day"), 1.0, 1e-9);
	g_assert_cmpfloat_with_epsilon(json_object_get_double_member(row, "sell_sale_rate"), 1.0 / 11.0, 1e-9);
	g_assert_cmpint(json_object_get_int_member(row, "expected_sales"), ==, 7);
	g_assert_cmpint(json_object_get_int_member(row, "realisable_units"), ==, 7);
	g_assert_cmpint(json_object_get_int_member(row, "book_units"), ==, 10);
	g_assert_cmpint(money_amount(row, "realisable_profit"), ==, 1920);
	g_assert_cmpint(money_amount(row, "realisable_cost"), ==, 740);
	g_assert_cmpint(money_amount(row, "profit"), ==, 280);
	totals = json_object_get_object_member(root_of(answer), "totals");
	g_assert_cmpint(money_amount(totals, "realisable_profit"), ==, 1920 + 850 + 240);
	g_clear_pointer(&answer, json_node_unref);

	/* A day's horizon: one unit, the cheapest. */
	answer = get_json(fixture, "/api/v1/market/deals?horizon_days=1", 200);
	row = deal_row(json_object_get_array_member(root_of(answer), "rows"), "9001", "realm-cheap");
	g_assert_cmpint(json_object_get_int_member(row, "realisable_units"), ==, 1);
	g_assert_cmpint(money_amount(row, "realisable_profit"), ==, 280);
	g_clear_pointer(&answer, json_node_unref);

	/* At least three a day: only realm-fast qualifies, so the deal is
	 * sold there, and what sells nowhere that fast is not a deal. */
	answer = get_json(fixture, "/api/v1/market/deals?min_sold_per_day=3", 200);
	rows = json_object_get_array_member(root_of(answer), "rows");
	g_assert_cmpuint(json_array_get_length(rows), ==, 1);
	row = deal_row(rows, "9001", "realm-cheap");
	g_assert_nonnull(row);
	g_assert_cmpstr(sell_venue_of(row), ==, "realm-fast");
	g_assert_cmpint(json_object_get_int_member(row, "expected_sales"), ==, 98);
	g_assert_cmpint(json_object_get_int_member(row, "realisable_units"), ==, 10);
	g_assert_cmpint(money_amount(row, "realisable_profit"), ==, 1750);
	g_assert_cmpfloat_with_epsilon(json_object_get_double_member(root_of(answer), "min_sold_per_day"), 3.0,
	                               1e-9);
	g_clear_pointer(&answer, json_node_unref);

	answer = get_json(fixture, "/api/v1/market/deals?min_sold_per_day=1000", 200);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(root_of(answer), "rows")), ==, 0);
	g_clear_pointer(&answer, json_node_unref);

	g_assert_cmpuint(http(fixture, "GET", "/api/v1/market/deals?min_sold_per_day=-1", NULL, NULL, NULL), ==, 400);
	g_assert_cmpuint(http(fixture, "GET", "/api/v1/market/deals?min_sold_per_day=lots", NULL, NULL, NULL), ==, 400);
	g_assert_cmpuint(http(fixture, "GET", "/api/v1/market/deals?horizon_days=0", NULL, NULL, NULL), ==, 400);
	g_assert_cmpuint(http(fixture, "GET", "/api/v1/market/deals?horizon_days=1000", NULL, NULL, NULL), ==, 400);

	page = get_page(fixture, "/market/deals?sort=realisable&min_sold_per_day=3");
	g_assert_nonnull(strstr(page, "Sold a day"));
	g_assert_nonnull(strstr(page, "Realisable"));
	g_assert_nonnull(strstr(page, "name=\"min_sold_per_day\""));
	g_assert_nonnull(strstr(page, "<option value=\"realisable\" selected>"));
	/* A heading's sort link keeps the floor. */
	g_assert_nonnull(strstr(page, "min_sold_per_day=3&amp;sort=profit"));
	assert_buttons_named(page, "/market/deals");
}

/*
 * Sorting by realisable profit is not sorting by profit: a fat margin on
 * one unit (9003: 8.50 a unit, one in the book) loses to a thinner one on
 * seven that will sell (9001: 2.80 a unit, 19.20 in all), and a row with
 * nothing to realise goes last either way. What breaks if it regresses:
 * the top of the page is a one-off nobody can repeat.
 */
static void
test_deals_realisable_order(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(JsonNode) by_profit = NULL;
	g_autoptr(JsonNode) by_realisable = NULL;
	JsonArray *rows;
	gint64 previous = G_MAXINT64;
	guint i;

	(void)user_data;
	seed_velocity(fixture);

	by_profit = get_json(fixture, "/api/v1/market/deals?sort=profit", 200);
	rows = json_object_get_array_member(root_of(by_profit), "rows");
	g_assert_cmpuint(json_array_get_length(rows), ==, 3);
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(rows, 0), "instrument_key"), ==,
	                "9003");

	by_realisable = get_json(fixture, "/api/v1/market/deals?sort=realisable", 200);
	rows = json_object_get_array_member(root_of(by_realisable), "rows");
	g_assert_cmpuint(json_array_get_length(rows), ==, 3);
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(rows, 0), "instrument_key"), ==,
	                "9001");
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(rows, 0), "venue_key"), ==,
	                "realm-cheap");

	for (i = 0; i < json_array_get_length(rows); i++)
	{
		gint64 amount = money_amount(json_array_get_object_element(rows, i), "realisable_profit");

		g_assert_cmpint(amount, <=, previous);
		previous = amount;
	}

	g_assert_cmpint(money_amount(json_array_get_object_element(rows, 1), "realisable_profit"), ==, 850);
	g_assert_cmpint(money_amount(json_array_get_object_element(rows, 2), "realisable_profit"), ==, 240);
}

/* Figures a source computed itself, as tsmctl pushes TSM's. */
static void
stats_snapshot(
	VentureSeriesStore	*store,
	const gchar		*venue,
	gint64			 taken_at,
	const gchar		*key,
	gint64			 market,
	gint64			 historical,
	gdouble			 sale_rate,
	gdouble			 sold_per_day
){
	g_autoptr(GError) error = NULL;
	VentureSeriesSnapshot *snap;
	VentureSeriesCommitResult result;
	VentureSeriesStats stats;

	venture_series_stats_init(&stats);
	stats.instrument_key = key;
	stats.market_value = market;
	stats.historical = historical;
	stats.sale_rate = sale_rate;
	stats.sold_per_day = sold_per_day;
	snap = venture_series_store_begin_snapshot(store, venue, "USD", taken_at, taken_at + 30, FALSE, &error);
	g_assert_no_error(error);
	g_assert_true(venture_series_snapshot_add_stats(snap, &stats, &error));
	g_assert_no_error(error);
	g_assert_true(venture_series_store_commit_snapshot(store, snap, &result, &error));
	g_assert_no_error(error);
}

/* A second source of the organization, its store open for writing. */
static VentureSeriesStore *
second_source(
	Fixture		*fixture,
	const gchar	*name,
	const gchar	*instrument_namespace
){
	g_autoptr(VentureDataSource) source = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *dir = NULL;
	VentureSeriesStore *store;

	source = venture_data_source_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(source), fixture->org);
	g_object_set(source, "name", name, "provider", "file_jsonl", "settings", "file: t.jsonl",
	             "schedule", "manual", "currency", "USD", "instrument-namespace", instrument_namespace,
	             "venue-namespace", "realm", NULL);
	save(fixture, source);
	dir = venture_feeds_store_dir(fixture->config, venture_entity_get_uuid(VENTURE_ENTITY(source)));
	store = venture_series_store_open(dir, &error);
	g_assert_no_error(error);

	return store;
}

/*
 * Another source's view, joined on the item key only within one
 * instrument namespace: "Pushes" (wow-item, TSM's way) keeps the EU
 * region at 2.00 for 9001 and realm-cheap at 1.50, so the deal there
 * reads 50% under the region and a third under its realm; "Elsewhere"
 * keeps a region figure for 9003 under another namespace, which must not
 * be read as the same item. Absent is absent: no ref, never a zero. The
 * instrument page carries the same object. What breaks if it regresses:
 * a TSM value from a different item shown as this one's, or "0.00"
 * where TSM has nothing to say.
 */
static void
test_deals_reference_source(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureSeriesStore) pushes = NULL;
	g_autoptr(VentureSeriesStore) elsewhere = NULL;
	g_autoptr(JsonNode) answer = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *page = NULL;
	gint64 t = g_get_real_time() / G_USEC_PER_SEC - 600;
	JsonArray *rows;
	JsonObject *row;
	JsonObject *ref;

	(void)user_data;
	seed_velocity(fixture);

	pushes = second_source(fixture, "Pushes", "wow-item");
	add_venue(pushes, "region-eu", "eu", "USD", t);
	add_venue(pushes, "realm-cheap", "eu", "USD", t);
	add_instrument(pushes, "9001", "Velvet Thread", "Trade Goods", t);
	stats_snapshot(pushes, "region-eu", t, "9001", 200, 210, 0.5, 12.0);
	stats_snapshot(pushes, "realm-cheap", t, "9001", 150, VENTURE_SERIES_NONE, NAN, NAN);

	elsewhere = second_source(fixture, "Elsewhere", "other-item");
	add_venue(elsewhere, "region-eu", "eu", "USD", t);
	add_instrument(elsewhere, "9003", "Not a clasp", "Things", t);
	stats_snapshot(elsewhere, "region-eu", t, "9003", 777, VENTURE_SERIES_NONE, NAN, NAN);

	answer = get_json(fixture, "/api/v1/market/deals", 200);
	rows = json_object_get_array_member(root_of(answer), "rows");
	row = deal_row(rows, "9001", "realm-cheap");
	g_assert_nonnull(row);
	g_assert_true(json_object_has_member(row, "ref"));
	ref = json_object_get_object_member(row, "ref");
	g_assert_cmpstr(json_object_get_string_member(ref, "source_name"), ==, "Pushes");
	g_assert_cmpstr(json_object_get_string_member(ref, "region_venue"), ==, "region-eu");
	g_assert_cmpint(money_amount(ref, "region_market"), ==, 200);
	g_assert_cmpint(money_amount(ref, "region_historical"), ==, 210);
	g_assert_cmpfloat_with_epsilon(json_object_get_double_member(ref, "region_sold_per_day"), 12.0, 1e-9);
	g_assert_cmpfloat_with_epsilon(json_object_get_double_member(ref, "region_sale_rate"), 0.5, 1e-9);
	g_assert_cmpfloat_with_epsilon(json_object_get_double_member(ref, "buy_vs_region_pct"), -50.0, 1e-9);
	/* Sold at realm-slow for 4.00: double the region's 2.00. */
	g_assert_cmpfloat_with_epsilon(json_object_get_double_member(ref, "sell_vs_region_pct"), 100.0, 1e-9);
	g_assert_cmpint(money_amount(ref, "buy_realm_market"), ==, 150);
	g_assert_cmpfloat_with_epsilon(json_object_get_double_member(ref, "buy_vs_realm_pct"), 100.0 * (100 - 150) / 150.0,
	                               1e-9);
	/* Pushes keeps no realm-slow: null, not zero. */
	g_assert_true(JSON_NODE_HOLDS_NULL(json_object_get_member(ref, "sell_realm_market")));

	/* 9003: only another namespace's key of the same spelling. */
	row = deal_row(rows, "9003", "realm-cheap");
	g_assert_nonnull(row);
	g_assert_false(json_object_has_member(row, "ref"));
	g_clear_pointer(&answer, json_node_unref);

	path = g_strdup_printf("/api/v1/market/i/%" G_GINT64_FORMAT "/9001?venue=realm-cheap", fixture->source_id);
	answer = get_json(fixture, path, 200);
	ref = json_object_get_object_member(root_of(answer), "ref");
	g_assert_nonnull(ref);
	g_assert_cmpint(money_amount(ref, "region_market"), ==, 200);
	g_assert_cmpint(money_amount(ref, "buy_realm_market"), ==, 150);
	g_clear_pointer(&answer, json_node_unref);
	g_clear_pointer(&path, g_free);

	path = g_strdup_printf("/api/v1/market/i/%" G_GINT64_FORMAT "/9003", fixture->source_id);
	answer = get_json(fixture, path, 200);
	g_assert_false(json_object_has_member(root_of(answer), "ref"));
	g_clear_pointer(&path, g_free);

	page = get_page(fixture, "/market/deals");
	g_assert_nonnull(strstr(page, "Ref. value"));
	g_assert_nonnull(strstr(page, "\xe2\x88\x92" "50%"));
	g_clear_pointer(&page, g_free);

	path = g_strdup_printf("/market/i/%" G_GINT64_FORMAT "/9001?venue=realm-cheap", fixture->source_id);
	page = get_page(fixture, path);
	g_assert_nonnull(strstr(page, "From Pushes"));
	g_assert_nonnull(strstr(page, "Region market value"));
	g_clear_pointer(&page, g_free);
	g_clear_pointer(&path, g_free);

	path = g_strdup_printf("/market/i/%" G_GINT64_FORMAT "/9003", fixture->source_id);
	page = get_page(fixture, path);
	g_assert_null(strstr(page, "Region market value"));
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

	/* Watch: promotes, puts it straight on the list, and comes back to
	 * the page saying so -- no detour through the generic entry form. */
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/market/i/%" G_GINT64_FORMAT "/2770", fixture->source_id);
	{
		g_autofree gchar *form = g_strdup_printf("action=watch&watchlist_id=%" G_GINT64_FORMAT, ID(list));

		g_assert_cmpuint(http(fixture, "POST", path, form, NULL, &location), ==, 302);
	}
	expected = g_strdup_printf("/market/i/%" G_GINT64_FORMAT "/2770?list_added=%" G_GINT64_FORMAT,
	                           fixture->source_id, ID(list));
	g_assert_cmpstr(location, ==, expected);
	page = get_page(fixture, location);
	g_assert_nonnull(strstr(page, "<div class=\"notice positive\" role=\"status\">Added to <strong>"));
	g_assert_nonnull(strstr(page, "class=\"list-tag\""));
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
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_WATCHLIST_ENTRY);
		g_autoptr(GPtrArray) found = NULL;

		g_assert_true(venture_query_add_filter_int(query, "watchlist-id", VENTURE_FILTER_OP_EQ, ID(list), NULL));
		found = venture_database_find(fixture->database, query, NULL);
		g_assert_cmpuint(found->len, ==, 1);
		entry = g_object_ref(g_ptr_array_index(found, 0));
	}
	g_object_set(entry, "target-buy", buy, NULL);
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
		".chart-line-1", ".chart-line-2", ".chart-line-3", ".chart-line-4", ".chart-key-3", ".chart-bridge",
		".chart-key-4", ".chart-heat-cell", ".chart-grid", ".chart-axis", ".sparkline",
		".market-action-row", ".form-inline", ".chart-legend", ".chart-caption", ".market-ranges",
		".market-range", ".market-trend", ".source-attribution", ".source-attribution-label", NULL
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

/*
 * Venue groups, the item finder, variants. A fourth venue named for two
 * realms holds a variant of copper ore whose name the store learns from
 * its plain item; a saved group names one venue by key and one by a part
 * of its name.
 */
static void
test_venue_groups_and_find(
	Fixture		*fixture,
	gconstpointer	 data
){
	g_autoptr(VentureEntity) source = NULL;
	g_autoptr(VentureSeriesStore) store = NULL;
	g_autoptr(VentureEntity) group = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *dir = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *page = NULL;
	VentureSeriesInstrument variant;
	VentureSeriesVenue venue;
	JsonObject *root;
	JsonArray *list;
	JsonObject *item;
	gint64 now;
	guint i;
	gboolean seen_variant = FALSE;
	static const Offer d2[] = { { "2770:b1472", 30, 150, 1 } };

	(void)data;
	seed_store(fixture);

	source = venture_database_get(fixture->database, VENTURE_TYPE_DATA_SOURCE, fixture->source_id, NULL);
	dir = venture_feeds_store_dir(fixture->config, venture_entity_get_uuid(source));
	store = venture_series_store_open(dir, &error);
	g_assert_no_error(error);
	now = g_get_real_time() / G_USEC_PER_SEC;

	memset(&venue, 0, sizeof(venue));
	venue.key = "3676";
	venue.namespace_ = "realm";
	venue.name = "Thrall, Area 52";
	venue.kind = "auction_house";
	venue.group_key = "eu";
	venue.currency = "USD";
	/* A provider's link template, one naming a value no instrument has,
	 * and one that is not a web address: only the first is ever shown. */
	venue.attrs_json = "{\"links\":["
		"{\"label\":\"Undermine Exchange\",\"url\":\"https://undermine.exchange/#us-thrall/{undermine_item}\"},"
		"{\"label\":\"Nowhere\",\"url\":\"https://example.org/{missing}\"},"
		"{\"label\":\"Script\",\"url\":\"javascript:alert(1)\"}]}";
	g_assert_true(venture_series_store_upsert_venue(store, &venue, now - 3600, &error));
	g_assert_no_error(error);

	/* A variant stored with no name of its own takes its plain item's. */
	memset(&variant, 0, sizeof(variant));
	variant.key = "2770:b1472";
	variant.namespace_ = "wow-item";
	variant.kind = "item";
	variant.parent_key = "2770";
	variant.attrs_json = "{\"undermine_item\":\"2770 x\",\"links\":[{\"label\":\"Wowhead\","
		"\"url\":\"https://www.wowhead.com/item=2770?bonus=1472\"}],"
		"\"display\":{\"color\":\"#a335ee\",\"icon\":\"/blizzard/icons/1.jpg\",\"lines\":["
		"{\"text\":\"Item Level 9\",\"color\":\"#ffd100\"},"
		"{\"text\":\"<b>bold</b>\",\"right\":\"Sword\",\"color\":\"red;background:url(x)\"}]}}";
	g_assert_true(venture_series_store_upsert_instrument(store, &variant, now - 3600, NULL, NULL, &error));
	g_assert_no_error(error);
	snapshot(store, "3676", "USD", now - 3600, d2, G_N_ELEMENTS(d2));
	g_clear_object(&store);

	group = VENTURE_ENTITY(venture_venue_group_new());
	venture_entity_set_organization_id(group, fixture->org);
	g_object_set(group, "name", "Bank realms", "venues", " realm-a ,area 52,", NULL);
	save(fixture, group);

	/* Browse in the group: realm-a's and the "Area 52" venue's rows only. */
	path = g_strdup_printf("/api/v1/market/browse?search=copper%%20ore&venue_group=%" G_GINT64_FORMAT,
	                       ID(group));
	node = get_json(fixture, path, 200);
	root = json_node_get_object(node);
	g_assert_cmpstr(json_object_get_string_member(root, "venue_group_name"), ==, "Bank realms");
	list = json_object_get_array_member(root, "rows");
	g_assert_cmpuint(json_array_get_length(list), ==, 2);

	for (i = 0; i < json_array_get_length(list); i++)
	{
		JsonObject *row = json_array_get_object_element(list, i);
		const gchar *venue_key = json_object_get_string_member(row, "venue_key");

		g_assert_true(0 == g_strcmp0(venue_key, "realm-a") || 0 == g_strcmp0(venue_key, "3676"));
		g_assert_cmpstr(json_object_get_string_member(row, "instrument_name"), ==, "Copper Ore");

		if (0 == g_strcmp0(venue_key, "3676"))
		{
			g_assert_cmpstr(json_object_get_string_member(row, "variant"), ==, "bonus 1472");
			seen_variant = TRUE;
		}
	}

	g_assert_true(seen_variant);
	g_clear_pointer(&node, json_node_unref);
	g_clear_pointer(&path, g_free);

	/* The picker offers the saved group; "My characters" needs a push
	 * source, which this organization has not got. */
	node = get_json(fixture, "/api/v1/market/browse", 200);
	list = json_object_get_array_member(json_node_get_object(node), "venue_groups");
	g_assert_cmpuint(json_array_get_length(list), ==, 1);
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(list, 0), "name"), ==,
	                "Bank realms");
	g_clear_pointer(&node, json_node_unref);

	/* "My characters" with no characters is an empty group: nothing, and
	 * a note saying why -- never every venue. */
	node = get_json(fixture, "/api/v1/market/browse?venue_group=characters", 200);
	root = json_node_get_object(node);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(root, "rows")), ==, 0);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(root, "notes")), >, 0);
	g_clear_pointer(&node, json_node_unref);

	/* Find: the plain item and its variant on their own rows. Everywhere,
	 * copper ore is cheapest at realm-c (100) and dearest at realm-a (120). */
	node = get_json(fixture, "/api/v1/market/find?search=COPPER%20ore", 200);
	list = json_object_get_array_member(json_node_get_object(node), "items");
	g_assert_cmpuint(json_array_get_length(list), ==, 2);

	for (i = 0; i < json_array_get_length(list); i++)
	{
		item = json_array_get_object_element(list, i);

		if (0 == g_strcmp0(json_object_get_string_member(item, "key"), "2770"))
		{
			g_assert_cmpint(json_object_get_int_member(item, "venues"), ==, 3);
			g_assert_cmpstr(json_object_get_string_member(json_object_get_object_member(item, "cheapest"),
			                                              "venue_key"), ==, "realm-c");
			g_assert_cmpstr(json_object_get_string_member(json_object_get_object_member(item, "dearest"),
			                                              "venue_key"), ==, "realm-a");
		}
		else
		{
			g_assert_cmpstr(json_object_get_string_member(item, "key"), ==, "2770:b1472");
			g_assert_cmpstr(json_object_get_string_member(item, "variant"), ==, "bonus 1472");
		}
	}

	g_clear_pointer(&node, json_node_unref);

	/* In the group the plain item is offered at realm-a alone. */
	path = g_strdup_printf("/api/v1/market/find?search=copper%%20ore&venue_group=%" G_GINT64_FORMAT,
	                       ID(group));
	node = get_json(fixture, path, 200);
	list = json_object_get_array_member(json_node_get_object(node), "items");

	for (i = 0; i < json_array_get_length(list); i++)
	{
		item = json_array_get_object_element(list, i);

		if (0 == g_strcmp0(json_object_get_string_member(item, "key"), "2770"))
			g_assert_cmpint(json_object_get_int_member(item, "venues"), ==, 1);
	}

	g_clear_pointer(&node, json_node_unref);
	g_clear_pointer(&path, g_free);

	/* The instrument's venue table, dearest first, unpriced last. */
	path = g_strdup_printf("/api/v1/market/i/%" G_GINT64_FORMAT "/2770?venues_dir=desc", fixture->source_id);
	node = get_json(fixture, path, 200);
	list = json_object_get_array_member(json_node_get_object(node), "venues");
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(list, 0), "venue_key"), ==,
	                "realm-a");
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(list, 2), "venue_key"), ==,
	                "realm-c");
	g_clear_pointer(&node, json_node_unref);
	g_clear_pointer(&path, g_free);

	/* Deals take the group and a search like browse does. */
	path = g_strdup_printf("/api/v1/market/deals?search=peacebloom&venue_group=%" G_GINT64_FORMAT, ID(group));
	node = get_json(fixture, path, 200);
	list = json_object_get_array_member(json_node_get_object(node), "rows");

	for (i = 0; i < json_array_get_length(list); i++)
	{
		JsonObject *row = json_array_get_object_element(list, i);

		g_assert_cmpstr(json_object_get_string_member(row, "instrument_key"), ==, "2447");
		g_assert_cmpstr(json_object_get_string_member(row, "venue_key"), !=, "realm-b");
	}

	g_clear_pointer(&node, json_node_unref);
	g_clear_pointer(&path, g_free);

	/* Links: the item's own, then the charted venue's template filled in
	 * from the item's attributes (escaped); the incomplete and the
	 * non-web links are left out, on the page and in each venue row. */
	path = g_strdup_printf("/api/v1/market/i/%" G_GINT64_FORMAT "/2770%%3Ab1472", fixture->source_id);
	node = get_json(fixture, path, 200);
	root = json_node_get_object(node);
	list = json_object_get_array_member(root, "links");
	g_assert_cmpuint(json_array_get_length(list), ==, 2);
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(list, 0), "url"), ==,
	                "https://www.wowhead.com/item=2770?bonus=1472");
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(list, 1), "url"), ==,
	                "https://undermine.exchange/#us-thrall/2770%20x");
	list = json_object_get_array_member(root, "venues");

	for (i = 0; i < json_array_get_length(list); i++)
	{
		JsonObject *row = json_array_get_object_element(list, i);
		guint expected = (0 == g_strcmp0(json_object_get_string_member(row, "venue_key"), "3676")) ? 1 : 0;

		g_assert_cmpuint(json_array_get_length(json_object_get_array_member(row, "links")), ==, expected);
	}
	g_clear_pointer(&node, json_node_unref);
	g_clear_pointer(&path, g_free);

	path = g_strdup_printf("/market/i/%" G_GINT64_FORMAT "/2770%%3Ab1472", fixture->source_id);
	page = get_page(fixture, path);
	g_assert_nonnull(strstr(page, "View on Wowhead"));

	/* The display: drawn in its colours, its text escaped, and a colour
	 * that is not #rrggbb never reaching a style attribute. */
	g_assert_nonnull(strstr(page, "<span style=\"color:#a335ee\">Copper Ore</span>"));
	g_assert_nonnull(strstr(page, "src=\"/blizzard/icons/1.jpg\""));
	g_assert_nonnull(strstr(page, "item-tip-card is-static"));
	g_assert_nonnull(strstr(page, "&lt;b&gt;bold&lt;/b&gt;"));
	g_assert_null(strstr(page, "background:url"));
	g_assert_nonnull(strstr(page, "rel=\"noopener noreferrer\""));
	g_assert_null(strstr(page, "javascript:"));
	g_assert_null(strstr(page, "Nowhere"));
	g_clear_pointer(&page, g_free);
	g_clear_pointer(&path, g_free);

	/* Refusals: an empty search, a group spec that is neither, another
	 * organization's group (or none at all). */
	g_assert_cmpuint(http(fixture, "GET", "/api/v1/market/find", NULL, NULL, NULL), ==, 400);
	g_assert_cmpuint(http(fixture, "GET", "/api/v1/market/browse?venue_group=nope", NULL, NULL, NULL), ==, 400);
	g_assert_cmpuint(http(fixture, "GET", "/api/v1/market/browse?venue_group=999999", NULL, NULL, NULL), ==, 404);

	/* The pages draw: the picker, the finder's table, the variant label. */
	page = get_page(fixture, "/market/find?search=copper");
	g_assert_nonnull(strstr(page, "name=\"venue_group\""));
	g_assert_nonnull(strstr(page, "Bank realms"));
	g_assert_nonnull(strstr(page, "(bonus 1472)"));
	g_assert_nonnull(strstr(page, "class=\"item-tip\""));
	g_assert_nonnull(strstr(page, "<span class=\"item-name\" style=\"color:#a335ee\">Copper Ore</span>"));
	g_clear_pointer(&page, g_free);
	page = get_page(fixture, "/market/find");
	g_assert_nonnull(strstr(page, "name=\"search\""));
}

/*
 * Categories: the store lists the paths its instruments are filed under,
 * browse, deals and find answer them, and the pages draw them as an
 * auction house does -- a group per first part with an "All" entry -- in
 * place of a text box. Find takes a category with no search.
 */
static void
test_categories(
	Fixture		*fixture,
	gconstpointer	 data
){
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *page = NULL;
	JsonArray *list;
	guint i;
	gboolean ore = FALSE;
	gboolean herbs = FALSE;

	(void)data;
	seed_store(fixture);

	node = get_json(fixture, "/api/v1/market/browse", 200);
	list = json_object_get_array_member(json_node_get_object(node), "categories");

	for (i = 0; i < json_array_get_length(list); i++)
	{
		JsonObject *category = json_array_get_object_element(list, i);
		const gchar *path = json_object_get_string_member(category, "path");

		if (0 == g_strcmp0(path, "Materials/Ore"))
		{
			ore = TRUE;
			g_assert_cmpint(json_object_get_int_member(category, "instruments"), ==, 1);
		}

		herbs = herbs || (0 == g_strcmp0(path, "Herbs"));
	}

	g_assert_true(ore);
	g_assert_true(herbs);
	g_clear_pointer(&node, json_node_unref);

	node = get_json(fixture, "/api/v1/market/deals", 200);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(node),
	                                                                    "categories")), >, 0);
	g_clear_pointer(&node, json_node_unref);

	/* Find by category alone: everything under Materials, nothing else. */
	node = get_json(fixture, "/api/v1/market/find?category=Materials", 200);
	list = json_object_get_array_member(json_node_get_object(node), "items");
	g_assert_cmpuint(json_array_get_length(list), >, 0);

	for (i = 0; i < json_array_get_length(list); i++)
		g_assert_true(g_str_has_prefix(json_object_get_string_member(json_array_get_object_element(list, i),
		                                                             "category"), "Materials/"));
	g_clear_pointer(&node, json_node_unref);

	/* The picker: grouped, "All" first, the subclass by its last part,
	 * the asked-for one selected, and escaped like everything else. */
	page = get_page(fixture, "/market/browse?category=Materials%2FOre");
	g_assert_nonnull(strstr(page, "<select name=\"category\">"));
	g_assert_nonnull(strstr(page, "<optgroup label=\"Materials\"><option value=\"Materials\">All Materials</option>"));
	g_assert_nonnull(strstr(page, "<option value=\"Materials/Ore\" selected>Ore</option>"));
	g_assert_nonnull(strstr(page, "<option value=\"Herbs\">All Herbs</option>"));
	g_assert_null(strstr(page, "Materials/<b>"));
	g_clear_pointer(&page, g_free);

	page = get_page(fixture, "/market/find");
	g_assert_nonnull(strstr(page, "All Materials"));
	g_clear_pointer(&page, g_free);

	/* A category not in the list stays selected rather than vanishing. */
	page = get_page(fixture, "/market/deals?category=Nowhere%2FElse");
	g_assert_nonnull(strstr(page, "<option value=\"Nowhere/Else\" selected>"));
}

/*
 * A store an older build wrote is upgraded when a page reads it. Only a
 * writer migrates; a push source's store is written only when somebody
 * pushes, so without this every page over it said "nothing sent yet" from
 * the upgrade until the next push.
 */
static void
test_reader_upgrades_store(
	Fixture		*fixture,
	gconstpointer	 data
){
	g_autoptr(VentureEntity) source = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *dir = NULL;
	g_autofree gchar *path = NULL;
	sqlite3 *db;
	sqlite3_stmt *stmt;
	gint version;

	(void)data;
	seed_store(fixture);

	source = venture_database_get(fixture->database, VENTURE_TYPE_DATA_SOURCE, fixture->source_id, NULL);
	dir = venture_feeds_store_dir(fixture->config, venture_entity_get_uuid(source));
	path = g_build_filename(dir, "store.db", NULL);

	/* Step 8 only backfills names, so a store at 7 is exactly what the
	 * build before it left. */
	g_assert_cmpint(sqlite3_open(path, &db), ==, SQLITE_OK);
	g_assert_cmpint(sqlite3_exec(db, "PRAGMA user_version = 7;", NULL, NULL, NULL), ==, SQLITE_OK);
	sqlite3_close(db);

	node = get_json(fixture, "/api/v1/market/browse", 200);
	g_assert_true(json_object_get_boolean_member(json_node_get_object(node), "available"));

	g_assert_cmpint(sqlite3_open(path, &db), ==, SQLITE_OK);
	g_assert_cmpint(sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &stmt, NULL), ==, SQLITE_OK);
	g_assert_cmpint(sqlite3_step(stmt), ==, SQLITE_ROW);
	version = sqlite3_column_int(stmt, 0);
	sqlite3_finalize(stmt);
	sqlite3_close(db);
	g_assert_cmpint(version, ==, (gint)venture_series_store_schema_version());
}

/* --- Price history and the last seven days ------------------------------------ */

/* An answer's ISO time as Unix seconds. */
static gint64
iso_unix(
	JsonObject	*object,
	const gchar	*member
){
	g_autoptr(GDateTime) moment = NULL;

	moment = g_date_time_new_from_iso8601(json_object_get_string_member(object, member), NULL);
	g_assert_nonnull(moment);

	return g_date_time_to_unix(moment);
}

/* The point of @points at @at, or NULL. */
static JsonObject *
point_at(
	JsonArray	*points,
	gint64		 at
){
	guint i;

	for (i = 0; i < json_array_get_length(points); i++)
		if (iso_unix(json_array_get_object_element(points, i), "at") == at)
			return json_array_get_object_element(points, i);

	return NULL;
}

/*
 * Silk cloth, hour by hour: at realm-x 1000 for 29 hours (one missing)
 * and 700 in the current hour -- a dip; at realm-y 1000; at realm-z, in
 * another group, 50 once. Returns the start of the current hour, the
 * newest snapshot's time.
 */
static gint64
seed_history(Fixture *fixture)
{
	g_autoptr(VentureEntity) source = NULL;
	g_autoptr(VentureSeriesStore) store = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *dir = NULL;
	gint64 now;
	gint64 base;
	gint k;

	source = venture_database_get(fixture->database, VENTURE_TYPE_DATA_SOURCE, fixture->source_id, NULL);
	dir = venture_feeds_store_dir(fixture->config, venture_entity_get_uuid(source));
	store = venture_series_store_open(dir, &error);
	g_assert_no_error(error);

	now = g_get_real_time() / G_USEC_PER_SEC;
	base = now - now % 3600;

	add_venue(store, "realm-x", "eu", "USD", base - 30 * 3600);
	add_venue(store, "realm-y", "eu", "USD", base - 30 * 3600);
	add_venue(store, "realm-z", "us", "USD", base - 30 * 3600);
	add_instrument(store, "4306", "Silk Cloth", "Tradeskill/Cloth", base - 30 * 3600);

	for (k = 29; k >= 0; k--)
	{
		Offer offer;

		if (5 == k)
			continue;

		offer.instrument = "4306";
		offer.id = 100 + (guint64)k;
		offer.price = (0 == k) ? 700 : 1000;
		offer.quantity = (0 == k) ? 3 : 10;
		snapshot(store, "realm-x", "USD", base - (gint64)k * 3600, &offer, 1);

		if (k <= 1)
		{
			offer.id = 200 + (guint64)k;
			offer.price = 1000;
			offer.quantity = 8;
			snapshot(store, "realm-y", "USD", base - (gint64)k * 3600, &offer, 1);
		}
	}

	{
		Offer offer = { "4306", 300, 50, 1 };

		snapshot(store, "realm-z", "USD", base, &offer, 1);
	}

	g_assert_true(venture_series_store_recompute_region(store, NULL, now, VENTURE_SERIES_NONE, NULL,
	                                                    NULL, &error));
	g_assert_no_error(error);

	return base;
}

/*
 * The price history: one slot an hour within the hourly window, one a
 * day beyond it (and beyond a shorter window when series.hourly_days is
 * shorter), a missing hour a gap where it fell, and a second line on the
 * same slots -- the region's median as it stood then, or another venue.
 * The instrument page draws it with its ranges and comparison; the API
 * route answers it alone; bad ranges and comparisons are refused.
 *
 * What breaks if this regresses: the chart that tells a dip from the new
 * normal joins an outage's neighbours, draws hours and days on one line,
 * or compares a venue with a region it is not in.
 */
static void
test_price_history(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(JsonNode) answer = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *page = NULL;
	JsonObject *history;
	JsonObject *compare;
	JsonObject *point;
	JsonArray *points;
	gint64 base;

	(void)user_data;

	base = seed_history(fixture);

	path = g_strdup_printf("/api/v1/market/history/%" G_GINT64_FORMAT "/4306?venue=realm-x&range=24h",
	                       fixture->source_id);
	answer = get_json(fixture, path, 200);
	g_assert_cmpstr(json_object_get_string_member(root_of(answer), "venue"), ==, "realm-x");
	g_assert_cmpstr(json_object_get_string_member(root_of(answer), "name"), ==, "Silk Cloth");
	history = json_object_get_object_member(root_of(answer), "history");
	g_assert_cmpstr(json_object_get_string_member(history, "range"), ==, "24h");
	g_assert_cmpstr(json_object_get_string_member(history, "resolution"), ==, "hour");
	g_assert_cmpstr(json_object_get_string_member(history, "currency"), ==, "USD");
	g_assert_false(json_object_get_boolean_member(history, "truncated"));
	g_assert_true(json_object_get_null_member(history, "compare"));
	points = json_object_get_array_member(history, "points");
	g_assert_cmpuint(json_array_get_length(points), ==, 24);
	g_assert_cmpint(iso_unix(history, "until"), ==, base);
	g_assert_cmpint(iso_unix(history, "since"), ==, base - 23 * 3600);

	point = point_at(points, base);
	g_assert_cmpint(json_object_get_int_member(point, "min_price"), ==, 700);
	g_assert_cmpint(json_object_get_int_member(point, "quantity"), ==, 3);
	point = point_at(points, base - 3600);
	g_assert_cmpint(json_object_get_int_member(point, "min_price"), ==, 1000);

	/* The missing hour is a gap where it fell. */
	point = point_at(points, base - 5 * 3600);
	g_assert_true(json_object_get_null_member(point, "min_price"));
	g_assert_true(json_object_get_null_member(point, "quantity"));
	g_clear_pointer(&answer, json_node_unref);
	g_clear_pointer(&path, g_free);

	/* The region: realm-x and realm-y, never realm-z's 50 from the US. */
	path = g_strdup_printf("/api/v1/market/history/%" G_GINT64_FORMAT "/4306?venue=realm-x&range=24h"
	                       "&compare=region", fixture->source_id);
	answer = get_json(fixture, path, 200);
	compare = json_object_get_object_member(json_object_get_object_member(root_of(answer), "history"),
	                                        "compare");
	g_assert_cmpstr(json_object_get_string_member(compare, "kind"), ==, "region");
	g_assert_cmpstr(json_object_get_string_member(compare, "group_key"), ==, "eu");
	points = json_object_get_array_member(compare, "points");
	g_assert_cmpuint(json_array_get_length(points), ==, 24);
	point = point_at(points, base);
	g_assert_cmpint(json_object_get_int_member(point, "min_price"), ==, 850);
	g_assert_cmpint(json_object_get_int_member(point, "venues"), ==, 2);
	point = point_at(points, base - 2 * 3600);
	g_assert_cmpint(json_object_get_int_member(point, "min_price"), ==, 1000);
	g_assert_cmpint(json_object_get_int_member(point, "venues"), ==, 1);
	g_clear_pointer(&answer, json_node_unref);
	g_clear_pointer(&path, g_free);

	/* Another venue, over the default range: the hourly window. */
	path = g_strdup_printf("/api/v1/market/history/%" G_GINT64_FORMAT "/4306?venue=realm-x&compare=realm-y",
	                       fixture->source_id);
	answer = get_json(fixture, path, 200);
	history = json_object_get_object_member(root_of(answer), "history");
	g_assert_cmpstr(json_object_get_string_member(history, "range"), ==, "14d");
	g_assert_cmpstr(json_object_get_string_member(history, "resolution"), ==, "hour");
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(history, "points")), ==, 14 * 24);
	compare = json_object_get_object_member(history, "compare");
	g_assert_cmpstr(json_object_get_string_member(compare, "kind"), ==, "venue");
	g_assert_cmpstr(json_object_get_string_member(compare, "venue"), ==, "realm-y");
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(compare, "points")), ==, 14 * 24);
	g_clear_pointer(&answer, json_node_unref);
	g_clear_pointer(&path, g_free);

	/* Beyond the hourly window: by the day, the day's lowest. */
	path = g_strdup_printf("/api/v1/market/history/%" G_GINT64_FORMAT "/4306?venue=realm-x&range=90d",
	                       fixture->source_id);
	answer = get_json(fixture, path, 200);
	history = json_object_get_object_member(root_of(answer), "history");
	g_assert_cmpstr(json_object_get_string_member(history, "resolution"), ==, "day");
	points = json_object_get_array_member(history, "points");
	g_assert_cmpuint(json_array_get_length(points), ==, 90);
	point = point_at(points, base - base % 86400);
	g_assert_cmpint(json_object_get_int_member(point, "min_price"), ==, 700);
	g_clear_pointer(&answer, json_node_unref);
	g_clear_pointer(&path, g_free);

	/* Everything: from the first day stored. */
	path = g_strdup_printf("/api/v1/market/history/%" G_GINT64_FORMAT "/4306?venue=realm-x&range=all",
	                       fixture->source_id);
	answer = get_json(fixture, path, 200);
	history = json_object_get_object_member(root_of(answer), "history");
	g_assert_cmpstr(json_object_get_string_member(history, "resolution"), ==, "day");
	g_assert_cmpint(iso_unix(history, "since"), ==, (base - 29 * 3600) - (base - 29 * 3600) % 86400);
	g_clear_pointer(&answer, json_node_unref);
	g_clear_pointer(&path, g_free);

	/* With no venue named, the page's own choice: the cheapest in stock. */
	path = g_strdup_printf("/api/v1/market/history/%" G_GINT64_FORMAT "/4306", fixture->source_id);
	answer = get_json(fixture, path, 200);
	g_assert_cmpstr(json_object_get_string_member(root_of(answer), "venue"), ==, "realm-z");
	g_clear_pointer(&answer, json_node_unref);
	g_clear_pointer(&path, g_free);

	/* A shorter hourly window moves the boundary: 14 days by the day. */
	g_object_set(fixture->config, "series-hourly-days", (gint64)7, NULL);
	path = g_strdup_printf("/api/v1/market/history/%" G_GINT64_FORMAT "/4306?venue=realm-x&range=14d",
	                       fixture->source_id);
	answer = get_json(fixture, path, 200);
	history = json_object_get_object_member(root_of(answer), "history");
	g_assert_cmpstr(json_object_get_string_member(history, "resolution"), ==, "day");
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(history, "points")), ==, 14);
	g_clear_pointer(&answer, json_node_unref);
	g_clear_pointer(&path, g_free);
	g_object_set(fixture->config, "series-hourly-days", (gint64)14, NULL);

	/* The instrument's answer carries the same history. */
	path = g_strdup_printf("/api/v1/market/i/%" G_GINT64_FORMAT "/4306?venue=realm-x&range=7d",
	                       fixture->source_id);
	answer = get_json(fixture, path, 200);
	history = json_object_get_object_member(root_of(answer), "history");
	g_assert_cmpstr(json_object_get_string_member(history, "range"), ==, "7d");
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(history, "points")), ==, 7 * 24);
	g_clear_pointer(&answer, json_node_unref);
	g_clear_pointer(&path, g_free);

	/* Refusals: a range that is not one, a venue that never listed it, an
	 * instrument nobody knows -- on both doors. */
	path = g_strdup_printf("/api/v1/market/history/%" G_GINT64_FORMAT "/4306?range=3w", fixture->source_id);
	g_assert_cmpuint(http(fixture, "GET", path, NULL, NULL, NULL), ==, 400);
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/api/v1/market/i/%" G_GINT64_FORMAT "/4306?range=3w", fixture->source_id);
	g_assert_cmpuint(http(fixture, "GET", path, NULL, NULL, NULL), ==, 400);
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/api/v1/market/history/%" G_GINT64_FORMAT "/4306?compare=realm-q",
	                       fixture->source_id);
	g_assert_cmpuint(http(fixture, "GET", path, NULL, NULL, NULL), ==, 404);
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/api/v1/market/history/%" G_GINT64_FORMAT "/nope", fixture->source_id);
	g_assert_cmpuint(http(fixture, "GET", path, NULL, NULL, NULL), ==, 404);
	g_clear_pointer(&path, g_free);

	/* The page: the ranges as links, the one drawn marked; four lines. */
	path = g_strdup_printf("/market/i/%" G_GINT64_FORMAT "/4306?venue=realm-x&range=7d&compare=region",
	                       fixture->source_id);
	page = get_page(fixture, path);
	g_assert_nonnull(strstr(page, "<h2>Price history</h2>"));
	g_assert_nonnull(strstr(page, "aria-current=\"page\">7d</a>"));
	g_assert_nonnull(strstr(page, "range=24h&amp;venue=realm-x&amp;compare=region"));
	g_assert_nonnull(strstr(page, "<option value=\"region\" selected>"));
	g_assert_nonnull(strstr(page, "class=\"chart-line chart-line-4\""));
	g_assert_nonnull(strstr(page, ">Region median</span>"));
	g_assert_nonnull(strstr(page, "Silk Cloth: lowest price, market value and quantity, 7 days"));
	assert_buttons_named(page, path);
}

/*
 * Deals and Browse say whether a price is a dip or the new normal: each
 * row's hourly lowest prices at its venue over seven days, as a
 * sparkline and a median, read for the whole page in one statement. The
 * dip at realm-x is 30% under its median of 1000; realm-z, seen once, is
 * too new to have a median.
 *
 * What breaks if this regresses: a price that has been the price all week
 * looks like a bargain because the region is dearer, and the buyer finds
 * out after buying.
 */
static void
test_deals_vs_median(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(JsonNode) answer = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *page = NULL;
	JsonObject *dip = NULL;
	JsonObject *fresh = NULL;
	JsonArray *rows;
	JsonArray *trend;
	guint i;

	(void)user_data;

	seed_history(fixture);

	path = g_strdup_printf("/api/v1/market/deals?source=%" G_GINT64_FORMAT, fixture->source_id);
	answer = get_json(fixture, path, 200);
	rows = json_object_get_array_member(root_of(answer), "rows");

	for (i = 0; i < json_array_get_length(rows); i++)
	{
		JsonObject *row = json_array_get_object_element(rows, i);

		if (0 == g_strcmp0(json_object_get_string_member(row, "venue_key"), "realm-x"))
			dip = row;
		if (0 == g_strcmp0(json_object_get_string_member(row, "venue_key"), "realm-z"))
			fresh = row;
	}

	g_assert_nonnull(dip);
	g_assert_nonnull(fresh);
	g_assert_cmpint(money_amount(dip, "median_7d"), ==, 1000);
	g_assert_cmpint(json_object_get_int_member(dip, "median_7d_hours"), ==, 29);
	g_assert_cmpfloat_with_epsilon(json_object_get_double_member(dip, "vs_median_7d_pct"), -30.0, 1e-9);
	trend = json_object_get_array_member(dip, "trend_7d");
	g_assert_cmpuint(json_array_get_length(trend), ==, 42);
	g_assert_cmpint(json_array_get_int_element(trend, 41), ==, 700);
	g_assert_true(json_array_get_null_element(trend, 0));

	g_assert_true(json_object_get_null_member(fresh, "median_7d"));
	g_assert_true(json_object_get_null_member(fresh, "vs_median_7d_pct"));
	g_assert_cmpint(json_object_get_int_member(fresh, "median_7d_hours"), ==, 1);
	g_clear_pointer(&answer, json_node_unref);
	g_clear_pointer(&path, g_free);

	/* Browse rows carry the same figures. */
	path = g_strdup_printf("/api/v1/market/browse?source=%" G_GINT64_FORMAT "&venue=realm-x",
	                       fixture->source_id);
	answer = get_json(fixture, path, 200);
	rows = json_object_get_array_member(root_of(answer), "rows");
	g_assert_cmpuint(json_array_get_length(rows), ==, 1);
	g_assert_cmpfloat_with_epsilon(json_object_get_double_member(json_array_get_object_element(rows, 0),
	                                                             "vs_median_7d_pct"), -30.0, 1e-9);
	g_clear_pointer(&path, g_free);

	/* The pages: a sparkline and the figure, sortable on Deals. */
	path = g_strdup_printf("/market/deals?source=%" G_GINT64_FORMAT, fixture->source_id);
	page = get_page(fixture, path);
	g_assert_nonnull(strstr(page, ">vs 7-day median</th>"));
	g_assert_nonnull(strstr(page, "<svg class=\"sparkline\""));
	g_assert_nonnull(strstr(page, "data-sort-value=\"-30\""));
	g_assert_nonnull(strstr(page, ">-30.0%</span>"));
	g_assert_nonnull(strstr(page, ">too new</span>"));
	g_clear_pointer(&page, g_free);
	g_clear_pointer(&path, g_free);

	path = g_strdup_printf("/market/browse?source=%" G_GINT64_FORMAT, fixture->source_id);
	page = get_page(fixture, path);
	g_assert_nonnull(strstr(page, "data-no-sort title=\"The price against"));
	g_assert_nonnull(strstr(page, ">-30.0%</span>"));
}

/* The entries of watchlist @list_id, as records. */
static GPtrArray *
list_entries(
	Fixture	*fixture,
	gint64	 list_id
){
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_WATCHLIST_ENTRY);
	GPtrArray *found;

	g_assert_true(venture_query_add_filter_int(query, "watchlist-id", VENTURE_FILTER_OP_EQ, list_id, NULL));
	found = venture_database_find(fixture->database, query, NULL);
	g_assert_nonnull(found);

	return found;
}

/* The id a Location's @name parameter carries, or 0. */
static gint64
location_id(
	const gchar	*location,
	const gchar	*name
){
	g_autofree gchar *needle = g_strdup_printf("%s=", name);
	const gchar *at = strstr(location, needle);

	return (NULL != at) ? g_ascii_strtoll(at + strlen(needle), NULL, 10) : 0;
}

/*
 * Lists on the Deals and Browse rows. "Add to list" puts a row's item on
 * a list named by id or by name -- a new name makes the list, a known
 * one (any case) is that list, a second press writes nothing -- and goes
 * back to the page it came from, its filters kept, saying so. Deals
 * filtered by a list (or two: their union) shows only their items; each
 * row says which lists it is on, and a tag's remove button takes it off.
 * Another organization's list is not found by any door.
 *
 * What breaks if this regresses: "bag flips" cannot be checked without
 * scrolling the whole deal list, a double click makes two lists, or a
 * list id from another organization reads its items into this one's page.
 */
static void
test_deals_watchlists(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(JsonNode) all = NULL;
	g_autoptr(JsonNode) filtered = NULL;
	g_autoptr(JsonNode) both = NULL;
	g_autoptr(GPtrArray) entries = NULL;
	g_autofree gchar *location = NULL;
	g_autofree gchar *form = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *page = NULL;
	g_autofree gchar *key = NULL;
	g_autofree gchar *other_key = NULL;
	JsonArray *rows;
	guint expected;
	gint64 list_id;
	gint64 second_id;
	gint64 entry_id = 0;
	guint i;

	(void)user_data;

	seed_store(fixture);

	/* Every row says its lists (none yet), and the answer the lists. */
	all = get_json(fixture, "/api/v1/market/deals", 200);
	rows = json_object_get_array_member(root_of(all), "rows");
	g_assert_cmpuint(json_array_get_length(rows), >, 1);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(root_of(all), "watchlist_choices")), ==, 0);
	key = g_strdup(json_object_get_string_member(json_array_get_object_element(rows, 0), "instrument_key"));

	for (i = 0; i < json_array_get_length(rows); i++)
	{
		JsonObject *row = json_array_get_object_element(rows, i);

		g_assert_cmpuint(json_array_get_length(json_object_get_array_member(row, "watchlists")), ==, 0);

		if ((NULL == other_key) && (0 != g_strcmp0(json_object_get_string_member(row, "instrument_key"), key)))
			other_key = g_strdup(json_object_get_string_member(row, "instrument_key"));
	}

	g_assert_nonnull(other_key);

	/* A new name makes the list; back to the same filtered page. */
	form = g_strdup_printf("data_source_id=%" G_GINT64_FORMAT "&key=%s&list=Bag+flips"
	                       "&return=%%2Fmarket%%2Fdeals%%3Fgroup%%3Deu%%26list_added%%3D9",
	                       fixture->source_id, key);
	g_assert_cmpuint(http(fixture, "POST", "/market/watchlists/add", form, NULL, &location), ==, 303);
	g_assert_true(g_str_has_prefix(location, "/market/deals?group=eu&list_added="));
	g_assert_nonnull(strstr(location, "&list_new=1"));
	list_id = location_id(location, "list_added");
	g_assert_cmpint(list_id, >, 0);
	page = get_page(fixture, location);
	g_assert_nonnull(strstr(page, "Made the list <strong>Bag flips</strong> and added it."));
	g_clear_pointer(&page, g_free);
	g_clear_pointer(&location, g_free);

	/* Again, and by the name in another case: the same list, one entry. */
	g_assert_cmpuint(http(fixture, "POST", "/market/watchlists/add", form, NULL, &location), ==, 303);
	g_assert_null(strstr(location, "list_new"));
	g_clear_pointer(&location, g_free);
	g_clear_pointer(&form, g_free);
	form = g_strdup_printf("data_source_id=%" G_GINT64_FORMAT "&key=%s&list=bag+FLIPS", fixture->source_id, key);
	g_assert_cmpuint(http(fixture, "POST", "/market/watchlists/add", form, NULL, &location), ==, 303);
	g_assert_cmpint(location_id(location, "list_added"), ==, list_id);
	g_clear_pointer(&location, g_free);
	entries = list_entries(fixture, list_id);
	g_assert_cmpuint(entries->len, ==, 1);
	g_clear_pointer(&entries, g_ptr_array_unref);

	/* Deals for the list: exactly the unfiltered rows of its item. */
	expected = 0;

	for (i = 0; i < json_array_get_length(rows); i++)
		if (0 == g_strcmp0(json_object_get_string_member(json_array_get_object_element(rows, i), "instrument_key"),
		                   key))
			expected++;

	path = g_strdup_printf("/api/v1/market/deals?watchlist=%" G_GINT64_FORMAT, list_id);
	filtered = get_json(fixture, path, 200);
	rows = json_object_get_array_member(root_of(filtered), "rows");
	g_assert_cmpuint(json_array_get_length(rows), ==, expected);
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(
		json_object_get_array_member(root_of(filtered), "watchlist_filter"), 0), "name"), ==, "Bag flips");

	for (i = 0; i < json_array_get_length(rows); i++)
	{
		JsonObject *row = json_array_get_object_element(rows, i);
		JsonArray *on = json_object_get_array_member(row, "watchlists");

		g_assert_cmpstr(json_object_get_string_member(row, "instrument_key"), ==, key);
		g_assert_cmpuint(json_array_get_length(on), ==, 1);
		g_assert_cmpint(json_object_get_int_member(json_array_get_object_element(on, 0), "id"), ==, list_id);
		entry_id = json_object_get_int_member(json_array_get_object_element(on, 0), "entry_id");
	}

	g_assert_cmpint(entry_id, >, 0);

	/* A second list, by the API twin, and the union of the two. */
	g_clear_pointer(&form, g_free);
	form = g_strdup_printf("data_source_id=%" G_GINT64_FORMAT "&key=%s&list=Favourites", fixture->source_id,
	                       other_key);
	g_assert_cmpuint(http(fixture, "POST", "/market/watchlists/add", form, NULL, &location), ==, 303);
	second_id = location_id(location, "list_added");
	g_assert_cmpint(second_id, >, 0);
	g_assert_cmpint(second_id, !=, list_id);
	g_clear_pointer(&location, g_free);
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/api/v1/market/deals?watchlist=%" G_GINT64_FORMAT "&watchlist=%" G_GINT64_FORMAT,
	                       list_id, second_id);
	both = get_json(fixture, path, 200);
	rows = json_object_get_array_member(root_of(both), "rows");
	g_assert_cmpuint(json_array_get_length(rows), >, expected);

	for (i = 0; i < json_array_get_length(rows); i++)
	{
		const gchar *its = json_object_get_string_member(json_array_get_object_element(rows, i), "instrument_key");

		g_assert_true((0 == g_strcmp0(its, key)) || (0 == g_strcmp0(its, other_key)));
	}

	/* The page: the List picker, the row's tag and "Add to list". */
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/market/deals?watchlist=%" G_GINT64_FORMAT, list_id);
	page = get_page(fixture, path);
	assert_buttons_named(page, path);
	g_assert_nonnull(strstr(page, "<input type=\"checkbox\" name=\"watchlist\""));
	g_assert_nonnull(strstr(page, " checked><span>Bag flips</span></label>"));
	g_assert_nonnull(strstr(page, "<datalist id=\"watchlist-names\">"));
	g_assert_nonnull(strstr(page, "class=\"list-tag\""));
	g_assert_nonnull(strstr(page, "action=\"/market/watchlists/add\" class=\"list-add\""));
	g_assert_nonnull(strstr(page, "action=\"/market/watchlists/remove\""));
	g_clear_pointer(&page, g_free);

	/* Browse rows have the same control, and the watchlist pages open
	 * the list in Deals. */
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/market/browse?source=%" G_GINT64_FORMAT, fixture->source_id);
	page = get_page(fixture, path);
	g_assert_nonnull(strstr(page, "action=\"/market/watchlists/add\" class=\"list-add\""));
	g_assert_nonnull(strstr(page, ">Bag flips</a>"));
	g_clear_pointer(&page, g_free);
	page = get_page(fixture, "/market/watchlists");
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("href=\"/market/deals?watchlist=%" G_GINT64_FORMAT "\"", list_id);
	g_assert_nonnull(strstr(page, path));
	g_clear_pointer(&page, g_free);
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/market/watchlists/%" G_GINT64_FORMAT, list_id);
	page = get_page(fixture, path);
	g_assert_nonnull(strstr(page, "Open in Deals"));
	g_assert_nonnull(strstr(page, "action=\"/market/watchlists/remove\""));
	g_clear_pointer(&page, g_free);

	/* Off the list from its tag: back to the page, saying so. */
	g_clear_pointer(&form, g_free);
	form = g_strdup_printf("entry_id=%" G_GINT64_FORMAT "&return=%%2Fmarket%%2Fdeals%%3Fwatchlist%%3D%" G_GINT64_FORMAT,
	                       entry_id, list_id);
	g_assert_cmpuint(http(fixture, "POST", "/market/watchlists/remove", form, NULL, &location), ==, 303);
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/market/deals?watchlist=%" G_GINT64_FORMAT "&list_removed=%" G_GINT64_FORMAT,
	                       list_id, list_id);
	g_assert_cmpstr(location, ==, path);
	page = get_page(fixture, location);
	g_assert_nonnull(strstr(page, "Removed from <strong>Bag flips</strong>."));
	g_assert_nonnull(strstr(page, "Nothing on the list is a deal right now."));
	g_clear_pointer(&page, g_free);
	g_clear_pointer(&location, g_free);
	entries = list_entries(fixture, list_id);
	g_assert_cmpuint(entries->len, ==, 0);
	g_clear_pointer(&entries, g_ptr_array_unref);

	/* Not a list, not this organization's list. */
	g_assert_cmpuint(http(fixture, "GET", "/api/v1/market/deals?watchlist=bags", NULL, NULL, NULL), ==, 400);
	{
		g_autoptr(VentureEntity) organization = VENTURE_ENTITY(venture_organization_new());
		g_autoptr(VentureEntity) theirs = VENTURE_ENTITY(venture_watchlist_new());
		gint64 their_source = foreign_source(fixture);

		g_object_set(organization, "name", "Third", "slug", "third", NULL);
		save(fixture, organization);
		venture_entity_set_organization_id(theirs, ID(organization));
		g_object_set(theirs, "name", "Their bags", NULL);
		save(fixture, theirs);

		g_clear_pointer(&path, g_free);
		path = g_strdup_printf("/api/v1/market/deals?watchlist=%" G_GINT64_FORMAT, ID(theirs));
		g_assert_cmpuint(http(fixture, "GET", path, NULL, NULL, NULL), ==, 404);
		g_clear_pointer(&path, g_free);
		path = g_strdup_printf("/api/v1/market/deals?watchlist=%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT,
		                       list_id, ID(theirs));
		g_assert_cmpuint(http(fixture, "GET", path, NULL, NULL, NULL), ==, 404);
		g_clear_pointer(&form, g_free);
		form = g_strdup_printf("data_source_id=%" G_GINT64_FORMAT "&key=%s&list_id=%" G_GINT64_FORMAT,
		                       fixture->source_id, key, ID(theirs));
		g_assert_cmpuint(http(fixture, "POST", "/market/watchlists/add", form, NULL, NULL), ==, 404);
		g_clear_pointer(&form, g_free);
		form = g_strdup_printf("data_source_id=%" G_GINT64_FORMAT "&key=%s&list=Bag+flips", their_source, key);
		g_assert_cmpuint(http(fixture, "POST", "/market/watchlists/add", form, NULL, NULL), ==, 404);
		entries = list_entries(fixture, ID(theirs));
		g_assert_cmpuint(entries->len, ==, 0);
	}

	/* A return that is not one of the Trading pages is not followed. */
	g_clear_pointer(&form, g_free);
	form = g_strdup_printf("data_source_id=%" G_GINT64_FORMAT "&key=%s&list=Bag+flips"
	                       "&return=https%%3A%%2F%%2Fevil.example%%2Fmarket%%2F", fixture->source_id, key);
	g_assert_cmpuint(http(fixture, "POST", "/market/watchlists/add", form, NULL, &location), ==, 303);
	g_assert_true(g_str_has_prefix(location, "/market/i/"));
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
	ADD("price-history", test_price_history);
	ADD("deals-vs-median", test_deals_vs_median);
	ADD("deals-buy-sell-venues", test_deals_buy_sell_venues);
	ADD("deals-freshness", test_deals_freshness);
	ADD("deals-connected-realms", test_deals_connected_realms);
	ADD("deals-ignore-asking-prices", test_deals_ignore_asking_prices);
	ADD("deals-velocity", test_deals_velocity);
	ADD("deals-realisable-order", test_deals_realisable_order);
	ADD("deals-reference-source", test_deals_reference_source);
	ADD("venue-index", test_venue_index);
	ADD("watchlist-and-actions", test_watchlist_and_actions);
	ADD("deals-watchlists", test_deals_watchlists);
	ADD("alerts", test_alerts);
	ADD("widgets", test_widgets);
	ADD("attribution", test_attribution);
	ADD("doors-and-looks", test_doors_and_looks);
	ADD("venue-groups-and-find", test_venue_groups_and_find);
	ADD("reader-upgrades-store", test_reader_upgrades_store);
	ADD("categories", test_categories);

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
