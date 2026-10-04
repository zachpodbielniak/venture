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
 *   realms                every connected realm the index lists, a few
 *                         with new data per fetch, when the settings name
 *                         none (see blizzard_fetch_all_realms())
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
	gint64 names;
	gint64 realms;

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

	if (!blizzard_parse_bool(settings, "include_commodities", TRUE, &self->include_commodities, error) ||
	    !blizzard_parse_bool(settings, "include_bid_only", TRUE, &self->include_bid_only, error))
		return NULL;

	names = 100;
	realms = 10;

	if (!blizzard_parse_count(settings, "item_names_per_fetch", names, 0, BLIZZARD_MAX_NAMES_PER_FETCH,
	                          &self->item_names_per_fetch, error) ||
	    !blizzard_parse_count(settings, "realms_per_fetch", realms, 1, BLIZZARD_MAX_REALMS_PER_FETCH,
	                          &self->realms_per_fetch, error))
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
	*out_item = (NULL != found) ? blizzard_item_copy(found) : NULL;
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

		if (!JSON_NODE_HOLDS_OBJECT(element))
			continue;

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

			if ((total < 0) && frozen->settings->include_bid_only)
			{
				total = blizzard_int(auction, "bid", -1);
				bid_only += (total >= 0) ? 1 : 0;
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

	if (bid_only > 0)
	{
		g_autofree gchar *note = g_strdup_printf("%s: %" G_GINT64_FORMAT " auctions have a bid and no "
		                                         "buyout; listed at their bid", venue_key, bid_only);

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
static void
blizzard_add_instruments(
	VentureFeedRequest	*request,
	BlizzardFrozen		*frozen,
	VentureFeedBatch	*batch,
	GHashTable		*seen
){
	g_autoptr(GPtrArray) keys = NULL;
	g_autoptr(GArray) unknown = NULL;
	g_autoptr(GHashTable) plain = NULL;
	GHashTableIter iter;
	gpointer key;
	gpointer value;
	guint fetched;
	guint i;

	keys = g_ptr_array_new();
	unknown = g_array_new(FALSE, FALSE, sizeof(gint64));
	plain = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_hash_table_iter_init(&iter, seen);

	g_mutex_lock(&frozen->lock);

	while (g_hash_table_iter_next(&iter, &key, &value))
	{
		BlizzardSeen *entry = value;
		guint j;
		gboolean listed = FALSE;

		g_ptr_array_add(keys, key);

		if (g_hash_table_contains(frozen->items, &entry->item_id))
			continue;

		for (j = 0; j < unknown->len; j++)
			listed = listed || (g_array_index(unknown, gint64, j) == entry->item_id);

		if (!listed)
			g_array_append_val(unknown, entry->item_id);
	}

	g_mutex_unlock(&frozen->lock);

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
				venture_feed_batch_add_instrument(batch, parent,
				                                  (NULL != item) ? item->name : NULL, "item",
				                                  (NULL != item) ? item->category : NULL,
				                                  NULL, NULL, NULL);
				g_hash_table_add(plain, g_strdup(parent));
			}
		}

		if ((NULL != item) && (NULL != item->name))
		{
			g_autoptr(JsonObject) attrs = NULL;
			g_autoptr(JsonNode) node = NULL;
			gint64 species = 0;

			attrs = (NULL != entry->variant) ? json_object_ref(entry->variant) : json_object_new();
			json_object_set_int_member(attrs, "item_id", entry->item_id);

			if (item->vendor_sell >= 0)
				json_object_set_int_member(attrs, "vendor_sell", item->vendor_sell);

			if (NULL != item->quality)
				json_object_set_string_member(attrs, "quality", item->quality);

			if (item->level > 0)
				json_object_set_int_member(attrs, "level", item->level);

			node = json_node_new(JSON_NODE_OBJECT);
			json_node_set_object(node, attrs);
			attrs_json = json_to_string(node, FALSE);

			/* A caged pet is its cage's item; the species says which. */
			{
				gint64 item_id;

				if (!blizzard_item_key_parse(instrument, &item_id, &species))
					species = 0;
			}
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
	g_autofree gchar *attrs = NULL;

	attrs = (0 == g_strcmp0(key, BLIZZARD_UNIT_COMMODITIES))
		? g_strdup_printf("{\"region\":\"%s\",\"commodities\":true}", frozen->settings->region)
		: g_strdup_printf("{\"region\":\"%s\",\"connected_realm_id\":%s}", frozen->settings->region,
		                  key);

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

/*
 * The every-realm unit, for a source whose settings name no realm: the
 * connected-realm index, then the realms in id order from where the last
 * fetch stopped, each with its own If-Modified-Since, until
 * realms_per_fetch of them had new data. 304s are cheap -- one request,
 * no body -- so a fetch walks past every realm that has not updated; it
 * stops early only to bound how much one batch holds. Its cursor is
 *
 *     {"after": <last realm id read>, "seen": {"<id>": <Last-Modified>, ...}}
 *
 * Listing realms in connected_realm_ids instead gives each its own unit,
 * schedule and adaptive polling, which is what a large region wants.
 */
static VentureFeedBatch *
blizzard_fetch_all_realms(
	VentureFeedRequest	 *request,
	BlizzardFrozen		 *frozen,
	GError			**error
){
	g_autoptr(VentureFeedBatch) batch = NULL;
	g_autoptr(JsonNode) listing = NULL;
	g_autoptr(JsonNode) cursor = NULL;
	g_autoptr(JsonObject) seen_times = NULL;
	g_autoptr(GArray) ids = NULL;
	g_autoptr(GHashTable) seen = NULL;
	g_autofree gchar *url = NULL;
	JsonArray *realms;
	JsonObject *previous;
	gint64 after;
	guint status;
	guint start;
	guint with_data;
	guint k;
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

	/* The index lists hrefs; the id is the path segment after
	 * connected-realm/. */
	ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	realms = blizzard_array(json_node_get_object(listing), "connected_realms");

	for (i = 0; (NULL != realms) && (i < json_array_get_length(realms)); i++)
	{
		JsonNode *element = json_array_get_element(realms, i);
		const gchar *href;
		const gchar *at;
		gint64 id;

		href = JSON_NODE_HOLDS_OBJECT(element)
			? venture_json_object_get_string(json_node_get_object(element), "href", NULL) : NULL;
		at = (NULL != href) ? strstr(href, "/connected-realm/") : NULL;
		id = (NULL != at) ? g_ascii_strtoll(at + strlen("/connected-realm/"), NULL, 10) : 0;

		if ((id > 0) && (id <= G_MAXINT32))
			g_array_append_val(ids, id);
	}

	g_array_sort(ids, blizzard_compare_int64);

	if ((0 == ids->len) || (ids->len > BLIZZARD_MAX_REALMS))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_SERIALIZATION,
		            "The connected realm index lists %u realms", ids->len);
		return NULL;
	}

	/* Where the last fetch stopped, and each realm's Last-Modified. */
	cursor = (NULL != venture_feed_request_get_cursor(request))
		? json_from_string(venture_feed_request_get_cursor(request), NULL) : NULL;
	previous = ((NULL != cursor) && JSON_NODE_HOLDS_OBJECT(cursor)) ? json_node_get_object(cursor) : NULL;
	after = blizzard_int(previous, "after", 0);
	seen_times = json_object_new();

	batch = venture_feed_batch_new();
	seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, blizzard_seen_free);

	for (start = 0; (start < ids->len) && (g_array_index(ids, gint64, start) <= after); start++)
		;

	with_data = 0;

	for (k = 0; k < ids->len; k++)
	{
		g_autoptr(GError) local_error = NULL;
		g_autofree gchar *key = NULL;
		gint64 id;
		gint64 since;
		gint64 last_modified = VENTURE_SERIES_NONE;

		id = g_array_index(ids, gint64, (start + k) % ids->len);
		key = g_strdup_printf("%" G_GINT64_FORMAT, id);
		since = blizzard_int(blizzard_object(previous, "seen"), key, VENTURE_SERIES_NONE);

		if (!blizzard_fetch_venue(request, frozen, batch, key, TRUE, since, seen, &status, &last_modified,
		                          &local_error))
		{
			/* A realm that went away is a note; anything else stops the
			 * walk, keeping what was read as a partial batch. */
			if (g_error_matches(local_error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND))
			{
				venture_feed_batch_add_note(batch, local_error->message);
				after = id;
				continue;
			}

			if (0 == with_data)
			{
				g_propagate_error(error, g_steal_pointer(&local_error));
				return NULL;
			}

			venture_feed_batch_set_error(batch, local_error->message, 0);
			break;
		}

		after = id;

		if ((200 == status) && (VENTURE_SERIES_NONE != last_modified))
			json_object_set_int_member(seen_times, key, last_modified);
		else if (VENTURE_SERIES_NONE != since)
			json_object_set_int_member(seen_times, key, since);

		if ((200 == status) && (++with_data >= frozen->settings->realms_per_fetch))
			break;
	}

	/* Realms not reached this time keep the times they had. */
	for (i = 0; i < ids->len; i++)
	{
		g_autofree gchar *key = g_strdup_printf("%" G_GINT64_FORMAT, g_array_index(ids, gint64, i));
		gint64 kept = blizzard_int(blizzard_object(previous, "seen"), key, VENTURE_SERIES_NONE);

		if (!json_object_has_member(seen_times, key) && (VENTURE_SERIES_NONE != kept))
			json_object_set_int_member(seen_times, key, kept);
	}

	{
		g_autoptr(JsonObject) next = json_object_new();
		g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
		g_autofree gchar *text = NULL;

		json_object_set_int_member(next, "after", after);
		json_object_set_object_member(next, "seen", json_object_ref(seen_times));
		json_node_set_object(node, next);
		text = json_to_string(node, FALSE);
		venture_feed_batch_set_cursor(batch, text);
	}

	if (0 == with_data)
	{
		venture_feed_batch_set_not_modified(batch, TRUE);
		return g_steal_pointer(&batch);
	}

	blizzard_add_instruments(request, frozen, batch, seen);

	return g_steal_pointer(&batch);
}

/* One unit: a realm, the commodity market, or every realm. */
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

	if (0 == g_strcmp0(unit, BLIZZARD_UNIT_ALL_REALMS))
		return blizzard_fetch_all_realms(request, frozen, error);

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
		"\"description\":\"Connected realm ids, each its own unit; empty reads every realm the index lists\"},"
		"\"include_commodities\":{\"type\":\"boolean\",\"title\":\"Commodity market\",\"default\":true},"
		"\"include_bid_only\":{\"type\":\"boolean\",\"title\":\"List bid-only auctions at their bid\","
		"\"default\":true},"
		"\"currency\":{\"type\":\"string\",\"title\":\"Currency\",\"default\":\"GOLD\","
		"\"description\":\"A registered currency with exponent 4: prices are in copper\"},"
		"\"locale\":{\"type\":\"string\",\"title\":\"Locale\",\"default\":\"en_US\"},"
		"\"item_names_per_fetch\":{\"type\":\"integer\",\"title\":\"Item names per fetch\",\"default\":100},"
		"\"realms_per_fetch\":{\"type\":\"integer\",\"title\":\"Realms with new data per fetch\","
		"\"default\":10},"
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

/* The connected realms the settings name, else the every-realm unit; then
 * the commodity market. */
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
		g_ptr_array_add(units, g_strdup(BLIZZARD_UNIT_ALL_REALMS));

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
