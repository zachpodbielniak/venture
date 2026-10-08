/*
 * blizzard-provider.c - World of Warcraft auction houses as a data source
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The `blizzard_auctions` provider: the reference example of a native data
 * source provider written as a full GObject, with a freeze step and units
 * of its own. (plugins/scripts/odds-api.c is the other way to write one:
 * a crispy script and venture_func_data_source_provider_new().)
 *
 * What a source of this provider fetches, one unit at a time:
 *
 *   <connected realm id>  that realm's auction house:
 *                         /data/wow/connected-realm/{id}/auctions
 *   commodities           the region-wide commodity market:
 *                         /data/wow/auctions/commodities -- 25 requests
 *                         of budget per call, 304 or not
 *   realm-index           when the settings name no realm: the region's
 *                         connected-realm index, kept so that every realm
 *                         it lists is a unit of its own (see "The realm
 *                         list" below)
 *
 * Each realm is a venue keyed by its connected realm id, grouped by
 * region, so the store's region figures compare realms of one region.
 * Each item (and each variant of one) is an instrument in the `wow-item`
 * namespace. Each auction is a listing with its id, so the store's sale
 * estimate can tell what sold between two hourly snapshots.
 *
 * The threading rules, which are the feeds module's:
 *
 *   - freeze runs on the main thread: it reads the settings and checks the
 *     currency's exponent, and makes the BlizzardFrozen every fetch of the
 *     source shares (its token and name caches);
 *   - a fetch runs on a GTask thread and touches nothing but its
 *     VentureFeedRequest and that frozen state: no database, no
 *     configuration, no currency created. Every request goes through the
 *     request's HTTP helper, so the operator's feeds.allowed_origins, the
 *     body cap, the deadline, the hourly budget and the no-redirect rule
 *     all apply -- including to the token request and to a test server
 *     named in api_base / oauth_base.
 *
 * The OAuth token is the one secret this file handles that the worker
 * does not know about (it knows client_secret, which it redacts). It lives
 * only in the frozen state, under its lock, is wiped when the state is
 * freed, and is never put in an error, a note, a log line or the batch.
 */

#include "blizzard.h"

#include <glib/gstdio.h>

#ifdef VENTURE_HAVE_SQLITE

#include <string.h>

/* ==========================================================================
 * Settings
 * ========================================================================== */

static const gchar *const blizzard_regions[] = { "us", "eu", "kr", "tw", NULL };

/*
 * An origin a setting may override: http or https, a host, an optional
 * port, and nothing else. A path would let a setting point the token
 * request somewhere other than /token; userinfo would be a credential in
 * a setting. The allowlist is still what decides whether it is reachable.
 */
static gchar *
blizzard_parse_origin(
	JsonObject	 *settings,
	const gchar	 *member,
	const gchar	 *fallback,
	GError		**error
){
	g_autoptr(GUri) uri = NULL;
	const gchar *text;
	const gchar *path;
	gchar *origin;

	text = venture_json_object_get_string(settings, member, NULL);

	if (NULL == text)
		return g_strdup(fallback);

	uri = g_uri_parse(text, G_URI_FLAGS_ENCODED, NULL);
	path = (NULL != uri) ? g_uri_get_path(uri) : NULL;

	if ((NULL == uri) || (NULL == g_uri_get_host(uri)) || (NULL != g_uri_get_userinfo(uri)) ||
	    (NULL != g_uri_get_query(uri)) || (NULL != g_uri_get_fragment(uri)) ||
	    ((NULL != path) && ('\0' != path[0]) && (0 != g_strcmp0(path, "/"))) ||
	    ((0 != g_ascii_strcasecmp(g_uri_get_scheme(uri), "https")) &&
	     (0 != g_ascii_strcasecmp(g_uri_get_scheme(uri), "http"))))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "%s is an origin such as https://us.api.blizzard.com: a scheme, "
		            "a host and a port, no path", member);
		return NULL;
	}

	origin = g_strdup(text);

	if (g_str_has_suffix(origin, "/"))
		origin[strlen(origin) - 1] = '\0';

	return origin;
}

/* A namespace or a locale goes into a query string unescaped, so it is
 * held to a spelling that needs no escaping. */
static gboolean
blizzard_parse_word(
	JsonObject	 *settings,
	const gchar	 *member,
	const gchar	 *pattern,
	const gchar	 *fallback,
	gchar		**out,
	GError		**error
){
	const gchar *text;

	text = venture_json_object_get_string(settings, member, fallback);

	/*
	 * Fix: CWE-93 (CRLF injection) -- this value goes into a query
	 * string unescaped (see the comment above). Without
	 * G_REGEX_DOLLAR_ENDONLY, PCRE's "$" matches just before a single
	 * trailing newline as well as at the true end of the string, so
	 * "en_US\n" passed "^[a-z]{2}_[A-Z]{2}$" and the newline rode along
	 * into the address Battle.net's HTTP client sends on the wire.
	 */
	if ((NULL == text) || !g_regex_match_simple(pattern, text, G_REGEX_DOLLAR_ENDONLY, 0))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "%s is not one Battle.net accepts", member);
		return FALSE;
	}

	*out = g_strdup(text);

	return TRUE;
}

/* A boolean setting: true or false, nothing a typo can turn into either. */
static gboolean
blizzard_parse_bool(
	JsonObject	 *settings,
	const gchar	 *member,
	gboolean	  fallback,
	gboolean	 *out,
	GError		**error
){
	JsonNode *node;

	*out = fallback;
	node = json_object_get_member(settings, member);

	if ((NULL == node) || JSON_NODE_HOLDS_NULL(node))
		return TRUE;

	if (!JSON_NODE_HOLDS_VALUE(node) || (G_TYPE_BOOLEAN != json_node_get_value_type(node)))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "%s is true or false", member);
		return FALSE;
	}

	*out = json_node_get_boolean(node);

	return TRUE;
}

static gboolean
blizzard_parse_count(
	JsonObject	 *settings,
	const gchar	 *member,
	gint64		  fallback,
	gint64		  low,
	gint64		  high,
	guint		 *out,
	GError		**error
){
	JsonNode *node;
	gint64 value;

	node = json_object_get_member(settings, member);
	value = fallback;

	if ((NULL != node) && !JSON_NODE_HOLDS_NULL(node))
	{
		if (!JSON_NODE_HOLDS_VALUE(node) || (G_TYPE_INT64 != json_node_get_value_type(node)))
			value = G_MININT64;
		else
			value = json_node_get_int(node);
	}

	if ((value < low) || (value > high))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "%s is a whole number from %" G_GINT64_FORMAT " to %" G_GINT64_FORMAT,
		            member, low, high);
		return FALSE;
	}

	*out = (guint)value;

	return TRUE;
}

/*
 * A list of ids: YAML numbers, or digit strings for an operator who
 * quoted them. Repeats are dropped, order kept.
 */
static GArray *
blizzard_parse_ids(
	JsonObject	 *settings,
	const gchar	 *member,
	guint		  most,
	gboolean	  allow_zero,
	GError		**error
){
	g_autoptr(GArray) ids = NULL;
	JsonNode *node;
	JsonArray *array;
	guint i;
	guint j;

	ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	node = json_object_get_member(settings, member);

	if ((NULL == node) || JSON_NODE_HOLDS_NULL(node))
		return g_steal_pointer(&ids);

	if (!JSON_NODE_HOLDS_ARRAY(node))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "%s is a list of numbers", member);
		return NULL;
	}

	array = json_node_get_array(node);

	if (json_array_get_length(array) > most)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "%s names at most %u", member, most);
		return NULL;
	}

	for (i = 0; i < json_array_get_length(array); i++)
	{
		JsonNode *element = json_array_get_element(array, i);
		gboolean repeated = FALSE;
		gint64 id = -1;

		if (JSON_NODE_HOLDS_VALUE(element) && (G_TYPE_INT64 == json_node_get_value_type(element)))
			id = json_node_get_int(element);
		else if (JSON_NODE_HOLDS_VALUE(element) &&
		         (G_TYPE_STRING == json_node_get_value_type(element)) &&
		         !g_ascii_string_to_signed(json_node_get_string(element), 10, 0, G_MAXINT32, &id, NULL))
			id = -1;

		if ((id < (allow_zero ? 0 : 1)) || (id > G_MAXINT32))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			            "%s: every entry is a %s number", member,
			            allow_zero ? "non-negative" : "positive");
			return NULL;
		}

		for (j = 0; j < ids->len; j++)
			repeated = repeated || (g_array_index(ids, gint64, j) == id);

		if (!repeated)
			g_array_append_val(ids, id);
	}

	return g_steal_pointer(&ids);
}

BlizzardSettings *
blizzard_settings_parse(
	JsonObject	 *settings,
	GError		**error
){
	g_autoptr(BlizzardSettings) self = NULL;
	g_autoptr(JsonObject) empty = NULL;
	g_autofree gchar *api_default = NULL;
	g_autofree gchar *dynamic_default = NULL;
	g_autofree gchar *static_default = NULL;
	const gchar *currency;

	if (NULL == settings)
		settings = empty = json_object_new();

	self = g_new0(BlizzardSettings, 1);

	/* Where: the region decides every default address and namespace. */
	self->region = g_strdup(venture_json_object_get_string(settings, "region", "us"));

	if (!g_strv_contains(blizzard_regions, self->region))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "region is us, eu, kr or tw, not %s", self->region);
		return NULL;
	}

	self->client_id = g_strdup(venture_json_object_get_string(settings, "client_id", NULL));

	if ((NULL != self->client_id) &&
	    ((strlen(self->client_id) > 256) || (NULL != strchr(self->client_id, ':'))))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "client_id is the id Blizzard's developer portal gave the client");
		return NULL;
	}

	api_default = g_strdup_printf("https://%s.api.blizzard.com", self->region);
	dynamic_default = g_strdup_printf("dynamic-%s", self->region);
	static_default = g_strdup_printf("static-%s", self->region);

	self->api_base = blizzard_parse_origin(settings, "api_base", api_default, error);

	if (NULL == self->api_base)
		return NULL;

	self->oauth_base = blizzard_parse_origin(settings, "oauth_base", "https://oauth.battle.net", error);

	if ((NULL == self->oauth_base) ||
	    !blizzard_parse_word(settings, "locale", "^[a-z]{2}_[A-Z]{2}$", "en_US", &self->locale, error) ||
	    !blizzard_parse_word(settings, "dynamic_namespace", "^[a-z0-9][a-z0-9.-]{0,63}$",
	                         dynamic_default, &self->dynamic_namespace, error) ||
	    !blizzard_parse_word(settings, "static_namespace", "^[a-z0-9][a-z0-9.-]{0,63}$",
	                         static_default, &self->static_namespace, error))
		return NULL;

	/* What: the realms, the commodity market, how bids and variants read. */
	self->realm_ids = blizzard_parse_ids(settings, "connected_realm_ids", BLIZZARD_MAX_REALMS, FALSE, error);

	if (NULL == self->realm_ids)
		return NULL;

	if (json_object_has_member(settings, "key_modifier_types"))
	{
		self->key_modifier_types = blizzard_parse_ids(settings, "key_modifier_types", 64, TRUE, error);

		if (NULL == self->key_modifier_types)
			return NULL;
	}

	/* Bid-only auctions are off by default: one cannot be bought now, so
	 * listed at its bid it set the "cheapest" price Deals, Browse and the
	 * arbitrage scan send a buyer to. */
	if (!blizzard_parse_bool(settings, "include_commodities", TRUE, &self->include_commodities, error) ||
	    !blizzard_parse_bool(settings, "include_bid_only", FALSE, &self->include_bid_only, error))
		return NULL;

	/* realms_per_fetch belonged to the every-realm walk, which is gone; a
	 * source that still says it is not refused for it. */
	if (!blizzard_parse_count(settings, "item_names_per_fetch", 100, 0, BLIZZARD_MAX_NAMES_PER_FETCH,
	                          &self->item_names_per_fetch, error) ||
	    !blizzard_parse_count(settings, "realm_index_hours", 24, 0, BLIZZARD_MAX_REALM_INDEX_HOURS,
	                          &self->realm_index_hours, error))
		return NULL;

	/* In what: a code the operator registered, never one made up here. */
	currency = venture_json_object_get_string(settings, "currency", BLIZZARD_DEFAULT_CURRENCY);

	if (!venture_currency_is_valid(currency))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "currency %s is not a currency code", currency);
		return NULL;
	}

	self->currency = g_ascii_strup(currency, -1);

	return g_steal_pointer(&self);
}

void
blizzard_settings_free(BlizzardSettings *settings)
{
	if (NULL == settings)
		return;

	g_free(settings->region);
	g_free(settings->client_id);
	g_free(settings->locale);
	g_free(settings->currency);
	g_free(settings->api_base);
	g_free(settings->oauth_base);
	g_free(settings->dynamic_namespace);
	g_free(settings->static_namespace);
	g_clear_pointer(&settings->realm_ids, g_array_unref);
	g_clear_pointer(&settings->key_modifier_types, g_array_unref);
	g_free(settings);
}

/* ==========================================================================
 * The frozen state
 * ========================================================================== */

void
blizzard_item_free(BlizzardItem *item)
{
	if (NULL == item)
		return;

	g_free(item->name);
	g_free(item->category);
	g_free(item->quality);
	g_free(item->display_json);
	g_free(item);
}

static BlizzardItem *
blizzard_item_copy(const BlizzardItem *item)
{
	BlizzardItem *copy;

	copy = g_new0(BlizzardItem, 1);
	copy->name = g_strdup(item->name);
	copy->category = g_strdup(item->category);
	copy->quality = g_strdup(item->quality);
	copy->vendor_sell = item->vendor_sell;
	copy->level = item->level;
	copy->display_json = g_strdup(item->display_json);
	copy->stale = item->stale;

	return copy;
}

BlizzardFrozen *
blizzard_frozen_new(BlizzardSettings *settings)
{
	BlizzardFrozen *frozen;

	frozen = g_new0(BlizzardFrozen, 1);
	frozen->settings = settings;
	g_mutex_init(&frozen->lock);
	frozen->realm_names = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	frozen->realm_slugs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	frozen->items = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free,
	                                      (GDestroyNotify)blizzard_item_free);

	return frozen;
}

void
blizzard_frozen_free(gpointer data)
{
	BlizzardFrozen *frozen = data;

	if (NULL == frozen)
		return;

	/* A token does not outlive the source in freed memory. */
	if (NULL != frozen->token)
	{
		memset(frozen->token, 0, strlen(frozen->token));
		g_free(frozen->token);
	}

	g_mutex_clear(&frozen->lock);
	g_hash_table_unref(frozen->realm_names);
	g_hash_table_unref(frozen->realm_slugs);
	g_hash_table_unref(frozen->items);
	blizzard_settings_free(frozen->settings);
	g_free(frozen);
}

/* ==========================================================================
 * JSON
 * ========================================================================== */

const gchar *
blizzard_text(
	JsonObject	*object,
	const gchar	*member,
	const gchar	*locale
){
	JsonNode *node;

	node = (NULL != object) ? json_object_get_member(object, member) : NULL;

	if (NULL == node)
		return NULL;

	if (JSON_NODE_HOLDS_VALUE(node) && (G_TYPE_STRING == json_node_get_value_type(node)))
		return json_node_get_string(node);

	/* Asked without a locale, Blizzard answers every one; pick ours. */
	if (JSON_NODE_HOLDS_OBJECT(node) && (NULL != locale))
		return venture_json_object_get_string(json_node_get_object(node), locale, NULL);

	return NULL;
}

gint64
blizzard_int(
	JsonObject	*object,
	const gchar	*member,
	gint64		 fallback
){
	JsonNode *node;

	node = (NULL != object) ? json_object_get_member(object, member) : NULL;

	if ((NULL == node) || !JSON_NODE_HOLDS_VALUE(node) ||
	    (G_TYPE_INT64 != json_node_get_value_type(node)))
		return fallback;

	return json_node_get_int(node);
}

static JsonObject *
blizzard_object(
	JsonObject	*object,
	const gchar	*member
){
	JsonNode *node;

	node = (NULL != object) ? json_object_get_member(object, member) : NULL;

	return ((NULL != node) && JSON_NODE_HOLDS_OBJECT(node)) ? json_node_get_object(node) : NULL;
}

static JsonArray *
blizzard_array(
	JsonObject	*object,
	const gchar	*member
){
	JsonNode *node;

	node = (NULL != object) ? json_object_get_member(object, member) : NULL;

	return ((NULL != node) && JSON_NODE_HOLDS_ARRAY(node)) ? json_node_get_array(node) : NULL;
}

/* ==========================================================================
 * HTTP: the token, and an authenticated GET
 * ========================================================================== */

/* Keeps one source's requests at most 100 a second (Blizzard's limit). */
static void
blizzard_space(BlizzardFrozen *frozen)
{
	gint64 wait;

	g_mutex_lock(&frozen->lock);
	wait = frozen->last_request_us + BLIZZARD_MIN_SPACING_US - g_get_monotonic_time();
	frozen->last_request_us = g_get_monotonic_time() + MAX(wait, 0);
	g_mutex_unlock(&frozen->lock);

	if (wait > 0)
		g_usleep((gulong)wait);
}

static void
blizzard_wipe(gchar *text)
{
	if (NULL != text)
	{
		memset(text, 0, strlen(text));
		g_free(text);
	}
}

/*
 * An access token: the cached one while it has a minute left, otherwise a
 * new one by the client-credentials grant -- POST {oauth_base}/token with
 * grant_type=client_credentials and HTTP Basic client_id:client_secret.
 * @renew drops the cached one first (Blizzard just refused it).
 */
static gchar *
blizzard_token(
	VentureFeedRequest	 *request,
	BlizzardFrozen		 *frozen,
	gboolean		  renew,
	GError			**error
){
	g_autoptr(VentureFeedHttpResponse) response = NULL;
	g_autoptr(GBytes) body = NULL;
	g_autoptr(JsonParser) parser = NULL;
	g_autofree gchar *url = NULL;
	gchar *pair;
	gchar *encoded;
	gchar *authorization;
	const gchar *headers[3];
	const gchar *secret;
	const gchar *token;
	JsonObject *answer;
	gint64 expires_in;
	gint64 now;
	gchar *copy;

	now = g_get_real_time() / G_USEC_PER_SEC;

	g_mutex_lock(&frozen->lock);

	if (renew && (NULL != frozen->token))
	{
		memset(frozen->token, 0, strlen(frozen->token));
		g_clear_pointer(&frozen->token, g_free);
	}

	if ((NULL != frozen->token) && (now < frozen->token_expires - 60))
	{
		copy = g_strdup(frozen->token);
		g_mutex_unlock(&frozen->lock);
		return copy;
	}

	g_mutex_unlock(&frozen->lock);

	secret = venture_feed_request_get_secret(request, "client_secret");

	if (venture_string_is_empty(frozen->settings->client_id) || venture_string_is_empty(secret))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		                    "Battle.net needs a client: set client_id in the source's settings "
		                    "and client_secret on its credentials page");
		return NULL;
	}

	/* The pair, its encoding and the header are wiped as soon as the
	 * request is made: they are the client secret in three spellings. */
	pair = g_strdup_printf("%s:%s", frozen->settings->client_id, secret);
	encoded = g_base64_encode((const guchar *)pair, strlen(pair));
	authorization = g_strconcat("Basic ", encoded, NULL);
	blizzard_wipe(pair);
	blizzard_wipe(encoded);

	headers[0] = "Authorization";
	headers[1] = authorization;
	headers[2] = NULL;
	url = g_strconcat(frozen->settings->oauth_base, "/token", NULL);
	body = g_bytes_new_static("grant_type=client_credentials", strlen("grant_type=client_credentials"));

	blizzard_space(frozen);
	response = venture_feed_request_http_send(request, "POST", url, headers, body,
	                                          "application/x-www-form-urlencoded",
	                                          BLIZZARD_COST_DEFAULT,
	                                          VENTURE_FEED_HTTP_UNCONDITIONAL |
	                                          VENTURE_FEED_HTTP_ANY_STATUS, error);
	blizzard_wipe(authorization);

	if (NULL == response)
		return NULL;

	if ((400 == response->status) || (401 == response->status) || (403 == response->status))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_UNAUTHENTICATED,
		            "Battle.net refused the client credentials (HTTP %u): check client_id in "
		            "the settings and client_secret on the credentials page", response->status);
		return NULL;
	}

	if (200 != response->status)
	{
		g_set_error(error, VENTURE_FEEDS_ERROR, VENTURE_FEEDS_ERROR_HTTP,
		            "The Battle.net token endpoint answered HTTP %u", response->status);
		return NULL;
	}

	parser = json_parser_new();

	if (!json_parser_load_from_data(parser, g_bytes_get_data(response->body, NULL),
	                                (gssize)g_bytes_get_size(response->body), NULL) ||
	    !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_SERIALIZATION,
		                    "The Battle.net token endpoint answered with something that is not JSON");
		return NULL;
	}

	answer = json_node_get_object(json_parser_get_root(parser));
	token = venture_json_object_get_string(answer, "access_token", NULL);
	expires_in = blizzard_int(answer, "expires_in", 3600);

	if (venture_string_is_empty(token))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_UNAUTHENTICATED,
		                    "The Battle.net token endpoint answered without a token");
		return NULL;
	}

	g_mutex_lock(&frozen->lock);
	blizzard_wipe(frozen->token);
	frozen->token = g_strdup(token);
	frozen->token_expires = now + CLAMP(expires_in, 60, 86400 * 7);
	copy = g_strdup(frozen->token);
	g_mutex_unlock(&frozen->lock);

	/* The parsed answer still holds the token; the parser goes with this
	 * frame, and the string with it. */
	return copy;
}

JsonNode *
blizzard_get_json(
	VentureFeedRequest	 *request,
	BlizzardFrozen		 *frozen,
	const gchar		 *url,
	guint			  cost,
	VentureFeedHttpFlags	  flags,
	const gchar *const	 *extra_headers,
	guint			 *out_status,
	gint64			 *out_last_modified,
	GError			**error
){
	guint attempt;

	*out_status = 0;

	if (NULL != out_last_modified)
		*out_last_modified = VENTURE_SERIES_NONE;

	for (attempt = 0; attempt < 2; attempt++)
	{
		g_autoptr(VentureFeedHttpResponse) response = NULL;
		g_autoptr(GPtrArray) headers = NULL;
		g_autoptr(JsonParser) parser = NULL;
		gchar *token;
		gchar *bearer;
		guint i;

		token = blizzard_token(request, frozen, attempt > 0, error);

		if (NULL == token)
			return NULL;

		/* The token goes in a header, never the query string: an address
		 * can end up in an error, a header cannot. */
		bearer = g_strconcat("Bearer ", token, NULL);
		blizzard_wipe(token);
		headers = g_ptr_array_new();
		g_ptr_array_add(headers, (gpointer)"Authorization");
		g_ptr_array_add(headers, bearer);

		for (i = 0; (NULL != extra_headers) && (NULL != extra_headers[i]) &&
		            (NULL != extra_headers[i + 1]); i += 2)
		{
			g_ptr_array_add(headers, (gpointer)extra_headers[i]);
			g_ptr_array_add(headers, (gpointer)extra_headers[i + 1]);
		}

		g_ptr_array_add(headers, NULL);

		blizzard_space(frozen);
		response = venture_feed_request_http_send(request, "GET", url,
		                                          (const gchar *const *)headers->pdata, NULL, NULL,
		                                          cost, flags | VENTURE_FEED_HTTP_ANY_STATUS, error);
		blizzard_wipe(bearer);

		if (NULL == response)
			return NULL;

		/* A cached token Blizzard no longer takes: once more, fresh. */
		if ((401 == response->status) && (0 == attempt))
			continue;

		*out_status = response->status;

		if (NULL != out_last_modified)
			*out_last_modified = response->last_modified;

		if (401 == response->status)
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_UNAUTHENTICATED,
			                    "Battle.net refused a token it had just issued (HTTP 401)");
			return NULL;
		}

		if (200 != response->status)
			return NULL;

		parser = json_parser_new();

		if (!json_parser_load_from_data(parser, g_bytes_get_data(response->body, NULL),
		                                (gssize)g_bytes_get_size(response->body), NULL) ||
		    (NULL == json_parser_get_root(parser)))
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_SERIALIZATION,
			                    "Battle.net answered with something that is not JSON");
			return NULL;
		}

		return json_node_copy(json_parser_get_root(parser));
	}

	return NULL;
}

/* ==========================================================================
 * Items and realms: names, fetched lazily and cached
 * ========================================================================== */

/* ==========================================================================
 * Item display: name colour, icon, tooltip
 * ========================================================================== */

static gchar *blizzard_icon_dir_path = NULL;

void
blizzard_set_icon_dir(const gchar *dir)
{
	g_free(blizzard_icon_dir_path);
	blizzard_icon_dir_path = g_strdup(dir);
}

const gchar *
blizzard_get_icon_dir(void)
{
	return blizzard_icon_dir_path;
}

/* The game's colours for an item's quality, as its name is drawn. */
static const gchar *
blizzard_quality_color(const gchar *quality)
{
	static const struct { const gchar *quality; const gchar *color; } colors[] = {
		{ "POOR", "#9d9d9d" }, { "COMMON", "#ffffff" }, { "UNCOMMON", "#1eff00" },
		{ "RARE", "#0070dd" }, { "EPIC", "#a335ee" }, { "LEGENDARY", "#ff8000" },
		{ "ARTIFACT", "#e6cc80" }, { "HEIRLOOM", "#00ccff" }, { "WOW_TOKEN", "#00ccff" },
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(colors); i++)
		if (0 == g_strcmp0(colors[i].quality, quality))
			return colors[i].color;

	return "#ffffff";
}

/* A {r, g, b} colour object as "#rrggbb"; NULL when there is none. */
static gchar *
blizzard_color(JsonObject *color)
{
	if (NULL == color)
		return NULL;

	return g_strdup_printf("#%02x%02x%02x",
	                       (guint)CLAMP(blizzard_int(color, "r", 255), 0, 255),
	                       (guint)CLAMP(blizzard_int(color, "g", 255), 0, 255),
	                       (guint)CLAMP(blizzard_int(color, "b", 255), 0, 255));
}

/* One tooltip line: text, an optional right-hand column, a colour. */
static void
blizzard_line(
	JsonArray	*lines,
	const gchar	*text,
	const gchar	*right,
	const gchar	*color
){
	JsonObject *line;

	if (venture_string_is_empty(text))
		return;

	line = json_object_new();
	json_object_set_string_member(line, "text", text);
	if (!venture_string_is_empty(right))
		json_object_set_string_member(line, "right", right);
	if (NULL != color)
		json_object_set_string_member(line, "color", color);
	json_array_add_object_element(lines, line);
}

/* A preview member's display_string, or a nested display's. */
static const gchar *
blizzard_display_string(JsonObject *object)
{
	JsonObject *display;

	if (NULL == object)
		return NULL;

	if (json_object_has_member(object, "display_string"))
		return venture_json_object_get_string(object, "display_string", NULL);

	display = blizzard_object(object, "display");
	return (NULL != display) ? venture_json_object_get_string(display, "display_string", NULL) : NULL;
}

/*
 * The icon, cached: Blizzard's file id names the file, so an icon shared
 * by a thousand items is fetched once, and one already on disk is never
 * fetched again -- icons do not change. Written to a temporary name and
 * renamed, so a page never serves half of one. Returns the file id, or 0
 * when there is no icon (no cache directory, no media, a failed fetch;
 * the next lookup of the item tries again).
 */
static gint64
blizzard_cache_icon(
	VentureFeedRequest	*request,
	BlizzardFrozen		*frozen,
	gint64			 item_id
){
	g_autoptr(JsonNode) media = NULL;
	g_autofree gchar *url = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *name = NULL;
	JsonArray *assets;
	const gchar *icon_url = NULL;
	gint64 file_id = 0;
	guint status;
	guint i;

	if (NULL == blizzard_icon_dir_path)
		return 0;

	url = g_strdup_printf("%s/data/wow/media/item/%" G_GINT64_FORMAT "?namespace=%s",
	                      frozen->settings->api_base, item_id, frozen->settings->static_namespace);
	media = blizzard_get_json(request, frozen, url, BLIZZARD_COST_DEFAULT,
	                          VENTURE_FEED_HTTP_UNCONDITIONAL, NULL, &status, NULL, NULL);

	if ((NULL == media) || !JSON_NODE_HOLDS_OBJECT(media))
		return 0;

	assets = blizzard_array(json_node_get_object(media), "assets");

	for (i = 0; (NULL != assets) && (i < json_array_get_length(assets)); i++)
	{
		JsonNode *element = json_array_get_element(assets, i);
		JsonObject *asset;

		if (!JSON_NODE_HOLDS_OBJECT(element))
			continue;

		asset = json_node_get_object(element);

		if (0 == g_strcmp0(venture_json_object_get_string(asset, "key", NULL), "icon"))
		{
			icon_url = venture_json_object_get_string(asset, "value", NULL);
			file_id = blizzard_int(asset, "file_data_id", 0);
		}
	}

	/* The address is held to feeds.allowed_origins by the request, like
	 * every other this source fetches. */
	if ((NULL == icon_url) || (file_id <= 0))
		return 0;

	name = g_strdup_printf("%" G_GINT64_FORMAT ".jpg", file_id);
	path = g_build_filename(blizzard_icon_dir_path, name, NULL);

	if (!g_file_test(path, G_FILE_TEST_EXISTS))
	{
		g_autoptr(VentureFeedHttpResponse) response = NULL;
		g_autofree gchar *partial = g_strconcat(path, ".partial", NULL);
		gsize size = 0;
		gconstpointer data;

		response = venture_feed_request_http_get(request, icon_url, NULL, 1, NULL);

		if ((NULL == response) || (200 != response->status) || (NULL == response->body))
			return 0;

		data = g_bytes_get_data(response->body, &size);

		/* An icon is a few kilobytes; anything else is not one. */
		if ((0 == size) || (size > 512 * 1024) ||
		    !g_file_set_contents(partial, data, (gssize)size, NULL) ||
		    (0 != g_rename(partial, path)))
		{
			g_unlink(partial);
			return 0;
		}
	}

	return file_id;
}

/*
 * The item as VENTURE draws it, as JSON text: {color, icon, lines}. The
 * name's colour is its quality's; the icon is this plugin's cached copy;
 * the lines are the game's tooltip, from the item's preview, in the
 * game's order and colours. VENTURE knows nothing of any of it -- it
 * draws whatever display an instrument's attributes carry.
 */
static gchar *
blizzard_item_display(
	VentureFeedRequest	*request,
	BlizzardFrozen		*frozen,
	gint64			 item_id,
	JsonObject		*item
){
	g_autoptr(JsonObject) display = json_object_new();
	g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
	JsonObject *preview = blizzard_object(item, "preview_item");
	JsonObject *quality = blizzard_object(item, "quality");
	JsonArray *lines = json_array_new();
	gint64 file_id;

	json_object_set_string_member(display, "color",
		blizzard_quality_color(venture_json_object_get_string(quality, "type", NULL)));

	file_id = blizzard_cache_icon(request, frozen, item_id);

	if (file_id > 0)
	{
		g_autofree gchar *icon = g_strdup_printf(BLIZZARD_ICON_ROUTE "%" G_GINT64_FORMAT ".jpg", file_id);

		json_object_set_string_member(display, "icon", icon);
	}

	if (NULL != preview)
	{
		JsonObject *weapon = blizzard_object(preview, "weapon");
		JsonArray *stats = blizzard_array(preview, "stats");
		JsonArray *spells = blizzard_array(preview, "spells");
		JsonObject *requirements = blizzard_object(preview, "requirements");
		JsonObject *sell = blizzard_object(preview, "sell_price");
		const gchar *level = blizzard_display_string(blizzard_object(preview, "level"));
		const gchar *description = venture_json_object_get_string(preview, "description", NULL);
		guint i;

		if (NULL != level)
			blizzard_line(lines, level, NULL, "#ffd100");
		else if (blizzard_int(item, "level", 0) > 0)
		{
			g_autofree gchar *text = g_strdup_printf("Item Level %" G_GINT64_FORMAT,
			                                         blizzard_int(item, "level", 0));

			blizzard_line(lines, text, NULL, "#ffd100");
		}

		blizzard_line(lines, venture_json_object_get_string(blizzard_object(preview, "binding"), "name", NULL),
		              NULL, NULL);
		blizzard_line(lines, venture_json_object_get_string(preview, "unique_equipped", NULL), NULL, NULL);

		/* "One-Hand            Sword", as the game draws it. */
		if (json_object_get_boolean_member_with_default(item, "is_equippable", FALSE) ||
		    (NULL != weapon) || json_object_has_member(preview, "armor"))
			blizzard_line(lines,
			              venture_json_object_get_string(blizzard_object(preview, "inventory_type"), "name", NULL),
			              json_object_get_boolean_member_with_default(preview, "is_subclass_hidden", FALSE)
			              ? NULL
			              : venture_json_object_get_string(blizzard_object(preview, "item_subclass"), "name", NULL),
			              NULL);

		if (NULL != weapon)
		{
			blizzard_line(lines, blizzard_display_string(blizzard_object(weapon, "damage")),
			              blizzard_display_string(blizzard_object(weapon, "attack_speed")), NULL);
			blizzard_line(lines, blizzard_display_string(blizzard_object(weapon, "dps")), NULL, NULL);
		}

		{
			JsonObject *armor = blizzard_object(preview, "armor");
			g_autofree gchar *color = blizzard_color(blizzard_object(blizzard_object(armor, "display"), "color"));

			blizzard_line(lines, blizzard_display_string(armor), NULL, color);
		}

		for (i = 0; (NULL != stats) && (i < json_array_get_length(stats)); i++)
		{
			JsonNode *element = json_array_get_element(stats, i);
			g_autofree gchar *color = NULL;

			if (!JSON_NODE_HOLDS_OBJECT(element))
				continue;

			color = blizzard_color(blizzard_object(blizzard_object(json_node_get_object(element), "display"),
			                                       "color"));
			blizzard_line(lines, blizzard_display_string(json_node_get_object(element)), NULL, color);
		}

		for (i = 0; (NULL != spells) && (i < json_array_get_length(spells)); i++)
		{
			JsonNode *element = json_array_get_element(spells, i);

			if (JSON_NODE_HOLDS_OBJECT(element))
				blizzard_line(lines, venture_json_object_get_string(json_node_get_object(element),
				                                                    "description", NULL),
				              NULL, "#1eff00");
		}

		blizzard_line(lines, blizzard_display_string(blizzard_object(preview, "durability")), NULL, NULL);
		blizzard_line(lines, blizzard_display_string(blizzard_object(requirements, "level")), NULL, NULL);

		if (!venture_string_is_empty(description))
		{
			g_autofree gchar *quoted = g_strdup_printf("\"%s\"", description);

			blizzard_line(lines, quoted, NULL, "#ffd100");
		}

		if (NULL != sell)
		{
			JsonObject *strings = blizzard_object(sell, "display_strings");
			g_autoptr(GString) price = g_string_new(NULL);
			static const gchar *const parts[] = { "gold", "silver", "copper" };
			static const gchar *const units[] = { "g", "s", "c" };
			guint p;

			for (p = 0; (NULL != strings) && (p < G_N_ELEMENTS(parts)); p++)
			{
				const gchar *amount = venture_json_object_get_string(strings, parts[p], NULL);

				if (!venture_string_is_empty(amount) && (0 != g_strcmp0(amount, "0")))
					g_string_append_printf(price, "%s%s%s", (price->len > 0) ? " " : "", amount, units[p]);
			}

			if (price->len > 0)
				blizzard_line(lines, "Sell Price:", price->str, NULL);
		}
	}

	json_object_set_array_member(display, "lines", lines);
	json_node_set_object(node, display);

	return json_to_string(node, FALSE);
}

gboolean
blizzard_fetch_item(
	VentureFeedRequest	 *request,
	BlizzardFrozen		 *frozen,
	gint64			  item_id,
	BlizzardItem		**out_item,
	GError			**error
){
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *url = NULL;
	BlizzardItem *item;
	BlizzardItem *found;
	JsonObject *object;
	const gchar *class_name;
	const gchar *subclass_name;
	guint status;

	*out_item = NULL;

	g_mutex_lock(&frozen->lock);
	found = g_hash_table_lookup(frozen->items, &item_id);
	*out_item = ((NULL != found) && !found->stale) ? blizzard_item_copy(found) : NULL;
	g_mutex_unlock(&frozen->lock);

	if (NULL != *out_item)
		return TRUE;

	url = g_strdup_printf("%s/data/wow/item/%" G_GINT64_FORMAT "?namespace=%s&locale=%s",
	                      frozen->settings->api_base, item_id,
	                      frozen->settings->static_namespace, frozen->settings->locale);
	{
		GError *local_error = NULL;

		node = blizzard_get_json(request, frozen, url, BLIZZARD_COST_DEFAULT,
		                         VENTURE_FEED_HTTP_UNCONDITIONAL, NULL, &status, NULL, &local_error);

		if (NULL != local_error)
		{
			g_propagate_error(error, local_error);
			return FALSE;
		}
	}

	/* A 5xx is "not now": nothing is cached, so the next fetch asks again.
	 * A 404 is "never": cached nameless, so it is not asked for again. */
	if ((NULL == node) && (404 != status))
		return TRUE;

	item = g_new0(BlizzardItem, 1);
	item->vendor_sell = -1;
	object = ((NULL != node) && JSON_NODE_HOLDS_OBJECT(node)) ? json_node_get_object(node) : NULL;

	if (NULL != object)
	{
		item->name = g_strdup(blizzard_text(object, "name", frozen->settings->locale));
		item->vendor_sell = blizzard_int(object, "sell_price", -1);
		item->level = blizzard_int(object, "level", 0);
		item->quality = g_strdup(venture_json_object_get_string(blizzard_object(object, "quality"),
		                                                         "type", NULL));
		class_name = blizzard_text(blizzard_object(object, "item_class"), "name",
		                           frozen->settings->locale);
		subclass_name = blizzard_text(blizzard_object(object, "item_subclass"), "name",
		                              frozen->settings->locale);

		/* A "/" inside a name would read as one more level of the path. */
		if (NULL != class_name)
		{
			g_autofree gchar *outer = g_strdelimit(g_strdup(class_name), "/", '-');
			g_autofree gchar *inner = (NULL != subclass_name)
				? g_strdelimit(g_strdup(subclass_name), "/", '-') : NULL;

			item->category = (NULL != inner) ? g_strconcat(outer, "/", inner, NULL)
			                                 : g_strdup(outer);
		}

		item->display_json = blizzard_item_display(request, frozen, item_id, object);
	}

	g_mutex_lock(&frozen->lock);
	g_hash_table_replace(frozen->items, g_memdup2(&item_id, sizeof(item_id)), item);
	*out_item = blizzard_item_copy(item);
	g_mutex_unlock(&frozen->lock);

	return TRUE;
}

/*
 * A connected realm's name: its realms' names, joined, from
 * /data/wow/connected-realm/{id}. Cached for the frozen state's life; a
 * realm whose name cannot be had is left nameless (the store keeps any
 * name it had), never a failure of the auctions it is attached to.
 */
static gchar *
blizzard_realm_name(
	VentureFeedRequest	*request,
	BlizzardFrozen		*frozen,
	const gchar		*realm_id,
	VentureFeedBatch	*batch
){
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GString) name = NULL;
	g_autofree gchar *url = NULL;
	JsonArray *realms;
	gchar *cached;
	guint status;
	guint i;

	g_mutex_lock(&frozen->lock);
	cached = g_strdup(g_hash_table_lookup(frozen->realm_names, realm_id));
	g_mutex_unlock(&frozen->lock);

	if (NULL != cached)
		return cached;

	url = g_strdup_printf("%s/data/wow/connected-realm/%s?namespace=%s&locale=%s",
	                      frozen->settings->api_base, realm_id,
	                      frozen->settings->dynamic_namespace, frozen->settings->locale);
	node = blizzard_get_json(request, frozen, url, BLIZZARD_COST_DEFAULT,
	                         VENTURE_FEED_HTTP_UNCONDITIONAL, NULL, &status, NULL, &error);

	if ((NULL == node) || !JSON_NODE_HOLDS_OBJECT(node))
	{
		g_autofree gchar *note = g_strdup_printf("connected realm %s: no name (%s)", realm_id,
		                                         (NULL != error) ? error->message : "not found");

		venture_feed_batch_add_note(batch, note);
		return NULL;
	}

	realms = blizzard_array(json_node_get_object(node), "realms");
	name = g_string_new(NULL);

	for (i = 0; (NULL != realms) && (i < json_array_get_length(realms)); i++)
	{
		JsonNode *element = json_array_get_element(realms, i);
		const gchar *one;
		const gchar *slug;

		if (!JSON_NODE_HOLDS_OBJECT(element))
			continue;

		/* Any one realm's slug names the connected realm on sites that
		 * list realms rather than connected realms. */
		slug = blizzard_text(json_node_get_object(element), "slug", NULL);

		if (!venture_string_is_empty(slug))
		{
			g_mutex_lock(&frozen->lock);
			if (!g_hash_table_contains(frozen->realm_slugs, realm_id))
				g_hash_table_replace(frozen->realm_slugs, g_strdup(realm_id), g_strdup(slug));
			g_mutex_unlock(&frozen->lock);
		}

		one = blizzard_text(json_node_get_object(element), "name", frozen->settings->locale);

		if (venture_string_is_empty(one))
			continue;

		if (name->len > 0)
			g_string_append(name, ", ");

		g_string_append(name, one);
	}

	if (0 == name->len)
		return NULL;

	g_mutex_lock(&frozen->lock);
	g_hash_table_replace(frozen->realm_names, g_strdup(realm_id), g_strdup(name->str));
	g_mutex_unlock(&frozen->lock);

	return g_string_free(g_steal_pointer(&name), FALSE);
}

/* ==========================================================================
 * Instrument keys
 *
 * The format, which the `tsm` export and anyone filtering the store reads:
 *
 *     <item id>[:b<bonus>,<bonus>...][:m<type>=<value>,...][:p<species>[.<breed>]]
 *
 *   - <item id>: the item, always, in decimal.
 *   - :b  the auction's bonus_lists, sorted ascending with repeats
 *         dropped. Bonuses are what make one item id several things to
 *         buy -- item level, sockets, a suffix -- and their order in the
 *         API carries no meaning, so sorting makes the key one spelling.
 *   - :m  the modifiers, as type=value pairs sorted by type then value;
 *         only the types listed in key_modifier_types when that setting
 *         is given. Some modifiers (the player level an item dropped at)
 *         split identical goods; narrow the list when they do.
 *   - :p  a caged battle pet's species, and its breed after a point. Level
 *         and quality are left out: they are what a buyer compares within
 *         one species-and-breed, not a different thing.
 *
 * A plain item's key is just its id: "2770". A variant's parent is the
 * plain item's key, so the store can roll variants up under the item.
 * ========================================================================== */

static gint
blizzard_compare_int64(
	gconstpointer	a,
	gconstpointer	b
){
	gint64 x = *(const gint64 *)a;
	gint64 y = *(const gint64 *)b;

	return (x < y) ? -1 : (x > y) ? 1 : 0;
}

typedef struct
{
	gint64	type;
	gint64	value;
} BlizzardModifier;

static gint
blizzard_compare_modifier(
	gconstpointer	a,
	gconstpointer	b
){
	const BlizzardModifier *x = a;
	const BlizzardModifier *y = b;

	if (x->type != y->type)
		return (x->type < y->type) ? -1 : 1;

	return (x->value < y->value) ? -1 : (x->value > y->value) ? 1 : 0;
}

static gboolean
blizzard_modifier_kept(
	const BlizzardSettings	*settings,
	gint64			 type
){
	guint i;

	if (NULL == settings->key_modifier_types)
		return TRUE;

	for (i = 0; i < settings->key_modifier_types->len; i++)
	{
		if (g_array_index(settings->key_modifier_types, gint64, i) == type)
			return TRUE;
	}

	return FALSE;
}

/*
 * One auction's item as a key, and -- for a variant -- the attributes that
 * say what the variant is. Returns NULL for an item with no usable id.
 */
static gchar *
blizzard_item_key(
	JsonObject		 *item,
	const BlizzardSettings	 *settings,
	gint64			 *out_item_id,
	JsonObject		**out_variant
){
	g_autoptr(GString) key = NULL;
	g_autoptr(GArray) bonuses = NULL;
	g_autoptr(GArray) modifiers = NULL;
	g_autoptr(JsonObject) variant = NULL;
	JsonArray *list;
	gint64 item_id;
	gint64 species;
	gint64 breed;
	guint i;

	*out_variant = NULL;
	item_id = blizzard_int(item, "id", -1);

	if (item_id <= 0)
		return NULL;

	*out_item_id = item_id;
	key = g_string_new(NULL);
	g_string_append_printf(key, "%" G_GINT64_FORMAT, item_id);
	variant = json_object_new();

	bonuses = g_array_new(FALSE, FALSE, sizeof(gint64));
	list = blizzard_array(item, "bonus_lists");

	for (i = 0; (NULL != list) && (i < json_array_get_length(list)); i++)
	{
		JsonNode *element = json_array_get_element(list, i);
		gint64 bonus;

		if (!JSON_NODE_HOLDS_VALUE(element) || (G_TYPE_INT64 != json_node_get_value_type(element)))
			continue;

		bonus = json_node_get_int(element);
		g_array_append_val(bonuses, bonus);
	}

	g_array_sort(bonuses, blizzard_compare_int64);

	if (bonuses->len > 0)
	{
		JsonArray *kept = json_array_new();
		gint64 previous = G_MININT64;

		g_string_append(key, ":b");

		for (i = 0; i < bonuses->len; i++)
		{
			gint64 bonus = g_array_index(bonuses, gint64, i);

			if (bonus == previous)
				continue;

			g_string_append_printf(key, "%s%" G_GINT64_FORMAT, (G_MININT64 == previous) ? "" : ",",
			                       bonus);
			json_array_add_int_element(kept, bonus);
			previous = bonus;
		}

		json_object_set_array_member(variant, "bonus_lists", kept);
	}

	modifiers = g_array_new(FALSE, FALSE, sizeof(BlizzardModifier));
	list = blizzard_array(item, "modifiers");

	for (i = 0; (NULL != list) && (i < json_array_get_length(list)); i++)
	{
		JsonNode *element = json_array_get_element(list, i);
		BlizzardModifier modifier;

		if (!JSON_NODE_HOLDS_OBJECT(element))
			continue;

		modifier.type = blizzard_int(json_node_get_object(element), "type", -1);
		modifier.value = blizzard_int(json_node_get_object(element), "value", G_MININT64);

		if ((modifier.type < 0) || (G_MININT64 == modifier.value) ||
		    !blizzard_modifier_kept(settings, modifier.type))
			continue;

		g_array_append_val(modifiers, modifier);
	}

	g_array_sort(modifiers, blizzard_compare_modifier);

	if (modifiers->len > 0)
	{
		JsonArray *kept = json_array_new();

		g_string_append(key, ":m");

		for (i = 0; i < modifiers->len; i++)
		{
			BlizzardModifier *modifier = &g_array_index(modifiers, BlizzardModifier, i);
			JsonObject *pair = json_object_new();

			g_string_append_printf(key, "%s%" G_GINT64_FORMAT "=%" G_GINT64_FORMAT,
			                       (0 == i) ? "" : ",", modifier->type, modifier->value);
			json_object_set_int_member(pair, "type", modifier->type);
			json_object_set_int_member(pair, "value", modifier->value);
			json_array_add_object_element(kept, pair);
		}

		json_object_set_array_member(variant, "modifiers", kept);
	}

	species = blizzard_int(item, "pet_species_id", 0);
	breed = blizzard_int(item, "pet_breed_id", 0);

	if (species > 0)
	{
		g_string_append_printf(key, ":p%" G_GINT64_FORMAT, species);
		json_object_set_int_member(variant, "pet_species_id", species);

		if (breed > 0)
		{
			g_string_append_printf(key, ".%" G_GINT64_FORMAT, breed);
			json_object_set_int_member(variant, "pet_breed_id", breed);
		}
	}

	if (json_object_get_size(variant) > 0)
		*out_variant = g_steal_pointer(&variant);

	return g_string_free(g_steal_pointer(&key), FALSE);
}

/* ==========================================================================
 * Auctions into a batch
 * ========================================================================== */

/*
 * time_left as the least time the auction still has, in seconds -- the
 * store's expires_in_min ("min" is minimum, not minutes). Blizzard gives a
 * bucket, and each bucket is "under N hours": SHORT < 30 min, MEDIUM 30 min
 * to 2 h, LONG 2 to 12 h, VERY_LONG 12 to 48 h. The *lower* bound of each
 * is used, because the sale estimate asks "could this auction have
 * expired by the next snapshot?": a listing that vanishes before even its
 * least remaining time has run out cannot have expired, so it sold (or was
 * cancelled). Using the upper bound would count every LONG auction that
 * ran out after three hours as a sale.
 */
static gint64
blizzard_time_left(const gchar *time_left)
{
	if (0 == g_strcmp0(time_left, "SHORT"))
		return 0;

	if (0 == g_strcmp0(time_left, "MEDIUM"))
		return 30 * 60;

	if (0 == g_strcmp0(time_left, "LONG"))
		return 2 * 3600;

	if (0 == g_strcmp0(time_left, "VERY_LONG"))
		return 12 * 3600;

	return -1;
}

/* One instrument the auctions named, until the batch gets it. */
typedef struct
{
	gint64		 item_id;
	JsonObject	*variant;	/* NULL for a plain item */
} BlizzardSeen;

static void
blizzard_seen_free(gpointer data)
{
	BlizzardSeen *seen = data;

	g_clear_pointer(&seen->variant, json_object_unref);
	g_free(seen);
}

/*
 * One venue's auctions -- a realm's or the commodity market's -- as a
 * complete snapshot. The price, in copper, is the auction's unit_price
 * (commodities, and stackables on a realm); else its buyout divided by
 * its quantity, rounded half to even; else, when include_bid_only is on,
 * its current bid divided the same way -- the least it can be had for,
 * if nobody outbids. An auction with none of the three, or no usable item
 * or quantity, is refused and counted.
 *
 * By default a bid-only auction is skipped, and counted apart from the
 * refused: it is not malformed, it just cannot be bought now. Listed, it
 * set the lowest price, so Deals sent a buyer to a realm where the item
 * could only be bid on; and its end -- won by a bid or expired -- read as
 * a listing that vanished, which the sale estimate counts as a buyout.
 */
static gboolean
blizzard_add_auctions(
	BlizzardFrozen		 *frozen,
	VentureFeedBatch	 *batch,
	const gchar		 *venue_key,
	gint64			  taken_at,
	JsonNode		 *root,
	GHashTable		 *seen,
	GError			**error
){
	JsonArray *auctions;
	gint64 refused;
	gint64 bid_only;
	guint i;

	if (!JSON_NODE_HOLDS_OBJECT(root) ||
	    (NULL == (auctions = blizzard_array(json_node_get_object(root), "auctions"))))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_SERIALIZATION,
		            "%s: the answer has no auctions list", venue_key);
		return FALSE;
	}

	if (!venture_feed_batch_begin_snapshot(batch, venue_key, frozen->settings->currency, taken_at, TRUE, error))
		return FALSE;

	refused = 0;
	bid_only = 0;

	for (i = 0; i < json_array_get_length(auctions); i++)
	{
		g_autoptr(JsonObject) variant = NULL;
		g_autofree gchar *key = NULL;
		JsonNode *element;
		JsonObject *auction;
		JsonObject *item;
		gint64 item_id = 0;
		gint64 quantity;
		gint64 price;
		gint64 total;
		gint64 listing_id;

		element = json_array_get_element(auctions, i);
		auction = JSON_NODE_HOLDS_OBJECT(element) ? json_node_get_object(element) : NULL;
		item = blizzard_object(auction, "item");
		key = (NULL != item) ? blizzard_item_key(item, frozen->settings, &item_id, &variant) : NULL;
		quantity = blizzard_int(auction, "quantity", 1);

		if ((NULL == key) || (quantity < 1))
		{
			refused++;
			continue;
		}

		price = blizzard_int(auction, "unit_price", -1);

		if (price < 0)
		{
			total = blizzard_int(auction, "buyout", -1);

			if ((total < 0) && (blizzard_int(auction, "bid", -1) >= 0))
			{
				bid_only++;

				if (!frozen->settings->include_bid_only)
					continue;

				total = blizzard_int(auction, "bid", -1);
			}

			price = (total >= 0) ? venture_series_math_div_round(total, quantity) : -1;
		}

		if (price < 0)
		{
			refused++;
			continue;
		}

		listing_id = blizzard_int(auction, "id", 0);

		if (!venture_feed_batch_add_listing(batch, venue_key, key,
		                                    (listing_id > 0) ? (guint64)listing_id : 0,
		                                    price, quantity, FALSE,
		                                    blizzard_time_left(venture_json_object_get_string(
		                                            auction, "time_left", NULL)),
		                                    NULL))
		{
			refused++;
			continue;
		}

		if (!g_hash_table_contains(seen, key))
		{
			BlizzardSeen *entry = g_new0(BlizzardSeen, 1);

			entry->item_id = item_id;
			entry->variant = g_steal_pointer(&variant);
			g_hash_table_insert(seen, g_steal_pointer(&key), entry);
		}
	}

	venture_feed_batch_add_refused(batch, refused);

	if (refused > 0)
	{
		g_autofree gchar *note = g_strdup_printf("%s: %" G_GINT64_FORMAT " auctions refused: no item, "
		                                         "no quantity or no price", venue_key, refused);

		venture_feed_batch_add_note(batch, note);
	}

	if ((bid_only > 0) && frozen->settings->include_bid_only)
	{
		g_autofree gchar *note = g_strdup_printf("%s: %" G_GINT64_FORMAT " auctions have a bid and no "
		                                         "buyout; listed at their bid", venue_key, bid_only);

		venture_feed_batch_add_note(batch, note);
	}
	else if (bid_only > 0)
	{
		g_autofree gchar *note = g_strdup_printf("%s: %" G_GINT64_FORMAT " bid-only auctions skipped: "
		                                         "no buyout, so they cannot be bought now", venue_key,
		                                         bid_only);

		venture_feed_batch_add_note(batch, note);
	}

	return TRUE;
}

static gint
blizzard_compare_keys(
	gconstpointer	a,
	gconstpointer	b
){
	return g_strcmp0(*(const gchar *const *)a, *(const gchar *const *)b);
}

/*
 * The instruments the auctions named: names fetched for at most
 * item_names_per_fetch items the cache does not know yet (lowest id first,
 * so a big realm is named in a stable order over several fetches), then
 * every instrument added -- a variant under its plain item, which is
 * added too. Name, category and attributes go in only once the item's
 * details are known: a NULL leaves what the store holds alone, so a
 * restart that has not re-fetched a name yet does not blank it.
 */
/*
 * An item's links to other sites: Wowhead's page for the item (with the
 * variant's bonuses, which Wowhead reads to show the right item level and
 * stats) or the battle pet, as attrs.links; and the item as Undermine
 * Exchange names it in a link -- the item id, or the pet cage and species
 * -- as attrs.undermine_item, for a venue's link template to fill in.
 */
static void
blizzard_item_links(
	JsonObject	*attrs,
	const gchar	*key,
	gint64		 item_id,
	gint64		 species
){
	g_autoptr(GString) wowhead = g_string_new(NULL);
	g_autofree gchar *undermine = NULL;
	JsonArray *links = json_array_new();
	JsonObject *link = json_object_new();
	const gchar *bonuses;

	if (species > 0)
	{
		g_string_append_printf(wowhead, "https://www.wowhead.com/battle-pet/%" G_GINT64_FORMAT, species);
		undermine = g_strdup_printf("%" G_GINT64_FORMAT "-%" G_GINT64_FORMAT, item_id, species);
	}
	else
	{
		g_string_append_printf(wowhead, "https://www.wowhead.com/item=%" G_GINT64_FORMAT, item_id);
		undermine = g_strdup_printf("%" G_GINT64_FORMAT, item_id);

		/* The key's ":b1472,6646" part is Wowhead's "?bonus=1472:6646". */
		bonuses = strstr(key, ":b");

		if (NULL != bonuses)
		{
			const gchar *end = strchr(bonuses + 2, ':');
			g_autofree gchar *list = g_strndup(bonuses + 2, (NULL != end) ? (gsize)(end - bonuses - 2)
			                                                              : strlen(bonuses + 2));

			g_strdelimit(list, ",", ':');
			g_string_append_printf(wowhead, "?bonus=%s", list);
		}
	}

	json_object_set_string_member(link, "label", "Wowhead");
	json_object_set_string_member(link, "url", wowhead->str);
	json_array_add_object_element(links, link);
	json_object_set_array_member(attrs, "links", links);
	json_object_set_string_member(attrs, "undermine_item", undermine);
}

/*
 * Fills the item cache from the source's own store for every @unknown item
 * the store already has a name for, and removes those from @unknown.
 *
 * The cache lives only as long as the process. Without this, every restart
 * forgot every name it had learned and spent the next fetches -- and the
 * hourly request budget -- asking Blizzard for items the store already
 * named, lowest id first, while the items still nameless waited behind
 * them. A missing or unreadable store (the source's first fetch) leaves
 * @unknown as it was.
 */
static void
blizzard_items_from_store(
	VentureFeedRequest	*request,
	BlizzardFrozen		*frozen,
	GArray			*unknown
){
	g_autoptr(VentureSeriesStore) reader = NULL;
	g_autoptr(GArray) still = NULL;
	VentureFeedSource *source;
	guint i;

	if (0 == unknown->len)
		return;

	source = venture_feed_request_get_source(request);
	if (NULL == source)
		return;

	reader = venture_series_store_open_reader(venture_feed_source_get_store_dir(source), NULL);
	if (NULL == reader)
		return;

	still = g_array_new(FALSE, FALSE, sizeof(gint64));

	for (i = 0; i < unknown->len; i++)
	{
		gint64 item_id = g_array_index(unknown, gint64, i);
		g_autoptr(VentureSeriesInstrumentRow) row = NULL;
		g_autofree gchar *key = g_strdup_printf("%" G_GINT64_FORMAT, item_id);
		BlizzardItem *item;

		if (!venture_series_store_get_instrument(reader, key, &row, NULL) ||
		    (NULL == row) || (NULL == row->name) || ('\0' == *row->name))
		{
			g_array_append_val(still, item_id);
			continue;
		}

		item = g_new0(BlizzardItem, 1);
		item->name = g_strdup(row->name);
		item->category = g_strdup(row->category);
		item->vendor_sell = -1;

		if (NULL != row->attrs_json)
		{
			g_autoptr(JsonParser) parser = json_parser_new();

			if (json_parser_load_from_data(parser, row->attrs_json, -1, NULL) &&
			    JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
			{
				JsonObject *attrs = json_node_get_object(json_parser_get_root(parser));

				item->vendor_sell = blizzard_int(attrs, "vendor_sell", -1);
				item->level = blizzard_int(attrs, "level", 0);
				item->quality = g_strdup(blizzard_text(attrs, "quality", NULL));

				if (json_object_has_member(attrs, "display") &&
				    JSON_NODE_HOLDS_OBJECT(json_object_get_member(attrs, "display")))
				{
					g_autoptr(JsonNode) display = json_node_copy(json_object_get_member(attrs,
					                                                                     "display"));

					item->display_json = json_to_string(display, FALSE);
				}
			}
		}

		/* Named by an earlier build, which drew no tooltip: its name serves
		 * now, and it is asked for again to get one. */
		item->stale = (NULL == item->display_json);

		if (item->stale)
			g_array_append_val(still, item_id);

		g_mutex_lock(&frozen->lock);
		g_hash_table_replace(frozen->items, g_memdup2(&item_id, sizeof(item_id)), item);
		g_mutex_unlock(&frozen->lock);
	}

	g_array_set_size(unknown, 0);
	g_array_append_vals(unknown, still->data, still->len);
}

/*
 * An item's attributes as the store keeps them, as JSON text: @variant's
 * own (nullable; a variant's bonuses and modifiers), the item id, its
 * vendor price, quality and level, its tooltip, and its links elsewhere
 * for @instrument (the plain item's key, a variant's, or a caged pet's
 * with @species).
 */
static gchar *
blizzard_item_attrs(
	const BlizzardItem	*item,
	const gchar		*instrument,
	gint64			 item_id,
	JsonObject		*variant,
	gint64			 species
){
	g_autoptr(JsonObject) attrs = NULL;
	g_autoptr(JsonNode) node = NULL;

	attrs = (NULL != variant) ? json_object_ref(variant) : json_object_new();
	json_object_set_int_member(attrs, "item_id", item_id);

	if (item->vendor_sell >= 0)
		json_object_set_int_member(attrs, "vendor_sell", item->vendor_sell);

	if (NULL != item->quality)
		json_object_set_string_member(attrs, "quality", item->quality);

	if (item->level > 0)
		json_object_set_int_member(attrs, "level", item->level);

	if (NULL != item->display_json)
	{
		g_autoptr(JsonParser) parser = json_parser_new();

		if (json_parser_load_from_data(parser, item->display_json, -1, NULL) &&
		    JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
			json_object_set_member(attrs, "display", json_node_copy(json_parser_get_root(parser)));
	}

	blizzard_item_links(attrs, instrument, item_id, species);
	node = json_node_new(JSON_NODE_OBJECT);
	json_node_set_object(node, attrs);

	return json_to_string(node, FALSE);
}

static void
blizzard_add_instruments(
	VentureFeedRequest	*request,
	BlizzardFrozen		*frozen,
	VentureFeedBatch	*batch,
	GHashTable		*seen
){
	g_autoptr(GPtrArray) keys = NULL;
	g_autoptr(GArray) unknown = NULL;
	g_autoptr(GHashTable) listed = NULL;
	g_autoptr(GHashTable) plain = NULL;
	GHashTableIter iter;
	gpointer key;
	gpointer value;
	guint fetched;
	guint i;

	keys = g_ptr_array_new();
	unknown = g_array_new(FALSE, FALSE, sizeof(gint64));
	listed = g_hash_table_new(g_int64_hash, g_int64_equal);
	plain = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_hash_table_iter_init(&iter, seen);

	g_mutex_lock(&frozen->lock);

	/* A set, not a scan of the list so far: a realm's snapshot names tens
	 * of thousands of items, and a linear "already listed?" made this
	 * quadratic in exactly the case -- a cold cache -- where it is big. */
	while (g_hash_table_iter_next(&iter, &key, &value))
	{
		BlizzardSeen *entry = value;

		g_ptr_array_add(keys, key);

		{
			BlizzardItem *known = g_hash_table_lookup(frozen->items, &entry->item_id);

			if (((NULL != known) && !known->stale) ||
			    g_hash_table_contains(listed, &entry->item_id))
				continue;
		}

		g_hash_table_add(listed, &entry->item_id);
		g_array_append_val(unknown, entry->item_id);
	}

	g_mutex_unlock(&frozen->lock);

	blizzard_items_from_store(request, frozen, unknown);

	g_ptr_array_sort(keys, blizzard_compare_keys);
	g_array_sort(unknown, blizzard_compare_int64);

	/* The names: bounded, and stopped by the first refusal -- a 429 here
	 * is the far end asking for quiet, and the worker honours it. */
	for (fetched = 0; (fetched < frozen->settings->item_names_per_fetch) && (fetched < unknown->len);
	     fetched++)
	{
		g_autoptr(BlizzardItem) item = NULL;
		g_autoptr(GError) error = NULL;

		if (!blizzard_fetch_item(request, frozen, g_array_index(unknown, gint64, fetched), &item, &error))
		{
			g_autofree gchar *note = g_strdup_printf("item names: stopped after %u: %s", fetched,
			                                         error->message);

			venture_feed_batch_add_note(batch, note);
			break;
		}
	}

	if (unknown->len > fetched)
	{
		g_autofree gchar *note = g_strdup_printf("item names: %u items still to name; the next fetches "
		                                         "name up to %u each", unknown->len - fetched,
		                                         frozen->settings->item_names_per_fetch);

		venture_feed_batch_add_note(batch, note);
	}

	for (i = 0; i < keys->len; i++)
	{
		const gchar *instrument = g_ptr_array_index(keys, i);
		BlizzardSeen *entry = g_hash_table_lookup(seen, instrument);
		g_autoptr(BlizzardItem) item = NULL;
		g_autofree gchar *parent = NULL;
		g_autofree gchar *attrs_json = NULL;
		g_autofree gchar *name = NULL;
		BlizzardItem *cached;

		g_mutex_lock(&frozen->lock);
		cached = g_hash_table_lookup(frozen->items, &entry->item_id);
		item = (NULL != cached) ? blizzard_item_copy(cached) : NULL;
		g_mutex_unlock(&frozen->lock);

		if (NULL != entry->variant)
		{
			parent = g_strdup_printf("%" G_GINT64_FORMAT, entry->item_id);

			/* The plain item, once, ahead of its variants (sorted keys
			 * put "2770" before "2770:b..."). */
			if (!g_hash_table_contains(seen, parent) && !g_hash_table_contains(plain, parent))
			{
				/* With its attributes and tooltip, like any item: a
				 * plain row left without them reads, at the next fetch,
				 * as named by a build that drew no tooltip, and is
				 * asked for again -- every fetch, for every item that
				 * is only ever listed as variants. */
				g_autofree gchar *parent_attrs = ((NULL != item) && (NULL != item->name))
					? blizzard_item_attrs(item, parent, entry->item_id, NULL, 0) : NULL;

				venture_feed_batch_add_instrument(batch, parent,
				                                  (NULL != item) ? item->name : NULL, "item",
				                                  (NULL != item) ? item->category : NULL,
				                                  NULL, parent_attrs, NULL);
				g_hash_table_add(plain, g_strdup(parent));
			}
		}

		if ((NULL != item) && (NULL != item->name))
		{
			gint64 item_id;
			gint64 species = 0;

			/* A caged pet is its cage's item; the species says which. */
			if (!blizzard_item_key_parse(instrument, &item_id, &species))
				species = 0;

			attrs_json = blizzard_item_attrs(item, instrument, entry->item_id, entry->variant, species);
			name = (species > 0)
				? g_strdup_printf("%s (pet %" G_GINT64_FORMAT ")", item->name, species)
				: g_strdup(item->name);
		}

		venture_feed_batch_add_instrument(batch, instrument, name, "item",
		                                  (NULL != item) ? item->category : NULL, parent,
		                                  attrs_json, NULL);
	}
}

/* ==========================================================================
 * The units
 * ========================================================================== */

/* "dynamic" namespace queries name it; the caller frees. */
static gchar *
blizzard_url(
	BlizzardFrozen	*frozen,
	const gchar	*path
){
	return g_strdup_printf("%s%s%snamespace=%s", frozen->settings->api_base, path,
	                       (NULL != strchr(path, '?')) ? "&" : "?",
	                       frozen->settings->dynamic_namespace);
}

static gboolean
blizzard_add_venue(
	BlizzardFrozen		 *frozen,
	VentureFeedBatch	 *batch,
	const gchar		 *key,
	const gchar		 *name,
	GError			**error
){
	g_autoptr(JsonObject) object = json_object_new();
	g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
	g_autofree gchar *attrs = NULL;
	g_autofree gchar *slug = NULL;
	gboolean commodities = (0 == g_strcmp0(key, BLIZZARD_UNIT_COMMODITIES));

	json_object_set_string_member(object, "region", frozen->settings->region);

	if (commodities)
		json_object_set_boolean_member(object, "commodities", TRUE);
	else
		json_object_set_int_member(object, "connected_realm_id", g_ascii_strtoll(key, NULL, 10));

	/* The realm's slug, and the venue's page on Undermine Exchange for an
	 * item ("{undermine_item}" is filled from the instrument's attributes).
	 * The commodity market is the same on every realm of a region, so any
	 * realm's page shows it. */
	g_mutex_lock(&frozen->lock);
	if (!commodities)
		slug = g_strdup(g_hash_table_lookup(frozen->realm_slugs, key));
	else
	{
		GHashTableIter iter;
		gpointer value;

		g_hash_table_iter_init(&iter, frozen->realm_slugs);
		if (g_hash_table_iter_next(&iter, NULL, &value))
			slug = g_strdup(value);
	}
	g_mutex_unlock(&frozen->lock);

	if (NULL != slug)
	{
		JsonArray *links = json_array_new();
		JsonObject *link = json_object_new();
		g_autofree gchar *url = g_strdup_printf("https://undermine.exchange/#%s-%s/{undermine_item}",
		                                        frozen->settings->region, slug);

		if (!commodities)
			json_object_set_string_member(object, "slug", slug);

		json_object_set_string_member(link, "label", "Undermine Exchange");
		json_object_set_string_member(link, "url", url);
		json_array_add_object_element(links, link);
		json_object_set_array_member(object, "links", links);
	}

	json_node_set_object(node, object);
	attrs = json_to_string(node, FALSE);

	return venture_feed_batch_add_venue(batch, key, name, "auction_house", frozen->settings->region,
	                                    frozen->settings->currency, attrs, error);
}

/*
 * An HTTP date, spelt in English whatever the server's locale: what
 * If-Modified-Since needs, and what g_date_time_format()'s %a would not
 * promise.
 */
static gchar *
blizzard_http_date(gint64 unix_time)
{
	static const gchar *const days[] = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };
	static const gchar *const months[] = {
		"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
	};
	g_autoptr(GDateTime) when = g_date_time_new_from_unix_utc(unix_time);

	return g_strdup_printf("%s, %02d %s %04d %02d:%02d:%02d GMT",
	                       days[g_date_time_get_day_of_week(when) - 1],
	                       g_date_time_get_day_of_month(when),
	                       months[g_date_time_get_month(when) - 1],
	                       g_date_time_get_year(when), g_date_time_get_hour(when),
	                       g_date_time_get_minute(when), g_date_time_get_second(when));
}

/*
 * What Blizzard said about one venue's auctions, as an error for anything
 * that is neither 200 nor 304.
 */
static gboolean
blizzard_judge_status(
	guint		  status,
	const gchar	 *what,
	GError		**error
){
	if ((200 == status) || (304 == status))
		return TRUE;

	if (404 == status)
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "Battle.net has no %s (HTTP 404): check the id and the region", what);
	else
		g_set_error(error, VENTURE_FEEDS_ERROR, VENTURE_FEEDS_ERROR_HTTP,
		            "Battle.net answered HTTP %u for %s", status, what);

	return FALSE;
}

/*
 * One venue: a connected realm's auctions or the commodity market. When
 * the venue is the unit, the request is the unit's own and the helper
 * sends and keeps the unit's If-Modified-Since. When it is one realm of
 * the every-realm unit (@side), the request carries @if_modified_since --
 * that realm's, from the cursor -- itself, and leaves the unit's alone: a
 * Last-Modified of realm 11 must not become the question asked of 3676.
 *
 * Returns the status in *@out_status: 304 adds nothing.
 */
static gboolean
blizzard_fetch_venue(
	VentureFeedRequest	 *request,
	BlizzardFrozen		 *frozen,
	VentureFeedBatch	 *batch,
	const gchar		 *venue_key,
	gboolean		  side,
	gint64			  if_modified_since,
	GHashTable		 *seen,
	guint			 *out_status,
	gint64			 *out_last_modified,
	GError			**error
){
	g_autoptr(JsonNode) root = NULL;
	g_autofree gchar *url = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *what = NULL;
	g_autofree gchar *since = NULL;
	g_autofree gchar *name = NULL;
	const gchar *headers[3] = { NULL, NULL, NULL };
	VentureFeedHttpFlags flags;
	gboolean commodities;
	gint64 last_modified;
	gint64 taken_at;
	guint cost;

	commodities = (0 == g_strcmp0(venue_key, BLIZZARD_UNIT_COMMODITIES));
	path = commodities ? g_strdup("/data/wow/auctions/commodities")
	                   : g_strdup_printf("/data/wow/connected-realm/%s/auctions", venue_key);
	what = commodities ? g_strdup_printf("%s commodity market", frozen->settings->region)
	                   : g_strdup_printf("connected realm %s", venue_key);
	url = blizzard_url(frozen, path);

	/* 25 for the commodity market, spent before the request goes out:
	 * Blizzard charges it even when it answers 304. */
	cost = commodities ? BLIZZARD_COST_COMMODITIES : BLIZZARD_COST_DEFAULT;
	flags = side ? VENTURE_FEED_HTTP_UNCONDITIONAL : VENTURE_FEED_HTTP_DEFAULT;

	if (side && (VENTURE_SERIES_NONE != if_modified_since))
	{
		since = blizzard_http_date(if_modified_since);
		headers[0] = "If-Modified-Since";
		headers[1] = since;
	}

	{
		GError *local_error = NULL;

		root = blizzard_get_json(request, frozen, url, cost, flags,
		                         (NULL != since) ? headers : NULL, out_status, &last_modified,
		                         &local_error);

		if (NULL != local_error)
		{
			g_propagate_error(error, local_error);
			return FALSE;
		}
	}

	if (NULL != out_last_modified)
		*out_last_modified = last_modified;

	if (!blizzard_judge_status(*out_status, what, error))
		return FALSE;

	if (304 == *out_status)
		return TRUE;

	if (commodities)
	{
		g_autofree gchar *region = g_ascii_strup(frozen->settings->region, -1);

		name = g_strdup_printf("%s commodities", region);
	}
	else
		name = blizzard_realm_name(request, frozen, venue_key, batch);

	if (!blizzard_add_venue(frozen, batch, venue_key, name, error))
		return FALSE;

	/* Blizzard's Last-Modified is when the snapshot was taken -- the time
	 * the store dates it by, and the clock adaptive polling learns. */
	taken_at = (VENTURE_SERIES_NONE != last_modified) ? last_modified
	                                                   : venture_feed_request_get_fetched_at(request);

	return blizzard_add_auctions(frozen, batch, venue_key, taken_at, root, seen, error);
}

/* ==========================================================================
 * The realm list
 *
 * A source that names no realm gets one unit per connected realm the
 * region's index lists -- the same unit names an explicit list makes
 * ("11", "3676"), so moving a source between the two keeps every unit's
 * If-Modified-Since and cursor (the store keeps them as ims:<unit> and
 * cursor:<unit>) and every venue's learned update time (keyed by the same
 * id). One unit per realm is what lets the adaptive schedule check each
 * realm just before its own hourly snapshot; the walk this replaced read
 * ten realms with new data a fetch, so on a region of 83 a realm's prices
 * were up to eight hours old.
 *
 * Units are listed on the main thread, from the settings alone, when the
 * source is frozen -- no credentials, no HTTP client, no network. So the
 * index is read by a unit of its own, realm-index, at most once every
 * realm_index_hours, and kept here: a file per API origin and namespace
 * under the plugin's cache directory, which list_units reads, and which a
 * failed read of the index leaves as it was -- the last known list keeps
 * serving. A list that changed moves the generation, and the plugin's
 * after-run hook (blizzard-plugin.c) asks the feeds service to freeze its
 * sources again, which lists their units afresh.
 *
 * Before any list is known (a new source, a new region) the units are
 * realm-index alone, then commodities: the first pass reads the index and
 * the next has every realm. The every-realm walk is not kept as a
 * fallback for that pass: it would read the same index, then a few realms
 * once, then be replaced -- a second fetch path, with a cursor of its own,
 * for one pass.
 * ========================================================================== */

typedef struct
{
	GArray	*ids;		/* gint64, ascending */
	gint64	 fetched_at;	/* Unix seconds */
} BlizzardRealmList;

static GMutex blizzard_realm_lock;
static gchar *blizzard_realm_dir_path = NULL;
static GHashTable *blizzard_realm_memory = NULL;	/* place -> BlizzardRealmList */
static gint blizzard_realm_generation = 0;

static void
blizzard_realm_list_free(gpointer data)
{
	BlizzardRealmList *list = data;

	g_array_unref(list->ids);
	g_free(list);
}

void
blizzard_set_realm_dir(const gchar *dir)
{
	g_mutex_lock(&blizzard_realm_lock);
	g_free(blizzard_realm_dir_path);
	blizzard_realm_dir_path = g_strdup(dir);
	g_mutex_unlock(&blizzard_realm_lock);
}

guint
blizzard_realms_get_generation(void)
{
	return (guint)g_atomic_int_get(&blizzard_realm_generation);
}

/*
 * Where a region's list is kept: the file under the cache directory, or,
 * with none, a name for the in-memory table. The index answers per API
 * origin and namespace, so those are the key; the origin is hashed into
 * the file name (it is an address), the namespace is a word the settings
 * held to [a-z0-9.-]. Caller holds the lock.
 */
static gchar *
blizzard_realm_place(const BlizzardSettings *settings)
{
	g_autofree gchar *key = NULL;
	g_autofree gchar *digest = NULL;
	g_autofree gchar *name = NULL;

	key = g_strconcat(settings->api_base, " ", settings->dynamic_namespace, NULL);

	if (NULL == blizzard_realm_dir_path)
		return g_strconcat("memory:", key, NULL);

	digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, key, -1);
	name = g_strdup_printf("%s-%.16s.json", settings->dynamic_namespace, digest);

	return g_build_filename(blizzard_realm_dir_path, name, NULL);
}

/* A list as the file spells it, judged as strictly as the index is. */
static BlizzardRealmList *
blizzard_realm_list_read(
	const BlizzardSettings	*settings,
	const gchar		*path
){
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(GArray) ids = NULL;
	JsonObject *object;
	JsonArray *array;
	BlizzardRealmList *list;
	guint i;

	if (!json_parser_load_from_file(parser, path, NULL) ||
	    !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
		return NULL;

	object = json_node_get_object(json_parser_get_root(parser));

	/* The name is a hash's prefix: make sure it is this origin's. */
	if ((0 != g_strcmp0(venture_json_object_get_string(object, "api_base", NULL), settings->api_base)) ||
	    (0 != g_strcmp0(venture_json_object_get_string(object, "namespace", NULL),
	                    settings->dynamic_namespace)) ||
	    (NULL == (array = blizzard_array(object, "ids"))) ||
	    (0 == json_array_get_length(array)) || (json_array_get_length(array) > BLIZZARD_MAX_REALMS))
		return NULL;

	ids = g_array_new(FALSE, FALSE, sizeof(gint64));

	for (i = 0; i < json_array_get_length(array); i++)
	{
		JsonNode *element = json_array_get_element(array, i);
		gint64 id;

		if (!JSON_NODE_HOLDS_VALUE(element) || (G_TYPE_INT64 != json_node_get_value_type(element)))
			return NULL;

		id = json_node_get_int(element);

		if ((id < 1) || (id > G_MAXINT32))
			return NULL;

		g_array_append_val(ids, id);
	}

	g_array_sort(ids, blizzard_compare_int64);

	list = g_new0(BlizzardRealmList, 1);
	list->ids = g_steal_pointer(&ids);
	list->fetched_at = blizzard_int(object, "fetched_at", 0);

	return list;
}

/*
 * The last known list for these settings' region, or FALSE when none is.
 * Memory first: it holds whatever this process wrote, and the file only
 * matters after a restart.
 */
static gboolean
blizzard_realms_load(
	const BlizzardSettings	 *settings,
	GArray			**out_ids,
	gint64			 *out_fetched_at
){
	g_autofree gchar *place = NULL;
	BlizzardRealmList *list;

	g_mutex_lock(&blizzard_realm_lock);

	if (NULL == blizzard_realm_memory)
		blizzard_realm_memory = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
		                                              blizzard_realm_list_free);

	place = blizzard_realm_place(settings);
	list = g_hash_table_lookup(blizzard_realm_memory, place);

	if ((NULL == list) && (NULL != blizzard_realm_dir_path))
	{
		list = blizzard_realm_list_read(settings, place);

		if (NULL != list)
			g_hash_table_replace(blizzard_realm_memory, g_strdup(place), list);
	}

	if (NULL != list)
	{
		*out_ids = g_array_copy(list->ids);

		if (NULL != out_fetched_at)
			*out_fetched_at = list->fetched_at;
	}

	g_mutex_unlock(&blizzard_realm_lock);

	return NULL != list;
}

/*
 * Keeps a list just read from the index, and says whether it differs from
 * the one known before (a changed list moves the generation). The file is
 * written whole under a temporary name and renamed; a write that fails
 * leaves the list in memory, for this process's life.
 */
static gboolean
blizzard_realms_store(
	const BlizzardSettings	*settings,
	GArray			*ids,
	gint64			 fetched_at,
	guint			*out_added,
	guint			*out_removed
){
	g_autofree gchar *place = NULL;
	BlizzardRealmList *before;
	BlizzardRealmList *list;
	gboolean changed;
	guint added;
	guint removed;
	guint i;
	guint j;

	g_mutex_lock(&blizzard_realm_lock);

	if (NULL == blizzard_realm_memory)
		blizzard_realm_memory = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
		                                              blizzard_realm_list_free);

	place = blizzard_realm_place(settings);
	before = g_hash_table_lookup(blizzard_realm_memory, place);

	if ((NULL == before) && (NULL != blizzard_realm_dir_path))
	{
		before = blizzard_realm_list_read(settings, place);

		if (NULL != before)
			g_hash_table_replace(blizzard_realm_memory, g_strdup(place), before);
	}

	/* Both ascending: one merge counts what came and what went. */
	added = 0;
	removed = 0;

	for (i = 0, j = 0; (NULL != before) && ((i < ids->len) || (j < before->ids->len));)
	{
		gint64 now_id = (i < ids->len) ? g_array_index(ids, gint64, i) : G_MAXINT64;
		gint64 was_id = (j < before->ids->len) ? g_array_index(before->ids, gint64, j) : G_MAXINT64;

		if (now_id == was_id)
		{
			i++;
			j++;
		}
		else if (now_id < was_id)
		{
			added++;
			i++;
		}
		else
		{
			removed++;
			j++;
		}
	}

	if (NULL == before)
		added = ids->len;

	changed = (NULL == before) || (added > 0) || (removed > 0);

	list = g_new0(BlizzardRealmList, 1);
	list->ids = g_array_copy(ids);
	list->fetched_at = fetched_at;
	g_hash_table_replace(blizzard_realm_memory, g_strdup(place), list);

	if (NULL != blizzard_realm_dir_path)
	{
		g_autoptr(JsonBuilder) builder = json_builder_new();
		g_autoptr(JsonNode) root = NULL;
		g_autofree gchar *text = NULL;

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "api_base");
		json_builder_add_string_value(builder, settings->api_base);
		json_builder_set_member_name(builder, "namespace");
		json_builder_add_string_value(builder, settings->dynamic_namespace);
		json_builder_set_member_name(builder, "fetched_at");
		json_builder_add_int_value(builder, fetched_at);
		json_builder_set_member_name(builder, "ids");
		json_builder_begin_array(builder);

		for (i = 0; i < ids->len; i++)
			json_builder_add_int_value(builder, g_array_index(ids, gint64, i));

		json_builder_end_array(builder);
		json_builder_end_object(builder);
		root = json_builder_get_root(builder);
		text = json_to_string(root, FALSE);

		/* g_file_set_contents() renames over the old file: a reader never
		 * sees half of one. A failure is only a list kept in memory. */
		(void)g_file_set_contents(place, text, -1, NULL);
	}

	if (changed)
		g_atomic_int_inc(&blizzard_realm_generation);

	g_mutex_unlock(&blizzard_realm_lock);

	*out_added = added;
	*out_removed = removed;

	return changed;
}

/*
 * The connected-realm index, as ids ascending: the index lists hrefs, and
 * the id is the path segment after connected-realm/. A side request in
 * the sense that its date is no unit's If-Modified-Since: the realm-index
 * unit must get the list whenever it asks, and a 304 would leave a source
 * whose cache was lost with no list at all.
 */
static GArray *
blizzard_read_realm_index(
	VentureFeedRequest	 *request,
	BlizzardFrozen		 *frozen,
	GError			**error
){
	g_autoptr(JsonNode) listing = NULL;
	g_autoptr(GArray) ids = NULL;
	g_autofree gchar *url = NULL;
	JsonArray *realms;
	guint status;
	guint i;

	url = blizzard_url(frozen, "/data/wow/connected-realm/index");

	{
		GError *local_error = NULL;

		listing = blizzard_get_json(request, frozen, url, BLIZZARD_COST_DEFAULT,
		                            VENTURE_FEED_HTTP_UNCONDITIONAL, NULL, &status, NULL, &local_error);

		if (NULL != local_error)
		{
			g_propagate_error(error, local_error);
			return NULL;
		}
	}

	if ((200 != status) && !blizzard_judge_status(status, "connected realm index", error))
		return NULL;

	if ((NULL == listing) || !JSON_NODE_HOLDS_OBJECT(listing))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_SERIALIZATION,
		                    "The connected realm index is not an object");
		return NULL;
	}

	ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	realms = blizzard_array(json_node_get_object(listing), "connected_realms");

	for (i = 0; (NULL != realms) && (i < json_array_get_length(realms)); i++)
	{
		JsonNode *element = json_array_get_element(realms, i);
		const gchar *href;
		const gchar *at;
		gboolean repeated = FALSE;
		gint64 id;
		guint j;

		href = JSON_NODE_HOLDS_OBJECT(element)
			? venture_json_object_get_string(json_node_get_object(element), "href", NULL) : NULL;
		at = (NULL != href) ? strstr(href, "/connected-realm/") : NULL;
		id = (NULL != at) ? g_ascii_strtoll(at + strlen("/connected-realm/"), NULL, 10) : 0;

		for (j = 0; j < ids->len; j++)
			repeated = repeated || (g_array_index(ids, gint64, j) == id);

		if ((id > 0) && (id <= G_MAXINT32) && !repeated)
			g_array_append_val(ids, id);
	}

	g_array_sort(ids, blizzard_compare_int64);

	/* An empty index is a fault at the far end, not a region with no
	 * realms: kept, it would take every realm's unit away. */
	if ((0 == ids->len) || (ids->len > BLIZZARD_MAX_REALMS))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_SERIALIZATION,
		            "The connected realm index lists %u realms", ids->len);
		return NULL;
	}

	return g_steal_pointer(&ids);
}

/*
 * The realm-index unit: the index, when the known list is older than
 * realm_index_hours (or there is none), kept for list_units. It stores
 * nothing in the series store -- an unchanged or fresh list is "not
 * modified", a changed one a batch carrying only a note -- and a failure
 * is the unit's own: the last known list goes on serving.
 */
static VentureFeedBatch *
blizzard_fetch_realm_index(
	VentureFeedRequest	 *request,
	BlizzardFrozen		 *frozen,
	GError			**error
){
	g_autoptr(VentureFeedBatch) batch = NULL;
	g_autoptr(GArray) known = NULL;
	g_autoptr(GArray) ids = NULL;
	g_autofree gchar *note = NULL;
	gint64 fetched_at = 0;
	gint64 now;
	guint added;
	guint removed;

	batch = venture_feed_batch_new();
	now = g_get_real_time() / G_USEC_PER_SEC;

	if (blizzard_realms_load(frozen->settings, &known, &fetched_at) &&
	    (now - fetched_at < (gint64)frozen->settings->realm_index_hours * 3600) && (now >= fetched_at))
	{
		venture_feed_batch_set_not_modified(batch, TRUE);
		return g_steal_pointer(&batch);
	}

	ids = blizzard_read_realm_index(request, frozen, error);

	if (NULL == ids)
		return NULL;

	if (!blizzard_realms_store(frozen->settings, ids, now, &added, &removed))
	{
		venture_feed_batch_set_not_modified(batch, TRUE);
		return g_steal_pointer(&batch);
	}

	note = g_strdup_printf("the index lists %u connected realms (%u new, %u gone); each is a unit "
	                       "of its own from the next pass", ids->len, added, removed);
	venture_feed_batch_add_note(batch, note);

	return g_steal_pointer(&batch);
}

/* One unit: a realm, the commodity market, or the realm index. */
static VentureFeedBatch *
blizzard_fetch(
	VentureFeedRequest	 *request,
	GError			**error
){
	g_autoptr(VentureFeedBatch) batch = NULL;
	g_autoptr(GHashTable) seen = NULL;
	BlizzardFrozen *frozen;
	const gchar *unit;
	guint status;

	frozen = venture_feed_request_get_frozen(request);
	unit = venture_feed_request_get_unit(request);

	if (NULL == frozen)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
		                    "The blizzard_auctions provider was not frozen");
		return NULL;
	}

	if (0 == g_strcmp0(unit, BLIZZARD_UNIT_REALM_INDEX))
		return blizzard_fetch_realm_index(request, frozen, error);

	/* Anything else is the commodity market or a realm's id: a Test of a
	 * unit named otherwise ("realms", from before) is refused here, not
	 * sent to Battle.net as a path. */
	{
		gint64 id = 0;

		if ((0 != g_strcmp0(unit, BLIZZARD_UNIT_COMMODITIES)) &&
		    ((NULL == unit) || !g_ascii_string_to_signed(unit, 10, 1, G_MAXINT32, &id, NULL)))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
			            "blizzard_auctions has no unit %s: a connected realm id, %s or %s",
			            (NULL != unit) ? unit : "(none)", BLIZZARD_UNIT_COMMODITIES,
			            BLIZZARD_UNIT_REALM_INDEX);
			return NULL;
		}
	}

	batch = venture_feed_batch_new();
	seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, blizzard_seen_free);

	if (!blizzard_fetch_venue(request, frozen, batch, unit, FALSE, VENTURE_SERIES_NONE, seen, &status, NULL,
	                          error))
		return NULL;

	if (304 == status)
	{
		venture_feed_batch_set_not_modified(batch, TRUE);
		return g_steal_pointer(&batch);
	}

	blizzard_add_instruments(request, frozen, batch, seen);

	return g_steal_pointer(&batch);
}

/* ==========================================================================
 * The provider
 * ========================================================================== */

#define BLIZZARD_TYPE_PROVIDER (blizzard_provider_get_type())
G_DECLARE_FINAL_TYPE(BlizzardProvider, blizzard_provider, BLIZZARD, PROVIDER, GObject)

struct _BlizzardProvider
{
	GObject parent_instance;
};

static void blizzard_provider_iface_init(VentureDataSourceProviderInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(BlizzardProvider, blizzard_provider, G_TYPE_OBJECT,
                              G_IMPLEMENT_INTERFACE(VENTURE_TYPE_DATA_SOURCE_PROVIDER,
                                                    blizzard_provider_iface_init))

static void
blizzard_provider_class_init(BlizzardProviderClass *klass)
{
	(void)klass;
}

static void
blizzard_provider_init(BlizzardProvider *self)
{
	(void)self;
}

static const gchar *
blizzard_provider_name(VentureDataSourceProvider *provider)
{
	(void)provider;

	return BLIZZARD_PROVIDER_NAME;
}

static const gchar *
blizzard_provider_label(VentureDataSourceProvider *provider)
{
	(void)provider;

	return "World of Warcraft auction houses (Battle.net)";
}

/*
 * Blizzard's API terms (section 2.13) require the application to name
 * Blizzard, clearly and conspicuously, as the source of the data -- and
 * to do it so the application does not appear endorsed by or affiliated
 * with Blizzard. Every page that shows this provider's data shows this
 * line, which says both halves.
 */
static const gchar *
blizzard_provider_attribution(VentureDataSourceProvider *provider)
{
	(void)provider;

	return BLIZZARD_ATTRIBUTION;
}

/*
 * The settings, as the data source form and docs show them. client_secret
 * is the one credential: marked sensitive, it is refused in the settings
 * YAML and sealed in the integration store on the source's credentials
 * page. requests_per_hour and record_types are the keys every source
 * shares; Blizzard's budget is 36,000 an hour per client.
 */
static JsonNode *
blizzard_provider_schema(VentureDataSourceProvider *provider)
{
	(void)provider;

	return json_from_string(
		"{\"type\":\"object\",\"properties\":{"
		"\"region\":{\"type\":\"string\",\"title\":\"Region\",\"enum\":[\"us\",\"eu\",\"kr\",\"tw\"],"
		"\"default\":\"us\"},"
		"\"client_id\":{\"type\":\"string\",\"title\":\"Client id\","
		"\"description\":\"The id of a client made at develop.battle.net; not a secret\"},"
		"\"client_secret\":{\"type\":\"string\",\"title\":\"Client secret\",\"x-sensitive\":true},"
		"\"connected_realm_ids\":{\"type\":\"array\",\"title\":\"Connected realms\","
		"\"description\":\"Connected realm ids, each its own unit. Empty: every realm the region's "
		"index lists, each its own unit all the same, and the index read again daily (realm_index_hours) "
		"so a realm Blizzard adds is picked up\"},"
		"\"include_commodities\":{\"type\":\"boolean\",\"title\":\"Commodity market\",\"default\":true},"
		"\"include_bid_only\":{\"type\":\"boolean\",\"title\":\"List bid-only auctions at their bid\","
		"\"default\":false,\"description\":\"An auction with a bid and no buyout cannot be bought now; "
		"listed, its bid becomes the lowest price Deals and Browse show\"},"
		"\"currency\":{\"type\":\"string\",\"title\":\"Currency\",\"default\":\"GOLD\","
		"\"description\":\"A registered currency with exponent 4: prices are in copper\"},"
		"\"locale\":{\"type\":\"string\",\"title\":\"Locale\",\"default\":\"en_US\"},"
		"\"item_names_per_fetch\":{\"type\":\"integer\",\"title\":\"Item names per fetch\",\"default\":100,"
		"\"minimum\":0,\"maximum\":5000,\"description\":\"Item lookups one fetch may make, 0 to 5000; "
		"each costs up to 3 requests (item, media, icon), so 5000 is up to 15,000 of the hour's 36,000\"},"
		"\"realm_index_hours\":{\"type\":\"integer\",\"title\":\"Hours between realm index reads\","
		"\"default\":24,\"minimum\":0,\"maximum\":168,\"description\":\"With no realms listed, how "
		"old the known realm list may grow before it is read again; 0 reads it whenever its unit runs\"},"
		"\"key_modifier_types\":{\"type\":\"array\",\"title\":\"Modifier types that make a variant\"},"
		"\"dynamic_namespace\":{\"type\":\"string\",\"title\":\"Dynamic namespace override\"},"
		"\"static_namespace\":{\"type\":\"string\",\"title\":\"Static namespace override\"},"
		"\"api_base\":{\"type\":\"string\",\"title\":\"API origin override\",\"x-endpoint-override\":true},"
		"\"oauth_base\":{\"type\":\"string\",\"title\":\"OAuth origin override\",\"x-endpoint-override\":true},"
		"\"requests_per_hour\":{\"type\":\"integer\",\"title\":\"Request budget an hour\",\"default\":36000}"
		"}}", NULL);
}

/*
 * Main thread. The settings are judged again (they were when the source
 * was saved), and the currency is held to what the prices need: a
 * registered code with exponent 4. The provider never registers one --
 * that is the operator's (or the demo's) to do, as a `currency` record.
 */
static gpointer
blizzard_provider_freeze(
	VentureDataSourceProvider	 *provider,
	VentureContext			 *context,
	JsonObject			 *settings,
	GDestroyNotify			 *out_free,
	GError				**error
){
	g_autoptr(BlizzardSettings) parsed = NULL;
	BlizzardFrozen *frozen;

	(void)provider;
	(void)context;

	parsed = blizzard_settings_parse(settings, error);

	if (NULL == parsed)
		return NULL;

	if (!venture_currency_is_registered(parsed->currency) ||
	    (BLIZZARD_CURRENCY_EXPONENT != venture_currency_get_exponent(parsed->currency)))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		            "Blizzard prices are in copper, so %s must be a registered currency with "
		            "exponent 4 (1 gold = 10000 copper); register it, or name another in the "
		            "settings' currency", parsed->currency);
		return NULL;
	}

	frozen = blizzard_frozen_new(g_steal_pointer(&parsed));
	*out_free = blizzard_frozen_free;

	return frozen;
}

/*
 * The connected realms the settings name; else every realm of the last
 * known index, as the same unit names, and the realm-index unit that keeps
 * that list current; then the commodity market. Reads the kept list (a
 * small file) but never the network: this runs on the main thread when a
 * source is saved or frozen. See "The realm list".
 */
static gchar **
blizzard_provider_units(
	VentureDataSourceProvider	 *provider,
	JsonObject			 *settings,
	GError				**error
){
	g_autoptr(BlizzardSettings) parsed = NULL;
	g_autoptr(GPtrArray) units = NULL;
	guint i;

	(void)provider;

	parsed = blizzard_settings_parse(settings, error);

	if (NULL == parsed)
		return NULL;

	units = g_ptr_array_new_with_free_func(g_free);

	for (i = 0; i < parsed->realm_ids->len; i++)
		g_ptr_array_add(units, g_strdup_printf("%" G_GINT64_FORMAT,
		                                       g_array_index(parsed->realm_ids, gint64, i)));

	if (0 == parsed->realm_ids->len)
	{
		g_autoptr(GArray) known = NULL;

		if (blizzard_realms_load(parsed, &known, NULL))
		{
			for (i = 0; i < known->len; i++)
				g_ptr_array_add(units, g_strdup_printf("%" G_GINT64_FORMAT,
				                                       g_array_index(known, gint64, i)));
		}

		g_ptr_array_add(units, g_strdup(BLIZZARD_UNIT_REALM_INDEX));
	}

	if (parsed->include_commodities)
		g_ptr_array_add(units, g_strdup(BLIZZARD_UNIT_COMMODITIES));

	g_ptr_array_add(units, NULL);

	return (gchar **)g_ptr_array_free(g_steal_pointer(&units), FALSE);
}

static void
blizzard_provider_thread(
	GTask		*task,
	gpointer	 source,
	gpointer	 task_data,
	GCancellable	*cancellable
){
	VentureFeedRequest *request = task_data;
	VentureFeedBatch *batch;
	GError *error = NULL;

	(void)source;
	(void)cancellable;

	batch = blizzard_fetch(request, &error);

	if (NULL == batch)
	{
		g_task_return_error(task, error);
		return;
	}

	g_task_return_pointer(task, batch, (GDestroyNotify)venture_feed_batch_unref);
}

/* A GTask thread from GLib's pool: every request below blocks, and the
 * feeds worker's own loop must not. */
static void
blizzard_provider_fetch_async(
	VentureDataSourceProvider	*provider,
	VentureFeedRequest		*request,
	GCancellable			*cancellable,
	GAsyncReadyCallback		 callback,
	gpointer			 user_data
){
	g_autoptr(GTask) task = NULL;

	task = g_task_new(provider, cancellable, callback, user_data);
	g_task_set_task_data(task, g_object_ref(request), g_object_unref);
	g_task_run_in_thread(task, blizzard_provider_thread);
}

static void
blizzard_provider_iface_init(VentureDataSourceProviderInterface *iface)
{
	iface->get_name = blizzard_provider_name;
	iface->get_label = blizzard_provider_label;
	iface->dup_settings_schema = blizzard_provider_schema;
	iface->freeze = blizzard_provider_freeze;
	iface->list_units = blizzard_provider_units;
	iface->fetch_async = blizzard_provider_fetch_async;
	iface->get_attribution = blizzard_provider_attribution;
}

VentureDataSourceProvider *
blizzard_provider_new(void)
{
	return VENTURE_DATA_SOURCE_PROVIDER(g_object_new(BLIZZARD_TYPE_PROVIDER, NULL));
}

#endif /* VENTURE_HAVE_SQLITE */
