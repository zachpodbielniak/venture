/*
 * venture-marketdata-accounts.c - What the account operations pages show
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Each public function answers one /accounts page's question as JSON,
 * and every door -- the page, its /api/v1/accounts twin, the reports, the
 * widgets and the CLI -- reads that one answer. See the header for the
 * shapes.
 *
 * The heavy reads are the store's: holdings are valued, summed, sorted
 * and paged in one SQL statement (venture_series_store_value_*), the
 * ledger is summed by SQLite in integers, balances arrive one row per
 * account and day. What is computed here is per account -- the attention
 * list, the account table's order -- and an operator has tens of
 * accounts, never more than the store's account cap.
 *
 * Nothing here converts a currency. A source values its holdings and
 * sums its ledger in its own currency; across sources the figures are
 * kept per currency, as every multi-currency total in VENTURE is.
 */

#include "venture.h"

#include <math.h>
#include <string.h>

#define MA_DAY (G_GINT64_CONSTANT(86400))
#define MA_HOUR (G_GINT64_CONSTANT(3600))
#define MA_MAX_SOURCES (500)
#define MA_LEDGER_DEFAULT (25)
#define MA_HISTORY_DAYS (90)
#define MA_PNL_DAYS (30)
#define MA_TOP_DEFAULT (10)
#define MA_MAX_PAGE_NUMBER (1000000)
#define MA_MAX_HOURS VENTURE_MARKETDATA_ACCOUNTS_MAX_EXPIRING_HOURS
#define MA_MAX_MAIL_DAYS VENTURE_MARKETDATA_ACCOUNTS_MAX_MAIL_DAYS
#define MA_MAX_STALE_DAYS VENTURE_MARKETDATA_ACCOUNTS_MAX_STALE_DAYS
#define MA_MAX_DEAD_DAYS (3650)

static const gchar *const ma_bases[] = {
	"conservative", "market", "min", "historical", "region_market", "region_sale_avg", NULL
};

static const gchar *const ma_account_sorts[] = {
	"attention", "name", "realm", "gold", "positions", "expiry", "inbound", "last_seen",
	"freshness", "login", NULL
};

static const gchar *const ma_inventory_sorts[] = {
	"value", "quantity", "name", "unit_value", "accounts", "days_of_supply", NULL
};

static const gchar *const ma_inventory_groups[] = {
	"login", NULL
};

static const gchar *const ma_pnl_groups[] = {
	"day", "week", "month", "account", "venue", "instrument", "source", "login", NULL
};

/* Where things sit, in the order a person looks for them. */
static const struct
{
	const gchar	*place;
	const gchar	*label;
} ma_places[] = {
	{ "bag", "Bags" },
	{ "bank", "Bank" },
	{ "reagent_bank", "Reagent bank" },
	{ "warbank", "Warband bank" },
	{ "guild", "Guild bank" },
	{ "mail", "Mail" },
	{ "auction", "On auction" },
	{ "void", "Void storage" },
	{ "equipped", "Equipped" },
	{ "currency", "Currencies" },
	{ "other", "Elsewhere" },
};

const gchar *const *
venture_marketdata_accounts_bases(void)
{
	return ma_bases;
}

const gchar *const *
venture_marketdata_accounts_sorts(void)
{
	return ma_account_sorts;
}

const gchar *const *
venture_marketdata_inventory_sorts(void)
{
	return ma_inventory_sorts;
}

void
venture_marketdata_accounts_query_init(VentureMarketdataAccountsQuery *query)
{
	g_return_if_fail(NULL != query);

	memset(query, 0, sizeof(*query));
}

void
venture_marketdata_inventory_query_init(VentureMarketdataInventoryQuery *query)
{
	g_return_if_fail(NULL != query);

	memset(query, 0, sizeof(*query));
	query->descending = TRUE;
}

void
venture_marketdata_pnl_query_init(VentureMarketdataPnlQuery *query)
{
	g_return_if_fail(NULL != query);

	memset(query, 0, sizeof(*query));
}

gchar *
venture_marketdata_account_path(
	gint64		 data_source_id,
	const gchar	*key
){
	g_autofree gchar *escaped = NULL;

	/* Every reserved character escaped, "/" included: the key is the
	 * rest of the path whatever it holds. */
	escaped = g_uri_escape_string((NULL != key) ? key : "", NULL, FALSE);

	return g_strdup_printf("/accounts/%" G_GINT64_FORMAT "/%s", data_source_id, escaped);
}

/* --- JSON helpers ---------------------------------------------------------- */

static gint64
ma_now(gint64 now)
{
	return (now > 0) ? now : g_get_real_time() / G_USEC_PER_SEC;
}

/*
 * The thirty days that end with @now's day: whole days, the way
 * venture_date_range_parse() reads "last_30_days" -- which day it is is
 * read in the configured zone, every boundary is a midnight UTC. A window
 * of now minus thirty times a day left the P&L's default and the same
 * page's "last 30 days" disagreeing about a sale at half past midnight
 * thirty days back, and moved with every second.
 */
static void
ma_thirty_days(
	VentureContext	*context,
	gint64		 now,
	gint64		*since,
	gint64		*until
){
	g_autoptr(GDateTime) moment = NULL;
	g_autoptr(GDateTime) local = NULL;
	g_autoptr(GDateTime) today = NULL;
	GTimeZone *zone;

	zone = venture_context_get_timezone(context);
	moment = g_date_time_new_from_unix_utc(now);
	local = (NULL != zone) ? g_date_time_to_timezone(moment, zone) : g_date_time_ref(moment);
	today = g_date_time_new_utc(g_date_time_get_year(local), g_date_time_get_month(local),
	                            g_date_time_get_day_of_month(local), 0, 0, 0.0);
	*until = g_date_time_to_unix(today) + MA_DAY;
	*since = *until - MA_PNL_DAYS * MA_DAY;
}

static JsonNode *
ma_node(JsonObject *root)
{
	JsonNode *node;

	node = json_node_new(JSON_NODE_OBJECT);
	json_node_take_object(node, root);

	return node;
}

static void
ma_set_text(
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
ma_note(
	JsonArray	*notes,
	const gchar	*text
){
	json_array_add_string_element(notes, text);
}

static gboolean
ma_in(
	const gchar		*value,
	const gchar *const	*choices
){
	return (NULL != value) && g_strv_contains(choices, value);
}

/* "one, two or three": the choices a refusal names. */
static gchar *
ma_choices(const gchar *const *choices)
{
	g_autoptr(GString) text = NULL;
	guint i;
	guint n;

	text = g_string_new(NULL);
	n = g_strv_length((gchar **)choices);

	for (i = 0; i < n; i++)
	{
		if (i > 0)
			g_string_append(text, (i + 1 == n) ? " or " : ", ");

		g_string_append(text, choices[i]);
	}

	return g_string_free(g_steal_pointer(&text), FALSE);
}

static gboolean
ma_check_choice(
	const gchar		 *value,
	const gchar *const	 *choices,
	const gchar		 *what,
	GError			**error
){
	g_autofree gchar *names = NULL;

	if ((NULL == value) || ma_in(value, choices))
		return TRUE;

	names = ma_choices(choices);
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
	            "%s is %s, not \"%s\"", what, names, value);

	return FALSE;
}

#ifdef VENTURE_HAVE_SQLITE

/* A duration in the unit a person reads it in: minutes, hours, days. */
static gchar *
ma_span(gint64 seconds)
{
	if (seconds < 0)
		seconds = -seconds;

	if (seconds < 90 * 60)
		return g_strdup_printf("%" G_GINT64_FORMAT " min", MAX(seconds / 60, 1));

	if (seconds < 36 * MA_HOUR)
		return g_strdup_printf("%" G_GINT64_FORMAT " h", (seconds + MA_HOUR / 2) / MA_HOUR);

	return g_strdup_printf("%" G_GINT64_FORMAT " d", (seconds + MA_DAY / 2) / MA_DAY);
}

/* "in 3 h" or "3 h ago", against @now. */
static gchar *
ma_when(
	gint64	at,
	gint64	now
){
	g_autofree gchar *span = NULL;

	span = ma_span(at - now);

	return (at > now) ? g_strdup_printf("in %s", span) : g_strdup_printf("%s ago", span);
}

/* A class or race the source writes in capitals, the way a person does. */
static gchar *
ma_title_case(const gchar *text)
{
	g_autofree gchar *lower = NULL;
	gchar *out;
	gboolean start;
	gchar *p;

	lower = g_utf8_strdown(text, -1);
	out = g_strdup(lower);
	start = TRUE;

	for (p = out; '\0' != *p; p++)
	{
		if (('_' == *p) || (' ' == *p) || ('-' == *p))
		{
			*p = ' ';
			start = TRUE;
			continue;
		}

		if (start && g_ascii_islower(*p))
			*p = g_ascii_toupper(*p);

		start = FALSE;
	}

	return out;
}

#endif /* VENTURE_HAVE_SQLITE */

static gboolean
ma_require_module(
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

/* Whether there is a store to read at all, and a note saying why not. */
static gboolean
ma_series_ready(
	VentureContext	*context,
	JsonArray	*notes
){
#ifndef VENTURE_HAVE_SQLITE
	(void)context;
	ma_note(notes, "No data sources: this build has no series store (it was built without SQLite).");
	return FALSE;
#else
	if (!venture_context_module_enabled(context, "feeds"))
	{
		ma_note(notes, "No data sources: market data feeds are off (feeds.enabled).");
		return FALSE;
	}

	return TRUE;
#endif
}

/* The answer's `attribution`: the lines of the sources it shows. */
static void
ma_attribute(
	VentureContext	*context,
	gint64		 organization_id,
	JsonObject	*root,
	GArray		*shown
){
	g_autoptr(GPtrArray) attributions = NULL;
	guint i;

	attributions = g_ptr_array_new_with_free_func(g_free);

	for (i = 0; (NULL != shown) && (i < shown->len); i++)
		venture_marketdata_attribution_add_source(context, organization_id,
		                                          g_array_index(shown, gint64, i), attributions);

	venture_marketdata_attribution_set(root, attributions);
}

/* --- Money kept per currency ------------------------------------------------- */

#ifdef VENTURE_HAVE_SQLITE

/* Minor units as a money node, or null when unknown. */
static JsonNode *
ma_money_node(
	gint64		 minor,
	const gchar	*currency
){
	g_autoptr(VentureMoney) money = NULL;

	if ((VENTURE_SERIES_NONE == minor) || venture_string_is_empty(currency) ||
	    !venture_currency_is_valid(currency))
		return json_node_new(JSON_NODE_NULL);

	money = venture_money_new_for_currency(minor, currency);

	if (NULL == money)
		return json_node_new(JSON_NODE_NULL);

	return venture_money_to_json(money);
}

static void
ma_set_money(
	JsonObject	*object,
	const gchar	*name,
	gint64		 minor,
	const gchar	*currency
){
	json_object_set_member(object, name, ma_money_node(minor, currency));
}

/* The money as a person reads it, or NULL when it cannot be said. */
static gchar *
ma_money_text(
	gint64		 minor,
	const gchar	*currency
){
	g_autoptr(VentureMoney) money = NULL;

	if ((VENTURE_SERIES_NONE == minor) || venture_string_is_empty(currency) ||
	    !venture_currency_is_valid(currency))
		return NULL;

	money = venture_money_new_for_currency(minor, currency);

	return (NULL != money) ? venture_money_to_display_string(money, TRUE) : NULL;
}

static void
ma_set_figure(
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
ma_set_ratio(
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
ma_set_time(
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
 * Adds @value in @currency to the per-currency @amounts, refusing an
 * overflow: a total that wrapped would be a figure, and a wrong one.
 */
static gboolean
ma_amounts_add(
	GArray		 *amounts,
	const gchar	 *currency,
	gint64		  value,
	GError		**error
){
	VentureSeriesAmount amount;
	guint i;

	if (venture_string_is_empty(currency) || (VENTURE_SERIES_NONE == value))
		return TRUE;

	for (i = 0; i < amounts->len; i++)
	{
		VentureSeriesAmount *have = &g_array_index(amounts, VentureSeriesAmount, i);

		if (0 != g_ascii_strcasecmp(have->currency, currency))
			continue;

		if (!venture_series_math_add(have->amount, value, &have->amount))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "A total in %s does not fit in a 64-bit integer", currency);
			return FALSE;
		}

		return TRUE;
	}

	memset(&amount, 0, sizeof(amount));
	g_strlcpy(amount.currency, currency, sizeof(amount.currency));
	amount.amount = value;
	g_array_append_val(amounts, amount);

	return TRUE;
}

static GArray *
ma_amounts_new(void)
{
	return g_array_new(FALSE, TRUE, sizeof(VentureSeriesAmount));
}

/* The amount in @currency, or NONE. */
static gint64
ma_amounts_get(
	GArray		*amounts,
	const gchar	*currency
){
	guint i;

	for (i = 0; (NULL != amounts) && (NULL != currency) && (i < amounts->len); i++)
	{
		VentureSeriesAmount *have = &g_array_index(amounts, VentureSeriesAmount, i);

		if (0 == g_ascii_strcasecmp(have->currency, currency))
			return have->amount;
	}

	return VENTURE_SERIES_NONE;
}

static gint
ma_compare_amounts(
	gconstpointer	a,
	gconstpointer	b
){
	return g_strcmp0(((const VentureSeriesAmount *)a)->currency,
	                 ((const VentureSeriesAmount *)b)->currency);
}

/* Money objects, one per currency, @first's first, the rest by code. */
static JsonArray *
ma_amounts_json(
	GArray		*amounts,
	const gchar	*first
){
	JsonArray *array;
	guint i;

	array = json_array_new();

	if (NULL == amounts)
		return array;

	g_array_sort(amounts, ma_compare_amounts);

	for (i = 0; (NULL != first) && (i < amounts->len); i++)
	{
		VentureSeriesAmount *have = &g_array_index(amounts, VentureSeriesAmount, i);

		if (0 == g_ascii_strcasecmp(have->currency, first))
			json_array_add_element(array, ma_money_node(have->amount, have->currency));
	}

	for (i = 0; i < amounts->len; i++)
	{
		VentureSeriesAmount *have = &g_array_index(amounts, VentureSeriesAmount, i);

		if ((NULL == first) || (0 != g_ascii_strcasecmp(have->currency, first)))
			json_array_add_element(array, ma_money_node(have->amount, have->currency));
	}

	return array;
}

/* --- Sources ------------------------------------------------------------------ */

/* The organization's live sources, by name. */
static GPtrArray *
ma_sources(
	VentureContext	 *context,
	gint64		  organization_id,
	GError		**error
){
	g_autoptr(VentureQuery) query = NULL;

	query = venture_query_new(VENTURE_TYPE_DATA_SOURCE);
	venture_query_set_organization(query, organization_id);
	venture_query_add_order(query, "name", VENTURE_SORT_ASCENDING, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	venture_query_set_limit(query, MA_MAX_SOURCES);

	return venture_database_find(venture_context_get_database(context), query, error);
}

/* One source, the organization's and live: NOT_FOUND says nothing about
 * whether another organization has one by that id. */
static VentureEntity *
ma_source(
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
ma_source_name(VentureEntity *source)
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

/* The source's currency, upper case, or NULL when it names none. */
static gchar *
ma_source_currency(VentureEntity *source)
{
	g_autofree gchar *currency = NULL;

	g_object_get(source, "currency", &currency, NULL);

	if (venture_string_is_empty(currency))
		return NULL;

	return g_ascii_strup(currency, -1);
}

/*
 * A read handle on a source's store. One that has stored nothing, or
 * cannot be read, is a note when @notes is given -- one bad store must
 * not take a page of several down -- and an error otherwise.
 */
static VentureSeriesStore *
ma_reader(
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

	name = ma_source_name(source);
	service = venture_context_get_feeds_service(context);

	if (NULL == service)
		g_set_error_literal(&local_error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		                    "Market data feeds are off (feeds.enabled)");
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
	ma_note(notes, text);

	return NULL;
}

/* venue key -> its name, for every venue the store knows. */
static GHashTable *
ma_venues(
	VentureSeriesStore	 *store,
	gchar			**out_default_group,
	GError			**error
){
	g_autoptr(GPtrArray) venues = NULL;
	g_autoptr(GHashTable) groups = NULL;
	GHashTable *names;
	guint i;

	*out_default_group = NULL;
	venues = venture_series_store_list_venues(store, error);

	if (NULL == venues)
		return NULL;

	names = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	groups = g_hash_table_new(g_str_hash, g_str_equal);

	for (i = 0; i < venues->len; i++)
	{
		VentureSeriesVenueRow *venue = g_ptr_array_index(venues, i);

		g_hash_table_insert(names, g_strdup(venue->key),
		                    g_strdup((NULL != venue->name) ? venue->name : venue->key));

		/* The region's own statistics travel under a venue of their
		 * own; it is not a place anybody trades. */
		if (!venture_string_is_empty(venue->group_key) && !g_str_has_prefix(venue->key, "region-"))
			g_hash_table_add(groups, venue->group_key);
	}

	/* An account on no venue (a shared bank) is valued on the region of
	 * the store's only group; with several the region is not guessed. */
	if (1 == g_hash_table_size(groups))
	{
		GHashTableIter iter;
		gpointer key;

		g_hash_table_iter_init(&iter, groups);

		if (g_hash_table_iter_next(&iter, &key, NULL))
			*out_default_group = g_strdup(key);
	}

	return names;
}

/* What a person calls the place an account is in: its group (a realm),
 * else its venue's name, else its own name. */
static gchar *
ma_realm(
	const VentureSeriesAccountRow	*account,
	GHashTable			*venues
){
	const gchar *name;

	if (!venture_string_is_empty(account->group_key))
		return g_strdup(account->group_key);

	if (!venture_string_is_empty(account->venue_key))
	{
		name = (NULL != venues) ? g_hash_table_lookup(venues, account->venue_key) : NULL;

		return g_strdup((NULL != name) ? name : account->venue_key);
	}

	return g_strdup("");
}

static const gchar *
ma_account_name(const VentureSeriesAccountRow *account)
{
	return venture_string_is_empty(account->name) ? account->key : account->name;
}

/*
 * Appends a store's logins to @into as {data_source_id, key, name,
 * accounts}, for a page's login picker; a key already there (from another
 * source) is not repeated, since the filter is by key. A source that
 * sends no logins adds nothing.
 */
static gboolean
ma_login_choices(
	VentureSeriesStore	 *store,
	gint64			  source_id,
	JsonArray		 *into,
	GError			**error
){
	g_autoptr(GPtrArray) rows = NULL;
	guint i;

	rows = venture_series_store_list_logins(store, error);

	if (NULL == rows)
		return FALSE;

	for (i = 0; i < rows->len; i++)
	{
		VentureSeriesLoginRow *login = g_ptr_array_index(rows, i);
		JsonObject *object;
		gboolean seen = FALSE;
		guint j;

		for (j = 0; !seen && (j < json_array_get_length(into)); j++)
			seen = (0 == g_strcmp0(json_object_get_string_member(json_array_get_object_element(into, j),
			                                                     "key"), login->key));

		if (seen)
			continue;

		object = json_object_new();
		json_object_set_int_member(object, "data_source_id", source_id);
		json_object_set_string_member(object, "key", login->key);
		json_object_set_string_member(object, "name", venture_string_is_empty(login->name)
		                                              ? login->key : login->name);
		json_object_set_int_member(object, "accounts", login->accounts);
		json_array_add_object_element(into, object);
	}

	return TRUE;
}

/* What a person calls the login an account is reached through: its name,
 * else its key; NULL for an account reached through none. */
static const gchar *
ma_login_name(const VentureSeriesAccountRow *account)
{
	if (venture_string_is_empty(account->login_key))
		return NULL;

	return venture_string_is_empty(account->login_name) ? account->login_key : account->login_name;
}

/* The source with accounts that a page about "the" inventory reads when
 * none is named: the first by name that has any, else the first. */
static VentureEntity *
ma_pick_source(
	VentureContext	 *context,
	gint64		  organization_id,
	gint64		  data_source_id,
	GPtrArray	 *sources,
	GError		**error
){
	VentureEntity *first = NULL;
	guint i;

	if (data_source_id > 0)
		return ma_source(context, organization_id, data_source_id, error);

	for (i = 0; (NULL != sources) && (i < sources->len); i++)
	{
		VentureEntity *source = g_ptr_array_index(sources, i);
		g_autoptr(VentureSeriesStore) store = NULL;
		g_autoptr(GPtrArray) accounts = NULL;

		if (NULL == first)
			first = source;

		store = ma_reader(context, source, NULL, NULL);
		accounts = (NULL != store) ? venture_series_store_list_accounts(store, NULL, NULL, NULL, 0, NULL) : NULL;

		if ((NULL != accounts) && (accounts->len > 0))
			return g_object_ref(source);
	}

	return (NULL != first) ? g_object_ref(first) : NULL;
}

/* The organization's sources, for a picker: id, name. */
static JsonArray *
ma_sources_json(GPtrArray *sources)
{
	JsonArray *array;
	guint i;

	array = json_array_new();

	for (i = 0; (NULL != sources) && (i < sources->len); i++)
	{
		VentureEntity *source = g_ptr_array_index(sources, i);
		g_autofree gchar *name = ma_source_name(source);
		JsonObject *object = json_object_new();

		json_object_set_int_member(object, "id", venture_entity_get_id(source));
		json_object_set_string_member(object, "name", name);
		json_array_add_object_element(array, object);
	}

	return array;
}

/* The basis a question names (checked already), or the default. */
static VentureSeriesValueBasis
ma_basis(const gchar *name)
{
	VentureSeriesValueBasis basis;

	if ((NULL == name) || !venture_series_value_basis_from_string(name, &basis))
		return VENTURE_SERIES_VALUE_CONSERVATIVE;

	return basis;
}

#endif /* VENTURE_HAVE_SQLITE */

/* --- The overview -------------------------------------------------------------- */

#ifdef VENTURE_HAVE_SQLITE

/* What one account's listings and mail add up to. */
typedef struct
{
	gint64	 positions;
	gint64	 units;
	gint64	 expired;
	gint64	 expired_units;
	gint64	 earliest_expired;	/* NONE: nothing expired */
	gint64	 soon;
	gint64	 soon_units;
	gint64	 soonest_future;	/* NONE: nothing still up with an expiry */
	gint64	 value;			/* in the source's currency */
	gint64	 mail_items;
	gint64	 mail_item_units;
	gint64	 mail_soon;
	gint64	 mail_soonest;		/* NONE: no mail expiring in the window */
	gint64	 inventory_value;	/* NONE: not valued (no currency) */
	gint64	 inventory_lines;
	gint64	 inventory_priced;
	GArray	*net;			/* the thirty days' net, per currency */
} MaOps;

static MaOps *
ma_ops_new(void)
{
	MaOps *ops;

	ops = g_new0(MaOps, 1);
	ops->earliest_expired = VENTURE_SERIES_NONE;
	ops->soonest_future = VENTURE_SERIES_NONE;
	ops->mail_soonest = VENTURE_SERIES_NONE;
	ops->inventory_value = VENTURE_SERIES_NONE;
	ops->net = g_array_new(FALSE, TRUE, sizeof(VentureSeriesAmount));

	return ops;
}

static void
ma_ops_free(gpointer data)
{
	MaOps *ops = data;

	g_clear_pointer(&ops->net, g_array_unref);
	g_free(ops);
}

static MaOps *
ma_ops_for(
	GHashTable	*table,
	const gchar	*key
){
	MaOps *ops;

	ops = g_hash_table_lookup(table, key);

	if (NULL == ops)
	{
		ops = ma_ops_new();
		g_hash_table_insert(table, g_strdup(key), ops);
	}

	return ops;
}

/* One place to log in to, and why. */
typedef struct
{
	gint64		 source_id;
	gchar		*realm;
	gchar		*login_key;	/* "" for none */
	gchar		*login_name;	/* NULL for none */
	JsonArray	*accounts;
	JsonArray	*reasons;
	gint		 tier;		/* 0 a deadline, 1 something to collect, 2 stale */
	gint64		 due;		/* the earliest deadline, NONE */
	gint64		 money;		/* what waits, in the source's currency */
	gint64		 oldest;	/* the longest unseen, for stale rows */
} MaAttention;

static void
ma_attention_free(gpointer data)
{
	MaAttention *row = data;

	g_free(row->realm);
	g_free(row->login_key);
	g_free(row->login_name);
	g_clear_pointer(&row->accounts, json_array_unref);
	g_clear_pointer(&row->reasons, json_array_unref);
	g_free(row);
}

/* One row of the account table, with the values its orders compare. */
typedef struct
{
	JsonObject	*json;
	gchar		*name_fold;
	gchar		*realm_fold;
	gchar		*login_fold;	/* "" for none, which sorts last */
	gint64		 gold;
	gint64		 positions;
	gint64		 soonest;
	gint64		 inbound;
	gint64		 last_seen;
	gint64		 synced_at;
	gint		 tier;
	gint64		 due;
} MaAccount;

static void
ma_account_free(gpointer data)
{
	MaAccount *row = data;

	g_clear_pointer(&row->json, json_object_unref);
	g_free(row->name_fold);
	g_free(row->realm_fold);
	g_free(row->login_fold);
	g_free(row);
}

/*
 * One login's share of the overview: the per-login cards. Every figure is
 * a sum of its accounts' own, so the cards add up to the headline. Kept
 * per source, since two sources' stores may each have a login by the
 * same key and they are not the same login.
 */
typedef struct
{
	gint64		 source_id;
	gchar		*key;		/* "" for the accounts reached through none */
	gchar		*name;
	gchar		*kind;
	gchar		*group_key;
	gint64		 accounts;
	gint64		 characters;
	gint64		 needs_login;
	gint64		 positions;
	gint64		 positions_expired;
	gint64		 positions_expiring;
	gint64		 inbound;
	gint64		 inventory_lines;
	gint64		 inventory_priced;
	GHashTable	*realms;
	GArray		*balances;
	GArray		*inventory;
	GArray		*positions_value;
	GArray		*net;
} MaLogin;

static void
ma_login_free(gpointer data)
{
	MaLogin *login = data;

	g_free(login->key);
	g_free(login->name);
	g_free(login->kind);
	g_free(login->group_key);
	g_clear_pointer(&login->realms, g_hash_table_unref);
	g_clear_pointer(&login->balances, g_array_unref);
	g_clear_pointer(&login->inventory, g_array_unref);
	g_clear_pointer(&login->positions_value, g_array_unref);
	g_clear_pointer(&login->net, g_array_unref);
	g_free(login);
}

/* Everything the overview gathers from every source. */
typedef struct
{
	const VentureMarketdataAccountsQuery	*query;
	VentureSeriesValueBasis			 basis;
	gint64					 now;
	gint64					 expiring_seconds;
	gint64					 mail_seconds;
	gint64					 stale_seconds;
	GArray					*balances;
	GArray					*inventory;
	gint64					 inventory_lines;
	gint64					 inventory_priced;
	gint64					 inventory_units;
	gint64					 positions;
	gint64					 positions_units;
	gint64					 positions_expired;
	gint64					 positions_soon;
	GArray					*positions_value;
	gint64					 inbound;
	gint64					 inbound_items;
	GArray					*inbound_money;
	GArray					*sales;
	GArray					*purchases;
	GArray					*net;
	gint64					 accounts;
	gint64					 characters;
	gint64					 stale;
	GHashTable				*realms;
	GHashTable				*attention;	/* "source|login|realm" -> MaAttention */
	GPtrArray				*rows;		/* MaAccount */
	GPtrArray				*logins;	/* MaLogin, in the order first met */
	GHashTable				*login_index;	/* "source|login" -> MaLogin (borrowed) */
	JsonArray				*login_choices;	/* every login, for the picker */
	gboolean				 any_login;	/* an account shown has a login */
	JsonArray				*sources;
	GArray					*shown;		/* source ids */
	gchar					*first_currency;
} MaOverview;

static void
ma_overview_clear(MaOverview *view)
{
	g_clear_pointer(&view->balances, g_array_unref);
	g_clear_pointer(&view->inventory, g_array_unref);
	g_clear_pointer(&view->positions_value, g_array_unref);
	g_clear_pointer(&view->inbound_money, g_array_unref);
	g_clear_pointer(&view->sales, g_array_unref);
	g_clear_pointer(&view->purchases, g_array_unref);
	g_clear_pointer(&view->net, g_array_unref);
	g_clear_pointer(&view->realms, g_hash_table_unref);
	g_clear_pointer(&view->attention, g_hash_table_unref);
	g_clear_pointer(&view->rows, g_ptr_array_unref);
	g_clear_pointer(&view->login_index, g_hash_table_unref);
	g_clear_pointer(&view->login_choices, json_array_unref);
	g_clear_pointer(&view->logins, g_ptr_array_unref);
	g_clear_pointer(&view->sources, json_array_unref);
	g_clear_pointer(&view->shown, g_array_unref);
	g_clear_pointer(&view->first_currency, g_free);
}

/* A reason to log in, under the account it is about. */
static void
ma_reason(
	MaAttention	*row,
	const VentureSeriesAccountRow *account,
	const gchar	*kind,
	const gchar	*text,
	gint64		 count,
	gint64		 at,
	gint64		 money,
	const gchar	*currency
){
	JsonObject *reason;

	reason = json_object_new();
	json_object_set_string_member(reason, "account_key", account->key);
	json_object_set_string_member(reason, "account_name", ma_account_name(account));
	json_object_set_string_member(reason, "kind", kind);
	json_object_set_string_member(reason, "text", text);
	json_object_set_int_member(reason, "count", count);
	ma_set_time(reason, "at", at);
	ma_set_money(reason, "money", money, currency);
	json_array_add_object_element(row->reasons, reason);
}

static MaAttention *
ma_attention_for(
	MaOverview	*view,
	gint64		 source_id,
	const gchar	*realm,
	const VentureSeriesAccountRow *account
){
	g_autofree gchar *key = NULL;
	MaAttention *row;

	/*
	 * An account with no realm is a place of its own (a shared bank). A
	 * realm is a place per login: the same realm under two logins is two
	 * visits, since reaching it means signing in with each. The key's
	 * separator is a control character no key may hold unescaped in a
	 * sensible export and, either way, only joins the parts.
	 */
	key = g_strdup_printf("%" G_GINT64_FORMAT "\x1f%s\x1f%s", source_id, account->login_key,
	                      venture_string_is_empty(realm) ? account->key : realm);
	row = g_hash_table_lookup(view->attention, key);

	if (NULL == row)
	{
		row = g_new0(MaAttention, 1);
		row->source_id = source_id;
		row->login_key = g_strdup(account->login_key);
		row->login_name = g_strdup(ma_login_name(account));
		row->realm = g_strdup(venture_string_is_empty(realm) ? ma_account_name(account) : realm);
		row->accounts = json_array_new();
		row->reasons = json_array_new();
		row->tier = 3;
		row->due = VENTURE_SERIES_NONE;
		row->money = 0;
		row->oldest = VENTURE_SERIES_NONE;
		g_hash_table_insert(view->attention, g_steal_pointer(&key), row);
	}

	return row;
}

/* Adds @account to @row's list of accounts, once. */
static void
ma_attention_account(
	MaAttention			*row,
	const VentureSeriesAccountRow	*account,
	gint64				 source_id
){
	g_autofree gchar *url = NULL;
	JsonObject *object;
	guint i;

	for (i = 0; i < json_array_get_length(row->accounts); i++)
	{
		JsonObject *have = json_array_get_object_element(row->accounts, i);

		if (0 == g_strcmp0(json_object_get_string_member(have, "key"), account->key))
			return;
	}

	url = venture_marketdata_account_path(source_id, account->key);
	object = json_object_new();
	json_object_set_string_member(object, "key", account->key);
	json_object_set_string_member(object, "name", ma_account_name(account));
	json_object_set_string_member(object, "url", url);
	json_array_add_object_element(row->accounts, object);
}

static void
ma_attention_deadline(
	MaAttention	*row,
	gint64		 due
){
	row->tier = 0;

	if ((VENTURE_SERIES_NONE == row->due) || (due < row->due))
		row->due = due;
}

/*
 * Why @account needs a login, if it does: listings already expired and
 * waiting to be collected, listings and mail about to expire (deadlines,
 * most urgent first), money or items waiting in the mail, and an account
 * nobody has looked at for a while. Returns the tier and the deadline the
 * account table sorts by.
 */
static gboolean
ma_account_attention(
	MaOverview			*view,
	VentureEntity			*source,
	const VentureSeriesAccountRow	*account,
	const gchar			*realm,
	const MaOps			*ops,
	const gchar			*currency,
	JsonArray			*reasons_text,
	gint				*out_tier,
	gint64				*out_due
){
	gint64 source_id = venture_entity_get_id(source);
	gint64 seen;
	gint64 mail_money;
	MaAttention *row = NULL;

	*out_tier = 3;
	*out_due = VENTURE_SERIES_NONE;

	if (ops->expired > 0)
	{
		g_autofree gchar *when = ma_when(ops->earliest_expired, view->now);
		g_autofree gchar *text = NULL;

		text = g_strdup_printf("%" G_GINT64_FORMAT " listing%s expired (%" G_GINT64_FORMAT
		                       " unit%s), the first %s: collect and relist",
		                       ops->expired, (1 == ops->expired) ? "" : "s", ops->expired_units,
		                       (1 == ops->expired_units) ? "" : "s", when);
		row = ma_attention_for(view, source_id, realm, account);
		ma_reason(row, account, "positions_expired", text, ops->expired, ops->earliest_expired,
		          VENTURE_SERIES_NONE, NULL);
		ma_attention_deadline(row, ops->earliest_expired);
		json_array_add_string_element(reasons_text, text);
	}

	if (ops->soon > 0)
	{
		g_autofree gchar *when = ma_when(ops->soonest_future, view->now);
		g_autofree gchar *text = NULL;

		text = g_strdup_printf("%" G_GINT64_FORMAT " listing%s expire%s within %" G_GINT64_FORMAT
		                       " h, the first %s",
		                       ops->soon, (1 == ops->soon) ? "" : "s", (1 == ops->soon) ? "s" : "",
		                       view->expiring_seconds / MA_HOUR, when);
		row = ma_attention_for(view, source_id, realm, account);
		ma_reason(row, account, "positions_expiring", text, ops->soon, ops->soonest_future,
		          VENTURE_SERIES_NONE, NULL);
		ma_attention_deadline(row, ops->soonest_future);
		json_array_add_string_element(reasons_text, text);
	}

	if (ops->mail_soon > 0)
	{
		g_autofree gchar *when = ma_when(ops->mail_soonest, view->now);
		g_autofree gchar *text = NULL;

		text = g_strdup_printf("%" G_GINT64_FORMAT " mail%s expire%s within %" G_GINT64_FORMAT
		                       " d, the first %s",
		                       ops->mail_soon, (1 == ops->mail_soon) ? "" : "s",
		                       (1 == ops->mail_soon) ? "s" : "", view->mail_seconds / MA_DAY, when);
		row = ma_attention_for(view, source_id, realm, account);
		ma_reason(row, account, "inbound_expiring", text, ops->mail_soon, ops->mail_soonest,
		          VENTURE_SERIES_NONE, NULL);
		ma_attention_deadline(row, ops->mail_soonest);
		json_array_add_string_element(reasons_text, text);
	}

	mail_money = ma_amounts_get(account->inbound_money, currency);

	if (((VENTURE_SERIES_NONE != mail_money) && (mail_money > 0)) || (ops->mail_items > 0))
	{
		g_autofree gchar *money = ma_money_text(mail_money, currency);
		g_autoptr(GString) text = g_string_new(NULL);

		if ((NULL != money) && (mail_money > 0))
			g_string_append(text, money);

		if (ops->mail_items > 0)
			g_string_append_printf(text, "%s%" G_GINT64_FORMAT " item%s (%" G_GINT64_FORMAT " unit%s)",
			                       (text->len > 0) ? " and " : "", ops->mail_items,
			                       (1 == ops->mail_items) ? "" : "s", ops->mail_item_units,
			                       (1 == ops->mail_item_units) ? "" : "s");

		g_string_append(text, " waiting in the mail");
		row = ma_attention_for(view, source_id, realm, account);
		ma_reason(row, account, "inbound_waiting", text->str, ops->mail_items,
		          VENTURE_SERIES_NONE, (mail_money > 0) ? mail_money : VENTURE_SERIES_NONE, currency);

		if (row->tier > 1)
			row->tier = 1;

		if ((VENTURE_SERIES_NONE != mail_money) && (mail_money > 0) &&
		    !venture_series_math_add(row->money, mail_money, &row->money))
			row->money = G_MAXINT64;

		json_array_add_string_element(reasons_text, text->str);
	}

	/* Not seen in use since... a source that never says counts from when
	 * the store first heard of it. */
	seen = (VENTURE_SERIES_NONE != account->last_seen) ? account->last_seen : account->first_seen;

	if (view->now - seen > view->stale_seconds)
	{
		g_autofree gchar *span = ma_span(view->now - seen);
		g_autofree gchar *text = g_strdup_printf("Not seen in use for %s", span);

		row = ma_attention_for(view, source_id, realm, account);
		ma_reason(row, account, "stale", text, 1, seen, VENTURE_SERIES_NONE, NULL);

		if (row->tier > 2)
			row->tier = 2;

		if ((VENTURE_SERIES_NONE == row->oldest) || (seen < row->oldest))
			row->oldest = seen;

		json_array_add_string_element(reasons_text, text);
		view->stale++;
	}

	if (NULL == row)
		return FALSE;

	ma_attention_account(row, account, source_id);

	/* The account's own tier, from its own reasons. */
	if ((ops->expired > 0) || (ops->soon > 0) || (ops->mail_soon > 0))
	{
		*out_tier = 0;

		if (ops->expired > 0)
			*out_due = ops->earliest_expired;

		if ((ops->soon > 0) && ((VENTURE_SERIES_NONE == *out_due) || (ops->soonest_future < *out_due)))
			*out_due = ops->soonest_future;

		if ((ops->mail_soon > 0) && ((VENTURE_SERIES_NONE == *out_due) || (ops->mail_soonest < *out_due)))
			*out_due = ops->mail_soonest;
	}
	else if ((ops->mail_items > 0) || ((VENTURE_SERIES_NONE != mail_money) && (mail_money > 0)))
		*out_tier = 1;
	else
		*out_tier = 2;

	return TRUE;
}

/* The account's attributes as an object, or an empty one. */
static JsonObject *
ma_attrs(const gchar *text)
{
	g_autoptr(JsonNode) node = NULL;

	node = venture_string_is_empty(text) ? NULL : venture_json_parse(text, NULL);

	if ((NULL == node) || !JSON_NODE_HOLDS_OBJECT(node))
		return json_object_new();

	return json_object_ref(json_node_get_object(node));
}

/* The closing balance of each of the trend's days, oldest first, carried
 * forward from the last change; null before anything was known. */
static JsonArray *
ma_trend(
	GPtrArray	*days,
	const gchar	*account_key,
	gint64		 first_day,
	guint		 n_days,
	gint64		*out_last
){
	JsonArray *values;
	gint64 current = VENTURE_SERIES_NONE;
	guint next = 0;
	guint i;

	values = json_array_new();

	/* The rows are by account then day; find this account's first. */
	while ((next < days->len) &&
	       (g_strcmp0(((VentureSeriesBalanceDay *)g_ptr_array_index(days, next))->account_key,
	                  account_key) < 0))
		next++;

	for (i = 0; i < n_days; i++)
	{
		gint64 day = first_day + (gint64)i * MA_DAY;

		while (next < days->len)
		{
			VentureSeriesBalanceDay *point = g_ptr_array_index(days, next);

			if ((0 != g_strcmp0(point->account_key, account_key)) || (point->day_start > day))
				break;

			current = point->amount;
			next++;
		}

		if (VENTURE_SERIES_NONE == current)
			json_array_add_null_element(values);
		else
			json_array_add_int_element(values, current);
	}

	*out_last = current;

	return values;
}

/* One account's row of the table. */
static MaAccount *
ma_account_row(
	MaOverview			*view,
	VentureEntity			*source,
	const VentureSeriesAccountRow	*account,
	const gchar			*realm,
	const MaOps			*ops,
	const gchar			*currency,
	GPtrArray			*days,
	gint64				 first_day
){
	g_autofree gchar *url = NULL;
	g_autofree gchar *source_name = NULL;
	MaAccount *row;
	JsonObject *json;
	JsonObject *attrs;
	JsonArray *reasons;
	JsonNode *member;
	gint64 gold;

	row = g_new0(MaAccount, 1);
	json = json_object_new();
	row->json = json;
	source_name = ma_source_name(source);
	url = venture_marketdata_account_path(venture_entity_get_id(source), account->key);

	json_object_set_int_member(json, "data_source_id", venture_entity_get_id(source));
	json_object_set_string_member(json, "source_name", source_name);
	json_object_set_string_member(json, "key", account->key);
	ma_set_text(json, "name", account->name);
	json_object_set_string_member(json, "display_name", ma_account_name(account));
	ma_set_text(json, "kind", account->kind);
	json_object_set_string_member(json, "group_key", account->group_key);
	json_object_set_string_member(json, "realm", realm);
	ma_set_text(json, "venue_key", account->venue_key);
	json_object_set_string_member(json, "url", url);

	/* The login the account is reached through; null for none, so a
	 * table can say "any login" for a shared bank. */
	if (venture_string_is_empty(account->login_key))
	{
		json_object_set_null_member(json, "login");
		json_object_set_null_member(json, "login_name");
	}
	else
	{
		json_object_set_string_member(json, "login", account->login_key);
		json_object_set_string_member(json, "login_name", ma_login_name(account));
	}

	/* The attributes as the source sent them, plus the two a table shows. */
	attrs = ma_attrs(account->attrs_json);
	member = json_object_get_member(attrs, "level");

	if ((NULL != member) && JSON_NODE_HOLDS_VALUE(member) &&
	    ((G_TYPE_INT64 == json_node_get_value_type(member)) ||
	     (G_TYPE_DOUBLE == json_node_get_value_type(member))))
		json_object_set_int_member(json, "level", (gint64)json_node_get_double(member));
	else
		json_object_set_null_member(json, "level");

	member = json_object_get_member(attrs, "class");

	if ((NULL != member) && JSON_NODE_HOLDS_VALUE(member) &&
	    (G_TYPE_STRING == json_node_get_value_type(member)) &&
	    !venture_string_is_empty(json_node_get_string(member)))
	{
		g_autofree gchar *shown = ma_title_case(json_node_get_string(member));

		json_object_set_string_member(json, "class", shown);
	}
	else
		json_object_set_null_member(json, "class");

	json_object_set_object_member(json, "attrs", attrs);

	gold = ma_amounts_get(account->balances, currency);
	ma_set_money(json, "gold", gold, currency);
	json_object_set_array_member(json, "balances", ma_amounts_json(account->balances, currency));
	ma_set_money(json, "inventory_value", ops->inventory_value, currency);
	json_object_set_array_member(json, "net_30d", ma_amounts_json(ops->net, currency));

	if ((NULL != days) && (NULL != currency))
	{
		JsonObject *trend = json_object_new();
		gint64 last = VENTURE_SERIES_NONE;

		json_object_set_string_member(trend, "currency", currency);
		json_object_set_array_member(trend, "values",
		                             ma_trend(days, account->key, first_day,
		                                      VENTURE_MARKETDATA_ACCOUNTS_TREND_DAYS, &last));
		json_object_set_object_member(json, "trend", trend);
	}
	else
		json_object_set_null_member(json, "trend");

	json_object_set_int_member(json, "positions", account->positions);
	json_object_set_int_member(json, "positions_units", ops->units);
	json_object_set_int_member(json, "positions_expired", ops->expired);
	json_object_set_int_member(json, "positions_expiring", ops->soon);
	ma_set_money(json, "positions_value", (ops->positions > 0) ? ops->value : VENTURE_SERIES_NONE,
	             currency);
	ma_set_time(json, "soonest_expiry", account->soonest_position_expiry);
	json_object_set_int_member(json, "inbound", account->inbound);
	json_object_set_int_member(json, "inbound_items", ops->mail_items);
	json_object_set_int_member(json, "inbound_expiring", ops->mail_soon);
	json_object_set_array_member(json, "inbound_money",
	                             ma_amounts_json(account->inbound_money, currency));
	ma_set_time(json, "soonest_inbound_expiry", account->soonest_inbound_expiry);
	ma_set_time(json, "last_seen", account->last_seen);
	ma_set_time(json, "first_seen", account->first_seen);
	ma_set_time(json, "synced_at", account->synced_at);
	ma_set_time(json, "holdings_at", account->holdings_at);
	ma_set_time(json, "positions_at", account->positions_at);
	ma_set_time(json, "inbound_at", account->inbound_at);
	json_object_set_int_member(json, "freshness_seconds", MAX(view->now - account->synced_at, 0));

	reasons = json_array_new();
	json_object_set_boolean_member(json, "needs_login",
		ma_account_attention(view, source, account, realm, ops, currency, reasons,
		                     &row->tier, &row->due));
	json_object_set_boolean_member(json, "stale",
		view->now - ((VENTURE_SERIES_NONE != account->last_seen) ? account->last_seen
		                                                         : account->first_seen)
		> view->stale_seconds);
	json_object_set_array_member(json, "reasons", reasons);

	row->name_fold = g_utf8_casefold(ma_account_name(account), -1);
	row->realm_fold = g_utf8_casefold(realm, -1);

	if (venture_string_is_empty(account->login_key))
		row->login_fold = g_strdup("");
	else
	{
		g_autofree gchar *fold = g_utf8_casefold(ma_login_name(account), -1);

		/* The key after the name, so two logins of one name still
		 * keep their accounts apart. */
		row->login_fold = g_strdup_printf("%s\x1f%s", fold, account->login_key);
	}
	row->gold = gold;
	row->positions = account->positions;
	row->soonest = account->soonest_position_expiry;
	row->inbound = account->inbound;
	row->last_seen = account->last_seen;
	row->synced_at = account->synced_at;

	return row;
}

/*
 * The card of the login @account is reached through, made on first
 * mention: one per source and login, and one per source for the accounts
 * reached through none. @rows are the store's logins, for the kind and
 * group a login line gave (NULL when no account shown names one).
 */
static MaLogin *
ma_login_for(
	MaOverview			*view,
	gint64				 source_id,
	const VentureSeriesAccountRow	*account,
	GPtrArray			*rows
){
	g_autofree gchar *index = NULL;
	MaLogin *login;
	guint i;

	index = g_strdup_printf("%" G_GINT64_FORMAT "\x1f%s", source_id, account->login_key);
	login = g_hash_table_lookup(view->login_index, index);

	if (NULL != login)
		return login;

	login = g_new0(MaLogin, 1);
	login->source_id = source_id;
	login->key = g_strdup(account->login_key);
	login->name = g_strdup(venture_string_is_empty(account->login_key) ? "No login"
	                                                                   : ma_login_name(account));
	login->realms = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	login->balances = ma_amounts_new();
	login->inventory = ma_amounts_new();
	login->positions_value = ma_amounts_new();
	login->net = ma_amounts_new();

	for (i = 0; (NULL != rows) && (i < rows->len); i++)
	{
		VentureSeriesLoginRow *row = g_ptr_array_index(rows, i);

		if (0 != g_strcmp0(row->key, account->login_key))
			continue;

		login->kind = g_strdup(row->kind);
		login->group_key = g_strdup(row->group_key);
		break;
	}

	g_ptr_array_add(view->logins, login);
	g_hash_table_insert(view->login_index, g_steal_pointer(&index), login);

	return login;
}

/* Everything one source adds to the overview. */
static gboolean
ma_overview_source(
	VentureContext	 *context,
	MaOverview	 *view,
	VentureEntity	 *source,
	JsonArray	 *notes,
	GError		**error
){
	g_autoptr(VentureSeriesStore) store = NULL;
	g_autoptr(GPtrArray) accounts = NULL;
	g_autoptr(GPtrArray) positions = NULL;
	g_autoptr(GPtrArray) inbound = NULL;
	g_autoptr(GPtrArray) days = NULL;
	g_autoptr(GHashTable) venues = NULL;
	g_autoptr(GHashTable) ops = NULL;
	g_autoptr(GHashTable) listed = NULL;
	g_autoptr(GPtrArray) login_rows = NULL;
	g_autofree gchar *currency = NULL;
	g_autofree gchar *default_group = NULL;
	g_autofree gchar *name = NULL;
	VentureSeriesTxnFilter txns;
	JsonObject *summary;
	gint64 source_id;
	gint64 first_day;
	guint i;

	source_id = venture_entity_get_id(source);
	name = ma_source_name(source);
	summary = json_object_new();
	json_object_set_int_member(summary, "id", source_id);
	json_object_set_string_member(summary, "name", name);
	json_object_set_int_member(summary, "accounts", 0);
	json_array_add_object_element(view->sources, summary);

	store = ma_reader(context, source, NULL, NULL);

	/* A source that has stored nothing has no accounts yet: that is the
	 * normal state of every market-only source, not something to say. */
	if (NULL == store)
		return TRUE;

	/* Every login the source knows, whatever the filters, for the
	 * picker that sets one. */
	if (!ma_login_choices(store, source_id, view->login_choices, error))
		return FALSE;

	accounts = venture_series_store_list_accounts(store, NULL, view->query->group_key, view->query->login, view->now, error);

	if (NULL == accounts)
		return FALSE;

	json_object_set_int_member(summary, "accounts", accounts->len);

	if (0 == accounts->len)
		return TRUE;

	g_array_append_val(view->shown, source_id);
	currency = ma_source_currency(source);

	if ((NULL != currency) && (NULL == view->first_currency))
		view->first_currency = g_strdup(currency);

	if (NULL == currency)
	{
		g_autofree gchar *text = g_strdup_printf("%s names no currency, so its holdings, listings "
		                                         "and ledger cannot be valued", name);

		ma_note(notes, text);
	}

	venues = ma_venues(store, &default_group, error);

	if (NULL == venues)
		return FALSE;

	ops = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, ma_ops_free);
	listed = g_hash_table_new(g_str_hash, g_str_equal);

	/* Only the accounts asked about (a group narrows them) count. */
	for (i = 0; i < accounts->len; i++)
		g_hash_table_add(listed, ((VentureSeriesAccountRow *)g_ptr_array_index(accounts, i))->key);

	/* Listings: what is up, what has run out, what runs out soon. */
	positions = venture_series_store_list_positions(store, NULL, error);

	if (NULL == positions)
		return FALSE;

	for (i = 0; i < positions->len; i++)
	{
		VentureSeriesPositionRow *position = g_ptr_array_index(positions, i);
		MaOps *mine;
		gint64 value;

		if (!g_hash_table_contains(listed, position->account_key))
			continue;

		mine = ma_ops_for(ops, position->account_key);
		mine->positions++;
		mine->units += position->quantity;
		view->positions++;
		view->positions_units += position->quantity;

		if ((VENTURE_SERIES_NONE != position->expires_at) && (position->expires_at <= view->now))
		{
			mine->expired++;
			mine->expired_units += position->quantity;
			view->positions_expired++;

			if ((VENTURE_SERIES_NONE == mine->earliest_expired) ||
			    (position->expires_at < mine->earliest_expired))
				mine->earliest_expired = position->expires_at;
		}
		else if (VENTURE_SERIES_NONE != position->expires_at)
		{
			if ((VENTURE_SERIES_NONE == mine->soonest_future) ||
			    (position->expires_at < mine->soonest_future))
				mine->soonest_future = position->expires_at;

			if (position->expires_at - view->now <= view->expiring_seconds)
			{
				mine->soon++;
				mine->soon_units += position->quantity;
				view->positions_soon++;
			}
		}

		if (!venture_series_math_mul(position->quantity, position->unit_price, &value))
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			                    "A listing's buyout does not fit in a 64-bit integer");
			return FALSE;
		}

		if (!ma_amounts_add(view->positions_value, position->currency, value, error))
			return FALSE;

		if ((NULL != currency) && (0 == g_ascii_strcasecmp(position->currency, currency)) &&
		    !venture_series_math_add(mine->value, value, &mine->value))
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			                    "An account's listings do not fit in a 64-bit integer");
			return FALSE;
		}
	}

	/* Mail: items waiting, and what expires inside the window. */
	inbound = venture_series_store_list_inbound(store, NULL, error);

	if (NULL == inbound)
		return FALSE;

	for (i = 0; i < inbound->len; i++)
	{
		VentureSeriesInboundRow *mail = g_ptr_array_index(inbound, i);
		MaOps *mine;

		if (!g_hash_table_contains(listed, mail->account_key))
			continue;

		mine = ma_ops_for(ops, mail->account_key);
		view->inbound++;

		if (NULL != mail->instrument_key)
		{
			mine->mail_items++;
			mine->mail_item_units += (VENTURE_SERIES_NONE != mail->quantity) ? mail->quantity : 1;
			view->inbound_items++;
		}

		if ((VENTURE_SERIES_NONE != mail->expires_at) && (mail->expires_at > view->now) &&
		    (mail->expires_at - view->now <= view->mail_seconds))
		{
			mine->mail_soon++;

			if ((VENTURE_SERIES_NONE == mine->mail_soonest) || (mail->expires_at < mine->mail_soonest))
				mine->mail_soonest = mail->expires_at;
		}
	}

	/* The sparklines: each account's closing balance a day, one read. */
	first_day = ((view->now / MA_DAY) - (VENTURE_MARKETDATA_ACCOUNTS_TREND_DAYS - 1)) * MA_DAY;

	if (NULL != currency)
	{
		days = venture_series_store_balance_days(store, currency, first_day, view->now + 1, error);

		if (NULL == days)
			return FALSE;
	}

	/*
	 * What the bags are worth, on the basis asked for. The value read
	 * narrows by one account at most, so a group (a realm) is valued an
	 * account at a time: valuing the whole source put every realm's bags
	 * under the one asked about. Each line's value is its own quantity at
	 * its own venue's price, so the accounts' totals add up exactly.
	 */
	if (NULL != currency)
	{
		guint shown = (NULL != view->query->group_key) ? accounts->len : 1;
		guint k;

		for (k = 0; k < shown; k++)
		{
			VentureSeriesValueFilter filter;
			VentureSeriesValueTotals worth;
			g_autoptr(GPtrArray) page = NULL;

			venture_series_value_filter_init(&filter);
			filter.currency = currency;
			filter.basis = view->basis;
			filter.default_group = default_group;
			filter.now = view->now;
			filter.exclude_place = "currency";
			filter.count = 1;
			filter.login_key = view->query->login;

			if (NULL != view->query->group_key)
				filter.account_key = ((VentureSeriesAccountRow *)g_ptr_array_index(accounts, k))->key;

			page = venture_series_store_value_instruments(store, &filter, &worth, error);

			if (NULL == page)
				return FALSE;

			if (!ma_amounts_add(view->inventory, currency, worth.value, error))
				return FALSE;

			view->inventory_lines += worth.lines;
			view->inventory_priced += worth.priced_lines;
			view->inventory_units += worth.quantity;
		}
	}

	/*
	 * The last thirty days of the ledger. A group reads it an account at a
	 * time, like the bags, so the net beside a realm's gold is that
	 * realm's; a row naming no account belongs to no realm. (Bucketing the
	 * whole source by account would do it in one read, but buckets are
	 * capped and refused past the cap, and a source may hold as many
	 * accounts as the cap.)
	 */
	{
		guint shown = (NULL != view->query->group_key) ? accounts->len : 1;
		guint k;

		for (k = 0; k < shown; k++)
		{
			g_autoptr(GPtrArray) totals = NULL;

			venture_series_txn_filter_init(&txns);
			ma_thirty_days(context, view->now, &txns.since, &txns.until);
			txns.login_key = view->query->login;

			if (NULL != view->query->group_key)
				txns.account_key = ((VentureSeriesAccountRow *)g_ptr_array_index(accounts, k))->key;

			totals = venture_series_store_txn_totals(store, &txns, VENTURE_SERIES_TXN_GROUP_MONTH, error);

			if (NULL == totals)
				return FALSE;

			for (i = 0; i < totals->len; i++)
			{
				VentureSeriesTxnTotal *bucket = g_ptr_array_index(totals, i);
				gint64 purchases;

				if (!venture_series_math_add(bucket->buys_amount, bucket->expense, &purchases) ||
				    !ma_amounts_add(view->sales, bucket->currency, bucket->sales_amount, error) ||
				    !ma_amounts_add(view->purchases, bucket->currency, purchases, error) ||
				    !ma_amounts_add(view->net, bucket->currency, bucket->net, error))
				{
					if ((NULL != error) && (NULL == *error))
						g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
						                    "The ledger's thirty days do not fit in a 64-bit integer");
					return FALSE;
				}
			}
		}
	}

	/*
	 * Each account's own figures beside the headline's: its holdings'
	 * value and its thirty days, one grouped read each rather than one
	 * read per account. The per-login cards are sums of these, so they
	 * add up to the headline exactly -- the same lines, valued the same
	 * way, grouped another way.
	 */
	if (NULL != currency)
	{
		VentureSeriesValueFilter filter;
		g_autoptr(GPtrArray) worth = NULL;

		venture_series_value_filter_init(&filter);
		filter.currency = currency;
		filter.basis = view->basis;
		filter.default_group = default_group;
		filter.now = view->now;
		filter.exclude_place = "currency";
		filter.login_key = view->query->login;
		worth = venture_series_store_value_totals(store, &filter, VENTURE_SERIES_VALUE_GROUP_ACCOUNT,
		                                          error);

		if (NULL == worth)
			return FALSE;

		for (i = 0; i < worth->len; i++)
		{
			VentureSeriesValueGroupTotal *total = g_ptr_array_index(worth, i);
			MaOps *mine;

			if (!g_hash_table_contains(listed, total->key))
				continue;

			mine = ma_ops_for(ops, total->key);
			mine->inventory_value = total->value;
			mine->inventory_lines = total->lines;
			mine->inventory_priced = total->priced_lines;
		}
	}

	{
		g_autoptr(GPtrArray) totals = NULL;

		venture_series_txn_filter_init(&txns);
		ma_thirty_days(context, view->now, &txns.since, &txns.until);
		txns.login_key = view->query->login;
		totals = venture_series_store_txn_totals(store, &txns, VENTURE_SERIES_TXN_GROUP_ACCOUNT, error);

		if (NULL == totals)
			return FALSE;

		for (i = 0; i < totals->len; i++)
		{
			VentureSeriesTxnTotal *bucket = g_ptr_array_index(totals, i);

			if (g_hash_table_contains(listed, bucket->key) &&
			    !ma_amounts_add(ma_ops_for(ops, bucket->key)->net, bucket->currency, bucket->net,
			                    error))
				return FALSE;
		}
	}

	/* The logins' own descriptions -- kind and group -- read only when an
	 * account shown names one. */
	for (i = 0; (NULL == login_rows) && (i < accounts->len); i++)
	{
		VentureSeriesAccountRow *account = g_ptr_array_index(accounts, i);

		if (venture_string_is_empty(account->login_key))
			continue;

		login_rows = venture_series_store_list_logins(store, error);

		if (NULL == login_rows)
			return FALSE;
	}

	for (i = 0; i < accounts->len; i++)
	{
		VentureSeriesAccountRow *account = g_ptr_array_index(accounts, i);
		g_autofree gchar *realm = ma_realm(account, venues);
		MaOps *mine = ma_ops_for(ops, account->key);
		MaAccount *shown;
		MaLogin *login;
		guint j;

		view->accounts++;

		if (0 == g_strcmp0(account->kind, "character"))
		{
			view->characters++;

			if ('\0' != realm[0])
				g_hash_table_add(view->realms, g_strdup(realm));
		}

		for (j = 0; j < account->balances->len; j++)
		{
			VentureSeriesAmount *balance = &g_array_index(account->balances, VentureSeriesAmount, j);

			if (!ma_amounts_add(view->balances, balance->currency, balance->amount, error))
				return FALSE;
		}

		for (j = 0; j < account->inbound_money->len; j++)
		{
			VentureSeriesAmount *money = &g_array_index(account->inbound_money, VentureSeriesAmount, j);

			if (!ma_amounts_add(view->inbound_money, money->currency, money->amount, error))
				return FALSE;
		}

		shown = ma_account_row(view, source, account, realm, mine, currency, days, first_day);
		g_ptr_array_add(view->rows, shown);

		/* The account's share of its login's card. */
		if (!venture_string_is_empty(account->login_key))
			view->any_login = TRUE;

		login = ma_login_for(view, source_id, account, login_rows);
		login->accounts++;

		if (0 == g_strcmp0(account->kind, "character"))
		{
			login->characters++;

			if ('\0' != realm[0])
				g_hash_table_add(login->realms, g_strdup(realm));
		}

		if (shown->tier < 3)
			login->needs_login++;

		login->positions += account->positions;
		login->positions_expired += mine->expired;
		login->positions_expiring += mine->soon;
		login->inbound += account->inbound;
		login->inventory_lines += mine->inventory_lines;
		login->inventory_priced += mine->inventory_priced;

		for (j = 0; j < account->balances->len; j++)
		{
			VentureSeriesAmount *balance = &g_array_index(account->balances, VentureSeriesAmount, j);

			if (!ma_amounts_add(login->balances, balance->currency, balance->amount, error))
				return FALSE;
		}

		for (j = 0; j < mine->net->len; j++)
		{
			VentureSeriesAmount *net = &g_array_index(mine->net, VentureSeriesAmount, j);

			if (!ma_amounts_add(login->net, net->currency, net->amount, error))
				return FALSE;
		}

		if (!ma_amounts_add(login->inventory, currency, mine->inventory_value, error) ||
		    ((mine->positions > 0) &&
		     !ma_amounts_add(login->positions_value, currency, mine->value, error)))
			return FALSE;
	}

	return TRUE;
}

static gint
ma_compare_attention(
	gconstpointer	a,
	gconstpointer	b
){
	const MaAttention *one = *(MaAttention *const *)a;
	const MaAttention *two = *(MaAttention *const *)b;

	if (one->tier != two->tier)
		return (one->tier < two->tier) ? -1 : 1;

	/* Deadlines soonest first (the past before the future); what waits,
	 * the most money first; the stale, the longest unseen first. */
	if ((0 == one->tier) && (one->due != two->due))
		return (one->due < two->due) ? -1 : 1;

	if ((1 == one->tier) && (one->money != two->money))
		return (one->money > two->money) ? -1 : 1;

	if ((2 == one->tier) && (one->oldest != two->oldest))
		return (one->oldest < two->oldest) ? -1 : 1;

	if (0 != g_utf8_collate(one->realm, two->realm))
		return g_utf8_collate(one->realm, two->realm);

	return g_strcmp0(one->login_key, two->login_key);
}

/* The order the account table asks for. */
typedef struct
{
	const gchar	*name;
	gboolean	 descending;
	gboolean	 realm_first;
	gboolean	 login_first;
} MaOrder;

/* Logins by name, the accounts reached through none ("") last. */
static gint
ma_compare_login_fold(
	const gchar	*a,
	const gchar	*b
){
	gint by;

	if (('\0' == a[0]) != ('\0' == b[0]))
		return ('\0' == a[0]) ? 1 : -1;

	by = g_strcmp0(a, b);

	return (by < 0) ? -1 : ((by > 0) ? 1 : 0);
}

/*
 * A string comparison as -1, 0 or 1. g_strcmp0() answers a byte
 * difference ("Zulu" against "Alpha" is 25), and ma_compare_accounts()
 * keeps +/-2 to mean "unknown, last either way" -- so an unclamped
 * difference was read as unknown and never turned round by a descending
 * order.
 */
static gint
ma_compare_text(
	const gchar	*a,
	const gchar	*b
){
	gint by = g_strcmp0(a, b);

	return (by < 0) ? -1 : ((by > 0) ? 1 : 0);
}

/* NONE sorts last whichever way: compare known values only. */
static gint
ma_compare_figure(
	gint64	a,
	gint64	b
){
	if (a == b)
		return 0;

	if (VENTURE_SERIES_NONE == a)
		return 2;

	if (VENTURE_SERIES_NONE == b)
		return -2;

	return (a < b) ? -1 : 1;
}

static gint
ma_compare_accounts(
	gconstpointer	a,
	gconstpointer	b,
	gpointer	data
){
	const MaAccount *one = *(MaAccount *const *)a;
	const MaAccount *two = *(MaAccount *const *)b;
	const MaOrder *order = data;
	const gchar *ma_sort_name = order->name;
	gint by = 0;

	/* Grouped by login, the login leads, then the realm: a login's rows
	 * and, inside it, a realm's must be contiguous for their headers. The
	 * accounts reached through no login come last either way. */
	if (order->login_first)
	{
		by = ma_compare_login_fold(one->login_fold, two->login_fold);

		if (0 != by)
			return by;
	}

	/* Grouped by realm, the realm leads whatever else sorts, or a realm's
	 * rows interleave with another's and its header is drawn twice. The
	 * realm sort itself goes the way it was asked, below. */
	if ((order->realm_first || order->login_first) && (0 != g_strcmp0(ma_sort_name, "realm")))
	{
		by = ma_compare_text(one->realm_fold, two->realm_fold);

		if (0 != by)
			return by;
	}

	if (0 == g_strcmp0(ma_sort_name, "name"))
		by = ma_compare_text(one->name_fold, two->name_fold);
	else if (0 == g_strcmp0(ma_sort_name, "realm"))
		by = ma_compare_text(one->realm_fold, two->realm_fold);
	else if (0 == g_strcmp0(ma_sort_name, "gold"))
		by = ma_compare_figure(one->gold, two->gold);
	else if (0 == g_strcmp0(ma_sort_name, "positions"))
		by = ma_compare_figure(one->positions, two->positions);
	else if (0 == g_strcmp0(ma_sort_name, "expiry"))
		by = ma_compare_figure(one->soonest, two->soonest);
	else if (0 == g_strcmp0(ma_sort_name, "inbound"))
		by = ma_compare_figure(one->inbound, two->inbound);
	else if (0 == g_strcmp0(ma_sort_name, "last_seen"))
		by = ma_compare_figure(one->last_seen, two->last_seen);
	else if (0 == g_strcmp0(ma_sort_name, "freshness"))
		by = ma_compare_figure(one->synced_at, two->synced_at);
	else if (0 == g_strcmp0(ma_sort_name, "login"))
	{
		/* No login is last whichever way the order runs. */
		by = ma_compare_login_fold(one->login_fold, two->login_fold);

		if (('\0' == one->login_fold[0]) != ('\0' == two->login_fold[0]))
			by *= 2;
	}
	else
	{
		/* Attention: the accounts with a deadline first, soonest first,
		 * then the ones with something waiting, then the rest. */
		by = (one->tier != two->tier) ? ((one->tier < two->tier) ? -1 : 1)
		                              : ma_compare_figure(one->due, two->due);
	}

	/* A descending order turns the comparison round but keeps the
	 * unknown (+/-2) last. */
	if (order->descending && (by >= -1) && (by <= 1))
		by = -by;

	if (0 != by)
		return by;

	by = g_strcmp0(one->realm_fold, two->realm_fold);

	return (0 != by) ? by : g_strcmp0(one->name_fold, two->name_fold);
}

/*
 * Puts each login's places together, keeping the order inside it: the
 * logins go in the order of their most urgent place, since signing out of
 * one and into another is the costly switch, and a list that alternated
 * between two logins would have it made over and over. The accounts
 * reached through none are a group of their own, ordered the same way.
 */
static void
ma_group_attention(GPtrArray *attention)
{
	g_autoptr(GHashTable) groups = NULL;
	g_autoptr(GPtrArray) order = NULL;
	g_autoptr(GPtrArray) grouped = NULL;
	guint i;
	guint j;

	groups = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                               (GDestroyNotify)g_ptr_array_unref);
	order = g_ptr_array_new();

	for (i = 0; i < attention->len; i++)
	{
		MaAttention *row = g_ptr_array_index(attention, i);
		g_autofree gchar *key = NULL;
		GPtrArray *members;

		key = g_strdup_printf("%" G_GINT64_FORMAT "\x1f%s", row->source_id, row->login_key);
		members = g_hash_table_lookup(groups, key);

		if (NULL == members)
		{
			members = g_ptr_array_new();
			g_ptr_array_add(order, members);
			g_hash_table_insert(groups, g_steal_pointer(&key), members);
		}

		g_ptr_array_add(members, row);
	}

	grouped = g_ptr_array_new();

	for (i = 0; i < order->len; i++)
	{
		GPtrArray *members = g_ptr_array_index(order, i);

		for (j = 0; j < members->len; j++)
			g_ptr_array_add(grouped, g_ptr_array_index(members, j));
	}

	for (i = 0; i < grouped->len; i++)
		attention->pdata[i] = g_ptr_array_index(grouped, i);
}

/* Logins by name, then source; the accounts reached through none last. */
static gint
ma_compare_logins(
	gconstpointer	a,
	gconstpointer	b
){
	const MaLogin *one = *(MaLogin *const *)a;
	const MaLogin *two = *(MaLogin *const *)b;
	gint by;

	if (('\0' == one->key[0]) != ('\0' == two->key[0]))
		return ('\0' == one->key[0]) ? 1 : -1;

	by = g_utf8_collate(one->name, two->name);

	if (0 != by)
		return by;

	if (one->source_id != two->source_id)
		return (one->source_id < two->source_id) ? -1 : 1;

	return g_strcmp0(one->key, two->key);
}

/* How many logins the accounts shown are reached through. */
static gint64
ma_count_logins(MaOverview *view)
{
	gint64 count = 0;
	guint i;

	for (i = 0; (NULL != view->logins) && (i < view->logins->len); i++)
	{
		MaLogin *login = g_ptr_array_index(view->logins, i);

		if ('\0' != login->key[0])
			count++;
	}

	return count;
}

/* One login's card. */
static JsonObject *
ma_login_json(
	MaLogin		*login,
	const gchar	*first_currency
){
	JsonObject *object;

	object = json_object_new();
	json_object_set_int_member(object, "data_source_id", login->source_id);

	if ('\0' == login->key[0])
	{
		/* No url: "reached through no login" is not a filter, it is
		 * what is left once each login is counted. */
		json_object_set_null_member(object, "key");
		json_object_set_null_member(object, "url");
	}
	else
	{
		g_autoptr(GString) url = g_string_new("/accounts?login=");

		g_string_append_uri_escaped(url, login->key, NULL, FALSE);
		g_string_append_printf(url, "&source=%" G_GINT64_FORMAT, login->source_id);
		json_object_set_string_member(object, "key", login->key);
		json_object_set_string_member(object, "url", url->str);
	}

	json_object_set_string_member(object, "name", login->name);
	ma_set_text(object, "kind", login->kind);
	ma_set_text(object, "group_key", venture_string_is_empty(login->group_key) ? NULL
	                                                                          : login->group_key);
	json_object_set_int_member(object, "accounts", login->accounts);
	json_object_set_int_member(object, "characters", login->characters);
	json_object_set_int_member(object, "realms", g_hash_table_size(login->realms));
	json_object_set_int_member(object, "needs_login", login->needs_login);
	json_object_set_array_member(object, "balances", ma_amounts_json(login->balances, first_currency));
	json_object_set_array_member(object, "inventory_value",
	                             ma_amounts_json(login->inventory, first_currency));
	json_object_set_int_member(object, "inventory_lines", login->inventory_lines);
	json_object_set_int_member(object, "inventory_priced_lines", login->inventory_priced);
	json_object_set_int_member(object, "positions", login->positions);
	json_object_set_int_member(object, "positions_expired", login->positions_expired);
	json_object_set_int_member(object, "positions_expiring", login->positions_expiring);
	json_object_set_array_member(object, "positions_value",
	                             ma_amounts_json(login->positions_value, first_currency));
	json_object_set_int_member(object, "inbound", login->inbound);
	json_object_set_array_member(object, "net_30d", ma_amounts_json(login->net, first_currency));

	return object;
}

#endif /* VENTURE_HAVE_SQLITE */

/* A threshold of a question: 0 is the default, the rest must lie in
 * [1, @max]. */
static gboolean
ma_threshold(
	gint64		  value,
	gint64		  fallback,
	gint64		  max,
	const gchar	 *what,
	gint64		 *out,
	GError		**error
){
	if (0 == value)
	{
		*out = fallback;
		return TRUE;
	}

	if ((value < 1) || (value > max))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "%s is a whole number from 1 to %" G_GINT64_FORMAT, what, max);
		return FALSE;
	}

	*out = value;

	return TRUE;
}

JsonNode *
venture_marketdata_accounts(
	VentureContext				 *context,
	const VentureMarketdataAccountsQuery	 *query,
	GError					**error
){
	JsonObject *root;
	JsonObject *echo;
	JsonObject *summary;
	JsonArray *notes;
	JsonArray *list;
	gint64 now;
	gint64 hours;
	gint64 mail_days;
	gint64 stale_days;
	gboolean available;
	guint i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	g_return_val_if_fail(NULL != query, NULL);

	if (!ma_require_module(context, error) ||
	    !ma_check_choice(query->basis, ma_bases, "The valuation basis", error) ||
	    !ma_check_choice(query->sort, ma_account_sorts, "The account table's order", error) ||
	    !ma_threshold(query->expiring_hours, VENTURE_MARKETDATA_ACCOUNTS_EXPIRING_HOURS, MA_MAX_HOURS,
	                  "expiring_hours", &hours, error) ||
	    !ma_threshold(query->mail_days, VENTURE_MARKETDATA_ACCOUNTS_MAIL_DAYS, MA_MAX_MAIL_DAYS,
	                  "mail_days", &mail_days, error) ||
	    !ma_threshold(query->stale_days, VENTURE_MARKETDATA_ACCOUNTS_STALE_DAYS, MA_MAX_STALE_DAYS,
	                  "stale_days", &stale_days, error))
		return NULL;

	now = ma_now(query->now);
	root = json_object_new();
	notes = json_array_new();
	json_object_set_array_member(root, "notes", notes);
	json_object_set_int_member(root, "data_source_id", query->data_source_id);

	{
		g_autoptr(GDateTime) moment = g_date_time_new_from_unix_utc(now);
		g_autofree gchar *text = venture_time_to_string(moment);

		json_object_set_string_member(root, "now", text);
	}

	echo = json_object_new();
	json_object_set_string_member(echo, "basis", (NULL != query->basis) ? query->basis : "conservative");
	json_object_set_int_member(echo, "expiring_hours", hours);
	json_object_set_int_member(echo, "mail_days", mail_days);
	json_object_set_int_member(echo, "stale_days", stale_days);
	ma_set_text(echo, "group_key", query->group_key);
	ma_set_text(echo, "login", query->login);
	json_object_set_string_member(echo, "sort", (NULL != query->sort) ? query->sort : "attention");
	json_object_set_boolean_member(echo, "descending", query->descending);
	json_object_set_object_member(root, "query", echo);

	list = json_array_new();
	for (i = 0; NULL != ma_bases[i]; i++)
		json_array_add_string_element(list, ma_bases[i]);
	json_object_set_array_member(root, "bases", list);
	list = json_array_new();
	for (i = 0; NULL != ma_account_sorts[i]; i++)
		json_array_add_string_element(list, ma_account_sorts[i]);
	json_object_set_array_member(root, "sorts", list);

	available = ma_series_ready(context, notes);
	json_object_set_boolean_member(root, "available", available);
	summary = json_object_new();

#ifdef VENTURE_HAVE_SQLITE
	{
		g_autoptr(GPtrArray) sources = NULL;
		g_autoptr(GPtrArray) attention = NULL;
		MaOverview view;
		MaOrder order;
		JsonArray *rows;
		GHashTableIter iter;
		gpointer value;

		memset(&view, 0, sizeof(view));
		view.query = query;
		view.basis = ma_basis(query->basis);
		view.now = now;
		view.expiring_seconds = hours * MA_HOUR;
		view.mail_seconds = mail_days * MA_DAY;
		view.stale_seconds = stale_days * MA_DAY;
		view.balances = ma_amounts_new();
		view.inventory = ma_amounts_new();
		view.positions_value = ma_amounts_new();
		view.inbound_money = ma_amounts_new();
		view.sales = ma_amounts_new();
		view.purchases = ma_amounts_new();
		view.net = ma_amounts_new();
		view.realms = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
		view.attention = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, ma_attention_free);
		view.rows = g_ptr_array_new_with_free_func(ma_account_free);
		view.logins = g_ptr_array_new_with_free_func(ma_login_free);
		view.login_index = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
		view.login_choices = json_array_new();
		view.sources = json_array_new();
		view.shown = g_array_new(FALSE, FALSE, sizeof(gint64));

		if (available)
		{
			if (query->data_source_id > 0)
			{
				VentureEntity *one = ma_source(context, query->organization_id,
				                               query->data_source_id, error);

				if (NULL == one)
				{
					ma_overview_clear(&view);
					json_object_unref(summary);
					json_object_unref(root);
					return NULL;
				}

				sources = g_ptr_array_new_with_free_func(g_object_unref);
				g_ptr_array_add(sources, one);
			}
			else
				sources = ma_sources(context, query->organization_id, error);

			if (NULL == sources)
			{
				ma_overview_clear(&view);
				json_object_unref(summary);
				json_object_unref(root);
				return NULL;
			}

			for (i = 0; i < sources->len; i++)
			{
				if (!ma_overview_source(context, &view, g_ptr_array_index(sources, i), notes, error))
				{
					ma_overview_clear(&view);
					json_object_unref(summary);
					json_object_unref(root);
					return NULL;
				}
			}

			if (0 == view.accounts)
				ma_note(notes, "No data source has sent any accounts yet: push them (venturectl "
				               "feeds push) or sync a source whose plugin reports them.");
		}

		/* Most urgent first. */
		attention = g_ptr_array_new();
		g_hash_table_iter_init(&iter, view.attention);

		while (g_hash_table_iter_next(&iter, NULL, &value))
			g_ptr_array_add(attention, value);

		g_ptr_array_sort(attention, ma_compare_attention);
		ma_group_attention(attention);
		rows = json_array_new();

		for (i = 0; i < attention->len; i++)
		{
			MaAttention *row = g_ptr_array_index(attention, i);
			JsonObject *object = json_object_new();
			g_autofree gchar *title = NULL;
			const gchar *severity;

			/* Which login to sign in with comes first: it is the slow
			 * switch, the realm after it the quick one. */
			if (NULL != row->login_name)
				title = g_strdup_printf("Log in to %s \xe2\x86\x92 %s", row->login_name, row->realm);
			else
				title = g_strdup_printf("Log in to %s", row->realm);

			severity = (0 == row->tier) ? ((row->due <= now) ? "overdue" : "soon")
			         : ((1 == row->tier) ? "waiting" : "stale");
			json_object_set_int_member(object, "data_source_id", row->source_id);
			json_object_set_string_member(object, "realm", row->realm);

			if (NULL != row->login_name)
			{
				JsonObject *login = json_object_new();

				json_object_set_string_member(login, "key", row->login_key);
				json_object_set_string_member(login, "name", row->login_name);
				json_object_set_object_member(object, "login", login);
			}
			else
				json_object_set_null_member(object, "login");

			json_object_set_string_member(object, "title", title);
			json_object_set_string_member(object, "severity", severity);
			ma_set_time(object, "due_at", row->due);
			json_object_set_array_member(object, "accounts", json_array_ref(row->accounts));
			json_object_set_array_member(object, "reasons", json_array_ref(row->reasons));
			json_array_add_object_element(rows, object);
		}

		json_object_set_array_member(root, "attention", rows);

		order.name = (NULL != query->sort) ? query->sort : "attention";
		order.descending = query->descending;
		order.realm_first = query->realm_first;
		order.login_first = query->login_first;
		g_ptr_array_sort_with_data(view.rows, ma_compare_accounts, &order);
		rows = json_array_new();

		for (i = 0; i < view.rows->len; i++)
		{
			MaAccount *row = g_ptr_array_index(view.rows, i);

			json_array_add_object_element(rows, g_steal_pointer(&row->json));
		}

		json_object_set_array_member(root, "accounts", rows);
		json_object_set_array_member(root, "sources", json_array_ref(view.sources));

		/* A card per login, only when some account has one: an operator
		 * with a single login sees the page as it always was. */
		rows = json_array_new();

		if (view.any_login)
		{
			g_ptr_array_sort(view.logins, ma_compare_logins);

			for (i = 0; i < view.logins->len; i++)
				json_array_add_object_element(rows, ma_login_json(g_ptr_array_index(view.logins, i),
				                                                  view.first_currency));
		}

		json_object_set_array_member(root, "logins", rows);
		json_object_set_int_member(summary, "logins", ma_count_logins(&view));
		json_object_set_array_member(root, "login_choices", json_array_ref(view.login_choices));

		json_object_set_int_member(summary, "accounts", view.accounts);
		json_object_set_int_member(summary, "characters", view.characters);
		json_object_set_int_member(summary, "realms", g_hash_table_size(view.realms));
		json_object_set_int_member(summary, "stale_accounts", view.stale);
		json_object_set_int_member(summary, "needs_login", attention->len);
		json_object_set_array_member(summary, "balances",
		                             ma_amounts_json(view.balances, view.first_currency));
		json_object_set_array_member(summary, "inventory_value",
		                             ma_amounts_json(view.inventory, view.first_currency));
		json_object_set_int_member(summary, "inventory_lines", view.inventory_lines);
		json_object_set_int_member(summary, "inventory_priced_lines", view.inventory_priced);
		json_object_set_int_member(summary, "inventory_units", view.inventory_units);
		json_object_set_int_member(summary, "positions", view.positions);
		json_object_set_int_member(summary, "positions_units", view.positions_units);
		json_object_set_int_member(summary, "positions_expired", view.positions_expired);
		json_object_set_int_member(summary, "positions_expiring", view.positions_soon);
		json_object_set_array_member(summary, "positions_value",
		                             ma_amounts_json(view.positions_value, view.first_currency));
		json_object_set_int_member(summary, "inbound", view.inbound);
		json_object_set_int_member(summary, "inbound_items", view.inbound_items);
		json_object_set_array_member(summary, "inbound_money",
		                             ma_amounts_json(view.inbound_money, view.first_currency));
		json_object_set_array_member(summary, "sales_30d", ma_amounts_json(view.sales, view.first_currency));
		json_object_set_array_member(summary, "purchases_30d",
		                             ma_amounts_json(view.purchases, view.first_currency));
		json_object_set_array_member(summary, "net_30d", ma_amounts_json(view.net, view.first_currency));
		ma_set_text(summary, "currency", view.first_currency);
		json_object_set_object_member(root, "summary", summary);
		ma_attribute(context, query->organization_id, root, view.shown);
		ma_overview_clear(&view);
	}
#else
	json_object_set_array_member(root, "sources", json_array_new());
	json_object_set_array_member(root, "attention", json_array_new());
	json_object_set_array_member(root, "accounts", json_array_new());
	json_object_set_array_member(root, "logins", json_array_new());
	json_object_set_array_member(root, "login_choices", json_array_new());
	json_object_set_int_member(summary, "accounts", 0);
	json_object_set_object_member(root, "summary", summary);
	ma_attribute(context, query->organization_id, root, NULL);
#endif

	return ma_node(root);
}

/* --- One account ---------------------------------------------------------------- */

#ifdef VENTURE_HAVE_SQLITE

static const gchar *
ma_place_label(const gchar *place)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS(ma_places); i++)
	{
		if (0 == g_strcmp0(ma_places[i].place, place))
			return ma_places[i].label;
	}

	return place;
}

/* How soon something runs out: expired, soon (inside @window), later,
 * or none when it never does. */
static const gchar *
ma_urgency(
	gint64	expires_at,
	gint64	now,
	gint64	window
){
	if (VENTURE_SERIES_NONE == expires_at)
		return "none";

	if (expires_at <= now)
		return "expired";

	return (expires_at - now <= window) ? "soon" : "later";
}

static void
ma_set_expiry(
	JsonObject	*object,
	gint64		 expires_at,
	gint64		 now,
	gint64		 window
){
	ma_set_time(object, "expires_at", expires_at);

	if (VENTURE_SERIES_NONE == expires_at)
		json_object_set_null_member(object, "expires_in_seconds");
	else
		json_object_set_int_member(object, "expires_in_seconds", expires_at - now);

	json_object_set_string_member(object, "urgency", ma_urgency(expires_at, now, window));
}

/* The promoted location an account stands for, when there is one. */
static void
ma_set_location(
	VentureContext	*context,
	JsonObject	*root,
	VentureEntity	*source,
	const gchar	*key
){
	g_autoptr(VentureEntity) location = NULL;
	g_autofree gchar *ref = NULL;

	json_object_set_null_member(root, "location");
	json_object_set_null_member(root, "listings_url");

	if (!venture_context_module_enabled(context, "sales"))
		return;

	ref = venture_marketdata_account_ref(source, key);

	if (NULL != ref)
		location = venture_marketdata_find_by_ref(venture_context_get_database(context),
		                                          VENTURE_TYPE_LOCATION,
		                                          venture_entity_get_organization_id(source), ref, NULL);

	if ((NULL != location) && !venture_entity_is_deleted(location))
	{
		g_autofree gchar *name = NULL;
		g_autofree gchar *url = NULL;
		g_autofree gchar *listings = NULL;
		JsonObject *object = json_object_new();
		gint64 id = venture_entity_get_id(location);

		g_object_get(location, "name", &name, NULL);
		url = g_strdup_printf("/e/location/%" G_GINT64_FORMAT, id);
		listings = g_strdup_printf("/e/listing?location_id=%" G_GINT64_FORMAT, id);
		json_object_set_int_member(object, "id", id);
		ma_set_text(object, "name", name);
		json_object_set_string_member(object, "url", url);
		json_object_set_object_member(root, "location", object);
		json_object_set_string_member(root, "listings_url", listings);
	}
}

/* The account's closing balance each day of the last ninety. */
static JsonObject *
ma_balance_history(
	VentureSeriesStore	 *store,
	const gchar		 *key,
	const gchar		 *currency,
	gint64			  now,
	GError			**error
){
	g_autoptr(GPtrArray) days = NULL;
	JsonObject *history;
	JsonArray *points;
	JsonArray *values;
	gint64 first_day;
	gint64 last = VENTURE_SERIES_NONE;
	guint i;

	first_day = ((now / MA_DAY) - (MA_HISTORY_DAYS - 1)) * MA_DAY;
	days = venture_series_store_balance_days(store, currency, first_day, now + 1, error);

	if (NULL == days)
		return NULL;

	values = ma_trend(days, key, first_day, MA_HISTORY_DAYS, &last);
	history = json_object_new();
	json_object_set_string_member(history, "currency", currency);
	points = json_array_new();

	for (i = 0; i < json_array_get_length(values); i++)
	{
		JsonObject *point;
		JsonNode *value = json_array_get_element(values, i);

		/* The chart starts where the history does: ninety days of
		 * nothing before an account was first seen say nothing. */
		if (JSON_NODE_HOLDS_NULL(value) && (0 == json_array_get_length(points)))
			continue;

		point = json_object_new();

		ma_set_time(point, "day", first_day + (gint64)i * MA_DAY);

		if (JSON_NODE_HOLDS_NULL(value))
			json_object_set_null_member(point, "amount");
		else
			json_object_set_int_member(point, "amount", json_node_get_int(value));

		json_array_add_object_element(points, point);
	}

	json_array_unref(values);
	json_object_set_array_member(history, "points", points);

	return history;
}

#endif /* VENTURE_HAVE_SQLITE */

JsonNode *
venture_marketdata_account(
	VentureContext				 *context,
	const VentureMarketdataAccountQuery	 *query,
	GError					**error
){
	JsonObject *root;
	JsonArray *notes;
	JsonArray *list;
	gint64 now;
	gint64 hours;
	guint i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	g_return_val_if_fail(NULL != query, NULL);

	if (!ma_require_module(context, error) ||
	    !ma_check_choice(query->basis, ma_bases, "The valuation basis", error) ||
	    !ma_threshold(query->expiring_hours, VENTURE_MARKETDATA_ACCOUNTS_EXPIRING_HOURS, MA_MAX_HOURS,
	                  "expiring_hours", &hours, error))
		return NULL;

	if (venture_string_is_empty(query->key))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "That is not an account");
		return NULL;
	}

	if (query->ledger_count > VENTURE_MARKETDATA_ACCOUNTS_MAX_PAGE)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "At most %d ledger rows", VENTURE_MARKETDATA_ACCOUNTS_MAX_PAGE);
		return NULL;
	}

	now = ma_now(query->now);
	root = json_object_new();
	notes = json_array_new();
	json_object_set_array_member(root, "notes", notes);
	json_object_set_int_member(root, "data_source_id", query->data_source_id);
	json_object_set_string_member(root, "key", query->key);
	json_object_set_string_member(root, "basis", (NULL != query->basis) ? query->basis : "conservative");
	json_object_set_int_member(root, "expiring_hours", hours);
	list = json_array_new();
	for (i = 0; NULL != ma_bases[i]; i++)
		json_array_add_string_element(list, ma_bases[i]);
	json_object_set_array_member(root, "bases", list);

	{
		g_autoptr(GDateTime) moment = g_date_time_new_from_unix_utc(now);
		g_autofree gchar *text = venture_time_to_string(moment);

		json_object_set_string_member(root, "now", text);
	}

	json_object_set_boolean_member(root, "available", ma_series_ready(context, notes));

#ifdef VENTURE_HAVE_SQLITE
	if (json_object_get_boolean_member(root, "available"))
	{
		g_autoptr(VentureEntity) source = NULL;
		g_autoptr(VentureSeriesStore) store = NULL;
		g_autoptr(VentureSeriesAccountRow) account = NULL;
		g_autoptr(GHashTable) venues = NULL;
		g_autoptr(GPtrArray) lines = NULL;
		g_autoptr(GPtrArray) positions = NULL;
		g_autoptr(GPtrArray) inbound = NULL;
		g_autoptr(GPtrArray) ledger = NULL;
		g_autoptr(GPtrArray) totals = NULL;
		g_autoptr(GArray) shown = NULL;
		g_autoptr(GArray) holdings_value = NULL;
		g_autoptr(GArray) positions_value = NULL;
		g_autoptr(GArray) sales = NULL;
		g_autoptr(GArray) purchases = NULL;
		g_autoptr(GArray) net = NULL;
		g_autofree gchar *currency = NULL;
		g_autofree gchar *default_group = NULL;
		g_autofree gchar *source_name = NULL;
		g_autofree gchar *realm = NULL;
		g_autoptr(GError) local_error = NULL;
		VentureSeriesValueFilter value_filter;
		VentureSeriesPositionFilter position_filter;
		VentureSeriesInboundFilter inbound_filter;
		VentureSeriesTxnFilter txn_filter;
		MaOverview view;
		MaAccount *row;
		MaOps *ops;
		JsonObject *object;
		JsonArray *array;
		gint64 source_id = query->data_source_id;

		source = ma_source(context, query->organization_id, source_id, error);

		if (NULL == source)
			goto fail;

		store = ma_reader(context, source, NULL, error);

		if ((NULL == store) ||
		    !venture_series_store_get_account(store, query->key, now, &account, error))
			goto fail;

		source_name = ma_source_name(source);

		if (NULL == account)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
			            "%s has no account \"%s\"", source_name, query->key);
			goto fail;
		}

		currency = ma_source_currency(source);
		json_object_set_string_member(root, "source_name", source_name);
		ma_set_text(root, "currency", currency);
		venues = ma_venues(store, &default_group, error);

		if (NULL == venues)
			goto fail;

		realm = ma_realm(account, venues);
		ops = ma_ops_new();

		/* Listings, valued and against the market where they are up. */
		venture_series_position_filter_init(&position_filter);
		position_filter.account_key = query->key;
		positions = venture_series_store_list_positions(store, &position_filter, error);

		if (NULL == positions)
		{
			ma_ops_free(ops);
			goto fail;
		}

		positions_value = ma_amounts_new();
		array = json_array_new();

		for (i = 0; i < positions->len; i++)
		{
			VentureSeriesPositionRow *position = g_ptr_array_index(positions, i);
			g_autoptr(VentureSeriesRow) market = NULL;
			g_autofree gchar *url = NULL;
			const gchar *venue_name;
			gint64 total = VENTURE_SERIES_NONE;

			object = json_object_new();
			json_array_add_object_element(array, object);
			json_object_set_string_member(object, "key", position->key);
			json_object_set_string_member(object, "venue_key", position->venue_key);
			venue_name = g_hash_table_lookup(venues, position->venue_key);
			ma_set_text(object, "venue_name", venue_name);
			json_object_set_string_member(object, "instrument_key", position->instrument_key);
			ma_set_text(object, "instrument_name", position->instrument_name);
			json_object_set_int_member(object, "quantity", position->quantity);
			ma_set_money(object, "unit_price", position->unit_price, position->currency);
			ma_set_money(object, "bid", position->bid, position->currency);

			if (venture_series_math_mul(position->quantity, position->unit_price, &total))
			{
				if (!ma_amounts_add(positions_value, position->currency, total, error))
				{
					ma_ops_free(ops);
					goto fail;
				}
			}
			else
				total = VENTURE_SERIES_NONE;

			ma_set_money(object, "total", total, position->currency);
			ops->positions++;
			ops->units += position->quantity;

			if ((VENTURE_SERIES_NONE != position->expires_at) && (position->expires_at <= now))
			{
				ops->expired++;
				ops->expired_units += position->quantity;

				if ((VENTURE_SERIES_NONE == ops->earliest_expired) ||
				    (position->expires_at < ops->earliest_expired))
					ops->earliest_expired = position->expires_at;
			}
			else if ((VENTURE_SERIES_NONE != position->expires_at) &&
			         (position->expires_at - now <= hours * MA_HOUR))
			{
				ops->soon++;
				ops->soon_units += position->quantity;

				if ((VENTURE_SERIES_NONE == ops->soonest_future) ||
				    (position->expires_at < ops->soonest_future))
					ops->soonest_future = position->expires_at;
			}

			/* The market where it is listed: a cheaper offer than this
			 * one is the undercut a seller logs in to answer. */
			if (!venture_series_store_get_current(store, position->venue_key, position->instrument_key,
			                                      &market, &local_error))
			{
				g_propagate_error(error, g_steal_pointer(&local_error));
				ma_ops_free(ops);
				goto fail;
			}

			if ((NULL != market) && (0 == g_ascii_strcasecmp(market->currency, position->currency)))
			{
				gboolean undercut;

				ma_set_money(object, "market", market->market_value, market->currency);
				ma_set_money(object, "venue_min", market->min_price, market->currency);
				ma_set_ratio(object, "vs_market_pct",
				             ((VENTURE_SERIES_NONE != market->market_value) && (market->market_value > 0))
				             ? ((gdouble)position->unit_price * 100.0 / (gdouble)market->market_value)
				             : NAN);
				undercut = (VENTURE_SERIES_NONE != market->min_price) && (market->quantity > 0) &&
				           (market->min_price < position->unit_price);
				json_object_set_boolean_member(object, "undercut", undercut);
				ma_set_time(object, "market_at", market->taken_at);
			}
			else
			{
				json_object_set_null_member(object, "market");
				json_object_set_null_member(object, "venue_min");
				json_object_set_null_member(object, "vs_market_pct");
				json_object_set_boolean_member(object, "undercut", FALSE);
				json_object_set_null_member(object, "market_at");
			}

			ma_set_expiry(object, position->expires_at, now, hours * MA_HOUR);
			ma_set_time(object, "posted_at", position->posted_at);
			ma_set_time(object, "first_seen", position->first_seen);
			url = venture_marketdata_instrument_path(source_id, position->instrument_key,
			                                         position->venue_key);
			json_object_set_string_member(object, "url", url);

			if ((NULL != currency) && (0 == g_ascii_strcasecmp(position->currency, currency)) &&
			    (VENTURE_SERIES_NONE != total))
				(void)venture_series_math_add(ops->value, total, &ops->value);
		}

		json_object_set_array_member(root, "positions", array);
		json_object_set_array_member(root, "positions_value", ma_amounts_json(positions_value, currency));

		/* Mail. */
		venture_series_inbound_filter_init(&inbound_filter);
		inbound_filter.account_key = query->key;
		inbound = venture_series_store_list_inbound(store, &inbound_filter, error);

		if (NULL == inbound)
		{
			ma_ops_free(ops);
			goto fail;
		}

		array = json_array_new();

		for (i = 0; i < inbound->len; i++)
		{
			VentureSeriesInboundRow *mail = g_ptr_array_index(inbound, i);
			const gchar *mail_currency = ('\0' != mail->currency[0]) ? mail->currency : currency;

			object = json_object_new();
			json_array_add_object_element(array, object);
			json_object_set_string_member(object, "key", mail->key);
			ma_set_text(object, "sender", mail->sender);
			ma_set_text(object, "subject", mail->subject);
			ma_set_money(object, "money", mail->money, mail_currency);
			ma_set_money(object, "cod", mail->cod, mail_currency);
			ma_set_text(object, "instrument_key", mail->instrument_key);
			ma_set_text(object, "instrument_name", mail->instrument_name);
			ma_set_figure(object, "quantity", mail->quantity);
			json_object_set_boolean_member(object, "returned", mail->returned);
			ma_set_expiry(object, mail->expires_at, now, VENTURE_MARKETDATA_ACCOUNTS_MAIL_DAYS * MA_DAY);

			if (NULL != mail->instrument_key)
			{
				g_autofree gchar *url = venture_marketdata_instrument_path(source_id,
				                                                           mail->instrument_key, NULL);

				json_object_set_string_member(object, "url", url);
				ops->mail_items++;
				ops->mail_item_units += (VENTURE_SERIES_NONE != mail->quantity) ? mail->quantity : 1;
			}
			else
				json_object_set_null_member(object, "url");

			if ((VENTURE_SERIES_NONE != mail->expires_at) && (mail->expires_at > now) &&
			    (mail->expires_at - now <= VENTURE_MARKETDATA_ACCOUNTS_MAIL_DAYS * MA_DAY))
			{
				ops->mail_soon++;

				if ((VENTURE_SERIES_NONE == ops->mail_soonest) || (mail->expires_at < ops->mail_soonest))
					ops->mail_soonest = mail->expires_at;
			}
		}

		json_object_set_array_member(root, "inbound", array);

		/* The account's own row, as the overview draws it, reasons and
		 * all, so the page header says what the overview said. */
		memset(&view, 0, sizeof(view));
		view.now = now;
		view.expiring_seconds = hours * MA_HOUR;
		view.mail_seconds = VENTURE_MARKETDATA_ACCOUNTS_MAIL_DAYS * MA_DAY;
		view.stale_seconds = VENTURE_MARKETDATA_ACCOUNTS_STALE_DAYS * MA_DAY;
		view.attention = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, ma_attention_free);
		row = ma_account_row(&view, source, account, realm, ops, currency, NULL, 0);
		ma_ops_free(ops);
		json_object_set_object_member(root, "account", g_steal_pointer(&row->json));
		ma_account_free(row);
		g_hash_table_unref(view.attention);

		ma_set_location(context, root, source, query->key);

		/* Money over time. */
		if (NULL != currency)
		{
			JsonObject *history = ma_balance_history(store, query->key, currency, now, error);

			if (NULL == history)
				goto fail;

			json_object_set_object_member(root, "balance_history", history);
		}
		else
			json_object_set_null_member(root, "balance_history");

		/* What it holds, place by place, valued. */
		venture_series_value_filter_init(&value_filter);
		value_filter.currency = (NULL != currency) ? currency : "XXX";
		value_filter.basis = ma_basis(query->basis);
		value_filter.default_group = default_group;
		value_filter.now = now;
		value_filter.account_key = query->key;
		lines = venture_series_store_value_lines(store, &value_filter, NULL, error);

		if (NULL == lines)
			goto fail;

		holdings_value = ma_amounts_new();
		array = json_array_new();

		{
			JsonArray *places = json_array_new();
			guint p;

			for (p = 0; p < G_N_ELEMENTS(ma_places) + 1; p++)
			{
				gint64 count = 0;
				gint64 units = 0;
				gint64 value = 0;
				gint64 unpriced = 0;
				guint j;

				for (j = 0; j < lines->len; j++)
				{
					VentureSeriesValuedLine *line = g_ptr_array_index(lines, j);
					gboolean known = FALSE;
					guint k;

					for (k = 0; k < G_N_ELEMENTS(ma_places); k++)
						known = known || (0 == g_strcmp0(line->place, ma_places[k].place));

					/* The last pass gathers any place this build does
					 * not name, so nothing held is left off the page. */
					if ((p < G_N_ELEMENTS(ma_places)) ? (0 != g_strcmp0(line->place, ma_places[p].place))
					                                  : known)
						continue;

					count++;
					units += line->quantity;

					if (VENTURE_SERIES_NONE == line->value)
						unpriced++;
					else if (!venture_series_math_add(value, line->value, &value))
						value = G_MAXINT64;
				}

				if (0 == count)
					continue;

				object = json_object_new();
				json_object_set_string_member(object, "place",
				                              (p < G_N_ELEMENTS(ma_places)) ? ma_places[p].place : "other");
				json_object_set_string_member(object, "label",
				                              (p < G_N_ELEMENTS(ma_places)) ? ma_places[p].label : "Elsewhere");
				json_object_set_int_member(object, "lines", count);
				json_object_set_int_member(object, "units", units);
				json_object_set_int_member(object, "unpriced", unpriced);
				ma_set_money(object, "value", (count > unpriced) ? value : VENTURE_SERIES_NONE, currency);
				json_array_add_object_element(places, object);
			}

			json_object_set_array_member(root, "places", places);
		}

		for (i = 0; i < lines->len; i++)
		{
			VentureSeriesValuedLine *line = g_ptr_array_index(lines, i);
			g_autofree gchar *url = venture_marketdata_instrument_path(source_id, line->instrument_key,
			                                                           NULL);

			object = json_object_new();
			json_array_add_object_element(array, object);
			json_object_set_string_member(object, "place", line->place);
			json_object_set_string_member(object, "place_label", ma_place_label(line->place));
			json_object_set_string_member(object, "instrument_key", line->instrument_key);
			ma_set_text(object, "instrument_name", line->instrument_name);
			ma_set_text(object, "category", line->category);
			json_object_set_int_member(object, "quantity", line->quantity);
			ma_set_money(object, "unit_value", line->unit_value, currency);
			ma_set_money(object, "value", line->value, currency);
			json_object_set_string_member(object, "url", url);

			if ((NULL != currency) && !ma_amounts_add(holdings_value, currency, line->value, error))
				goto fail;
		}

		json_object_set_array_member(root, "holdings", array);
		json_object_set_member(root, "holdings_value",
		                       ma_money_node((NULL != currency) ? ma_amounts_get(holdings_value, currency)
		                                                        : VENTURE_SERIES_NONE, currency));

		/* The ledger: the newest rows, and the last thirty days summed. */
		venture_series_txn_filter_init(&txn_filter);
		txn_filter.account_key = query->key;
		txn_filter.descending = TRUE;
		txn_filter.count = (query->ledger_count > 0) ? query->ledger_count : MA_LEDGER_DEFAULT;
		ledger = venture_series_store_list_txns(store, &txn_filter, error);

		if (NULL == ledger)
			goto fail;

		array = json_array_new();

		for (i = 0; i < ledger->len; i++)
		{
			VentureSeriesTxnRow *txn = g_ptr_array_index(ledger, i);

			object = json_object_new();
			json_array_add_object_element(array, object);
			json_object_set_string_member(object, "key", txn->key);
			json_object_set_string_member(object, "kind", txn->kind);
			ma_set_text(object, "instrument_key", txn->instrument_key);
			ma_set_text(object, "instrument_name", txn->instrument_name);
			ma_set_figure(object, "quantity", txn->quantity);
			ma_set_money(object, "unit_price", txn->unit_price, txn->currency);
			ma_set_money(object, "amount", txn->amount, txn->currency);
			ma_set_text(object, "venue_key", txn->venue_key);
			ma_set_text(object, "counterparty", txn->counterparty);
			ma_set_text(object, "source", txn->source);
			ma_set_time(object, "at", txn->at);
		}

		json_object_set_array_member(root, "ledger", array);

		venture_series_txn_filter_init(&txn_filter);
		txn_filter.account_key = query->key;
		ma_thirty_days(context, now, &txn_filter.since, &txn_filter.until);
		totals = venture_series_store_txn_totals(store, &txn_filter, VENTURE_SERIES_TXN_GROUP_MONTH, error);

		if (NULL == totals)
			goto fail;

		sales = ma_amounts_new();
		purchases = ma_amounts_new();
		net = ma_amounts_new();

		for (i = 0; i < totals->len; i++)
		{
			VentureSeriesTxnTotal *bucket = g_ptr_array_index(totals, i);
			gint64 spent;

			if (!venture_series_math_add(bucket->buys_amount, bucket->expense, &spent) ||
			    !ma_amounts_add(sales, bucket->currency, bucket->sales_amount, error) ||
			    !ma_amounts_add(purchases, bucket->currency, spent, error) ||
			    !ma_amounts_add(net, bucket->currency, bucket->net, error))
				goto fail;
		}

		object = json_object_new();
		json_object_set_array_member(object, "sales", ma_amounts_json(sales, currency));
		json_object_set_array_member(object, "purchases", ma_amounts_json(purchases, currency));
		json_object_set_array_member(object, "net", ma_amounts_json(net, currency));
		json_object_set_object_member(root, "ledger_30d", object);

		shown = g_array_new(FALSE, FALSE, sizeof(gint64));
		g_array_append_val(shown, source_id);
		ma_attribute(context, query->organization_id, root, shown);

		return ma_node(root);

	fail:
		/* Every error path set @error; nothing is half-answered. */
		json_object_unref(root);
		return NULL;
	}
#endif

	json_object_set_null_member(root, "account");
	json_object_set_array_member(root, "positions", json_array_new());
	json_object_set_array_member(root, "inbound", json_array_new());
	json_object_set_array_member(root, "holdings", json_array_new());
	json_object_set_array_member(root, "places", json_array_new());
	json_object_set_array_member(root, "ledger", json_array_new());
	ma_attribute(context, query->organization_id, root, NULL);

	return ma_node(root);
}

/* --- The inventory ------------------------------------------------------------------ */

#ifdef VENTURE_HAVE_SQLITE

/*
 * @groups (consumed) with the one whose "key" is null -- the accounts
 * reached through no login -- moved to the end, where every login list
 * puts it; the store sorts "" first.
 */
static JsonArray *
ma_logins_last(JsonArray *groups)
{
	JsonArray *sorted;
	JsonNode *none = NULL;
	guint i;

	sorted = json_array_new();

	for (i = 0; i < json_array_get_length(groups); i++)
	{
		JsonObject *group = json_array_get_object_element(groups, i);

		if (json_object_get_null_member(group, "key"))
			none = json_array_dup_element(groups, i);
		else
			json_array_add_element(sorted, json_array_dup_element(groups, i));
	}

	if (NULL != none)
		json_array_add_element(sorted, none);

	json_array_unref(groups);

	return sorted;
}

#endif /* VENTURE_HAVE_SQLITE */

JsonNode *
venture_marketdata_inventory(
	VentureContext				 *context,
	const VentureMarketdataInventoryQuery	 *query,
	GError					**error
){
	JsonObject *root;
	JsonObject *echo;
	JsonArray *notes;
	JsonArray *list;
	gint64 now;
	gint64 dead_days;
	guint page;
	guint per_page;
	guint i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	g_return_val_if_fail(NULL != query, NULL);

	if (!ma_require_module(context, error) ||
	    !ma_check_choice(query->basis, ma_bases, "The valuation basis", error) ||
	    !ma_check_choice(query->sort, ma_inventory_sorts, "The inventory's order", error) ||
	    !ma_check_choice(query->group_by, ma_inventory_groups, "The inventory's grouping", error) ||
	    !ma_threshold(query->dead_days, VENTURE_MARKETDATA_ACCOUNTS_DEAD_DAYS, MA_MAX_DEAD_DAYS,
	                  "dead_days", &dead_days, error))
		return NULL;

	page = (0 == query->page) ? 1 : query->page;
	per_page = (0 == query->per_page) ? 50 : query->per_page;

	if ((page > MA_MAX_PAGE_NUMBER) || (per_page > VENTURE_MARKETDATA_ACCOUNTS_MAX_PAGE))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "A page is at most %d rows, and there are at most %d pages",
		            VENTURE_MARKETDATA_ACCOUNTS_MAX_PAGE, MA_MAX_PAGE_NUMBER);
		return NULL;
	}

	now = ma_now(query->now);
	root = json_object_new();
	notes = json_array_new();
	json_object_set_array_member(root, "notes", notes);
	json_object_set_int_member(root, "data_source_id", query->data_source_id);
	json_object_set_string_member(root, "basis", (NULL != query->basis) ? query->basis : "conservative");
	list = json_array_new();
	for (i = 0; NULL != ma_bases[i]; i++)
		json_array_add_string_element(list, ma_bases[i]);
	json_object_set_array_member(root, "bases", list);
	list = json_array_new();
	for (i = 0; NULL != ma_inventory_sorts[i]; i++)
		json_array_add_string_element(list, ma_inventory_sorts[i]);
	json_object_set_array_member(root, "sorts", list);

	echo = json_object_new();
	ma_set_text(echo, "account", query->account);
	ma_set_text(echo, "login", query->login);
	ma_set_text(echo, "group_by", query->group_by);
	ma_set_text(echo, "place", query->place);
	ma_set_text(echo, "category", query->category);
	ma_set_text(echo, "search", query->search);
	ma_set_text(echo, "min_value", query->min_value);
	json_object_set_boolean_member(echo, "dead", query->dead);
	json_object_set_int_member(echo, "dead_days", dead_days);
	json_object_set_string_member(echo, "sort", (NULL != query->sort) ? query->sort : "value");
	json_object_set_boolean_member(echo, "descending", query->descending);
	json_object_set_object_member(root, "query", echo);
	json_object_set_int_member(root, "page", page);
	json_object_set_int_member(root, "per_page", per_page);

	json_object_set_boolean_member(root, "available", ma_series_ready(context, notes));
	json_object_set_array_member(root, "rows", json_array_new());
	json_object_set_array_member(root, "accounts", json_array_new());
	json_object_set_array_member(root, "logins", json_array_new());
	json_object_set_array_member(root, "by_login", json_array_new());

	/* The places a holding can be in, for a picker. */
	list = json_array_new();

	for (i = 0; i < G_N_ELEMENTS(ma_places); i++)
	{
		JsonObject *place = json_object_new();

		json_object_set_string_member(place, "place", ma_places[i].place);
		json_object_set_string_member(place, "label", ma_places[i].label);
		json_array_add_object_element(list, place);
	}

	json_object_set_array_member(root, "places", list);
	json_object_set_int_member(root, "pages", 0);
	json_object_set_null_member(root, "portfolio_value");

#ifdef VENTURE_HAVE_SQLITE
	if (json_object_get_boolean_member(root, "available"))
	{
		g_autoptr(GPtrArray) sources = NULL;
		g_autoptr(VentureEntity) source = NULL;
		g_autoptr(VentureSeriesStore) store = NULL;
		g_autoptr(GPtrArray) accounts = NULL;
		g_autoptr(GPtrArray) rows = NULL;
		g_autoptr(GPtrArray) lines = NULL;
		g_autoptr(GPtrArray) keys = NULL;
		g_autoptr(GPtrArray) whole = NULL;
		g_autoptr(GHashTable) venues = NULL;
		g_autoptr(GHashTable) login_of = NULL;
		g_autoptr(GArray) shown = NULL;
		g_autoptr(VentureMoney) bound = NULL;
		g_autofree gchar *currency = NULL;
		g_autofree gchar *default_group = NULL;
		g_autofree gchar *source_name = NULL;
		VentureSeriesValueFilter filter;
		VentureSeriesValueFilter everything;
		VentureSeriesValueTotals totals;
		VentureSeriesValueTotals portfolio;
		JsonObject *object;
		JsonArray *array;
		gint64 source_id;

		sources = ma_sources(context, query->organization_id, error);

		if (NULL == sources)
			goto fail;

		json_object_set_array_member(root, "sources", ma_sources_json(sources));
		source = ma_pick_source(context, query->organization_id, query->data_source_id, sources, error);

		if ((NULL == source) && (query->data_source_id > 0))
			goto fail;

		if (NULL == source)
		{
			ma_note(notes, "No data sources yet.");
			goto done;
		}

		source_id = venture_entity_get_id(source);
		source_name = ma_source_name(source);
		currency = ma_source_currency(source);
		json_object_set_int_member(root, "data_source_id", source_id);
		json_object_set_string_member(root, "source_name", source_name);
		ma_set_text(root, "currency", currency);
		store = ma_reader(context, source, notes, NULL);

		if (NULL == store)
			goto done;

		/* A bound must be in the currency the source values in: nothing
		 * converts, and a bound in another would answer nothing. */
		if (NULL != query->min_value)
		{
			bound = venture_marketdata_parse_amount(query->min_value, error);

			if (NULL == bound)
				goto fail;

			if ((NULL == currency) || (0 != g_ascii_strcasecmp(venture_money_get_currency(bound), currency)))
			{
				g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
				            "min_value is in %s; %s values its holdings in %s",
				            venture_money_get_currency(bound), source_name,
				            (NULL != currency) ? currency : "no currency");
				goto fail;
			}
		}

		if (NULL == currency)
			ma_note(notes, "This source names no currency, so nothing held can be valued: set its "
			               "currency to the one its prices are in.");

		venues = ma_venues(store, &default_group, error);

		if (NULL == venues)
			goto fail;

		accounts = venture_series_store_list_accounts(store, NULL, NULL, NULL, now, error);

		if (NULL == accounts)
			goto fail;

		array = json_array_new();
		login_of = g_hash_table_new(g_str_hash, g_str_equal);

		for (i = 0; i < accounts->len; i++)
		{
			VentureSeriesAccountRow *account = g_ptr_array_index(accounts, i);
			g_autofree gchar *realm = ma_realm(account, venues);

			object = json_object_new();
			json_object_set_string_member(object, "key", account->key);
			json_object_set_string_member(object, "name", ma_account_name(account));
			json_object_set_string_member(object, "realm", realm);
			ma_set_text(object, "login", venture_string_is_empty(account->login_key)
			                             ? NULL : account->login_key);
			ma_set_text(object, "login_name", ma_login_name(account));
			json_array_add_object_element(array, object);
			g_hash_table_insert(login_of, account->key, account);
		}

		json_object_set_array_member(root, "accounts", array);

		/* The logins, for a picker; empty for a source that sends none. */
		if (!ma_login_choices(store, source_id, json_object_get_array_member(root, "logins"), error))
			goto fail;

		if (0 == accounts->len)
			ma_note(notes, "This source has sent no accounts yet.");

		venture_series_value_filter_init(&filter);
		filter.currency = (NULL != currency) ? currency : "XXX";
		filter.basis = ma_basis(query->basis);
		filter.default_group = default_group;
		filter.now = now;
		filter.account_key = query->account;
		filter.login_key = query->login;
		filter.place = query->place;

		/* A game's own tokens are holdings too, but not stock: left out
		 * unless asked for by place. */
		filter.exclude_place = (NULL == query->place) ? "currency" : NULL;
		filter.search = query->search;
		filter.category_prefix = query->category;
		filter.min_value = (NULL != bound) ? venture_money_get_amount(bound) : VENTURE_SERIES_NONE;
		filter.unsold_since = query->dead ? now - dead_days * MA_DAY : VENTURE_SERIES_NONE;

		if (NULL != query->sort)
			(void)venture_series_value_sort_from_string(query->sort, &filter.sort);

		filter.descending = query->descending;
		filter.offset = (page - 1) * per_page;
		filter.count = per_page;

		/* The bound's minor units must be the store's: its currency's own
		 * exponent, whatever the text was written with. */
		if (NULL != bound)
		{
			g_autoptr(VentureMoney) natural = venture_money_new_for_currency(0, currency);
			g_autoptr(VentureMoney) scaled = NULL;

			if (NULL != natural)
				scaled = venture_money_rescale(bound, venture_money_get_exponent(natural), error);

			if (NULL == scaled)
			{
				if ((NULL != error) && (NULL == *error))
					g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
					            "min_value cannot be read in %s", currency);
				goto fail;
			}

			filter.min_value = venture_money_get_amount(scaled);
		}

		rows = venture_series_store_value_instruments(store, &filter, &totals, error);

		if (NULL == rows)
			goto fail;

		/* The whole portfolio, for each row's share of it. */
		venture_series_value_filter_init(&everything);
		everything.currency = filter.currency;
		everything.basis = filter.basis;
		everything.default_group = default_group;
		everything.now = now;
		everything.exclude_place = "currency";
		everything.count = 1;
		whole = venture_series_store_value_instruments(store, &everything, &portfolio, error);

		if (NULL == whole)
			goto fail;

		json_object_set_member(root, "portfolio_value",
		                       (portfolio.priced_lines > 0) ? ma_money_node(portfolio.value, currency)
		                                                    : json_node_new(JSON_NODE_NULL));
		object = json_object_new();
		json_object_set_int_member(object, "instruments", totals.instruments);
		json_object_set_int_member(object, "lines", totals.lines);
		json_object_set_int_member(object, "priced_lines", totals.priced_lines);
		json_object_set_int_member(object, "units", totals.quantity);
		ma_set_money(object, "value", (totals.priced_lines > 0) ? totals.value : VENTURE_SERIES_NONE,
		             currency);
		json_object_set_object_member(root, "totals", object);
		json_object_set_int_member(root, "total", totals.instruments);
		json_object_set_int_member(root, "pages", (totals.instruments + per_page - 1) / per_page);

		if ((totals.lines > totals.priced_lines) && (NULL != currency))
		{
			g_autofree gchar *text = NULL;

			text = g_strdup_printf("%" G_GINT64_FORMAT " of %" G_GINT64_FORMAT " holding lines have no "
			                       "%s price on the %s basis and count for nothing in the totals.",
			                       totals.lines - totals.priced_lines, totals.lines, currency,
			                       venture_series_value_basis_to_string(filter.basis));
			ma_note(notes, text);
		}

		/* Each row's breakdown, for this page's instruments only. */
		keys = g_ptr_array_new();

		for (i = 0; i < rows->len; i++)
			g_ptr_array_add(keys, ((VentureSeriesValuedInstrument *)g_ptr_array_index(rows, i))->instrument_key);

		g_ptr_array_add(keys, NULL);

		if (rows->len > 0)
		{
			VentureSeriesValueFilter lines_filter = filter;

			lines_filter.min_value = VENTURE_SERIES_NONE;
			lines = venture_series_store_value_lines(store, &lines_filter,
			                                         (const gchar *const *)keys->pdata, error);

			if (NULL == lines)
				goto fail;
		}

		array = json_array_new();

		for (i = 0; i < rows->len; i++)
		{
			VentureSeriesValuedInstrument *row = g_ptr_array_index(rows, i);
			g_autofree gchar *url = venture_marketdata_instrument_path(source_id, row->instrument_key, NULL);
			JsonArray *breakdown = json_array_new();
			guint j;

			object = json_object_new();
			json_array_add_object_element(array, object);
			json_object_set_string_member(object, "instrument_key", row->instrument_key);
			ma_set_text(object, "instrument_name", row->instrument_name);
			ma_set_text(object, "category", row->category);
			json_object_set_int_member(object, "quantity", row->quantity);
			json_object_set_int_member(object, "accounts", row->accounts);
			json_object_set_int_member(object, "lines", row->lines);
			json_object_set_int_member(object, "priced_lines", row->priced_lines);
			ma_set_money(object, "unit_value", row->unit_value, currency);
			ma_set_money(object, "value", row->value, currency);
			ma_set_ratio(object, "share",
			             ((VENTURE_SERIES_NONE != row->value) && (portfolio.value > 0))
			             ? (gdouble)row->value / (gdouble)portfolio.value : NAN);
			ma_set_ratio(object, "sold_per_day", row->sold_per_day);
			ma_set_ratio(object, "days_of_supply",
			             (isfinite(row->sold_per_day) && (row->sold_per_day > 0.0))
			             ? (gdouble)row->quantity / row->sold_per_day : NAN);
			ma_set_time(object, "last_sale", row->last_sale);
			json_object_set_string_member(object, "url", url);

			for (j = 0; (NULL != lines) && (j < lines->len); j++)
			{
				VentureSeriesValuedLine *line = g_ptr_array_index(lines, j);
				JsonObject *part;

				if (0 != g_strcmp0(line->instrument_key, row->instrument_key))
					continue;

				part = json_object_new();
				json_object_set_string_member(part, "account_key", line->account_key);
				json_object_set_string_member(part, "account_name",
				                              (NULL != line->account_name) ? line->account_name
				                                                           : line->account_key);

				{
					VentureSeriesAccountRow *holder = g_hash_table_lookup(login_of,
					                                                      line->account_key);

					ma_set_text(part, "login", ((NULL == holder) ||
					                            venture_string_is_empty(holder->login_key))
					                           ? NULL : holder->login_key);
					ma_set_text(part, "login_name", (NULL != holder) ? ma_login_name(holder) : NULL);
				}
				json_object_set_string_member(part, "place", line->place);
				json_object_set_string_member(part, "place_label", ma_place_label(line->place));
				json_object_set_int_member(part, "quantity", line->quantity);
				ma_set_money(part, "value", line->value, currency);
				json_array_add_object_element(breakdown, part);
			}

			json_object_set_array_member(object, "breakdown", breakdown);
		}

		json_object_set_array_member(root, "rows", array);

		/*
		 * The same holdings, login by login: the lines the filter selects,
		 * valued line by line as the rows are, so the logins add up to the
		 * totals. A minimum value bounds instruments, not lines, and has
		 * no meaning for a login's sum, so it is left out and said.
		 */
		if (0 == g_strcmp0(query->group_by, "login"))
		{
			g_autoptr(GPtrArray) groups = NULL;
			VentureSeriesValueFilter by_login = filter;

			by_login.min_value = VENTURE_SERIES_NONE;
			groups = venture_series_store_value_totals(store, &by_login, VENTURE_SERIES_VALUE_GROUP_LOGIN,
			                                           error);

			if (NULL == groups)
				goto fail;

			if (NULL != bound)
				ma_note(notes, "The holdings by login are not bounded by min_value: it applies to "
				               "an item's total, and a login's sum is of lines.");

			array = json_array_new();

			for (i = 0; i < groups->len; i++)
			{
				VentureSeriesValueGroupTotal *group = g_ptr_array_index(groups, i);
				gboolean none = venture_string_is_empty(group->key);

				object = json_object_new();
				ma_set_text(object, "key", none ? NULL : group->key);
				json_object_set_string_member(object, "name",
				                              none ? "No login"
				                                   : (!venture_string_is_empty(group->label)
				                                      ? group->label : group->key));
				json_object_set_int_member(object, "lines", group->lines);
				json_object_set_int_member(object, "priced_lines", group->priced_lines);
				json_object_set_int_member(object, "units", group->quantity);
				ma_set_money(object, "value", (group->priced_lines > 0) ? group->value
				                                                        : VENTURE_SERIES_NONE,
				             currency);
				ma_set_ratio(object, "share", ((group->priced_lines > 0) && (totals.value > 0))
				                              ? (gdouble)group->value / (gdouble)totals.value : NAN);

				json_array_add_object_element(array, object);
			}

			json_object_set_array_member(root, "by_login", ma_logins_last(array));
		}

		shown = g_array_new(FALSE, FALSE, sizeof(gint64));
		g_array_append_val(shown, source_id);
		ma_attribute(context, query->organization_id, root, shown);

		return ma_node(root);

	fail:
		json_object_unref(root);
		return NULL;

	done:
		;
	}
#else
	(void)now;
#endif

	if (!json_object_has_member(root, "sources"))
		json_object_set_array_member(root, "sources", json_array_new());

	if (!json_object_has_member(root, "totals"))
	{
		JsonObject *object = json_object_new();

		json_object_set_int_member(object, "instruments", 0);
		json_object_set_int_member(object, "lines", 0);
		json_object_set_int_member(object, "priced_lines", 0);
		json_object_set_int_member(object, "units", 0);
		json_object_set_null_member(object, "value");
		json_object_set_object_member(root, "totals", object);
		json_object_set_int_member(root, "total", 0);
	}

	ma_attribute(context, query->organization_id, root, NULL);

	return ma_node(root);
}

/* --- Profit and loss ------------------------------------------------------------------- */

#define MA_TREND_MAX_POINTS (2000)

#ifdef VENTURE_HAVE_SQLITE


/* One ledger bucket's money fields onto @object, in its currency. */
static void
ma_set_bucket(
	JsonObject			*object,
	const VentureSeriesTxnTotal	*bucket
){
	const gchar *currency = bucket->currency;

	json_object_set_string_member(object, "currency", currency);
	json_object_set_int_member(object, "txns", bucket->txns);
	json_object_set_int_member(object, "sales", bucket->sales);
	json_object_set_int_member(object, "sold_units", bucket->sold_units);
	ma_set_money(object, "sales_amount", bucket->sales_amount, currency);
	json_object_set_int_member(object, "buys", bucket->buys);
	json_object_set_int_member(object, "bought_units", bucket->bought_units);
	ma_set_money(object, "buys_amount", bucket->buys_amount, currency);
	ma_set_money(object, "income", bucket->income, currency);
	ma_set_money(object, "expense", bucket->expense, currency);
	ma_set_money(object, "net", bucket->net, currency);
	json_object_set_int_member(object, "expired_units", bucket->expired_units);
	json_object_set_int_member(object, "cancelled_units", bucket->cancelled_units);
}

/* Adds @bucket into @sum field by field, refusing an overflow. */
static gboolean
ma_bucket_add(
	VentureSeriesTxnTotal		 *sum,
	const VentureSeriesTxnTotal	 *bucket,
	GError				**error
){
	gint64 *into[] = {
		&sum->txns, &sum->sales, &sum->sold_units, &sum->sales_amount, &sum->buys,
		&sum->bought_units, &sum->buys_amount, &sum->income, &sum->expense,
		&sum->expired_units, &sum->cancelled_units, &sum->net
	};
	const gint64 from[] = {
		bucket->txns, bucket->sales, bucket->sold_units, bucket->sales_amount, bucket->buys,
		bucket->bought_units, bucket->buys_amount, bucket->income, bucket->expense,
		bucket->expired_units, bucket->cancelled_units, bucket->net
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(from); i++)
	{
		if (!venture_series_math_add(*into[i], from[i], into[i]))
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			                    "The ledger's totals do not fit in a 64-bit integer");
			return FALSE;
		}
	}

	return TRUE;
}

/* The next period's first second after @start, for a day, week or month. */
static gint64
ma_next_period(
	VentureSeriesTxnGroup	group,
	gint64			start
){
	g_autoptr(GDateTime) moment = NULL;
	g_autoptr(GDateTime) next = NULL;

	if (VENTURE_SERIES_TXN_GROUP_DAY == group)
		return start + MA_DAY;

	if (VENTURE_SERIES_TXN_GROUP_WEEK == group)
		return start + 7 * MA_DAY;

	moment = g_date_time_new_from_unix_utc(start);
	next = (NULL != moment) ? g_date_time_add_months(moment, 1) : NULL;

	return (NULL != next) ? g_date_time_to_unix(next) : start + 31 * MA_DAY;
}

/*
 * The chart behind the table: net, sales and purchases a period, in the
 * source's currency, every period from the first to the last filled in --
 * a day with no trades is a zero, not a day the axis skips.
 */
static JsonObject *
ma_trend_points(
	GPtrArray		*buckets,
	VentureSeriesTxnGroup	 group,
	const gchar		*currency
){
	JsonObject *trend;
	JsonArray *points;
	gint64 first = VENTURE_SERIES_NONE;
	gint64 last = VENTURE_SERIES_NONE;
	gint64 period;
	guint count = 0;
	guint i;

	trend = json_object_new();
	points = json_array_new();
	json_object_set_string_member(trend, "group",
	                              (VENTURE_SERIES_TXN_GROUP_DAY == group) ? "day"
	                              : ((VENTURE_SERIES_TXN_GROUP_WEEK == group) ? "week" : "month"));
	ma_set_text(trend, "currency", currency);
	json_object_set_array_member(trend, "points", points);

	for (i = 0; i < buckets->len; i++)
	{
		VentureSeriesTxnTotal *bucket = g_ptr_array_index(buckets, i);

		if ((NULL == currency) || (0 != g_ascii_strcasecmp(bucket->currency, currency)) ||
		    (VENTURE_SERIES_NONE == bucket->period_start))
			continue;

		if (VENTURE_SERIES_NONE == first)
			first = bucket->period_start;

		last = bucket->period_start;
	}

	if (VENTURE_SERIES_NONE == first)
		return trend;

	for (period = first; (period <= last) && (count < MA_TREND_MAX_POINTS);
	     period = ma_next_period(group, period), count++)
	{
		JsonObject *point = json_object_new();
		gint64 sales = 0;
		gint64 purchases = 0;
		gint64 net = 0;

		for (i = 0; i < buckets->len; i++)
		{
			VentureSeriesTxnTotal *bucket = g_ptr_array_index(buckets, i);

			if ((bucket->period_start != period) || (0 != g_ascii_strcasecmp(bucket->currency, currency)))
				continue;

			sales = bucket->sales_amount;
			(void)venture_series_math_add(bucket->buys_amount, bucket->expense, &purchases);
			net = bucket->net;
		}

		ma_set_time(point, "period_start", period);
		json_object_set_int_member(point, "sales", sales);
		json_object_set_int_member(point, "purchases", purchases);
		json_object_set_int_member(point, "net", net);
		json_array_add_object_element(points, point);
	}

	return trend;
}

static gint
ma_compare_profit(
	gconstpointer	a,
	gconstpointer	b
){
	const VentureSeriesTxnTotal *one = *(VentureSeriesTxnTotal *const *)a;
	const VentureSeriesTxnTotal *two = *(VentureSeriesTxnTotal *const *)b;

	/* The bucket's net is its cash profit: sales after the cut and other
	 * income, less what was bought and spent. */
	if (one->net != two->net)
		return (one->net > two->net) ? -1 : 1;

	return g_strcmp0(one->key, two->key);
}

#endif /* VENTURE_HAVE_SQLITE */

JsonNode *
venture_marketdata_external_pnl(
	VentureContext				 *context,
	const VentureMarketdataPnlQuery		 *query,
	GError					**error
){
	JsonObject *root;
	JsonArray *notes;
	JsonArray *list;
	const gchar *group_name;
	gint64 now;
	gint64 since;
	gint64 until;
	guint top;
	guint i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	g_return_val_if_fail(NULL != query, NULL);

	group_name = (NULL != query->group_by) ? query->group_by : "day";

	if (!ma_require_module(context, error) ||
	    !ma_check_choice(group_name, ma_pnl_groups, "group_by", error))
		return NULL;

	if (query->top > VENTURE_MARKETDATA_ACCOUNTS_MAX_PAGE)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "top is 1 to %d", VENTURE_MARKETDATA_ACCOUNTS_MAX_PAGE);
		return NULL;
	}

	top = (0 == query->top) ? MA_TOP_DEFAULT : query->top;
	now = ma_now(query->now);
	ma_thirty_days(context, now, &since, &until);
	since = (0 == query->since) ? since : ((query->since < 0) ? G_MININT64 : query->since);
	until = (0 == query->until) ? until : ((query->until < 0) ? G_MININT64 : query->until);

	if ((G_MININT64 != since) && (G_MININT64 != until) && (until <= since))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "The window ends before it starts");
		return NULL;
	}

	root = json_object_new();
	notes = json_array_new();
	json_object_set_array_member(root, "notes", notes);
	json_object_set_int_member(root, "data_source_id", query->data_source_id);
	json_object_set_string_member(root, "group_by", group_name);
	list = json_array_new();
	for (i = 0; NULL != ma_pnl_groups[i]; i++)
		json_array_add_string_element(list, ma_pnl_groups[i]);
	json_object_set_array_member(root, "groups", list);
	json_object_set_int_member(root, "top", top);

	{
		JsonObject *echo = json_object_new();

		ma_set_text(echo, "account", query->account);
		ma_set_text(echo, "login", query->login);
		ma_set_text(echo, "venue", query->venue);
		ma_set_text(echo, "instrument", query->instrument);
		ma_set_text(echo, "source", query->source);
		json_object_set_object_member(root, "query", echo);
	}

	json_object_set_boolean_member(root, "available", ma_series_ready(context, notes));
	json_object_set_array_member(root, "totals", json_array_new());
	json_object_set_array_member(root, "buckets", json_array_new());
	json_object_set_array_member(root, "top_items", json_array_new());
	json_object_set_array_member(root, "logins", json_array_new());
	json_object_set_null_member(root, "trend");

	{
		JsonObject *flips = json_object_new();

		json_object_set_array_member(flips, "rows", json_array_new());
		json_object_set_array_member(flips, "totals", json_array_new());
		json_object_set_int_member(flips, "count", 0);
		json_object_set_object_member(root, "flips", flips);
	}

#ifdef VENTURE_HAVE_SQLITE
	{
		g_autoptr(GDateTime) from = NULL;
		g_autoptr(GDateTime) to = NULL;
		g_autofree gchar *from_text = NULL;
		g_autofree gchar *to_text = NULL;

		from = (G_MININT64 != since) ? g_date_time_new_from_unix_utc(since) : NULL;
		to = (G_MININT64 != until) ? g_date_time_new_from_unix_utc(until) : NULL;
		from_text = (NULL != from) ? venture_time_to_string(from) : NULL;
		to_text = (NULL != to) ? venture_time_to_string(to) : NULL;
		ma_set_text(root, "since", from_text);
		ma_set_text(root, "until", to_text);
	}

	if (json_object_get_boolean_member(root, "available"))
	{
		g_autoptr(GPtrArray) sources = NULL;
		g_autoptr(VentureEntity) source = NULL;
		g_autoptr(VentureSeriesStore) store = NULL;
		g_autoptr(GPtrArray) buckets = NULL;
		g_autoptr(GPtrArray) items = NULL;
		g_autoptr(GPtrArray) flips = NULL;
		g_autoptr(GPtrArray) sums = NULL;
		g_autoptr(GPtrArray) trend = NULL;
		g_autoptr(GArray) shown = NULL;
		g_autoptr(GArray) flip_cost = NULL;
		g_autoptr(GArray) flip_proceeds = NULL;
		g_autoptr(GArray) flip_profit = NULL;
		g_autofree gchar *currency = NULL;
		g_autofree gchar *source_name = NULL;
		VentureSeriesTxnFilter filter;
		VentureSeriesTxnGroup group;
		VentureSeriesTxnGroup trend_group;
		JsonObject *object;
		JsonArray *array;
		gint64 source_id;
		gint64 matched = 0;

		sources = ma_sources(context, query->organization_id, error);

		if (NULL == sources)
			goto fail;

		json_object_set_array_member(root, "sources", ma_sources_json(sources));
		source = ma_pick_source(context, query->organization_id, query->data_source_id, sources, error);

		if ((NULL == source) && (query->data_source_id > 0))
			goto fail;

		if (NULL == source)
		{
			ma_note(notes, "No data sources yet.");
			goto done;
		}

		source_id = venture_entity_get_id(source);
		source_name = ma_source_name(source);
		currency = ma_source_currency(source);
		json_object_set_int_member(root, "data_source_id", source_id);
		json_object_set_string_member(root, "source_name", source_name);
		ma_set_text(root, "currency", currency);
		store = ma_reader(context, source, notes, NULL);

		if (NULL == store)
			goto done;

		if (!ma_login_choices(store, source_id, json_object_get_array_member(root, "logins"), error))
			goto fail;

		(void)venture_series_txn_group_from_string(group_name, &group);
		venture_series_txn_filter_init(&filter);
		filter.account_key = query->account;
		filter.login_key = query->login;
		filter.venue_key = query->venue;
		filter.instrument_key = query->instrument;
		filter.source = query->source;
		filter.since = (G_MININT64 != since) ? since : VENTURE_SERIES_NONE;
		filter.until = (G_MININT64 != until) ? until : VENTURE_SERIES_NONE;

		buckets = venture_series_store_txn_totals(store, &filter, group, error);

		if (NULL == buckets)
			goto fail;

		/* The totals: every grouping partitions the same rows, so the
		 * buckets summed per currency are the window's figures. */
		sums = g_ptr_array_new_with_free_func((GDestroyNotify)venture_series_txn_total_free);
		array = json_array_new();

		for (i = 0; i < buckets->len; i++)
		{
			VentureSeriesTxnTotal *bucket = g_ptr_array_index(buckets, i);
			VentureSeriesTxnTotal *sum = NULL;
			guint j;

			for (j = 0; j < sums->len; j++)
			{
				VentureSeriesTxnTotal *have = g_ptr_array_index(sums, j);

				if (0 == g_ascii_strcasecmp(have->currency, bucket->currency))
					sum = have;
			}

			if (NULL == sum)
			{
				sum = g_new0(VentureSeriesTxnTotal, 1);
				sum->period_start = VENTURE_SERIES_NONE;
				g_strlcpy(sum->currency, bucket->currency, sizeof(sum->currency));
				g_ptr_array_add(sums, sum);
			}

			if (!ma_bucket_add(sum, bucket, error))
			{
				json_array_unref(array);
				goto fail;
			}

			object = json_object_new();
			json_array_add_object_element(array, object);
			ma_set_text(object, "key", bucket->key);
			ma_set_text(object, "label", (NULL != bucket->label) ? bucket->label : bucket->key);

			/* The accounts reached through no login are one bucket, said
			 * as such: an empty label reads as a missing one. */
			if ((VENTURE_SERIES_TXN_GROUP_LOGIN == group) && venture_string_is_empty(bucket->key))
			{
				json_object_set_null_member(object, "key");
				json_object_set_string_member(object, "label", "No login");
			}

			ma_set_time(object, "period_start", bucket->period_start);
			ma_set_bucket(object, bucket);

			if ((VENTURE_SERIES_TXN_GROUP_ACCOUNT == group) && !venture_string_is_empty(bucket->key))
			{
				g_autofree gchar *url = venture_marketdata_account_path(source_id, bucket->key);

				json_object_set_string_member(object, "url", url);
			}
			else if ((VENTURE_SERIES_TXN_GROUP_INSTRUMENT == group) && !venture_string_is_empty(bucket->key))
			{
				g_autofree gchar *url = venture_marketdata_instrument_path(source_id, bucket->key, NULL);

				json_object_set_string_member(object, "url", url);
			}
			else if ((VENTURE_SERIES_TXN_GROUP_LOGIN == group) && !venture_string_is_empty(bucket->key))
			{
				g_autoptr(GString) url = g_string_new("/accounts/pnl?login=");

				g_string_append_uri_escaped(url, bucket->key, NULL, FALSE);
				g_string_append_printf(url, "&source=%" G_GINT64_FORMAT, source_id);
				json_object_set_string_member(object, "url", url->str);
			}
			else
				json_object_set_null_member(object, "url");
		}

		json_object_set_array_member(root, "buckets", array);
		array = json_array_new();

		for (i = 0; i < sums->len; i++)
		{
			object = json_object_new();
			ma_set_bucket(object, g_ptr_array_index(sums, i));
			json_array_add_object_element(array, object);
		}

		json_object_set_array_member(root, "totals", array);

		/* The chart: the grouping itself when it is a period, else the
		 * finest period that keeps the window under a chart's points. */
		if ((VENTURE_SERIES_TXN_GROUP_DAY == group) || (VENTURE_SERIES_TXN_GROUP_WEEK == group) ||
		    (VENTURE_SERIES_TXN_GROUP_MONTH == group))
			json_object_set_object_member(root, "trend", ma_trend_points(buckets, group, currency));
		else
		{
			gint64 span = ((G_MININT64 != since) && (G_MININT64 != until)) ? until - since : G_MAXINT64;

			trend_group = (span <= 62 * MA_DAY) ? VENTURE_SERIES_TXN_GROUP_DAY
			            : ((span <= 400 * MA_DAY) ? VENTURE_SERIES_TXN_GROUP_WEEK
			                                      : VENTURE_SERIES_TXN_GROUP_MONTH);
			trend = venture_series_store_txn_totals(store, &filter, trend_group, error);

			if (NULL == trend)
				goto fail;

			json_object_set_object_member(root, "trend", ma_trend_points(trend, trend_group, currency));
		}

		/* The best items: each instrument's cash profit in the window. */
		items = venture_series_store_txn_totals(store, &filter, VENTURE_SERIES_TXN_GROUP_INSTRUMENT, error);

		if (NULL == items)
			goto fail;

		g_ptr_array_sort(items, ma_compare_profit);
		array = json_array_new();

		for (i = 0; (i < items->len) && (json_array_get_length(array) < top); i++)
		{
			VentureSeriesTxnTotal *item = g_ptr_array_index(items, i);
			g_autofree gchar *url = NULL;

			if (venture_string_is_empty(item->key) || ((0 == item->sales) && (0 == item->buys)))
				continue;

			url = venture_marketdata_instrument_path(source_id, item->key, NULL);
			object = json_object_new();
			json_object_set_string_member(object, "instrument_key", item->key);
			ma_set_text(object, "instrument_name", item->label);
			ma_set_bucket(object, item);
			ma_set_money(object, "profit", item->net, item->currency);
			json_object_set_string_member(object, "url", url);
			json_array_add_object_element(array, object);
		}

		json_object_set_array_member(root, "top_items", array);

		/* Flips: buys matched to later sales, first in first out. */
		flips = venture_series_store_flips(store, &filter, error);

		if (NULL == flips)
			goto fail;

		flip_cost = ma_amounts_new();
		flip_proceeds = ma_amounts_new();
		flip_profit = ma_amounts_new();
		array = json_array_new();

		for (i = 0; i < flips->len; i++)
		{
			VentureSeriesFlip *flip = g_ptr_array_index(flips, i);
			g_autofree gchar *url = NULL;

			if (!ma_amounts_add(flip_cost, flip->currency, flip->cost, error) ||
			    !ma_amounts_add(flip_proceeds, flip->currency, flip->proceeds, error) ||
			    !ma_amounts_add(flip_profit, flip->currency, flip->profit, error) ||
			    !venture_series_math_add(matched, flip->matched_units, &matched))
			{
				json_array_unref(array);

				if ((NULL != error) && (NULL == *error))
					g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
					                    "The flips do not fit in a 64-bit integer");
				goto fail;
			}

			if (i >= top)
				continue;

			url = venture_marketdata_instrument_path(source_id, flip->instrument_key, NULL);
			object = json_object_new();
			json_object_set_string_member(object, "instrument_key", flip->instrument_key);
			ma_set_text(object, "instrument_name", flip->instrument_name);
			json_object_set_string_member(object, "currency", flip->currency);
			json_object_set_int_member(object, "bought_units", flip->bought_units);
			ma_set_money(object, "bought_amount", flip->bought_amount, flip->currency);
			json_object_set_int_member(object, "matched_units", flip->matched_units);
			ma_set_money(object, "cost", flip->cost, flip->currency);
			ma_set_money(object, "proceeds", flip->proceeds, flip->currency);
			ma_set_money(object, "profit", flip->profit, flip->currency);
			ma_set_money(object, "avg_buy",
			             (flip->matched_units > 0)
			             ? venture_series_math_div_round(flip->cost, flip->matched_units)
			             : VENTURE_SERIES_NONE, flip->currency);
			ma_set_money(object, "avg_sell",
			             (flip->matched_units > 0)
			             ? venture_series_math_div_round(flip->proceeds, flip->matched_units)
			             : VENTURE_SERIES_NONE, flip->currency);
			ma_set_ratio(object, "roi", (flip->cost > 0) ? (gdouble)flip->profit / (gdouble)flip->cost : NAN);
			json_object_set_int_member(object, "unmatched_sold_units", flip->unmatched_sold_units);
			json_object_set_int_member(object, "open_units", flip->open_units);
			ma_set_money(object, "open_cost", flip->open_cost, flip->currency);
			json_object_set_int_member(object, "held", flip->held);
			ma_set_time(object, "first_buy", flip->first_buy);
			ma_set_time(object, "last_sale", flip->last_sale);
			json_object_set_string_member(object, "url", url);
			json_array_add_object_element(array, object);
		}

		object = json_object_get_object_member(root, "flips");
		json_object_set_array_member(object, "rows", array);
		json_object_set_int_member(object, "count", flips->len);
		json_object_set_int_member(object, "matched_units", matched);
		array = json_array_new();

		for (i = 0; i < flip_profit->len; i++)
		{
			VentureSeriesAmount *profit = &g_array_index(flip_profit, VentureSeriesAmount, i);
			gint64 cost = ma_amounts_get(flip_cost, profit->currency);
			JsonObject *total = json_object_new();

			json_object_set_string_member(total, "currency", profit->currency);
			ma_set_money(total, "cost", cost, profit->currency);
			ma_set_money(total, "proceeds", ma_amounts_get(flip_proceeds, profit->currency),
			             profit->currency);
			ma_set_money(total, "profit", profit->amount, profit->currency);
			ma_set_ratio(total, "roi", (cost > 0) ? (gdouble)profit->amount / (gdouble)cost : NAN);
			json_array_add_object_element(array, total);
		}

		json_object_set_array_member(object, "totals", array);

		shown = g_array_new(FALSE, FALSE, sizeof(gint64));
		g_array_append_val(shown, source_id);
		ma_attribute(context, query->organization_id, root, shown);

		return ma_node(root);

	fail:
		json_object_unref(root);
		return NULL;

	done:
		;
	}
#else
	(void)since;
	(void)until;
	(void)top;
#endif

	if (!json_object_has_member(root, "sources"))
		json_object_set_array_member(root, "sources", json_array_new());

	ma_attribute(context, query->organization_id, root, NULL);

	return ma_node(root);
}
