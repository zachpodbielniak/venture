/*
 * test-arbitrage-scan.c - Finding opportunities: every strategy against a
 * seeded store, the filters, presets, export, the pages, the reports and
 * the one path from an opportunity to a trade
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * One store, seeded through its writer as test-market-pages does: four
 * auction houses in one group (three in dollars, one in euros), three
 * bookmakers and an exchange. A herb is cheap at realm-a and dear at
 * realm-b; a potion is made from herbs, a vial and a reusable mortar; two
 * matches carry odds. Every figure asserted is worked by hand in the
 * test's comment, so a change that moves a fee, a transfer or a stake
 * fails here rather than in somebody's trade. The server runs on a
 * kernel-chosen port with authentication off: these tests are about what
 * the scan says, test-auth about who may ask.
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
	gint64			 now;
	gint64			 herb_product;
	gint64			 potion_product;
	gint64			 vial_product;
	gint64			 mortar_product;
	gint64			 recipe;
} Fixture;

/* ==========================================================================
 * Helpers
 * ========================================================================== */

static void
save(
	Fixture		*fixture,
	gpointer	 record
){
	g_autoptr(GError) error = NULL;

	if (!venture_database_save(fixture->database, VENTURE_ENTITY(record), NULL, &error))
		g_error("save refused: %s", error->message);
}

static void
save_refused(
	Fixture		*fixture,
	gpointer	 record,
	const gchar	*fragment
){
	g_autoptr(GError) error = NULL;
	gboolean saved;

	saved = venture_database_save(fixture->database, VENTURE_ENTITY(record), NULL, &error);
	g_assert_false(saved);
	g_assert_nonnull(error);

	if (NULL == strstr(error->message, fragment))
		g_error("expected \"%s\" in: %s", fragment, error->message);
}

static VentureEntity *
record(
	Fixture		*fixture,
	const gchar	*type
){
	VentureEntity *entity;

	entity = venture_entity_registry_create(venture_entity_registry_get_default(), type, NULL);
	g_assert_nonnull(entity);
	venture_entity_set_organization_id(entity, fixture->org);

	return entity;
}

static void
field(
	gpointer	 entity,
	const gchar	*name,
	const gchar	*value
){
	g_autoptr(GError) error = NULL;

	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(entity), name, value, &error));
	g_assert_no_error(error);
}

/* A JSON object from text, for options. */
static JsonObject *
object_of(const gchar *text)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) node = NULL;

	node = venture_json_parse(text, &error);
	g_assert_no_error(error);
	g_assert_true(JSON_NODE_HOLDS_OBJECT(node));

	return json_object_ref(json_node_get_object(node));
}

/* Runs a scan and returns its answer's root, which must exist. */
static JsonNode *
scan(
	Fixture		*fixture,
	const gchar	*options
){
	g_autoptr(JsonObject) asked = object_of(options);
	g_autoptr(GError) error = NULL;
	JsonNode *answer;

	answer = venture_arbitrage_scan_run(fixture->context, fixture->org, asked, &error);

	if (NULL == answer)
		g_error("scan %s refused: %s", options, error->message);

	return answer;
}

static JsonArray *
rows_of(JsonNode *answer)
{
	return json_object_get_array_member(json_node_get_object(answer), "rows");
}

static JsonObject *
row_with_key(
	JsonNode	*answer,
	const gchar	*key
){
	JsonArray *rows = rows_of(answer);
	guint i;

	for (i = 0; i < json_array_get_length(rows); i++)
	{
		JsonObject *row = json_array_get_object_element(rows, i);

		if (0 == g_strcmp0(json_object_get_string_member(row, "key"), key))
			return row;
	}

	return NULL;
}

static gint64
amount_of(
	JsonObject	*object,
	const gchar	*member
){
	JsonNode *node = json_object_get_member(object, member);

	g_assert_nonnull(node);

	if (!JSON_NODE_HOLDS_OBJECT(node))
		g_error("%s is not money", member);

	return json_object_get_int_member(json_node_get_object(node), "amount");
}

/* Whether any note says @fragment. */
static gboolean
noted(
	JsonNode	*answer,
	const gchar	*fragment
){
	JsonArray *notes = json_object_get_array_member(json_node_get_object(answer), "notes");
	guint i;

	for (i = 0; i < json_array_get_length(notes); i++)
		if (NULL != strstr(json_array_get_string_element(notes, i), fragment))
			return TRUE;

	return FALSE;
}

/* An opportunity's key with "S" standing for the fixture's source id. */
static gchar *
key_of(
	Fixture		*fixture,
	const gchar	*pattern
){
	g_autofree gchar *source = NULL;
	g_auto(GStrv) parts = NULL;

	source = g_strdup_printf("%" G_GINT64_FORMAT, fixture->source_id);
	parts = g_strsplit(pattern, "S", -1);

	return g_strjoinv(source, parts);
}

/* --- HTTP ---------------------------------------------------------------- */

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

static guint
http(
	Fixture		 *fixture,
	const gchar	 *method,
	const gchar	 *path,
	const gchar	 *form,
	gchar		**out_body,
	gchar		**out_location,
	gchar		**out_type
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

	if (NULL != out_type)
		*out_type = g_strdup(soup_message_headers_get_content_type(
			soup_message_get_response_headers(message), NULL));

	g_bytes_unref(reply.body);

	return soup_message_get_status(message);
}

static gchar *
get_page(
	Fixture		*fixture,
	const gchar	*path
){
	gchar *body = NULL;
	guint status;

	status = http(fixture, "GET", path, NULL, &body, NULL, NULL);

	if (200 != status)
		g_error("GET %s answered %u: %s", path, status, body);

	return body;
}

/* Every button says what it does. */
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

/* ==========================================================================
 * The store
 * ========================================================================== */

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
	const gchar		*kind,
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
	venue.kind = kind;
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
	const gchar		*kind,
	const gchar		*category,
	const gchar		*parent,
	gint64			 seen_at
){
	g_autoptr(GError) error = NULL;
	VentureSeriesInstrument instrument;

	memset(&instrument, 0, sizeof(instrument));
	instrument.key = key;
	instrument.namespace_ = "item";
	instrument.name = name;
	instrument.kind = kind;
	instrument.category = category;
	instrument.parent_key = parent;
	g_assert_true(venture_series_store_upsert_instrument(store, &instrument, seen_at, NULL, NULL, &error));
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

	snap = venture_series_store_begin_snapshot(store, venue, currency, taken_at, taken_at + 30, TRUE,
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

static void
quote(
	VentureSeriesStore	*store,
	const gchar		*venue,
	const gchar		*instrument,
	VentureSeriesQuoteSide	 side,
	gdouble			 odds,
	gint64			 taken_at
){
	g_autoptr(GError) error = NULL;
	VentureSeriesQuote row;
	VentureSeriesCommitResult result;

	memset(&row, 0, sizeof(row));
	row.venue_key = venue;
	row.instrument_key = instrument;
	row.side = side;
	row.value = (gint64)llround(odds * VENTURE_SERIES_ODDS_SCALE);
	row.currency = NULL;
	row.liquidity = VENTURE_SERIES_NONE;
	row.taken_at = taken_at;
	g_assert_true(venture_series_store_add_quotes(store, &row, 1, &result, &error));
	g_assert_no_error(error);
}

/*
 * The market. Prices are minor units (cents):
 *
 *   herb    realm-a 10.00 x10, realm-b 20.00 x5, realm-c 18.00 x3,
 *           realm-d 30.00 EUR x2
 *   potion  realm-b 90.00 x4
 *   vial    realm-b 1.00 x10
 *   mortar  realm-c 5.00 x1
 *   evil    realm-a 1.00 x1 (a name that is a script), realm-b 3.00 x1
 *
 * Two matches: match-1 home/away at bk-1 (2.1/1.8) and bk-2 (1.9/2.2),
 * laid at ex-1 (home 2.0); match-2 home/away/draw at bk-1, bk-2 and bk-3.
 * Every snapshot is ten minutes old; @old_venue (if any) three hours.
 */
static void
seed_store(
	Fixture		*fixture,
	const gchar	*old_venue
){
	g_autoptr(VentureEntity) source = NULL;
	g_autoptr(VentureSeriesStore) store = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *dir = NULL;
	gint64 t;
	static const Offer a[] = {
		{ "herb", 1, 1000, 10 }, { "evil", 2, 100, 1 }
	};
	static const Offer b[] = {
		{ "herb", 10, 2000, 5 }, { "potion", 11, 9000, 4 }, { "vial", 12, 100, 10 },
		{ "evil", 13, 300, 1 }
	};
	static const Offer c[] = { { "herb", 20, 1800, 3 }, { "mortar", 21, 500, 1 } };
	static const Offer d[] = { { "herb", 30, 3000, 2 } };

	source = venture_database_get(fixture->database, VENTURE_TYPE_DATA_SOURCE, fixture->source_id, NULL);
	dir = venture_feeds_store_dir(fixture->config, venture_entity_get_uuid(source));
	store = venture_series_store_open(dir, &error);
	g_assert_no_error(error);

	t = fixture->now - 600;
	add_venue(store, "realm-a", "auction_house", "eu", "USD", t);
	add_venue(store, "realm-b", "auction_house", "eu", "USD", t);
	add_venue(store, "realm-c", "auction_house", "eu", "USD", t);
	add_venue(store, "realm-d", "auction_house", "eu", "EUR", t);
	add_venue(store, "bk-1", "bookmaker", NULL, NULL, t);
	add_venue(store, "bk-2", "bookmaker", NULL, NULL, t);
	add_venue(store, "bk-3", "bookmaker", NULL, NULL, t);
	add_venue(store, "ex-1", "exchange", NULL, NULL, t);
	add_instrument(store, "herb", "Peacebloom", "item", "Herbs", NULL, t);
	add_instrument(store, "potion", "Healing Potion", "item", "Potions", NULL, t);
	add_instrument(store, "vial", "Empty Vial", "item", "Parts", NULL, t);
	add_instrument(store, "mortar", "Mortar", "item", "Tools", NULL, t);
	add_instrument(store, "evil", hostile_name, "item", "Herbs", NULL, t);
	add_instrument(store, "match-1", "Reds v Blues", "event", "Football", NULL, t);
	add_instrument(store, "home", "Reds", "outcome", "Football", "match-1", t);
	add_instrument(store, "away", "Blues", "outcome", "Football", "match-1", t);
	add_instrument(store, "match-2", "Greens v Whites", "event", "Football", NULL, t);
	add_instrument(store, "h2", "Greens", "outcome", "Football", "match-2", t);
	add_instrument(store, "a2", "Whites", "outcome", "Football", "match-2", t);
	add_instrument(store, "d2", "Draw", "outcome", "Football", "match-2", t);

	snapshot(store, "realm-a", "USD", (0 == g_strcmp0(old_venue, "realm-a")) ? fixture->now - 3 * 3600 : t,
	         a, G_N_ELEMENTS(a));
	snapshot(store, "realm-b", "USD", t, b, G_N_ELEMENTS(b));
	snapshot(store, "realm-c", "USD", t, c, G_N_ELEMENTS(c));
	snapshot(store, "realm-d", "EUR", t, d, G_N_ELEMENTS(d));

	quote(store, "bk-1", "home", VENTURE_SERIES_QUOTE_BACK, 2.1, t);
	quote(store, "bk-1", "away", VENTURE_SERIES_QUOTE_BACK, 1.8, t);
	quote(store, "bk-2", "home", VENTURE_SERIES_QUOTE_BACK, 1.9, t);
	quote(store, "bk-2", "away", VENTURE_SERIES_QUOTE_BACK, 2.2, t);
	quote(store, "ex-1", "home", VENTURE_SERIES_QUOTE_LAY, 2.0, t);
	quote(store, "bk-1", "h2", VENTURE_SERIES_QUOTE_BACK, 2.0, t);
	quote(store, "bk-2", "a2", VENTURE_SERIES_QUOTE_BACK, 2.0, t);
	quote(store, "bk-3", "d2", VENTURE_SERIES_QUOTE_BACK, 3.0, t);

	g_assert_true(venture_series_store_recompute_region(store, NULL, fixture->now, VENTURE_SERIES_NONE, NULL,
	                                                    NULL, &error));
	g_assert_no_error(error);
}

/* A venue record for a store venue, with a fee model and a transfer. */
static gint64
venue_record(
	Fixture		*fixture,
	const gchar	*key,
	const gchar	*model,
	const gchar	*params,
	const gchar	*transfer
){
	g_autoptr(VentureEntity) venue = record(fixture, "venue");

	g_object_set(venue, "name", key, "key", key, "namespace", "realm", "data-source-id", fixture->source_id,
	             "fee-model", model, "fee-params", params, NULL);

	if (NULL != transfer)
		field(venue, "transfer-cost", transfer);

	save(fixture, venue);

	return ID(venue);
}

/* The usual venue records: realm-a free with a 1.00 move, realm-b a 5%
 * cut with a 0.50 move, ex-1 a 2% commission. realm-c has none. */
static void
seed_venues(Fixture *fixture)
{
	venue_record(fixture, "realm-a", "none", NULL, "1.00 USD");
	venue_record(fixture, "realm-b", "percent", "cut_percent: 5", "0.50 USD");
	venue_record(fixture, "ex-1", "commission", "rate_percent: 2", NULL);
}

/* ==========================================================================
 * The fixture
 * ========================================================================== */

static void
fixture_set_up(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDataSource) source = NULL;

	(void)user_data;

	fixture->state_dir = g_dir_make_tmp("venture-arbitrage-scan-XXXXXX", &error);
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
	 * the feeds and arbitrage tables exist now. */
	g_assert_true(venture_database_migrate(fixture->database, venture_entity_registry_get_default(),
	                                       &error));
	g_assert_no_error(error);

	/* Manual, so nothing starts a worker behind the test's back. */
	source = venture_data_source_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(source), fixture->org);
	g_object_set(source, "name", "Auctions", "provider", "file_jsonl", "settings", "file: x.jsonl",
	             "schedule", "manual", "currency", "USD", "instrument-namespace", "item",
	             "venue-namespace", "realm", NULL);
	save(fixture, source);
	fixture->source_id = ID(source);
	fixture->now = g_get_real_time() / G_USEC_PER_SEC;

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

/* Products, their instruments, and the potion's recipe: two herbs and a
 * vial a batch, and a mortar that is a tool. */
static void
seed_recipe(Fixture *fixture)
{
	static const struct
	{
		const gchar	*name;
		const gchar	*key;
	} products[] = {
		{ "Peacebloom", "herb" }, { "Healing Potion", "potion" }, { "Empty Vial", "vial" },
		{ "Mortar", "mortar" }
	};
	gint64 ids[4];
	guint i;

	for (i = 0; i < G_N_ELEMENTS(products); i++)
	{
		g_autoptr(VentureEntity) product = record(fixture, "product");
		g_autoptr(VentureEntity) instrument = record(fixture, "instrument");

		g_object_set(product, "name", products[i].name, NULL);
		save(fixture, product);
		ids[i] = ID(product);
		g_object_set(instrument, "name", products[i].name, "key", products[i].key, "namespace", "item",
		             "data-source-id", fixture->source_id, "product-id", ids[i], NULL);
		save(fixture, instrument);
	}

	fixture->herb_product = ids[0];
	fixture->potion_product = ids[1];
	fixture->vial_product = ids[2];
	fixture->mortar_product = ids[3];

	{
		g_autoptr(VentureEntity) recipe = record(fixture, "recipe");

		g_object_set(recipe, "name", "Brew", "output-product-id", fixture->potion_product,
		             "output-quantity", (gint64)1, "active", TRUE, NULL);
		save(fixture, recipe);
		fixture->recipe = ID(recipe);
	}

	{
		const gint64 parts[3][3] = {
			{ 0, 2, FALSE }, { 2, 1, FALSE }, { 3, 1, TRUE }
		};

		for (i = 0; i < 3; i++)
		{
			g_autoptr(VentureEntity) component = record(fixture, "recipe_component");

			g_object_set(component, "recipe-id", fixture->recipe, "product-id", ids[parts[i][0]],
			             "quantity", parts[i][1], "reusable", (gboolean)parts[i][2], NULL);
			save(fixture, component);
		}
	}
}

/* ==========================================================================
 * Strategies
 * ========================================================================== */

/*
 * spread, worked by hand for two herbs. Bought at realm-a (no fees, two
 * listings at 10.00: 20.00) and sold at realm-b (the best other venue,
 * 20.00 each: 40.00, less a 5% cut of 2.00); moving the lot costs
 * realm-a's 1.00 and realm-b's 0.50. Net 40.00 - 2.00 - 20.00 - 1.50 =
 * 16.50 on capital 21.50. The legs say exactly that, in store terms, for
 * the plan. realm-c has no venue record and says so. What breaks if
 * this regresses: a flip priced without a venue's cut or the move.
 */
static void
test_spread(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(JsonNode) answer = NULL;
	g_autofree gchar *key = NULL;
	JsonObject *row;
	JsonArray *legs;
	JsonObject *leg;

	(void)user_data;

	seed_store(fixture, NULL);
	seed_venues(fixture);

	answer = scan(fixture, "{\"strategy\":\"spread\",\"units\":2,\"category_path\":\"Herbs\"}");
	g_assert_true(json_object_get_boolean_member(json_node_get_object(answer), "available"));
	key = key_of(fixture, "spread:S:realm-a>S:realm-b:herb");
	row = row_with_key(answer, key);
	g_assert_nonnull(row);
	g_assert_cmpstr(json_object_get_string_member(row, "currency"), ==, "USD");
	g_assert_cmpint(json_object_get_int_member(row, "units"), ==, 2);
	g_assert_cmpint(amount_of(row, "net"), ==, 1650);
	g_assert_cmpint(amount_of(row, "capital"), ==, 2150);
	g_assert_cmpint(amount_of(row, "transfer_cost"), ==, 150);
	g_assert_cmpfloat_with_epsilon(json_object_get_double_member(row, "roi"), 1650.0 / 2150.0, 1e-9);
	g_assert_cmpint(amount_of(json_object_get_object_member(row, "sell"), "fees"), ==, 200);
	g_assert_cmpfloat(json_object_get_double_member(row, "confidence"), >, 0.0);

	legs = json_object_get_array_member(row, "legs");
	g_assert_cmpuint(json_array_get_length(legs), ==, 4);
	leg = json_array_get_object_element(legs, 0);
	g_assert_cmpstr(json_object_get_string_member(leg, "kind"), ==, "buy");
	g_assert_cmpstr(json_object_get_string_member(leg, "venue_key"), ==, "realm-a");
	g_assert_cmpstr(json_object_get_string_member(leg, "amount"), ==, "20.00 USD");
	g_assert_cmpint(json_object_get_int_member(leg, "quantity"), ==, 2);
	leg = json_array_get_object_element(legs, 1);
	g_assert_cmpstr(json_object_get_string_member(leg, "kind"), ==, "sell");
	g_assert_cmpstr(json_object_get_string_member(leg, "amount"), ==, "40.00 USD");
	g_assert_cmpstr(json_object_get_string_member(leg, "fees"), ==, "2.00 USD");
	leg = json_array_get_object_element(legs, 2);
	g_assert_cmpstr(json_object_get_string_member(leg, "kind"), ==, "fee");
	g_assert_cmpstr(json_object_get_string_member(leg, "amount"), ==, "1.00 USD");

	/* The euro venue sells dearest, but has no rate to dollars: it is
	 * skipped and the note says why, never compared as if equal. */
	g_assert_true(noted(answer, "No exchange rate from EUR to USD"));
	g_assert_null(row_with_key(answer, "spread:1:realm-a>1:realm-d:herb"));

	/* The evil instrument: 1.00 at realm-a, 3.00 at realm-b less 5%. */
	g_assert_nonnull(row_with_key(answer, key_of(fixture, "spread:S:realm-a>S:realm-b:evil")));
}

/*
 * A spread of nothing or below is left out and counted as unprofitable;
 * the drop-shipper's list (buy_sources) prices the two cheapest sources
 * against the same sell; a rate on file brings the euro venue in, the
 * sell converted into the buy side's dollars (30.00 EUR at 11/10 is
 * 33.00 USD each).
 */
static void
test_spread_sources_and_rates(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(JsonNode) answer = NULL;
	g_autoptr(JsonNode) converted = NULL;
	g_autoptr(VentureEntity) rate = NULL;
	JsonObject *row;
	JsonObject *excluded;

	(void)user_data;

	seed_store(fixture, NULL);
	seed_venues(fixture);

	answer = scan(fixture, "{\"units\":1,\"buy_sources\":2,\"instrument\":\"herb\"}");
	g_assert_nonnull(row_with_key(answer, key_of(fixture, "spread:S:realm-a>S:realm-b:herb")));

	/* realm-c at 18.00 into realm-b at 20.00 less 1.00 and the 0.50
	 * move: 0.50. */
	row = row_with_key(answer, key_of(fixture, "spread:S:realm-c>S:realm-b:herb"));
	g_assert_nonnull(row);
	g_assert_cmpint(amount_of(row, "net"), ==, 50);
	g_assert_nonnull(strstr(json_array_get_string_element(json_object_get_array_member(row, "warnings"), 0),
	                        "realm-c has no venue record"));

	/* Sold where it is cheapest it loses: never shown, counted. */
	answer = scan(fixture, "{\"units\":1,\"instrument\":\"herb\",\"buy_venues\":\"realm-b\","
	                       "\"sell_venues\":\"realm-a,realm-c\"}");
	g_assert_cmpuint(json_array_get_length(rows_of(answer)), ==, 0);
	excluded = json_object_get_object_member(json_node_get_object(answer), "excluded");
	g_assert_cmpint(json_object_get_int_member(excluded, "unprofitable"), >=, 1);

	rate = record(fixture, "exchange_rate");
	g_object_set(rate, "from-currency", "EUR", "to-currency", "USD", "rate-numerator", (gint64)11,
	             "rate-denominator", (gint64)10, NULL);
	field(rate, "effective-at", "2026-01-01T00:00:00Z");
	save(fixture, rate);

	converted = scan(fixture, "{\"units\":1,\"instrument\":\"herb\"}");
	row = row_with_key(converted, key_of(fixture, "spread:S:realm-a>S:realm-d:herb"));
	g_assert_nonnull(row);
	g_assert_cmpstr(json_object_get_string_member(row, "currency"), ==, "USD");
	/* 33.00 USD less 10.00 and realm-a's 1.00 move (realm-d has no
	 * record): 22.00. The sell leg stays in euros, at its venue. */
	g_assert_cmpint(amount_of(row, "net"), ==, 2200);
	g_assert_cmpstr(json_object_get_string_member(
		json_array_get_object_element(json_object_get_array_member(row, "legs"), 1), "amount"), ==,
		"30.00 EUR");
	g_assert_false(noted(converted, "No exchange rate from EUR to USD"));

	/* Bought in euros at realm-d and sold in dollars needs the other
	 * direction, which is not on file: a rate is never inverted. */
	g_assert_true(noted(converted, "No exchange rate from USD to EUR"));
}

/*
 * The filters judge every strategy's rows alike: least profit, least
 * ROI, most capital (a cap, FlippingPal's max buyout) and least
 * confidence each leave the herb out when it falls short, and keep it
 * when it passes; a bound in another currency with no rate leaves the
 * row out rather than compare euros with dollars. Stale data is
 * excluded by max_age_hours however good it looks.
 */
static void
test_filters(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	static const struct
	{
		const gchar	*options;
		gboolean	 kept;
	} cases[] = {
		{ "\"min_profit\":\"16.50 USD\"", TRUE },
		{ "\"min_profit\":\"16.51 USD\"", FALSE },
		{ "\"min_roi\":\"76\"", TRUE },
		{ "\"min_roi\":\"77\"", FALSE },
		{ "\"max_capital\":\"21.50 USD\"", TRUE },
		{ "\"max_capital\":\"21.49 USD\"", FALSE },
		/* Fresh, four venues, one price each, the book deep enough:
		 * as sure as the formula gets. */
		{ "\"min_confidence\":\"1\"", TRUE },
		{ "\"min_profit\":\"1.00 EUR\"", FALSE },
		{ "\"max_age_hours\":1", TRUE },
	};
	g_autofree gchar *key = NULL;
	guint i;

	(void)user_data;

	seed_store(fixture, NULL);
	seed_venues(fixture);
	key = key_of(fixture, "spread:S:realm-a>S:realm-b:herb");

	for (i = 0; i < G_N_ELEMENTS(cases); i++)
	{
		g_autofree gchar *options = NULL;
		g_autoptr(JsonNode) answer = NULL;

		options = g_strdup_printf("{\"units\":2,\"instrument\":\"herb\",%s}", cases[i].options);
		answer = scan(fixture, options);

		if (cases[i].kept != (NULL != row_with_key(answer, key)))
			g_error("%s: the herb should be %s", cases[i].options, cases[i].kept ? "kept" : "left out");
	}

	/*
	 * Twenty herbs asked of a book of ten: priced for the ten there are,
	 * the twenty remembered, and half as sure -- which a bound of 0.6
	 * leaves out and 0.5 keeps.
	 */
	{
		g_autoptr(JsonNode) thin = scan(fixture, "{\"units\":20,\"instrument\":\"herb\",\"min_confidence\":\"0.5\"}");
		g_autoptr(JsonNode) sure = scan(fixture, "{\"units\":20,\"instrument\":\"herb\",\"min_confidence\":\"0.6\"}");
		JsonObject *row = row_with_key(thin, key);

		g_assert_nonnull(row);
		g_assert_cmpint(json_object_get_int_member(row, "units"), ==, 10);
		g_assert_cmpint(json_object_get_int_member(row, "requested_units"), ==, 20);
		g_assert_cmpfloat_with_epsilon(json_object_get_double_member(row, "confidence"), 0.5, 1e-9);
		g_assert_null(row_with_key(sure, key));
	}
}

static void
test_stale(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(JsonNode) any_age = NULL;
	g_autoptr(JsonNode) fresh = NULL;

	(void)user_data;

	/* realm-a's snapshot is three hours old. */
	seed_store(fixture, "realm-a");
	seed_venues(fixture);
	any_age = scan(fixture, "{\"units\":2,\"instrument\":\"herb\"}");
	g_assert_nonnull(row_with_key(any_age, key_of(fixture, "spread:S:realm-a>S:realm-b:herb")));
	fresh = scan(fixture, "{\"units\":2,\"instrument\":\"herb\",\"max_age_hours\":1}");
	g_assert_null(row_with_key(fresh, key_of(fixture, "spread:S:realm-a>S:realm-b:herb")));
	g_assert_true(noted(fresh, "older than max_age_hours"));
	/* The next cheapest, fresh, is what is left. */
	g_assert_nonnull(row_with_key(fresh, key_of(fixture, "spread:S:realm-c>S:realm-b:herb")));
}

/*
 * transform, three batches. With the vial's only venue left out of the
 * buy set the row is blank -- net, capital and ROI null, never zero --
 * and names the vial. With it in: six herbs at realm-a (60.00), one
 * mortar (a tool: once, not three times) at realm-c (5.00), three vials
 * at realm-b (3.00); three potions at realm-b (270.00) less 5% (13.50);
 * the moves from realm-a (1.00) and to realm-b (0.50; realm-c has no
 * record). Net 270.00 - 13.50 - 68.00 - 1.50 = 187.00. What breaks if
 * this regresses: a craft priced with a missing input as free, or a tool
 * bought once per batch.
 */
static void
test_transform(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(JsonNode) blank = NULL;
	g_autoptr(JsonNode) priced = NULL;
	g_autofree gchar *options = NULL;
	JsonObject *row;
	JsonArray *inputs;
	JsonArray *missing;
	guint i;

	(void)user_data;

	seed_store(fixture, NULL);
	seed_venues(fixture);
	seed_recipe(fixture);

	options = g_strdup_printf("{\"strategy\":\"transform\",\"units\":3,\"recipe_id\":%" G_GINT64_FORMAT
	                          ",\"buy_venues\":\"realm-a,realm-c\"}", fixture->recipe);
	blank = scan(fixture, options);
	g_assert_cmpuint(json_array_get_length(rows_of(blank)), ==, 1);
	row = json_array_get_object_element(rows_of(blank), 0);
	g_assert_true(JSON_NODE_HOLDS_NULL(json_object_get_member(row, "net")));
	g_assert_true(JSON_NODE_HOLDS_NULL(json_object_get_member(row, "roi")));
	missing = json_object_get_array_member(row, "missing");
	g_assert_cmpuint(json_array_get_length(missing), ==, 1);
	g_assert_nonnull(strstr(json_array_get_string_element(missing, 0), "Empty Vial"));

	/* Blank is kept only while no bound asks a number of it. */
	{
		g_autofree gchar *bounded = g_strdup_printf("{\"strategy\":\"transform\",\"units\":3,\"recipe_id\":%"
		                                            G_GINT64_FORMAT ",\"buy_venues\":\"realm-a,realm-c\","
		                                            "\"min_roi\":\"1\"}", fixture->recipe);
		g_autoptr(JsonNode) none = scan(fixture, bounded);

		g_assert_cmpuint(json_array_get_length(rows_of(none)), ==, 0);
	}

	g_free(options);
	options = g_strdup_printf("{\"strategy\":\"transform\",\"units\":3,\"recipe_id\":%" G_GINT64_FORMAT "}",
	                          fixture->recipe);
	priced = scan(fixture, options);
	g_assert_cmpuint(json_array_get_length(rows_of(priced)), ==, 1);
	row = json_array_get_object_element(rows_of(priced), 0);
	g_assert_false(json_object_has_member(row, "missing"));
	g_assert_cmpint(amount_of(row, "cost"), ==, 6800);
	g_assert_cmpint(amount_of(row, "gross"), ==, 27000);
	g_assert_cmpint(amount_of(row, "transfer_cost"), ==, 150);
	g_assert_cmpint(amount_of(row, "net"), ==, 27000 - 1350 - 6800 - 150);
	inputs = json_object_get_array_member(row, "inputs");

	for (i = 0; i < json_array_get_length(inputs); i++)
	{
		JsonObject *line = json_array_get_object_element(inputs, i);
		const gchar *name = json_object_get_string_member(line, "name");

		if (0 == g_strcmp0(name, "Mortar"))
		{
			g_assert_true(json_object_get_boolean_member(line, "reusable"));
			g_assert_cmpint(json_object_get_int_member(line, "quantity"), ==, 1);
			g_assert_cmpstr(json_object_get_string_member(line, "venue_key"), ==, "realm-c");
		}
		else if (0 == g_strcmp0(name, "Peacebloom"))
		{
			g_assert_cmpint(json_object_get_int_member(line, "quantity"), ==, 6);
			g_assert_cmpstr(json_object_get_string_member(line, "venue_key"), ==, "realm-a");
			g_assert_cmpint(amount_of(line, "cost"), ==, 6000);
		}
	}
}

/*
 * deal: realm-a's herb at 10.00 is under the group's deal price (the
 * median of 10, 18 and 20 dollars: 18.00) and sold back at the region's
 * median makes 8.00; realm-c at exactly the deal price makes nothing and
 * is left out. Undermine's rule, priced.
 */
static void
test_deal(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(JsonNode) answer = NULL;
	JsonObject *row;

	(void)user_data;

	seed_store(fixture, NULL);
	seed_venues(fixture);

	answer = scan(fixture, "{\"strategy\":\"deal\",\"instrument\":\"herb\"}");
	row = row_with_key(answer, key_of(fixture, "deal:S:realm-a>S:realm-a:herb"));
	g_assert_nonnull(row);
	g_assert_cmpint(amount_of(row, "net"), ==, 800);
	g_assert_null(row_with_key(answer, key_of(fixture, "deal:S:realm-c>S:realm-c:herb")));
	g_assert_null(row_with_key(answer, key_of(fixture, "deal:S:realm-b>S:realm-b:herb")));
}

/*
 * cover: match-1's best back odds are home 2.1 at bk-1 and away 2.2 at
 * bk-2, S = 0.9307. A 100.00 stake splits 51.16 / 48.84; the rounded
 * stakes pay 107.43 and 107.44, so 7.43 is sure (7.44 before rounding).
 * match-2 with bk-3 left out of the set has no odds for its draw: the row
 * is blank and names it -- two of three outcomes are not an event.
 */
static void
test_cover(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(JsonNode) answer = NULL;
	JsonObject *row;
	JsonArray *legs;
	JsonArray *missing;

	(void)user_data;

	seed_store(fixture, NULL);

	answer = scan(fixture, "{\"strategy\":\"cover\",\"total_stake\":\"100.00 USD\","
	                       "\"buy_venues\":\"bk-1,bk-2\"}");
	row = row_with_key(answer, key_of(fixture, "cover:S:match-1"));
	g_assert_nonnull(row);
	g_assert_true(json_object_get_boolean_member(row, "is_surebet"));
	g_assert_cmpint(amount_of(row, "net"), ==, 743);
	g_assert_cmpint(amount_of(row, "ideal_profit"), ==, 744);
	g_assert_cmpint(amount_of(row, "capital"), ==, 10000);
	/* Outcomes in key order: away (bk-2, 48.84), then home (bk-1). */
	legs = json_object_get_array_member(row, "legs");
	g_assert_cmpuint(json_array_get_length(legs), ==, 2);
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(legs, 0), "venue_key"), ==, "bk-2");
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(legs, 0), "amount"), ==,
	                "48.84 USD");
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(legs, 1), "venue_key"), ==, "bk-1");
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(legs, 1), "amount"), ==,
	                "51.16 USD");

	row = row_with_key(answer, key_of(fixture, "cover:S:match-2"));
	g_assert_nonnull(row);
	missing = json_object_get_array_member(row, "missing");
	g_assert_cmpuint(json_array_get_length(missing), ==, 1);
	g_assert_nonnull(strstr(json_array_get_string_element(missing, 0), "Draw"));
	g_assert_true(JSON_NODE_HOLDS_NULL(json_object_get_member(row, "net")));
}

/*
 * back_lay: home backed at bk-1 at 2.1 and laid at ex-1 at 2.0, whose
 * commission model keeps 2%. A 100.00 back needs a lay of 106.06 (a
 * liability of 106.06): 3.94 if the back wins, and 3.93 if the lay does
 * -- the exchange leaves 103.9388, paid in whole cents -- so the row's
 * net, what the pair is sure to make, is 3.93. Without ex-1's commission
 * record the lay would look 2% cheaper than it is.
 */
static void
test_back_lay(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(JsonNode) answer = NULL;
	JsonObject *row;

	(void)user_data;

	seed_store(fixture, NULL);
	seed_venues(fixture);

	answer = scan(fixture, "{\"strategy\":\"back_lay\",\"total_stake\":\"100.00 USD\"}");
	row = row_with_key(answer, key_of(fixture, "back_lay:S:home:bk-1>ex-1"));
	g_assert_nonnull(row);
	g_assert_cmpint(amount_of(row, "lay_stake"), ==, 10606);
	g_assert_cmpint(amount_of(row, "liability"), ==, 10606);
	g_assert_cmpint(amount_of(row, "if_back_wins"), ==, 394);
	g_assert_cmpint(amount_of(row, "if_lay_wins"), ==, 393);
	g_assert_cmpint(amount_of(row, "net"), ==, 393);
	g_assert_cmpfloat_with_epsilon(json_object_get_double_member(row, "commission"), 0.02, 1e-12);
	g_assert_cmpint(amount_of(row, "capital"), ==, 20606);
}

/*
 * Odds on a match that has started are not an opportunity: books stop
 * moving a finished match's prices, so its last quotes look exactly like
 * a surebet nobody can take. match-3 kicked off a minute ago and match-4
 * kicks off tomorrow, both priced like match-1 (S = 0.9307); cover keeps
 * match-4 and leaves match-3 out, and back_lay does the same for their
 * outcomes, which ask their event. match-1 and match-2 name no start and
 * are kept, and the scan says so. What breaks if this regresses: the
 * surebet page offers the stakes for a match that is over.
 */
static void
test_started_events(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) source = NULL;
	g_autoptr(VentureSeriesStore) store = NULL;
	g_autoptr(JsonNode) covered = NULL;
	g_autoptr(JsonNode) laid = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *dir = NULL;
	g_autofree gchar *started = NULL;
	g_autofree gchar *later = NULL;
	gint64 t;
	guint i;
	const struct
	{
		const gchar	*event;
		const gchar	*home;
		const gchar	*away;
		gint64		 kickoff;
	} events[] = {
		{ "match-3", "h3", "a3", -60 },
		{ "match-4", "h4", "a4", 86400 }
	};

	(void)user_data;

	seed_store(fixture, NULL);
	seed_venues(fixture);

	source = venture_database_get(fixture->database, VENTURE_TYPE_DATA_SOURCE, fixture->source_id, NULL);
	dir = venture_feeds_store_dir(fixture->config, venture_entity_get_uuid(source));
	store = venture_series_store_open(dir, &error);
	g_assert_no_error(error);
	t = fixture->now - 600;

	for (i = 0; i < G_N_ELEMENTS(events); i++)
	{
		g_autoptr(GDateTime) when = g_date_time_new_from_unix_utc(fixture->now + events[i].kickoff);
		g_autofree gchar *stamp = g_date_time_format(when, "%Y-%m-%dT%H:%M:%SZ");
		g_autofree gchar *attrs = g_strdup_printf("{\"commence_time\":\"%s\"}", stamp);
		VentureSeriesInstrument instrument;

		memset(&instrument, 0, sizeof(instrument));
		instrument.key = events[i].event;
		instrument.namespace_ = "item";
		instrument.name = events[i].event;
		instrument.kind = "event";
		instrument.category = "Football";
		instrument.attrs_json = attrs;
		g_assert_true(venture_series_store_upsert_instrument(store, &instrument, t, NULL, NULL, &error));
		g_assert_no_error(error);
		add_instrument(store, events[i].home, "Home", "outcome", "Football", events[i].event, t);
		add_instrument(store, events[i].away, "Away", "outcome", "Football", events[i].event, t);
		quote(store, "bk-1", events[i].home, VENTURE_SERIES_QUOTE_BACK, 2.1, t);
		quote(store, "bk-2", events[i].away, VENTURE_SERIES_QUOTE_BACK, 2.2, t);
		quote(store, "ex-1", events[i].home, VENTURE_SERIES_QUOTE_LAY, 2.0, t);
	}

	covered = scan(fixture, "{\"strategy\":\"cover\",\"total_stake\":\"100.00 USD\","
	                        "\"buy_venues\":\"bk-1,bk-2\"}");
	started = key_of(fixture, "cover:S:match-3");
	later = key_of(fixture, "cover:S:match-4");
	g_assert_null(row_with_key(covered, started));
	g_assert_nonnull(row_with_key(covered, later));
	g_assert_cmpint(amount_of(row_with_key(covered, later), "net"), ==, 743);
	g_assert_nonnull(row_with_key(covered, key_of(fixture, "cover:S:match-1")));
	g_assert_true(noted(covered, "1 event that had already started"));
	g_assert_true(noted(covered, "2 events name no commence_time"));

	laid = scan(fixture, "{\"strategy\":\"back_lay\",\"total_stake\":\"100.00 USD\"}");
	g_assert_null(row_with_key(laid, key_of(fixture, "back_lay:S:h3:bk-1>ex-1")));
	g_assert_nonnull(row_with_key(laid, key_of(fixture, "back_lay:S:h4:bk-1>ex-1")));
	g_assert_nonnull(row_with_key(laid, key_of(fixture, "back_lay:S:home:bk-1>ex-1")));
	g_assert_true(noted(laid, "1 event that had already started"));
}

/* ==========================================================================
 * From an opportunity to a trade
 * ========================================================================== */

/*
 * Recording goes through the plan and the `record` action: a planned
 * trade whose legs are the opportunity's -- the buy at realm-a for 20.00,
 * the sell at realm-b for 40.00 with 2.00 fees, a fee at each end for the
 * move -- on venue and instrument records promoted for it, and the
 * expected profit the performance report reads. An opportunity that is
 * no longer there is NOT_FOUND, never recorded from what a page showed.
 */
static void
test_record_from_plan(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(JsonObject) options = NULL;
	g_autoptr(VentureEntity) trade = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) legs = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *key = NULL;
	g_autofree gchar *strategy = NULL;
	g_autofree gchar *expected = NULL;
	gint status;
	gint kind;

	(void)user_data;

	seed_store(fixture, NULL);
	seed_venues(fixture);
	key = key_of(fixture, "spread:S:realm-a>S:realm-b:herb");
	options = object_of("{\"units\":2,\"instrument\":\"herb\"}");

	trade = venture_arbitrage_record_opportunity(fixture->context, fixture->org, options, key, NULL,
	                                             VENTURE_USER_ROLE_OWNER, &error);
	g_assert_no_error(error);
	g_assert_nonnull(trade);
	g_object_get(trade, "status", &status, "strategy", &strategy, "expected", &expected, NULL);
	g_assert_cmpint(status, ==, VENTURE_ARBITRAGE_TRADE_STATUS_PLANNED);
	g_assert_cmpstr(strategy, ==, "spread");
	g_assert_nonnull(strstr(expected, "16.50 USD"));

	query = venture_query_new(VENTURE_TYPE_ARBITRAGE_LEG);
	venture_query_set_limit(query, 0);
	venture_query_add_filter_int(query, "trade-id", VENTURE_FILTER_OP_EQ, ID(trade), NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	legs = venture_database_find(fixture->database, query, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(legs->len, ==, 4);

	{
		static const gchar *const amounts[] = { "20.00 USD", "40.00 USD", "1.00 USD", "0.50 USD" };
		static const gint kinds[] = {
			VENTURE_ARBITRAGE_LEG_KIND_BUY, VENTURE_ARBITRAGE_LEG_KIND_SELL,
			VENTURE_ARBITRAGE_LEG_KIND_FEE, VENTURE_ARBITRAGE_LEG_KIND_FEE
		};
		guint i;

		for (i = 0; i < legs->len; i++)
		{
			VentureEntity *leg = g_ptr_array_index(legs, i);
			g_autoptr(VentureMoney) amount = NULL;
			g_autofree gchar *text = NULL;
			gint64 venue;
			gint leg_status;

			g_object_get(leg, "kind", &kind, "status", &leg_status, "amount", &amount, "venue-id", &venue,
			             NULL);
			text = venture_money_to_string(amount);
			g_assert_cmpint(kind, ==, kinds[i]);
			g_assert_cmpint(leg_status, ==, VENTURE_ARBITRAGE_LEG_STATUS_PLANNED);
			g_assert_cmpstr(text, ==, amounts[i]);
			g_assert_cmpint(venue, >, 0);
		}
	}

	/* The instrument was promoted to a record for the legs to name. */
	{
		g_autoptr(VentureEntity) instrument = NULL;
		gint64 instrument_id = 0;

		g_object_get(g_ptr_array_index(legs, 0), "instrument-id", &instrument_id, NULL);
		instrument = venture_database_get(fixture->database, VENTURE_TYPE_INSTRUMENT, instrument_id, NULL);
		g_assert_nonnull(instrument);
	}

	g_clear_object(&trade);
	trade = venture_arbitrage_record_opportunity(fixture->context, fixture->org, options,
	                                             "spread:999:nowhere>999:nowhere:herb", NULL,
	                                             VENTURE_USER_ROLE_OWNER, &error);
	g_assert_null(trade);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_assert_nonnull(strstr(error->message, "no longer there"));
}

/* ==========================================================================
 * Export, presets, fee models, plugins
 * ========================================================================== */

/*
 * csv writes one line per row with the figures, quoting what needs it
 * and defusing a name a spreadsheet would run as a formula; the shopping
 * list groups every buy by venue with a total per currency. A plugin's
 * format registered with a plain function is served the same rows.
 */
static GBytes *
export_count(
	JsonArray	 *rows,
	JsonObject	 *options,
	gpointer	  user_data,
	GError		**error
){
	g_autofree gchar *text = NULL;

	(void)options;
	(void)error;

	text = g_strdup_printf("%s %u", (const gchar *)user_data, json_array_get_length(rows));

	return g_bytes_new(text, strlen(text));
}

static void
test_export(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	VentureExportFormatRegistry *formats;
	g_autoptr(JsonNode) answer = NULL;
	g_autoptr(GBytes) csv = NULL;
	g_autoptr(GBytes) list = NULL;
	g_autoptr(GBytes) plugin = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *csv_text = NULL;
	g_autofree gchar *list_text = NULL;
	JsonObject *root;

	(void)user_data;

	seed_store(fixture, NULL);
	seed_venues(fixture);
	answer = scan(fixture, "{\"units\":2,\"category_path\":\"Herbs\"}");
	root = json_node_get_object(answer);
	formats = venture_context_get_export_formats(fixture->context);

	csv = venture_export_format_registry_export(formats, "csv", json_object_get_array_member(root, "rows"),
	                                            json_object_get_object_member(root, "options"), &error);
	g_assert_no_error(error);
	csv_text = g_strndup(g_bytes_get_data(csv, NULL), g_bytes_get_size(csv));
	g_assert_true(g_str_has_prefix(csv_text, "strategy,key,title,instrument,"));
	g_assert_nonnull(strstr(csv_text, "16.50 USD"));
	g_assert_nonnull(strstr(csv_text, "Peacebloom"));

	list = venture_export_format_registry_export(formats, "shopping_list",
	                                             json_object_get_array_member(root, "rows"), NULL, &error);
	g_assert_no_error(error);
	list_text = g_strndup(g_bytes_get_data(list, NULL), g_bytes_get_size(list));
	g_assert_nonnull(strstr(list_text, "At realm-a"));
	g_assert_nonnull(strstr(list_text, "2 x Peacebloom  20.00 USD"));
	g_assert_null(strstr(list_text, "At realm-b"));

	g_assert_true(venture_export_format_registry_add(formats, "count", "Count", "text/plain", "txt",
	                                                 export_count, (gpointer)"rows", NULL, &error));
	plugin = venture_export_format_registry_export(formats, "count", json_object_get_array_member(root, "rows"),
	                                               NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpmem(g_bytes_get_data(plugin, NULL), g_bytes_get_size(plugin), "rows 2", 6);

	g_assert_null(venture_export_format_registry_export(formats, "tsm", NULL, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_clear_error(&error);

	/* A title a data source wrote as a formula is defused, even one that
	 * starts with a minus and a digit; a negative figure is not. */
	{
		g_autoptr(JsonArray) hostile = json_array_new();
		g_autoptr(VentureMoney) loss = venture_money_new(-150, "USD", 2);
		g_autoptr(GBytes) defused = NULL;
		g_autofree gchar *defused_text = NULL;
		JsonObject *row = json_object_new();

		json_object_set_string_member(row, "strategy", "spread");
		json_object_set_string_member(row, "key", "k");
		json_object_set_string_member(row, "title", "-2+3+cmd|' /C calc'!E2");
		json_object_set_string_member(row, "instrument_name", "\t=1+1");
		json_object_set_member(row, "net", venture_money_to_json(loss));
		json_array_add_object_element(hostile, row);

		defused = venture_export_format_registry_export(formats, "csv", hostile, NULL, &error);
		g_assert_no_error(error);
		defused_text = g_strndup(g_bytes_get_data(defused, NULL), g_bytes_get_size(defused));
		g_assert_nonnull(strstr(defused_text, ",'-2+3+cmd|"));
		g_assert_nonnull(strstr(defused_text, ",'\t=1+1"));
		g_assert_nonnull(strstr(defused_text, ",-1.50 USD"));
		g_assert_null(strstr(defused_text, ",'-1.50 USD"));
	}

	/* A content type becomes a header verbatim: no line breaks in it. */
	g_assert_false(venture_export_format_registry_add(formats, "split", "Split", "text/plain\r\nX-Evil: 1",
	                                                  "txt", export_count, (gpointer)"rows", NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
}

/*
 * An export format with zero bytes in it, as a plugin's binary format
 * would have: the body ends at the format's own length.
 */
static GBytes *
export_with_nuls(
	JsonArray	 *rows,
	JsonObject	 *options,
	gpointer	  user_data,
	GError		**error
){
	(void)rows;
	(void)options;
	(void)user_data;
	(void)error;

	return g_bytes_new_static("PK\0\3\0x", 6);
}

/*
 * /arbitrage/export serves a format's bytes as they are. A plugin may
 * register a binary format (a spreadsheet, an archive); copying its bytes
 * into a NUL-terminated string ended the download at the first zero byte,
 * so this one arrived as two bytes of six.
 */
static void
test_export_binary(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	VentureExportFormatRegistry *formats;
	g_autoptr(GError) error = NULL;
	g_autoptr(SoupMessage) message = NULL;
	g_autofree gchar *url = NULL;
	Reply reply;

	(void)user_data;

	seed_store(fixture, NULL);
	seed_venues(fixture);
	formats = venture_context_get_export_formats(fixture->context);
	g_assert_true(venture_export_format_registry_add(formats, "zipish", "Zipish", "application/octet-stream",
	                                                 "bin", export_with_nuls, NULL, NULL, &error));
	g_assert_no_error(error);

	memset(&reply, 0, sizeof(reply));
	url = g_strconcat(venture_web_server_get_base_url(fixture->server), "/arbitrage/export?format=zipish",
	                  NULL);
	message = soup_message_new("GET", url);
	soup_session_send_and_read_async(fixture->session, message, G_PRIORITY_DEFAULT, NULL, reply_done,
	                                 &reply);

	while (!reply.done)
		g_main_context_iteration(NULL, TRUE);

	g_assert_no_error(reply.error);
	g_assert_cmpuint(soup_message_get_status(message), ==, 200);
	g_assert_cmpmem(g_bytes_get_data(reply.body, NULL), g_bytes_get_size(reply.body), "PK\0\3\0x", 6);
	g_bytes_unref(reply.body);
}

/*
 * A preset is a saved question: its strategy, venue sets and filters
 * load with preset_id, and what is asked beside it wins. Its filters are
 * read by the scan's own reader at the save, so a misspelt option or an
 * unknown strategy is refused there rather than ignored at every scan;
 * another organization's preset is NOT_FOUND.
 */
static void
test_presets(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) preset = NULL;
	g_autoptr(VentureEntity) bad = NULL;
	g_autoptr(JsonNode) loaded = NULL;
	g_autoptr(JsonNode) overridden = NULL;
	g_autoptr(JsonObject) asked = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *options = NULL;
	JsonObject *normalised;

	(void)user_data;

	seed_store(fixture, NULL);
	seed_venues(fixture);

	preset = record(fixture, "arbitrage_strategy");
	g_object_set(preset, "name", "Herbs from C", "strategy", "spread", "buy-venues", "realm-c",
	             "options", "units: 1\nmin_profit: 0.10 USD\ncategory_path: Herbs\n", NULL);
	save(fixture, preset);

	options = g_strdup_printf("{\"preset_id\":%" G_GINT64_FORMAT "}", ID(preset));
	loaded = scan(fixture, options);
	normalised = json_object_get_object_member(json_node_get_object(loaded), "options");
	g_assert_cmpstr(json_object_get_string_member(normalised, "buy_venues"), ==, "realm-c");
	g_assert_cmpstr(json_object_get_string_member(normalised, "min_profit"), ==, "0.10 USD");
	g_assert_nonnull(row_with_key(loaded, key_of(fixture, "spread:S:realm-c>S:realm-b:herb")));
	g_assert_null(row_with_key(loaded, key_of(fixture, "spread:S:realm-a>S:realm-b:herb")));

	/* Asked beside it wins; a blank box does not clear it. */
	g_free(options);
	options = g_strdup_printf("{\"preset_id\":%" G_GINT64_FORMAT ",\"buy_venues\":\"realm-a\","
	                          "\"min_roi\":\"\"}", ID(preset));
	overridden = scan(fixture, options);
	g_assert_nonnull(row_with_key(overridden, key_of(fixture, "spread:S:realm-a>S:realm-b:herb")));

	bad = record(fixture, "arbitrage_strategy");
	g_object_set(bad, "name", "Typo", "strategy", "spread", "options", "min_proft: 1.00 USD", NULL);
	save_refused(fixture, bad, "min_proft");
	g_object_set(bad, "options", "min_roi: lots", NULL);
	save_refused(fixture, bad, "min_roi");
	g_object_set(bad, "options", "", "strategy", "astrology", NULL);
	save_refused(fixture, bad, "astrology");
	g_object_set(bad, "strategy", "cover", "options", "preset_id: 1", NULL);
	save_refused(fixture, bad, "cannot load another preset");

	asked = object_of("{\"preset_id\":99999}");
	g_assert_null(venture_arbitrage_scan_run(fixture->context, fixture->org, asked, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_clear_error(&error);

	/* A misspelt question is refused by name, not ignored. */
	g_clear_pointer(&asked, json_object_unref);
	asked = object_of("{\"min_proft\":\"1.00 USD\"}");
	g_assert_null(venture_arbitrage_scan_run(fixture->context, fixture->org, asked, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_assert_nonnull(strstr(error->message, "min_proft"));
	g_clear_error(&error);
	g_clear_pointer(&asked, json_object_unref);
	asked = object_of("{\"min_profit\":\"1.00\"}");
	g_assert_null(venture_arbitrage_scan_run(fixture->context, fixture->org, asked, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_assert_nonnull(strstr(error->message, "names no currency"));
}

/*
 * A venue's fee model is judged when written: an unregistered name is
 * refused naming the registered ones, parameters the model does not take
 * are refused, and a name kept from before is left alone -- a venue
 * saved under a plugin's model while the module was off stays editable.
 * A scan at that venue blanks the row and names the missing model rather
 * than reading it as no fee.
 */
static void
test_fee_model_on_venues(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) venue = NULL;
	g_autoptr(VentureEntity) stored = NULL;
	g_autoptr(JsonNode) answer = NULL;
	JsonObject *row;

	(void)user_data;

	seed_store(fixture, NULL);
	venue_record(fixture, "realm-a", "none", NULL, "1.00 USD");

	venue = record(fixture, "venue");
	g_object_set(venue, "name", "realm-b", "key", "realm-b", "data-source-id", fixture->source_id,
	             "fee-model", "wow_auction", NULL);
	save_refused(fixture, venue, "registered: percent, commission, none");
	g_object_set(venue, "fee-model", "percent", "fee-params", "cut_percnt: 5", NULL);
	save_refused(fixture, venue, "cut_percnt");

	/* With the module off nothing judges a fee model... */
	venture_config_set_module_enabled(fixture->config, "arbitrage", FALSE);
	g_object_set(venue, "fee-model", "wow_auction", "fee-params", NULL, NULL);
	save(fixture, venue);
	venture_config_set_module_enabled(fixture->config, "arbitrage", TRUE);

	/* ...and a name kept is not judged again. */
	stored = venture_database_get(fixture->database, VENTURE_TYPE_VENUE, ID(venue), NULL);
	g_object_set(stored, "notes", "Plugin's model", NULL);
	save(fixture, stored);
	g_object_set(stored, "fee-model", "wow_auction_2", NULL);
	save_refused(fixture, stored, "not a registered fee model");

	answer = scan(fixture, "{\"units\":1,\"instrument\":\"herb\"}");
	row = row_with_key(answer, key_of(fixture, "spread:S:realm-a>S:realm-b:herb"));
	g_assert_nonnull(row);
	g_assert_true(JSON_NODE_HOLDS_NULL(json_object_get_member(row, "net")));
	g_assert_nonnull(strstr(json_array_get_string_element(json_object_get_array_member(row, "missing"), 0),
	                        "wow_auction is not loaded"));
}

/* A plugin's strategy, registered with plain functions: one opportunity
 * and its own option, filtered and planned like a built-in one. */
static gboolean
toy_scan(
	VentureArbitrageScan	 *scan_state,
	gpointer		  user_data,
	GError			**error
){
	g_autoptr(VentureMoney) price = venture_money_new(1000, "USD", 2);
	g_autoptr(VentureMoney) sell = venture_money_new(1500, "USD", 2);
	VentureArbitrageSide buy;
	VentureArbitrageSide sold;
	VentureArbitrageMarket market;
	GPtrArray *sources;
	gint64 source_id;

	g_assert_cmpstr(user_data, ==, "toy");
	g_assert_cmpstr(json_object_get_string_member(venture_arbitrage_scan_get_options(scan_state), "toy_level"),
	                ==, "3");
	sources = venture_arbitrage_scan_get_sources(scan_state);
	source_id = venture_entity_get_id(g_ptr_array_index(sources, 0));

	memset(&buy, 0, sizeof(buy));
	buy.data_source_id = source_id;
	buy.venue_key = "realm-c";
	buy.instrument_key = "mortar";
	buy.units = 1;
	buy.unit_price = price;
	buy.taken_at = venture_arbitrage_scan_get_now(scan_state);
	buy.depth = 1.0;
	sold = buy;
	sold.venue_key = "realm-a";
	sold.unit_price = sell;
	market.sale_rate = NAN;
	market.sold_per_day = NAN;
	market.dispersion = NAN;
	market.venues = 3;

	return venture_arbitrage_scan_add_flip(scan_state, "toy:mortar", "A toy flip", &buy, &sold, &market, NULL,
	                                       error);
}

static void
test_plugin_strategy(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	static const gchar *const toy_options[] = { "toy_level", NULL };
	static const gchar *const shadowing[] = { "min_profit", NULL };
	VentureArbitrageStrategyRegistry *strategies;
	g_autoptr(JsonNode) answer = NULL;
	g_autoptr(JsonObject) asked = NULL;
	g_autoptr(GError) error = NULL;
	JsonObject *row;

	(void)user_data;

	seed_store(fixture, NULL);
	seed_venues(fixture);
	strategies = venture_context_get_arbitrage_strategies(fixture->context);

	g_assert_true(venture_arbitrage_strategy_registry_add(strategies, "toy", "Toy", "A test's", toy_options,
	                                                      toy_scan, NULL, (gpointer)"toy", NULL, &error));
	g_assert_no_error(error);
	g_assert_false(venture_arbitrage_strategy_registry_add(strategies, "spread", "Again", "", NULL, toy_scan,
	                                                       NULL, NULL, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_ALREADY_EXISTS);
	g_clear_error(&error);
	g_assert_false(venture_arbitrage_strategy_registry_add(strategies, "toy_two", "Toy", "", shadowing,
	                                                       toy_scan, NULL, NULL, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);

	/* 15.00 less 10.00 and realm-a's 1.00 move: 4.00. */
	answer = scan(fixture, "{\"strategy\":\"toy\",\"toy_level\":\"3\"}");
	row = row_with_key(answer, "toy:mortar");
	g_assert_nonnull(row);
	g_assert_cmpstr(json_object_get_string_member(row, "strategy"), ==, "toy");
	g_assert_cmpint(amount_of(row, "net"), ==, 400);

	/* Its option is its own: spread does not take it. */
	asked = object_of("{\"strategy\":\"spread\",\"toy_level\":\"3\"}");
	g_assert_null(venture_arbitrage_scan_run(fixture->context, fixture->org, asked, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
}

/*
 * With the module off a scan is refused as configuration; with feeds off
 * it is a page, not an error -- "no data sources" -- and a source in
 * another organization is NOT_FOUND by id.
 */
static void
test_modules_off(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(JsonNode) answer = NULL;
	g_autoptr(JsonObject) asked = NULL;
	g_autoptr(GError) error = NULL;

	(void)user_data;

	g_object_set(fixture->config, "feeds-enabled", FALSE, NULL);
	answer = scan(fixture, "{}");
	g_assert_false(json_object_get_boolean_member(json_node_get_object(answer), "available"));
	g_assert_true(noted(answer, "market data feeds are off"));
	g_object_set(fixture->config, "feeds-enabled", TRUE, NULL);

	asked = object_of("{\"data_source_id\":424242}");
	g_assert_null(venture_arbitrage_scan_run(fixture->context, fixture->org, asked, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_clear_error(&error);

	venture_config_set_module_enabled(fixture->config, "arbitrage", FALSE);
	g_assert_null(venture_arbitrage_scan_run(fixture->context, fixture->org, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
	venture_config_set_module_enabled(fixture->config, "arbitrage", TRUE);
}

/* ==========================================================================
 * Pages, reports, the widget
 * ========================================================================== */

/*
 * /arbitrage draws the scan the API answers: the herb's row, its record
 * button, export links, the strategy tabs; a name that is a script is
 * text; every button is named. Record attempt posts the question and the
 * key and lands on the trade. /arbitrage/calc works with no script: the
 * surebet's answer is on the page that asked. The export is a download.
 */
static void
test_pages(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *page = NULL;
	g_autofree gchar *calc = NULL;
	g_autofree gchar *api = NULL;
	g_autofree gchar *location = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *type = NULL;
	g_autofree gchar *form = NULL;
	g_autofree gchar *key = NULL;
	g_autofree gchar *escaped_key = NULL;
	g_autoptr(JsonNode) twin = NULL;
	g_autoptr(GError) error = NULL;

	(void)user_data;

	seed_store(fixture, NULL);
	seed_venues(fixture);

	page = get_page(fixture, "/arbitrage?units=2&category_path=Herbs");
	assert_buttons_named(page, "/arbitrage");
	g_assert_nonnull(strstr(page, "Peacebloom: realm-a"));
	g_assert_nonnull(strstr(page, "Record attempt"));
	g_assert_nonnull(strstr(page, "/arbitrage/export?format=csv"));
	g_assert_nonnull(strstr(page, "/arbitrage/export?format=shopping_list"));
	g_assert_nonnull(strstr(page, "aria-current=\"page\""));
	g_assert_nonnull(strstr(page, "class=\"sparkline"));
	g_assert_null(strstr(page, hostile_name));
	g_assert_nonnull(strstr(page, "&lt;script&gt;"));

	/* The twin answers the same rows. */
	g_assert_cmpuint(http(fixture, "GET", "/api/v1/arbitrage/scan?units=2&category_path=Herbs", NULL, &api,
	                      NULL, NULL), ==, 200);
	twin = json_from_string(api, &error);
	g_assert_no_error(error);
	key = key_of(fixture, "spread:S:realm-a>S:realm-b:herb");
	g_assert_nonnull(row_with_key(twin, key));

	/* Record attempt: the question, narrowed, and the key. */
	escaped_key = g_uri_escape_string(key, NULL, FALSE);
	form = g_strdup_printf("units=2&instrument=herb&key=%s", escaped_key);
	g_assert_cmpuint(http(fixture, "POST", "/arbitrage/record", form, &body, &location, NULL), ==, 303);
	g_assert_true(g_str_has_prefix(location, "/e/arbitrage_trade/"));

	calc = get_page(fixture, "/arbitrage/calc?calc=surebet&odds=2.1%2C+2.2&stake=100.00+USD");
	assert_buttons_named(calc, "/arbitrage/calc");
	g_assert_nonnull(strstr(calc, "Sure profit"));
	g_assert_nonnull(strstr(calc, "$7.43"));
	g_clear_pointer(&calc, g_free);
	calc = get_page(fixture, "/arbitrage/calc?calc=surebet&odds=2.1%2C+1.0&stake=100.00+USD");
	g_assert_nonnull(strstr(calc, "role=\"alert\""));
	g_assert_nonnull(strstr(calc, "Outcome 2"));
	g_clear_pointer(&api, g_free);
	g_assert_cmpuint(http(fixture, "GET", "/api/v1/arbitrage/calc?calc=back_lay&back_odds=3.2&lay_odds=3.0"
	                      "&commission=2&stake=100.00+USD", NULL, &api, NULL, NULL), ==, 200);
	g_assert_nonnull(strstr(api, "\"worst\""));

	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(http(fixture, "GET", "/arbitrage/export?format=csv&units=2&category_path=Herbs", NULL,
	                      &body, NULL, &type), ==, 200);
	g_assert_cmpstr(type, ==, "text/csv");
	g_assert_nonnull(strstr(body, "16.50 USD"));
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(http(fixture, "GET", "/arbitrage/export?format=astrology", NULL, &body, NULL, NULL), ==,
	                 404);

	g_clear_pointer(&api, g_free);
	g_assert_cmpuint(http(fixture, "GET", "/api/v1/arbitrage/registries", NULL, &api, NULL, NULL), ==, 200);
	g_assert_nonnull(strstr(api, "\"back_lay\""));
	g_assert_nonnull(strstr(api, "\"shopping_list\""));
	g_assert_nonnull(strstr(api, "\"commission\""));
}

/*
 * arbitrage_scan and craft_arbitrage through the doors: the report run
 * directly, the web API with its options, the report page with its
 * controls. craft_arbitrage lists each input at its cheapest venue (the
 * shopping list) and keeps a blank profit for an unquoted input.
 */
static void
test_reports(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	VentureReport *report;
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(JsonObject) options = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *api = NULL;
	g_autofree gchar *page = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *craft_options = NULL;

	(void)user_data;

	seed_store(fixture, NULL);
	seed_venues(fixture);
	seed_recipe(fixture);

	report = venture_report_registry_lookup(venture_context_get_report_registry(fixture->context),
	                                        "arbitrage_scan");
	g_assert_nonnull(report);
	options = object_of("{\"units\":\"2\",\"instrument\":\"herb\",\"min_roi\":\"50\",\"top\":\"5\"}");
	result = venture_report_generate(report, fixture->context, NULL, options, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 1);
	g_clear_object(&result);

	g_assert_cmpuint(http(fixture, "GET", "/api/v1/reports/arbitrage_scan?units=2&instrument=herb"
	                      "&min_profit=16.50+USD&buy_sources=2&max_age_hours=24&sort=roi", NULL, &api, NULL,
	                      NULL), ==, 200);
	g_assert_nonnull(strstr(api, "Peacebloom: realm-a"));
	g_assert_null(strstr(api, "Peacebloom: realm-c"));

	page = get_page(fixture, "/reports/arbitrage_scan?units=2&instrument=herb");
	g_assert_nonnull(strstr(page, "name=\"min_profit\""));
	g_assert_nonnull(strstr(page, "Peacebloom: realm-a"));

	report = venture_report_registry_lookup(venture_context_get_report_registry(fixture->context),
	                                        "craft_arbitrage");
	g_assert_nonnull(report);
	craft_options = g_strdup_printf("{\"recipe_id\":%" G_GINT64_FORMAT ",\"units\":3,"
	                                "\"buy_venues\":\"realm-a,realm-c\"}", fixture->recipe);
	g_clear_pointer(&options, json_object_unref);
	options = object_of(craft_options);
	result = venture_report_generate(report, fixture->context, NULL, options, &error);
	g_assert_no_error(error);
	/* Three inputs, the output and the profit. */
	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 5);
	g_clear_object(&result);

	g_clear_pointer(&api, g_free);
	path = g_strdup_printf("/api/v1/reports/craft_arbitrage?recipe_id=%" G_GINT64_FORMAT "&units=3",
	                       fixture->recipe);
	g_assert_cmpuint(http(fixture, "GET", path, NULL, &api, NULL, NULL), ==, 200);
	g_assert_nonnull(strstr(api, "Reusable: bought once"));
	g_assert_nonnull(strstr(api, "realm-a"));
}

/*
 * The opportunities card answers from the same scan, scoped to the page's
 * organization; its options are judged by the scan's reader at the save.
 */
static void
test_widget(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureDashboard) dashboard = NULL;
	g_autoptr(VentureDashboardWidget) card = NULL;
	g_autoptr(VentureDashboardWidget) bad = NULL;
	g_autoptr(VentureWidgetResult) result = NULL;
	VentureWidgetScope scope;
	gint64 orgs[1];
	JsonArray *data;

	(void)user_data;

	seed_store(fixture, NULL);
	seed_venues(fixture);

	dashboard = venture_dashboard_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(dashboard), fixture->org);
	g_object_set(dashboard, "name", "Arbitrage", "slug", "arbitrage", NULL);
	save(fixture, dashboard);
	card = venture_dashboard_widget_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(card), fixture->org);
	g_object_set(card, "dashboard-id", ID(dashboard), "kind", "opportunities", "limit", (gint64)3,
	             "options", "{\"units\":2,\"instrument\":\"herb\"}", NULL);
	save(fixture, card);

	bad = venture_dashboard_widget_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(bad), fixture->org);
	g_object_set(bad, "dashboard-id", ID(dashboard), "kind", "opportunities",
	             "options", "{\"min_proft\":\"1.00 USD\"}", NULL);
	save_refused(fixture, bad, "min_proft");

	orgs[0] = fixture->org;
	memset(&scope, 0, sizeof(scope));
	scope.organization_ids = orgs;
	scope.n_organizations = 1;
	result = venture_dashboard_render_widget(fixture->context, card, &scope);
	g_assert_null(result->error);
	data = json_node_get_array(result->data);
	g_assert_cmpuint(json_array_get_length(data), >=, 1);
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(data, 0), "net_formatted"),
	                ==, "$16.50");
	g_assert_nonnull(strstr(result->html, "Peacebloom"));
}

/*
 * Every arbitrage-* class @page's markup uses must have a rule in the
 * stylesheet the page inlined. Returns how many distinct classes it
 * checked, so the caller can tell a page that rendered nothing from one
 * that is fully styled.
 */
static guint
assert_arbitrage_classes_styled(
	const gchar	*page,
	const gchar	*look
){
	g_autoptr(GHashTable) seen = NULL;
	g_autofree gchar *style = NULL;
	const gchar *end;
	const gchar *cursor;

	seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

	/* The stylesheet is inlined: everything before the last </style> is
	 * rules, everything after is markup. */
	end = g_strrstr(page, "</style>");
	g_assert_nonnull(end);
	style = g_strndup(page, (gsize)(end - page));
	cursor = end;

	while (NULL != (cursor = strstr(cursor, "class=\"")))
	{
		g_auto(GStrv) names = NULL;
		const gchar *close;
		g_autofree gchar *value = NULL;
		guint i;

		cursor += strlen("class=\"");
		close = strchr(cursor, '"');
		g_assert_nonnull(close);
		value = g_strndup(cursor, (gsize)(close - cursor));
		names = g_strsplit(value, " ", -1);

		for (i = 0; NULL != names[i]; i++)
		{
			g_autofree gchar *rule = NULL;

			if (!g_str_has_prefix(names[i], "arbitrage-") || g_hash_table_contains(seen, names[i]))
				continue;

			rule = g_strconcat(".", names[i], NULL);

			if (NULL == strstr(style, rule))
				g_error("the %s look has no rule for %s", look, rule);

			g_hash_table_add(seen, g_strdup(names[i]));
		}

		cursor = close;
	}

	return g_hash_table_size(seen);
}

/*
 * Both looks style every class the arbitrage pages draw. The looks share
 * class names and each is written separately, so a rule added to one
 * and forgotten in the other leaves a page unstyled in that look only,
 * which no other test would see.
 */
static void
test_looks(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	static const gchar *const looks[] = { "industrial", "classic", NULL };
	static const gchar *const paths[] = {
		"/arbitrage?units=2&category_path=Herbs",
		"/arbitrage/calc?calc=surebet&odds=2.1%2C+2.2&stake=100.00+USD",
		NULL
	};
	guint i;
	guint j;

	(void)user_data;

	seed_store(fixture, NULL);
	seed_venues(fixture);

	for (i = 0; NULL != looks[i]; i++)
	{
		guint checked = 0;

		g_object_set(fixture->config, "ui-look", looks[i], NULL);

		for (j = 0; NULL != paths[j]; j++)
		{
			g_autofree gchar *page = get_page(fixture, paths[j]);

			checked += assert_arbitrage_classes_styled(page, looks[i]);
		}

		/* Tabs, tab, presets, filters, exports, table; calc, answer. */
		g_assert_cmpuint(checked, >=, 8);
	}
}

#define ADD(path, func) \
	g_test_add("/arbitrage-scan/" path, Fixture, NULL, fixture_set_up, func, fixture_tear_down)

gint
main(
	gint	 argc,
	gchar	**argv
){
	g_test_init(&argc, &argv, NULL);

	ADD("spread", test_spread);
	ADD("spread-sources-and-rates", test_spread_sources_and_rates);
	ADD("filters", test_filters);
	ADD("stale", test_stale);
	ADD("transform", test_transform);
	ADD("deal", test_deal);
	ADD("cover", test_cover);
	ADD("back-lay", test_back_lay);
	ADD("started-events", test_started_events);
	ADD("record-from-plan", test_record_from_plan);
	ADD("export", test_export);
	ADD("export-binary", test_export_binary);
	ADD("presets", test_presets);
	ADD("fee-model-on-venues", test_fee_model_on_venues);
	ADD("plugin-strategy", test_plugin_strategy);
	ADD("modules-off", test_modules_off);
	ADD("pages", test_pages);
	ADD("looks", test_looks);
	ADD("reports", test_reports);
	ADD("widget", test_widget);

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
