/*
 * odds-api.c - Bookmakers' odds from the-odds-api.com, as a data source
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A crispy plugin, and the reference example of a data source provider
 * written without a GObject: one fetch function handed to
 * venture_func_data_source_provider_new(). (The Blizzard plugin in
 * plugins/blizzard-auctions/ is the other way, a full provider with a
 * freeze step.) Drop it in a plugin directory and the server compiles it
 * on demand; nothing here is built by the Makefile.
 *
 * A data source with provider `odds_api` and settings
 *
 *     units: [soccer_epl, basketball_nba]   # the-odds-api's sport keys
 *     regions: uk                           # whose bookmakers: us, uk, eu, au
 *     requests_per_hour: 20
 *
 * and the API key sealed on its credentials page (`api_key`) fetches, per
 * sport, GET /v4/sports/{sport}/odds?regions=..&markets=h2h&oddsFormat=
 * decimal and stores what came back:
 *
 *   event     -> an instrument of kind `event`, keyed by the event's id,
 *                called "Home vs Away", with commence_time in its attrs;
 *   outcome   -> an instrument of kind `outcome` under its event, keyed
 *                "<event id>:<outcome name>" -- a team, or "Draw";
 *   bookmaker -> a venue of kind `bookmaker`, keyed by the bookmaker's key,
 *                grouped by the regions asked for;
 *   price     -> a back quote in decimal odds at that bookmaker, dated by
 *                its market's last_update.
 *
 * That is exactly the shape the arbitrage `cover` strategy reads: every
 * outcome of one event, each with its best odds anywhere, and a surebet
 * when the implied probabilities sum to less than one.
 *
 * The far end's quota rides on every answer as x-requests-remaining,
 * x-requests-used and x-requests-last; the run records the first two in
 * place of its own budget (the far end's count is the one it enforces).
 *
 * The API key -- read this before deploying. the-odds-api takes its key
 * only in the query string (apiKey=...), never a header. A query string
 * is where keys leak: proxies log it, an HTTP library's debug output
 * prints it, an error that quotes the address carries it. What keeps it
 * here:
 *
 *   - it is a sealed credential (`x-sensitive` in the schema), refused in
 *     the settings YAML, stored encrypted in the integration store;
 *   - the feeds HTTP helper names only the origin in every error it makes,
 *     never the address;
 *   - this script never puts the address, or anything the far end said,
 *     in an error -- only statuses and counts -- and still runs its
 *     messages through venture_feed_request_redact();
 *   - the worker replaces every credential of six bytes or more with
 *     [redacted] in everything it records: errors, notes, names.
 *
 * What none of that covers is the far end and the network between: use
 * https (the default api_base), and do not route the feed through a proxy
 * that logs full URLs.
 */

#include <venture/venture.h>

#include <string.h>

/* Everything below the entry points needs the feeds API, which a build
 * with no SQLite does not have. */
#ifdef VENTURE_HAVE_SQLITE

/* The default origin; a test points api_base at a scripted server. */
#define ODDS_API_DEFAULT_BASE	"https://api.the-odds-api.com"

/* ==========================================================================
 * Reading the answer
 * ========================================================================== */

static const gchar *
odds_text(
	JsonObject	*object,
	const gchar	*member
){
	JsonNode *node;

	node = (NULL != object) ? json_object_get_member(object, member) : NULL;

	return ((NULL != node) && JSON_NODE_HOLDS_VALUE(node) &&
	        (G_TYPE_STRING == json_node_get_value_type(node)))
		? json_node_get_string(node) : NULL;
}

/* An ISO 8601 time with a zone as Unix seconds, or @fallback. */
static gint64
odds_time(
	const gchar	*text,
	gint64		 fallback
){
	g_autoptr(GDateTime) when = NULL;

	when = (NULL != text) ? g_date_time_new_from_iso8601(text, NULL) : NULL;

	return (NULL != when) ? g_date_time_to_unix(when) : fallback;
}

/*
 * A price as scaled decimal odds. The API sends a JSON number; it is spelt
 * back in its shortest exact form and parsed as a decimal, so 2.23 is
 * 2.23 and not the binary double nearest it.
 */
static gboolean
odds_price(
	JsonObject	*outcome,
	gint64		*out_scaled
){
	gchar buffer[G_ASCII_DTOSTR_BUF_SIZE];
	JsonNode *node;
	const gchar *text;

	node = json_object_get_member(outcome, "price");

	if ((NULL == node) || !JSON_NODE_HOLDS_VALUE(node))
		return FALSE;

	if (G_TYPE_DOUBLE == json_node_get_value_type(node))
		text = g_ascii_dtostr(buffer, sizeof(buffer), json_node_get_double(node));
	else if (G_TYPE_INT64 == json_node_get_value_type(node))
	{
		g_snprintf(buffer, sizeof(buffer), "%" G_GINT64_FORMAT, json_node_get_int(node));
		text = buffer;
	}
	else if (G_TYPE_STRING == json_node_get_value_type(node))
		text = json_node_get_string(node);
	else
		return FALSE;

	return venture_feed_decimal_to_odds(text, "decimal", out_scaled, NULL);
}

/*
 * One event: its instrument, its outcomes', each bookmaker as a venue,
 * and a back quote per bookmaker and outcome. Counts what it had to
 * refuse rather than failing the fetch: one malformed price is not a
 * reason to drop a sport.
 */
static void
odds_add_event(
	VentureFeedBatch	*batch,
	JsonObject		*event,
	const gchar		*group,
	gint64			 fetched_at,
	gint64			*refused
){
	g_autoptr(JsonObject) attrs = NULL;
	g_autoptr(JsonNode) attrs_node = NULL;
	g_autofree gchar *attrs_json = NULL;
	g_autofree gchar *name = NULL;
	JsonArray *bookmakers;
	const gchar *event_id;
	const gchar *home;
	const gchar *away;
	const gchar *category;
	guint b;

	event_id = odds_text(event, "id");

	if ((NULL == event_id) || (strlen(event_id) > 200))
	{
		(*refused)++;
		return;
	}

	home = odds_text(event, "home_team");
	away = odds_text(event, "away_team");
	category = (NULL != odds_text(event, "sport_title")) ? odds_text(event, "sport_title")
	                                                     : odds_text(event, "sport_key");
	name = ((NULL != home) && (NULL != away)) ? g_strdup_printf("%s vs %s", home, away)
	                                          : g_strdup((NULL != category) ? category : event_id);

	/* When it starts is what a person and a stale-odds filter both read. */
	attrs = json_object_new();

	if (NULL != odds_text(event, "commence_time"))
		json_object_set_string_member(attrs, "commence_time", odds_text(event, "commence_time"));

	if (NULL != odds_text(event, "sport_key"))
		json_object_set_string_member(attrs, "sport_key", odds_text(event, "sport_key"));

	if (NULL != home)
		json_object_set_string_member(attrs, "home_team", home);

	if (NULL != away)
		json_object_set_string_member(attrs, "away_team", away);

	attrs_node = json_node_new(JSON_NODE_OBJECT);
	json_node_set_object(attrs_node, json_object_ref(attrs));
	attrs_json = json_to_string(attrs_node, FALSE);

	if (!venture_feed_batch_add_instrument(batch, event_id, name, "event", category, NULL,
	                                       attrs_json, NULL))
	{
		(*refused)++;
		return;
	}

	bookmakers = (json_object_has_member(event, "bookmakers") &&
	              JSON_NODE_HOLDS_ARRAY(json_object_get_member(event, "bookmakers")))
		? json_object_get_array_member(event, "bookmakers") : NULL;

	for (b = 0; (NULL != bookmakers) && (b < json_array_get_length(bookmakers)); b++)
	{
		JsonNode *element = json_array_get_element(bookmakers, b);
		JsonObject *bookmaker;
		JsonArray *markets;
		const gchar *venue;
		gint64 bookmaker_time;
		guint m;

		bookmaker = JSON_NODE_HOLDS_OBJECT(element) ? json_node_get_object(element) : NULL;
		venue = odds_text(bookmaker, "key");

		if ((NULL == venue) ||
		    !venture_feed_batch_add_venue(batch, venue, odds_text(bookmaker, "title"), "bookmaker",
		                                  group, NULL, NULL, NULL))
		{
			(*refused)++;
			continue;
		}

		bookmaker_time = odds_time(odds_text(bookmaker, "last_update"), fetched_at);
		markets = (json_object_has_member(bookmaker, "markets") &&
		           JSON_NODE_HOLDS_ARRAY(json_object_get_member(bookmaker, "markets")))
			? json_object_get_array_member(bookmaker, "markets") : NULL;

		for (m = 0; (NULL != markets) && (m < json_array_get_length(markets)); m++)
		{
			JsonNode *market_node = json_array_get_element(markets, m);
			JsonObject *market;
			JsonArray *outcomes;
			gint64 taken_at;
			guint o;

			market = JSON_NODE_HOLDS_OBJECT(market_node) ? json_node_get_object(market_node) : NULL;

			/* Head to head only: a spread or a total is a different
			 * bet on the same event, and mixing them would make a
			 * surebet out of two unrelated prices. */
			if (0 != g_strcmp0(odds_text(market, "key"), "h2h"))
				continue;

			taken_at = odds_time(odds_text(market, "last_update"), bookmaker_time);
			outcomes = (json_object_has_member(market, "outcomes") &&
			            JSON_NODE_HOLDS_ARRAY(json_object_get_member(market, "outcomes")))
				? json_object_get_array_member(market, "outcomes") : NULL;

			for (o = 0; (NULL != outcomes) && (o < json_array_get_length(outcomes)); o++)
			{
				JsonNode *outcome_node = json_array_get_element(outcomes, o);
				g_autofree gchar *outcome_key = NULL;
				JsonObject *outcome;
				const gchar *outcome_name;
				gint64 scaled;

				outcome = JSON_NODE_HOLDS_OBJECT(outcome_node) ? json_node_get_object(outcome_node) : NULL;
				outcome_name = odds_text(outcome, "name");

				if ((NULL == outcome_name) || !odds_price(outcome, &scaled))
				{
					(*refused)++;
					continue;
				}

				outcome_key = g_strdup_printf("%s:%s", event_id, outcome_name);

				/* The same outcome under every bookmaker is one
				 * instrument; adding it again is an upsert. */
				if (!venture_feed_batch_add_instrument(batch, outcome_key, outcome_name, "outcome",
				                                       category, event_id, NULL, NULL) ||
				    !venture_feed_batch_add_quote(batch, venue, outcome_key, VENTURE_SERIES_QUOTE_BACK,
				                                  scaled, NULL, VENTURE_SERIES_NONE, taken_at, NULL))
					(*refused)++;
			}
		}
	}
}

/* ==========================================================================
 * The fetch
 * ========================================================================== */

/*
 * One sport. Runs on a GTask thread (the provider is a function provider),
 * so it may block on the request's HTTP helper; it touches nothing but the
 * request.
 */
static VentureFeedBatch *
odds_api_fetch(
	VentureFeedRequest	 *request,
	gpointer		  user_data,
	GError			**error
){
	g_autoptr(VentureFeedBatch) batch = NULL;
	g_autoptr(VentureFeedHttpResponse) response = NULL;
	g_autoptr(JsonParser) parser = NULL;
	g_autofree gchar *escaped_key = NULL;
	g_autofree gchar *url = NULL;
	JsonObject *settings;
	JsonNode *root;
	const gchar *sport;
	const gchar *regions;
	const gchar *base;
	const gchar *key;
	gint64 refused = 0;
	guint i;

	(void)user_data;

	settings = venture_feed_request_get_settings(request);
	sport = venture_feed_request_get_unit(request);
	regions = venture_json_object_get_string(settings, "regions", "us");
	base = venture_json_object_get_string(settings, "api_base", ODDS_API_DEFAULT_BASE);
	key = venture_feed_request_get_secret(request, "api_key");

	/* What goes into the address unescaped is held to a spelling that
	 * needs no escaping; the key is escaped. */
	if (!g_regex_match_simple("^[a-z0-9_]{1,100}$", sport, 0, 0))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "%s is not a sport key: the units are the-odds-api's sport keys, e.g. soccer_epl",
		            sport);
		return NULL;
	}

	if (!g_regex_match_simple("^[a-z]{2}(,[a-z]{2}){0,7}$", regions, 0, 0))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "regions is a comma list of region codes: us, uk, eu, au");
		return NULL;
	}

	if (!g_regex_match_simple("^https?://[A-Za-z0-9.:\\[\\]-]+/?$", base, 0, 0))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "api_base is an origin, such as https://api.the-odds-api.com");
		return NULL;
	}

	if (venture_string_is_empty(key))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		                    "the-odds-api needs a key: set api_key on the source's credentials page");
		return NULL;
	}

	escaped_key = g_uri_escape_string(key, NULL, FALSE);
	url = g_strdup_printf("%s%sv4/sports/%s/odds?regions=%s&markets=h2h&oddsFormat=decimal"
	                      "&dateFormat=iso&apiKey=%s", base, g_str_has_suffix(base, "/") ? "" : "/",
	                      sport, regions, escaped_key);

	/* The key is in this address, so the address goes nowhere but the
	 * request: every message below names the sport and a status only. */
	response = venture_feed_request_http_send(request, "GET", url, NULL, NULL, NULL, 1,
	                                          VENTURE_FEED_HTTP_ANY_STATUS, error);
	memset(url, 0, strlen(url));
	memset(escaped_key, 0, strlen(escaped_key));

	if (NULL == response)
		return NULL;

	if ((401 == response->status) || (403 == response->status))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_UNAUTHENTICATED,
		            "the-odds-api refused the key (HTTP %u): check api_key on the credentials page",
		            response->status);
		return NULL;
	}

	if (200 != response->status)
	{
		g_set_error(error, VENTURE_FEEDS_ERROR, VENTURE_FEEDS_ERROR_HTTP,
		            "the-odds-api answered HTTP %u for %s", response->status, sport);
		return NULL;
	}

	batch = venture_feed_batch_new();

	/* The far end's own count of the account's requests. */
	{
		const gchar *used = venture_feed_http_response_get_header(response, "x-requests-used");
		const gchar *left = venture_feed_http_response_get_header(response, "x-requests-remaining");
		const gchar *last = venture_feed_http_response_get_header(response, "x-requests-last");
		gint64 used_count;
		gint64 left_count;

		if ((NULL != used) && (NULL != left) &&
		    g_ascii_string_to_signed(used, 10, 0, G_MAXINT64, &used_count, NULL) &&
		    g_ascii_string_to_signed(left, 10, 0, G_MAXINT64, &left_count, NULL))
			venture_feed_batch_set_remote_quota(batch, used_count, left_count);

		if (NULL != last)
		{
			g_autofree gchar *note = NULL;
			gint64 cost;

			if (g_ascii_string_to_signed(last, 10, 0, 1000000, &cost, NULL))
			{
				note = g_strdup_printf("%s: this request cost %" G_GINT64_FORMAT
				                       " of the-odds-api's quota", sport, cost);
				venture_feed_batch_add_note(batch, note);
			}
		}
	}

	parser = json_parser_new();

	if (!json_parser_load_from_data(parser, g_bytes_get_data(response->body, NULL),
	                                (gssize)g_bytes_get_size(response->body), NULL) ||
	    !JSON_NODE_HOLDS_ARRAY(json_parser_get_root(parser)))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_SERIALIZATION,
		            "the-odds-api answered %s with something that is not a list of events", sport);
		return NULL;
	}

	root = json_parser_get_root(parser);

	for (i = 0; i < json_array_get_length(json_node_get_array(root)); i++)
	{
		JsonNode *element = json_array_get_element(json_node_get_array(root), i);

		if (!JSON_NODE_HOLDS_OBJECT(element))
		{
			refused++;
			continue;
		}

		odds_add_event(batch, json_node_get_object(element), regions,
		               venture_feed_request_get_fetched_at(request), &refused);
	}

	if (refused > 0)
	{
		g_autofree gchar *note = NULL;
		g_autofree gchar *redacted = NULL;

		note = g_strdup_printf("%s: %" G_GINT64_FORMAT " events, bookmakers or prices refused as "
		                       "malformed", sport, refused);
		redacted = venture_feed_request_redact(request, note);
		venture_feed_batch_add_note(batch, redacted);
		venture_feed_batch_add_refused(batch, refused);
	}

	return g_steal_pointer(&batch);
}

#endif /* VENTURE_HAVE_SQLITE */

/* ==========================================================================
 * Entry points
 * ========================================================================== */

/*
 * The settings. `units` (the sport keys) and `requests_per_hour` are keys
 * every source shares; api_key is the credential, sealed and refused in
 * the settings YAML.
 */
#ifdef VENTURE_HAVE_SQLITE
static const gchar odds_api_schema[] =
	"{\"type\":\"object\",\"required\":[\"units\"],\"properties\":{"
	"\"units\":{\"type\":\"array\",\"title\":\"Sports\","
	"\"description\":\"the-odds-api's sport keys, one unit each: soccer_epl, basketball_nba\"},"
	"\"regions\":{\"type\":\"string\",\"title\":\"Regions\",\"default\":\"us\","
	"\"description\":\"Whose bookmakers: us, uk, eu, au, comma separated\"},"
	"\"api_base\":{\"type\":\"string\",\"title\":\"API origin override\",\"x-endpoint-override\":true},"
	"\"requests_per_hour\":{\"type\":\"integer\",\"title\":\"Request budget an hour\"},"
	"\"api_key\":{\"type\":\"string\",\"title\":\"API key\",\"x-sensitive\":true}"
	"}}";
#endif

/**
 * venture_plugin_info:
 *
 * Returns: (transfer none): a one-line description
 */
const gchar *
venture_plugin_info(void);

const gchar *
venture_plugin_info(void)
{
	return "Bookmakers' head-to-head odds from the-odds-api.com, as the odds_api data source "
	       "(compiled on demand)";
}

/**
 * venture_plugin_register:
 * @context: the wiring, giving access to every registry
 * @error: (out) (optional): return location for a #GError
 *
 * Registers the `odds_api` provider. A build with no SQLite has no series
 * store to put odds in, and says so.
 *
 * Returns: %TRUE if the plugin registered successfully
 */
gboolean
venture_plugin_register(
	VentureContext	 *context,
	GError		**error
);

gboolean
venture_plugin_register(
	VentureContext	 *context,
	GError		**error
){
#ifdef VENTURE_HAVE_SQLITE
	g_autoptr(VentureDataSourceProvider) provider = NULL;

	provider = venture_func_data_source_provider_new("odds_api", "Bookmakers' odds (the-odds-api.com)",
	                                                 odds_api_schema, odds_api_fetch, NULL, NULL);

	return venture_data_source_provider_registry_add(venture_context_get_data_source_providers(context),
	                                                 provider, error);
#else
	(void)context;

	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_UNSUPPORTED,
	                    "odds_api needs a build with SQLite: odds are kept in a series store");
	return FALSE;
#endif
}
