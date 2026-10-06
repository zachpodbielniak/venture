/*
 * blizzard-plugin.c - The Blizzard auction house plugin: registration, the
 *                     `wow_auction` fee model and the `tsm` export
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A native plugin, and the reference example of one that reaches several
 * of VENTURE's extension points at once:
 *
 *   - a data source provider, `blizzard_auctions` (blizzard-provider.c),
 *     so a data_source record can sync World of Warcraft auction houses
 *     into its series store;
 *   - a fee model, `wow_auction` (below), so a venue can say what the
 *     auction house charges and every arbitrage scan prices it in;
 *   - an export format, `tsm` (below), so the rows a scan found can be
 *     pasted into TradeSkillMaster as a group;
 *   - a record action, `import_recipes` on data_source
 *     (blizzard-recipes.c), so the crafting professions become recipes
 *     the production module crafts and the `transform` strategy prices.
 *
 * Core stays game-agnostic: nothing outside this directory knows what a
 * realm, a bonus id or copper is.
 *
 * It is built into $(OUTDIR)/plugins-optional/, not $(OUTDIR)/plugins/:
 * the test suite loads everything in the latter into every fixture, and a
 * provider that wants a registered GOLD currency and credentials has no
 * business in a fixture that did not ask for it. An operator enables it by
 * adding that directory (installed as $(LIBDIR)/venture/plugins-optional)
 * to plugins.paths. docs/examples/wow-auction-house-feed.org walks through
 * the setup.
 */

#include "blizzard.h"

#include <string.h>
#include <glib/gstdio.h>

/* ==========================================================================
 * The `wow_auction` fee model
 *
 * What a World of Warcraft auction house keeps when something sells, and
 * what listing it costs:
 *
 *   - the cut: cut_percent (5 by default) of what the auction sold for,
 *     taken on sale;
 *   - the deposit: a share of the item's *vendor* price, per unit, by
 *     duration, paid up front and refunded when the auction sells (lost
 *     when it expires -- which is what an arbitrage scan's listing loss
 *     counts against the sale rate). The shares are the classic formula,
 *     15% of the vendor price per 12 hours: 15% for 12 h, 30% for 24 h,
 *     60% for 48 h (duration_hours, 48 by default). Retail has charged
 *     commodities differently since Dragonflight; deposit_factor_percent
 *     replaces the formula with one percent of the vendor price when the
 *     classic shares are not the ones a realm charges.
 *
 * The vendor price is the instrument's `vendor_sell` attribute, in copper,
 * which the provider stores once it has fetched the item's details; an
 * item whose details it has not fetched yet has no deposit in the quote
 * rather than a guessed one. Prices are copper, so the sale must be in a
 * currency with exponent 4 (GOLD, registered by the operator): anything
 * else is refused rather than scaled by a power of ten nobody checked.
 * Buying is free.
 * ========================================================================== */

static const gchar *const wow_fee_keys[] = {
	"cut_percent", "duration_hours", "deposit_factor_percent", NULL
};

/* A percent parameter as parts per million, from a YAML number or text. */
static gboolean
wow_fee_percent(
	JsonObject	 *params,
	const gchar	 *member,
	gint64		  fallback,
	gint64		 *out_ppm,
	GError		**error
){
	g_autofree gchar *text = NULL;
	JsonNode *node;

	*out_ppm = fallback;
	node = (NULL != params) ? json_object_get_member(params, member) : NULL;

	if ((NULL == node) || JSON_NODE_HOLDS_NULL(node))
		return TRUE;

	if (JSON_NODE_HOLDS_VALUE(node) && (G_TYPE_INT64 == json_node_get_value_type(node)))
		text = g_strdup_printf("%" G_GINT64_FORMAT, json_node_get_int(node));
	else if (JSON_NODE_HOLDS_VALUE(node) && (G_TYPE_STRING == json_node_get_value_type(node)))
		text = g_strdup(json_node_get_string(node));
	else if (JSON_NODE_HOLDS_VALUE(node) && (G_TYPE_DOUBLE == json_node_get_value_type(node)))
	{
		gchar buffer[G_ASCII_DTOSTR_BUF_SIZE];

		/* The shortest spelling that reads back as the same double,
		 * so 2.5 is "2.5" and a parser held to four places can judge
		 * what was written rather than a binary approximation. */
		text = g_strdup(g_ascii_dtostr(buffer, sizeof(buffer), json_node_get_double(node)));
	}

	if ((NULL == text) || !venture_arbitrage_parse_percent(text, FALSE, out_ppm, NULL))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "%s is a percent, e.g. 5 or 2.5", member);
		return FALSE;
	}

	return TRUE;
}

/* The classic deposit share for a duration, in parts per million. */
static gint64
wow_fee_classic_share(gint64 hours)
{
	return (12 == hours) ? 150000 : (24 == hours) ? 300000 : 600000;
}

/* Reads every parameter, judging each; what compute and validate share. */
static gboolean
wow_fee_read(
	JsonObject	 *params,
	gint64		 *out_cut,
	gint64		 *out_share,
	GError		**error
){
	g_autoptr(GList) members = NULL;
	GList *member;
	gint64 hours;

	members = (NULL != params) ? json_object_get_members(params) : NULL;

	for (member = members; NULL != member; member = member->next)
	{
		if (!g_strv_contains(wow_fee_keys, member->data))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			            "The wow_auction fee model takes cut_percent, duration_hours and "
			            "deposit_factor_percent; %s is not one of them", (const gchar *)member->data);
			return FALSE;
		}
	}

	if (!wow_fee_percent(params, "cut_percent", 50000, out_cut, error))
		return FALSE;

	if (*out_cut > 1000000)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "cut_percent is at most 100");
		return FALSE;
	}

	hours = 48;

	if ((NULL != params) && json_object_has_member(params, "duration_hours"))
	{
		JsonNode *node = json_object_get_member(params, "duration_hours");

		hours = (JSON_NODE_HOLDS_VALUE(node) && (G_TYPE_INT64 == json_node_get_value_type(node)))
			? json_node_get_int(node) : -1;

		if ((12 != hours) && (24 != hours) && (48 != hours))
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			                    "duration_hours is 12, 24 or 48");
			return FALSE;
		}
	}

	if (!wow_fee_percent(params, "deposit_factor_percent", wow_fee_classic_share(hours), out_share, error))
		return FALSE;

	if (*out_share > 10000000)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "deposit_factor_percent is at most 1000");
		return FALSE;
	}

	return TRUE;
}

static gboolean
wow_fee_validate(
	JsonObject	 *params,
	gpointer	  user_data,
	GError		**error
){
	gint64 cut;
	gint64 share;

	(void)user_data;

	return wow_fee_read(params, &cut, &share, error);
}

static gboolean
wow_fee_compute(
	JsonObject		 *params,
	VentureFeeSide		  side,
	const VentureMoney	 *amount,
	gint64			  units,
	const VentureMoney	 *reference,
	JsonObject		 *attrs,
	VentureFeeQuote		 *out,
	gpointer		  user_data,
	GError			**error
){
	g_autoptr(VentureMoney) vendor = NULL;
	g_autoptr(VentureMoney) all_units = NULL;
	JsonNode *node;
	gint64 cut;
	gint64 share;
	gint64 vendor_sell;

	(void)reference;
	(void)user_data;

	if (!wow_fee_read(params, &cut, &share, error))
		return FALSE;

	/* The auction house charges the seller; buying out costs the price. */
	if (VENTURE_FEE_SIDE_BUY == side)
	{
		out->fee = venture_money_new(0, venture_money_get_currency(amount),
		                             venture_money_get_exponent(amount));
		return TRUE;
	}

	if (BLIZZARD_CURRENCY_EXPONENT != venture_money_get_exponent(amount))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "wow_auction prices copper: the sale must be in a currency with exponent 4, "
		            "and %s has %u", venture_money_get_currency(amount),
		            (guint)venture_money_get_exponent(amount));
		return FALSE;
	}

	out->fee = venture_money_multiply_rational(amount, cut, 1000000, error);

	if (NULL == out->fee)
		return FALSE;

	/* The deposit, on the vendor price, when the source has fetched it. */
	node = (NULL != attrs) ? json_object_get_member(attrs, "vendor_sell") : NULL;
	vendor_sell = ((NULL != node) && JSON_NODE_HOLDS_VALUE(node) &&
	               (G_TYPE_INT64 == json_node_get_value_type(node)))
		? json_node_get_int(node) : -1;

	if (vendor_sell < 0)
		return TRUE;

	vendor = venture_money_new(vendor_sell, venture_money_get_currency(amount),
	                           venture_money_get_exponent(amount));
	all_units = venture_money_multiply_int(vendor, MAX(units, (gint64)1), error);

	if (NULL == all_units)
		return FALSE;

	out->deposit = venture_money_multiply_rational(all_units, share, 1000000, error);
	out->deposit_refundable = TRUE;

	return NULL != out->deposit;
}

/* ==========================================================================
 * The `tsm` export format
 *
 * TradeSkillMaster imports a group from a comma-separated list of its item
 * strings. This writes one line:
 *
 *     i:2770,i:2771,p:1234
 *
 *   - i:<item id> for every item a row names: the row's instrument, and a
 *     transform row's inputs (what the shopping list buys);
 *   - p:<species> for a caged battle pet;
 *   - each once, in the order the rows first name it;
 *   - a variant collapses to its item: TSM groups by item, and how it
 *     spells bonus ids has changed between its versions.
 *
 * A row whose instrument is not a Blizzard key (an odds feed's outcome, a
 * supplier's SKU) adds nothing. No WoW item at all is an empty line, not
 * an error: an empty scan exports an empty group.
 * ========================================================================== */

/* The key format is documented with blizzard_item_key() in
 * blizzard-provider.c; this reads its start back, and lives here because
 * the export needs it in a build with no SQLite (and so no provider). */
gboolean
blizzard_item_key_parse(
	const gchar	*key,
	gint64		*out_item_id,
	gint64		*out_pet_species
){
	const gchar *pet;
	gchar *end;
	gint64 item_id;

	if ((NULL == key) || !g_ascii_isdigit(key[0]))
		return FALSE;

	item_id = g_ascii_strtoll(key, &end, 10);

	if ((item_id <= 0) || (('\0' != *end) && (':' != *end)))
		return FALSE;

	*out_item_id = item_id;

	if (NULL != out_pet_species)
	{
		pet = strstr(key, ":p");
		*out_pet_species = (NULL != pet) ? g_ascii_strtoll(pet + 2, NULL, 10) : 0;
	}

	return TRUE;
}

static void
tsm_add(
	GString		*out,
	GHashTable	*written,
	const gchar	*key
){
	g_autofree gchar *token = NULL;
	gint64 item_id;
	gint64 species;

	if (!blizzard_item_key_parse(key, &item_id, &species))
		return;

	token = (species > 0) ? g_strdup_printf("p:%" G_GINT64_FORMAT, species)
	                      : g_strdup_printf("i:%" G_GINT64_FORMAT, item_id);

	if (g_hash_table_contains(written, token))
		return;

	if (out->len > 0)
		g_string_append_c(out, ',');

	g_string_append(out, token);
	g_hash_table_add(written, g_steal_pointer(&token));
}

static GBytes *
tsm_export(
	JsonArray	 *opportunities,
	JsonObject	 *options,
	gpointer	  user_data,
	GError		**error
){
	g_autoptr(GHashTable) written = NULL;
	g_autoptr(GString) out = NULL;
	guint i;
	guint j;

	(void)options;
	(void)user_data;
	(void)error;

	written = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	out = g_string_new(NULL);

	for (i = 0; (NULL != opportunities) && (i < json_array_get_length(opportunities)); i++)
	{
		JsonNode *element = json_array_get_element(opportunities, i);
		JsonObject *row;
		JsonArray *inputs;

		if (!JSON_NODE_HOLDS_OBJECT(element))
			continue;

		row = json_node_get_object(element);
		tsm_add(out, written, venture_json_object_get_string(row, "instrument_key", NULL));

		inputs = (json_object_has_member(row, "inputs") &&
		          JSON_NODE_HOLDS_ARRAY(json_object_get_member(row, "inputs")))
			? json_object_get_array_member(row, "inputs") : NULL;

		for (j = 0; (NULL != inputs) && (j < json_array_get_length(inputs)); j++)
		{
			JsonNode *input = json_array_get_element(inputs, j);

			if (JSON_NODE_HOLDS_OBJECT(input))
				tsm_add(out, written, venture_json_object_get_string(json_node_get_object(input),
				                                                     "instrument_key", NULL));
		}
	}

	g_string_append_c(out, '\n');

	return g_string_free_to_bytes(g_steal_pointer(&out));
}

/* ==========================================================================
 * Entry points
 * ========================================================================== */

/**
 * venture_plugin_info:
 *
 * Shown in `venturectl`'s plugin list and at /api/v1/plugins.
 *
 * Returns: (transfer none): a one-line description
 */
const gchar *
venture_plugin_info(void);

const gchar *
venture_plugin_info(void)
{
	return "World of Warcraft auction houses from Battle.net: the blizzard_auctions data source, "
	       "the wow_auction fee model, the tsm export and recipe import";
}

/* ==========================================================================
 * Icons
 * ========================================================================== */

/*
 * GET /blizzard/icons/<file id>.jpg: an icon from the plugin's cache.
 *
 * Only a file id and ".jpg" are accepted, so the name can reach no other
 * file. An icon never changes for its id, so the browser may keep it a
 * year without asking again ("immutable"); a page of a hundred items
 * costs no request for any icon it has drawn before. Signed-in users
 * only, like the pages that show them.
 */
static HtmxResponse *
blizzard_icon_route(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *server = user_data;
	g_autofree gchar *path = NULL;
	g_autofree gchar *contents = NULL;
	const gchar *file;
	const gchar *p;
	HtmxResponse *response;
	gsize length = 0;

	response = venture_web_server_require_page(server, request, VENTURE_USER_ROLE_VIEWER);

	if (NULL != response)
		return response;

	file = (NULL != params) ? g_hash_table_lookup(params, "file") : NULL;

	for (p = file; (NULL != p) && g_ascii_isdigit(*p); p++)
		;

	if ((NULL == file) || (p == file) || (0 != g_strcmp0(p, ".jpg")) || (strlen(file) > 24) ||
	    (NULL == blizzard_get_icon_dir()))
		return htmx_response_not_found();

	path = g_build_filename(blizzard_get_icon_dir(), file, NULL);

	if (!g_file_get_contents(path, &contents, &length, NULL))
		return htmx_response_not_found();

	response = htmx_response_new();
	htmx_response_set_bytes(response, g_bytes_new_take(g_steal_pointer(&contents), length));
	htmx_response_set_content_type(response, "image/jpeg");
	htmx_response_add_header(response, "Cache-Control", "private, max-age=31536000, immutable");

	return response;
}

/* ==========================================================================
 * Class colours
 *
 * The game's own colour for each class, as its UI draws a character's name
 * (RAID_CLASS_COLORS). tsmctl pushes a character's class as the game
 * spells it ("DEATHKNIGHT"); the Accounts pages draw it in its colour.
 * ========================================================================== */

static gboolean
blizzard_register_class_colors(
	VentureContext	 *context,
	GError		**error
){
	static const struct { const gchar *name; const gchar *color; } classes[] = {
		{ "DEATHKNIGHT", "#c41e3a" }, { "DEMONHUNTER", "#a330c9" }, { "DRUID", "#ff7c0a" },
		{ "EVOKER", "#33937f" }, { "HUNTER", "#aad372" }, { "MAGE", "#3fc7eb" },
		{ "MONK", "#00ff98" }, { "PALADIN", "#f48cba" }, { "PRIEST", "#ffffff" },
		{ "ROGUE", "#fff468" }, { "SHAMAN", "#0070dd" }, { "WARLOCK", "#8788ee" },
		{ "WARRIOR", "#c69b6d" },
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(classes); i++)
		if (!venture_context_set_account_class_color(context, classes[i].name, classes[i].color, error))
			return FALSE;

	return TRUE;
}

static gboolean
blizzard_web(
	VentureWebServer	 *server,
	gpointer		  user_data,
	GError			**error
){
	(void)user_data;
	(void)error;

	venture_web_server_add_classified_route(server, HTMX_METHOD_GET,
		BLIZZARD_ICON_ROUTE ":file", VENTURE_DATA_CLASS_REFERENCE,
		VENTURE_HOSTED_ROUTE_NONE, blizzard_icon_route, server);

	return TRUE;
}

/**
 * venture_plugin_register:
 * @context: the wiring, giving access to every registry
 * @error: (out) (optional): return location for a #GError
 *
 * Called once per context the plugin is loaded into. Each registry is the
 * context's, so a name already held -- the plugin loaded twice into one
 * context -- is refused rather than replaced.
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
	/*
	 * The fee model first: a venue naming wow_auction is refused at its
	 * save until the model is registered, so it has to be in place
	 * before anything loads venues.
	 */
	if (!venture_fee_model_registry_add(venture_context_get_fee_models(context),
	                                    BLIZZARD_FEE_MODEL_NAME,
	                                    "World of Warcraft auction house: a 5% cut on sale and a "
	                                    "refundable deposit on the vendor price by duration",
	                                    wow_fee_compute, wow_fee_validate, NULL, NULL, error))
		return FALSE;

	if (!blizzard_register_class_colors(context, error))
		return FALSE;

	if (!venture_export_format_registry_add(venture_context_get_export_formats(context),
	                                        BLIZZARD_EXPORT_NAME, "TSM group import", "text/plain",
	                                        "txt", tsm_export, NULL, NULL, error))
		return FALSE;

#ifdef VENTURE_HAVE_SQLITE
	/* Icons are fetched once into the plugin's own cache and served from
	 * it; without a cache directory the items simply have none. */
	{
		g_autoptr(GError) cache_error = NULL;
		g_autofree gchar *dir = venture_context_get_plugin_cache_dir(context, "blizzard-auctions",
		                                                             &cache_error);
		g_autofree gchar *icons = (NULL != dir) ? g_build_filename(dir, "icons", NULL) : NULL;

		if ((NULL != icons) && (0 == g_mkdir_with_parents(icons, 0700)))
			blizzard_set_icon_dir(icons);
		else
			g_message("blizzard-auctions: no icon cache (%s); items are drawn without icons",
			          (NULL != cache_error) ? cache_error->message : "cannot create it");
	}

	{
		g_autoptr(VentureDataSourceProvider) provider = blizzard_provider_new();

		if (!venture_data_source_provider_registry_add(venture_context_get_data_source_providers(context),
		                                               provider, error))
			return FALSE;
	}

	/* Last: a record action is not taken back when a later step of the
	 * load fails (docs/plugins.org, "When a load fails"); everything
	 * above is. */
	if (!blizzard_recipes_register(context, error))
		return FALSE;

	venture_context_add_web_extension(context, blizzard_web, NULL, NULL);
#else
	g_message("blizzard-auctions: this build has no SQLite, so no series store: the "
	          "blizzard_auctions data source and recipe import are not available");
#endif

	return TRUE;
}
