/*
 * venture-arbitrage-crafting.c - What every recipe makes, and the flip
 *                                planner's shopping list
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Neither answer computes a price, a fee or a profit of its own. Crafting
 * asks the `transform` strategy (venture-arbitrage-strategies.c) every
 * recipe at once with losses kept, and adds only what a person needs to
 * read it: the recipe's category path and the pickers' choices. The
 * planner reads the trades "Record attempt" and "Add to plan" wrote --
 * planned arbitrage_trade records, whose legs are the plan the strategy
 * made -- and regroups their legs as a shopping list; re-pricing asks
 * each trade's own question again (kept in its `expected`) and compares.
 * A plan is not a second record type beside the trade: a planned trade
 * *is* a line of the plan, and executing its legs is how it is carried
 * out.
 */

#include "venture.h"
#include "arbitrage/venture-arbitrage-engine-private.h"

#include <math.h>
#include <string.h>

/* ==========================================================================
 * Crafting
 * ========================================================================== */

static const gchar *const craft_options[] = {
	"data_source_id", "recipe_category_id", "recipe_id", "buy_realm", "sell_realm", "venue_group",
	"units", "sell_basis", "max_age_hours", "share", "character", "profession", "expansion", "only_known",
	"min_profit", "min_margin", "max_cost", "min_sold_per_day", "recipe_list", NULL
};

/* The options the scan reads as they are given. The realms are read here,
 * and so is which recipes: Crafting chooses them itself (below) and hands
 * the scan the set. */
static const gchar *const craft_passed[] = {
	"data_source_id", "venue_group", "units", "sell_basis", "max_age_hours", "share", NULL
};

/* The most recipes one Crafting question prices. Every one of them is
 * priced -- the scan's own bound of 200 by name is not Crafting's, or the
 * most profitable craft of a thousand known would never be seen -- so
 * past this the question must be narrowed, by profession or category. */
#define CRAFT_RECIPES_MAX (5000)

const gchar *const *
venture_arbitrage_crafting_option_names(void)
{
	return craft_options;
}

/* A picked realm as a comma-separated `buy_venues`/`sell_venues`. */
static gboolean
craft_realm(
	VentureContext	 *context,
	gint64		  organization_id,
	const gchar	 *option,
	const gchar	 *wanted,
	JsonObject	 *scan_options,
	const gchar	 *scan_option,
	JsonObject	 *question,
	GError		**error
){
	g_auto(GStrv) keys = NULL;
	g_autofree gchar *label = NULL;
	g_autofree gchar *joined = NULL;

	if (venture_string_is_empty(wanted))
		return TRUE;

	if (!venture_marketdata_realm_venue_keys(context, organization_id, wanted, &keys, &label, error))
	{
		g_prefix_error(error, "%s: ", option);
		return FALSE;
	}

	joined = g_strjoinv(",", keys);
	json_object_set_string_member(scan_options, scan_option, joined);
	json_object_set_string_member(question, option, label);

	return TRUE;
}

/* A yes/no option: TRUE for 1/true/yes/on, FALSE for 0/false/no/off,
 * @fallback when not given; anything else is refused. */
static gboolean
craft_flag(
	JsonObject	 *question,
	const gchar	 *option,
	gboolean	  fallback,
	gboolean	 *out,
	GError		**error
){
	const gchar *text = venture_json_object_get_string(question, option, NULL);

	*out = fallback;

	if (NULL == text)
		return TRUE;

	if ((0 == g_ascii_strcasecmp(text, "1")) || (0 == g_ascii_strcasecmp(text, "true")) ||
	    (0 == g_ascii_strcasecmp(text, "yes")) || (0 == g_ascii_strcasecmp(text, "on")))
		*out = TRUE;
	else if ((0 == g_ascii_strcasecmp(text, "0")) || (0 == g_ascii_strcasecmp(text, "false")) ||
	         (0 == g_ascii_strcasecmp(text, "no")) || (0 == g_ascii_strcasecmp(text, "off")))
		*out = FALSE;
	else
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "%s is 1 or 0", option);
		return FALSE;
	}

	return TRUE;
}

/* A whole-number option, 0 when not given; anything else is refused. */
static gboolean
craft_id(
	JsonObject	 *question,
	const gchar	 *option,
	gint64		 *out,
	GError		**error
){
	const gchar *text = venture_json_object_get_string(question, option, NULL);

	*out = 0;

	if ((NULL != text) && !g_ascii_string_to_signed(text, 10, 1, G_MAXINT64, out, NULL))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "%s is a record's id", option);
		return FALSE;
	}

	return TRUE;
}

/* A recipe's knowers, from its known-by field: account keys, or an empty
 * list for a recipe nobody was said to know (or one written by hand that
 * is not a list of text). */
static JsonArray *
craft_knowers(VentureEntity *recipe)
{
	g_autofree gchar *text = NULL;
	g_autoptr(JsonNode) node = NULL;
	JsonArray *keys;
	guint i;

	keys = json_array_new();
	g_object_get(recipe, "known-by", &text, NULL);
	node = venture_string_is_empty(text) ? NULL : venture_json_parse(text, NULL);

	if ((NULL == node) || !JSON_NODE_HOLDS_ARRAY(node))
		return keys;

	for (i = 0; i < json_array_get_length(json_node_get_array(node)); i++)
	{
		JsonNode *element = json_array_get_element(json_node_get_array(node), i);

		if (JSON_NODE_HOLDS_VALUE(element) && (G_TYPE_STRING == json_node_get_value_type(element)) &&
		    !venture_string_is_empty(json_node_get_string(element)))
			json_array_add_string_element(keys, json_node_get_string(element));
	}

	return keys;
}

/* The organization's `recipe` categories: id -> parent id, and id -> name. */
typedef struct
{
	GHashTable	*parents;	/* gint64 id -> gint64 parent */
	GHashTable	*names;		/* gint64 id -> gchar* name */
} CraftTree;

static void
craft_tree_clear(CraftTree *tree)
{
	g_clear_pointer(&tree->parents, g_hash_table_unref);
	g_clear_pointer(&tree->names, g_hash_table_unref);
}

static gboolean
craft_tree_load(
	VentureDatabase	 *database,
	gint64		  organization_id,
	CraftTree	 *tree,
	GError		**error
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) found = NULL;
	guint i;

	tree->parents = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);
	tree->names = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);
	query = venture_query_new(VENTURE_TYPE_CATEGORY);
	venture_query_set_organization(query, organization_id);
	venture_query_set_limit(query, 2000);

	if (!venture_query_add_filter_string(query, "applies-to", VENTURE_FILTER_OP_EQ, "recipe", error))
		return FALSE;

	found = venture_database_find(database, query, error);

	if (NULL == found)
		return FALSE;

	for (i = 0; i < found->len; i++)
	{
		VentureEntity *category = g_ptr_array_index(found, i);
		gint64 id = venture_entity_get_id(category);
		gint64 parent = 0;
		gchar *name = NULL;

		g_object_get(category, "name", &name, "parent-id", &parent, NULL);
		g_hash_table_insert(tree->parents, g_memdup2(&id, sizeof(id)), g_memdup2(&parent, sizeof(parent)));
		g_hash_table_insert(tree->names, g_memdup2(&id, sizeof(id)), name);
	}

	return TRUE;
}

/* How deep @id is (1 for a profession), and its ancestor at @level; 0
 * when it is not in the tree. The walk is bounded, as every tree walk. */
static guint
craft_tree_depth(
	CraftTree	*tree,
	gint64		 id,
	guint		 level,
	gint64		*out_ancestor
){
	gint64 chain[VENTURE_CATEGORY_MAX_DEPTH + 1];
	guint depth = 0;
	gint64 at = id;

	*out_ancestor = 0;

	while ((at > 0) && (depth <= VENTURE_CATEGORY_MAX_DEPTH))
	{
		gint64 *parent = g_hash_table_lookup(tree->parents, &at);
		guint seen;

		if (NULL == parent)
			return 0;

		for (seen = 0; seen < depth; seen++)
			if (chain[seen] == at)
				return 0;

		chain[depth++] = at;
		at = *parent;
	}

	if ((level > 0) && (level <= depth))
		*out_ancestor = chain[depth - level];

	return depth;
}

/*
 * The names that are expansions: a category one under a profession that
 * has categories of its own beneath it (profession / expansion / section,
 * as both imports file them). A recipe filed straight under one is in
 * that expansion too; one filed straight under any other second level is
 * in a section of a profession with no expansion given.
 */
static GHashTable *
craft_expansion_names(CraftTree *tree)
{
	GHashTable *names;
	GHashTableIter iter;
	gpointer key;

	names = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_hash_table_iter_init(&iter, tree->parents);

	while (g_hash_table_iter_next(&iter, &key, NULL))
	{
		gint64 expansion = 0;

		if (craft_tree_depth(tree, *(gint64 *)key, 2, &expansion) >= 3)
			g_hash_table_add(names, g_strdup(g_hash_table_lookup(tree->names, &expansion)));
	}

	return names;
}

/* The expansion recipe category @id is in, or NULL. */
static const gchar *
craft_expansion_of(
	CraftTree	*tree,
	GHashTable	*expansions,
	gint64		 id
){
	gint64 expansion = 0;
	guint depth;
	const gchar *name;

	depth = craft_tree_depth(tree, id, 2, &expansion);
	name = (expansion > 0) ? g_hash_table_lookup(tree->names, &expansion) : NULL;

	if ((NULL == name) || (depth < 2) || ((2 == depth) && !g_hash_table_contains(expansions, name)))
		return NULL;

	return name;
}

/* Every category beneath (and at) a category at @level -- 1 a profession,
 * 2 an expansion -- called @name, case folded; an empty set for none. */
static GHashTable *
craft_level_categories(
	VentureDatabase	 *database,
	CraftTree	 *tree,
	guint		  level,
	const gchar	 *name,
	GError		**error
){
	g_autofree gchar *wanted = NULL;
	GHashTable *set;
	GHashTableIter iter;
	gpointer key;
	gpointer value;

	set = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	wanted = g_utf8_casefold(name, -1);
	g_hash_table_iter_init(&iter, tree->names);

	while (g_hash_table_iter_next(&iter, &key, &value))
	{
		g_autofree gchar *folded = (NULL != value) ? g_utf8_casefold(value, -1) : NULL;
		g_autoptr(GArray) beneath = NULL;
		gint64 ancestor = 0;
		guint j;

		if ((0 != g_strcmp0(folded, wanted)) || (craft_tree_depth(tree, *(gint64 *)key, 0, &ancestor) != level))
			continue;

		beneath = venture_category_descendants(database, VENTURE_TYPE_CATEGORY, *(gint64 *)key, TRUE, error);

		if (NULL == beneath)
		{
			g_hash_table_unref(set);
			return NULL;
		}

		for (j = 0; j < beneath->len; j++)
			g_hash_table_add(set, g_memdup2(&g_array_index(beneath, gint64, j), sizeof(gint64)));
	}

	return set;
}

/* Narrows @categories (NULL: not narrowed yet) to @under. */
static void
craft_narrow(
	GHashTable	**categories,
	GHashTable	 *under
){
	GHashTableIter iter;
	gpointer key;

	if (NULL == *categories)
	{
		*categories = g_hash_table_ref(under);
		return;
	}

	g_hash_table_iter_init(&iter, *categories);

	while (g_hash_table_iter_next(&iter, &key, NULL))
		if (!g_hash_table_contains(under, key))
			g_hash_table_iter_remove(&iter);
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(CraftTree, craft_tree_clear)

/* Each of @keys' account attributes (account key -> JsonObject), from the
 * first of the organization's data sources whose store has the account;
 * empty with feeds off or no store. Only to show ranks: never an error. */
static GHashTable *
craft_knower_attrs(
	VentureContext	*context,
	gint64		 organization_id,
	JsonArray	*keys
){
	GHashTable *attrs;

	attrs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)json_object_unref);

#ifdef VENTURE_HAVE_SQLITE
	if ((NULL != keys) && (json_array_get_length(keys) > 0) && venture_context_module_enabled(context, "feeds") &&
	    (NULL != venture_context_get_feeds_service(context)))
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_DATA_SOURCE);
		g_autoptr(GPtrArray) sources = NULL;
		gint64 now = g_get_real_time() / G_USEC_PER_SEC;
		guint i;

		venture_query_set_organization(query, organization_id);
		venture_query_set_limit(query, VENTURE_ARBITRAGE_SCAN_SOURCES);
		sources = venture_database_find(venture_context_get_database(context), query, NULL);

		for (i = 0; (NULL != sources) && (i < sources->len); i++)
		{
			g_autoptr(VentureSeriesStore) store = NULL;
			guint j;

			store = venture_feeds_service_open_reader(venture_context_get_feeds_service(context),
			                                          venture_entity_get_id(g_ptr_array_index(sources, i)),
			                                          NULL);

			for (j = 0; (NULL != store) && (j < json_array_get_length(keys)); j++)
			{
				const gchar *key = json_array_get_string_element(keys, j);
				g_autoptr(VentureSeriesAccountRow) account = NULL;
				g_autoptr(JsonNode) node = NULL;

				if (g_hash_table_contains(attrs, key) ||
				    !venture_series_store_get_account(store, key, now, &account, NULL) || (NULL == account) ||
				    venture_string_is_empty(account->attrs_json))
					continue;

				node = venture_json_parse(account->attrs_json, NULL);

				if ((NULL != node) && JSON_NODE_HOLDS_OBJECT(node))
					g_hash_table_insert(attrs, g_strdup(key), json_object_ref(json_node_get_object(node)));
			}
		}
	}
#else
	(void)context;
	(void)organization_id;
	(void)keys;
#endif

	return attrs;
}

/*
 * A knower's rank for a recipe: "Khaz Algar 65/100" from the account's
 * "profession_tiers:<profession>" entry for @expansion, else (no
 * expansion known) its overall "profession:<profession>" skill and cap.
 * NULL when the account says neither.
 */
static gchar *
craft_knower_rank(
	JsonObject	*attrs,
	const gchar	*profession,
	const gchar	*expansion
){
	g_autofree gchar *member = NULL;
	g_autofree gchar *cap_member = NULL;
	JsonNode *node;

	if ((NULL == attrs) || venture_string_is_empty(profession))
		return NULL;

	if (!venture_string_is_empty(expansion))
	{
		g_autoptr(JsonArray) tiers = NULL;
		guint i;

		member = g_strconcat("profession_tiers:", profession, NULL);
		node = json_object_has_member(attrs, member) ? json_object_get_member(attrs, member) : NULL;
		tiers = venture_marketdata_profession_tiers(((NULL != node) && JSON_NODE_HOLDS_VALUE(node) &&
		                                              (G_TYPE_STRING == json_node_get_value_type(node)))
		                                            ? json_node_get_string(node) : NULL, NULL, NULL);

		for (i = 0; i < json_array_get_length(tiers); i++)
		{
			JsonObject *tier = json_array_get_object_element(tiers, i);

			if (0 == g_strcmp0(venture_json_object_get_string(tier, "label", NULL), expansion))
				return g_strdup(venture_json_object_get_string(tier, "text", NULL));
		}

		return NULL;
	}

	g_clear_pointer(&member, g_free);
	member = g_strconcat("profession:", profession, NULL);
	cap_member = g_strconcat("profession_max:", profession, NULL);
	node = json_object_has_member(attrs, member) ? json_object_get_member(attrs, member) : NULL;

	if ((NULL == node) || !JSON_NODE_HOLDS_VALUE(node) || (G_TYPE_INT64 != json_node_get_value_type(node)))
		return NULL;

	if (json_object_has_member(attrs, cap_member) &&
	    JSON_NODE_HOLDS_VALUE(json_object_get_member(attrs, cap_member)) &&
	    (G_TYPE_INT64 == json_node_get_value_type(json_object_get_member(attrs, cap_member))))
		return g_strdup_printf("%s %" G_GINT64_FORMAT "/%" G_GINT64_FORMAT, profession,
		                       json_node_get_int(node), json_object_get_int_member(attrs, cap_member));

	return g_strdup_printf("%s %" G_GINT64_FORMAT, profession, json_node_get_int(node));
}

/* Orders text for the pickers. */
static gint
craft_compare_text(
	gconstpointer	a,
	gconstpointer	b
){
	return g_utf8_collate(*(const gchar *const *)a, *(const gchar *const *)b);
}

/*
 * The recipes this question prices, chosen in the query that bounds them:
 * one by recipe_id, else those filed under recipe_category_id and under
 * the profession (both, when both are given), at most CRAFT_RECIPES_MAX
 * or a refusal. Then, from those, the ones a character asked for knows,
 * and -- with only_known, on by default whenever any of them is known by
 * somebody -- the ones somebody knows. *@out_known maps each kept recipe's
 * id to its knowers; *@out_characters is every knower of the fetched
 * recipes, for the picker.
 */
static gboolean
craft_choose_recipes(
	VentureDatabase	 *database,
	gint64		  organization_id,
	CraftTree	 *tree,
	JsonObject	 *question,
	GArray		 *listed,
	JsonArray	 *notes,
	GArray		**out_ids,
	GHashTable	**out_known,
	JsonArray	**out_characters,
	GError		**error
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) recipes = NULL;
	g_autoptr(GHashTable) categories = NULL;
	g_autoptr(GHashTable) wanted = NULL;
	g_autoptr(GHashTable) everyone = NULL;
	g_autoptr(GPtrArray) names = NULL;
	g_autoptr(GArray) ids = NULL;
	g_autoptr(GHashTable) known = NULL;
	const gchar *profession;
	const gchar *expansion;
	const gchar *characters;
	gboolean any_known = FALSE;
	gboolean only_known;
	gint64 recipe_id;
	gint64 category_id;
	GHashTableIter iter;
	gpointer key;
	guint i;

	if (!craft_id(question, "recipe_id", &recipe_id, error) ||
	    !craft_id(question, "recipe_category_id", &category_id, error))
		return FALSE;

	profession = venture_json_object_get_string(question, "profession", NULL);
	expansion = venture_json_object_get_string(question, "expansion", NULL);
	characters = venture_json_object_get_string(question, "character", NULL);
	query = venture_query_new(VENTURE_TYPE_RECIPE);
	venture_query_set_organization(query, organization_id);
	venture_query_set_limit(query, CRAFT_RECIPES_MAX + 1);

	if (!venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, error))
		return FALSE;

	if ((recipe_id > 0) && !venture_query_add_filter_int(query, "id", VENTURE_FILTER_OP_EQ, recipe_id, error))
		return FALSE;

	/* A recipe list is a set of ids, narrowed in the same query: the
	 * bound counts only what the list holds. */
	if ((NULL != listed) && (listed->len > 0))
	{
		g_autoptr(GPtrArray) operands = g_ptr_array_new_with_free_func(g_free);

		for (i = 0; i < listed->len; i++)
			g_ptr_array_add(operands, g_strdup_printf("%" G_GINT64_FORMAT, g_array_index(listed, gint64, i)));

		if (!venture_query_add_filter(query, "id", VENTURE_FILTER_OP_IN, operands, error))
			return FALSE;
	}

	/* A category and a profession narrow in the query, so the bound
	 * counts only what they keep. */
	if (category_id > 0)
	{
		g_autoptr(GArray) beneath = venture_category_descendants(database, VENTURE_TYPE_CATEGORY, category_id,
		                                                         TRUE, error);

		if (NULL == beneath)
			return FALSE;

		categories = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);

		for (i = 0; i < beneath->len; i++)
			g_hash_table_add(categories, g_memdup2(&g_array_index(beneath, gint64, i), sizeof(gint64)));
	}

	/* A profession is a top-level recipe category, an expansion one
	 * under a profession; both, with a category, narrow to what all of
	 * them hold. */
	{
		const gchar *levels[2];
		const gchar *labels[2] = { "a profession", "an expansion" };
		guint level;

		levels[0] = profession;
		levels[1] = expansion;

		for (level = 0; level < 2; level++)
		{
			g_autoptr(GHashTable) under = NULL;

			if (venture_string_is_empty(levels[level]))
				continue;

			under = craft_level_categories(database, tree, level + 1, levels[level], error);

			if (NULL == under)
				return FALSE;

			if (0 == g_hash_table_size(under))
			{
				g_autofree gchar *note = g_strdup_printf("No recipe category is %s called %s.", labels[level],
				                                         levels[level]);

				json_array_add_string_element(notes, note);
			}

			craft_narrow(&categories, under);
		}
	}

	if (NULL != categories)
	{
		g_autoptr(GPtrArray) operands = g_ptr_array_new_with_free_func(g_free);

		g_hash_table_iter_init(&iter, categories);

		while (g_hash_table_iter_next(&iter, &key, NULL))
			g_ptr_array_add(operands, g_strdup_printf("%" G_GINT64_FORMAT, *(gint64 *)key));

		if ((operands->len > 0) &&
		    !venture_query_add_filter(query, "category-id", VENTURE_FILTER_OP_IN, operands, error))
			return FALSE;
	}

	/* Nothing filed where it was asked, or an empty list: no recipe,
	 * without asking. */
	if (((NULL != categories) && (0 == g_hash_table_size(categories))) ||
	    ((NULL != listed) && (0 == listed->len)))
		recipes = g_ptr_array_new_with_free_func(g_object_unref);
	else
		recipes = venture_database_find(database, query, error);

	if (NULL == recipes)
		return FALSE;

	if (recipes->len > CRAFT_RECIPES_MAX)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "More than %d recipes answer this question; narrow it to a profession or a recipe "
		            "category", CRAFT_RECIPES_MAX);
		return FALSE;
	}

	/* The characters asked for, comma separated (a key holds none). */
	wanted = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

	if (!venture_string_is_empty(characters))
	{
		g_auto(GStrv) parts = g_strsplit(characters, ",", -1);

		for (i = 0; NULL != parts[i]; i++)
		{
			g_strstrip(parts[i]);

			if ('\0' != parts[i][0])
				g_hash_table_add(wanted, g_strdup(parts[i]));
		}
	}

	everyone = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	known = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, (GDestroyNotify)json_array_unref);
	ids = g_array_new(FALSE, FALSE, sizeof(gint64));

	for (i = 0; i < recipes->len; i++)
	{
		JsonArray *keys = craft_knowers(g_ptr_array_index(recipes, i));
		gint64 id = venture_entity_get_id(g_ptr_array_index(recipes, i));
		guint j;

		any_known = any_known || (json_array_get_length(keys) > 0);

		for (j = 0; j < json_array_get_length(keys); j++)
			g_hash_table_add(everyone, g_strdup(json_array_get_string_element(keys, j)));

		g_hash_table_insert(known, g_memdup2(&id, sizeof(id)), keys);
	}

	/* Only what somebody knows, unless asked otherwise -- and never
	 * where nobody knows anything, which would be an empty page for an
	 * organization that imported recipes from Battle.net alone. A
	 * recipe list is what a person chose, so it shows whole unless
	 * only_known is asked for. */
	if (!craft_flag(question, "only_known", any_known && (NULL == listed), &only_known, error))
		return FALSE;

	json_object_set_string_member(question, "only_known", only_known ? "1" : "0");

	for (i = 0; i < recipes->len; i++)
	{
		gint64 id = venture_entity_get_id(g_ptr_array_index(recipes, i));
		JsonArray *keys = g_hash_table_lookup(known, &id);
		gboolean keep = !only_known || (json_array_get_length(keys) > 0);
		guint j;

		if (keep && (g_hash_table_size(wanted) > 0))
		{
			keep = FALSE;

			for (j = 0; !keep && (j < json_array_get_length(keys)); j++)
				keep = g_hash_table_contains(wanted, json_array_get_string_element(keys, j));
		}

		if (keep)
			g_array_append_val(ids, id);
		else
			g_hash_table_remove(known, &id);
	}

	names = g_ptr_array_new();
	g_hash_table_iter_init(&iter, everyone);

	while (g_hash_table_iter_next(&iter, &key, NULL))
		g_ptr_array_add(names, key);

	g_ptr_array_sort(names, craft_compare_text);
	*out_characters = json_array_new();

	for (i = 0; i < names->len; i++)
		json_array_add_string_element(*out_characters, g_ptr_array_index(names, i));

	*out_ids = g_steal_pointer(&ids);
	*out_known = g_steal_pointer(&known);

	return TRUE;
}

/*
 * A money bound, in @currency: an amount that names its currency ("50
 * GOLD", "50g") is that, and a bare number is in the row's own currency
 * -- what a person typing "50" on a page of gold crafts means. NULL with
 * *@error when the text is no amount; NULL with no error when it names
 * another currency than the row's.
 */
static VentureMoney *
craft_bound(
	const gchar	 *text,
	const gchar	 *currency,
	GError		**error
){
	g_autoptr(VentureMoney) named = NULL;
	g_autoptr(VentureMoney) bare = NULL;

	named = venture_marketdata_parse_amount(text, NULL);

	if (NULL != named)
		return (0 == g_strcmp0(venture_money_get_currency(named), currency)) ? g_steal_pointer(&named) : NULL;

	bare = venture_money_from_string(text, currency, error);

	return g_steal_pointer(&bare);
}

/* How a row stands against the question's bounds: NULL when it is kept,
 * else why it is not. A bound that is not an amount is an error. */
static gboolean
craft_judge(
	JsonObject	 *question,
	JsonObject	 *row,
	const gchar	**out_reason,
	GError		**error
){
	static const gchar *const money_bounds[] = { "min_profit", "max_cost", NULL };
	const gchar *currency;
	gboolean blanked;
	guint i;

	*out_reason = NULL;
	blanked = json_object_has_member(row, "missing") &&
	          (json_array_get_length(json_object_get_array_member(row, "missing")) > 0);
	currency = venture_json_object_get_string(row, "currency", NULL);

	for (i = 0; NULL != money_bounds[i]; i++)
	{
		const gchar *text = venture_json_object_get_string(question, money_bounds[i], NULL);
		g_autoptr(VentureMoney) bound = NULL;
		g_autoptr(VentureMoney) figure = NULL;
		g_autoptr(GError) local_error = NULL;

		if (NULL == text)
			continue;

		if (blanked || (NULL == currency))
		{
			*out_reason = "unpriced";
			return TRUE;
		}

		bound = craft_bound(text, currency, &local_error);

		if (NULL != local_error)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "%s is an amount: %s",
			            money_bounds[i], local_error->message);
			return FALSE;
		}

		if (NULL == bound)
		{
			*out_reason = "other_currency";
			return TRUE;
		}

		figure = venture_arbitrage_get_money(row, (0 == i) ? "net" : "cost");

		if ((NULL == figure) ||
		    (0 != g_strcmp0(venture_money_get_currency(figure), venture_money_get_currency(bound))))
		{
			*out_reason = "unpriced";
			return TRUE;
		}

		if ((0 == i) ? (venture_money_compare(figure, bound) < 0) : (venture_money_compare(figure, bound) > 0))
		{
			*out_reason = money_bounds[i];
			return TRUE;
		}
	}

	/* The margin, a percent; sold per day, a number of units. */
	{
		const gchar *text = venture_json_object_get_string(question, "min_margin", NULL);

		if (NULL != text)
		{
			gint64 ppm;
			gdouble margin;

			if (!venture_arbitrage_parse_percent(text, TRUE, &ppm, error))
			{
				g_prefix_error(error, "min_margin: ");
				return FALSE;
			}

			margin = venture_arbitrage_get_ratio(row, "margin");

			if (blanked || isnan(margin))
			{
				*out_reason = "unpriced";
				return TRUE;
			}

			if (margin < ((gdouble)ppm / 1000000.0))
			{
				*out_reason = "min_margin";
				return TRUE;
			}
		}

		text = venture_json_object_get_string(question, "min_sold_per_day", NULL);

		if (NULL != text)
		{
			gchar *end = NULL;
			gdouble least = g_ascii_strtod(text, &end);
			gdouble sold;

			if ((NULL == end) || ('\0' != *end) || !isfinite(least))
			{
				g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
				                    "min_sold_per_day is a number of units");
				return FALSE;
			}

			sold = venture_arbitrage_get_ratio(row, "sold_per_day");

			if (isnan(sold) || (sold < least))
			{
				*out_reason = isnan(sold) ? "no_velocity" : "min_sold_per_day";
				return TRUE;
			}
		}
	}

	return TRUE;
}

/* The organization's recipe categories, {id, path}, by path. */
static gint
craft_compare_paths(
	gconstpointer	a,
	gconstpointer	b
){
	JsonObject *x = *(JsonObject *const *)a;
	JsonObject *y = *(JsonObject *const *)b;

	return g_utf8_collate(json_object_get_string_member(x, "path"), json_object_get_string_member(y, "path"));
}

static JsonArray *
craft_categories(
	VentureDatabase	*database,
	gint64		 organization_id,
	GHashTable	*paths
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) found = NULL;
	g_autoptr(GPtrArray) sorted = NULL;
	JsonArray *array;
	guint i;

	array = json_array_new();
	query = venture_query_new(VENTURE_TYPE_CATEGORY);
	venture_query_set_organization(query, organization_id);
	venture_query_set_limit(query, 2000);

	if (!venture_query_add_filter_string(query, "applies-to", VENTURE_FILTER_OP_EQ, "recipe", NULL))
		return array;

	found = venture_database_find(database, query, NULL);
	sorted = g_ptr_array_new_with_free_func((GDestroyNotify)json_object_unref);

	for (i = 0; (NULL != found) && (i < found->len); i++)
	{
		VentureEntity *category = g_ptr_array_index(found, i);
		gint64 id = venture_entity_get_id(category);
		g_autofree gchar *path = venture_category_path(database, VENTURE_TYPE_CATEGORY, id, NULL);
		JsonObject *object;

		if (NULL == path)
			continue;

		object = json_object_new();
		json_object_set_int_member(object, "id", id);
		json_object_set_string_member(object, "path", path);
		g_ptr_array_add(sorted, object);
		g_hash_table_insert(paths, g_memdup2(&id, sizeof(id)), g_strdup(path));
	}

	g_ptr_array_sort(sorted, craft_compare_paths);

	for (i = 0; i < sorted->len; i++)
		json_array_add_object_element(array, json_object_ref(g_ptr_array_index(sorted, i)));

	return array;
}

JsonNode *
venture_arbitrage_crafting(
	VentureContext	 *context,
	gint64		  organization_id,
	JsonObject	 *asked,
	GError		**error
){
	g_autoptr(JsonObject) scan_options = NULL;
	g_autoptr(JsonObject) question = NULL;
	g_autoptr(JsonNode) answer = NULL;
	g_autoptr(JsonNode) choices = NULL;
	g_autoptr(JsonNode) groups = NULL;
	g_autoptr(GHashTable) paths = NULL;
	g_autoptr(GHashTable) unpriced = NULL;
	g_autoptr(GHashTable) known = NULL;
	g_autoptr(GHashTable) dropped = NULL;
	g_autoptr(GArray) recipe_ids = NULL;
	g_autoptr(GList) members = NULL;
	g_autoptr(JsonArray) notes = NULL;
	g_autoptr(JsonArray) characters = NULL;
	g_autoptr(GHashTable) expansions = NULL;
	g_autoptr(GHashTable) knower_attrs = NULL;
	g_autoptr(GArray) listed = NULL;
	g_autoptr(JsonArray) listed_lists = NULL;
	g_autoptr(GHashTable) memberships = NULL;
	g_auto(CraftTree) tree = { NULL, NULL };
	VentureDatabase *database;
	JsonObject *root;
	JsonArray *rows;
	JsonArray *realms;
	JsonArray *kept;
	JsonArray *shown;
	JsonArray *professions;
	GList *member;
	GHashTableIter iter;
	gpointer key;
	gpointer value;
	guint i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	if (organization_id <= 0)
		organization_id = venture_context_get_default_organization_id(context);

	database = venture_context_get_database(context);
	scan_options = json_object_new();
	question = json_object_new();
	members = (NULL != asked) ? json_object_get_members(asked) : NULL;

	/* The question: a name nobody knows is refused, as the scan refuses
	 * one -- a misspelt filter is a filter not applied. */
	for (member = members; NULL != member; member = member->next)
	{
		g_autofree gchar *text = NULL;

		if (!g_strv_contains(craft_options, member->data))
		{
			g_autofree gchar *names = g_strjoinv(", ", (gchar **)craft_options);

			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "Crafting takes %s; %s is not one of them", names, (const gchar *)member->data);
			return NULL;
		}

		text = venture_arbitrage_node_text(json_object_get_member(asked, member->data));

		if (NULL != text)
			g_strstrip(text);

		if (venture_string_is_empty(text))
			continue;

		json_object_set_string_member(question, member->data, text);

		if (g_strv_contains(craft_passed, member->data))
			json_object_set_string_member(scan_options, member->data, text);
	}

	/* Which recipes: chosen here, every one of them priced -- the losses
	 * too, since a craft that loses money is an answer as well. A recipe
	 * list is read first: another organization's is not found. */
	notes = json_array_new();

	if (!venture_string_is_empty(venture_json_object_get_string(question, "recipe_list", NULL)))
	{
		g_autoptr(GString) ids = g_string_new(NULL);

		if (!venture_marketdata_list_ids(context, venture_arbitrage_recipe_list_kind(), organization_id,
		                                 venture_json_object_get_string(question, "recipe_list", NULL), &listed,
		                                 &listed_lists, error))
			return NULL;

		/* Echoed as ids, so a page's links keep the list. */
		for (i = 0; i < json_array_get_length(listed_lists); i++)
			g_string_append_printf(ids, "%s%" G_GINT64_FORMAT, (0 == i) ? "" : ",",
			                       json_object_get_int_member(json_array_get_object_element(listed_lists, i),
			                                                  "id"));

		json_object_set_string_member(question, "recipe_list", ids->str);

		if (0 == listed->len)
			json_array_add_string_element(notes, "The list holds no recipe yet: add one from a Crafting row.");
	}

	if (!craft_tree_load(database, organization_id, &tree, error) ||
	    !craft_choose_recipes(database, organization_id, &tree, question, listed, notes, &recipe_ids, &known,
	                          &characters, error))
	{
		craft_tree_clear(&tree);
		return NULL;
	}

	expansions = craft_expansion_names(&tree);

	json_object_set_string_member(scan_options, "strategy", "transform");
	json_object_set_int_member(scan_options, "top", VENTURE_ARBITRAGE_SCAN_TOP_MAX);

	if (!craft_realm(context, organization_id, "buy_realm",
	                 venture_json_object_get_string(question, "buy_realm", NULL), scan_options, "buy_venues",
	                 question, error) ||
	    !craft_realm(context, organization_id, "sell_realm",
	                 venture_json_object_get_string(question, "sell_realm", NULL), scan_options, "sell_venues",
	                 question, error))
		return NULL;

	answer = venture_arbitrage_scan_run_recipes(context, organization_id, scan_options, TRUE, recipe_ids, error);

	if (NULL == answer)
		return NULL;

	root = json_node_get_object(answer);
	rows = json_object_get_array_member(root, "rows");
	paths = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);
	unpriced = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	json_object_set_array_member(root, "categories", craft_categories(database, organization_id, paths));
	dropped = g_hash_table_new(g_str_hash, g_str_equal);
	shown = json_array_new();

	for (i = 0; i < json_array_get_length(notes); i++)
		json_array_add_string_element(json_object_get_array_member(root, "notes"),
		                              json_array_get_string_element(notes, i));

	/* Each row as the strategy wrote it, its recipe's category path and
	 * who knows it beside it, judged against the question's bounds --
	 * which leave out a recipe with no price, as the scan's do; with no
	 * bound it is kept, last, and says what is unpriced. */
	for (i = 0; (NULL != rows) && (i < json_array_get_length(rows)); i++)
	{
		JsonObject *row = json_array_get_object_element(rows, i);
		gint64 category_id = venture_json_object_get_int(row, "recipe_category_id", 0);
		gint64 recipe_id = venture_json_object_get_int(row, "recipe_id", 0);
		JsonArray *knowers = g_hash_table_lookup(known, &recipe_id);
		const gchar *reason = NULL;
		JsonArray *warnings;
		guint j;

		if (!craft_judge(question, row, &reason, error))
			return NULL;

		if (NULL != reason)
		{
			g_hash_table_insert(dropped, (gpointer)reason,
			                    GUINT_TO_POINTER(GPOINTER_TO_UINT(g_hash_table_lookup(dropped, reason)) + 1));
			continue;
		}

		json_object_set_array_member(row, "known_by", (NULL != knowers) ? json_array_ref(knowers)
		                                                                : json_array_new());

		if ((category_id > 0) && (NULL != craft_expansion_of(&tree, expansions, category_id)))
			json_object_set_string_member(row, "expansion", craft_expansion_of(&tree, expansions, category_id));
		else
			json_object_set_null_member(row, "expansion");

		json_object_set_boolean_member(row, "unpriced", json_object_has_member(row, "missing") &&
		                               (json_array_get_length(json_object_get_array_member(row, "missing")) > 0));
		json_array_add_object_element(shown, json_object_ref(row));

		if (category_id > 0)
		{
			const gchar *path = g_hash_table_lookup(paths, &category_id);
			g_autofree gchar *read = NULL;

			if (NULL == path)
			{
				read = venture_category_path(database, VENTURE_TYPE_CATEGORY, category_id, NULL);
				path = read;
			}

			if (NULL != path)
				json_object_set_string_member(row, "recipe_category", path);
		}

		warnings = json_object_has_member(row, "warnings") ? json_object_get_array_member(row, "warnings")
		                                                    : NULL;

		for (j = 0; (NULL != warnings) && (j < json_array_get_length(warnings)); j++)
		{
			const gchar *warning = json_array_get_string_element(warnings, j);
			JsonNode *sell = json_object_get_member(row, "sell");

			if ((NULL != warning) && (NULL != strstr(warning, "no fees counted")) && (NULL != sell) &&
			    JSON_NODE_HOLDS_OBJECT(sell))
				g_hash_table_add(unpriced, g_strdup(venture_json_object_get_string(json_node_get_object(sell),
				                                                                   "venue_name", "")));
		}
	}

	/* Each knower's rank in the row's profession and expansion, from
	 * their account's attributes, where the source says one. */
	knower_attrs = craft_knower_attrs(context, organization_id, characters);

	for (i = 0; i < json_array_get_length(shown); i++)
	{
		JsonObject *row = json_array_get_object_element(shown, i);
		JsonArray *keys = json_object_get_array_member(row, "known_by");
		JsonArray *knowers = json_array_new();
		g_autofree gchar *profession = NULL;
		const gchar *path = venture_json_object_get_string(row, "recipe_category", NULL);
		guint j;

		if (NULL != path)
		{
			const gchar *cut = strstr(path, " / ");

			profession = (NULL != cut) ? g_strndup(path, (gsize)(cut - path)) : g_strdup(path);
		}

		for (j = 0; j < json_array_get_length(keys); j++)
		{
			const gchar *knower = json_array_get_string_element(keys, j);
			JsonObject *object = json_object_new();
			g_autofree gchar *rank = craft_knower_rank(g_hash_table_lookup(knower_attrs, knower), profession,
			                                           venture_json_object_get_string(row, "expansion", NULL));

			json_object_set_string_member(object, "key", knower);

			if (NULL != rank)
				json_object_set_string_member(object, "rank", rank);
			else
				json_object_set_null_member(object, "rank");

			json_array_add_object_element(knowers, object);
		}

		json_object_set_array_member(row, "knowers", knowers);
	}

	/* The lists each recipe is on, for the row's tags: two reads for
	 * the page. */
	{
		g_autoptr(GArray) shown_ids = g_array_new(FALSE, FALSE, sizeof(gint64));

		for (i = 0; i < json_array_get_length(shown); i++)
		{
			gint64 id = venture_json_object_get_int(json_array_get_object_element(shown, i), "recipe_id", 0);

			if (id > 0)
				g_array_append_val(shown_ids, id);
		}

		memberships = venture_marketdata_list_memberships(context, venture_arbitrage_recipe_list_kind(),
		                                                  organization_id, shown_ids);

		for (i = 0; i < json_array_get_length(shown); i++)
		{
			JsonObject *row = json_array_get_object_element(shown, i);
			gint64 id = venture_json_object_get_int(row, "recipe_id", 0);
			JsonArray *on = g_hash_table_lookup(memberships, &id);

			json_object_set_array_member(row, "recipe_lists", (NULL != on) ? json_array_ref(on) : json_array_new());
		}
	}

	json_object_set_array_member(root, "recipe_list_choices",
	                             venture_marketdata_list_choices(context, venture_arbitrage_recipe_list_kind(),
	                                                             organization_id));

	if (NULL != listed_lists)
		json_object_set_array_member(root, "recipe_list_filter", json_array_ref(listed_lists));

	json_object_set_array_member(root, "rows", shown);
	json_object_set_int_member(root, "recipes", recipe_ids->len);

	/* What the bounds left out, by reason, as the scan's `excluded`. */
	{
		JsonObject *excluded = json_object_get_object_member(root, "excluded");

		g_hash_table_iter_init(&iter, dropped);

		while (g_hash_table_iter_next(&iter, &key, &value))
			json_object_set_int_member(excluded, key, GPOINTER_TO_UINT(value));

		if (g_hash_table_contains(dropped, "unpriced"))
		{
			g_autofree gchar *note = g_strdup_printf("%u recipe%s with no price %s left out by the "
			                                         "bounds asked.",
			                                         GPOINTER_TO_UINT(g_hash_table_lookup(dropped,
			                                                                              "unpriced")),
			                                         (1 == GPOINTER_TO_UINT(g_hash_table_lookup(dropped,
			                                                                                    "unpriced")))
			                                         ? "" : "s",
			                                         (1 == GPOINTER_TO_UINT(g_hash_table_lookup(dropped,
			                                                                                    "unpriced")))
			                                         ? "was" : "were");

			json_array_add_string_element(json_object_get_array_member(root, "notes"), note);
		}
	}

	if (g_hash_table_size(unpriced) > 0)
	{
		g_autofree gchar *note = NULL;
		guint count = g_hash_table_size(unpriced);

		note = g_strdup_printf("%u venue%s sold at here %s no fee model, so the auction house's cut and "
		                       "deposit are not counted there and those crafts read better than they are. "
		                       "Give each venue record its fee model (wow_auction for a World of "
		                       "Warcraft realm).", count, (1 == count) ? "" : "s",
		                       (1 == count) ? "has" : "have");
		json_array_add_string_element(json_object_get_array_member(root, "notes"), note);
	}

	/* The pickers: a realm per connected realm (the region's commodity
	 * market is open from every one of them, so it is no choice), the
	 * venue groups, and the realms the answer used. */
	if (!venture_marketdata_realm_choices(context, organization_id,
	                                      venture_json_object_get_string(question, "venue_group", NULL),
	                                      &choices, error))
		return NULL;

	realms = json_node_get_array(choices);
	kept = json_array_new();

	for (i = 0; i < json_array_get_length(realms); i++)
	{
		JsonObject *choice = json_array_get_object_element(realms, i);

		if (!venture_json_object_get_bool(choice, "region_wide", FALSE))
			json_array_add_object_element(kept, json_object_ref(choice));
	}

	json_object_set_array_member(root, "realm_choices", kept);
	groups = venture_marketdata_venue_group_choices(context, organization_id);
	json_object_set_array_member(root, "venue_groups", json_array_ref(json_node_get_array(groups)));
	json_object_set_object_member(root, "question", json_object_ref(question));
	json_object_set_int_member(root, "stale_after_seconds", venture_marketdata_stale_seconds(context));

	/* The character and profession pickers: everybody who knows one of
	 * the recipes the question could reach, and the top-level recipe
	 * categories (the professions). */
	json_object_set_array_member(root, "character_choices", json_array_ref(characters));
	professions = json_array_new();

	{
		JsonArray *categories = json_object_get_array_member(root, "categories");

		for (i = 0; i < json_array_get_length(categories); i++)
		{
			const gchar *path = json_object_get_string_member(json_array_get_object_element(categories, i),
			                                                  "path");

			if ((NULL != path) && (NULL == strstr(path, " / ")))
				json_array_add_string_element(professions, path);
		}
	}

	json_object_set_array_member(root, "profession_choices", professions);

	{
		g_autoptr(GPtrArray) sorted = g_ptr_array_new();
		JsonArray *offered = json_array_new();
		GHashTableIter names;
		gpointer name;

		g_hash_table_iter_init(&names, expansions);

		while (g_hash_table_iter_next(&names, &name, NULL))
			g_ptr_array_add(sorted, name);

		g_ptr_array_sort(sorted, craft_compare_text);

		for (i = 0; i < sorted->len; i++)
			json_array_add_string_element(offered, g_ptr_array_index(sorted, i));

		json_object_set_array_member(root, "expansion_choices", offered);
	}

	return g_steal_pointer(&answer);
}

/* ==========================================================================
 * Recipe lists
 * ========================================================================== */

const VentureMarketdataListKind *
venture_arbitrage_recipe_list_kind(void)
{
	static VentureMarketdataListKind kind;
	static gsize ready = 0;

	/* The types are registered at run time, so the table is finished
	 * on first use. */
	if (g_once_init_enter(&ready))
	{
		kind.list_type = VENTURE_TYPE_RECIPE_LIST;
		kind.list_label = "recipe list";
		kind.entry_type = VENTURE_TYPE_RECIPE_LIST_ENTRY;
		kind.list_field = "recipe-list-id";
		kind.item_field = "recipe-id";
		g_once_init_leave(&ready, 1);
	}

	return &kind;
}

static gint64
craft_int_property(
	VentureEntity	*entity,
	const gchar	*property
){
	gint64 value = 0;

	if (NULL != entity)
		g_object_get(entity, property, &value, NULL);

	return value;
}

/* A reference written to a record of another organization is refused;
 * a value kept from @previous is left alone, as every reference is. */
static gboolean
craft_same_organization(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	const gchar	 *property,
	GType		  type,
	const gchar	 *label,
	GError		**error
){
	g_autoptr(VentureEntity) target = NULL;
	gint64 id = craft_int_property(entity, property);

	if ((NULL != previous) && (craft_int_property(previous, property) == id))
		return TRUE;

	target = venture_database_get(database, type, id, NULL);

	if ((NULL != target) &&
	    (venture_entity_get_organization_id(target) != venture_entity_get_organization_id(entity)))
	{
		venture_set_error_validation(error, label, "is another organization's");
		return FALSE;
	}

	return TRUE;
}

/*
 * An entry names a list and a recipe of its own organization, once per
 * list: a second entry for the same recipe would be a second tag nobody
 * could tell apart, and removing one would leave the recipe listed.
 */
static gboolean
craft_validate_recipe_list_entry(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	gpointer	  user_data,
	GError		**error
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	gint64 list_id = craft_int_property(entity, "recipe-list-id");
	gint64 recipe_id = craft_int_property(entity, "recipe-id");
	guint i;

	(void)user_data;

	/* NOT_NULL covers strings and times only; a reference reads 0. */
	if (list_id <= 0)
	{
		venture_set_error_validation(error, "Recipe list", "is required");
		return FALSE;
	}

	if (recipe_id <= 0)
	{
		venture_set_error_validation(error, "Recipe", "is required");
		return FALSE;
	}

	if (!craft_same_organization(database, entity, previous, "recipe-list-id", VENTURE_TYPE_RECIPE_LIST,
	                             "Recipe list", error) ||
	    !craft_same_organization(database, entity, previous, "recipe-id", VENTURE_TYPE_RECIPE, "Recipe", error))
		return FALSE;

	/* Once per list, judged when either half is written. */
	if ((NULL != previous) && (craft_int_property(previous, "recipe-list-id") == list_id) &&
	    (craft_int_property(previous, "recipe-id") == recipe_id))
		return TRUE;

	query = venture_query_new(VENTURE_TYPE_RECIPE_LIST_ENTRY);
	venture_query_set_limit(query, 2);

	if (!venture_query_add_filter_int(query, "recipe-list-id", VENTURE_FILTER_OP_EQ, list_id, error) ||
	    !venture_query_add_filter_int(query, "recipe-id", VENTURE_FILTER_OP_EQ, recipe_id, error))
		return FALSE;

	rows = venture_database_find(database, query, error);

	if (NULL == rows)
		return FALSE;

	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *other = g_ptr_array_index(rows, i);

		if (venture_entity_get_id(other) != venture_entity_get_id(entity))
		{
			venture_set_error_validation(error, "Recipe",
				"is already on this list (entry #%" G_GINT64_FORMAT ")", venture_entity_get_id(other));
			return FALSE;
		}
	}

	return TRUE;
}

void
venture_arbitrage_recipe_lists_install(VentureDatabase *database)
{
	g_return_if_fail(VENTURE_IS_DATABASE(database));

	venture_database_add_save_validator(database, VENTURE_TYPE_RECIPE_LIST_ENTRY,
	                                    craft_validate_recipe_list_entry, NULL, NULL);
}

gboolean
venture_arbitrage_recipe_list_add(
	VentureContext		 *context,
	gint64			  organization_id,
	gint64			  list_id,
	const gchar		 *list_name,
	gint64			  recipe_id,
	const VentureActor	 *actor,
	VentureEntity		**out_entry,
	gboolean		 *out_created_list,
	gboolean		 *out_created_entry,
	GError			**error
){
	g_autoptr(VentureEntity) recipe = NULL;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), FALSE);

	if (organization_id <= 0)
		organization_id = venture_context_get_default_organization_id(context);

	/* Another organization's recipe is no recipe, as its list is no
	 * list: NOT_FOUND, never a validation message naming it. */
	if (recipe_id > 0)
		recipe = venture_database_get(venture_context_get_database(context), VENTURE_TYPE_RECIPE, recipe_id, NULL);

	if ((NULL == recipe) || venture_entity_is_deleted(recipe) ||
	    (venture_entity_get_organization_id(recipe) != organization_id))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "No recipe #%" G_GINT64_FORMAT " in this organization", recipe_id);
		return FALSE;
	}

	return venture_marketdata_list_put(context, venture_arbitrage_recipe_list_kind(), organization_id, list_id,
	                                   list_name, recipe_id, actor, out_entry, out_created_list, out_created_entry,
	                                   error);
}

JsonNode *
venture_arbitrage_recipe_lists(
	VentureContext	*context,
	gint64		 organization_id
){
	JsonObject *root;
	JsonNode *node;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	if (organization_id <= 0)
		organization_id = venture_context_get_default_organization_id(context);

	root = json_object_new();
	json_object_set_array_member(root, "recipe_lists",
	                             venture_marketdata_list_choices(context, venture_arbitrage_recipe_list_kind(),
	                                                             organization_id));
	node = json_node_new(JSON_NODE_OBJECT);
	json_node_take_object(node, root);

	return node;
}

/* ==========================================================================
 * The flip planner: regrouping
 * ========================================================================== */

/* Money per currency: one slot per figure the totals add up. */
typedef struct
{
	gchar		*currency;
	VentureMoney	*outlay;
	VentureMoney	*gross;
	VentureMoney	*cut;
	VentureMoney	*deposit;
	VentureMoney	*listing_loss;
	guint		 trades;
} PlanTotals;

static void
plan_totals_free(gpointer data)
{
	PlanTotals *totals = data;

	g_free(totals->currency);
	g_clear_pointer(&totals->outlay, venture_money_free);
	g_clear_pointer(&totals->gross, venture_money_free);
	g_clear_pointer(&totals->cut, venture_money_free);
	g_clear_pointer(&totals->deposit, venture_money_free);
	g_clear_pointer(&totals->listing_loss, venture_money_free);
	g_free(totals);
}

/* One line of a venue's list: an instrument (at a price, when selling). */
typedef struct
{
	gint64		 instrument_id;
	gchar		*instrument_name;
	gchar		*instrument_key;
	gint64		 data_source_id;
	gint64		 quantity;
	VentureMoney	*unit_price;	/* buy: the dearest planned; sell: the price */
	VentureMoney	*amount;
	VentureMoney	*fees;
	GArray		*trades;	/* gint64 */
} PlanLine;

static void
plan_line_free(gpointer data)
{
	PlanLine *line = data;

	g_free(line->instrument_name);
	g_free(line->instrument_key);
	g_clear_pointer(&line->unit_price, venture_money_free);
	g_clear_pointer(&line->amount, venture_money_free);
	g_clear_pointer(&line->fees, venture_money_free);
	g_array_unref(line->trades);
	g_free(line);
}

/* One venue's part of the list. */
typedef struct
{
	gint64		 venue_id;
	gchar		*venue_name;
	gchar		*venue_key;
	gint64		 data_source_id;
	GPtrArray	*lines;		/* PlanLine, in the order first planned */
	GHashTable	*by_key;	/* line key -> PlanLine */
	GHashTable	*totals;	/* currency -> VentureMoney (amount and fees) */
} PlanVenue;

static void
plan_venue_free(gpointer data)
{
	PlanVenue *venue = data;

	g_free(venue->venue_name);
	g_free(venue->venue_key);
	g_ptr_array_unref(venue->lines);
	g_hash_table_unref(venue->by_key);
	g_hash_table_unref(venue->totals);
	g_free(venue);
}

/* *@slot += @part, starting from nothing; a mismatch or an overflow is an
 * error, never a figure left short. */
static gboolean
plan_add(
	VentureMoney		**slot,
	const VentureMoney	 *part,
	GError			**error
){
	VentureMoney *sum;

	if (NULL == part)
		return TRUE;

	if (NULL == *slot)
	{
		*slot = venture_money_copy(part);
		return TRUE;
	}

	sum = venture_money_add(*slot, part, error);

	if (NULL == sum)
		return FALSE;

	venture_money_free(*slot);
	*slot = sum;

	return TRUE;
}

/* A money member written as text ("12.50 GOLD"); NULL when absent. A text
 * that is not money is an error: a plan read short is worse than none. */
static gboolean
plan_money(
	JsonObject	 *object,
	const gchar	 *member,
	VentureMoney	**out,
	GError		**error
){
	const gchar *text;

	*out = NULL;
	text = venture_json_object_get_string(object, member, NULL);

	if (venture_string_is_empty(text))
		return TRUE;

	*out = venture_money_from_string(text, NULL, error);

	if (NULL == *out)
	{
		g_prefix_error(error, "%s: ", member);
		return FALSE;
	}

	return TRUE;
}

static PlanTotals *
plan_totals_for(
	GHashTable	*totals,
	GPtrArray	*order,
	const gchar	*currency
){
	PlanTotals *found = g_hash_table_lookup(totals, currency);

	if (NULL == found)
	{
		found = g_new0(PlanTotals, 1);
		found->currency = g_strdup(currency);
		g_hash_table_insert(totals, g_strdup(currency), found);
		g_ptr_array_add(order, found);
	}

	return found;
}

static PlanVenue *
plan_venue_for(
	GHashTable	*venues,
	GPtrArray	*order,
	JsonObject	*leg
){
	g_autofree gchar *key = NULL;
	PlanVenue *venue;
	gint64 venue_id;

	venue_id = venture_json_object_get_int(leg, "venue_id", 0);
	key = (venue_id > 0)
		? g_strdup_printf("#%" G_GINT64_FORMAT, venue_id)
		: g_strdup_printf("%" G_GINT64_FORMAT "\037%s", venture_json_object_get_int(leg, "data_source_id", 0),
		                  venture_json_object_get_string(leg, "venue_key", ""));
	venue = g_hash_table_lookup(venues, key);

	if (NULL != venue)
		return venue;

	venue = g_new0(PlanVenue, 1);
	venue->venue_id = venue_id;
	venue->venue_name = g_strdup(venture_json_object_get_string(leg, "venue_name",
	                             venture_json_object_get_string(leg, "venue_key", "Unknown venue")));
	venue->venue_key = g_strdup(venture_json_object_get_string(leg, "venue_key", NULL));
	venue->data_source_id = venture_json_object_get_int(leg, "data_source_id", 0);
	venue->lines = g_ptr_array_new_with_free_func(plan_line_free);
	venue->by_key = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	venue->totals = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                      (GDestroyNotify)venture_money_free);
	g_hash_table_insert(venues, g_steal_pointer(&key), venue);
	g_ptr_array_add(order, venue);

	return venue;
}

/*
 * Adds one leg to its venue's list. Buys merge by instrument, keeping the
 * dearest unit price as the most a unit may cost; sells merge by
 * instrument and price, since what to post is a price as well as a count.
 */
static gboolean
plan_add_leg(
	PlanVenue	 *venue,
	JsonObject	 *leg,
	gboolean	  selling,
	gint64		  trade_id,
	VentureMoney	 *unit_price,
	VentureMoney	 *amount,
	VentureMoney	 *fees,
	GError		**error
){
	g_autofree gchar *key = NULL;
	g_autofree gchar *price = NULL;
	PlanLine *line;
	VentureMoney *before;
	gint64 instrument_id;
	gint64 quantity;

	instrument_id = venture_json_object_get_int(leg, "instrument_id", 0);
	quantity = venture_json_object_get_int(leg, "quantity", 0);
	price = (selling && (NULL != unit_price)) ? venture_money_to_string(unit_price) : g_strdup("");
	key = (instrument_id > 0)
		? g_strdup_printf("#%" G_GINT64_FORMAT "\037%s", instrument_id, price)
		: g_strdup_printf("%s\037%s", venture_json_object_get_string(leg, "instrument_key",
		                  venture_json_object_get_string(leg, "notes", "")), price);
	line = g_hash_table_lookup(venue->by_key, key);

	if (NULL == line)
	{
		line = g_new0(PlanLine, 1);
		line->instrument_id = instrument_id;
		line->instrument_name = g_strdup(venture_json_object_get_string(leg, "instrument_name",
		                                 venture_json_object_get_string(leg, "notes", NULL)));
		line->instrument_key = g_strdup(venture_json_object_get_string(leg, "instrument_key", NULL));
		line->data_source_id = venture_json_object_get_int(leg, "data_source_id", venue->data_source_id);
		line->trades = g_array_new(FALSE, FALSE, sizeof(gint64));
		g_hash_table_insert(venue->by_key, g_steal_pointer(&key), line);
		g_ptr_array_add(venue->lines, line);
	}

	line->quantity += MAX((gint64)0, quantity);
	g_array_append_val(line->trades, trade_id);

	if (NULL != unit_price)
	{
		if (NULL == line->unit_price)
			line->unit_price = venture_money_copy(unit_price);
		else if (!selling && (0 == g_strcmp0(venture_money_get_currency(unit_price),
		                                     venture_money_get_currency(line->unit_price))) &&
		         (venture_money_compare(unit_price, line->unit_price) > 0))
		{
			venture_money_free(line->unit_price);
			line->unit_price = venture_money_copy(unit_price);
		}
	}

	if (!plan_add(&line->amount, amount, error) || !plan_add(&line->fees, fees, error))
		return FALSE;

	/* The venue's own total, per currency: what it takes (a buy) or
	 * brings (a sell), its fees with it. */
	if (NULL != amount)
	{
		const gchar *currency = venture_money_get_currency(amount);
		g_autoptr(VentureMoney) sum = NULL;

		before = g_hash_table_lookup(venue->totals, currency);
		sum = (NULL != before) ? venture_money_copy(before) : NULL;

		if (!plan_add(&sum, amount, error))
			return FALSE;

		if ((NULL != fees) && (0 == g_strcmp0(venture_money_get_currency(fees), currency)))
		{
			g_autoptr(VentureMoney) adjusted = NULL;

			adjusted = selling ? venture_money_subtract(sum, fees, error) : venture_money_add(sum, fees, error);

			if (NULL == adjusted)
				return FALSE;

			g_clear_pointer(&sum, venture_money_free);
			sum = g_steal_pointer(&adjusted);
		}

		g_hash_table_insert(venue->totals, g_strdup(currency), g_steal_pointer(&sum));
	}

	return TRUE;
}

static gint
plan_compare_venues(
	gconstpointer	a,
	gconstpointer	b
){
	const PlanVenue *x = *(const PlanVenue *const *)a;
	const PlanVenue *y = *(const PlanVenue *const *)b;

	return g_utf8_collate(x->venue_name, y->venue_name);
}

static gint
plan_compare_lines(
	gconstpointer	a,
	gconstpointer	b
){
	const PlanLine *x = *(const PlanLine *const *)a;
	const PlanLine *y = *(const PlanLine *const *)b;

	return g_utf8_collate((NULL != x->instrument_name) ? x->instrument_name : "",
	                      (NULL != y->instrument_name) ? y->instrument_name : "");
}

static JsonArray *
plan_venues_json(GPtrArray *venues)
{
	JsonArray *array;
	guint i;
	guint j;

	array = json_array_new();
	g_ptr_array_sort(venues, plan_compare_venues);

	for (i = 0; i < venues->len; i++)
	{
		PlanVenue *venue = g_ptr_array_index(venues, i);
		JsonObject *object = json_object_new();
		JsonArray *lines = json_array_new();
		JsonArray *totals = json_array_new();
		g_autoptr(GList) currencies = NULL;
		GList *currency;

		if (venue->venue_id > 0)
			json_object_set_int_member(object, "venue_id", venue->venue_id);

		json_object_set_string_member(object, "venue_name", venue->venue_name);

		if (NULL != venue->venue_key)
			json_object_set_string_member(object, "venue_key", venue->venue_key);

		if (venue->data_source_id > 0)
			json_object_set_int_member(object, "data_source_id", venue->data_source_id);

		g_ptr_array_sort(venue->lines, plan_compare_lines);

		for (j = 0; j < venue->lines->len; j++)
		{
			PlanLine *line = g_ptr_array_index(venue->lines, j);
			JsonObject *one = json_object_new();
			JsonArray *trades = json_array_new();
			guint k;

			if (line->instrument_id > 0)
				json_object_set_int_member(one, "instrument_id", line->instrument_id);

			json_object_set_string_member(one, "instrument_name",
			                              (NULL != line->instrument_name) ? line->instrument_name : "");

			if (NULL != line->instrument_key)
				json_object_set_string_member(one, "instrument_key", line->instrument_key);

			if (line->data_source_id > 0)
				json_object_set_int_member(one, "data_source_id", line->data_source_id);

			json_object_set_int_member(one, "quantity", line->quantity);
			venture_arbitrage_set_money(one, "unit_price", line->unit_price);
			venture_arbitrage_set_money(one, "amount", line->amount);
			venture_arbitrage_set_money(one, "fees", line->fees);

			for (k = 0; k < line->trades->len; k++)
				json_array_add_int_element(trades, g_array_index(line->trades, gint64, k));

			json_object_set_array_member(one, "trades", trades);
			json_array_add_object_element(lines, one);
		}

		currencies = g_hash_table_get_keys(venue->totals);
		currencies = g_list_sort(currencies, (GCompareFunc)g_strcmp0);

		for (currency = currencies; NULL != currency; currency = currency->next)
		{
			JsonObject *total = json_object_new();

			json_object_set_string_member(total, "currency", currency->data);
			venture_arbitrage_set_money(total, "amount", g_hash_table_lookup(venue->totals, currency->data));
			json_array_add_object_element(totals, total);
		}

		json_object_set_array_member(object, "lines", lines);
		json_object_set_array_member(object, "totals", totals);
		json_array_add_object_element(array, object);
	}

	return array;
}

/* A trade's expected deposit or listing loss: a money string, or an array
 * of them (one per currency), as `profit` may be. */
static gboolean
plan_expected(
	JsonObject	 *expected,
	const gchar	 *member,
	GHashTable	 *totals,
	GPtrArray	 *order,
	gboolean	  listing_loss,
	GError		**error
){
	g_autoptr(VentureMoney) money = NULL;
	PlanTotals *slot;

	if ((NULL == expected) || !json_object_has_member(expected, member))
		return TRUE;

	if (!plan_money(expected, member, &money, error))
		return FALSE;

	if (NULL == money)
		return TRUE;

	slot = plan_totals_for(totals, order, venture_money_get_currency(money));

	return plan_add(listing_loss ? &slot->listing_loss : &slot->deposit, money, error);
}

/* What a total is: @a less @b and @c, each optional. */
static VentureMoney *
plan_less(
	const VentureMoney	 *a,
	const VentureMoney	 *b,
	const VentureMoney	 *c,
	const gchar		 *currency,
	GError			**error
){
	g_autoptr(VentureMoney) out = NULL;

	out = (NULL != a) ? venture_money_copy(a) : venture_money_new_for_currency(0, currency);

	if (NULL != b)
	{
		VentureMoney *next = venture_money_subtract(out, b, error);

		if (NULL == next)
			return NULL;

		venture_money_free(out);
		out = next;
	}

	if (NULL != c)
	{
		VentureMoney *next = venture_money_subtract(out, c, error);

		if (NULL == next)
			return NULL;

		venture_money_free(out);
		out = next;
	}

	return g_steal_pointer(&out);
}

static gint
plan_compare_totals(
	gconstpointer	a,
	gconstpointer	b
){
	return g_strcmp0((*(PlanTotals *const *)a)->currency, (*(PlanTotals *const *)b)->currency);
}

JsonObject *
venture_arbitrage_planner_build(
	JsonArray	 *trades,
	GError		**error
){
	g_autoptr(GHashTable) buy_venues = NULL;
	g_autoptr(GHashTable) sell_venues = NULL;
	g_autoptr(GHashTable) fee_venues = NULL;
	g_autoptr(GPtrArray) buy_order = NULL;
	g_autoptr(GPtrArray) sell_order = NULL;
	g_autoptr(GPtrArray) fee_order = NULL;
	g_autoptr(GHashTable) totals = NULL;
	g_autoptr(GPtrArray) totals_order = NULL;
	g_autoptr(JsonObject) out = NULL;
	JsonArray *totals_json;
	guint i;
	guint j;

	buy_venues = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	sell_venues = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	fee_venues = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	buy_order = g_ptr_array_new_with_free_func(plan_venue_free);
	sell_order = g_ptr_array_new_with_free_func(plan_venue_free);
	fee_order = g_ptr_array_new_with_free_func(plan_venue_free);
	totals = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	totals_order = g_ptr_array_new_with_free_func(plan_totals_free);

	for (i = 0; (NULL != trades) && (i < json_array_get_length(trades)); i++)
	{
		JsonObject *trade = json_array_get_object_element(trades, i);
		gint64 trade_id = venture_json_object_get_int(trade, "id", 0);
		JsonNode *expected_node = json_object_get_member(trade, "expected");
		JsonObject *expected = ((NULL != expected_node) && JSON_NODE_HOLDS_OBJECT(expected_node))
			? json_node_get_object(expected_node) : NULL;
		JsonArray *legs = json_object_has_member(trade, "legs") ? json_object_get_array_member(trade, "legs")
		                                                        : NULL;
		g_autoptr(GHashTable) counted = g_hash_table_new(g_str_hash, g_str_equal);

		for (j = 0; (NULL != legs) && (j < json_array_get_length(legs)); j++)
		{
			JsonObject *leg = json_array_get_object_element(legs, j);
			const gchar *kind = venture_json_object_get_string(leg, "kind", "buy");
			const gchar *status = venture_json_object_get_string(leg, "status", "planned");
			g_autoptr(VentureMoney) unit_price = NULL;
			g_autoptr(VentureMoney) amount = NULL;
			g_autoptr(VentureMoney) fees = NULL;
			gboolean buying = (0 == g_strcmp0(kind, "buy")) || (0 == g_strcmp0(kind, "stake"));
			gboolean selling = (0 == g_strcmp0(kind, "sell")) || (0 == g_strcmp0(kind, "payout"));
			gboolean fee = (0 == g_strcmp0(kind, "fee")) || (0 == g_strcmp0(kind, "transfer"));
			PlanTotals *slot;

			/* Only what is still to do: an executed leg is the books'
			 * now, a cancelled or failed one is not happening. */
			if (0 != g_strcmp0(status, "planned"))
				continue;

			if (!buying && !selling && !fee)
				continue;

			if (!plan_money(leg, "unit_price", &unit_price, error) ||
			    !plan_money(leg, "amount", &amount, error) ||
			    !plan_money(leg, "fees", &fees, error))
			{
				g_prefix_error(error, "Trade #%" G_GINT64_FORMAT ": ", trade_id);
				return NULL;
			}

			if (NULL == amount)
				continue;

			slot = plan_totals_for(totals, totals_order, venture_money_get_currency(amount));
			g_hash_table_add(counted, (gpointer)slot->currency);

			if (selling)
			{
				if (!plan_add(&slot->gross, amount, error) || !plan_add(&slot->cut, fees, error) ||
				    !plan_add_leg(plan_venue_for(sell_venues, sell_order, leg), leg, TRUE, trade_id,
				                  unit_price, amount, fees, error))
					return NULL;
			}
			else
			{
				if (!plan_add(&slot->outlay, amount, error) || !plan_add(&slot->outlay, fees, error) ||
				    !plan_add_leg(plan_venue_for(buying ? buy_venues : fee_venues,
				                                 buying ? buy_order : fee_order, leg),
				                  leg, FALSE, trade_id, unit_price, amount, fees, error))
					return NULL;
			}
		}

		if (!plan_expected(expected, "deposit", totals, totals_order, FALSE, error) ||
		    !plan_expected(expected, "listing_loss", totals, totals_order, TRUE, error))
		{
			g_prefix_error(error, "Trade #%" G_GINT64_FORMAT ": ", trade_id);
			return NULL;
		}

		/* A trade counts once in each currency it spends or brings. */
		{
			GHashTableIter iter;
			gpointer currency;

			g_hash_table_iter_init(&iter, counted);

			while (g_hash_table_iter_next(&iter, &currency, NULL))
				((PlanTotals *)g_hash_table_lookup(totals, currency))->trades++;
		}
	}

	out = json_object_new();
	json_object_set_array_member(out, "buy", plan_venues_json(buy_order));
	json_object_set_array_member(out, "sell", plan_venues_json(sell_order));
	json_object_set_array_member(out, "fees", plan_venues_json(fee_order));
	totals_json = json_array_new();
	g_ptr_array_sort(totals_order, plan_compare_totals);

	/* One block per currency, never a sum of two: revenue is the sales
	 * less the cut less the deposits relisting is expected to lose,
	 * profit is that less every coin spent getting there. */
	for (i = 0; i < totals_order->len; i++)
	{
		PlanTotals *slot = g_ptr_array_index(totals_order, i);
		g_autoptr(VentureMoney) revenue = NULL;
		g_autoptr(VentureMoney) profit = NULL;
		JsonObject *total = json_object_new();

		revenue = plan_less(slot->gross, slot->cut, slot->listing_loss, slot->currency, error);
		profit = (NULL != revenue) ? plan_less(revenue, slot->outlay, NULL, slot->currency, error) : NULL;

		if (NULL == profit)
		{
			json_object_unref(total);
			json_array_unref(totals_json);
			g_prefix_error(error, "Totals in %s: ", slot->currency);
			return NULL;
		}

		json_object_set_string_member(total, "currency", slot->currency);
		json_object_set_int_member(total, "trades", slot->trades);
		venture_arbitrage_set_money(total, "outlay", slot->outlay);
		venture_arbitrage_set_money(total, "gross", slot->gross);
		venture_arbitrage_set_money(total, "cut", slot->cut);
		venture_arbitrage_set_money(total, "deposit", slot->deposit);
		venture_arbitrage_set_money(total, "listing_loss", slot->listing_loss);
		venture_arbitrage_set_money(total, "revenue", revenue);
		venture_arbitrage_set_money(total, "profit", profit);
		json_array_add_object_element(totals_json, total);
	}

	json_object_set_array_member(out, "totals", totals_json);

	return g_steal_pointer(&out);
}

/* ==========================================================================
 * The flip planner: reading the plan
 * ========================================================================== */

/* A record's name, read once per answer. */
static gchar *
plan_name_of(
	VentureDatabase	*database,
	GHashTable	*cache,
	GType		 type,
	gint64		 id,
	gchar		**out_key,
	gint64		*out_source
){
	g_autofree gchar *cache_key = NULL;
	gchar **cached;

	*out_key = NULL;
	*out_source = 0;

	if (id <= 0)
		return NULL;

	cache_key = g_strdup_printf("%s\037%" G_GINT64_FORMAT, g_type_name(type), id);
	cached = g_hash_table_lookup(cache, cache_key);

	if (NULL == cached)
	{
		g_autoptr(VentureEntity) record = venture_database_get(database, type, id, NULL);
		gint64 source = 0;

		cached = g_new0(gchar *, 4);

		if (NULL != record)
		{
			cached[0] = venture_entity_get_display_name(record);
			g_object_get(record, "key", &cached[1], "data-source-id", &source, NULL);
			cached[2] = g_strdup_printf("%" G_GINT64_FORMAT, source);
		}

		g_hash_table_insert(cache, g_steal_pointer(&cache_key), cached);
	}

	*out_key = g_strdup(cached[1]);
	*out_source = (NULL != cached[2]) ? g_ascii_strtoll(cached[2], NULL, 10) : 0;

	return g_strdup(cached[0]);
}

static void
plan_money_member(
	JsonObject	*object,
	const gchar	*member,
	VentureEntity	*entity,
	const gchar	*property
){
	VentureMoney *money = NULL;
	g_autofree gchar *text = NULL;

	g_object_get(entity, property, &money, NULL);

	if (NULL == money)
		return;

	text = venture_money_to_string(money);
	json_object_set_string_member(object, member, text);
	venture_money_free(money);
}

/* The planned trades and their legs, as venture_arbitrage_planner_build()
 * reads them. */
static JsonArray *
plan_read(
	VentureDatabase	 *database,
	gint64		  organization_id,
	GError		**error
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureQuery) leg_query = NULL;
	g_autoptr(GPtrArray) trades = NULL;
	g_autoptr(GPtrArray) legs = NULL;
	g_autoptr(GPtrArray) ids = NULL;
	g_autoptr(GHashTable) by_id = NULL;
	g_autoptr(GHashTable) names = NULL;
	JsonArray *array;
	guint i;

	query = venture_query_new(VENTURE_TYPE_ARBITRAGE_TRADE);
	venture_query_set_organization(query, organization_id);
	venture_query_set_limit(query, VENTURE_ARBITRAGE_PLANNER_MAX_TRADES + 1);

	if (!venture_query_add_filter_string(query, "status", VENTURE_FILTER_OP_EQ, "planned", error) ||
	    !venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, error))
		return NULL;

	trades = venture_database_find(database, query, error);

	if (NULL == trades)
		return NULL;

	/* All of the plan or a refusal: a shopping list that leaves trades
	 * out buys short. */
	if (trades->len > VENTURE_ARBITRAGE_PLANNER_MAX_TRADES)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
		            "More than %d trades are planned; finish, abandon or remove some first",
		            VENTURE_ARBITRAGE_PLANNER_MAX_TRADES);
		return NULL;
	}

	array = json_array_new();
	by_id = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	names = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_strfreev);
	ids = g_ptr_array_new_with_free_func(g_free);

	for (i = 0; i < trades->len; i++)
	{
		VentureEntity *trade = g_ptr_array_index(trades, i);
		JsonObject *object = json_object_new();
		g_autofree gchar *name = NULL;
		g_autofree gchar *strategy = NULL;
		g_autofree gchar *expected = NULL;
		g_autoptr(GDateTime) created = NULL;
		gint64 id;

		g_object_get(trade, "name", &name, "strategy", &strategy, "expected", &expected, "created-at",
		             &created, NULL);
		json_object_set_int_member(object, "id", venture_entity_get_id(trade));
		json_object_set_string_member(object, "name", (NULL != name) ? name : "");
		json_object_set_string_member(object, "strategy", (NULL != strategy) ? strategy : "");

		if (NULL != created)
		{
			g_autofree gchar *text = venture_time_to_string(created);

			json_object_set_string_member(object, "created_at", text);
		}

		if (!venture_string_is_empty(expected))
		{
			g_autoptr(JsonNode) node = json_from_string(expected, NULL);

			if ((NULL != node) && JSON_NODE_HOLDS_OBJECT(node))
				json_object_set_member(object, "expected", g_steal_pointer(&node));
		}

		json_object_set_array_member(object, "legs", json_array_new());
		json_array_add_object_element(array, object);
		id = venture_entity_get_id(trade);
		g_hash_table_insert(by_id, g_memdup2(&id, sizeof(id)), object);
		g_ptr_array_add(ids, g_strdup_printf("%" G_GINT64_FORMAT, venture_entity_get_id(trade)));
	}

	if (0 == ids->len)
		return array;

	leg_query = venture_query_new(VENTURE_TYPE_ARBITRAGE_LEG);
	venture_query_set_organization(leg_query, organization_id);
	venture_query_set_limit(leg_query, 0);

	if (!venture_query_add_filter(leg_query, "trade-id", VENTURE_FILTER_OP_IN, ids, error) ||
	    !venture_query_add_order(leg_query, "id", VENTURE_SORT_ASCENDING, error))
	{
		json_array_unref(array);
		return NULL;
	}

	legs = venture_database_find(database, leg_query, error);

	if (NULL == legs)
	{
		json_array_unref(array);
		return NULL;
	}

	for (i = 0; i < legs->len; i++)
	{
		VentureEntity *leg = g_ptr_array_index(legs, i);
		JsonObject *object;
		JsonObject *trade;
		g_autofree gchar *venue_name = NULL;
		g_autofree gchar *venue_key = NULL;
		g_autofree gchar *instrument_name = NULL;
		g_autofree gchar *instrument_key = NULL;
		gint64 trade_id;
		gint64 venue_id;
		gint64 instrument_id;
		gint64 quantity;
		gint64 venue_source;
		gint64 instrument_source;
		gint kind;
		gint status;

		g_object_get(leg, "trade-id", &trade_id, "venue-id", &venue_id, "instrument-id", &instrument_id,
		             "quantity", &quantity, "kind", &kind, "status", &status, NULL);
		trade = g_hash_table_lookup(by_id, &trade_id);

		if (NULL == trade)
			continue;

		object = json_object_new();
		json_object_set_int_member(object, "id", venture_entity_get_id(leg));
		json_object_set_string_member(object, "kind", venture_enum_to_nick(VENTURE_TYPE_ARBITRAGE_LEG_KIND, kind));
		json_object_set_string_member(object, "status",
		                              venture_enum_to_nick(VENTURE_TYPE_ARBITRAGE_LEG_STATUS, status));
		venue_name = plan_name_of(database, names, VENTURE_TYPE_VENUE, venue_id, &venue_key, &venue_source);
		instrument_name = plan_name_of(database, names, VENTURE_TYPE_INSTRUMENT, instrument_id, &instrument_key,
		                               &instrument_source);

		if (venue_id > 0)
			json_object_set_int_member(object, "venue_id", venue_id);

		if (NULL != venue_name)
			json_object_set_string_member(object, "venue_name", venue_name);

		if (NULL != venue_key)
			json_object_set_string_member(object, "venue_key", venue_key);

		if (instrument_id > 0)
			json_object_set_int_member(object, "instrument_id", instrument_id);

		if (NULL != instrument_name)
			json_object_set_string_member(object, "instrument_name", instrument_name);

		if (NULL != instrument_key)
			json_object_set_string_member(object, "instrument_key", instrument_key);

		if ((instrument_source > 0) || (venue_source > 0))
			json_object_set_int_member(object, "data_source_id",
			                           (instrument_source > 0) ? instrument_source : venue_source);

		json_object_set_int_member(object, "quantity", quantity);
		plan_money_member(object, "unit_price", leg, "unit-price");
		plan_money_member(object, "amount", leg, "amount");
		plan_money_member(object, "fees", leg, "fees");

		{
			g_autofree gchar *notes = NULL;

			g_object_get(leg, "notes", &notes, NULL);

			if (!venture_string_is_empty(notes))
				json_object_set_string_member(object, "notes", notes);
		}

		json_array_add_object_element(json_object_get_array_member(trade, "legs"), object);
	}

	return array;
}

/* Whether a row's buy side (or sell side) is stale: a flip says so on its
 * side, a craft on the row. */
static gboolean
plan_side_stale(
	JsonObject	*row,
	const gchar	*side
){
	g_autofree gchar *flat = g_strconcat(side, "_stale", NULL);
	JsonNode *node = json_object_get_member(row, side);

	if ((NULL != node) && JSON_NODE_HOLDS_OBJECT(node) &&
	    json_object_has_member(json_node_get_object(node), "stale"))
		return venture_json_object_get_bool(json_node_get_object(node), "stale", FALSE);

	return venture_json_object_get_bool(row, flat, FALSE);
}

/*
 * Asks one trade's question again and says how it stands. @answers keeps
 * each question's answer, so trades from one question ask once.
 */
static void
plan_reprice(
	VentureContext	*context,
	gint64		 organization_id,
	JsonObject	*trade,
	GHashTable	*answers
){
	g_autoptr(JsonNode) question_node = NULL;
	g_autofree gchar *question_text = NULL;
	JsonObject *reprice;
	JsonObject *expected;
	JsonObject *question;
	JsonNode *answer;
	JsonArray *rows;
	JsonObject *found;
	const gchar *key;
	const gchar *state;
	g_autofree gchar *message = NULL;
	guint i;

	reprice = json_object_new();
	json_object_set_object_member(trade, "reprice", reprice);
	expected = json_object_has_member(trade, "expected") ? json_object_get_object_member(trade, "expected") : NULL;
	question = ((NULL != expected) && json_object_has_member(expected, "question") &&
	            JSON_NODE_HOLDS_OBJECT(json_object_get_member(expected, "question")))
		? json_object_get_object_member(expected, "question") : NULL;
	key = (NULL != expected) ? venture_json_object_get_string(expected, "key", NULL) : NULL;

	/* What it promised, for the comparison. */
	if ((NULL != expected) && json_object_has_member(expected, "profit") &&
	    JSON_NODE_HOLDS_ARRAY(json_object_get_member(expected, "profit")) &&
	    (json_array_get_length(json_object_get_array_member(expected, "profit")) > 0))
	{
		g_autoptr(VentureMoney) planned = venture_money_from_string(
			json_array_get_string_element(json_object_get_array_member(expected, "profit"), 0), NULL, NULL);

		venture_arbitrage_set_money(reprice, "planned_net", planned);
	}

	if ((NULL == question) || venture_string_is_empty(key))
	{
		json_object_set_string_member(reprice, "state", "unknown");
		json_object_set_string_member(reprice, "message", "Recorded without the question that found it, so it "
		                                                  "cannot be asked again; check it by hand.");
		return;
	}

	question_node = json_node_new(JSON_NODE_OBJECT);
	json_node_set_object(question_node, question);
	question_text = json_to_string(question_node, FALSE);
	answer = g_hash_table_lookup(answers, question_text);

	if (NULL == answer)
	{
		g_autoptr(JsonObject) asked = json_object_new();
		g_autoptr(GError) error = NULL;
		g_autoptr(GList) members = json_object_get_members(question);
		GList *member;

		for (member = members; NULL != member; member = member->next)
			json_object_set_member(asked, member->data,
			                       json_node_copy(json_object_get_member(question, member->data)));

		/* Every row, losses kept: a trade that now loses money is the
		 * answer "no longer profitable", not "gone". */
		json_object_set_int_member(asked, "top", VENTURE_ARBITRAGE_SCAN_TOP_MAX);
		answer = venture_arbitrage_scan_run_full(context, organization_id, asked, TRUE, &error);

		if (NULL == answer)
		{
			JsonObject *failed = json_object_new();

			json_object_set_string_member(failed, "error", (NULL != error) ? error->message : "failed");
			answer = json_node_new(JSON_NODE_OBJECT);
			json_node_take_object(answer, failed);
		}

		g_hash_table_insert(answers, g_strdup(question_text), answer);
	}

	if (json_object_has_member(json_node_get_object(answer), "error"))
	{
		json_object_set_string_member(reprice, "state", "gone");
		message = g_strdup_printf("Its question can no longer be asked: %s",
		                          json_object_get_string_member(json_node_get_object(answer), "error"));
		json_object_set_string_member(reprice, "message", message);
		return;
	}

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
		json_object_set_string_member(reprice, "state", "gone");
		json_object_set_string_member(reprice, "message", "No longer there: the listings moved, or a price "
		                                                  "is older than its question allows.");
		return;
	}

	{
		g_autoptr(VentureMoney) net = venture_arbitrage_get_money(found, "net");
		gboolean buy_stale = plan_side_stale(found, "buy");
		gboolean sell_stale = plan_side_stale(found, "sell");

		venture_arbitrage_set_money(reprice, "net", net);
		venture_arbitrage_set_ratio(reprice, "roi", venture_arbitrage_get_ratio(found, "roi"));
		json_object_set_boolean_member(reprice, "buy_stale", buy_stale);
		json_object_set_boolean_member(reprice, "sell_stale", sell_stale);

		if (json_object_has_member(found, "missing") &&
		    (json_array_get_length(json_object_get_array_member(found, "missing")) > 0))
		{
			JsonArray *missing = json_object_get_array_member(found, "missing");
			g_autoptr(GString) text = g_string_new("Unquoted now: ");

			for (i = 0; i < json_array_get_length(missing); i++)
				g_string_append_printf(text, "%s%s", (i > 0) ? "; " : "",
				                       json_array_get_string_element(missing, i));

			state = "unquoted";
			message = g_string_free(g_steal_pointer(&text), FALSE);
		}
		else if ((NULL == net) || (venture_money_get_amount(net) <= 0))
		{
			state = "unprofitable";
			message = g_strdup("No longer profitable on today's prices.");
		}
		else if (buy_stale || sell_stale)
		{
			state = "stale";
			message = g_strdup_printf("Still pays, but the %s price%s old: check the realm before acting.",
			                          (buy_stale && sell_stale) ? "buy and sell" : buy_stale ? "buy" : "sell",
			                          (buy_stale && sell_stale) ? "s are" : " is");
		}
		else
		{
			state = "ok";
			message = g_strdup("Still pays on today's prices.");
		}

		json_object_set_string_member(reprice, "state", state);
		json_object_set_string_member(reprice, "message", message);
	}
}

JsonNode *
venture_arbitrage_planner(
	VentureContext	 *context,
	gint64		  organization_id,
	gboolean	  reprice,
	GError		**error
){
	g_autoptr(JsonArray) trades = NULL;
	g_autoptr(JsonObject) built = NULL;
	g_autoptr(GHashTable) answers = NULL;
	g_autoptr(GList) members = NULL;
	JsonObject *root;
	JsonArray *notes;
	JsonNode *node;
	GList *member;
	guint repriced;
	guint i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	if (!venture_context_module_enabled(context, "arbitrage"))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG, "The arbitrage module is off");
		return NULL;
	}

	if (organization_id <= 0)
		organization_id = venture_context_get_default_organization_id(context);

	trades = plan_read(venture_context_get_database(context), organization_id, error);

	if (NULL == trades)
		return NULL;

	built = venture_arbitrage_planner_build(trades, error);

	if (NULL == built)
		return NULL;

	notes = json_array_new();
	repriced = 0;

	if (reprice)
	{
		answers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)json_node_unref);

		for (i = 0; i < json_array_get_length(trades); i++)
		{
			if (repriced >= VENTURE_ARBITRAGE_PLANNER_MAX_REPRICE)
				break;

			plan_reprice(context, organization_id, json_array_get_object_element(trades, i), answers);
			repriced++;
		}

		if (repriced < json_array_get_length(trades))
		{
			g_autofree gchar *note = g_strdup_printf("Only the newest %d planned trades were priced again.",
			                                         VENTURE_ARBITRAGE_PLANNER_MAX_REPRICE);

			json_array_add_string_element(notes, note);
		}
	}

	if (0 == json_array_get_length(trades))
		json_array_add_string_element(notes, "Nothing is planned. Add a deal, a flip or a craft to the plan "
		                                     "from Deals, Arbitrage or Crafting.");

	root = json_object_new();
	json_object_set_boolean_member(root, "available", TRUE);
	json_object_set_boolean_member(root, "repriced", reprice);
	json_object_set_array_member(root, "trades", json_array_ref(trades));
	members = json_object_get_members(built);

	for (member = members; NULL != member; member = member->next)
		json_object_set_member(root, member->data, json_node_copy(json_object_get_member(built, member->data)));

	json_object_set_array_member(root, "notes", notes);
	node = json_node_new(JSON_NODE_OBJECT);
	json_node_take_object(node, root);

	return node;
}

/* ==========================================================================
 * The flip planner: writing it out, and taking a trade off it
 * ========================================================================== */

static void
plan_csv_money(
	GString		*out,
	JsonObject	*object,
	const gchar	*member
){
	g_autoptr(VentureMoney) money = venture_arbitrage_get_money(object, member);
	g_autofree gchar *text = (NULL != money) ? venture_money_to_string(money) : NULL;

	g_string_append_c(out, ',');
	venture_arbitrage_csv_field(out, text);
}

static void
plan_csv_section(
	GString		*out,
	JsonObject	*answer,
	const gchar	*section
){
	JsonArray *venues;
	guint i;
	guint j;

	venues = json_object_has_member(answer, section) ? json_object_get_array_member(answer, section) : NULL;

	for (i = 0; (NULL != venues) && (i < json_array_get_length(venues)); i++)
	{
		JsonObject *venue = json_array_get_object_element(venues, i);
		JsonArray *lines = json_object_get_array_member(venue, "lines");

		for (j = 0; (NULL != lines) && (j < json_array_get_length(lines)); j++)
		{
			JsonObject *line = json_array_get_object_element(lines, j);
			g_autofree gchar *quantity = g_strdup_printf("%" G_GINT64_FORMAT,
			                                             venture_json_object_get_int(line, "quantity", 0));

			venture_arbitrage_csv_field(out, section);
			g_string_append_c(out, ',');
			venture_arbitrage_csv_field(out, venture_json_object_get_string(venue, "venue_name", ""));
			g_string_append_c(out, ',');
			venture_arbitrage_csv_field(out, venture_json_object_get_string(line, "instrument_name", ""));
			g_string_append_c(out, ',');
			venture_arbitrage_csv_field(out, venture_json_object_get_string(line, "instrument_key", ""));
			g_string_append_c(out, ',');
			venture_arbitrage_csv_field(out, quantity);
			plan_csv_money(out, line, "unit_price");
			plan_csv_money(out, line, "amount");
			plan_csv_money(out, line, "fees");
			g_string_append(out, "\r\n");
		}
	}
}

GBytes *
venture_arbitrage_planner_csv(JsonObject *answer)
{
	g_autoptr(GString) out = NULL;
	JsonArray *totals;
	guint i;

	out = g_string_new("section,venue,instrument,key,quantity,unit_price,amount,fees\r\n");

	if (NULL == answer)
		return g_string_free_to_bytes(g_steal_pointer(&out));

	plan_csv_section(out, answer, "buy");
	plan_csv_section(out, answer, "fees");
	plan_csv_section(out, answer, "sell");
	totals = json_object_has_member(answer, "totals") ? json_object_get_array_member(answer, "totals") : NULL;

	/* A total per currency, its figures in the amount column's place and
	 * named in the instrument's: outlay, revenue after the cut and the
	 * expected lost deposits, profit. */
	for (i = 0; (NULL != totals) && (i < json_array_get_length(totals)); i++)
	{
		JsonObject *total = json_array_get_object_element(totals, i);
		static const gchar *const figures[] = { "outlay", "gross", "cut", "deposit", "listing_loss", "revenue",
		                                        "profit", NULL };
		guint j;

		for (j = 0; NULL != figures[j]; j++)
		{
			g_string_append(out, "total,");
			venture_arbitrage_csv_field(out, venture_json_object_get_string(total, "currency", ""));
			g_string_append_c(out, ',');
			venture_arbitrage_csv_field(out, figures[j]);
			g_string_append(out, ",,");
			plan_csv_money(out, total, figures[j]);
			g_string_append(out, ",\r\n");
		}
	}

	return g_string_free_to_bytes(g_steal_pointer(&out));
}

JsonArray *
venture_arbitrage_planner_export_rows(JsonObject *answer)
{
	g_autoptr(GHashTable) seen = NULL;
	JsonArray *rows;
	JsonArray *venues;
	guint i;
	guint j;

	rows = json_array_new();
	seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	venues = ((NULL != answer) && json_object_has_member(answer, "buy"))
		? json_object_get_array_member(answer, "buy") : NULL;

	for (i = 0; (NULL != venues) && (i < json_array_get_length(venues)); i++)
	{
		JsonArray *lines = json_object_get_array_member(json_array_get_object_element(venues, i), "lines");

		for (j = 0; (NULL != lines) && (j < json_array_get_length(lines)); j++)
		{
			JsonObject *line = json_array_get_object_element(lines, j);
			const gchar *key = venture_json_object_get_string(line, "instrument_key", NULL);
			JsonObject *row;

			if ((NULL == key) || !g_hash_table_add(seen, g_strdup(key)))
				continue;

			row = json_object_new();
			json_object_set_string_member(row, "instrument_key", key);
			json_object_set_string_member(row, "instrument_name",
			                              venture_json_object_get_string(line, "instrument_name", ""));

			if (json_object_has_member(line, "data_source_id"))
				json_object_set_int_member(row, "data_source_id",
				                           venture_json_object_get_int(line, "data_source_id", 0));

			json_array_add_object_element(rows, row);
		}
	}

	return rows;
}

gboolean
venture_arbitrage_planner_remove(
	VentureContext		 *context,
	gint64			  organization_id,
	gint64			  trade_id,
	const VentureActor	 *actor,
	GError			**error
){
	g_autoptr(VentureEntity) trade = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) legs = NULL;
	VentureDatabase *database;
	gint status;
	guint i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), FALSE);

	if (organization_id <= 0)
		organization_id = venture_context_get_default_organization_id(context);

	database = venture_context_get_database(context);
	trade = (trade_id > 0) ? venture_database_get(database, VENTURE_TYPE_ARBITRAGE_TRADE, trade_id, NULL) : NULL;

	if ((NULL == trade) || venture_entity_is_deleted(trade) ||
	    (venture_entity_get_organization_id(trade) != organization_id))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "No planned trade #%" G_GINT64_FORMAT " in this organization", trade_id);
		return FALSE;
	}

	g_object_get(trade, "status", &status, NULL);

	if (VENTURE_ARBITRAGE_TRADE_STATUS_PLANNED != status)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
		            "Trade #%" G_GINT64_FORMAT " is under way or finished: close or abandon it instead",
		            trade_id);
		return FALSE;
	}

	query = venture_query_new(VENTURE_TYPE_ARBITRAGE_LEG);
	venture_query_set_organization(query, organization_id);
	venture_query_set_limit(query, 0);

	if (!venture_query_add_filter_int(query, "trade-id", VENTURE_FILTER_OP_EQ, trade_id, error))
		return FALSE;

	legs = venture_database_find(database, query, error);

	if (NULL == legs)
		return FALSE;

	for (i = 0; i < legs->len; i++)
	{
		gint leg_status;

		g_object_get(g_ptr_array_index(legs, i), "status", &leg_status, NULL);

		if (VENTURE_ARBITRAGE_LEG_STATUS_EXECUTED == leg_status)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
			            "Trade #%" G_GINT64_FORMAT " has an executed leg: close or abandon it instead",
			            trade_id);
			return FALSE;
		}
	}

	if (!venture_database_begin(database, error))
		return FALSE;

	for (i = 0; i < legs->len; i++)
	{
		if (!venture_database_delete(database, g_ptr_array_index(legs, i), actor, error))
		{
			venture_database_rollback(database);
			return FALSE;
		}
	}

	if (!venture_database_delete(database, trade, actor, error))
	{
		venture_database_rollback(database);
		return FALSE;
	}

	return venture_database_commit(database, error);
}
