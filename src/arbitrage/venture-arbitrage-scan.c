/*
 * venture-arbitrage-scan.c - The scan: one question, one strategy, the
 * filters, and the one path from an opportunity to a trade
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A scan reads a question (venture_arbitrage_scan_options_normalise()),
 * opens the organization's stores, and hands the strategy a
 * VentureArbitrageScan to add candidates to. The strategy finds; the scan
 * judges: the filters below apply to every strategy's candidates the same
 * way, so a plugin's strategy is filtered, sorted and cut exactly like a
 * built-in one.
 *
 * Three rules hold everywhere in this file:
 *
 *  - nothing adds across currencies. A sell side in another currency is
 *    converted into the buy side's through the organization's
 *    exchange_rate table on the scan's date, or the candidate is skipped
 *    with a note; a filter amount in another currency is converted the
 *    same way;
 *  - nothing unquoted is read as zero. A candidate missing a price or a
 *    fee is kept with its figures blank and the missing thing named;
 *  - an opportunity reaches the books only through its strategy's plan
 *    and the `record` action (venture_arbitrage_record_opportunity()).
 *
 * Everything runs on the main thread against read handles, and every
 * read is bounded; see VENTURE_ARBITRAGE_SCAN_CANDIDATES.
 */

#include "venture.h"
#include "arbitrage/venture-arbitrage-engine-private.h"

#include <math.h>
#include <string.h>

#define ARB_STRATEGIES_CONTEXT_KEY "venture-arbitrage-strategies"
#define ARB_EXPORTS_CONTEXT_KEY "venture-arbitrage-export-formats"
#define ARB_ENGINE_INSTALLED_KEY "venture-arbitrage-engine-installed"

/* ==========================================================================
 * The strategy registry
 * ========================================================================== */

typedef struct
{
	gchar				*name;
	gchar				*label;
	gchar				*description;
	gchar				**options;
	VentureArbitrageScanFunc	 scan;
	VentureArbitragePlanFunc	 plan;
	gpointer			 user_data;
	GDestroyNotify			 destroy;
} ArbStrategy;

struct _VentureArbitrageStrategyRegistry
{
	GObject		 parent_instance;

	GPtrArray	*entries;	/* ArbStrategy, built-ins first */
};

G_DEFINE_FINAL_TYPE(VentureArbitrageStrategyRegistry, venture_arbitrage_strategy_registry,
                    G_TYPE_OBJECT)

static void
arb_strategy_free(gpointer data)
{
	ArbStrategy *entry;

	entry = data;

	if (NULL != entry->destroy)
		entry->destroy(entry->user_data);

	g_free(entry->name);
	g_free(entry->label);
	g_free(entry->description);
	g_strfreev(entry->options);
	g_free(entry);
}

static void
venture_arbitrage_strategy_registry_finalize(GObject *object)
{
	g_ptr_array_unref(VENTURE_ARBITRAGE_STRATEGY_REGISTRY(object)->entries);

	G_OBJECT_CLASS(venture_arbitrage_strategy_registry_parent_class)->finalize(object);
}

static void
venture_arbitrage_strategy_registry_class_init(VentureArbitrageStrategyRegistryClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = venture_arbitrage_strategy_registry_finalize;
}

static void
venture_arbitrage_strategy_registry_init(VentureArbitrageStrategyRegistry *self)
{
	self->entries = g_ptr_array_new_with_free_func(arb_strategy_free);
}

static ArbStrategy *
arb_strategy_lookup(
	VentureArbitrageStrategyRegistry	*self,
	const gchar				*name
){
	guint i;

	for (i = 0; (NULL != self) && (NULL != name) && (i < self->entries->len); i++)
	{
		ArbStrategy *entry = g_ptr_array_index(self->entries, i);

		if (0 == g_strcmp0(entry->name, name))
			return entry;
	}

	return NULL;
}

VentureArbitrageStrategyRegistry *
venture_arbitrage_strategy_registry_new(void)
{
	VentureArbitrageStrategyRegistry *self;

	self = g_object_new(VENTURE_TYPE_ARBITRAGE_STRATEGY_REGISTRY, NULL);
	venture_arbitrage_register_builtin_strategies(self);

	return self;
}

gboolean
venture_arbitrage_strategy_registry_add(
	VentureArbitrageStrategyRegistry	 *self,
	const gchar				 *name,
	const gchar				 *label,
	const gchar				 *description,
	const gchar *const			 *options,
	VentureArbitrageScanFunc		  scan,
	VentureArbitragePlanFunc		  plan,
	gpointer				  user_data,
	GDestroyNotify				  destroy,
	GError					**error
){
	ArbStrategy *entry;
	guint i;

	g_return_val_if_fail(VENTURE_IS_ARBITRAGE_STRATEGY_REGISTRY(self), FALSE);
	g_return_val_if_fail(NULL != scan, FALSE);

	if (!venture_arbitrage_name_check("strategy", name, error))
		return FALSE;

	if (NULL != arb_strategy_lookup(self, name))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_ALREADY_EXISTS,
		            "A strategy named %s is already registered", name);
		return FALSE;
	}

	/* A strategy's own option may not shadow a common one: the common
	 * reader would judge it first and the strategy never see it. */
	for (i = 0; (NULL != options) && (NULL != options[i]); i++)
	{
		if (!venture_arbitrage_name_check("option", options[i], error))
			return FALSE;

		if (g_strv_contains(venture_arbitrage_scan_option_names(), options[i]))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "%s is a common scan option; a strategy's own options need "
			            "names of their own", options[i]);
			return FALSE;
		}
	}

	entry = g_new0(ArbStrategy, 1);
	entry->name = g_strdup(name);
	entry->label = g_strdup((NULL != label) ? label : name);
	entry->description = g_strdup((NULL != description) ? description : "");
	entry->options = g_strdupv((gchar **)options);
	entry->scan = scan;
	entry->plan = plan;
	entry->user_data = user_data;
	entry->destroy = destroy;
	g_ptr_array_add(self->entries, entry);

	return TRUE;
}

gboolean
venture_arbitrage_strategy_registry_has(
	VentureArbitrageStrategyRegistry	*self,
	const gchar				*name
){
	g_return_val_if_fail(VENTURE_IS_ARBITRAGE_STRATEGY_REGISTRY(self), FALSE);

	return NULL != arb_strategy_lookup(self, name);
}

gboolean
venture_arbitrage_strategy_registry_remove(
	VentureArbitrageStrategyRegistry	*self,
	const gchar	*name
){
	gpointer entry;

	g_return_val_if_fail(VENTURE_IS_ARBITRAGE_STRATEGY_REGISTRY(self), FALSE);

	entry = (gpointer)arb_strategy_lookup(self, name);

	/* Only the plugin manager's rollback calls this: a scan, a venue or
	 * a page holds a name, never an entry, so nothing is left pointing
	 * at what is freed here. */
	return (NULL != entry) && g_ptr_array_remove(self->entries, entry);
}

gchar **
venture_arbitrage_strategy_registry_dup_names(VentureArbitrageStrategyRegistry *self)
{
	GStrvBuilder *builder;
	gchar **names;
	guint i;

	g_return_val_if_fail(VENTURE_IS_ARBITRAGE_STRATEGY_REGISTRY(self), NULL);

	builder = g_strv_builder_new();

	for (i = 0; i < self->entries->len; i++)
		g_strv_builder_add(builder, ((ArbStrategy *)g_ptr_array_index(self->entries, i))->name);

	names = g_strv_builder_end(builder);
	g_strv_builder_unref(builder);

	return names;
}

const gchar *
venture_arbitrage_strategy_registry_get_label(
	VentureArbitrageStrategyRegistry	*self,
	const gchar				*name
){
	ArbStrategy *entry;

	g_return_val_if_fail(VENTURE_IS_ARBITRAGE_STRATEGY_REGISTRY(self), NULL);

	entry = arb_strategy_lookup(self, name);

	return (NULL != entry) ? entry->label : NULL;
}

const gchar *
venture_arbitrage_strategy_registry_get_description(
	VentureArbitrageStrategyRegistry	*self,
	const gchar				*name
){
	ArbStrategy *entry;

	g_return_val_if_fail(VENTURE_IS_ARBITRAGE_STRATEGY_REGISTRY(self), NULL);

	entry = arb_strategy_lookup(self, name);

	return (NULL != entry) ? entry->description : NULL;
}

VentureArbitrageStrategyRegistry *
venture_context_get_arbitrage_strategies(VentureContext *context)
{
	VentureArbitrageStrategyRegistry *registry;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	registry = g_object_get_data(G_OBJECT(context), ARB_STRATEGIES_CONTEXT_KEY);

	if (NULL == registry)
	{
		registry = venture_arbitrage_strategy_registry_new();
		g_object_set_data_full(G_OBJECT(context), ARB_STRATEGIES_CONTEXT_KEY, registry,
		                       g_object_unref);
	}

	return registry;
}

VentureExportFormatRegistry *
venture_context_get_export_formats(VentureContext *context)
{
	VentureExportFormatRegistry *registry;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	registry = g_object_get_data(G_OBJECT(context), ARB_EXPORTS_CONTEXT_KEY);

	if (NULL == registry)
	{
		registry = venture_export_format_registry_new();
		g_object_set_data_full(G_OBJECT(context), ARB_EXPORTS_CONTEXT_KEY, registry,
		                       g_object_unref);
	}

	return registry;
}

/* ==========================================================================
 * The question
 * ========================================================================== */

typedef enum
{
	ARB_OPT_TEXT = 0,	/* free text */
	ARB_OPT_INT,		/* a whole number in [min, max] */
	ARB_OPT_MONEY,		/* an amount naming its currency */
	ARB_OPT_PERCENT,	/* a percent, kept as text */
	ARB_OPT_FRACTION,	/* a ratio in [0, 1], kept as text */
	ARB_OPT_LIST,		/* comma separated keys */
	ARB_OPT_CHOICE		/* one of a fixed set */
} ArbOptionKind;

typedef struct
{
	const gchar		*name;
	ArbOptionKind		 kind;
	gint64			 min;
	gint64			 max;
	const gchar *const	*choices;
} ArbOption;

static const gchar *const arb_sell_bases[] = {
	"min", "market", "sale_avg", "region_median", "region_market", "bid", NULL
};

static const gchar *const arb_sorts[] = {
	"profit", "roi", "roi_per_day", "annualized", "ev", "confidence", NULL
};

/*
 * Every common option, in the order the docs list them. The names are the
 * report options of arbitrage_scan, so each must be forwarded by every
 * door (AGENTS.md: "A report option is dropped unless every door
 * forwards it"); none may be called `limit`.
 */
static const ArbOption arb_options[] = {
	{ "strategy", ARB_OPT_TEXT, 0, 0, NULL },
	{ "preset_id", ARB_OPT_INT, 1, G_MAXINT64, NULL },
	{ "data_source_id", ARB_OPT_INT, 0, G_MAXINT64, NULL },
	{ "buy_venues", ARB_OPT_LIST, 0, 0, NULL },
	{ "sell_venues", ARB_OPT_LIST, 0, 0, NULL },
	{ "venue_group", ARB_OPT_TEXT, 0, 0, NULL },
	{ "group_key", ARB_OPT_TEXT, 0, 0, NULL },
	{ "category_path", ARB_OPT_TEXT, 0, 0, NULL },
	{ "kind", ARB_OPT_TEXT, 0, 0, NULL },
	{ "instrument", ARB_OPT_TEXT, 0, 0, NULL },
	{ "recipe_id", ARB_OPT_INT, 1, G_MAXINT64, NULL },
	{ "units", ARB_OPT_INT, 1, VENTURE_ARBITRAGE_MAX_UNITS, NULL },
	{ "buy_sources", ARB_OPT_INT, 1, 10, NULL },
	{ "sell_basis", ARB_OPT_CHOICE, 0, 0, arb_sell_bases },
	{ "total_stake", ARB_OPT_MONEY, 0, 0, NULL },
	{ "min_profit", ARB_OPT_MONEY, 0, 0, NULL },
	{ "min_roi", ARB_OPT_PERCENT, 0, 0, NULL },
	{ "min_sale_rate", ARB_OPT_PERCENT, 0, 0, NULL },
	{ "max_capital", ARB_OPT_MONEY, 0, 0, NULL },
	{ "max_buy_pct", ARB_OPT_PERCENT, 0, 0, NULL },
	{ "min_confidence", ARB_OPT_FRACTION, 0, 0, NULL },
	{ "max_age_hours", ARB_OPT_INT, 1, 24 * 366, NULL },
	{ "share", ARB_OPT_PERCENT, 0, 0, NULL },
	{ "sort", ARB_OPT_CHOICE, 0, 0, arb_sorts },
	{ "top", ARB_OPT_INT, 1, VENTURE_ARBITRAGE_SCAN_TOP_MAX, NULL }
};

/* Names a caller's options may carry that the scan does not read: the
 * report machinery adds the organization and a venture, and a page its
 * as-of time. */
static const gchar *const arb_ignored_options[] = {
	"organization_id", "venture_id", "as_of", NULL
};

const gchar *const *
venture_arbitrage_scan_option_names(void)
{
	static gsize initialised = 0;
	static const gchar *names[G_N_ELEMENTS(arb_options) + 1];

	if (g_once_init_enter(&initialised))
	{
		guint i;

		for (i = 0; i < G_N_ELEMENTS(arb_options); i++)
			names[i] = arb_options[i].name;

		names[G_N_ELEMENTS(arb_options)] = NULL;
		g_once_init_leave(&initialised, 1);
	}

	return names;
}

static const ArbOption *
arb_option_lookup(const gchar *name)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS(arb_options); i++)
	{
		if (0 == g_strcmp0(arb_options[i].name, name))
			return &arb_options[i];
	}

	return NULL;
}

/* One option's value, judged and written in its one spelling. */
static gboolean
arb_option_normalise(
	const ArbOption	 *option,
	JsonNode	 *node,
	JsonObject	 *out,
	GError		**error
){
	g_autofree gchar *text = NULL;

	text = venture_arbitrage_node_text(node);

	if (NULL != text)
		g_strstrip(text);

	/* An empty box on a form is an option not asked. */
	if (venture_string_is_empty(text))
		return TRUE;

	switch (option->kind)
	{
	case ARB_OPT_INT:
	{
		gint64 value;

		if (!g_ascii_string_to_signed(text, 10, option->min, option->max, &value, NULL))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "%s is a whole number from %" G_GINT64_FORMAT " to %" G_GINT64_FORMAT,
			            option->name, option->min, option->max);
			return FALSE;
		}

		json_object_set_int_member(out, option->name, value);
		return TRUE;
	}

	case ARB_OPT_MONEY:
	{
		g_autoptr(VentureMoney) money = NULL;
		g_autofree gchar *written = NULL;

		money = venture_marketdata_parse_amount(text, error);

		if (NULL == money)
		{
			g_prefix_error(error, "%s: ", option->name);
			return FALSE;
		}

		if (venture_money_is_negative(money))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "%s cannot be negative", option->name);
			return FALSE;
		}

		/* A stake of nothing splits into nothing: cover refused the whole
		 * scan and back_lay quietly answered no rows. One refusal here. */
		if ((0 == g_strcmp0(option->name, "total_stake")) && venture_money_is_zero(money))
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			                    "total_stake must be more than nothing");
			return FALSE;
		}

		written = venture_money_to_string(money);
		json_object_set_string_member(out, option->name, written);
		return TRUE;
	}

	case ARB_OPT_PERCENT:
	{
		gint64 ppm;

		if (!venture_arbitrage_parse_percent(text, FALSE, &ppm, error))
		{
			g_prefix_error(error, "%s: ", option->name);
			return FALSE;
		}

		if ((0 == g_strcmp0(option->name, "share")) && ((ppm <= 0) || (ppm > 1000000)))
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			                    "share is a percent above 0 and at most 100");
			return FALSE;
		}

		if ((0 == g_strcmp0(option->name, "min_sale_rate")) && (ppm > 1000000))
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			                    "min_sale_rate is a percent of listings, at most 100");
			return FALSE;
		}

		json_object_set_string_member(out, option->name, text);
		return TRUE;
	}

	case ARB_OPT_FRACTION:
	{
		gchar *end = NULL;
		gdouble value;

		value = g_ascii_strtod(text, &end);

		if ((NULL == end) || ('\0' != *end) || !isfinite(value) || (value < 0.0) || (value > 1.0))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "%s is a number from 0 to 1, e.g. 0.5", option->name);
			return FALSE;
		}

		json_object_set_string_member(out, option->name, text);
		return TRUE;
	}

	case ARB_OPT_LIST:
	{
		g_auto(GStrv) parts = NULL;
		g_autoptr(GString) list = NULL;
		guint i;

		parts = g_strsplit(text, ",", -1);
		list = g_string_new(NULL);

		for (i = 0; NULL != parts[i]; i++)
		{
			g_strstrip(parts[i]);

			if ('\0' == parts[i][0])
				continue;

			if (list->len > 0)
				g_string_append_c(list, ',');

			g_string_append(list, parts[i]);
		}

		if (list->len > 0)
			json_object_set_string_member(out, option->name, list->str);

		return TRUE;
	}

	case ARB_OPT_CHOICE:
		if (!g_strv_contains(option->choices, text))
		{
			g_autofree gchar *known = g_strjoinv(", ", (gchar **)option->choices);

			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "%s is one of %s, not \"%s\"", option->name, known, text);
			return FALSE;
		}

		json_object_set_string_member(out, option->name, text);
		return TRUE;

	case ARB_OPT_TEXT:
	default:
		if (strlen(text) > 512)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "%s is at most 512 bytes", option->name);
			return FALSE;
		}

		json_object_set_string_member(out, option->name, text);
		return TRUE;
	}
}

/*
 * The preset's half of a question: its record's fields and its options
 * mapping, read as options. Another organization's preset, or a deleted
 * one, is NOT_FOUND.
 */
static JsonObject *
arb_preset_options(
	VentureDatabase	 *database,
	gint64		  organization_id,
	gint64		  preset_id,
	GError		**error
){
	g_autoptr(VentureEntity) preset = NULL;
	g_autoptr(JsonObject) options = NULL;
	g_autofree gchar *strategy = NULL;
	g_autofree gchar *buy = NULL;
	g_autofree gchar *sell = NULL;
	g_autofree gchar *text = NULL;
	gint64 source;

	preset = venture_database_get(database, VENTURE_TYPE_ARBITRAGE_STRATEGY, preset_id, NULL);

	if ((NULL == preset) || venture_entity_is_deleted(preset) ||
	    (venture_entity_get_organization_id(preset) != organization_id))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "No arbitrage preset #%" G_GINT64_FORMAT " in this organization", preset_id);
		return NULL;
	}

	g_object_get(preset, "strategy", &strategy, "data-source-id", &source,
	             "buy-venues", &buy, "sell-venues", &sell, "options", &text, NULL);
	options = venture_arbitrage_parse_mapping(text, "A preset's filters", "min_roi: 15", error);

	if (NULL == options)
		return NULL;

	if (json_object_has_member(options, "preset_id"))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "A preset's filters cannot load another preset");
		return NULL;
	}

	if (!venture_string_is_empty(strategy))
		json_object_set_string_member(options, "strategy", strategy);

	if (source > 0)
		json_object_set_int_member(options, "data_source_id", source);

	if (!venture_string_is_empty(buy))
		json_object_set_string_member(options, "buy_venues", buy);

	if (!venture_string_is_empty(sell))
		json_object_set_string_member(options, "sell_venues", sell);

	return g_steal_pointer(&options);
}

/*
 * The reader every door shares. @strategies judges the strategy and lends
 * its own option names; @database is NULL when presets may not be loaded
 * (a preset's own filters).
 */
static JsonObject *
arb_normalise(
	VentureDatabase				 *database,
	VentureArbitrageStrategyRegistry	 *strategies,
	gint64					  organization_id,
	JsonObject				 *options,
	GError					**error
){
	g_autoptr(JsonObject) merged = NULL;
	g_autoptr(JsonObject) out = NULL;
	g_autoptr(GList) members = NULL;
	ArbStrategy *strategy;
	const gchar *name;
	GList *member;
	gint64 preset_id;

	merged = json_object_new();
	preset_id = 0;

	if ((NULL != options) && json_object_has_member(options, "preset_id"))
	{
		preset_id = venture_json_object_get_int(options, "preset_id", 0);

		if (preset_id <= 0)
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			                    "preset_id is a preset's id");
			return NULL;
		}

		if (NULL == database)
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			                    "A preset's filters cannot load another preset");
			return NULL;
		}
	}

	/* The preset first; what is asked beside it wins. */
	if (preset_id > 0)
	{
		g_autoptr(JsonObject) preset = NULL;

		preset = arb_preset_options(database, organization_id, preset_id, error);

		if (NULL == preset)
			return NULL;

		members = json_object_get_members(preset);

		for (member = members; NULL != member; member = member->next)
			json_object_set_member(merged, member->data,
			                       json_node_copy(json_object_get_member(preset, member->data)));

		g_clear_pointer(&members, g_list_free);
	}

	if (NULL != options)
	{
		members = json_object_get_members(options);

		for (member = members; NULL != member; member = member->next)
		{
			JsonNode *node = json_object_get_member(options, member->data);
			g_autofree gchar *text = venture_arbitrage_node_text(node);

			/* A blank box does not clear what the preset says. */
			if ((preset_id > 0) && venture_string_is_empty(text))
				continue;

			json_object_set_member(merged, member->data, json_node_copy(node));
		}

		g_clear_pointer(&members, g_list_free);
	}

	name = venture_json_object_get_string(merged, "strategy", NULL);

	if (venture_string_is_empty(name))
		name = "spread";

	strategy = arb_strategy_lookup(strategies, name);

	if (NULL == strategy)
	{
		g_auto(GStrv) names = venture_arbitrage_strategy_registry_dup_names(strategies);
		g_autofree gchar *list = g_strjoinv(", ", names);

		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "\"%s\" is not a registered strategy; registered: %s", name, list);
		return NULL;
	}

	out = json_object_new();
	json_object_set_string_member(out, "strategy", strategy->name);

	if (preset_id > 0)
		json_object_set_int_member(out, "preset_id", preset_id);

	members = json_object_get_members(merged);

	for (member = members; NULL != member; member = member->next)
	{
		const ArbOption *option;
		const gchar *key;

		key = member->data;

		if ((0 == g_strcmp0(key, "strategy")) || (0 == g_strcmp0(key, "preset_id")) ||
		    g_strv_contains(arb_ignored_options, key))
			continue;

		option = arb_option_lookup(key);

		if (NULL != option)
		{
			if (!arb_option_normalise(option, json_object_get_member(merged, key), out, error))
				return NULL;

			continue;
		}

		/* A strategy's own option travels as it was given. */
		if ((NULL != strategy->options) && g_strv_contains((const gchar *const *)strategy->options, key))
		{
			json_object_set_member(out, key, json_node_copy(json_object_get_member(merged, key)));
			continue;
		}

		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "%s is not a scan option; the %s strategy takes the common options "
		            "(docs/arbitrage.org, \"The question\")%s%s", key, strategy->name,
		            (NULL != strategy->options) ? " and " : "",
		            (NULL != strategy->options) ? strategy->options[0] : "");
		return NULL;
	}

	return g_steal_pointer(&out);
}

JsonObject *
venture_arbitrage_scan_options_normalise(
	VentureContext	 *context,
	gint64		  organization_id,
	JsonObject	 *options,
	GError		**error
){
	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	if (organization_id <= 0)
		organization_id = venture_context_get_default_organization_id(context);

	return arb_normalise(venture_context_get_database(context),
	                     venture_context_get_arbitrage_strategies(context), organization_id,
	                     options, error);
}

/* ==========================================================================
 * The scan's state
 * ========================================================================== */

struct _VentureArbitrageScan
{
	VentureContext			*context;
	VentureDatabase			*database;
	VentureFeeModelRegistry		*fees;
	gint64				 organization_id;
	JsonObject			*options;
	gint64				 now;
	gint64				 stale_after;	/* series.stale_minutes, in seconds */
	GDateTime			*when;
	gint64				 units;
	gchar				**buy_venues;
	gchar				**sell_venues;
	gchar				*venue_group_name;
	GPtrArray			*sources;	/* VentureEntity */
	GHashTable			*stores;	/* source id -> store, or none */
	GHashTable			*venues;	/* "id\037key" -> VentureArbitrageVenue */
	GHashTable			*noted;
	JsonArray			*notes;
	GPtrArray			*rows;		/* JsonObject */
	VentureExchangePolicy		*policy;
	GHashTable			*no_rate;	/* "FROM>TO" -> count */
	GHashTable			*attrs;		/* "id\037key" -> JsonObject, or NULL */
	guint				 never_sells;
};

static void
arb_venue_free(gpointer data)
{
	VentureArbitrageVenue *venue;

	venue = data;
	g_free(venue->name);
	g_free(venue->currency);
	g_free(venue->fee_model);
	g_clear_pointer(&venue->fee_params, json_object_unref);
	g_clear_pointer(&venue->transfer_cost, venture_money_free);
	g_free(venue->problem);
	g_free(venue);
}

static void
arb_unref_store(gpointer store)
{
	if (NULL != store)
		g_object_unref(store);
}

static void
arb_unref_attrs(gpointer attrs)
{
	if (NULL != attrs)
		json_object_unref(attrs);
}

static void
arb_scan_free(VentureArbitrageScan *scan)
{
	if (NULL == scan)
		return;

	g_clear_pointer(&scan->options, json_object_unref);
	g_clear_pointer(&scan->when, g_date_time_unref);
	g_strfreev(scan->buy_venues);
	g_strfreev(scan->sell_venues);
	g_free(scan->venue_group_name);
	g_clear_pointer(&scan->sources, g_ptr_array_unref);
	g_clear_pointer(&scan->stores, g_hash_table_unref);
	g_clear_pointer(&scan->venues, g_hash_table_unref);
	g_clear_pointer(&scan->noted, g_hash_table_unref);
	g_clear_pointer(&scan->notes, json_array_unref);
	g_clear_pointer(&scan->rows, g_ptr_array_unref);
	g_clear_object(&scan->policy);
	g_clear_pointer(&scan->no_rate, g_hash_table_unref);
	g_clear_pointer(&scan->attrs, g_hash_table_unref);
	g_free(scan);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(VentureArbitrageScan, arb_scan_free)

VentureContext *
venture_arbitrage_scan_get_context(VentureArbitrageScan *scan)
{
	g_return_val_if_fail(NULL != scan, NULL);

	return scan->context;
}

gint64
venture_arbitrage_scan_get_organization_id(VentureArbitrageScan *scan)
{
	g_return_val_if_fail(NULL != scan, 0);

	return scan->organization_id;
}

JsonObject *
venture_arbitrage_scan_get_options(VentureArbitrageScan *scan)
{
	g_return_val_if_fail(NULL != scan, NULL);

	return scan->options;
}

gint64
venture_arbitrage_scan_get_now(VentureArbitrageScan *scan)
{
	g_return_val_if_fail(NULL != scan, 0);

	return scan->now;
}

gint64
venture_arbitrage_scan_get_units(VentureArbitrageScan *scan)
{
	g_return_val_if_fail(NULL != scan, 1);

	return scan->units;
}

GPtrArray *
venture_arbitrage_scan_get_sources(VentureArbitrageScan *scan)
{
	g_return_val_if_fail(NULL != scan, NULL);

	return scan->sources;
}

const gchar *const *
venture_arbitrage_scan_get_buy_venues(VentureArbitrageScan *scan)
{
	g_return_val_if_fail(NULL != scan, NULL);

	return (const gchar *const *)scan->buy_venues;
}

gboolean
venture_arbitrage_scan_venue_allowed(
	VentureArbitrageScan	*scan,
	gboolean		 buy,
	const gchar		*venue_key
){
	gchar **set;

	g_return_val_if_fail(NULL != scan, FALSE);

	set = buy ? scan->buy_venues : scan->sell_venues;

	return (NULL == set) || ((NULL != venue_key) && g_strv_contains((const gchar *const *)set, venue_key));
}

void
venture_arbitrage_scan_add_note(
	VentureArbitrageScan	*scan,
	const gchar		*note
){
	g_return_if_fail(NULL != scan);

	if ((NULL == note) || g_hash_table_contains(scan->noted, note))
		return;

	g_hash_table_add(scan->noted, g_strdup(note));
	json_array_add_string_element(scan->notes, note);
}

void
venture_arbitrage_scan_add(
	VentureArbitrageScan	*scan,
	JsonObject		*opportunity
){
	g_return_if_fail(NULL != scan);
	g_return_if_fail(NULL != opportunity);

	if (!json_object_has_member(opportunity, "strategy"))
		json_object_set_string_member(opportunity, "strategy",
		                              venture_json_object_get_string(scan->options, "strategy", ""));

	g_ptr_array_add(scan->rows, opportunity);
}

const VentureArbitrageVenue *
venture_arbitrage_scan_venue(
	VentureArbitrageScan	*scan,
	gint64			 data_source_id,
	const gchar		*venue_key
){
	g_autofree gchar *cache_key = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) record = NULL;
	VentureArbitrageVenue *venue;

	g_return_val_if_fail(NULL != scan, NULL);

	cache_key = g_strdup_printf("%" G_GINT64_FORMAT "\037%s", data_source_id,
	                            (NULL != venue_key) ? venue_key : "");
	venue = g_hash_table_lookup(scan->venues, cache_key);

	if (NULL != venue)
		return venue;

	venue = g_new0(VentureArbitrageVenue, 1);
	g_hash_table_insert(scan->venues, g_steal_pointer(&cache_key), venue);

	/* The venue record a source's venue was promoted to, if any: its
	 * fees, its transfer cost and its currency are what the scan
	 * prices with. */
	query = venture_query_new(VENTURE_TYPE_VENUE);
	venture_query_set_organization(query, scan->organization_id);
	venture_query_set_limit(query, 1);

	if ((NULL != venue_key) &&
	    venture_query_add_filter_int(query, "data-source-id", VENTURE_FILTER_OP_EQ, data_source_id, NULL) &&
	    venture_query_add_filter_string(query, "key", VENTURE_FILTER_OP_EQ, venue_key, NULL))
		record = venture_database_find_one(scan->database, query, NULL);

	if (NULL == record)
	{
		venue->name = g_strdup(venue_key);
		return venue;
	}

	venue->record_id = venture_entity_get_id(record);
	g_object_get(record, "name", &venue->name, "currency", &venue->currency,
	             "fee-model", &venue->fee_model, "transfer-cost", &venue->transfer_cost,
	             "transfer-hours", &venue->transfer_hours, NULL);

	if (venture_string_is_empty(venue->fee_model))
		g_clear_pointer(&venue->fee_model, g_free);

	{
		g_autoptr(GError) local_error = NULL;
		g_autofree gchar *params = NULL;

		g_object_get(record, "fee-params", &params, NULL);
		venue->fee_params = venture_fee_model_parse_params(params, &local_error);

		if (NULL == venue->fee_params)
			venue->problem = g_strdup_printf("%s's fee parameters: %s",
			                                 venue->name, local_error->message);
		else if ((NULL != venue->fee_model) &&
		         !venture_fee_model_registry_has(scan->fees, venue->fee_model))
			venue->problem = g_strdup_printf("%s's fee model %s is not loaded",
			                                 venue->name, venue->fee_model);
	}

	return venue;
}

/*
 * An instrument's stored attributes, for a fee model keyed on the thing
 * itself (an auction house's deposit is a share of the vendor price). Read
 * once per scan; an unknown instrument, a store that cannot be read and
 * attributes that are not an object are all "none", remembered as NULL.
 */
static JsonObject *
arb_instrument_attrs(
	VentureArbitrageScan	*scan,
	gint64			 data_source_id,
	const gchar		*instrument_key
){
	g_autofree gchar *cache_key = NULL;
	JsonObject *attrs = NULL;
	gpointer found;

	if (NULL == instrument_key)
		return NULL;

	cache_key = g_strdup_printf("%" G_GINT64_FORMAT "\037%s", data_source_id, instrument_key);

	if (g_hash_table_lookup_extended(scan->attrs, cache_key, NULL, &found))
		return found;

#ifdef VENTURE_HAVE_SQLITE
	{
		g_autoptr(VentureSeriesInstrumentRow) row = NULL;
		VentureSeriesStore *store;

		store = venture_arbitrage_scan_open_store(scan, data_source_id);

		if ((NULL != store) &&
		    venture_series_store_get_instrument(store, instrument_key, &row, NULL) &&
		    (NULL != row) && (NULL != row->attrs_json))
		{
			g_autoptr(JsonNode) node = json_from_string(row->attrs_json, NULL);

			if ((NULL != node) && JSON_NODE_HOLDS_OBJECT(node))
				attrs = json_object_ref(json_node_get_object(node));
		}
	}
#endif

	g_hash_table_insert(scan->attrs, g_steal_pointer(&cache_key), attrs);

	return attrs;
}

gboolean
venture_arbitrage_scan_fees(
	VentureArbitrageScan	 *scan,
	gint64			  data_source_id,
	const gchar		 *venue_key,
	VentureFeeSide		  side,
	const VentureMoney	 *amount,
	gint64			  units,
	const VentureMoney	 *reference,
	const gchar		 *instrument_key,
	VentureFeeQuote		 *out,
	GError			**error
){
	const VentureArbitrageVenue *venue;

	g_return_val_if_fail(NULL != scan, FALSE);
	g_return_val_if_fail(NULL != amount, FALSE);

	memset(out, 0, sizeof(*out));
	venue = venture_arbitrage_scan_venue(scan, data_source_id, venue_key);

	if (NULL != venue->problem)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, venue->problem);
		return FALSE;
	}

	if (!venture_fee_model_registry_compute(scan->fees, venue->fee_model, venue->fee_params, side,
	                                        amount, units, reference,
	                                        arb_instrument_attrs(scan, data_source_id, instrument_key),
	                                        out, error))
	{
		g_prefix_error(error, "%s: ", (NULL != venue->name) ? venue->name : venue_key);
		return FALSE;
	}

	return TRUE;
}

VentureMoney *
venture_arbitrage_scan_convert(
	VentureArbitrageScan	 *scan,
	const VentureMoney	 *amount,
	const gchar		 *currency,
	GError			**error
){
	g_return_val_if_fail(NULL != scan, NULL);
	g_return_val_if_fail(NULL != amount, NULL);

	if ((NULL == currency) || (0 == g_strcmp0(venture_money_get_currency(amount), currency)))
		return venture_money_copy(amount);

	if (NULL == scan->policy)
		scan->policy = venture_rate_table_policy_new(scan->database, scan->organization_id);

	return venture_exchange_policy_convert(scan->policy, amount, currency, scan->when, error);
}

#ifdef VENTURE_HAVE_SQLITE
VentureSeriesStore *
venture_arbitrage_scan_open_store(
	VentureArbitrageScan	*scan,
	gint64			 data_source_id
){
	g_autoptr(GError) error = NULL;
	VentureFeedsService *service;
	VentureSeriesStore *store;
	gpointer found;
	guint i;

	g_return_val_if_fail(NULL != scan, NULL);

	if (g_hash_table_lookup_extended(scan->stores, &data_source_id, NULL, &found))
		return found;

	store = NULL;
	service = venture_context_get_feeds_service(scan->context);

	if (NULL != service)
		store = venture_feeds_service_open_reader(service, data_source_id, &error);

	if (NULL == store)
	{
		g_autofree gchar *note = NULL;
		g_autofree gchar *name = NULL;

		for (i = 0; (NULL == name) && (i < scan->sources->len); i++)
		{
			VentureEntity *source = g_ptr_array_index(scan->sources, i);

			if (venture_entity_get_id(source) == data_source_id)
				name = venture_entity_get_display_name(source);
		}

		if ((NULL != error) && g_error_matches(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND))
			note = g_strdup_printf("%s has stored nothing yet",
			                       (NULL != name) ? name : "A data source");
		else
			note = g_strdup_printf("%s cannot be read: %s",
			                       (NULL != name) ? name : "A data source",
			                       (NULL != error) ? error->message : "market data feeds are off");

		venture_arbitrage_scan_add_note(scan, note);
	}

	g_hash_table_insert(scan->stores, g_memdup2(&data_source_id, sizeof(data_source_id)), store);

	return store;
}
#endif

/* ==========================================================================
 * Flips: the one way a buy and a sell become an opportunity
 * ========================================================================== */

static void
arb_set_time(
	JsonObject	*object,
	const gchar	*member,
	gint64		 when
){
	g_autoptr(GDateTime) moment = NULL;
	g_autofree gchar *text = NULL;

	moment = (when > 0) ? g_date_time_new_from_unix_utc(when) : NULL;

	if (NULL == moment)
	{
		json_object_set_null_member(object, member);
		return;
	}

	text = venture_time_to_string(moment);
	json_object_set_string_member(object, member, text);
}

/* A leg in store terms, which the default plan promotes and records. */
static JsonObject *
arb_leg(
	const gchar		*kind,
	gint64			 data_source_id,
	const gchar		*venue_key,
	const gchar		*instrument_key,
	gint64			 quantity,
	const VentureMoney	*unit_price,
	const VentureMoney	*amount,
	const VentureMoney	*fees
){
	JsonObject *leg;

	leg = json_object_new();
	json_object_set_string_member(leg, "kind", kind);
	json_object_set_int_member(leg, "data_source_id", data_source_id);
	json_object_set_string_member(leg, "venue_key", venue_key);

	if (NULL != instrument_key)
		json_object_set_string_member(leg, "instrument_key", instrument_key);

	if (quantity > 0)
		json_object_set_int_member(leg, "quantity", quantity);

	if (NULL != unit_price)
	{
		g_autofree gchar *text = venture_money_to_string(unit_price);

		json_object_set_string_member(leg, "unit_price", text);
	}

	if (NULL != amount)
	{
		g_autofree gchar *text = venture_money_to_string(amount);

		json_object_set_string_member(leg, "amount", text);
	}

	if ((NULL != fees) && !venture_money_is_zero(fees))
	{
		g_autofree gchar *text = venture_money_to_string(fees);

		json_object_set_string_member(leg, "fees", text);
	}

	return leg;
}

/* A side's description in the answer. */
static JsonObject *
arb_side_object(
	VentureArbitrageScan		*scan,
	const VentureArbitrageSide	*side,
	const VentureMoney		*unit_price,
	const VentureMoney		*amount,
	const VentureMoney		*fees
){
	const VentureArbitrageVenue *venue;
	JsonObject *object;

	venue = venture_arbitrage_scan_venue(scan, side->data_source_id, side->venue_key);
	object = json_object_new();
	json_object_set_int_member(object, "data_source_id", side->data_source_id);
	json_object_set_string_member(object, "venue_key", side->venue_key);
	json_object_set_string_member(object, "venue_name",
	                              (NULL != venue->name) ? venue->name : side->venue_key);

	if (venue->record_id > 0)
		json_object_set_int_member(object, "venue_id", venue->record_id);

	venture_arbitrage_set_money(object, "unit_price", unit_price);
	venture_arbitrage_set_money(object, "amount", amount);
	venture_arbitrage_set_money(object, "fees", fees);
	arb_set_time(object, "taken_at", side->taken_at);

	/* How old the price is, judged as the Trading pages judge it: a
	 * spread from a realm whose feed stopped is a spread nobody can take. */
	if (side->taken_at > 0)
	{
		json_object_set_int_member(object, "age_seconds", MAX((gint64)0, scan->now - side->taken_at));
		json_object_set_boolean_member(object, "stale", scan->now - side->taken_at > scan->stale_after);
	}
	else
	{
		json_object_set_null_member(object, "age_seconds");
		json_object_set_boolean_member(object, "stale", TRUE);
	}

	return object;
}

/* An amount, scaled to all the units of a side. */
static VentureMoney *
arb_side_amount(
	const VentureArbitrageSide	 *side,
	GError				**error
){
	if (NULL != side->amount)
		return venture_money_copy(side->amount);

	return venture_money_multiply_int(side->unit_price, side->units, error);
}

/* Records that a pair had no rate, once per currency pair, in a note at
 * the end. */
static void
arb_no_rate(
	VentureArbitrageScan	*scan,
	const gchar		*from,
	const gchar		*to
){
	g_autofree gchar *pair = NULL;
	guint count;

	pair = g_strdup_printf("%s>%s", from, to);
	count = GPOINTER_TO_UINT(g_hash_table_lookup(scan->no_rate, pair));
	g_hash_table_insert(scan->no_rate, g_steal_pointer(&pair), GUINT_TO_POINTER(count + 1));
}

/* Converts *@amount in place into @currency, or says there is no rate. */
static gboolean
arb_convert_in_place(
	VentureArbitrageScan	*scan,
	VentureMoney		**amount,
	const gchar		 *currency
){
	VentureMoney *converted;

	if ((NULL == *amount) || (0 == g_strcmp0(venture_money_get_currency(*amount), currency)))
		return TRUE;

	converted = venture_arbitrage_scan_convert(scan, *amount, currency, NULL);

	if (NULL == converted)
	{
		arb_no_rate(scan, venture_money_get_currency(*amount), currency);
		return FALSE;
	}

	venture_money_free(*amount);
	*amount = converted;

	return TRUE;
}

/* The `share` option as a fraction, one when not asked. */
static gdouble
arb_share(VentureArbitrageScan *scan)
{
	gint64 ppm;

	if (!venture_arbitrage_member_percent(scan->options, "share", 1000000, FALSE, &ppm, NULL) ||
	    (ppm <= 0))
		return 1.0;

	return MIN(1.0, (gdouble)ppm / 1000000.0);
}

gboolean
venture_arbitrage_scan_add_flip(
	VentureArbitrageScan		 *scan,
	const gchar			 *key,
	const gchar			 *title,
	const VentureArbitrageSide	 *buy,
	const VentureArbitrageSide	 *sell,
	const VentureArbitrageMarket	 *market,
	JsonObject			 *extra,
	GError				**error
){
	g_autoptr(VentureMoney) buy_amount = NULL;
	g_autoptr(VentureMoney) sell_gross = NULL;
	g_autoptr(VentureMoney) sell_native = NULL;
	g_autoptr(VentureMoney) sell_fees_native = NULL;
	g_autoptr(VentureMoney) buy_fees = NULL;
	g_autoptr(VentureMoney) sell_fees = NULL;
	g_autoptr(VentureMoney) deposit = NULL;
	g_autoptr(VentureMoney) transfer = NULL;
	g_autoptr(VentureMoney) unwind = NULL;
	g_autoptr(VentureMoney) reference = NULL;
	g_autoptr(GError) local_error = NULL;
	g_autoptr(GPtrArray) missing = NULL;
	g_autoptr(GPtrArray) warnings = NULL;
	VentureArbitrageFlipInput input;
	VentureArbitrageFlip flip;
	VentureArbitrageEvidence evidence;
	VentureFeeQuote buy_quote;
	VentureFeeQuote sell_quote;
	const VentureArbitrageVenue *buy_venue;
	const VentureArbitrageVenue *sell_venue;
	const gchar *currency;
	gboolean moved;
	gboolean refundable;
	gint64 transit_hours;
	JsonObject *opportunity;
	JsonArray *legs;
	JsonArray *array;
	guint i;

	g_return_val_if_fail(NULL != scan, FALSE);
	g_return_val_if_fail((NULL != buy) && (NULL != sell) && (NULL != market), FALSE);
	g_return_val_if_fail((NULL != buy->unit_price) && (NULL != sell->unit_price), FALSE);

	memset(&buy_quote, 0, sizeof(buy_quote));
	memset(&sell_quote, 0, sizeof(sell_quote));
	missing = g_ptr_array_new_with_free_func(g_free);
	warnings = g_ptr_array_new_with_free_func(g_free);
	currency = venture_money_get_currency(buy->unit_price);
	buy_venue = venture_arbitrage_scan_venue(scan, buy->data_source_id, buy->venue_key);
	sell_venue = venture_arbitrage_scan_venue(scan, sell->data_source_id, sell->venue_key);
	moved = (buy->data_source_id != sell->data_source_id) ||
	        (0 != g_strcmp0(buy->venue_key, sell->venue_key));

	buy_amount = arb_side_amount(buy, error);
	sell_native = (NULL != buy_amount) ? arb_side_amount(sell, error) : NULL;

	if (NULL == sell_native)
		return FALSE;

	/* --- What each venue charges, in its own currency --- */

	if (0 == buy_venue->record_id)
		g_ptr_array_add(warnings, g_strdup_printf("%s has no venue record: no fees counted there",
		                                          buy->venue_key));

	if (moved && (0 == sell_venue->record_id))
		g_ptr_array_add(warnings, g_strdup_printf("%s has no venue record: no fees counted there",
		                                          sell->venue_key));

	if (!venture_arbitrage_scan_fees(scan, buy->data_source_id, buy->venue_key, VENTURE_FEE_SIDE_BUY,
	                                 buy_amount, buy->units, NULL, buy->instrument_key, &buy_quote,
	                                 &local_error))
	{
		g_ptr_array_add(missing, g_strdup_printf("fees: %s", local_error->message));
		g_clear_error(&local_error);
	}

	if (NULL != sell->reference)
	{
		reference = venture_money_multiply_int(sell->reference, sell->units, error);

		if (NULL == reference)
		{
			venture_fee_quote_clear(&buy_quote);
			return FALSE;
		}
	}

	if (!venture_arbitrage_scan_fees(scan, sell->data_source_id, sell->venue_key, VENTURE_FEE_SIDE_SELL,
	                                 sell_native, sell->units, reference, sell->instrument_key,
	                                 &sell_quote, &local_error))
	{
		g_ptr_array_add(missing, g_strdup_printf("fees: %s", local_error->message));
		g_clear_error(&local_error);
	}

	buy_fees = g_steal_pointer(&buy_quote.fee);
	sell_fees_native = g_steal_pointer(&sell_quote.fee);
	deposit = g_steal_pointer(&sell_quote.deposit);
	refundable = sell_quote.deposit_refundable || (NULL == deposit);
	venture_fee_quote_clear(&buy_quote);
	venture_fee_quote_clear(&sell_quote);

	/* --- Into the buy side's currency, or not at all --- */

	sell_gross = venture_money_copy(sell_native);
	sell_fees = (NULL != sell_fees_native) ? venture_money_copy(sell_fees_native) : NULL;

	if (!arb_convert_in_place(scan, &sell_gross, currency) ||
	    !arb_convert_in_place(scan, &sell_fees, currency) ||
	    !arb_convert_in_place(scan, &deposit, currency))
		return TRUE;

	/* Moving the lot: what each venue says moving to or from it costs. */
	transit_hours = 0;

	if (moved)
	{
		const VentureArbitrageVenue *ends[2];

		ends[0] = buy_venue;
		ends[1] = sell_venue;

		for (i = 0; i < 2; i++)
		{
			g_autoptr(VentureMoney) part = NULL;

			transit_hours += MAX((gint64)0, ends[i]->transfer_hours);

			if (NULL == ends[i]->transfer_cost)
				continue;

			part = venture_money_copy(ends[i]->transfer_cost);

			if (!arb_convert_in_place(scan, &part, currency))
				return TRUE;

			if (NULL == transfer)
			{
				transfer = g_steal_pointer(&part);
				continue;
			}

			{
				VentureMoney *sum = venture_money_add(transfer, part, error);

				if (NULL == sum)
					return FALSE;

				venture_money_free(transfer);
				transfer = sum;
			}
		}
	}

	/* --- The figures --- */

	opportunity = json_object_new();
	json_object_set_string_member(opportunity, "strategy",
	                              venture_json_object_get_string(scan->options, "strategy", ""));
	json_object_set_string_member(opportunity, "key", key);
	json_object_set_string_member(opportunity, "title", (NULL != title) ? title : key);
	json_object_set_string_member(opportunity, "instrument_key", buy->instrument_key);

	if (NULL != buy->instrument_name)
		json_object_set_string_member(opportunity, "instrument_name", buy->instrument_name);

	json_object_set_int_member(opportunity, "data_source_id", buy->data_source_id);
	json_object_set_string_member(opportunity, "currency", currency);
	json_object_set_int_member(opportunity, "units", buy->units);
	json_object_set_object_member(opportunity, "buy",
		arb_side_object(scan, buy, buy->unit_price, buy_amount, buy_fees));
	json_object_set_object_member(opportunity, "sell",
		arb_side_object(scan, sell, sell->unit_price, sell_native, sell_fees_native));
	venture_arbitrage_set_money(json_object_get_object_member(opportunity, "sell"), "deposit", deposit);
	venture_arbitrage_set_money(opportunity, "transfer_cost", transfer);
	venture_arbitrage_set_ratio(opportunity, "sale_rate", market->sale_rate);
	venture_arbitrage_set_ratio(opportunity, "sold_per_day", market->sold_per_day);
	json_object_set_int_member(opportunity, "venues", market->venues);

	{
		gint64 oldest = MIN(buy->taken_at, sell->taken_at);

		json_object_set_int_member(opportunity, "age_seconds", MAX((gint64)0, scan->now - oldest));
		arb_set_time(opportunity, "taken_at", oldest);
	}

	if (0 != g_strcmp0(venture_money_get_currency(sell_native), currency))
	{
		g_autofree gchar *note = g_strdup_printf("%s converted to %s at the exchange_rate on file",
		                                         venture_money_get_currency(sell_native), currency);

		g_ptr_array_add(warnings, g_steal_pointer(&note));
	}

	/* Legs, in store terms, for the plan: the buy, the sell, and a fee at
	 * each end for moving the lot, in that venue's own currency. */
	legs = json_array_new();
	json_array_add_object_element(legs, arb_leg("buy", buy->data_source_id, buy->venue_key,
		buy->instrument_key, buy->units, buy->unit_price, buy_amount, buy_fees));
	json_array_add_object_element(legs, arb_leg("sell", sell->data_source_id, sell->venue_key,
		sell->instrument_key, sell->units, sell->unit_price, sell_native, sell_fees_native));

	if (moved)
	{
		if ((NULL != buy_venue->transfer_cost) && !venture_money_is_zero(buy_venue->transfer_cost))
			json_array_add_object_element(legs, arb_leg("fee", buy->data_source_id, buy->venue_key,
				NULL, 0, NULL, buy_venue->transfer_cost, NULL));

		if ((NULL != sell_venue->transfer_cost) && !venture_money_is_zero(sell_venue->transfer_cost))
			json_array_add_object_element(legs, arb_leg("fee", sell->data_source_id, sell->venue_key,
				NULL, 0, NULL, sell_venue->transfer_cost, NULL));
	}

	json_object_set_array_member(opportunity, "legs", legs);

	/* A fee nobody could compute blanks the row and says why: it is
	 * never read as no fee. */
	if (missing->len > 0)
	{
		array = json_array_new();

		for (i = 0; i < missing->len; i++)
			json_array_add_string_element(array, g_ptr_array_index(missing, i));

		json_object_set_array_member(opportunity, "missing", array);
		json_object_set_null_member(opportunity, "net");
		json_object_set_null_member(opportunity, "capital");
		json_object_set_null_member(opportunity, "roi");
	}
	else
	{
		venture_arbitrage_flip_input_init(&input);
		input.buy_cost = buy_amount;
		input.buy_fees = buy_fees;
		input.sell_gross = sell_gross;
		input.sell_fees = sell_fees;
		input.deposit = deposit;
		input.deposit_refundable = refundable;
		input.sale_rate = market->sale_rate;
		input.transfer_cost = transfer;
		input.units = buy->units;
		input.sold_per_day = market->sold_per_day;
		input.share = arb_share(scan);
		input.transit_days = (gdouble)transit_hours / 24.0;

		/* Unwinding a lot that never sells loses its deposit and the
		 * move back. */
		{
			const VentureMoney *parts[2];

			parts[0] = deposit;
			parts[1] = transfer;

			for (i = 0; i < 2; i++)
			{
				VentureMoney *next;

				if (NULL == parts[i])
					continue;

				if (NULL == unwind)
				{
					unwind = venture_money_copy(parts[i]);
					continue;
				}

				next = venture_money_add(unwind, parts[i], error);

				if (NULL == next)
				{
					json_object_unref(opportunity);
					return FALSE;
				}

				venture_money_free(unwind);
				unwind = next;
			}
		}

		input.unwind_loss = unwind;

		if (!venture_arbitrage_flip(&input, &flip, &local_error))
		{
			json_object_unref(opportunity);

			/* A market that never sells is left out and counted:
			 * relisting forever loses every deposit. */
			if (isfinite(market->sale_rate) && (market->sale_rate <= 0.0))
			{
				scan->never_sells++;
				return TRUE;
			}

			g_propagate_error(error, g_steal_pointer(&local_error));
			return FALSE;
		}

		venture_arbitrage_set_money(opportunity, "net", flip.net);
		venture_arbitrage_set_money(opportunity, "capital", flip.capital);
		venture_arbitrage_set_money(opportunity, "listing_loss", flip.listing_loss);
		venture_arbitrage_set_money(opportunity, "ev", flip.ev);
		venture_arbitrage_set_ratio(opportunity, "roi", flip.roi);
		venture_arbitrage_set_ratio(opportunity, "relists", flip.relists);
		venture_arbitrage_set_ratio(opportunity, "lock_days", flip.lock_days);
		venture_arbitrage_set_ratio(opportunity, "roi_per_day", flip.roi_per_day);
		venture_arbitrage_set_ratio(opportunity, "annualized", flip.annualized);

		if (isnan(market->sale_rate) && (NULL != deposit) && !venture_money_is_zero(deposit))
			g_ptr_array_add(warnings, g_strdup("Sale rate unknown: no relists counted"));

		venture_arbitrage_flip_clear(&flip);
	}

	evidence.age_seconds = MAX((gint64)0, scan->now - MIN(buy->taken_at, sell->taken_at));
	evidence.interval_seconds = MAX(buy->interval_seconds, sell->interval_seconds);
	evidence.venues = market->venues;
	evidence.dispersion = market->dispersion;
	evidence.depth = buy->depth;
	venture_arbitrage_set_ratio(opportunity, "confidence", venture_arbitrage_confidence(&evidence));

	array = json_array_new();

	for (i = 0; i < warnings->len; i++)
		json_array_add_string_element(array, g_ptr_array_index(warnings, i));

	json_object_set_array_member(opportunity, "warnings", array);

	/* What to re-ask to find this one again cheaply. */
	if (!json_object_has_member(opportunity, "narrow"))
	{
		JsonObject *narrow = json_object_new();

		json_object_set_string_member(narrow, "instrument", buy->instrument_key);
		json_object_set_object_member(opportunity, "narrow", narrow);
	}

	if (NULL != extra)
	{
		g_autoptr(GList) members = json_object_get_members(extra);
		GList *member;

		for (member = members; NULL != member; member = member->next)
			json_object_set_member(opportunity, member->data,
			                       json_node_copy(json_object_get_member(extra, member->data)));
	}

	venture_arbitrage_scan_add(scan, opportunity);

	return TRUE;
}

/* ==========================================================================
 * Running a scan: sources, the strategy, the filters, the order
 * ========================================================================== */

/* The organization's live sources, the one asked for or all of them. */
static GPtrArray *
arb_sources(
	VentureDatabase	 *database,
	gint64		  organization_id,
	gint64		  data_source_id,
	GError		**error
){
	g_autoptr(VentureQuery) query = NULL;

	query = venture_query_new(VENTURE_TYPE_DATA_SOURCE);
	venture_query_set_organization(query, organization_id);
	venture_query_add_order(query, "name", VENTURE_SORT_ASCENDING, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	venture_query_set_limit(query, VENTURE_ARBITRAGE_SCAN_SOURCES + 1);

	if ((data_source_id > 0) &&
	    !venture_query_add_filter_int(query, "id", VENTURE_FILTER_OP_EQ, data_source_id, error))
		return NULL;

	return venture_database_find(database, query, error);
}

/* A comma list as a vector, or NULL for "every". */
static gchar **
arb_list(
	JsonObject	*options,
	const gchar	*member
){
	const gchar *text;

	text = venture_json_object_get_string(options, member, NULL);

	if (venture_string_is_empty(text))
		return NULL;

	return g_strsplit(text, ",", -1);
}

/*
 * One side's venues narrowed to a venue group's @group, as Deals narrows
 * its pickers: no list is the whole group; a list keeps what is in the
 * group; a list with nothing in it -- left over from the group asked
 * before -- is set aside for the whole group, with a note, rather than
 * answer nothing. Transfer full of @set.
 */
static gchar **
arb_narrow_to_group(
	VentureArbitrageScan	*scan,
	gchar			**set,
	gchar			**group,
	const gchar		*side
){
	g_autoptr(GPtrArray) kept = NULL;
	guint dropped = 0;
	guint i;

	if (NULL == set)
		return g_strdupv(group);

	kept = g_ptr_array_new_with_free_func(g_free);

	for (i = 0; NULL != set[i]; i++)
	{
		if (g_strv_contains((const gchar *const *)group, set[i]))
			g_ptr_array_add(kept, g_strdup(set[i]));
		else
			dropped++;
	}

	g_strfreev(set);

	if (0 == kept->len)
	{
		g_autofree gchar *note = g_strdup_printf("None of %s is in the venue group, so every venue of "
		                                         "the group is used.", side);

		venture_arbitrage_scan_add_note(scan, note);
		return g_strdupv(group);
	}

	if (dropped > 0)
	{
		g_autofree gchar *note = g_strdup_printf("%u of %s %s not in the venue group and %s left out.",
		                                         dropped, side, (1 == dropped) ? "is" : "are",
		                                         (1 == dropped) ? "was" : "were");

		venture_arbitrage_scan_add_note(scan, note);
	}

	g_ptr_array_add(kept, NULL);
	return (gchar **)g_ptr_array_free(g_steal_pointer(&kept), FALSE);
}

static void
arb_exclude(
	GHashTable	*excluded,
	const gchar	*reason
){
	guint count;

	count = GPOINTER_TO_UINT(g_hash_table_lookup(excluded, reason));
	g_hash_table_insert(excluded, (gpointer)reason, GUINT_TO_POINTER(count + 1));
}

/* A filter amount in a row's currency, or NULL when there is no rate. */
static VentureMoney *
arb_bound(
	VentureArbitrageScan	*scan,
	const gchar		*option,
	const gchar		*currency
){
	g_autoptr(VentureMoney) bound = NULL;
	const gchar *text;

	text = venture_json_object_get_string(scan->options, option, NULL);
	bound = (NULL != text) ? venture_money_from_string(text, NULL, NULL) : NULL;

	if (NULL == bound)
		return NULL;

	return venture_arbitrage_scan_convert(scan, bound, currency, NULL);
}

static gboolean
arb_ratio_option(
	VentureArbitrageScan	*scan,
	const gchar		*option,
	gdouble			*out
){
	const gchar *text;
	gint64 ppm;

	text = venture_json_object_get_string(scan->options, option, NULL);

	if (NULL == text)
		return FALSE;

	if (0 == g_strcmp0(option, "min_confidence"))
	{
		*out = g_ascii_strtod(text, NULL);
		return TRUE;
	}

	if (!venture_arbitrage_parse_percent(text, FALSE, &ppm, NULL))
		return FALSE;

	*out = (gdouble)ppm / 1000000.0;

	return TRUE;
}

/*
 * Why a row is left out, or NULL to keep it. A blanked row -- something
 * unquoted -- cannot be judged against a bound, so it is kept only when no
 * bound was asked for, and never counted as a loss.
 */
static const gchar *
arb_judge(
	VentureArbitrageScan	*scan,
	JsonObject		*row,
	gboolean		 keep_all
){
	g_autoptr(VentureMoney) net = NULL;
	g_autoptr(VentureMoney) capital = NULL;
	const gchar *currency;
	gboolean blanked;
	gdouble bound;
	gdouble value;

	blanked = json_object_has_member(row, "missing") &&
	          (json_array_get_length(json_object_get_array_member(row, "missing")) > 0);
	currency = venture_json_object_get_string(row, "currency", NULL);

	/* Stale data is stale whatever it promises. */
	if (json_object_has_member(scan->options, "max_age_hours") &&
	    json_object_has_member(row, "age_seconds") &&
	    (json_object_get_int_member(row, "age_seconds") >
	     venture_json_object_get_int(scan->options, "max_age_hours", 0) * 3600))
		return "stale";

	if (keep_all)
		return NULL;

	if (blanked)
	{
		static const gchar *const bounds[] = {
			"min_profit", "min_roi", "min_sale_rate", "max_capital", "max_buy_pct",
			"min_confidence", NULL
		};
		guint i;

		for (i = 0; NULL != bounds[i]; i++)
			if (json_object_has_member(scan->options, bounds[i]))
				return "unquoted";

		return NULL;
	}

	net = venture_arbitrage_get_money(row, "net");
	capital = venture_arbitrage_get_money(row, "capital");

	if ((NULL == net) || (venture_money_get_amount(net) <= 0))
		return "unprofitable";

	if (json_object_has_member(scan->options, "min_profit"))
	{
		g_autoptr(VentureMoney) least = arb_bound(scan, "min_profit", currency);

		if (NULL == least)
			return "no_rate";

		if (venture_money_compare(net, least) < 0)
			return "min_profit";
	}

	if (json_object_has_member(scan->options, "max_capital"))
	{
		g_autoptr(VentureMoney) most = arb_bound(scan, "max_capital", currency);

		if (NULL == most)
			return "no_rate";

		if ((NULL == capital) || (venture_money_compare(capital, most) > 0))
			return "max_capital";
	}

	if (arb_ratio_option(scan, "min_roi", &bound))
	{
		value = venture_arbitrage_get_ratio(row, "roi");

		if (!isfinite(value) || (value < bound))
			return "min_roi";
	}

	if (arb_ratio_option(scan, "min_sale_rate", &bound))
	{
		value = venture_arbitrage_get_ratio(row, "sale_rate");

		if (!isfinite(value) || (value < bound))
			return "min_sale_rate";
	}

	if (arb_ratio_option(scan, "max_buy_pct", &bound))
	{
		value = venture_arbitrage_get_ratio(row, "buy_vs_sale_avg");

		if (!isfinite(value) || (value > bound))
			return "max_buy_pct";
	}

	if (arb_ratio_option(scan, "min_confidence", &bound))
	{
		value = venture_arbitrage_get_ratio(row, "confidence");

		if (!isfinite(value) || (value < bound))
			return "min_confidence";
	}

	return NULL;
}

typedef struct
{
	const gchar	*sort;
	const gchar	*book;
} ArbOrder;

/* Larger first; a missing figure last whichever way. */
static gint
arb_compare_ratio(
	gdouble	a,
	gdouble	b
){
	if (isfinite(a) && !isfinite(b))
		return -1;

	if (!isfinite(a) && isfinite(b))
		return 1;

	if (!isfinite(a) && !isfinite(b))
		return 0;

	return (a > b) ? -1 : ((a < b) ? 1 : 0);
}

static gint
arb_compare_rows(
	gconstpointer	a,
	gconstpointer	b,
	gpointer	data
){
	JsonObject *left = *(JsonObject *const *)a;
	JsonObject *right = *(JsonObject *const *)b;
	const ArbOrder *order = data;
	const gchar *member;
	gint result;

	member = order->sort;

	/*
	 * Profit and EV are money, and money in two currencies has no order.
	 * Rows group by currency -- the book currency first -- and order by
	 * amount within it; a ratio sort crosses currencies freely.
	 */
	if ((0 == g_strcmp0(member, "profit")) || (0 == g_strcmp0(member, "ev")))
	{
		g_autoptr(VentureMoney) x = NULL;
		g_autoptr(VentureMoney) y = NULL;
		const gchar *cx;
		const gchar *cy;

		cx = venture_json_object_get_string(left, "currency", "");
		cy = venture_json_object_get_string(right, "currency", "");

		if (0 != g_strcmp0(cx, cy))
		{
			if (0 == g_strcmp0(cx, order->book))
				return -1;

			if (0 == g_strcmp0(cy, order->book))
				return 1;

			return g_strcmp0(cx, cy);
		}

		x = venture_arbitrage_get_money(left, (0 == g_strcmp0(member, "profit")) ? "net" : "ev");
		y = venture_arbitrage_get_money(right, (0 == g_strcmp0(member, "profit")) ? "net" : "ev");

		if ((NULL != x) && (NULL == y))
			return -1;

		if ((NULL == x) && (NULL != y))
			return 1;

		if ((NULL != x) && (NULL != y) &&
		    (0 == g_strcmp0(venture_money_get_currency(x), venture_money_get_currency(y))))
		{
			result = -venture_money_compare(x, y);

			if (0 != result)
				return result;
		}
	}
	else
	{
		result = arb_compare_ratio(venture_arbitrage_get_ratio(left, member),
		                           venture_arbitrage_get_ratio(right, member));

		if (0 != result)
			return result;
	}

	/* Ties in a stable, readable order. */
	return g_strcmp0(venture_json_object_get_string(left, "key", ""),
	                 venture_json_object_get_string(right, "key", ""));
}

/* "spread: no exchange rate from EUR to GOLD (3 candidates skipped)". */
static void
arb_rate_notes(VentureArbitrageScan *scan)
{
	GHashTableIter iter;
	gpointer key;
	gpointer value;

	g_hash_table_iter_init(&iter, scan->no_rate);

	while (g_hash_table_iter_next(&iter, &key, &value))
	{
		g_auto(GStrv) pair = g_strsplit(key, ">", 2);
		g_autofree gchar *note = NULL;

		note = g_strdup_printf("No exchange rate from %s to %s on file: %u candidate%s skipped. "
		                       "Record an exchange_rate to compare them.", pair[0], pair[1],
		                       GPOINTER_TO_UINT(value), (1 == GPOINTER_TO_UINT(value)) ? "" : "s");
		venture_arbitrage_scan_add_note(scan, note);
	}
}

JsonNode *
venture_arbitrage_scan_run_full(
	VentureContext	 *context,
	gint64		  organization_id,
	JsonObject	 *options,
	gboolean	  keep_all,
	GError		**error
){
	g_autoptr(VentureArbitrageScan) scan = NULL;
	g_autoptr(JsonObject) normalised = NULL;
	g_autoptr(GHashTable) excluded = NULL;
	g_autoptr(GPtrArray) kept = NULL;
	g_autofree gchar *book = NULL;
	VentureArbitrageStrategyRegistry *strategies;
	ArbStrategy *strategy;
	ArbOrder order;
	JsonObject *root;
	JsonObject *counts;
	JsonArray *rows;
	JsonNode *node;
	GHashTableIter iter;
	gpointer key;
	gpointer value;
	gboolean available;
	guint examined;
	guint top;
	guint i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	if (organization_id <= 0)
		organization_id = venture_context_get_default_organization_id(context);

	if (!venture_context_module_enabled(context, "arbitrage"))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG, "The arbitrage module is off");
		return NULL;
	}

	normalised = venture_arbitrage_scan_options_normalise(context, organization_id, options, error);

	if (NULL == normalised)
		return NULL;

	strategies = venture_context_get_arbitrage_strategies(context);
	strategy = arb_strategy_lookup(strategies,
	                               venture_json_object_get_string(normalised, "strategy", "spread"));

	scan = g_new0(VentureArbitrageScan, 1);
	scan->context = context;
	scan->database = venture_context_get_database(context);
	scan->fees = venture_context_get_fee_models(context);
	scan->organization_id = organization_id;
	scan->options = json_object_ref(normalised);
	scan->now = g_get_real_time() / G_USEC_PER_SEC;
	scan->stale_after = venture_marketdata_stale_seconds(context);
	scan->when = g_date_time_new_from_unix_utc(scan->now);
	scan->units = venture_json_object_get_int(normalised, "units", 1);
	scan->buy_venues = arb_list(normalised, "buy_venues");
	scan->sell_venues = arb_list(normalised, "sell_venues");
	/* A store that failed to open is remembered too, as NULL. */
	scan->stores = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, arb_unref_store);
	scan->venues = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, arb_venue_free);
	scan->noted = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	scan->notes = json_array_new();
	scan->rows = g_ptr_array_new_with_free_func((GDestroyNotify)json_object_unref);
	scan->no_rate = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	scan->attrs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, arb_unref_attrs);

	available = TRUE;

#ifndef VENTURE_HAVE_SQLITE
	available = FALSE;
	venture_arbitrage_scan_add_note(scan, "No data sources: this build has no series store "
	                                      "(it was built without SQLite).");
#else
	if (!venture_context_module_enabled(context, "feeds"))
	{
		available = FALSE;
		venture_arbitrage_scan_add_note(scan, "No data sources: market data feeds are off "
		                                      "(feeds.enabled).");
	}
#endif

	/* With feeds off their records' table may not exist at all: there is
	 * nothing to look up, and the answer is the note above. */
	if (available)
		scan->sources = arb_sources(scan->database, organization_id,
		                            venture_json_object_get_int(normalised, "data_source_id", 0), error);
	else
		scan->sources = g_ptr_array_new_with_free_func(g_object_unref);

	if (NULL == scan->sources)
		return NULL;

	if (available && (venture_json_object_get_int(normalised, "data_source_id", 0) > 0) &&
	    (0 == scan->sources->len))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "No data source #%" G_GINT64_FORMAT " in this organization",
		            venture_json_object_get_int(normalised, "data_source_id", 0));
		return NULL;
	}

	if (scan->sources->len > VENTURE_ARBITRAGE_SCAN_SOURCES)
	{
		g_ptr_array_set_size(scan->sources, VENTURE_ARBITRAGE_SCAN_SOURCES);
		venture_arbitrage_scan_add_note(scan, "Only the first 20 data sources by name were "
		                                      "scanned; name one with data_source_id.");
	}

	if (available && (0 == scan->sources->len))
	{
		available = FALSE;
		venture_arbitrage_scan_add_note(scan, "No data sources in this organization yet.");
	}

	/* A venue group narrows both sides to its venues, as on Deals: the
	 * keys it stands for in every source, each connected realm whole.
	 * Judged even with no data, so a group that is not there is still
	 * refused rather than quietly scanning everything. */
	if (!venture_string_is_empty(venture_json_object_get_string(normalised, "venue_group", NULL)))
	{
		g_auto(GStrv) group = NULL;

		if (!venture_marketdata_venue_group_venue_keys(context, organization_id,
		                                               venture_json_object_get_string(normalised,
		                                                                              "venue_group", NULL),
		                                               &group, &scan->venue_group_name, error))
			return NULL;

		if (available && (0 == g_strv_length(group)))
			venture_arbitrage_scan_add_note(scan, "The venue group names no venue any source has, so "
			                                      "nothing is bought or sold.");

		scan->buy_venues = arb_narrow_to_group(scan, scan->buy_venues, group, "buy_venues");
		scan->sell_venues = arb_narrow_to_group(scan, scan->sell_venues, group, "sell_venues");
	}

	if (available && !strategy->scan(scan, strategy->user_data, error))
		return NULL;

	/* --- Judging every candidate the same way --- */

	examined = scan->rows->len;
	excluded = g_hash_table_new(g_str_hash, g_str_equal);
	kept = g_ptr_array_new_with_free_func((GDestroyNotify)json_object_unref);

	for (i = 0; i < scan->rows->len; i++)
	{
		JsonObject *row = g_ptr_array_index(scan->rows, i);
		const gchar *reason = arb_judge(scan, row, keep_all);

		if (NULL != reason)
		{
			arb_exclude(excluded, reason);
			continue;
		}

		g_ptr_array_add(kept, json_object_ref(row));
	}

	if (scan->never_sells > 0)
	{
		g_autofree gchar *note = g_strdup_printf("%u candidate%s never sell%s (a sale rate of 0) and "
		                                         "%s left out.", scan->never_sells,
		                                         (1 == scan->never_sells) ? "" : "s",
		                                         (1 == scan->never_sells) ? "s" : "",
		                                         (1 == scan->never_sells) ? "was" : "were");

		venture_arbitrage_scan_add_note(scan, note);
		g_hash_table_insert(excluded, (gpointer)"never_sells", GUINT_TO_POINTER(scan->never_sells));
	}

	arb_rate_notes(scan);

	book = venture_database_get_book_currency(scan->database, organization_id);
	order.sort = venture_json_object_get_string(normalised, "sort", "profit");
	order.book = book;
	g_ptr_array_sort_with_data(kept, arb_compare_rows, &order);

	top = (guint)venture_json_object_get_int(normalised, "top", 50);

	/* --- The answer --- */

	root = json_object_new();
	json_object_set_boolean_member(root, "available", available);
	json_object_set_string_member(root, "strategy", strategy->name);
	json_object_set_object_member(root, "options", json_object_ref(normalised));

	if (NULL != scan->venue_group_name)
		json_object_set_string_member(root, "venue_group_name", scan->venue_group_name);

	json_object_set_int_member(root, "examined", examined);
	json_object_set_boolean_member(root, "truncated", kept->len > top);
	counts = json_object_new();
	g_hash_table_iter_init(&iter, excluded);

	while (g_hash_table_iter_next(&iter, &key, &value))
		json_object_set_int_member(counts, key, GPOINTER_TO_UINT(value));

	json_object_set_object_member(root, "excluded", counts);
	rows = json_array_new();

	for (i = 0; (i < kept->len) && (i < top); i++)
		json_array_add_object_element(rows, json_object_ref(g_ptr_array_index(kept, i)));

	json_object_set_array_member(root, "rows", rows);
	json_object_set_array_member(root, "notes", json_array_ref(scan->notes));

	/* The providers of the rows kept -- a buy side from one source and a
	 * sell side from another names both -- never of every source the
	 * scan read: a source that answered nothing shown is not on the page. */
	{
		g_autoptr(GPtrArray) attributions = g_ptr_array_new_with_free_func(g_free);

		venture_marketdata_attribution_collect(context, organization_id,
		                                       json_object_get_member(root, "rows"), attributions);
		venture_marketdata_attribution_set(root, attributions);
	}

	node = json_node_new(JSON_NODE_OBJECT);
	json_node_take_object(node, root);

	return node;
}

JsonNode *
venture_arbitrage_scan_run(
	VentureContext	 *context,
	gint64		  organization_id,
	JsonObject	 *options,
	GError		**error
){
	return venture_arbitrage_scan_run_full(context, organization_id, options, FALSE, error);
}

/* ==========================================================================
 * From an opportunity to a trade
 * ========================================================================== */

/* A short name for the trade: the opportunity's title, cut. */
static gchar *
arb_trade_name(JsonObject *opportunity)
{
	const gchar *title;

	title = venture_json_object_get_string(opportunity, "title",
	                                       venture_json_object_get_string(opportunity, "key", "Attempt"));

	if (g_utf8_strlen(title, -1) <= 120)
		return g_strdup(title);

	return g_utf8_substring(title, 0, 120);
}

/* What the scan expected, verbatim enough for the performance report. */
static JsonObject *
arb_expected(JsonObject *opportunity)
{
	g_autoptr(VentureMoney) net = NULL;
	g_autoptr(VentureMoney) capital = NULL;
	JsonObject *expected;
	JsonArray *profit;

	expected = json_object_new();
	net = venture_arbitrage_get_money(opportunity, "net");
	capital = venture_arbitrage_get_money(opportunity, "capital");
	profit = json_array_new();

	if (NULL != net)
	{
		g_autofree gchar *text = venture_money_to_string(net);

		json_array_add_string_element(profit, text);
	}

	json_object_set_array_member(expected, "profit", profit);

	if (NULL != capital)
	{
		g_autofree gchar *text = venture_money_to_string(capital);

		json_object_set_string_member(expected, "capital", text);
	}

	venture_arbitrage_set_ratio(expected, "roi", venture_arbitrage_get_ratio(opportunity, "roi"));
	venture_arbitrage_set_ratio(expected, "confidence",
	                            venture_arbitrage_get_ratio(opportunity, "confidence"));

	if (json_object_has_member(opportunity, "age_seconds"))
		json_object_set_int_member(expected, "data_age_seconds",
		                           json_object_get_int_member(opportunity, "age_seconds"));

	json_object_set_string_member(expected, "key", venture_json_object_get_string(opportunity, "key", ""));

	return expected;
}

JsonObject *
venture_arbitrage_plan_legs(
	VentureContext		 *context,
	gint64			  organization_id,
	JsonObject		 *opportunity,
	const VentureActor	 *actor,
	GError			**error
){
	g_autoptr(JsonObject) request = NULL;
	g_autofree gchar *name = NULL;
	JsonArray *source_legs;
	JsonArray *legs;
	guint i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	g_return_val_if_fail(NULL != opportunity, NULL);

	if (json_object_has_member(opportunity, "missing") &&
	    (json_array_get_length(json_object_get_array_member(opportunity, "missing")) > 0))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "This opportunity is missing a price or a fee; it cannot be "
		                    "recorded until everything in it is quoted");
		return NULL;
	}

	source_legs = json_object_has_member(opportunity, "legs")
		? json_object_get_array_member(opportunity, "legs") : NULL;

	if ((NULL == source_legs) || (0 == json_array_get_length(source_legs)))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "This opportunity names no legs to plan");
		return NULL;
	}

	legs = json_array_new();
	request = json_object_new();
	json_object_set_array_member(request, "legs", legs);

	for (i = 0; i < json_array_get_length(source_legs); i++)
	{
		JsonObject *source = json_array_get_object_element(source_legs, i);
		JsonObject *leg = json_object_new();
		static const gchar *const copied[] = { "kind", "quantity", "unit_price", "amount", "fees",
		                                       "notes", NULL };
		const gchar *venue_key;
		const gchar *instrument_key;
		gint64 source_id;
		guint j;

		json_array_add_object_element(legs, leg);
		json_object_set_string_member(leg, "status", "planned");

		for (j = 0; NULL != copied[j]; j++)
			if (json_object_has_member(source, copied[j]))
				json_object_set_member(leg, copied[j],
				                       json_node_copy(json_object_get_member(source, copied[j])));

		source_id = venture_json_object_get_int(source, "data_source_id", 0);
		venue_key = venture_json_object_get_string(source, "venue_key", NULL);
		instrument_key = venture_json_object_get_string(source, "instrument_key", NULL);

		/* A leg that already names its records keeps them. */
		if (json_object_has_member(source, "venue_id"))
			json_object_set_int_member(leg, "venue_id", venture_json_object_get_int(source, "venue_id", 0));
		else if ((source_id > 0) && (NULL != venue_key))
		{
			g_autoptr(VentureEntity) venue = NULL;

			if (!venture_marketdata_promote_venue(context, organization_id, source_id, venue_key,
			                                      actor, &venue, error))
				return NULL;

			json_object_set_int_member(leg, "venue_id", venture_entity_get_id(venue));
		}

		if (json_object_has_member(source, "instrument_id"))
			json_object_set_int_member(leg, "instrument_id",
			                           venture_json_object_get_int(source, "instrument_id", 0));
		else if ((source_id > 0) && (NULL != instrument_key))
		{
			g_autoptr(VentureEntity) instrument = NULL;

			if (!venture_marketdata_promote_instrument(context, organization_id, source_id,
			                                           instrument_key, actor, &instrument, error))
				return NULL;

			json_object_set_int_member(leg, "instrument_id", venture_entity_get_id(instrument));
		}
	}

	name = arb_trade_name(opportunity);
	json_object_set_string_member(request, "name", name);
	json_object_set_string_member(request, "strategy",
	                              venture_json_object_get_string(opportunity, "strategy", ""));
	json_object_set_object_member(request, "expected", arb_expected(opportunity));

	return g_steal_pointer(&request);
}

JsonObject *
venture_arbitrage_plan(
	VentureContext		 *context,
	gint64			  organization_id,
	JsonObject		 *options,
	const gchar		 *key,
	const VentureActor	 *actor,
	JsonObject		**out_opportunity,
	GError			**error
){
	g_autoptr(JsonObject) asked = NULL;
	g_autoptr(JsonNode) answer = NULL;
	g_autoptr(GList) members = NULL;
	ArbStrategy *strategy;
	JsonObject *found;
	JsonArray *rows;
	GList *member;
	guint i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	if (venture_string_is_empty(key))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "Which opportunity? key names one");
		return NULL;
	}

	if (organization_id <= 0)
		organization_id = venture_context_get_default_organization_id(context);

	/* The same question, every row: the one wanted must not fall off the
	 * end of a page. */
	asked = json_object_new();

	if (NULL != options)
	{
		members = json_object_get_members(options);

		for (member = members; NULL != member; member = member->next)
			if (0 != g_strcmp0(member->data, "key"))
				json_object_set_member(asked, member->data,
				                       json_node_copy(json_object_get_member(options, member->data)));
	}

	json_object_set_int_member(asked, "top", VENTURE_ARBITRAGE_SCAN_TOP_MAX);
	answer = venture_arbitrage_scan_run(context, organization_id, asked, error);

	if (NULL == answer)
		return NULL;

	rows = json_object_get_array_member(json_node_get_object(answer), "rows");
	found = NULL;

	for (i = 0; (NULL != rows) && (i < json_array_get_length(rows)) && (NULL == found); i++)
	{
		JsonObject *row = json_array_get_object_element(rows, i);

		if (0 == g_strcmp0(venture_json_object_get_string(row, "key", NULL), key))
			found = row;
	}

	if (NULL == found)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "That opportunity is no longer there: the prices moved, or it no longer "
		            "passes the filters (%s). Scan again.", key);
		return NULL;
	}

	strategy = arb_strategy_lookup(venture_context_get_arbitrage_strategies(context),
	                               venture_json_object_get_string(found, "strategy", NULL));

	if (NULL != out_opportunity)
		*out_opportunity = json_object_ref(found);

	if ((NULL != strategy) && (NULL != strategy->plan))
		return strategy->plan(context, organization_id, found, actor, strategy->user_data, error);

	return venture_arbitrage_plan_legs(context, organization_id, found, actor, error);
}

/*
 * The `record` action's parameters for the opportunity @key names: the
 * plan, with the organization the action judges it in. Shared by
 * recording and staging, so an approved proposal performs exactly what a
 * direct record would have.
 */
static GHashTable *
arb_opportunity_parameters(
	VentureContext		 *context,
	gint64			  organization_id,
	JsonObject		 *options,
	const gchar		 *key,
	const VentureActor	 *actor,
	GError			**error
){
	g_autoptr(JsonObject) request = NULL;
	g_autoptr(JsonNode) node = NULL;

	request = venture_arbitrage_plan(context, organization_id, options, key, actor, NULL, error);

	if (NULL == request)
		return NULL;

	json_object_set_int_member(request, "organization_id", organization_id);
	node = json_node_new(JSON_NODE_OBJECT);
	json_node_set_object(node, request);

	return venture_action_parameters_from_json(node, error);
}

VentureEntity *
venture_arbitrage_record_opportunity(
	VentureContext		 *context,
	gint64			  organization_id,
	JsonObject		 *options,
	const gchar		 *key,
	const VentureActor	 *actor,
	VentureUserRole		  role,
	GError			**error
){
	g_autoptr(GHashTable) params = NULL;
	VentureDatabase *database;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	if (organization_id <= 0)
		organization_id = venture_context_get_default_organization_id(context);

	params = arb_opportunity_parameters(context, organization_id, options, key, actor, error);

	if (NULL == params)
		return NULL;

	/* The action, not the function: the organization's roles, a second
	 * actor's approval and the action's own consent operation all
	 * apply, exactly as when a person records a trade by hand. */
	database = venture_context_get_database(context);

	return venture_action_registry_perform(venture_database_get_action_registry(database),
	                                       "arbitrage_trade", 0, "record", params, actor, role, error);
}

VentureConfirmation *
venture_arbitrage_stage_opportunity(
	VentureContext		 *context,
	gint64			  organization_id,
	JsonObject		 *options,
	const gchar		 *key,
	const VentureActor	 *actor,
	VentureUserRole		  role,
	const gchar		 *via,
	GError			**error
){
	g_autoptr(GHashTable) params = NULL;
	g_autoptr(VentureEntity) placeholder = NULL;
	VentureActionRegistry *registry;
	VentureAction *action;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	if (organization_id <= 0)
		organization_id = venture_context_get_default_organization_id(context);

	registry = venture_database_get_action_registry(venture_context_get_database(context));
	action = venture_action_registry_lookup(registry, "arbitrage_trade", "record");

	if (NULL == action)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		                    "Recording a trade is not available: the arbitrage module is off");
		return NULL;
	}

	params = arb_opportunity_parameters(context, organization_id, options, key, actor, error);

	if (NULL == params)
		return NULL;

	/* A type-level action's target is the ID-0 placeholder, exactly as
	 * the generic action route stages one; the queue places it in the
	 * organization the parameters name. */
	placeholder = g_object_new(VENTURE_TYPE_ARBITRAGE_TRADE, NULL);

	return venture_confirmation_store_stage_action(venture_context_get_confirmations(context), action,
	                                               placeholder, params, actor, role,
	                                               (NULL != via) ? via : "rest-api", error);
}

/* ==========================================================================
 * Installation
 * ========================================================================== */

/*
 * A preset is held to what runs: a registered strategy (when written, as
 * a venture type is), its filters read by the scan's own reader, its data
 * source in its own organization.
 */
static gboolean
arb_validate_preset(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	gpointer	  user_data,
	GError		**error
){
	VentureArbitrageStrategyRegistry *strategies;
	g_autoptr(JsonObject) options = NULL;
	g_autoptr(JsonObject) normalised = NULL;
	g_autofree gchar *strategy = NULL;
	g_autofree gchar *old_strategy = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *old_text = NULL;
	gint64 source;

	(void)user_data;

	strategies = g_object_get_data(G_OBJECT(database), VENTURE_ARBITRAGE_STRATEGIES_KEY);
	g_object_get(entity, "strategy", &strategy, "options", &text, "data-source-id", &source, NULL);

	if (NULL != previous)
		g_object_get(previous, "strategy", &old_strategy, "options", &old_text, NULL);

	if (source > 0)
	{
		g_autoptr(VentureEntity) data_source = NULL;

		data_source = venture_database_get(database, VENTURE_TYPE_DATA_SOURCE, source, NULL);

		if ((NULL == data_source) ||
		    (venture_entity_get_organization_id(data_source) != venture_entity_get_organization_id(entity)))
		{
			venture_set_error_validation(error, "Data source", "is not one of this organization's");
			return FALSE;
		}
	}

	if (NULL == strategies)
		return TRUE;

	/* Kept from before, it is not judged again: a plugin's strategy
	 * gone must not make its presets uneditable. */
	if ((NULL != previous) && (0 == g_strcmp0(strategy, old_strategy)) &&
	    (0 == g_strcmp0(text, old_text)))
		return TRUE;

	options = venture_arbitrage_parse_mapping(text, "A preset's filters", "min_roi: 15", error);

	if (NULL == options)
		return FALSE;

	if (!venture_string_is_empty(strategy))
		json_object_set_string_member(options, "strategy", strategy);

	normalised = arb_normalise(NULL, strategies, venture_entity_get_organization_id(entity), options,
	                           error);

	if (NULL == normalised)
	{
		g_prefix_error(error, "Filters: ");
		return FALSE;
	}

	return TRUE;
}

void
venture_arbitrage_engine_install(VentureContext *context)
{
	VentureDatabase *database;

	g_return_if_fail(VENTURE_IS_CONTEXT(context));

	database = venture_context_get_database(context);

	/* References, so the registries outlive a context the tests drop
	 * before the database; the last context over a database wins. */
	g_object_set_data_full(G_OBJECT(database), VENTURE_ARBITRAGE_FEES_KEY,
	                       g_object_ref(venture_context_get_fee_models(context)), g_object_unref);
	g_object_set_data_full(G_OBJECT(database), VENTURE_ARBITRAGE_STRATEGIES_KEY,
	                       g_object_ref(venture_context_get_arbitrage_strategies(context)),
	                       g_object_unref);

	if (NULL != g_object_get_data(G_OBJECT(database), ARB_ENGINE_INSTALLED_KEY))
		return;

	g_object_set_data(G_OBJECT(database), ARB_ENGINE_INSTALLED_KEY, GINT_TO_POINTER(1));
	venture_database_add_save_validator(database, VENTURE_TYPE_VENUE,
	                                    venture_arbitrage_fees_validate_venue, NULL, NULL);
	venture_database_add_save_validator(database, VENTURE_TYPE_ARBITRAGE_STRATEGY,
	                                    arb_validate_preset, NULL, NULL);
}
