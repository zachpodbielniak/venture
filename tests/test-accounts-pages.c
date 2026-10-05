/*
 * test-accounts-pages.c - The account operations pages, their API twins,
 * the reports, the widgets and the valuation behind them
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The operator's accounts arrive the way they do in production: JSON
 * lines pushed to a push source, through the feeds worker, into the
 * source's series store (and, after the run, the mirror). The tests then
 * ask the one implementation (venture-marketdata-accounts.c) with a fixed
 * "now" and compare its figures with what the seed says they must be, to
 * the minor unit; the page tests read what a person sees through a real
 * server on a kernel-chosen port, with authentication off -- test-auth is
 * about who may see these pages, this file about what they say.
 *
 * Money is GOLD, a registered currency with four minor digits and coins,
 * so the figures here are what a game's ledger is: 10.0000 GOLD is
 * 100000 copper.
 */

#include <venture.h>
#include <libsoup/soup.h>
#include <math.h>
#include <string.h>

#include "venture-test-util.h"

#ifdef VENTURE_HAVE_SQLITE

#define ID(record) (venture_entity_get_id(VENTURE_ENTITY(record)))
#define HOUR (G_GINT64_CONSTANT(3600))
#define DAY (G_GINT64_CONSTANT(86400))

static const gchar hostile_name[] = "<img src=x onerror=alert(1)>";
static const gchar hostile_key[] = "Evil/Acct 1";
static const gchar hostile_item[] = "<script>alert(\"pwn\")</script>";

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
} Fixture;

/* --- Helpers ------------------------------------------------------------------ */

static void
save(
	Fixture		*fixture,
	gpointer	 record
){
	g_autoptr(GError) error = NULL;

	if (!venture_database_save(fixture->database, VENTURE_ENTITY(record), NULL, &error))
		g_error("save refused: %s", error->message);
}

static gchar *
iso(gint64 unix_time)
{
	g_autoptr(GDateTime) when = g_date_time_new_from_unix_utc(unix_time);

	return g_date_time_format(when, "%Y-%m-%dT%H:%M:%SZ");
}

static VentureFeedsService *
service_of(Fixture *fixture)
{
	VentureFeedsService *service;

	service = venture_context_get_feeds_service(fixture->context);
	g_assert_nonnull(service);

	return service;
}

/* Lets runs come back until nothing is pending, bounded. */
static void
settle(Fixture *fixture)
{
	VentureFeedsService *service;
	gint64 deadline;

	service = venture_context_get_feeds_service(fixture->context);

	if (NULL == service)
		return;

	deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;

	while (venture_feeds_service_count_pending(service) > 0)
	{
		if (g_get_monotonic_time() > deadline)
			g_error("the feeds worker did not settle within 30 seconds");

		if (!g_main_context_iteration(NULL, FALSE))
			g_usleep(2000);
	}

	while (g_main_context_iteration(NULL, FALSE))
		;
}

/* A push source in @org, GOLD, switched on. */
static gint64
push_source(
	Fixture		*fixture,
	gint64		 org,
	const gchar	*name
){
	g_autoptr(VentureEntity) source = NULL;

	source = VENTURE_ENTITY(venture_data_source_new());
	venture_entity_set_organization_id(source, org);
	g_object_set(source, "name", name, "provider", "push", "schedule", "manual", "currency", "GOLD",
	             "instrument-namespace", "wow-item", "venue-namespace", "wow-realm", NULL);
	save(fixture, source);

	return ID(source);
}

/* Pushes @text to @source and waits, bounded, for its run. */
static void
push(
	Fixture		*fixture,
	gint64		 source,
	const gchar	*text
){
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) run = NULL;
	g_autofree gchar *push_id = NULL;
	gint64 run_id = 0;
	VentureDataSourceRunStatus status;

	body = g_bytes_new(text, strlen(text));
	g_assert_true(venture_feeds_service_push(service_of(fixture), source, body, &push_id, &error));
	g_assert_no_error(error);
	g_assert_true(venture_feeds_service_wait_push(service_of(fixture), push_id, 60, &run_id));
	g_assert_cmpint(run_id, >, 0);
	settle(fixture);

	run = venture_database_get(fixture->database, VENTURE_TYPE_DATA_SOURCE_RUN, run_id, &error);
	g_assert_no_error(error);
	g_object_get(run, "status", &status, NULL);

	if (VENTURE_DATA_SOURCE_RUN_STATUS_FAILED == status)
	{
		g_autofree gchar *message = NULL;

		g_object_get(run, "error", &message, NULL);
		g_error("push failed: %s", message);
	}
}

static void
line(
	GString		*body,
	const gchar	*format,
	...
) G_GNUC_PRINTF(2, 3);

static void
line(
	GString		*body,
	const gchar	*format,
	...
){
	va_list args;

	va_start(args, format);
	g_string_append_vprintf(body, format, args);
	va_end(args);
	g_string_append_c(body, '\n');
}

/*
 * The seed every test reads, relative to @now:
 *
 *   venues realm-a, realm-b (group us) and region-us (the region's own
 *   statistics); ore, herb, a pet and a hostile item;
 *   Drgold on realm-a, Herbz and Oldtimer on realm-b, a shared bank;
 *   positions: Drgold ore 5 @ 9.5 (+2 h, undercut by the 9.0 minimum),
 *   Drgold herb 2 @ 1.0 (expired 3 h ago), Herbz ore 1 @ 15.0 (+48 h);
 *   mail: Drgold 100.0 money (+20 d), Herbz 3 herb (+2 d);
 *   the ledger below, nine rows in the last thirty days and one before.
 */
static void
seed(Fixture *fixture)
{
	g_autoptr(GString) body = g_string_new(NULL);
	gint64 now = fixture->now;
	g_autofree gchar *taken = iso(now - HOUR);
	g_autofree gchar *snap = iso(now - 10 * 60);
	g_autofree gchar *t_soon = iso(now + 2 * HOUR);
	g_autofree gchar *t_gone = iso(now - 3 * HOUR);
	g_autofree gchar *t_later = iso(now + 48 * HOUR);
	g_autofree gchar *t_mail1 = iso(now + 20 * DAY);
	g_autofree gchar *t_mail2 = iso(now + 2 * DAY);
	g_autofree gchar *seen1 = iso(now - DAY);
	g_autofree gchar *seen2 = iso(now - 2 * DAY);
	g_autofree gchar *seen20 = iso(now - 20 * DAY);
	g_autofree gchar *seen_bank = iso(now - HOUR);
	g_autofree gchar *b10 = iso(now - 10 * DAY);
	g_autofree gchar *b2 = iso(now - 2 * DAY);
	g_autofree gchar *b5 = iso(now - 5 * DAY);
	g_autofree gchar *d10 = iso(now - 10 * DAY);
	g_autofree gchar *d5 = iso(now - 5 * DAY);
	g_autofree gchar *d4 = iso(now - 4 * DAY);
	g_autofree gchar *d3 = iso(now - 3 * DAY);
	g_autofree gchar *d2 = iso(now - 2 * DAY);
	g_autofree gchar *d1 = iso(now - DAY);
	g_autofree gchar *d1b = iso(now - DAY + HOUR);
	g_autofree gchar *d40 = iso(now - 40 * DAY);
	g_autofree gchar *escaped_name = NULL;
	g_autofree gchar *escaped_item = NULL;

	escaped_name = g_strescape(hostile_name, NULL);
	escaped_item = g_strescape(hostile_item, NULL);

	line(body, "{\"type\":\"venue\",\"key\":\"realm-a\",\"name\":\"Realm A\",\"group\":\"us\"}");
	line(body, "{\"type\":\"venue\",\"key\":\"realm-b\",\"name\":\"Realm B\",\"group\":\"us\"}");
	line(body, "{\"type\":\"venue\",\"key\":\"region-us\",\"name\":\"US region\",\"group\":\"region\"}");
	line(body, "{\"type\":\"instrument\",\"key\":\"ore\",\"name\":\"Copper Ore\",\"kind\":\"item\","
	           "\"category\":\"Trade Goods/Metal\"}");
	line(body, "{\"type\":\"instrument\",\"key\":\"herb\",\"name\":\"Peacebloom\",\"kind\":\"item\","
	           "\"category\":\"Trade Goods/Herb\"}");
	line(body, "{\"type\":\"instrument\",\"key\":\"pet\",\"name\":\"Fel Kitten\",\"kind\":\"item\","
	           "\"category\":\"Battle Pet\"}");
	line(body, "{\"type\":\"instrument\",\"key\":\"evil\",\"name\":\"%s\",\"kind\":\"item\"}", escaped_item);

	/* Each realm's market, and the region's sale averages. */
	line(body, "{\"type\":\"snapshot\",\"venue\":\"realm-a\",\"taken_at\":\"%s\"}", taken);
	line(body, "{\"type\":\"stat\",\"venue\":\"realm-a\",\"instrument\":\"ore\",\"market\":\"10.0000\","
	           "\"min\":\"9.0000\",\"historical\":\"11.0000\",\"quantity\":100}");
	line(body, "{\"type\":\"stat\",\"venue\":\"realm-a\",\"instrument\":\"herb\",\"market\":\"2.0000\","
	           "\"min\":\"1.5000\",\"quantity\":40}");
	line(body, "{\"type\":\"stat\",\"venue\":\"realm-a\",\"instrument\":\"pet\",\"market\":\"500.0000\","
	           "\"min\":\"450.0000\",\"quantity\":3}");
	line(body, "{\"type\":\"snapshot\",\"venue\":\"realm-b\",\"taken_at\":\"%s\"}", taken);
	line(body, "{\"type\":\"stat\",\"venue\":\"realm-b\",\"instrument\":\"ore\",\"market\":\"12.0000\","
	           "\"min\":\"11.0000\",\"quantity\":50}");
	line(body, "{\"type\":\"stat\",\"venue\":\"realm-b\",\"instrument\":\"herb\",\"market\":\"3.0000\","
	           "\"min\":\"2.5000\",\"quantity\":30}");
	line(body, "{\"type\":\"snapshot\",\"venue\":\"region-us\",\"taken_at\":\"%s\"}", taken);
	line(body, "{\"type\":\"stat\",\"venue\":\"region-us\",\"instrument\":\"ore\",\"sale_avg\":\"8.0000\","
	           "\"sold_per_day\":\"50\"}");
	line(body, "{\"type\":\"stat\",\"venue\":\"region-us\",\"instrument\":\"herb\",\"sale_avg\":\"2.5000\"}");
	line(body, "{\"type\":\"stat\",\"venue\":\"region-us\",\"instrument\":\"pet\",\"sale_avg\":\"480.0000\"}");

	/* The accounts. */
	line(body, "{\"type\":\"account\",\"key\":\"Drgold-A\",\"name\":\"Drgold\",\"kind\":\"character\","
	           "\"group\":\"Realm A\",\"venue\":\"realm-a\",\"last_seen\":\"%s\","
	           "\"attrs\":{\"class\":\"WARRIOR\",\"level\":80}}", seen1);
	line(body, "{\"type\":\"account\",\"key\":\"Herbz-B\",\"name\":\"Herbz\",\"kind\":\"character\","
	           "\"group\":\"Realm B\",\"venue\":\"realm-b\",\"last_seen\":\"%s\","
	           "\"attrs\":{\"class\":\"DRUID\",\"level\":70}}", seen2);
	line(body, "{\"type\":\"account\",\"key\":\"Oldtimer-B\",\"name\":\"Oldtimer\",\"kind\":\"character\","
	           "\"group\":\"Realm B\",\"venue\":\"realm-b\",\"last_seen\":\"%s\"}", seen20);
	line(body, "{\"type\":\"account\",\"key\":\"warbank:ME\",\"name\":\"Warband bank\",\"kind\":\"shared\","
	           "\"last_seen\":\"%s\"}", seen_bank);
	line(body, "{\"type\":\"account\",\"key\":\"%s\",\"name\":\"%s\",\"kind\":\"character\","
	           "\"group\":\"%s\",\"venue\":\"realm-a\",\"last_seen\":\"%s\"}",
	     hostile_key, escaped_name, escaped_name, seen1);

	/* Money over time: history points, not a snapshot. */
	line(body, "{\"type\":\"balance\",\"account\":\"Drgold-A\",\"currency\":\"GOLD\",\"amount\":\"1000.0000\","
	           "\"at\":\"%s\"}", b10);
	line(body, "{\"type\":\"balance\",\"account\":\"Drgold-A\",\"currency\":\"GOLD\",\"amount\":\"1500.0000\","
	           "\"at\":\"%s\"}", b2);
	line(body, "{\"type\":\"balance\",\"account\":\"Herbz-B\",\"currency\":\"GOLD\",\"amount\":\"250.0000\","
	           "\"at\":\"%s\"}", b5);
	line(body, "{\"type\":\"balance\",\"account\":\"warbank:ME\",\"currency\":\"GOLD\",\"amount\":\"40.0000\","
	           "\"at\":\"%s\"}", b5);
	line(body, "{\"type\":\"balance\",\"account\":\"Oldtimer-B\",\"currency\":\"GOLD\",\"amount\":\"0.5000\","
	           "\"at\":\"%s\"}", b10);

	/* What each holds and has up, restated in full. */
	line(body, "{\"type\":\"account_snapshot\",\"account\":\"Drgold-A\",\"at\":\"%s\","
	           "\"covers\":[\"holdings\",\"positions\",\"inbound\"]}", snap);
	line(body, "{\"type\":\"holding\",\"account\":\"Drgold-A\",\"place\":\"bag\",\"instrument\":\"ore\",\"quantity\":10}");
	line(body, "{\"type\":\"holding\",\"account\":\"Drgold-A\",\"place\":\"bank\",\"instrument\":\"herb\",\"quantity\":4}");
	line(body, "{\"type\":\"holding\",\"account\":\"Drgold-A\",\"place\":\"bank\",\"instrument\":\"pet\",\"quantity\":1}");
	line(body, "{\"type\":\"holding\",\"account\":\"Drgold-A\",\"place\":\"bag\",\"instrument\":\"evil\",\"quantity\":1}");
	line(body, "{\"type\":\"holding\",\"account\":\"Drgold-A\",\"place\":\"currency\","
	           "\"instrument\":\"currency:3008\",\"quantity\":100}");
	line(body, "{\"type\":\"position\",\"account\":\"Drgold-A\",\"venue\":\"realm-a\",\"id\":\"p1\","
	           "\"instrument\":\"ore\",\"quantity\":5,\"price\":\"9.5000\",\"expires_at\":\"%s\"}", t_soon);
	line(body, "{\"type\":\"position\",\"account\":\"Drgold-A\",\"venue\":\"realm-a\",\"id\":\"p2\","
	           "\"instrument\":\"herb\",\"quantity\":2,\"price\":\"1.0000\",\"expires_at\":\"%s\"}", t_gone);
	line(body, "{\"type\":\"inbound\",\"account\":\"Drgold-A\",\"id\":\"m1\",\"sender\":\"Auction House\","
	           "\"subject\":\"Auction successful\",\"money\":\"100.0000\",\"expires_at\":\"%s\"}", t_mail1);
	line(body, "{\"type\":\"account_snapshot\",\"account\":\"Herbz-B\",\"at\":\"%s\","
	           "\"covers\":[\"holdings\",\"positions\",\"inbound\"]}", snap);
	line(body, "{\"type\":\"holding\",\"account\":\"Herbz-B\",\"place\":\"bag\",\"instrument\":\"ore\",\"quantity\":5}");
	line(body, "{\"type\":\"holding\",\"account\":\"Herbz-B\",\"place\":\"reagent_bank\",\"instrument\":\"herb\","
	           "\"quantity\":10}");
	line(body, "{\"type\":\"position\",\"account\":\"Herbz-B\",\"venue\":\"realm-b\",\"id\":\"p3\","
	           "\"instrument\":\"ore\",\"quantity\":1,\"price\":\"15.0000\",\"expires_at\":\"%s\"}", t_later);
	line(body, "{\"type\":\"inbound\",\"account\":\"Herbz-B\",\"id\":\"m2\",\"sender\":\"Auction House\","
	           "\"subject\":\"Auction expired\",\"instrument\":\"herb\",\"quantity\":3,\"returned\":true,"
	           "\"expires_at\":\"%s\"}", t_mail2);
	line(body, "{\"type\":\"account_snapshot\",\"account\":\"warbank:ME\",\"at\":\"%s\","
	           "\"covers\":[\"holdings\"]}", snap);
	line(body, "{\"type\":\"holding\",\"account\":\"warbank:ME\",\"place\":\"warbank\",\"instrument\":\"herb\","
	           "\"quantity\":2}");
	line(body, "{\"type\":\"holding\",\"account\":\"%s\",\"place\":\"bag\",\"instrument\":\"evil\",\"quantity\":7}",
	     hostile_key);

	/* The ledger. */
	line(body, "{\"type\":\"txn\",\"id\":\"t1\",\"account\":\"Drgold-A\",\"venue\":\"realm-a\",\"kind\":\"buy\","
	           "\"instrument\":\"ore\",\"quantity\":3,\"amount\":\"10.0000\",\"source\":\"Auction\",\"at\":\"%s\"}", d10);
	line(body, "{\"type\":\"txn\",\"id\":\"t2\",\"account\":\"Drgold-A\",\"venue\":\"realm-a\",\"kind\":\"sale\","
	           "\"instrument\":\"ore\",\"quantity\":1,\"amount\":\"5.0000\",\"source\":\"Auction\",\"at\":\"%s\"}", d5);
	line(body, "{\"type\":\"txn\",\"id\":\"t3\",\"account\":\"Herbz-B\",\"venue\":\"realm-b\",\"kind\":\"sale\","
	           "\"instrument\":\"ore\",\"quantity\":2,\"amount\":\"12.0000\",\"source\":\"Auction\",\"at\":\"%s\"}", d1);
	line(body, "{\"type\":\"txn\",\"id\":\"t4\",\"account\":\"Herbz-B\",\"venue\":\"realm-b\",\"kind\":\"buy\","
	           "\"instrument\":\"ore\",\"quantity\":2,\"amount\":\"8.0000\",\"source\":\"Auction\",\"at\":\"%s\"}", d1b);
	line(body, "{\"type\":\"txn\",\"id\":\"t5\",\"account\":\"Drgold-A\",\"venue\":\"realm-a\",\"kind\":\"sale\","
	           "\"instrument\":\"herb\",\"quantity\":2,\"amount\":\"3.0000\",\"source\":\"Auction\",\"at\":\"%s\"}", d4);
	line(body, "{\"type\":\"txn\",\"id\":\"t6\",\"account\":\"Drgold-A\",\"kind\":\"income\",\"amount\":\"5.0000\","
	           "\"source\":\"Quest\",\"at\":\"%s\"}", d3);
	line(body, "{\"type\":\"txn\",\"id\":\"t7\",\"account\":\"Drgold-A\",\"kind\":\"expense\",\"amount\":\"1.0000\","
	           "\"source\":\"Repair\",\"at\":\"%s\"}", d3);
	line(body, "{\"type\":\"txn\",\"id\":\"t8\",\"account\":\"Drgold-A\",\"venue\":\"realm-a\",\"kind\":\"expired\","
	           "\"instrument\":\"herb\",\"quantity\":1,\"at\":\"%s\"}", d2);
	line(body, "{\"type\":\"txn\",\"id\":\"t9\",\"account\":\"Herbz-B\",\"venue\":\"realm-b\",\"kind\":\"cancelled\","
	           "\"instrument\":\"ore\",\"quantity\":1,\"at\":\"%s\"}", d1);
	line(body, "{\"type\":\"txn\",\"id\":\"t10\",\"account\":\"Drgold-A\",\"venue\":\"realm-a\",\"kind\":\"sale\","
	           "\"instrument\":\"ore\",\"quantity\":1,\"amount\":\"4.0000\",\"source\":\"Auction\",\"at\":\"%s\"}", d40);

	push(fixture, fixture->source_id, body->str);
}

/* A money object's minor units, or G_MININT64 when null. */
static gint64
money_amount(
	JsonObject	*object,
	const gchar	*member
){
	JsonNode *node;

	node = (NULL != object) ? json_object_get_member(object, member) : NULL;

	if ((NULL == node) || !JSON_NODE_HOLDS_OBJECT(node))
		return G_MININT64;

	return json_object_get_int_member(json_node_get_object(node), "amount");
}

/* The first money of an array member, in minor units. */
static gint64
first_amount(
	JsonObject	*object,
	const gchar	*member
){
	JsonArray *amounts;

	amounts = json_object_get_array_member(object, member);

	if ((NULL == amounts) || (0 == json_array_get_length(amounts)))
		return G_MININT64;

	return json_object_get_int_member(json_array_get_object_element(amounts, 0), "amount");
}

static JsonObject *
root_of(JsonNode *node)
{
	g_assert_nonnull(node);

	return json_node_get_object(node);
}

/* The row of @array whose @member is @value. */
static JsonObject *
find_row(
	JsonArray	*array,
	const gchar	*member,
	const gchar	*value
){
	guint i;

	for (i = 0; (NULL != array) && (i < json_array_get_length(array)); i++)
	{
		JsonObject *row = json_array_get_object_element(array, i);

		if (0 == g_strcmp0(json_object_get_string_member_with_default(row, member, NULL), value))
			return row;
	}

	g_error("no row with %s = %s", member, value);
	return NULL;
}

static JsonNode *
overview(
	Fixture		*fixture,
	gint64		 hours,
	gint64		 mail_days,
	gint64		 stale_days,
	const gchar	*sort,
	gboolean	 descending
){
	g_autoptr(GError) error = NULL;
	VentureMarketdataAccountsQuery query;
	JsonNode *answer;

	venture_marketdata_accounts_query_init(&query);
	query.organization_id = fixture->org;
	query.now = fixture->now;
	query.expiring_hours = hours;
	query.mail_days = mail_days;
	query.stale_days = stale_days;
	query.sort = sort;
	query.descending = descending;
	answer = venture_marketdata_accounts(fixture->context, &query, &error);
	g_assert_no_error(error);
	g_assert_nonnull(answer);

	return answer;
}

/* --- HTTP ------------------------------------------------------------------------- */

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
http_get(
	Fixture		 *fixture,
	const gchar	 *path,
	gchar		**out_body
){
	g_autoptr(SoupMessage) message = NULL;
	g_autofree gchar *url = NULL;
	Reply reply;

	memset(&reply, 0, sizeof(reply));
	url = g_strconcat(venture_web_server_get_base_url(fixture->server), path, NULL);
	message = soup_message_new("GET", url);
	g_assert_nonnull(message);
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	soup_session_send_and_read_async(fixture->session, message, G_PRIORITY_DEFAULT, NULL, reply_done, &reply);

	while (!reply.done)
		g_main_context_iteration(NULL, TRUE);

	g_assert_no_error(reply.error);

	if (NULL != out_body)
		*out_body = g_strndup(g_bytes_get_data(reply.body, NULL), g_bytes_get_size(reply.body));

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

	status = http_get(fixture, path, &body);

	if (200 != status)
		g_error("GET %s answered %u: %s", path, status, body);

	return body;
}

static JsonNode *
get_json(
	Fixture		*fixture,
	const gchar	*path,
	guint		 status
){
	g_autofree gchar *body = NULL;
	g_autoptr(GError) error = NULL;
	JsonNode *node;

	g_assert_cmpuint(http_get(fixture, path, &body), ==, status);
	node = json_from_string(body, &error);
	g_assert_no_error(error);
	g_assert_nonnull(node);

	return node;
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

/* --- The CLI ------------------------------------------------------------------------ */

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
 * Runs build/debug/venturectl against the fixture's server and returns
 * what it printed, or NULL when the binary is not built (the suite builds
 * it; a lone test-one may not have). The server answers on this thread's
 * loop, so the wait iterates it.
 */
static gchar *
cli(
	Fixture			*fixture,
	const gchar *const	*args
){
	g_autoptr(GSubprocessLauncher) launcher = NULL;
	g_autoptr(GSubprocess) process = NULL;
	g_autoptr(GPtrArray) argv = NULL;
	g_autoptr(GError) error = NULL;
	CliRun run;
	guint i;

	if (!g_file_test("build/debug/venturectl", G_FILE_TEST_IS_EXECUTABLE))
		return NULL;

	argv = g_ptr_array_new();
	g_ptr_array_add(argv, (gpointer)"build/debug/venturectl");
	g_ptr_array_add(argv, (gpointer)"--server");
	g_ptr_array_add(argv, (gpointer)venture_web_server_get_base_url(fixture->server));
	g_ptr_array_add(argv, (gpointer)"--format");
	g_ptr_array_add(argv, (gpointer)"json");

	for (i = 0; NULL != args[i]; i++)
		g_ptr_array_add(argv, (gpointer)args[i]);

	g_ptr_array_add(argv, NULL);
	launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
	g_subprocess_launcher_unsetenv(launcher, "VENTURE_TOKEN");
	process = g_subprocess_launcher_spawnv(launcher, (const gchar *const *)argv->pdata, &error);
	g_assert_no_error(error);

	memset(&run, 0, sizeof(run));
	g_subprocess_communicate_utf8_async(process, NULL, NULL, cli_done, &run);

	while (!run.done)
		g_main_context_iteration(NULL, TRUE);

	g_assert_no_error(run.error);

	if (!g_subprocess_get_successful(process))
		g_error("venturectl %s failed: %s", args[0], run.err);

	g_free(run.err);

	return run.out;
}

/* --- The fixture ------------------------------------------------------------------- */

static void
fixture_set_up(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) gold = NULL;

	(void)user_data;

	fixture->state_dir = g_dir_make_tmp("venture-accounts-pages-XXXXXX", &error);
	g_assert_no_error(error);

	fixture->config = venture_config_new();
	g_object_set(fixture->config, "state-dir", fixture->state_dir, "feeds-enabled", TRUE,
	             "feeds-run-window-minutes", (gint64)0, "server-bind-address", "127.0.0.1",
	             "server-port", (gint64)0, "security-require-auth", FALSE,
	             "locale-timezone", "UTC", NULL);

	fixture->database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(fixture->database, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	fixture->context = venture_context_new(fixture->config, fixture->database);
	fixture->org = venture_context_get_default_organization_id(fixture->context);
	g_assert_true(venture_database_migrate(fixture->database, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);

	/* GOLD: four minor digits, read as coins. */
	gold = VENTURE_ENTITY(venture_currency_new());
	g_object_set(gold, "code", "GOLD", "name", "Gold", "exponent", (gint64)4,
	             "denominations", "[{\"suffix\":\"g\",\"units\":10000},{\"suffix\":\"s\",\"units\":100},"
	             "{\"suffix\":\"c\",\"units\":1}]", NULL);
	venture_entity_set_organization_id(gold, fixture->org);
	save(fixture, gold);

	fixture->source_id = push_source(fixture, fixture->org, "TSM");
	fixture->now = g_get_real_time() / G_USEC_PER_SEC;

	fixture->server = venture_web_server_new(fixture->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(fixture->server, &error));
	g_assert_no_error(error);
	fixture->session = soup_session_new_with_options("timeout", 60, NULL);
}

static void
fixture_tear_down(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureConfig) everything = NULL;
	g_autoptr(VentureModuleRegistry) registry = NULL;

	(void)user_data;

	settle(fixture);
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

	/* The registries are process-wide: the next test must not inherit
	 * GOLD or a module switched off. */
	venture_currency_clear_registered();
	everything = venture_config_new();
	registry = venture_module_registry_new();
	venture_module_registry_register_builtins(registry);
	venture_module_registry_configure(registry, everything, NULL);
	venture_module_registry_apply(registry, venture_entity_registry_get_default());
}

/* --- The tests ---------------------------------------------------------------------- */

/*
 * Every page is a page before anything arrives: with feeds off "no data
 * sources", with a source that has sent nothing a note saying how
 * accounts arrive, and an account the store has never seen is NOT_FOUND.
 * What breaks if this regresses: the Trading section's first rows answer
 * an install without game data with an error page.
 */
static void
test_empty(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	static const gchar *const pages[] = { "/accounts", "/accounts/inventory", "/accounts/pnl", NULL };
	g_autoptr(JsonNode) answer = NULL;
	g_autofree gchar *body = NULL;
	guint i;

	(void)user_data;

	/* A source that has stored nothing: every page answers, empty. */
	for (i = 0; NULL != pages[i]; i++)
	{
		g_autofree gchar *page = get_page(fixture, pages[i]);

		assert_buttons_named(page, pages[i]);
	}

	answer = get_json(fixture, "/api/v1/accounts", 200);
	g_assert_true(json_object_get_boolean_member(root_of(answer), "available"));
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(root_of(answer), "accounts")), ==, 0);
	g_assert_nonnull(strstr(json_array_get_string_element(json_object_get_array_member(root_of(answer), "notes"), 0),
	                        "No data source has sent any accounts yet"));
	g_clear_pointer(&answer, json_node_unref);

	body = get_page(fixture, "/accounts");
	g_assert_nonnull(strstr(body, "No accounts yet"));
	g_assert_nonnull(strstr(body, "Nothing needs a login."));
	g_clear_pointer(&body, g_free);

	/* An account nobody sent is not found, on the page and its twin. */
	g_assert_cmpuint(http_get(fixture, "/accounts/1/Nobody", NULL), ==, 404);

	/* Feeds off: still a page, saying why there is nothing. */
	g_object_set(fixture->config, "feeds-enabled", FALSE, NULL);
	answer = get_json(fixture, "/api/v1/accounts", 200);
	g_assert_false(json_object_get_boolean_member(root_of(answer), "available"));
	g_clear_pointer(&answer, json_node_unref);
	answer = get_json(fixture, "/api/v1/accounts/inventory", 200);
	g_assert_false(json_object_get_boolean_member(root_of(answer), "available"));
	g_clear_pointer(&answer, json_node_unref);
	answer = get_json(fixture, "/api/v1/accounts/pnl", 200);
	g_assert_false(json_object_get_boolean_member(root_of(answer), "available"));
	g_clear_pointer(&answer, json_node_unref);
	body = get_page(fixture, "/accounts/inventory");
	g_assert_nonnull(strstr(body, "No data sources: market data feeds are off"));
}

/*
 * The overview's figures are the seed's, to the copper: money on hand
 * summed across accounts, what is listed at buyout, what waits in the
 * mail, the thirty days' net (sales and income less purchases and
 * expenses, the row forty days old left out), and the inventory at the
 * conservative basis. What breaks: a headline figure that disagrees with
 * the account rows beneath it, or a ledger row outside the window counted.
 */
static void
test_overview_figures(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(JsonNode) answer = NULL;
	JsonObject *summary;
	JsonObject *drgold;
	JsonArray *accounts;
	JsonArray *trend;

	(void)user_data;

	seed(fixture);
	answer = overview(fixture, 0, 0, 0, NULL, FALSE);
	summary = json_object_get_object_member(root_of(answer), "summary");

	/* 1500 + 250 + 40 + 0.5 */
	g_assert_cmpint(first_amount(summary, "balances"), ==, 17905000);
	g_assert_cmpint(json_object_get_int_member(summary, "accounts"), ==, 5);
	g_assert_cmpint(json_object_get_int_member(summary, "characters"), ==, 4);
	g_assert_cmpint(json_object_get_int_member(summary, "positions"), ==, 3);
	g_assert_cmpint(json_object_get_int_member(summary, "positions_expired"), ==, 1);
	g_assert_cmpint(json_object_get_int_member(summary, "positions_expiring"), ==, 1);
	/* 5 x 9.5 + 2 x 1.0 + 1 x 15.0 */
	g_assert_cmpint(first_amount(summary, "positions_value"), ==, 645000);
	g_assert_cmpint(first_amount(summary, "inbound_money"), ==, 1000000);
	g_assert_cmpint(json_object_get_int_member(summary, "inbound_items"), ==, 1);
	/* 5 + 12 + 3 + 5 - 10 - 8 - 1 */
	g_assert_cmpint(first_amount(summary, "net_30d"), ==, 60000);
	g_assert_cmpint(first_amount(summary, "sales_30d"), ==, 200000);
	g_assert_cmpint(first_amount(summary, "purchases_30d"), ==, 190000);
	/* Conservative: ore 15 x 8.0, herb 4 x 2.0 + 10 x 2.5 + 2 x 2.5, pet
	 * 480.0; the hostile item has no price and counts for nothing. */
	g_assert_cmpint(first_amount(summary, "inventory_value"), ==, 6380000);
	g_assert_cmpint(json_object_get_int_member(summary, "inventory_lines"), ==, 8);
	g_assert_cmpint(json_object_get_int_member(summary, "inventory_priced_lines"), ==, 6);

	accounts = json_object_get_array_member(root_of(answer), "accounts");
	drgold = find_row(accounts, "key", "Drgold-A");
	g_assert_cmpint(money_amount(drgold, "gold"), ==, 15000000);
	g_assert_cmpstr(json_object_get_string_member(drgold, "realm"), ==, "Realm A");
	g_assert_cmpstr(json_object_get_string_member(drgold, "class"), ==, "Warrior");
	g_assert_cmpint(json_object_get_int_member(drgold, "level"), ==, 80);
	g_assert_cmpint(json_object_get_int_member(drgold, "positions"), ==, 2);
	g_assert_cmpint(money_amount(drgold, "positions_value"), ==, 495000);
	g_assert_true(json_object_get_boolean_member(drgold, "needs_login"));

	/* The sparkline: thirty closing balances, the last the newest. */
	trend = json_object_get_array_member(json_object_get_object_member(drgold, "trend"), "values");
	g_assert_cmpuint(json_array_get_length(trend), ==, VENTURE_MARKETDATA_ACCOUNTS_TREND_DAYS);
	g_assert_cmpint(json_array_get_int_element(trend, VENTURE_MARKETDATA_ACCOUNTS_TREND_DAYS - 1), ==,
	                15000000);
	g_assert_cmpint(json_array_get_int_element(trend, VENTURE_MARKETDATA_ACCOUNTS_TREND_DAYS - 5), ==,
	                10000000);
	g_assert_true(JSON_NODE_HOLDS_NULL(json_array_get_element(trend, 0)));

	/* An unknown basis or sort is refused, never ignored. */
	{
		g_autoptr(GError) error = NULL;
		g_autoptr(JsonNode) refused = NULL;
		VentureMarketdataAccountsQuery query;

		venture_marketdata_accounts_query_init(&query);
		query.organization_id = fixture->org;
		query.basis = "cheapest";
		refused = venture_marketdata_accounts(fixture->context, &query, &error);
		g_assert_null(refused);
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
		g_clear_error(&error);
		query.basis = NULL;
		query.sort = "password";
		refused = venture_marketdata_accounts(fixture->context, &query, &error);
		g_assert_null(refused);
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	}
}

/*
 * The attention list: one row per place to log in to, the most urgent
 * first -- a listing already expired before mail that expires in two
 * days -- each threshold moving exactly the reasons it names. What
 * breaks: the realm a person must visit now listed below one that can
 * wait, or a threshold that changes nothing.
 */
static void
test_attention(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(JsonNode) answer = NULL;
	JsonArray *rows;
	JsonObject *first;
	JsonObject *second;
	JsonArray *reasons;
	gboolean stale = FALSE;
	gboolean expiring = FALSE;
	guint i;

	(void)user_data;

	seed(fixture);
	answer = overview(fixture, 0, 0, 0, NULL, FALSE);
	rows = json_object_get_array_member(root_of(answer), "attention");

	/* Realm A (overdue), Realm B (mail in 2 d, Oldtimer stale); the bank
	 * waits for nothing, and the hostile account has nothing to do. */
	g_assert_cmpuint(json_array_get_length(rows), ==, 2);
	first = json_array_get_object_element(rows, 0);
	second = json_array_get_object_element(rows, 1);
	g_assert_cmpstr(json_object_get_string_member(first, "title"), ==, "Log in to Realm A");
	g_assert_cmpstr(json_object_get_string_member(first, "severity"), ==, "overdue");
	g_assert_cmpstr(json_object_get_string_member(second, "title"), ==, "Log in to Realm B");
	g_assert_cmpstr(json_object_get_string_member(second, "severity"), ==, "soon");

	reasons = json_object_get_array_member(first, "reasons");
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(reasons, 0), "kind"), ==,
	                "positions_expired");

	for (i = 0; i < json_array_get_length(reasons); i++)
		expiring = expiring || (0 == g_strcmp0(json_object_get_string_member(
			json_array_get_object_element(reasons, i), "kind"), "positions_expiring"));

	g_assert_true(expiring);
	reasons = json_object_get_array_member(second, "reasons");

	for (i = 0; i < json_array_get_length(reasons); i++)
		stale = stale || (0 == g_strcmp0(json_object_get_string_member(
			json_array_get_object_element(reasons, i), "kind"), "stale"));

	g_assert_true(stale);
	g_clear_pointer(&answer, json_node_unref);

	/* One hour: the listing two hours out is no longer "expiring". */
	answer = overview(fixture, 1, 0, 0, NULL, FALSE);
	reasons = json_object_get_array_member(json_array_get_object_element(
		json_object_get_array_member(root_of(answer), "attention"), 0), "reasons");

	for (i = 0; i < json_array_get_length(reasons); i++)
		g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(reasons, i), "kind"), !=,
		                "positions_expiring");

	g_clear_pointer(&answer, json_node_unref);

	/* One mail day and thirty stale days: Realm B has only an item
	 * waiting, which follows every deadline. */
	answer = overview(fixture, 0, 1, 30, NULL, FALSE);
	rows = json_object_get_array_member(root_of(answer), "attention");
	g_assert_cmpuint(json_array_get_length(rows), ==, 2);
	second = json_array_get_object_element(rows, 1);
	g_assert_cmpstr(json_object_get_string_member(second, "severity"), ==, "waiting");
	reasons = json_object_get_array_member(second, "reasons");
	g_assert_cmpuint(json_array_get_length(reasons), ==, 1);
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(reasons, 0), "kind"), ==,
	                "inbound_waiting");
	g_clear_pointer(&answer, json_node_unref);

	/* The table's orders: richest first, and by name. */
	answer = overview(fixture, 0, 0, 0, "gold", TRUE);
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(
		json_object_get_array_member(root_of(answer), "accounts"), 0), "key"), ==, "Drgold-A");
	g_clear_pointer(&answer, json_node_unref);
	answer = overview(fixture, 0, 0, 0, "name", FALSE);
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(
		json_object_get_array_member(root_of(answer), "accounts"), 1), "key"), ==, "Drgold-A");
}

/*
 * One account in full: listings against the market (the 9.0 minimum
 * undercuts the 9.5 listing, which is 95% of the 10.0 market value),
 * mail, holdings by place valued, the ledger newest first, and the
 * location the mirror made for it. What breaks: an undercut a seller
 * logs in to answer never shown, or a place's total that differs from
 * its lines.
 */
static void
test_account(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(JsonNode) answer = NULL;
	g_autoptr(GError) error = NULL;
	VentureMarketdataAccountQuery query;
	JsonObject *root;
	JsonObject *position;
	JsonObject *place;
	JsonArray *array;

	(void)user_data;

	seed(fixture);
	memset(&query, 0, sizeof(query));
	query.organization_id = fixture->org;
	query.data_source_id = fixture->source_id;
	query.key = "Drgold-A";
	query.now = fixture->now;
	answer = venture_marketdata_account(fixture->context, &query, &error);
	g_assert_no_error(error);
	root = root_of(answer);

	array = json_object_get_array_member(root, "positions");
	g_assert_cmpuint(json_array_get_length(array), ==, 2);
	/* Soonest expiry first: the expired herb, then the ore. */
	position = json_array_get_object_element(array, 0);
	g_assert_cmpstr(json_object_get_string_member(position, "urgency"), ==, "expired");
	position = json_array_get_object_element(array, 1);
	g_assert_cmpstr(json_object_get_string_member(position, "instrument_key"), ==, "ore");
	g_assert_cmpstr(json_object_get_string_member(position, "urgency"), ==, "soon");
	g_assert_true(json_object_get_boolean_member(position, "undercut"));
	g_assert_cmpint(money_amount(position, "venue_min"), ==, 90000);
	g_assert_cmpint(money_amount(position, "total"), ==, 475000);
	g_assert_cmpfloat_with_epsilon(json_object_get_double_member(position, "vs_market_pct"), 95.0, 1e-9);

	array = json_object_get_array_member(root, "inbound");
	g_assert_cmpuint(json_array_get_length(array), ==, 1);
	g_assert_cmpint(money_amount(json_array_get_object_element(array, 0), "money"), ==, 1000000);

	/* Bag: ore 10 x 8.0 and the unpriced hostile item; bank: herb 4 x
	 * 2.0 and the pet at 480.0; currencies listed, never valued. */
	array = json_object_get_array_member(root, "places");
	place = find_row(array, "place", "bag");
	g_assert_cmpint(money_amount(place, "value"), ==, 800000);
	g_assert_cmpint(json_object_get_int_member(place, "unpriced"), ==, 1);
	place = find_row(array, "place", "bank");
	g_assert_cmpint(money_amount(place, "value"), ==, 4880000);
	place = find_row(array, "place", "currency");
	g_assert_true(JSON_NODE_HOLDS_NULL(json_object_get_member(place, "value")));
	g_assert_cmpint(money_amount(root, "holdings_value"), ==, 5680000);

	/* The ledger, newest first. */
	array = json_object_get_array_member(root, "ledger");
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(array, 0), "key"), ==, "t8");

	/* The mirror promoted the account: its location is linked. */
	g_assert_true(JSON_NODE_HOLDS_OBJECT(json_object_get_member(root, "location")));
	g_assert_nonnull(strstr(json_object_get_string_member(root, "listings_url"), "/e/listing?location_id="));

	/* Its balance history starts where the history does. */
	array = json_object_get_array_member(json_object_get_object_member(root, "balance_history"), "points");
	g_assert_cmpint(json_object_get_int_member(json_array_get_object_element(array, 0), "amount"), ==,
	                10000000);
	g_assert_cmpint(json_object_get_int_member(json_array_get_object_element(array,
	                json_array_get_length(array) - 1), "amount"), ==, 15000000);
	g_clear_pointer(&answer, json_node_unref);

	/* Another basis values the same lines differently: min is 9.0. */
	query.basis = "min";
	answer = venture_marketdata_account(fixture->context, &query, &error);
	g_assert_no_error(error);
	place = find_row(json_object_get_array_member(root_of(answer), "places"), "place", "bag");
	g_assert_cmpint(money_amount(place, "value"), ==, 900000);
	g_clear_pointer(&answer, json_node_unref);

	/* An account the source never sent is not found. */
	query.basis = NULL;
	query.key = "Nobody";
	answer = venture_marketdata_account(fixture->context, &query, &error);
	g_assert_null(answer);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
}

/*
 * The inventory across accounts: per item, valued and sorted in the
 * store's SQL, each with its breakdown, its share of the whole and its
 * days to sell at the region's pace; a bound in the source's currency,
 * dead stock, and paging. What breaks: a total that is not the sum of
 * its lines, a sort done after the page was cut, or a bound in another
 * currency silently matching nothing.
 */
static void
test_inventory(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(JsonNode) answer = NULL;
	g_autoptr(GError) error = NULL;
	VentureMarketdataInventoryQuery query;
	JsonObject *root;
	JsonObject *row;
	JsonArray *rows;
	JsonArray *breakdown;

	(void)user_data;

	seed(fixture);
	venture_marketdata_inventory_query_init(&query);
	query.organization_id = fixture->org;
	query.now = fixture->now;
	answer = venture_marketdata_inventory(fixture->context, &query, &error);
	g_assert_no_error(error);
	root = root_of(answer);
	rows = json_object_get_array_member(root, "rows");

	/* Most valuable first: pet 480, ore 120, herb 38, then the item
	 * nobody prices (unknown sorts last). */
	g_assert_cmpuint(json_array_get_length(rows), ==, 4);
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(rows, 0), "instrument_key"),
	                ==, "pet");
	row = json_array_get_object_element(rows, 1);
	g_assert_cmpstr(json_object_get_string_member(row, "instrument_key"), ==, "ore");
	g_assert_cmpint(json_object_get_int_member(row, "quantity"), ==, 15);
	g_assert_cmpint(money_amount(row, "value"), ==, 1200000);
	g_assert_cmpint(money_amount(row, "unit_value"), ==, 80000);
	g_assert_cmpfloat_with_epsilon(json_object_get_double_member(row, "share"), 120.0 / 638.0, 1e-9);
	g_assert_cmpfloat_with_epsilon(json_object_get_double_member(row, "days_of_supply"), 15.0 / 50.0, 1e-9);
	breakdown = json_object_get_array_member(row, "breakdown");
	g_assert_cmpuint(json_array_get_length(breakdown), ==, 2);
	row = json_array_get_object_element(rows, 2);
	g_assert_cmpstr(json_object_get_string_member(row, "instrument_key"), ==, "herb");
	/* The shared bank has no realm: valued on the region's sale average. */
	g_assert_cmpint(money_amount(row, "value"), ==, 380000);
	row = json_array_get_object_element(rows, 3);
	g_assert_cmpstr(json_object_get_string_member(row, "instrument_key"), ==, "evil");
	g_assert_true(JSON_NODE_HOLDS_NULL(json_object_get_member(row, "value")));
	g_assert_cmpint(money_amount(json_object_get_object_member(root, "totals"), "value"), ==, 6380000);
	g_assert_cmpint(money_amount(root, "portfolio_value"), ==, 6380000);
	g_assert_cmpint(json_object_get_int_member(root, "total"), ==, 4);
	g_clear_pointer(&answer, json_node_unref);

	/* By name, a page of one: the third page is the third name (the
	 * hostile item's "<" sorts first). */
	query.sort = "name";
	query.descending = FALSE;
	query.per_page = 1;
	query.page = 3;
	answer = venture_marketdata_inventory(fixture->context, &query, &error);
	g_assert_no_error(error);
	root = root_of(answer);
	g_assert_cmpint(json_object_get_int_member(root, "pages"), ==, 4);
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(
		json_object_get_array_member(root, "rows"), 0), "instrument_key"), ==, "pet");
	g_clear_pointer(&answer, json_node_unref);

	/* A bound in GOLD keeps what is worth at least 100.00 GOLD in total. */
	venture_marketdata_inventory_query_init(&query);
	query.organization_id = fixture->org;
	query.now = fixture->now;
	query.min_value = "100.00 GOLD";
	answer = venture_marketdata_inventory(fixture->context, &query, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(root_of(answer), "rows")), ==, 2);
	g_clear_pointer(&answer, json_node_unref);

	/* One in another currency, or none, is refused. */
	query.min_value = "100.00 USD";
	answer = venture_marketdata_inventory(fixture->context, &query, &error);
	g_assert_null(answer);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);
	query.min_value = "100";
	answer = venture_marketdata_inventory(fixture->context, &query, &error);
	g_assert_null(answer);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);

	/* Dead stock over three days: herb last sold four days ago, the pet
	 * and the hostile item never; ore sold yesterday. */
	query.min_value = NULL;
	query.dead = TRUE;
	query.dead_days = 3;
	answer = venture_marketdata_inventory(fixture->context, &query, &error);
	g_assert_no_error(error);
	rows = json_object_get_array_member(root_of(answer), "rows");
	g_assert_cmpuint(json_array_get_length(rows), ==, 3);
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(rows, 1), "instrument_key"),
	                ==, "herb");
	g_clear_pointer(&answer, json_node_unref);

	/* One account, one place. */
	venture_marketdata_inventory_query_init(&query);
	query.organization_id = fixture->org;
	query.now = fixture->now;
	query.account = "Herbz-B";
	query.place = "reagent_bank";
	answer = venture_marketdata_inventory(fixture->context, &query, &error);
	g_assert_no_error(error);
	rows = json_object_get_array_member(root_of(answer), "rows");
	g_assert_cmpuint(json_array_get_length(rows), ==, 1);
	g_assert_cmpint(money_amount(json_array_get_object_element(rows, 0), "value"), ==, 250000);
	g_clear_pointer(&answer, json_node_unref);

	/* Sorts are an allowlist. */
	query.sort = "rowid";
	answer = venture_marketdata_inventory(fixture->context, &query, &error);
	g_assert_null(answer);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
}

/*
 * The profit and loss is the ledger summed in integers: the window's
 * totals, the same figures by account and by item, and the flips -- the
 * three ore bought for 10.0000 matched to the sales that followed, each
 * sale's share of the cost taken from what is left, so a third of 10.0000
 * and the two thirds after it add back to exactly 10.0000. What breaks:
 * a copper lost or invented in the split, a sale matched to a buy made
 * after it, or the forty-day-old row counted in thirty days.
 */
static void
test_pnl(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(JsonNode) answer = NULL;
	g_autoptr(GError) error = NULL;
	VentureMarketdataPnlQuery query;
	JsonObject *root;
	JsonObject *total;
	JsonObject *flip;
	JsonObject *flips;
	JsonArray *rows;

	(void)user_data;

	seed(fixture);
	venture_marketdata_pnl_query_init(&query);
	query.organization_id = fixture->org;
	query.now = fixture->now;
	query.group_by = "account";
	answer = venture_marketdata_external_pnl(fixture->context, &query, &error);
	g_assert_no_error(error);
	root = root_of(answer);

	total = json_array_get_object_element(json_object_get_array_member(root, "totals"), 0);
	g_assert_cmpint(money_amount(total, "sales_amount"), ==, 200000);
	g_assert_cmpint(money_amount(total, "buys_amount"), ==, 180000);
	g_assert_cmpint(money_amount(total, "income"), ==, 50000);
	g_assert_cmpint(money_amount(total, "expense"), ==, 10000);
	g_assert_cmpint(money_amount(total, "net"), ==, 60000);
	g_assert_cmpint(json_object_get_int_member(total, "sold_units"), ==, 5);
	g_assert_cmpint(json_object_get_int_member(total, "bought_units"), ==, 5);
	g_assert_cmpint(json_object_get_int_member(total, "expired_units"), ==, 1);
	g_assert_cmpint(json_object_get_int_member(total, "cancelled_units"), ==, 1);

	rows = json_object_get_array_member(root, "buckets");
	g_assert_cmpint(money_amount(find_row(rows, "key", "Drgold-A"), "net"), ==, 20000);
	g_assert_cmpint(money_amount(find_row(rows, "key", "Herbz-B"), "net"), ==, 40000);

	/* The best item by cash profit: herb (3.0) before ore (-1.0). */
	rows = json_object_get_array_member(root, "top_items");
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(rows, 0), "instrument_key"), ==,
	                "herb");
	g_assert_cmpint(money_amount(json_array_get_object_element(rows, 1), "profit"), ==, -10000);

	/* Flips: only ore was bought. */
	flips = json_object_get_object_member(root, "flips");
	rows = json_object_get_array_member(flips, "rows");
	g_assert_cmpuint(json_array_get_length(rows), ==, 1);
	flip = json_array_get_object_element(rows, 0);
	g_assert_cmpint(json_object_get_int_member(flip, "matched_units"), ==, 3);
	g_assert_cmpint(money_amount(flip, "cost"), ==, 100000);
	g_assert_cmpint(money_amount(flip, "proceeds"), ==, 170000);
	g_assert_cmpint(money_amount(flip, "profit"), ==, 70000);
	g_assert_cmpint(money_amount(flip, "avg_buy"), ==, 33333);
	g_assert_cmpfloat_with_epsilon(json_object_get_double_member(flip, "roi"), 0.7, 1e-9);
	/* The buy after the last sale is still open, and the accounts hold 15. */
	g_assert_cmpint(json_object_get_int_member(flip, "open_units"), ==, 2);
	g_assert_cmpint(money_amount(flip, "open_cost"), ==, 80000);
	g_assert_cmpint(json_object_get_int_member(flip, "held"), ==, 15);
	g_clear_pointer(&answer, json_node_unref);

	/* By day: the chart's points fill every day from the first to the
	 * last, and sum to the same net. */
	query.group_by = "day";
	answer = venture_marketdata_external_pnl(fixture->context, &query, &error);
	g_assert_no_error(error);
	{
		JsonArray *points = json_object_get_array_member(json_object_get_object_member(root_of(answer), "trend"),
		                                                 "points");
		gint64 sum = 0;
		guint i;

		g_assert_cmpuint(json_array_get_length(points), >=, 10);

		for (i = 0; i < json_array_get_length(points); i++)
			sum += json_object_get_int_member(json_array_get_object_element(points, i), "net");

		g_assert_cmpint(sum, ==, 60000);
	}
	g_clear_pointer(&answer, json_node_unref);

	/* No bounds: the old sale counts, and is unmatched (before the buy). */
	query.since = -1;
	query.until = -1;
	query.group_by = "instrument";
	answer = venture_marketdata_external_pnl(fixture->context, &query, &error);
	g_assert_no_error(error);
	total = json_array_get_object_element(json_object_get_array_member(root_of(answer), "totals"), 0);
	g_assert_cmpint(money_amount(total, "sales_amount"), ==, 240000);
	flip = json_array_get_object_element(json_object_get_array_member(
		json_object_get_object_member(root_of(answer), "flips"), "rows"), 0);
	g_assert_cmpint(json_object_get_int_member(flip, "unmatched_sold_units"), ==, 1);
	g_assert_cmpint(money_amount(flip, "cost"), ==, 100000);
	g_clear_pointer(&answer, json_node_unref);

	/* One account's: the flip is split across accounts, so Drgold's own
	 * matching sees one sale against his buy. */
	query.since = 0;
	query.until = 0;
	query.account = "Drgold-A";
	answer = venture_marketdata_external_pnl(fixture->context, &query, &error);
	g_assert_no_error(error);
	flip = json_array_get_object_element(json_object_get_array_member(
		json_object_get_object_member(root_of(answer), "flips"), "rows"), 0);
	g_assert_cmpint(json_object_get_int_member(flip, "matched_units"), ==, 1);
	g_assert_cmpint(money_amount(flip, "cost"), ==, 33333);
	g_assert_cmpint(json_object_get_int_member(flip, "open_units"), ==, 2);
	g_assert_cmpint(money_amount(flip, "open_cost"), ==, 66667);
	g_clear_pointer(&answer, json_node_unref);

	/* A grouping that does not exist is refused. */
	query.group_by = "hour";
	answer = venture_marketdata_external_pnl(fixture->context, &query, &error);
	g_assert_null(answer);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
}

/*
 * The pages draw what the answers say: the attention list, the account
 * table, an account with a key holding a slash, the inventory and the
 * P&L; hostile names escaped everywhere; every button named; the new
 * classes styled in both looks. What breaks: a character named like a
 * tag running script on the operator's own page, or a page unstyled in
 * the look a person chose.
 */
static void
test_pages(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	static const gchar *const classes[] = {
		".accounts-attention", ".attention-row", ".attention-reasons", ".accounts-places",
		".accounts-place", ".expiry-expired", ".expiry-soon", ".share-bar", ".accounts-controls",
		".accounts-group-row", ".account-gold", ".account-sub", ".row-head", NULL
	};
	g_autofree gchar *industrial = NULL;
	g_autofree gchar *classic = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *escaped = NULL;
	guint i;

	(void)user_data;

	seed(fixture);

	industrial = get_page(fixture, "/accounts?by_realm=1");
	assert_buttons_named(industrial, "/accounts");
	g_assert_nonnull(strstr(industrial, "Log in to Realm A"));
	g_assert_nonnull(strstr(industrial, ">Drgold</a>"));
	g_assert_nonnull(strstr(industrial, ">1500g<"));
	g_assert_null(strstr(industrial, hostile_name));
	g_assert_nonnull(strstr(industrial, "&lt;img src=x onerror=alert(1)&gt;"));
	g_assert_nonnull(strstr(industrial, "class=\"accounts-group-row\""));
	g_assert_nonnull(strstr(industrial, "instrument panel"));

	/* The account whose key holds a slash, by the link the page drew. */
	escaped = g_uri_escape_string(hostile_key, NULL, FALSE);
	path = g_strdup_printf("/accounts/%" G_GINT64_FORMAT "/%s", fixture->source_id, escaped);
	g_assert_nonnull(strstr(industrial, path));
	{
		g_autofree gchar *page = get_page(fixture, path);

		assert_buttons_named(page, path);
		g_assert_null(strstr(page, hostile_name));
		g_assert_null(strstr(page, hostile_item));
		g_assert_nonnull(strstr(page, "&lt;script&gt;"));
	}

	{
		g_autofree gchar *account_path = g_strdup_printf("/accounts/%" G_GINT64_FORMAT "/Drgold-A",
		                                                 fixture->source_id);
		g_autofree gchar *page = get_page(fixture, account_path);

		assert_buttons_named(page, account_path);
		g_assert_nonnull(strstr(page, "undercut"));
		g_assert_nonnull(strstr(page, "expiry-expired"));
		g_assert_nonnull(strstr(page, "chart-line"));
		g_assert_nonnull(strstr(page, "Location record"));
	}

	{
		g_autofree gchar *page = get_page(fixture, "/accounts/inventory?sort=value&dir=desc");

		assert_buttons_named(page, "/accounts/inventory");
		g_assert_nonnull(strstr(page, ">Fel Kitten</a>"));
		g_assert_nonnull(strstr(page, "share-bar"));
		g_assert_null(strstr(page, hostile_item));
	}

	{
		g_autofree gchar *page = get_page(fixture, "/accounts/pnl?period=last_90_days&group_by=week");

		assert_buttons_named(page, "/accounts/pnl");
		g_assert_nonnull(strstr(page, "Flips"));
		g_assert_nonnull(strstr(page, "chart-bar"));
	}

	/* A span typed into the address stays the chosen period: without its
	 * own option the select showed the first preset and the next Show
	 * asked about a different window. */
	{
		g_autofree gchar *page = get_page(fixture, "/accounts/pnl?period=2026-01-01..2026-03-31");

		g_assert_nonnull(strstr(page, "<option value=\"2026-01-01..2026-03-31\" selected>"));
		g_assert_null(strstr(page, "\" selected>last 30 days<"));
	}

	{
		g_autofree gchar *page = get_page(fixture, "/accounts/pnl");

		g_assert_nonnull(strstr(page, "<option value=\"last_30_days\" selected>"));
		g_assert_null(strstr(page, "<option value=\"\""));
	}

	/* A bad question is a 400 page, not a crash. */
	g_assert_cmpuint(http_get(fixture, "/accounts?basis=cheapest", NULL), ==, 400);
	g_assert_cmpuint(http_get(fixture, "/api/v1/accounts/pnl?group_by=hour", NULL), ==, 400);

	/* Both looks carry every class the pages use. */
	g_object_set(fixture->config, "ui-look", "classic", NULL);
	classic = get_page(fixture, "/accounts");
	g_assert_nonnull(strstr(classic, "warm monochrome"));

	for (i = 0; NULL != classes[i]; i++)
	{
		if ((NULL == strstr(industrial, classes[i])) || (NULL == strstr(classic, classes[i])))
			g_error("%s is not styled in both looks", classes[i]);
	}

	/*
	 * The rule that bolds a character who needs a login names the cell
	 * the page draws (a td.row-head, not a th: a rule naming a th matched
	 * nothing), in both looks; and the share bar draws its line as a
	 * border, not a shadow, which neither look uses.
	 */
	g_assert_nonnull(strstr(industrial, "<tr class=\"needs-login\"><td class=\"row-head\">"));

	{
		const gchar *const looks[] = { industrial, classic, NULL };

		for (i = 0; NULL != looks[i]; i++)
		{
			const gchar *rule;
			g_autofree gchar *block = NULL;

			g_assert_nonnull(strstr(looks[i], ".accounts-table tr.needs-login > td.row-head a"));
			rule = strstr(looks[i], ".share-bar {");
			g_assert_nonnull(rule);
			block = g_strndup(rule, (gsize)(strchr(rule, '}') - rule));
			g_assert_null(strstr(block, "box-shadow"));
			g_assert_nonnull(strstr(block, "border: 1px solid var(--border)"));
		}
	}
}

/*
 * A second source with three characters on two realms, valued at market:
 *
 *   venues v-east (group East, ore 10.0), v-west (group West, ore 20.0);
 *   Alpha (East, 30 gold, 2 ore), Delta (West, 20 gold, 5 ore),
 *   Zulu (East, 10 gold, 1 ore);
 *   the last day's ledger: Alpha sells for 7.0, Delta for 50.0, Zulu
 *   buys for 2.0.
 *
 * Every figure differs between the realms, so a figure summed over the
 * whole source cannot pass for one realm's.
 */
static gint64
seed_realms(Fixture *fixture)
{
	g_autoptr(GString) body = g_string_new(NULL);
	g_autofree gchar *taken = iso(fixture->now - HOUR);
	g_autofree gchar *snap = iso(fixture->now - 10 * 60);
	g_autofree gchar *seen = iso(fixture->now - DAY);
	g_autofree gchar *d1 = iso(fixture->now - DAY);
	gint64 source;

	source = push_source(fixture, fixture->org, "Realms");
	line(body, "{\"type\":\"venue\",\"key\":\"v-east\",\"name\":\"East\",\"group\":\"East\"}");
	line(body, "{\"type\":\"venue\",\"key\":\"v-west\",\"name\":\"West\",\"group\":\"West\"}");
	line(body, "{\"type\":\"instrument\",\"key\":\"ore\",\"name\":\"Copper Ore\",\"kind\":\"item\"}");
	line(body, "{\"type\":\"snapshot\",\"venue\":\"v-east\",\"taken_at\":\"%s\"}", taken);
	line(body, "{\"type\":\"stat\",\"venue\":\"v-east\",\"instrument\":\"ore\",\"market\":\"10.0000\","
	           "\"quantity\":100}");
	line(body, "{\"type\":\"snapshot\",\"venue\":\"v-west\",\"taken_at\":\"%s\"}", taken);
	line(body, "{\"type\":\"stat\",\"venue\":\"v-west\",\"instrument\":\"ore\",\"market\":\"20.0000\","
	           "\"quantity\":100}");

	line(body, "{\"type\":\"account\",\"key\":\"Alpha-E\",\"name\":\"Alpha\",\"kind\":\"character\","
	           "\"group\":\"East\",\"venue\":\"v-east\",\"last_seen\":\"%s\"}", seen);
	line(body, "{\"type\":\"account\",\"key\":\"Delta-W\",\"name\":\"Delta\",\"kind\":\"character\","
	           "\"group\":\"West\",\"venue\":\"v-west\",\"last_seen\":\"%s\"}", seen);
	line(body, "{\"type\":\"account\",\"key\":\"Zulu-E\",\"name\":\"Zulu\",\"kind\":\"character\","
	           "\"group\":\"East\",\"venue\":\"v-east\",\"last_seen\":\"%s\"}", seen);
	line(body, "{\"type\":\"balance\",\"account\":\"Alpha-E\",\"currency\":\"GOLD\",\"amount\":\"30.0000\","
	           "\"at\":\"%s\"}", d1);
	line(body, "{\"type\":\"balance\",\"account\":\"Delta-W\",\"currency\":\"GOLD\",\"amount\":\"20.0000\","
	           "\"at\":\"%s\"}", d1);
	line(body, "{\"type\":\"balance\",\"account\":\"Zulu-E\",\"currency\":\"GOLD\",\"amount\":\"10.0000\","
	           "\"at\":\"%s\"}", d1);
	line(body, "{\"type\":\"account_snapshot\",\"account\":\"Alpha-E\",\"at\":\"%s\",\"covers\":[\"holdings\"]}",
	     snap);
	line(body, "{\"type\":\"holding\",\"account\":\"Alpha-E\",\"place\":\"bag\",\"instrument\":\"ore\",\"quantity\":2}");
	line(body, "{\"type\":\"account_snapshot\",\"account\":\"Delta-W\",\"at\":\"%s\",\"covers\":[\"holdings\"]}",
	     snap);
	line(body, "{\"type\":\"holding\",\"account\":\"Delta-W\",\"place\":\"bag\",\"instrument\":\"ore\",\"quantity\":5}");
	line(body, "{\"type\":\"account_snapshot\",\"account\":\"Zulu-E\",\"at\":\"%s\",\"covers\":[\"holdings\"]}",
	     snap);
	line(body, "{\"type\":\"holding\",\"account\":\"Zulu-E\",\"place\":\"bag\",\"instrument\":\"ore\",\"quantity\":1}");
	line(body, "{\"type\":\"txn\",\"id\":\"r1\",\"account\":\"Alpha-E\",\"venue\":\"v-east\",\"kind\":\"sale\","
	           "\"instrument\":\"ore\",\"quantity\":1,\"amount\":\"7.0000\",\"at\":\"%s\"}", d1);
	line(body, "{\"type\":\"txn\",\"id\":\"r2\",\"account\":\"Delta-W\",\"venue\":\"v-west\",\"kind\":\"sale\","
	           "\"instrument\":\"ore\",\"quantity\":1,\"amount\":\"50.0000\",\"at\":\"%s\"}", d1);
	line(body, "{\"type\":\"txn\",\"id\":\"r3\",\"account\":\"Zulu-E\",\"venue\":\"v-east\",\"kind\":\"buy\","
	           "\"instrument\":\"ore\",\"quantity\":1,\"amount\":\"2.0000\",\"at\":\"%s\"}", d1);
	push(fixture, source, body->str);

	return source;
}

/* Every occurrence of @needle in @haystack. */
static guint
count_of(
	const gchar	*haystack,
	const gchar	*needle
){
	const gchar *at = haystack;
	guint count = 0;

	while (NULL != (at = strstr(at, needle)))
	{
		count++;
		at += strlen(needle);
	}

	return count;
}

/*
 * The account table's orders and the realm narrowing, over two realms
 * whose figures all differ. What breaks if this regresses:
 *
 *   - a descending name or realm sort leaving most pairs ascending,
 *     because the string comparison's byte difference (25 for Zulu
 *     against Alpha) was read as "unknown, last either way" and never
 *     turned round;
 *   - grouping by realm under another sort interleaving the realms, so
 *     one realm's header is drawn twice with another's rows between;
 *   - a realm's summary showing the whole source's inventory value and
 *     thirty days' net beside that realm's gold.
 */
static void
test_realms(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(JsonNode) answer = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *page = NULL;
	JsonArray *rows;
	JsonObject *summary;
	gint64 source;

	(void)user_data;

	source = seed_realms(fixture);

	/* Name, descending: Zulu, Delta, Alpha. */
	path = g_strdup_printf("/api/v1/accounts?source=%" G_GINT64_FORMAT "&sort=name&dir=desc", source);
	answer = get_json(fixture, path, 200);
	rows = json_object_get_array_member(root_of(answer), "accounts");
	g_assert_cmpuint(json_array_get_length(rows), ==, 3);
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(rows, 0), "key"), ==, "Zulu-E");
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(rows, 1), "key"), ==, "Delta-W");
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(rows, 2), "key"), ==, "Alpha-E");
	g_clear_pointer(&answer, json_node_unref);
	g_clear_pointer(&path, g_free);

	/* Realm, descending: West before East. */
	path = g_strdup_printf("/api/v1/accounts?source=%" G_GINT64_FORMAT "&sort=realm&dir=desc", source);
	answer = get_json(fixture, path, 200);
	rows = json_object_get_array_member(root_of(answer), "accounts");
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(rows, 0), "key"), ==, "Delta-W");
	g_clear_pointer(&answer, json_node_unref);
	g_clear_pointer(&path, g_free);

	/* Gold, descending, grouped: Alpha (30) and Zulu (10) under East,
	 * Delta (20) under West, one header each. */
	path = g_strdup_printf("/api/v1/accounts?source=%" G_GINT64_FORMAT "&by_realm=1&sort=gold&dir=desc",
	                       source);
	answer = get_json(fixture, path, 200);
	rows = json_object_get_array_member(root_of(answer), "accounts");
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(rows, 0), "key"), ==, "Alpha-E");
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(rows, 1), "key"), ==, "Zulu-E");
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(rows, 2), "key"), ==, "Delta-W");
	g_clear_pointer(&answer, json_node_unref);
	g_clear_pointer(&path, g_free);

	path = g_strdup_printf("/accounts?source=%" G_GINT64_FORMAT "&by_realm=1&sort=gold&dir=desc", source);
	page = get_page(fixture, path);
	g_assert_cmpuint(count_of(page, "class=\"accounts-group-row\""), ==, 2);
	g_assert_cmpuint(count_of(page, ">East</th>"), ==, 1);
	g_assert_cmpuint(count_of(page, ">West</th>"), ==, 1);
	g_clear_pointer(&path, g_free);

	/* East alone: its gold, its bags at market (3 ore at 10.0) and its
	 * thirty days (7.0 sold less 2.0 bought), never the source's. */
	path = g_strdup_printf("/api/v1/accounts?source=%" G_GINT64_FORMAT "&group=East&basis=market", source);
	answer = get_json(fixture, path, 200);
	summary = json_object_get_object_member(root_of(answer), "summary");
	g_assert_cmpint(json_object_get_int_member(summary, "accounts"), ==, 2);
	g_assert_cmpint(first_amount(summary, "balances"), ==, 400000);
	g_assert_cmpint(first_amount(summary, "inventory_value"), ==, 300000);
	g_assert_cmpint(json_object_get_int_member(summary, "inventory_units"), ==, 3);
	g_assert_cmpint(first_amount(summary, "net_30d"), ==, 50000);
	g_assert_cmpint(first_amount(summary, "sales_30d"), ==, 70000);
	g_clear_pointer(&answer, json_node_unref);
	g_clear_pointer(&path, g_free);

	/* The whole source still counts everything: 3 x 10.0 + 5 x 20.0 and
	 * 7.0 + 50.0 - 2.0. */
	path = g_strdup_printf("/api/v1/accounts?source=%" G_GINT64_FORMAT "&basis=market", source);
	answer = get_json(fixture, path, 200);
	summary = json_object_get_object_member(root_of(answer), "summary");
	g_assert_cmpint(first_amount(summary, "inventory_value"), ==, 1300000);
	g_assert_cmpint(first_amount(summary, "net_30d"), ==, 550000);
}

/*
 * The P&L's default window is the window "last_30_days" names: thirty
 * whole days ending with today, every boundary a midnight UTC. A sale at
 * half past midnight on the window's first day counts; one a second
 * before that day does not, under either spelling. What breaks if this
 * regresses: the page labelling its default "last 30 days" while counting
 * now minus thirty times a day, so the same page totals differently with
 * and without ?period=last_30_days, and moves every second.
 */
static void
test_pnl_window(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GString) body = g_string_new(NULL);
	g_autoptr(JsonNode) plain = NULL;
	g_autoptr(JsonNode) named = NULL;
	g_autofree gchar *inside = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *seen = NULL;
	g_autofree gchar *path = NULL;
	JsonObject *total;
	gint64 start;
	gint64 source;

	(void)user_data;

	/* The fixture's zone is UTC, so today is the UTC day. */
	start = (fixture->now / DAY) * DAY + DAY - 30 * DAY;
	inside = iso(start + 30 * 60);
	before = iso(start - 1);
	seen = iso(fixture->now - DAY);

	source = push_source(fixture, fixture->org, "Window");
	line(body, "{\"type\":\"account\",\"key\":\"Edge\",\"name\":\"Edge\",\"kind\":\"character\","
	           "\"last_seen\":\"%s\"}", seen);
	line(body, "{\"type\":\"txn\",\"id\":\"w1\",\"account\":\"Edge\",\"kind\":\"income\","
	           "\"amount\":\"3.0000\",\"at\":\"%s\"}", inside);
	line(body, "{\"type\":\"txn\",\"id\":\"w2\",\"account\":\"Edge\",\"kind\":\"income\","
	           "\"amount\":\"100.0000\",\"at\":\"%s\"}", before);
	push(fixture, source, body->str);

	path = g_strdup_printf("/api/v1/accounts/pnl?source=%" G_GINT64_FORMAT, source);
	plain = get_json(fixture, path, 200);
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/api/v1/accounts/pnl?source=%" G_GINT64_FORMAT "&period=last_30_days", source);
	named = get_json(fixture, path, 200);

	total = json_array_get_object_element(json_object_get_array_member(root_of(named), "totals"), 0);
	g_assert_cmpint(money_amount(total, "net"), ==, 30000);
	total = json_array_get_object_element(json_object_get_array_member(root_of(plain), "totals"), 0);
	g_assert_cmpint(money_amount(total, "net"), ==, 30000);
	g_assert_cmpstr(json_object_get_string_member(root_of(plain), "since"), ==,
	                json_object_get_string_member(root_of(named), "since"));
	g_assert_cmpstr(json_object_get_string_member(root_of(plain), "until"), ==,
	                json_object_get_string_member(root_of(named), "until"));
}

/* The widget scope of the default organization. */
static void
scope_of(
	Fixture			*fixture,
	VentureWidgetScope	*scope,
	gint64			*orgs
){
	orgs[0] = fixture->org;
	memset(scope, 0, sizeof(*scope));
	scope->organization_ids = orgs;
	scope->n_organizations = 1;
}

static VentureDashboardWidget *
widget_of(
	Fixture		*fixture,
	gint64		 dashboard,
	const gchar	*kind,
	const gchar	*options
){
	VentureDashboardWidget *widget;

	widget = venture_dashboard_widget_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(widget), fixture->org);
	g_object_set(widget, "dashboard-id", dashboard, "kind", kind, "options", options, NULL);
	save(fixture, widget);

	return widget;
}

/*
 * The four account cards draw their card and their JSON from one loop:
 * the figures in the data are the figures on the card. Options a card
 * does not read are refused at the save, and the Operations template
 * imports. What breaks: a dashboard that says one thing to the assistant
 * reading its JSON and another to the person looking at it.
 */
static void
test_widgets(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureDashboard) dashboard = NULL;
	g_autoptr(VentureDashboardWidget) attention = NULL;
	g_autoptr(VentureDashboardWidget) summary = NULL;
	g_autoptr(VentureDashboardWidget) holdings = NULL;
	g_autoptr(VentureDashboardWidget) pnl = NULL;
	g_autoptr(VentureDashboardWidget) bad = NULL;
	g_autoptr(VentureWidgetResult) result = NULL;
	g_autoptr(GError) error = NULL;
	VentureWidgetScope scope;
	JsonArray *data;
	JsonObject *object;
	gint64 orgs[1];

	(void)user_data;

	seed(fixture);
	scope_of(fixture, &scope, orgs);
	dashboard = venture_dashboard_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(dashboard), fixture->org);
	g_object_set(dashboard, "name", "Ops", "slug", "ops", NULL);
	save(fixture, dashboard);

	attention = widget_of(fixture, ID(dashboard), "accounts_attention", NULL);
	summary = widget_of(fixture, ID(dashboard), "accounts_summary", "{\"basis\": \"min\"}");
	holdings = widget_of(fixture, ID(dashboard), "holdings_value", NULL);
	pnl = widget_of(fixture, ID(dashboard), "external_pnl", NULL);

	result = venture_dashboard_render_widget(fixture->context, attention, &scope);
	g_assert_null(result->error);
	data = json_node_get_array(result->data);
	g_assert_cmpuint(json_array_get_length(data), ==, 2);
	object = json_array_get_object_element(data, 0);
	g_assert_cmpstr(json_object_get_string_member(object, "title"), ==, "Log in to Realm A");
	g_assert_nonnull(strstr(result->html, "Log in to Realm A"));
	g_assert_nonnull(strstr(result->html, "attention-row is-overdue"));
	g_clear_pointer(&result, venture_widget_result_free);

	result = venture_dashboard_render_widget(fixture->context, summary, &scope);
	g_assert_null(result->error);
	object = json_node_get_object(result->data);
	g_assert_cmpint(first_amount(object, "balances"), ==, 17905000);
	g_assert_nonnull(strstr(result->html, json_object_get_string_member(object, "balances_formatted")));
	g_assert_nonnull(strstr(result->html, json_object_get_string_member(object, "inventory_value_formatted")));
	g_clear_pointer(&result, venture_widget_result_free);

	result = venture_dashboard_render_widget(fixture->context, holdings, &scope);
	g_assert_null(result->error);
	data = json_node_get_array(result->data);
	object = json_array_get_object_element(data, 0);
	g_assert_cmpstr(json_object_get_string_member(object, "key"), ==, "pet");
	g_assert_cmpint(money_amount(object, "value"), ==, 4800000);
	g_assert_nonnull(strstr(result->html, json_object_get_string_member(object, "value_formatted")));
	g_clear_pointer(&result, venture_widget_result_free);

	result = venture_dashboard_render_widget(fixture->context, pnl, &scope);
	g_assert_null(result->error);
	object = json_node_get_object(result->data);
	g_assert_cmpint(money_amount(object, "net"), ==, 60000);
	g_assert_nonnull(strstr(result->html, json_object_get_string_member(object, "net_formatted")));
	g_assert_nonnull(strstr(result->html, "sparkline"));
	g_clear_pointer(&result, venture_widget_result_free);

	/* An option the card does not read, or a basis nobody knows. */
	bad = venture_dashboard_widget_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(bad), fixture->org);
	g_object_set(bad, "dashboard-id", ID(dashboard), "kind", "accounts_summary",
	             "options", "{\"basis\": \"cheapest\"}", NULL);
	g_assert_false(venture_database_save(fixture->database, VENTURE_ENTITY(bad), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	g_object_set(bad, "options", "{\"place\": \"bag\"}", NULL);
	g_assert_false(venture_database_save(fixture->database, VENTURE_ENTITY(bad), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);

	/*
	 * Each number to its own range. A source's id has no ceiling; each
	 * threshold stops where the core's does. What breaks: one shared
	 * 1..3650 refusing a card for source 5000, and saving an
	 * expiring_hours of 721 or a mail_days of 61 that every render then
	 * answers with an error.
	 */
	{
		static const gchar *const refused[] = {
			"{\"data_source_id\": 0}", "{\"expiring_hours\": 721}", "{\"mail_days\": 61}",
			"{\"stale_days\": 3651}", "{\"expiring_hours\": 0}", NULL
		};
		g_autoptr(VentureDashboardWidget) far = NULL;
		g_autoptr(VentureDashboardWidget) edge = NULL;
		guint i;

		far = widget_of(fixture, ID(dashboard), "accounts_summary", "{\"data_source_id\": 5000}");
		edge = widget_of(fixture, ID(dashboard), "accounts_attention",
		                 "{\"expiring_hours\": 720, \"mail_days\": 60, \"stale_days\": 3650}");
		g_clear_pointer(&result, venture_widget_result_free);
		result = venture_dashboard_render_widget(fixture->context, edge, &scope);
		g_assert_null(result->error);
		g_clear_pointer(&result, venture_widget_result_free);

		g_object_set(bad, "kind", "accounts_attention", NULL);

		for (i = 0; NULL != refused[i]; i++)
		{
			g_object_set(bad, "options", refused[i], NULL);
			g_assert_false(venture_database_save(fixture->database, VENTURE_ENTITY(bad), NULL, &error));
			g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
			g_clear_error(&error);
		}
	}

	/* The Operations template imports, and its cards answer. */
	{
		g_autoptr(VentureDashboard) operations = NULL;

		operations = venture_dashboard_create_from_template(fixture->context, "operations", 0, NULL, &error);
		g_assert_no_error(error);
		g_assert_nonnull(operations);
	}
}

/*
 * The reports answer the same figures through the report door the web
 * API, the page, the CLI, MCP and the assistant all use, with the
 * options forwarded; and listing_performance groups by location. What
 * breaks: an option dropped on one door and the report answering a
 * different question there.
 */
static void
test_reports(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	static const gchar *const options[] = {
		"account_key", "place", "expiring_hours", "mail_days", "stale_days", "dead_days", "basis", NULL
	};
	g_autoptr(JsonNode) answer = NULL;
	g_autoptr(VentureMcpCatalog) catalog = NULL;
	g_autoptr(JsonNode) schema = NULL;
	g_autoptr(JsonNode) tools = NULL;
	g_autoptr(GError) error = NULL;
	JsonObject *properties = NULL;
	JsonArray *metrics;
	JsonArray *rows;
	guint i;

	(void)user_data;

	seed(fixture);

	answer = get_json(fixture, "/api/v1/reports/external_pnl?period=last_30_days&group_by=account", 200);
	metrics = json_object_get_array_member(root_of(answer), "metrics");
	g_assert_cmpint(money_amount(find_row(metrics, "key", "net"), "value"), ==, 60000);
	rows = json_object_get_array_member(root_of(answer), "rows");
	g_assert_cmpuint(json_array_get_length(rows), ==, 2);
	g_clear_pointer(&answer, json_node_unref);

	answer = get_json(fixture, "/api/v1/reports/account_holdings?period=all&top=1&sort=value", 200);
	rows = json_object_get_array_member(root_of(answer), "rows");
	g_assert_cmpuint(json_array_get_length(rows), ==, 1);
	g_clear_pointer(&answer, json_node_unref);

	answer = get_json(fixture, "/api/v1/reports/account_holdings?period=all&account_key=Herbz-B&dead_days=3",
	                  200);
	rows = json_object_get_array_member(root_of(answer), "rows");
	g_assert_cmpuint(json_array_get_length(rows), ==, 1);
	g_clear_pointer(&answer, json_node_unref);

	answer = get_json(fixture, "/api/v1/reports/accounts?period=all&stale_days=30&sort=gold", 200);
	metrics = json_object_get_array_member(root_of(answer), "metrics");
	g_assert_cmpint(money_amount(find_row(metrics, "key", "gold"), "value"), ==, 17905000);
	g_clear_pointer(&answer, json_node_unref);

	/* The report page draws its controls and its table. */
	{
		g_autofree gchar *page = get_page(fixture, "/reports/accounts?period=all");

		g_assert_nonnull(strstr(page, "name=\"expiring_hours\""));
		g_assert_nonnull(strstr(page, "Drgold"));
	}

	/* The CLI: its allow-list passes the options, and the verbs ask
	 * the pages' questions. */
	{
		static const gchar *const report[] = {
			"report", "external_pnl", "last_30_days", "group_by=account", "account_key=Herbz-B", NULL
		};
		static const gchar *const attention[] = { "accounts", "attention", "mail_days=1", NULL };
		static const gchar *const inventory[] = {
			"accounts", "inventory", "place=reagent_bank", "sort=value", NULL
		};
		g_autofree gchar *out = cli(fixture, report);

		if (NULL != out)
		{
			g_autofree gchar *second = NULL;
			g_autofree gchar *third = NULL;

			g_assert_nonnull(strstr(out, "Herbz"));
			g_assert_null(strstr(out, "Drgold"));
			second = cli(fixture, attention);
			g_assert_nonnull(strstr(second, "\"mail_days\" : 1"));
			g_assert_nonnull(strstr(second, "Log in to Realm A"));
			third = cli(fixture, inventory);
			g_assert_nonnull(strstr(third, "\"place\" : \"reagent_bank\""));
			g_assert_null(strstr(third, "Fel Kitten"));
		}
	}

	/* MCP: the report tool declares every new option. */
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
}

/*
 * listing_performance gains group_by=location: who posted a listing.
 * What breaks: the per-character sale rate the mirrored listings exist
 * to give being impossible to ask for.
 */
static void
test_listing_performance_location(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) shop = NULL;
	g_autoptr(VentureEntity) venture = NULL;
	g_autoptr(VentureEntity) product = NULL;
	g_autofree gchar *text = NULL;
	g_autoptr(VentureEntity) listing = NULL;
	g_autoptr(VentureMoney) price = NULL;
	g_autoptr(GDateTime) listed = NULL;
	g_autoptr(JsonNode) answer = NULL;
	JsonArray *rows;

	(void)user_data;

	shop = VENTURE_ENTITY(venture_location_new());
	venture_entity_set_organization_id(shop, fixture->org);
	g_object_set(shop, "name", "Shop Front", NULL);
	save(fixture, shop);
	venture = VENTURE_ENTITY(venture_venture_new());
	venture_entity_set_organization_id(venture, fixture->org);
	g_object_set(venture, "name", "Shop", NULL);
	save(fixture, venture);
	product = VENTURE_ENTITY(venture_product_new());
	venture_entity_set_organization_id(product, fixture->org);
	g_object_set(product, "name", "Widget", "venture-id", ID(venture), NULL);
	save(fixture, product);

	price = venture_money_from_string("5.00 USD", NULL, NULL);
	listed = g_date_time_new_now_utc();
	listing = VENTURE_ENTITY(venture_listing_new());
	venture_entity_set_organization_id(listing, fixture->org);
	g_object_set(listing, "product-id", ID(product), "location-id", ID(shop), "channel", "shop",
	             "quantity", (gint64)2, "unit-price", price, "listed-at", listed, NULL);
	save(fixture, listing);

	answer = get_json(fixture, "/api/v1/reports/listing_performance?period=all&group_by=location", 200);
	rows = json_object_get_array_member(root_of(answer), "rows");
	g_assert_cmpuint(json_array_get_length(rows), ==, 1);
	text = json_to_string(answer, FALSE);
	g_assert_nonnull(strstr(text, "Shop Front"));
}

/*
 * Another organization's source is not found through every answer, and
 * the default organization's overview does not count it. What breaks:
 * one legal entity's characters, gold and ledger read by naming a number.
 */
static void
test_organizations(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) other = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) answer = NULL;
	VentureMarketdataAccountsQuery overview_query;
	VentureMarketdataAccountQuery account_query;
	VentureMarketdataInventoryQuery inventory_query;
	VentureMarketdataPnlQuery pnl_query;
	gint64 other_org;
	gint64 other_source;

	(void)user_data;

	seed(fixture);
	other = VENTURE_ENTITY(venture_organization_new());
	g_object_set(other, "name", "Evermoor Trading", "slug", "evermoor", NULL);
	save(fixture, other);
	other_org = ID(other);
	other_source = push_source(fixture, other_org, "Evermoor TSM");

	venture_marketdata_accounts_query_init(&overview_query);
	overview_query.organization_id = other_org;
	overview_query.data_source_id = fixture->source_id;
	answer = venture_marketdata_accounts(fixture->context, &overview_query, &error);
	g_assert_null(answer);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_clear_error(&error);

	/* The other organization's own overview has nothing of ours. */
	overview_query.data_source_id = 0;
	answer = venture_marketdata_accounts(fixture->context, &overview_query, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(root_of(answer), "accounts")), ==, 0);
	g_clear_pointer(&answer, json_node_unref);

	memset(&account_query, 0, sizeof(account_query));
	account_query.organization_id = fixture->org;
	account_query.data_source_id = other_source;
	account_query.key = "Drgold-A";
	answer = venture_marketdata_account(fixture->context, &account_query, &error);
	g_assert_null(answer);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_clear_error(&error);

	venture_marketdata_inventory_query_init(&inventory_query);
	inventory_query.organization_id = other_org;
	inventory_query.data_source_id = fixture->source_id;
	answer = venture_marketdata_inventory(fixture->context, &inventory_query, &error);
	g_assert_null(answer);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_clear_error(&error);

	venture_marketdata_pnl_query_init(&pnl_query);
	pnl_query.organization_id = other_org;
	pnl_query.data_source_id = fixture->source_id;
	answer = venture_marketdata_external_pnl(fixture->context, &pnl_query, &error);
	g_assert_null(answer);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
}

#define ADD(path, func) \
	g_test_add("/accounts-pages/" path, Fixture, NULL, fixture_set_up, func, fixture_tear_down)

gint
main(
	gint	 argc,
	gchar	**argv
){
	g_test_init(&argc, &argv, NULL);

	ADD("empty", test_empty);
	ADD("overview-figures", test_overview_figures);
	ADD("attention", test_attention);
	ADD("account", test_account);
	ADD("inventory", test_inventory);
	ADD("pnl", test_pnl);
	ADD("pages", test_pages);
	ADD("realms", test_realms);
	ADD("pnl-window", test_pnl_window);
	ADD("widgets", test_widgets);
	ADD("reports", test_reports);
	ADD("listing-performance-location", test_listing_performance_location);
	ADD("organizations", test_organizations);

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
