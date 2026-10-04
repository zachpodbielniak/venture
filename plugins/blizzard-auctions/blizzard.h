/*
 * blizzard.h - What the Blizzard auction house plugin's files share
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Private to plugins/blizzard-auctions/: nothing here is VENTURE's API.
 * The plugin is three files that each read like a chapter --
 *
 *   blizzard-plugin.c    registration, the `wow_auction` fee model and the
 *                        `tsm` export format
 *   blizzard-provider.c  the `blizzard_auctions` data source provider: the
 *                        settings, the OAuth token, the auction mapping
 *   blizzard-recipes.c   the `import_recipes` action on a data source
 *
 * -- and this header is what they hand each other: the parsed settings,
 * the frozen per-source state (token and name caches), and the HTTP and
 * JSON helpers both the worker-side fetch and the main-thread import use.
 */

#ifndef BLIZZARD_H
#define BLIZZARD_H

#include <venture/venture.h>

G_BEGIN_DECLS

/* The registry names, as a data source, a venue and an export name them. */
#define BLIZZARD_PROVIDER_NAME		"blizzard_auctions"
#define BLIZZARD_FEE_MODEL_NAME		"wow_auction"
#define BLIZZARD_EXPORT_NAME		"tsm"
#define BLIZZARD_ACTION_NAME		"import_recipes"

/* The line shown wherever this provider's data is: Blizzard's API terms
 * (section 2.13) want Blizzard named as the source, without the page
 * looking endorsed by or affiliated with Blizzard. */
#define BLIZZARD_ATTRIBUTION \
	"Auction house data provided by Blizzard Entertainment via the Battle.net API. " \
	"Not affiliated with or endorsed by Blizzard Entertainment."

/* The namespace instrument keys live in, and the default currency. */
#define BLIZZARD_INSTRUMENT_NAMESPACE	"wow-item"
#define BLIZZARD_DEFAULT_CURRENCY	"GOLD"

/*
 * Blizzard prices are in copper: 1 gold is 100 silver is 10000 copper.
 * The currency the provider emits must therefore have exponent 4, so a
 * price of 12345 copper reads as 1.2345 GOLD; the provider refuses to run
 * against one registered otherwise.
 */
#define BLIZZARD_CURRENCY_EXPONENT	(4)

/* The two units that are not a connected realm's id. */
#define BLIZZARD_UNIT_COMMODITIES	"commodities"
#define BLIZZARD_UNIT_ALL_REALMS	"realms"

/*
 * What one request costs against Blizzard's budget (36,000 an hour per
 * client). The commodities endpoint costs 25 -- and Blizzard charges it
 * even when the answer is 304 Not Modified, which is why the cost is
 * spent before the request goes out, never refunded after.
 */
#define BLIZZARD_COST_DEFAULT		(1)
#define BLIZZARD_COST_COMMODITIES	(25)

/*
 * Blizzard's other limit: 100 requests a second. Requests of one source go
 * one at a time already; this spacing keeps a burst of name lookups
 * against a local mirror (or a fast network) under it too.
 */
#define BLIZZARD_MIN_SPACING_US		(10000)

/* Bounds a setting may not pass. */
#define BLIZZARD_MAX_REALMS		(1000)
#define BLIZZARD_MAX_NAMES_PER_FETCH	(1000)
#define BLIZZARD_MAX_REALMS_PER_FETCH	(200)

/**
 * BlizzardSettings:
 * @region: us, eu, kr or tw
 * @client_id: (nullable): the Battle.net client's id; not a secret (the
 *   secret is sealed, and reaches a fetch as a credential)
 * @locale: the locale names are asked in, e.g. en_US
 * @currency: the currency prices are emitted in: the setting, else GOLD
 * @api_base: the API origin, no trailing slash
 * @oauth_base: the token endpoint's origin, no trailing slash
 * @dynamic_namespace: e.g. dynamic-us: auctions, realms
 * @static_namespace: e.g. static-us: items, professions, recipes
 * @realm_ids: (element-type gint64): the connected realms; empty for every
 *   one the index lists
 * @include_commodities: whether the region's commodity market is a unit
 * @include_bid_only: whether an auction with a bid and no buyout is listed
 *   at its bid
 * @item_names_per_fetch: item lookups one fetch may make; 0 for none
 * @realms_per_fetch: in the every-realm unit, realms with new data one
 *   fetch reads before it stops
 * @key_modifier_types: (element-type gint64) (nullable): the modifier
 *   types that make an item a variant; %NULL for every one
 *
 * A source's settings, read and judged once.
 */
typedef struct
{
	gchar		*region;
	gchar		*client_id;
	gchar		*locale;
	gchar		*currency;
	gchar		*api_base;
	gchar		*oauth_base;
	gchar		*dynamic_namespace;
	gchar		*static_namespace;
	GArray		*realm_ids;
	gboolean	 include_commodities;
	gboolean	 include_bid_only;
	guint		 item_names_per_fetch;
	guint		 realms_per_fetch;
	GArray		*key_modifier_types;
} BlizzardSettings;

/**
 * blizzard_settings_parse:
 * @settings: a data source's settings
 * @error: (out) (optional): return location for a #GError
 *
 * Reads and judges the settings. Pure: it runs when a source is saved
 * (through list_units) and when it is frozen.
 *
 * Returns: (transfer full) (nullable): the settings
 */
BlizzardSettings *
blizzard_settings_parse(
	JsonObject	 *settings,
	GError		**error
);

/**
 * blizzard_settings_free:
 * @settings: (transfer full) (nullable): settings
 */
void
blizzard_settings_free(BlizzardSettings *settings);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(BlizzardSettings, blizzard_settings_free)

/**
 * BlizzardItem:
 * @name: (nullable): the item's name; %NULL when Blizzard does not know it
 * @category: (nullable): "Item class/Subclass"
 * @vendor_sell: what a vendor pays for one, in copper; -1 when unknown
 * @quality: (nullable): POOR, COMMON, ... as Blizzard spells it
 * @level: the item level, or 0
 *
 * What the static item endpoint said about one item id.
 */
typedef struct
{
	gchar	*name;
	gchar	*category;
	gint64	 vendor_sell;
	gchar	*quality;
	gint64	 level;
} BlizzardItem;

/**
 * BlizzardFrozen:
 * @settings: the source's settings
 * @lock: guards everything below
 * @token: (nullable): the OAuth access token; never logged, never stored
 * @token_expires: when it stops working, Unix seconds
 * @last_request_us: monotonic time of the last request, for spacing
 * @realm_names: connected realm id (text) -> name
 * @items: item id (gint64 key) -> #BlizzardItem; an entry with a %NULL
 *   name is "asked, and Blizzard did not know"
 *
 * What a source's fetches share: made on the main thread when the source
 * is frozen, and read by the fetches on a GTask thread. The settings are
 * immutable; the caches are not, and are only touched under @lock. They
 * live as long as the frozen source -- until the source is saved or the
 * configuration changes -- which is the token's natural lifetime too
 * (Blizzard's last a day).
 */
typedef struct
{
	BlizzardSettings	*settings;
	GMutex			 lock;
	gchar			*token;
	gint64			 token_expires;
	gint64			 last_request_us;
	GHashTable		*realm_names;
	GHashTable		*items;
} BlizzardFrozen;

/**
 * blizzard_frozen_new:
 * @settings: (transfer full): the parsed settings
 *
 * Returns: (transfer full): frozen state with empty caches
 */
BlizzardFrozen *
blizzard_frozen_new(BlizzardSettings *settings);

/**
 * blizzard_frozen_free:
 * @data: (transfer full) (nullable): a #BlizzardFrozen
 *
 * Frees it, wiping the token from memory first.
 */
void
blizzard_frozen_free(gpointer data);

#ifdef VENTURE_HAVE_SQLITE

/**
 * blizzard_get_json:
 * @request: the request the fetch was given
 * @frozen: the source's frozen state
 * @url: the address
 * @cost: what it costs against the budget
 * @flags: %VENTURE_FEED_HTTP_UNCONDITIONAL for a side request; the helper
 *   adds %VENTURE_FEED_HTTP_ANY_STATUS itself
 * @extra_headers: (nullable) (array zero-terminated=1): more name, value
 *   pairs (an If-Modified-Since of the provider's own)
 * @out_status: (out): the HTTP status
 * @out_last_modified: (out) (optional): the answer's Last-Modified, Unix
 *   seconds, or %VENTURE_SERIES_NONE
 * @error: (out) (optional): return location for a #GError
 *
 * GETs a Blizzard API address with the bearer token, getting one first
 * when the cache has none, and once more when Blizzard answers 401 to a
 * cached one (it was revoked, or the clock lied). Blocks: a GTask thread
 * or a main-thread action only.
 *
 * Returns: (transfer full) (nullable): the parsed JSON for a 200; %NULL
 *   with no error for any other status the caller judges by
 *   @out_status (304, 404...); %NULL with an error for a transport
 *   failure, a 429, unusable credentials or a body that is not JSON
 */
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
);

#endif /* VENTURE_HAVE_SQLITE */

/**
 * blizzard_text:
 * @object: a JSON object
 * @member: a member that is a string, or a localised object
 *   {"en_US": "..."} when no locale was asked for
 * @locale: (nullable): the locale to pick from an object
 *
 * Returns: (transfer none) (nullable): the text
 */
const gchar *
blizzard_text(
	JsonObject	*object,
	const gchar	*member,
	const gchar	*locale
);

/**
 * blizzard_int:
 * @object: (nullable): a JSON object
 * @member: a member
 * @fallback: what to answer when it is missing or not an integer
 *
 * Returns: the member as an integer
 */
gint64
blizzard_int(
	JsonObject	*object,
	const gchar	*member,
	gint64		 fallback
);

#ifdef VENTURE_HAVE_SQLITE

/**
 * blizzard_fetch_item:
 * @request: the request
 * @frozen: the frozen state, whose item cache is read and filled
 * @item_id: the item
 * @out_item: (out) (transfer full) (nullable): a copy of what is known
 * @error: (out) (optional): return location for a #GError
 *
 * The item's static details, from the cache or from
 * /data/wow/item/{id} (a side request, cost 1). An item Blizzard does not
 * know (404) is cached as nameless and is not an error.
 *
 * Returns: %TRUE unless the request itself failed
 */
gboolean
blizzard_fetch_item(
	VentureFeedRequest	 *request,
	BlizzardFrozen		 *frozen,
	gint64			  item_id,
	BlizzardItem		**out_item,
	GError			**error
);

#endif /* VENTURE_HAVE_SQLITE */

/**
 * blizzard_item_free:
 * @item: (transfer full) (nullable): an item
 */
void
blizzard_item_free(BlizzardItem *item);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(BlizzardItem, blizzard_item_free)

/**
 * blizzard_item_key_parse:
 * @key: an instrument key the provider made
 * @out_item_id: (out): the item id it starts with
 * @out_pet_species: (out) (optional): the pet species, or 0
 *
 * Reads back the start of a key in the format blizzard_item_key()
 * documents (blizzard-provider.c): the item id, and the pet species when
 * there is one. Anything else -- an odds feed's outcome, a SKU -- is not
 * a Blizzard key.
 *
 * Returns: whether @key is one
 */
gboolean
blizzard_item_key_parse(
	const gchar	*key,
	gint64		*out_item_id,
	gint64		*out_pet_species
);

#ifdef VENTURE_HAVE_SQLITE

/**
 * blizzard_provider_new:
 *
 * Returns: (transfer full): the `blizzard_auctions` provider
 */
VentureDataSourceProvider *
blizzard_provider_new(void);

/**
 * blizzard_recipes_register:
 * @context: the context the plugin is loading into
 * @error: (out) (optional): return location for a #GError
 *
 * Registers the `import_recipes` action on data sources, once per
 * database, and points it at @context (the last context over a database
 * wins, as every per-database registration here does).
 *
 * Returns: %TRUE on success
 */
gboolean
blizzard_recipes_register(
	VentureContext	 *context,
	GError		**error
);

#endif /* VENTURE_HAVE_SQLITE */

G_END_DECLS

#endif /* BLIZZARD_H */
