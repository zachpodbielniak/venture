/*
 * blizzard-recipes.c - Crafting recipes from Battle.net, as recipe records
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The `import_recipes` action on a `blizzard_auctions` data source: it
 * walks Blizzard's profession tree --
 *
 *   /data/wow/profession/index                       the professions
 *   /data/wow/profession/{id}                        their skill tiers
 *   /data/wow/profession/{id}/skill-tier/{tier}      each tier's recipes
 *   /data/wow/recipe/{id}                            reagents and output
 *
 * -- and makes a `recipe` with one `recipe_component` per reagent, so the
 * production module can craft them and the arbitrage `transform` strategy
 * can price them from the auction houses.
 *
 * Recipes name products, and an auction house prices instruments; an
 * item becomes both. Each item the recipe names gets its `instrument`
 * record (promoted from the source's store when the store has seen it,
 * made outright when it has not), and that instrument's product is what
 * the recipe points at. An instrument with no product gets one only when
 * the person running the import asks (`create_products`, with the venture
 * products belong to); otherwise the recipe is skipped and the result
 * says which item needs a product, because inventing a catalogue nobody
 * asked for is worse than an import that stops to ask.
 *
 * This is a main-thread action: it writes the database. It reaches
 * Battle.net through the same request the feeds module freezes for a
 * run (venture_feeds_service_open_request()), with the blocking helper,
 * so the operator's allowlist, the deadline, the body cap and the
 * no-redirect rule hold exactly as they do on the worker. Each request
 * blocks the main loop for at most the deadline; max_recipes bounds how
 * many one call makes, and the cursor in the answer continues the walk.
 *
 * Running it twice changes nothing the first run made: a recipe is found
 * again by its output product and name, a component by its recipe and
 * product, and only a quantity that moved is written.
 *
 * Handed `recipes` -- the list a person's characters know, from
 * TradeSkillMaster by way of tsmctl -- it reads none of that tree and
 * makes exactly those, by the same instrument, product and component
 * path ("Recipes handed in" below).
 */

#include "blizzard.h"

#ifdef VENTURE_HAVE_SQLITE

#include <string.h>

/* The most recipes one call may read, and the default. */
#define BLIZZARD_RECIPES_MAX		(500)
#define BLIZZARD_RECIPES_DEFAULT	(50)

/* What the result lists of the recipes it skipped, at most. */
#define BLIZZARD_RECIPES_REASONS	(10)

/* Where the database's last context is kept, for the action. */
#define BLIZZARD_CONTEXT_KEY		"blizzard-auctions-context"

/* ==========================================================================
 * The walk's state
 * ========================================================================== */

typedef struct
{
	VentureContext		*context;
	VentureDatabase		*database;
	VentureFeedRequest	*request;
	BlizzardFrozen		*frozen;
	const VentureActor	*actor;
	gint64			 organization_id;
	gint64			 source_id;
	gint64			 venture_id;
	gboolean		 create_products;
	gchar			*namespace_;

	/* Where the recipe being read is filed in the professions' book:
	 * the profession, its skill tier (an expansion) and the tier's
	 * category ("Materials"). Borrowed from the walk's JSON. */
	const gchar		*profession_name;
	const gchar		*tier_name;
	const gchar		*category_name;
	GHashTable		*categories;	/* "parent\037name" -> category id */

	guint			 read;
	guint			 created;
	guint			 updated;
	guint			 unchanged;
	guint			 skipped;
	GPtrArray		*reasons;	/* gchar*, the first few */
} BlizzardImport;

static void
blizzard_import_skip(
	BlizzardImport	*import,
	const gchar	*reason
){
	import->skipped++;

	if (import->reasons->len < BLIZZARD_RECIPES_REASONS)
		g_ptr_array_add(import->reasons, g_strdup(reason));
}

/* A GET of a static-namespace address, as JSON; NULL with no error for a
 * status the caller judges. */
static JsonNode *
blizzard_import_get(
	BlizzardImport	 *import,
	const gchar	 *path,
	guint		 *out_status,
	GError		**error
){
	g_autofree gchar *url = NULL;

	url = g_strdup_printf("%s%s?namespace=%s&locale=%s", import->frozen->settings->api_base, path,
	                      import->frozen->settings->static_namespace,
	                      import->frozen->settings->locale);

	return blizzard_get_json(import->request, import->frozen, url, BLIZZARD_COST_DEFAULT,
	                         VENTURE_FEED_HTTP_UNCONDITIONAL, NULL, out_status, NULL, error);
}

/* Like blizzard_import_get(), but anything but a 200 is an error. */
static JsonObject *
blizzard_import_require(
	BlizzardImport	 *import,
	const gchar	 *path,
	const gchar	 *what,
	JsonNode	**out_node,
	GError		**error
){
	GError *local_error = NULL;
	guint status;

	*out_node = blizzard_import_get(import, path, &status, &local_error);

	if (NULL != local_error)
	{
		g_propagate_error(error, local_error);
		return NULL;
	}

	if ((NULL == *out_node) || !JSON_NODE_HOLDS_OBJECT(*out_node))
	{
		g_set_error(error, VENTURE_ERROR, (404 == status) ? VENTURE_ERROR_NOT_FOUND : VENTURE_ERROR_FAILED,
		            "Battle.net answered HTTP %u for %s", status, what);
		return NULL;
	}

	return json_node_get_object(*out_node);
}

/* The ids under @member, each an object with an `id`, in the order given
 * or sorted. */
static GArray *
blizzard_import_ids(
	JsonArray	*array,
	gboolean	 sorted
){
	GArray *ids;
	guint i;

	ids = g_array_new(FALSE, FALSE, sizeof(gint64));

	for (i = 0; (NULL != array) && (i < json_array_get_length(array)); i++)
	{
		JsonNode *element = json_array_get_element(array, i);
		gint64 id;

		if (!JSON_NODE_HOLDS_OBJECT(element))
			continue;

		id = blizzard_int(json_node_get_object(element), "id", 0);

		if (id > 0)
			g_array_append_val(ids, id);
	}

	if (sorted)
	{
		guint a;
		guint b;

		/* A handful of professions and tiers: insertion sort says it. */
		for (a = 1; a < ids->len; a++)
		{
			gint64 value = g_array_index(ids, gint64, a);

			for (b = a; (b > 0) && (g_array_index(ids, gint64, b - 1) > value); b--)
				g_array_index(ids, gint64, b) = g_array_index(ids, gint64, b - 1);

			g_array_index(ids, gint64, b) = value;
		}
	}

	return ids;
}

/* ==========================================================================
 * Records
 * ========================================================================== */

/*
 * The oldest record of @type with @field_a = @value_a (and @field_b =
 * @text_b or @int_b when given), deleted ones included: an import that
 * looked only at live rows would make a second of something deleted.
 */
static gboolean
blizzard_find_one(
	BlizzardImport	 *import,
	GType		  type,
	const gchar	 *field_a,
	gint64		  value_a,
	const gchar	 *field_b,
	const gchar	 *text_b,
	gint64		  int_b,
	VentureEntity	**out,
	GError		**error
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) found = NULL;

	*out = NULL;
	query = venture_query_new(type);
	venture_query_set_organization(query, import->organization_id);
	venture_query_set_include_deleted(query, TRUE);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);

	if (!venture_query_add_filter_int(query, field_a, VENTURE_FILTER_OP_EQ, value_a, error))
		return FALSE;

	if ((NULL != field_b) &&
	    !((NULL != text_b)
	      ? venture_query_add_filter_string(query, field_b, VENTURE_FILTER_OP_EQ, text_b, error)
	      : venture_query_add_filter_int(query, field_b, VENTURE_FILTER_OP_EQ, int_b, error)))
		return FALSE;

	found = venture_database_find(import->database, query, error);

	if (NULL == found)
		return FALSE;

	if (found->len > 0)
		*out = g_object_ref(g_ptr_array_index(found, 0));

	return TRUE;
}

static VentureEntity *
blizzard_new_record(
	BlizzardImport	 *import,
	const gchar	 *type_name,
	GError		**error
){
	VentureEntity *entity;

	entity = venture_entity_registry_create(venture_entity_registry_get_default(), type_name, error);

	if (NULL != entity)
		venture_entity_set_organization_id(entity, import->organization_id);

	return entity;
}

/*
 * The product an item stands for, by way of its instrument: the record
 * filed under this source with the item's key (restored if it was
 * deleted), else one promoted from the store, else one made here.
 * *@out_product is 0, with *@out_reason set, when the item has no product
 * and none may be made.
 */
static gboolean
blizzard_item_product(
	BlizzardImport	 *import,
	gint64		  item_id,
	const gchar	 *item_name,
	gint64		 *out_product,
	gchar		**out_reason,
	GError		**error
){
	g_autoptr(VentureEntity) instrument = NULL;
	g_autoptr(GError) promote_error = NULL;
	g_autofree gchar *key = NULL;
	g_autofree gchar *name = NULL;
	gint64 product_id = 0;

	*out_product = 0;
	key = g_strdup_printf("%" G_GINT64_FORMAT, item_id);

	if (!blizzard_find_one(import, VENTURE_TYPE_INSTRUMENT, "data-source-id", import->source_id,
	                       "key", key, 0, &instrument, error))
		return FALSE;

	if ((NULL != instrument) && venture_entity_is_deleted(instrument) &&
	    !venture_database_restore(import->database, instrument, import->actor, error))
		return FALSE;

	/* The store's row carries what the auctions said: kind, category,
	 * attributes. A key the store has never seen is made by hand. */
	if ((NULL == instrument) &&
	    !venture_marketdata_promote_instrument(import->context, import->organization_id,
	                                           import->source_id, key, import->actor,
	                                           &instrument, &promote_error))
	{
		if (!g_error_matches(promote_error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND))
		{
			g_propagate_error(error, g_steal_pointer(&promote_error));
			return FALSE;
		}

		instrument = blizzard_new_record(import, "instrument", error);

		if (NULL == instrument)
			return FALSE;

		g_object_set(instrument, "name", (NULL != item_name) ? item_name : key, "key", key,
		             "namespace", import->namespace_, "data-source-id", import->source_id, NULL);

		if (!venture_entity_set_field_from_string(instrument, "kind", "item", error) ||
		    !venture_database_save(import->database, instrument, import->actor, error))
			return FALSE;
	}

	g_object_get(instrument, "product-id", &product_id, "name", &name, NULL);

	if (product_id > 0)
	{
		*out_product = product_id;
		return TRUE;
	}

	if (!import->create_products)
	{
		*out_reason = g_strdup_printf("%s (item %" G_GINT64_FORMAT ") has no product: link its "
		                              "instrument to one, or import with create_products",
		                              (NULL != name) ? name : key, item_id);
		return TRUE;
	}

	{
		g_autoptr(VentureEntity) product = blizzard_new_record(import, "product", error);

		if (NULL == product)
			return FALSE;

		g_object_set(product, "name", (NULL != name) ? name : key, "venture-id", import->venture_id,
		             NULL);

		if (!venture_database_save(import->database, product, import->actor, error))
			return FALSE;

		product_id = venture_entity_get_id(product);
	}

	g_object_set(instrument, "product-id", product_id, NULL);

	if (!venture_database_save(import->database, instrument, import->actor, error))
		return FALSE;

	*out_product = product_id;

	return TRUE;
}

/*
 * The `recipe` category @name under @parent_id (0: a top level), made
 * when there is none: a profession, its tier, the tier's section. Found
 * by name, parent and `applies-to`, a live one first; a deleted one is
 * left deleted and another made, since a person deleted it. Remembered
 * for the rest of the walk.
 */
static gboolean
blizzard_category(
	BlizzardImport	 *import,
	gint64		  parent_id,
	const gchar	 *name,
	gint64		 *out_id,
	GError		**error
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) found = NULL;
	g_autoptr(VentureEntity) category = NULL;
	g_autofree gchar *cache_key = NULL;
	gpointer cached;
	guint i;

	*out_id = 0;
	cache_key = g_strdup_printf("%" G_GINT64_FORMAT "\037%s", parent_id, name);

	if (g_hash_table_lookup_extended(import->categories, cache_key, NULL, &cached))
	{
		*out_id = *(gint64 *)cached;
		return TRUE;
	}

	query = venture_query_new(VENTURE_TYPE_CATEGORY);
	venture_query_set_organization(query, import->organization_id);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);

	if (!venture_query_add_filter_string(query, "name", VENTURE_FILTER_OP_EQ, name, error) ||
	    !venture_query_add_filter_string(query, "applies-to", VENTURE_FILTER_OP_EQ, "recipe", error))
		return FALSE;

	found = venture_database_find(import->database, query, error);

	if (NULL == found)
		return FALSE;

	for (i = 0; (i < found->len) && (0 == *out_id); i++)
	{
		gint64 parent = 0;

		g_object_get(g_ptr_array_index(found, i), "parent-id", &parent, NULL);

		if (parent == parent_id)
			*out_id = venture_entity_get_id(g_ptr_array_index(found, i));
	}

	if (0 == *out_id)
	{
		category = blizzard_new_record(import, "category", error);

		if (NULL == category)
			return FALSE;

		g_object_set(category, "name", name, "applies-to", "recipe", NULL);

		if (parent_id > 0)
			g_object_set(category, "parent-id", parent_id, NULL);

		if (!venture_database_save(import->database, category, import->actor, error))
			return FALSE;

		*out_id = venture_entity_get_id(category);
	}

	g_hash_table_insert(import->categories, g_steal_pointer(&cache_key), g_memdup2(out_id, sizeof(gint64)));

	return TRUE;
}

/* Where the recipe being read is filed: profession / tier / section, the
 * tier left out when it is only the profession's name again. 0 when the
 * walk knows none. */
static gboolean
blizzard_recipe_category(
	BlizzardImport	 *import,
	gint64		 *out_id,
	GError		**error
){
	const gchar *levels[3];
	gint64 parent;
	guint n;
	guint i;

	*out_id = 0;
	n = 0;

	if (!venture_string_is_empty(import->profession_name))
		levels[n++] = import->profession_name;

	if (!venture_string_is_empty(import->tier_name) &&
	    (0 != g_strcmp0(import->tier_name, import->profession_name)))
		levels[n++] = import->tier_name;

	if (!venture_string_is_empty(import->category_name))
		levels[n++] = import->category_name;

	parent = 0;

	for (i = 0; i < n; i++)
	{
		if (!blizzard_category(import, parent, levels[i], &parent, error))
			return FALSE;
	}

	*out_id = parent;

	return TRUE;
}

/* One reagent line: the product and the quantity, merged by product. */
typedef struct
{
	gint64	product_id;
	gint64	quantity;
} BlizzardLine;

/* What one recipe came to. */
typedef enum
{
	BLIZZARD_RECIPE_SKIPPED = 0,
	BLIZZARD_RECIPE_CREATED,
	BLIZZARD_RECIPE_UPDATED,
	BLIZZARD_RECIPE_UNCHANGED
} BlizzardRecipeOutcome;

/* One recipe's import, in the caller's transaction: what it came to, and
 * why when it was skipped. FALSE is an error. */
typedef gboolean (*BlizzardRecipeFunc)(BlizzardImport		 *import,
                                       gpointer			  data,
                                       BlizzardRecipeOutcome	 *out_outcome,
                                       gchar			**out_reason,
                                       GError			**error);

/* Adds @count of @product_id to @lines. Two slots of one reagent are one
 * line: the component validator allows one line per product. */
static void
blizzard_lines_add(
	GArray	*lines,
	gint64	 product_id,
	gint64	 count
){
	BlizzardLine line;
	guint j;

	for (j = 0; j < lines->len; j++)
	{
		BlizzardLine *existing = &g_array_index(lines, BlizzardLine, j);

		if (existing->product_id == product_id)
		{
			existing->quantity += count;
			return;
		}
	}

	line.product_id = product_id;
	line.quantity = count;
	g_array_append_val(lines, line);
}

/*
 * The recipe's lines brought to @lines: one per reagent, a quantity
 * brought up to date. A deleted line is left deleted and a new one made
 * beside it. With @prune, a consumed line whose product @lines no longer
 * names is deleted, because the list it came from is the whole recipe; a
 * reusable line (a tool someone added by hand) is left alone either way,
 * and without @prune so is every line the list does not name. *@changed
 * is set when anything was written.
 */
static gboolean
blizzard_write_components(
	BlizzardImport	 *import,
	VentureEntity	 *record,
	GArray		 *lines,
	gboolean	  prune,
	gboolean	 *changed,
	GError		**error
){
	guint i;

	for (i = 0; i < lines->len; i++)
	{
		BlizzardLine *line = &g_array_index(lines, BlizzardLine, i);
		g_autoptr(VentureEntity) component = NULL;
		gint64 stored = 0;

		if (!blizzard_find_one(import, VENTURE_TYPE_RECIPE_COMPONENT, "recipe-id",
		                       venture_entity_get_id(record), "product-id", NULL, line->product_id,
		                       &component, error))
			return FALSE;

		if ((NULL != component) && venture_entity_is_deleted(component))
			g_clear_object(&component);

		if (NULL == component)
		{
			component = blizzard_new_record(import, "recipe_component", error);

			if (NULL == component)
				return FALSE;

			g_object_set(component, "recipe-id", venture_entity_get_id(record),
			             "product-id", line->product_id, "quantity", line->quantity, NULL);
		}
		else
		{
			g_object_get(component, "quantity", &stored, NULL);

			if (stored == line->quantity)
				continue;

			g_object_set(component, "quantity", line->quantity, NULL);
		}

		if (!venture_database_save(import->database, component, import->actor, error))
			return FALSE;

		*changed = TRUE;
	}

	if (prune)
	{
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(GPtrArray) found = NULL;

		query = venture_query_new(VENTURE_TYPE_RECIPE_COMPONENT);
		venture_query_set_organization(query, import->organization_id);

		if (!venture_query_add_filter_int(query, "recipe-id", VENTURE_FILTER_OP_EQ,
		                                  venture_entity_get_id(record), error))
			return FALSE;

		found = venture_database_find(import->database, query, error);

		if (NULL == found)
			return FALSE;

		for (i = 0; i < found->len; i++)
		{
			VentureEntity *component = g_ptr_array_index(found, i);
			gboolean reusable = FALSE;
			gboolean listed = FALSE;
			gint64 product = 0;
			guint j;

			g_object_get(component, "product-id", &product, "reusable", &reusable, NULL);

			for (j = 0; (j < lines->len) && !listed; j++)
				listed = (g_array_index(lines, BlizzardLine, j).product_id == product);

			if (listed || reusable)
				continue;

			if (!venture_database_delete(import->database, component, import->actor, error))
				return FALSE;

			*changed = TRUE;
		}
	}

	return TRUE;
}

/*
 * One recipe JSON as records, inside the caller's transaction. A recipe
 * that cannot be made -- it makes no item, an item has no product -- is
 * a skip with *@out_reason; FALSE is an error (the caller decides whether
 * it stops the import).
 */
static gboolean
blizzard_import_recipe(
	BlizzardImport		 *import,
	gpointer		  data,
	BlizzardRecipeOutcome	 *out_outcome,
	gchar			**out_reason,
	GError			**error
){
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(GArray) lines = NULL;
	JsonObject *recipe = data;
	JsonObject *crafted;
	JsonObject *quantity;
	JsonArray *reagents;
	const gchar *name;
	const gchar *locale;
	gint64 recipe_id;
	gint64 output_id;
	gint64 output_quantity;
	gint64 category_id;
	gboolean changed;
	guint i;

	*out_outcome = BLIZZARD_RECIPE_SKIPPED;
	locale = import->frozen->settings->locale;
	recipe_id = blizzard_int(recipe, "id", 0);
	name = blizzard_text(recipe, "name", locale);
	crafted = json_object_has_member(recipe, "crafted_item")
		? json_object_get_object_member(recipe, "crafted_item") : NULL;
	reagents = json_object_has_member(recipe, "reagents")
		? json_object_get_array_member(recipe, "reagents") : NULL;

	if (venture_string_is_empty(name))
		name = "Unnamed recipe";

	/* An enchant makes no item to sell, and a recipe of nothing is not a
	 * craft anybody prices. */
	if ((NULL == crafted) || (blizzard_int(crafted, "id", 0) <= 0) ||
	    (NULL == reagents) || (0 == json_array_get_length(reagents)))
	{
		*out_reason = g_strdup_printf("%s (recipe %" G_GINT64_FORMAT ") makes no item from reagents",
		                              name, recipe_id);
		return TRUE;
	}

	/* crafted_quantity is {"value": n}, or a range {"minimum", "maximum"}
	 * for a proc; the minimum is the batch a craft can count on. */
	quantity = json_object_has_member(recipe, "crafted_quantity")
		? json_object_get_object_member(recipe, "crafted_quantity") : NULL;
	output_quantity = blizzard_int(quantity, "value", blizzard_int(quantity, "minimum", 1));
	output_quantity = MAX(output_quantity, 1);

	if (!blizzard_item_product(import, blizzard_int(crafted, "id", 0),
	                           blizzard_text(crafted, "name", locale), &output_id, out_reason, error))
		return FALSE;

	if (0 == output_id)
		return TRUE;

	lines = g_array_new(FALSE, FALSE, sizeof(BlizzardLine));

	for (i = 0; i < json_array_get_length(reagents); i++)
	{
		JsonNode *element = json_array_get_element(reagents, i);
		JsonObject *line;
		JsonObject *reagent;
		gint64 product_id;
		gint64 count;

		line = JSON_NODE_HOLDS_OBJECT(element) ? json_node_get_object(element) : NULL;
		reagent = ((NULL != line) && json_object_has_member(line, "reagent"))
			? json_object_get_object_member(line, "reagent") : NULL;
		count = blizzard_int(line, "quantity", 1);

		if ((NULL == reagent) || (blizzard_int(reagent, "id", 0) <= 0) || (count < 1))
			continue;

		if (!blizzard_item_product(import, blizzard_int(reagent, "id", 0),
		                           blizzard_text(reagent, "name", locale), &product_id,
		                           out_reason, error))
			return FALSE;

		if (0 == product_id)
			return TRUE;

		blizzard_lines_add(lines, product_id, count);
	}

	/* The recipe: found again by what it makes and what it is called. */
	if (!blizzard_find_one(import, VENTURE_TYPE_RECIPE, "output-product-id", output_id, "name", name, 0,
	                       &record, error))
		return FALSE;

	if ((NULL != record) && venture_entity_is_deleted(record))
	{
		*out_reason = g_strdup_printf("%s was deleted here; restore it to update it", name);
		return TRUE;
	}

	changed = FALSE;

	if (!blizzard_recipe_category(import, &category_id, error))
		return FALSE;

	if (NULL == record)
	{
		g_autofree gchar *notes = g_strdup_printf("Imported from Battle.net recipe %" G_GINT64_FORMAT,
		                                          recipe_id);

		record = blizzard_new_record(import, "recipe", error);

		if (NULL == record)
			return FALSE;

		g_object_set(record, "name", name, "output-product-id", output_id,
		             "output-quantity", output_quantity, "active", TRUE, "notes", notes, NULL);

		if (import->venture_id > 0)
			g_object_set(record, "venture-id", import->venture_id, NULL);

		if (category_id > 0)
			g_object_set(record, "category-id", category_id, NULL);

		if (!venture_database_save(import->database, record, import->actor, error))
			return FALSE;

		*out_outcome = BLIZZARD_RECIPE_CREATED;
	}
	else
	{
		gint64 stored;
		gint64 filed;
		gboolean moved = FALSE;

		g_object_get(record, "output-quantity", &stored, "category-id", &filed, NULL);

		if (stored != output_quantity)
		{
			g_object_set(record, "output-quantity", output_quantity, NULL);
			moved = TRUE;
		}

		/* Filed only when nobody filed it: a recipe imported before the
		 * walk filed recipes gets its place, and one a person moved
		 * keeps theirs. */
		if ((0 == filed) && (category_id > 0))
		{
			g_object_set(record, "category-id", category_id, NULL);
			moved = TRUE;
		}

		if (moved)
		{
			if (!venture_database_save(import->database, record, import->actor, error))
				return FALSE;

			changed = TRUE;
		}
	}

	/* The lines. Battle.net's recipe is not the whole of a recipe a
	 * person keeps here, so a line it does not name is left alone. */
	if (!blizzard_write_components(import, record, lines, FALSE, &changed, error))
		return FALSE;

	if (BLIZZARD_RECIPE_CREATED != *out_outcome)
		*out_outcome = changed ? BLIZZARD_RECIPE_UPDATED : BLIZZARD_RECIPE_UNCHANGED;

	return TRUE;
}

/*
 * One recipe in a transaction of its own, so a failure keeps every recipe
 * before it. A save the records refused (a validator, a missing target)
 * skips this recipe and says why, naming it by @label; anything else
 * stops the import.
 */
static gboolean
blizzard_import_one(
	BlizzardImport		 *import,
	BlizzardRecipeFunc	  func,
	gpointer		  data,
	const gchar		 *label,
	GError			**error
){
	g_autoptr(GError) local_error = NULL;
	g_autofree gchar *reason = NULL;
	BlizzardRecipeOutcome outcome;

	if (!venture_database_begin(import->database, error))
		return FALSE;

	if (!func(import, data, &outcome, &reason, &local_error))
	{
		venture_database_rollback(import->database);

		if (!g_error_matches(local_error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION) &&
		    !g_error_matches(local_error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND))
		{
			g_propagate_error(error, g_steal_pointer(&local_error));
			return FALSE;
		}

		g_clear_pointer(&reason, g_free);
		reason = g_strdup_printf("%s: %s", label, local_error->message);
		blizzard_import_skip(import, reason);
		return TRUE;
	}

	/* A skip writes nothing; whatever it found on the way (an instrument
	 * promoted for a reagent) is still worth keeping. */
	if (!venture_database_commit(import->database, error))
		return FALSE;

	switch (outcome)
	{
	case BLIZZARD_RECIPE_CREATED:
		import->created++;
		break;
	case BLIZZARD_RECIPE_UPDATED:
		import->updated++;
		break;
	case BLIZZARD_RECIPE_UNCHANGED:
		import->unchanged++;
		break;
	case BLIZZARD_RECIPE_SKIPPED:
	default:
		blizzard_import_skip(import, (NULL != reason) ? reason : "a recipe");
		break;
	}

	return TRUE;
}

/* ==========================================================================
 * The walk
 * ========================================================================== */

/*
 * Reads the cursor "<profession>/<tier>/<offset>" a previous call handed
 * back; an empty one starts at the beginning.
 */
static gboolean
blizzard_parse_cursor(
	const gchar	 *text,
	gint64		 *profession,
	gint64		 *tier,
	gint64		 *offset,
	GError		**error
){
	g_auto(GStrv) parts = NULL;

	*profession = 0;
	*tier = 0;
	*offset = 0;

	if (venture_string_is_empty(text))
		return TRUE;

	parts = g_strsplit(text, "/", -1);

	if ((3 != g_strv_length(parts)) ||
	    !g_ascii_string_to_signed(parts[0], 10, 1, G_MAXINT32, profession, NULL) ||
	    !g_ascii_string_to_signed(parts[1], 10, 1, G_MAXINT32, tier, NULL) ||
	    !g_ascii_string_to_signed(parts[2], 10, 0, G_MAXINT32, offset, NULL))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "cursor is what the last import answered: profession/tier/offset");
		return FALSE;
	}

	return TRUE;
}

/*
 * Walks professions, tiers and recipes from the cursor until max_recipes
 * recipes were read. Each recipe is one transaction: a failure keeps what
 * came before it. Returns the cursor to continue from, "" when done.
 */
/* The name an entry of an index array gives @id, or NULL. */
static const gchar *
blizzard_entry_name(
	JsonArray	*array,
	gint64		 id,
	const gchar	*locale
){
	guint i;

	for (i = 0; (NULL != array) && (i < json_array_get_length(array)); i++)
	{
		JsonNode *element = json_array_get_element(array, i);

		if (JSON_NODE_HOLDS_OBJECT(element) && (blizzard_int(json_node_get_object(element), "id", 0) == id))
			return blizzard_text(json_node_get_object(element), "name", locale);
	}

	return NULL;
}

/* Whether a profession is one the import was asked for: by its id or its
 * name, case folded. No list is every profession. */
static gboolean
blizzard_profession_wanted(
	gchar *const	*wanted,
	gint64		 id,
	const gchar	*name
){
	g_autofree gchar *folded = NULL;
	guint i;

	if ((NULL == wanted) || (NULL == wanted[0]))
		return TRUE;

	folded = (NULL != name) ? g_utf8_casefold(name, -1) : NULL;

	for (i = 0; NULL != wanted[i]; i++)
	{
		g_autofree gchar *want = g_utf8_casefold(wanted[i], -1);
		gint64 number;

		if (g_ascii_string_to_signed(wanted[i], 10, 1, G_MAXINT64, &number, NULL) && (number == id))
			return TRUE;

		if ((NULL != folded) && (0 == g_strcmp0(want, folded)))
			return TRUE;
	}

	return FALSE;
}

/* Whether a skill tier's name has @wanted in it, case folded: "Khaz
 * Algar" keeps "Khaz Algar Blacksmithing". No filter is every tier. */
static gboolean
blizzard_tier_wanted(
	const gchar	*wanted,
	const gchar	*name
){
	g_autofree gchar *want = NULL;
	g_autofree gchar *folded = NULL;

	if (venture_string_is_empty(wanted))
		return TRUE;

	if (NULL == name)
		return FALSE;

	want = g_utf8_casefold(wanted, -1);
	folded = g_utf8_casefold(name, -1);

	return NULL != strstr(folded, want);
}

static gchar *
blizzard_import_walk(
	BlizzardImport	 *import,
	gint64		  only_profession,
	gchar *const	 *professions_wanted,
	const gchar	 *skill_tier,
	const gchar	 *cursor,
	guint		  budget,
	GError		**error
){
	g_autoptr(JsonNode) index_node = NULL;
	g_autoptr(GArray) professions = NULL;
	JsonObject *index_object;
	gint64 start_profession;
	gint64 start_tier;
	gint64 start_offset;
	guint p;

	if (!blizzard_parse_cursor(cursor, &start_profession, &start_tier, &start_offset, error))
		return NULL;

	index_object = blizzard_import_require(import, "/data/wow/profession/index", "the profession index",
	                                       &index_node, error);

	if (NULL == index_object)
		return NULL;

	professions = blizzard_import_ids(json_object_has_member(index_object, "professions")
	                                  ? json_object_get_array_member(index_object, "professions") : NULL,
	                                  TRUE);

	for (p = 0; p < professions->len; p++)
	{
		g_autoptr(JsonNode) profession_node = NULL;
		g_autoptr(GArray) tiers = NULL;
		g_autofree gchar *path = NULL;
		JsonObject *profession;
		gint64 profession_id;
		guint t;

		profession_id = g_array_index(professions, gint64, p);

		if (((only_profession > 0) && (profession_id != only_profession)) ||
		    ((start_profession > 0) && (profession_id < start_profession)) ||
		    !blizzard_profession_wanted(professions_wanted, profession_id,
		                                blizzard_entry_name(json_object_has_member(index_object, "professions")
		                                                    ? json_object_get_array_member(index_object,
		                                                                                   "professions")
		                                                    : NULL, profession_id,
		                                                    import->frozen->settings->locale)))
			continue;

		path = g_strdup_printf("/data/wow/profession/%" G_GINT64_FORMAT, profession_id);
		profession = blizzard_import_require(import, path, "a profession", &profession_node, error);

		if (NULL == profession)
			return NULL;

		tiers = blizzard_import_ids(json_object_has_member(profession, "skill_tiers")
		                            ? json_object_get_array_member(profession, "skill_tiers") : NULL,
		                            TRUE);
		import->profession_name = blizzard_text(profession, "name", import->frozen->settings->locale);

		for (t = 0; t < tiers->len; t++)
		{
			g_autoptr(JsonNode) tier_node = NULL;
			g_autoptr(GArray) recipes = NULL;
			g_autoptr(GPtrArray) sections = NULL;
			g_autofree gchar *tier_path = NULL;
			JsonObject *tier;
			JsonArray *categories;
			gint64 tier_id;
			guint first;
			guint r;
			guint c;

			tier_id = g_array_index(tiers, gint64, t);

			if ((profession_id == start_profession) && (tier_id < start_tier))
				continue;

			/* An expansion asked for by name: the profession's list
			 * names every tier, so the others are never fetched. */
			if (!blizzard_tier_wanted(skill_tier,
			                          blizzard_entry_name(json_object_get_array_member(profession,
			                                                                           "skill_tiers"),
			                                              tier_id, import->frozen->settings->locale)))
				continue;

			tier_path = g_strdup_printf("/data/wow/profession/%" G_GINT64_FORMAT "/skill-tier/%"
			                            G_GINT64_FORMAT, profession_id, tier_id);
			tier = blizzard_import_require(import, tier_path, "a skill tier", &tier_node, error);

			if (NULL == tier)
				return NULL;

			/* Every category's recipes, in Blizzard's order, each
			 * remembering which section of the book it is in. */
			recipes = g_array_new(FALSE, FALSE, sizeof(gint64));
			sections = g_ptr_array_new();
			categories = json_object_has_member(tier, "categories")
				? json_object_get_array_member(tier, "categories") : NULL;
			import->tier_name = blizzard_text(tier, "name", import->frozen->settings->locale);

			for (c = 0; (NULL != categories) && (c < json_array_get_length(categories)); c++)
			{
				JsonNode *element = json_array_get_element(categories, c);
				g_autoptr(GArray) some = NULL;
				guint k;

				if (!JSON_NODE_HOLDS_OBJECT(element) ||
				    !json_object_has_member(json_node_get_object(element), "recipes"))
					continue;

				some = blizzard_import_ids(json_object_get_array_member(json_node_get_object(element),
				                                                        "recipes"), FALSE);
				g_array_append_vals(recipes, some->data, some->len);

				for (k = 0; k < some->len; k++)
					g_ptr_array_add(sections, (gpointer)blizzard_text(json_node_get_object(element), "name",
					                                                  import->frozen->settings->locale));
			}

			first = ((profession_id == start_profession) && (tier_id == start_tier))
				? (guint)MIN(start_offset, (gint64)recipes->len) : 0;

			for (r = first; r < recipes->len; r++)
			{
				g_autoptr(JsonNode) recipe_node = NULL;
				g_autofree gchar *recipe_path = NULL;
				g_autofree gchar *label = NULL;
				JsonObject *recipe;

				if (0 == budget)
					return g_strdup_printf("%" G_GINT64_FORMAT "/%" G_GINT64_FORMAT "/%u",
					                       profession_id, tier_id, r);

				recipe_path = g_strdup_printf("/data/wow/recipe/%" G_GINT64_FORMAT,
				                              g_array_index(recipes, gint64, r));
				recipe = blizzard_import_require(import, recipe_path, "a recipe", &recipe_node, error);

				if (NULL == recipe)
					return NULL;

				budget--;
				import->read++;
				import->category_name = g_ptr_array_index(sections, r);
				label = g_strdup_printf("%s (recipe %" G_GINT64_FORMAT ")",
				                        venture_json_object_get_string(recipe, "name", "a recipe"),
				                        blizzard_int(recipe, "id", 0));

				if (!blizzard_import_one(import, blizzard_import_recipe, recipe, label, error))
					return NULL;
			}
		}
	}

	return g_strdup("");
}

/* ==========================================================================
 * Recipes handed in
 *
 * `recipes` is the list itself -- what a person's characters know, as
 * TradeSkillMaster has it (`tsmctl venture recipes` sends it) -- so the
 * import is exactly those recipes and nothing of Battle.net is read: no
 * token, no index, no walk. Each element names its spell, the item it
 * makes and its reagents by item id. A recipe is found again by its spell
 * (`external-ref` wow-spell:<id>), else by what it makes and its name as
 * the walk finds one -- adopting it, so a recipe imported from Battle.net
 * first is the same record -- and the list is the whole recipe: a reagent
 * it no longer names is deleted. It is filed profession / expansion /
 * section, as the walk files profession / tier / section, and who knows
 * it -- market data account keys, "Name-Realm" -- is its `known-by`
 * field, replaced whole, which Crafting filters on. An element of the
 * wrong shape is a skip with its reason, never the end of the call.
 * ========================================================================== */

/* The most recipes one call may be handed, and reagents one may name. */
#define BLIZZARD_SUPPLIED_MAX		(2000)
#define BLIZZARD_SUPPLIED_REAGENTS	(32)
#define BLIZZARD_SUPPLIED_KNOWN_BY	(100)

/* What a supplied recipe's spell is filed as, and the notes line that says
 * which characters know it. */
#define BLIZZARD_SPELL_NAMESPACE	"wow-spell"
#define BLIZZARD_KNOWN_BY		"Known by: "

/* The parameters that steer the walk, refused beside `recipes`. */
static const gchar *const blizzard_walk_parameters[] = {
	"max_recipes", "profession_id", "professions", "skill_tier", "cursor"
};

typedef struct
{
	gint64		 item;
	gint64		 quantity;
	const gchar	*name;
} BlizzardReagent;

/* One element of `recipes`, read. Its strings are borrowed from the
 * parameter's JSON. */
typedef struct
{
	gint64		 spell_id;
	const gchar	*name;
	const gchar	*profession;
	const gchar	*expansion;
	const gchar	*category;
	gint64		 item;
	const gchar	*item_name;
	gint64		 quantity;
	GArray		*reagents;	/* BlizzardReagent */
	gchar		*known_by;	/* "A, B"; "" for nobody; NULL when not said */
	gchar		*known_keys;	/* the same as a JSON list; "" for nobody */
} BlizzardSupplied;

static void
blizzard_supplied_clear(BlizzardSupplied *supplied)
{
	g_clear_pointer(&supplied->reagents, g_array_unref);
	g_clear_pointer(&supplied->known_by, g_free);
	g_clear_pointer(&supplied->known_keys, g_free);
}

/* @member of @object, or NULL when it is missing or null. */
static JsonNode *
blizzard_member(
	JsonObject	*object,
	const gchar	*member
){
	JsonNode *node;

	node = json_object_has_member(object, member) ? json_object_get_member(object, member) : NULL;

	return ((NULL != node) && !JSON_NODE_HOLDS_NULL(node)) ? node : NULL;
}

/* @node as a string, or NULL when it is anything else. */
static const gchar *
blizzard_node_text(JsonNode *node)
{
	if ((NULL == node) || !JSON_NODE_HOLDS_VALUE(node) || (G_TYPE_STRING != json_node_get_value_type(node)))
		return NULL;

	return json_node_get_string(node);
}

/*
 * @node as a number of at least @least: a JSON integer as it is, or a
 * number with a fraction rounded down when @round_down allows one (a
 * whole number written 4.0 is always accepted). FALSE for anything else.
 */
static gboolean
blizzard_node_count(
	JsonNode	*node,
	gint64		 least,
	gboolean	 round_down,
	gint64		*out
){
	if ((NULL == node) || !JSON_NODE_HOLDS_VALUE(node))
		return FALSE;

	if (G_TYPE_INT64 == json_node_get_value_type(node))
		*out = json_node_get_int(node);
	else if (G_TYPE_DOUBLE == json_node_get_value_type(node))
	{
		gdouble value = json_node_get_double(node);
		gint64 whole;

		/* Out of range (NaN included) before the cast, which would be
		 * undefined for it. */
		if (!((value >= 0.0) && (value <= (gdouble)G_MAXINT32)))
			return FALSE;

		whole = (gint64)value;

		if (!round_down && ((gdouble)whole < value))
			return FALSE;

		*out = whole;
	}
	else
		return FALSE;

	return (*out >= least) && (*out <= G_MAXINT32);
}

/* Orders names for blizzard_supplied_read()'s known-by list. */
static gint
blizzard_compare_names(
	gconstpointer	a,
	gconstpointer	b
){
	return g_strcmp0(*(const gchar *const *)a, *(const gchar *const *)b);
}

/*
 * Reads one element of `recipes` into @out. FALSE, with *@out_reason,
 * when it is not the shape the action documents; @out is cleared by the
 * caller either way.
 */
static gboolean
blizzard_supplied_read(
	JsonNode		 *element,
	BlizzardSupplied	 *out,
	gchar			**out_reason
){
	JsonObject *object;
	JsonNode *node;
	JsonArray *reagents;
	guint i;

	if (!JSON_NODE_HOLDS_OBJECT(element))
	{
		*out_reason = g_strdup("is not an object");
		return FALSE;
	}

	object = json_node_get_object(element);

	if (!blizzard_node_count(blizzard_member(object, "spell_id"), 1, FALSE, &out->spell_id))
	{
		*out_reason = g_strdup("spell_id must be a positive whole number");
		return FALSE;
	}

	out->name = blizzard_node_text(blizzard_member(object, "name"));
	out->profession = blizzard_node_text(blizzard_member(object, "profession"));

	if (venture_string_is_empty(out->name) || venture_string_is_empty(out->profession))
	{
		*out_reason = g_strdup("name and profession must be text");
		return FALSE;
	}

	/* The expansion (the profession's tier, "Khaz Algar") and the
	 * section of its book: text, or nothing. */
	node = blizzard_member(object, "expansion");
	out->expansion = blizzard_node_text(node);

	if ((NULL != node) && (NULL == out->expansion))
	{
		*out_reason = g_strdup("expansion must be text or null");
		return FALSE;
	}

	node = blizzard_member(object, "category");
	out->category = blizzard_node_text(node);

	if ((NULL != node) && (NULL == out->category))
	{
		*out_reason = g_strdup("category must be text or null");
		return FALSE;
	}

	if (!blizzard_node_count(blizzard_member(object, "item"), 1, FALSE, &out->item))
	{
		*out_reason = g_strdup("item must be the id of the item it makes");
		return FALSE;
	}

	out->item_name = blizzard_node_text(blizzard_member(object, "item_name"));

	/* TradeSkillMaster's number made is an average where a craft can
	 * make more than one (1.5 for a proc); a recipe counts whole units,
	 * so it is rounded down -- the batch a craft can count on, as the
	 * walk reads a range's minimum. Left out, a craft makes one. */
	node = blizzard_member(object, "quantity");
	out->quantity = 1;

	if ((NULL != node) && !blizzard_node_count(node, 1, TRUE, &out->quantity))
	{
		*out_reason = g_strdup("quantity must be a number of at least 1");
		return FALSE;
	}

	node = blizzard_member(object, "reagents");
	reagents = ((NULL != node) && JSON_NODE_HOLDS_ARRAY(node)) ? json_node_get_array(node) : NULL;

	if ((NULL == reagents) || (0 == json_array_get_length(reagents)) ||
	    (json_array_get_length(reagents) > BLIZZARD_SUPPLIED_REAGENTS))
	{
		*out_reason = g_strdup_printf("reagents must be a list of 1 to %d reagents",
		                              BLIZZARD_SUPPLIED_REAGENTS);
		return FALSE;
	}

	out->reagents = g_array_new(FALSE, FALSE, sizeof(BlizzardReagent));

	for (i = 0; i < json_array_get_length(reagents); i++)
	{
		JsonNode *line = json_array_get_element(reagents, i);
		BlizzardReagent reagent;

		if (!JSON_NODE_HOLDS_OBJECT(line) ||
		    !blizzard_node_count(blizzard_member(json_node_get_object(line), "item"), 1, FALSE,
		                         &reagent.item) ||
		    !blizzard_node_count(blizzard_member(json_node_get_object(line), "quantity"), 1, FALSE,
		                         &reagent.quantity))
		{
			*out_reason = g_strdup_printf("reagents[%u] must be {\"item\": id, \"quantity\": whole number "
			                              "of at least 1}", i);
			return FALSE;
		}

		reagent.name = blizzard_node_text(blizzard_member(json_node_get_object(line), "name"));
		g_array_append_val(out->reagents, reagent);
	}

	/* Who knows it, as market data account keys ("Name-Realm", the key of
	 * the character's `account` line): kept exactly, sorted and once
	 * each, so the same characters in another order are no change. Not
	 * said is not "nobody". */
	node = blizzard_member(object, "known_by");

	if (NULL != node)
	{
		g_autoptr(GPtrArray) names = g_ptr_array_new_with_free_func(g_free);
		g_autoptr(GString) joined = g_string_new(NULL);
		g_autoptr(JsonArray) keys = json_array_new();
		g_autoptr(JsonNode) keys_node = NULL;
		JsonArray *array = JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;

		if ((NULL == array) || (json_array_get_length(array) > BLIZZARD_SUPPLIED_KNOWN_BY))
		{
			*out_reason = g_strdup_printf("known_by must be a list of at most %d names",
			                              BLIZZARD_SUPPLIED_KNOWN_BY);
			return FALSE;
		}

		for (i = 0; i < json_array_get_length(array); i++)
		{
			const gchar *text = blizzard_node_text(json_array_get_element(array, i));
			const gchar *at;

			if (venture_string_is_empty(text))
			{
				*out_reason = g_strdup("known_by must be a list of account keys");
				return FALSE;
			}

			/* A key is matched exactly, so it is never cleaned up --
			 * one that could not be a key (a control character would
			 * also end its notes line) is refused. */
			for (at = text; '\0' != *at; at++)
			{
				if ((guchar)*at < 0x20)
				{
					*out_reason = g_strdup("known_by holds a key with a control character");
					return FALSE;
				}
			}

			g_ptr_array_add(names, g_strdup(text));
		}

		g_ptr_array_sort(names, blizzard_compare_names);

		for (i = 0; i < names->len; i++)
		{
			if ((i > 0) && (0 == g_strcmp0(g_ptr_array_index(names, i), g_ptr_array_index(names, i - 1))))
				continue;

			if (joined->len > 0)
				g_string_append(joined, ", ");

			g_string_append(joined, g_ptr_array_index(names, i));
			json_array_add_string_element(keys, g_ptr_array_index(names, i));
		}

		out->known_by = g_string_free(g_steal_pointer(&joined), FALSE);
		keys_node = json_node_new(JSON_NODE_ARRAY);
		json_node_set_array(keys_node, keys);
		out->known_keys = (json_array_get_length(keys) > 0) ? venture_json_to_string(keys_node, FALSE)
		                                                    : g_strdup("");
	}

	return TRUE;
}

/*
 * @notes with its "Known by: " line saying @known_by: replaced where there
 * is one, added at the end where there is none, taken out when @known_by
 * is empty. Every other line is a person's and is kept as it is.
 */
static gchar *
blizzard_notes_known_by(
	const gchar	*notes,
	const gchar	*known_by
){
	g_auto(GStrv) lines = NULL;
	GString *out;
	gboolean placed = FALSE;
	guint i;

	lines = g_strsplit((NULL != notes) ? notes : "", "\n", -1);
	out = g_string_new(NULL);

	for (i = 0; NULL != lines[i]; i++)
	{
		const gchar *line = lines[i];

		if (g_str_has_prefix(line, BLIZZARD_KNOWN_BY))
		{
			if (placed || venture_string_is_empty(known_by))
				continue;

			placed = TRUE;

			if (i > 0)
				g_string_append_c(out, '\n');

			g_string_append_printf(out, "%s%s", BLIZZARD_KNOWN_BY, known_by);
			continue;
		}

		if (i > 0)
			g_string_append_c(out, '\n');

		g_string_append(out, line);
	}

	if (!placed && !venture_string_is_empty(known_by))
	{
		if ((out->len > 0) && ('\n' != out->str[out->len - 1]))
			g_string_append_c(out, '\n');

		g_string_append_printf(out, "%s%s", BLIZZARD_KNOWN_BY, known_by);
	}

	return g_string_free(out, FALSE);
}

/* The recipe filed under @ref, deleted ones included. */
static gboolean
blizzard_find_reference(
	BlizzardImport	 *import,
	const gchar	 *ref,
	VentureEntity	**out,
	GError		**error
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) found = NULL;

	*out = NULL;
	query = venture_query_new(VENTURE_TYPE_RECIPE);
	venture_query_set_organization(query, import->organization_id);
	venture_query_set_include_deleted(query, TRUE);

	if (!venture_query_add_filter_string(query, "external-ref", VENTURE_FILTER_OP_EQ, ref, error))
		return FALSE;

	found = venture_database_find(import->database, query, error);

	if (NULL == found)
		return FALSE;

	if (found->len > 0)
		*out = g_object_ref(g_ptr_array_index(found, 0));

	return TRUE;
}

/* One supplied recipe as records: a BlizzardRecipeFunc. */
static gboolean
blizzard_supplied_recipe(
	BlizzardImport		 *import,
	gpointer		  data,
	BlizzardRecipeOutcome	 *out_outcome,
	gchar			**out_reason,
	GError			**error
){
	BlizzardSupplied *supplied = data;
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(GArray) lines = NULL;
	g_autofree gchar *ref = NULL;
	gint64 output_id;
	gint64 category_id;
	gboolean changed = FALSE;
	guint i;

	*out_outcome = BLIZZARD_RECIPE_SKIPPED;

	if (!blizzard_item_product(import, supplied->item, supplied->item_name, &output_id, out_reason, error))
		return FALSE;

	if (0 == output_id)
		return TRUE;

	lines = g_array_new(FALSE, FALSE, sizeof(BlizzardLine));

	for (i = 0; i < supplied->reagents->len; i++)
	{
		BlizzardReagent *reagent = &g_array_index(supplied->reagents, BlizzardReagent, i);
		gint64 product_id;

		if (!blizzard_item_product(import, reagent->item, reagent->name, &product_id, out_reason, error))
			return FALSE;

		if (0 == product_id)
			return TRUE;

		blizzard_lines_add(lines, product_id, reagent->quantity);
	}

	/* The recipe: by its spell, else as the walk finds one -- what it
	 * makes and what it is called -- when nothing else claimed it. */
	ref = g_strdup_printf(BLIZZARD_SPELL_NAMESPACE ":%" G_GINT64_FORMAT, supplied->spell_id);

	if (!blizzard_find_reference(import, ref, &record, error))
		return FALSE;

	if (NULL == record)
	{
		g_autoptr(VentureEntity) named = NULL;
		g_autofree gchar *held = NULL;

		if (!blizzard_find_one(import, VENTURE_TYPE_RECIPE, "output-product-id", output_id, "name",
		                       supplied->name, 0, &named, error))
			return FALSE;

		if (NULL != named)
			g_object_get(named, "external-ref", &held, NULL);

		if ((NULL != named) && venture_string_is_empty(held))
			record = g_steal_pointer(&named);
	}

	if ((NULL != record) && venture_entity_is_deleted(record))
	{
		*out_reason = g_strdup_printf("%s (spell %" G_GINT64_FORMAT ") was deleted here; restore it to "
		                              "update it", supplied->name, supplied->spell_id);
		return TRUE;
	}

	/* Profession / expansion / section, as the walk files profession /
	 * skill tier / section; a level not given is left out. */
	import->profession_name = supplied->profession;
	import->tier_name = supplied->expansion;
	import->category_name = supplied->category;

	if (!blizzard_recipe_category(import, &category_id, error))
		return FALSE;

	if (NULL == record)
	{
		g_autofree gchar *origin = NULL;
		g_autofree gchar *notes = NULL;

		origin = g_strdup_printf("Imported from TradeSkillMaster: spell %" G_GINT64_FORMAT, supplied->spell_id);
		notes = blizzard_notes_known_by(origin, supplied->known_by);
		record = blizzard_new_record(import, "recipe", error);

		if (NULL == record)
			return FALSE;

		g_object_set(record, "name", supplied->name, "output-product-id", output_id,
		             "output-quantity", supplied->quantity, "active", TRUE, "notes", notes,
		             "external-ref", ref, NULL);

		if (!venture_string_is_empty(supplied->known_keys))
			g_object_set(record, "known-by", supplied->known_keys, NULL);

		if (import->venture_id > 0)
			g_object_set(record, "venture-id", import->venture_id, NULL);

		if (category_id > 0)
			g_object_set(record, "category-id", category_id, NULL);

		if (!venture_database_save(import->database, record, import->actor, error))
			return FALSE;

		*out_outcome = BLIZZARD_RECIPE_CREATED;
	}
	else
	{
		g_autofree gchar *held = NULL;
		g_autofree gchar *notes = NULL;
		g_autofree gchar *knowers = NULL;
		gint64 stored;
		gint64 makes;
		gint64 filed;
		gboolean moved = FALSE;

		g_object_get(record, "output-quantity", &stored, "output-product-id", &makes, "category-id", &filed,
		             "external-ref", &held, "notes", &notes, "known-by", &knowers, NULL);

		if (stored != supplied->quantity)
		{
			g_object_set(record, "output-quantity", supplied->quantity, NULL);
			moved = TRUE;
		}

		/* The spell is the recipe; what it makes is what the list says. */
		if (makes != output_id)
		{
			g_object_set(record, "output-product-id", output_id, NULL);
			moved = TRUE;
		}

		if (venture_string_is_empty(held))
		{
			g_object_set(record, "external-ref", ref, NULL);
			moved = TRUE;
		}

		/* Filed only when nobody filed it, as the walk does. */
		if ((0 == filed) && (category_id > 0))
		{
			g_object_set(record, "category-id", category_id, NULL);
			moved = TRUE;
		}

		/* Who knows it is replaced whole: a character that no longer
		 * knows it drops off. */
		if ((NULL != supplied->known_keys) &&
		    (0 != g_strcmp0(supplied->known_keys, (NULL != knowers) ? knowers : "")))
		{
			g_object_set(record, "known-by",
			             venture_string_is_empty(supplied->known_keys) ? NULL : supplied->known_keys, NULL);
			moved = TRUE;
		}

		if (NULL != supplied->known_by)
		{
			g_autofree gchar *told = blizzard_notes_known_by(notes, supplied->known_by);

			if (0 != g_strcmp0(told, (NULL != notes) ? notes : ""))
			{
				g_object_set(record, "notes", told, NULL);
				moved = TRUE;
			}
		}

		if (moved)
		{
			if (!venture_database_save(import->database, record, import->actor, error))
				return FALSE;

			changed = TRUE;
		}
	}

	/* The list is the whole recipe: a reagent it stopped naming goes. */
	if (!blizzard_write_components(import, record, lines, TRUE, &changed, error))
		return FALSE;

	if (BLIZZARD_RECIPE_CREATED != *out_outcome)
		*out_outcome = changed ? BLIZZARD_RECIPE_UPDATED : BLIZZARD_RECIPE_UNCHANGED;

	return TRUE;
}

/*
 * The `recipes` parameter as a JSON array: as sent, or parsed from the
 * text a form or `venturectl act` posts. NULL with *@error when it is
 * neither or too long.
 */
static JsonNode *
blizzard_supplied_list(
	JsonNode	 *node,
	GError		**error
){
	g_autoptr(JsonNode) parsed = NULL;
	const gchar *text;

	text = blizzard_node_text(node);

	if (NULL != text)
	{
		parsed = venture_json_parse(text, error);

		if (NULL == parsed)
			return NULL;

		node = parsed;
	}

	if (!JSON_NODE_HOLDS_ARRAY(node))
	{
		venture_set_error_validation(error, "recipes", "must be a JSON array of recipes");
		return NULL;
	}

	if (json_array_get_length(json_node_get_array(node)) > BLIZZARD_SUPPLIED_MAX)
	{
		venture_set_error_validation(error, "recipes", "holds at most %d recipes a call; send the rest in "
		                             "another", BLIZZARD_SUPPLIED_MAX);
		return NULL;
	}

	return (NULL != parsed) ? g_steal_pointer(&parsed) : json_node_ref(node);
}

/*
 * Every element of @list, each in a transaction of its own. FALSE only
 * for what stops the import (the database); a bad element is a skip.
 */
static gboolean
blizzard_supplied_run(
	BlizzardImport	 *import,
	JsonArray	 *list,
	GError		**error
){
	guint i;

	for (i = 0; i < json_array_get_length(list); i++)
	{
		g_autofree gchar *reason = NULL;
		g_autofree gchar *label = NULL;
		BlizzardSupplied supplied;
		gboolean ok;

		memset(&supplied, 0, sizeof(supplied));
		import->read++;

		if (!blizzard_supplied_read(json_array_get_element(list, i), &supplied, &reason))
		{
			g_autofree gchar *skip = NULL;

			skip = (supplied.spell_id > 0)
				? g_strdup_printf("recipes[%u] (spell %" G_GINT64_FORMAT "): %s", i, supplied.spell_id, reason)
				: g_strdup_printf("recipes[%u]: %s", i, reason);
			blizzard_import_skip(import, skip);
			blizzard_supplied_clear(&supplied);
			continue;
		}

		label = g_strdup_printf("%s (spell %" G_GINT64_FORMAT ")", supplied.name, supplied.spell_id);
		ok = blizzard_import_one(import, blizzard_supplied_recipe, &supplied, label, error);
		blizzard_supplied_clear(&supplied);

		if (!ok)
			return FALSE;
	}

	return TRUE;
}

/* ==========================================================================
 * The action
 * ========================================================================== */

static VentureContext *
blizzard_context_for(VentureDatabase *database)
{
	GWeakRef *ref;

	ref = g_object_get_data(G_OBJECT(database), BLIZZARD_CONTEXT_KEY);

	return (NULL != ref) ? g_weak_ref_get(ref) : NULL;
}

static gboolean
blizzard_import_allowed(
	VentureAction		 *action,
	VentureEntity		 *entity,
	const VentureActor	 *actor,
	GError			**error
){
	g_autoptr(VentureContext) context = NULL;
	g_autofree gchar *provider = NULL;

	(void)actor;

	if (venture_entity_is_deleted(entity))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
		                    "A deleted data source imports nothing");
		return FALSE;
	}

	g_object_get(entity, "provider", &provider, NULL);

	if (0 != g_strcmp0(provider, BLIZZARD_PROVIDER_NAME))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "import_recipes reads Battle.net's professions; this source's provider is %s",
		            (NULL != provider) ? provider : "unset");
		return FALSE;
	}

	context = blizzard_context_for(VENTURE_DATABASE(venture_action_get_data(action)));

	if ((NULL == context) || (NULL == venture_context_get_feeds_service(context)))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		                    "Market data feeds are off (feeds.enabled)");
		return FALSE;
	}

	if (!venture_context_module_enabled(context, "production") ||
	    !venture_context_module_enabled(context, "marketdata"))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		                    "Importing recipes needs the production and marketdata modules on");
		return FALSE;
	}

	return TRUE;
}

static JsonNode *
blizzard_param(
	GHashTable	*params,
	const gchar	*name
){
	JsonNode *node;

	node = (NULL != params) ? g_hash_table_lookup(params, name) : NULL;

	return ((NULL != node) && JSON_NODE_HOLDS_VALUE(node)) ? node : NULL;
}

/* Whether products may be made, and for which venture, since a product
 * needs one: both paths read it the same way. */
static gboolean
blizzard_import_products(
	GHashTable	 *params,
	BlizzardImport	 *import,
	GError		**error
){
	JsonNode *node;

	node = blizzard_param(params, "create_products");
	import->create_products = (NULL != node) && json_node_get_boolean(node);
	node = blizzard_param(params, "venture_id");
	import->venture_id = (NULL != node) ? json_node_get_int(node) : 0;

	if (import->create_products && (import->venture_id <= 0))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "create_products needs venture_id: a product belongs to a venture");
		return FALSE;
	}

	return TRUE;
}

/*
 * import_recipes handed `recipes`: no request is opened, so Battle.net is
 * never asked for anything, and the walk's parameters are refused rather
 * than ignored -- a caller sending both meant something this cannot do.
 */
static VentureEntity *
blizzard_supplied_invoke(
	VentureContext		 *context,
	VentureEntity		 *entity,
	GHashTable		 *params,
	JsonNode		 *recipes,
	const VentureActor	 *actor,
	GError			**error
){
	g_autoptr(JsonNode) list = NULL;
	g_autoptr(GPtrArray) reasons = NULL;
	g_autoptr(GHashTable) categories = NULL;
	g_autoptr(GString) result = NULL;
	g_autofree gchar *namespace_ = NULL;
	BlizzardImport import;
	JsonNode *node;
	guint i;

	for (i = 0; i < G_N_ELEMENTS(blizzard_walk_parameters); i++)
	{
		node = blizzard_param(params, blizzard_walk_parameters[i]);

		/* A form posts every field; an empty one asks for nothing. */
		if ((NULL != node) && !((G_TYPE_STRING == json_node_get_value_type(node)) &&
		                        venture_string_is_empty(json_node_get_string(node))))
		{
			venture_set_error_validation(error, blizzard_walk_parameters[i],
			                             "steers the walk of Battle.net's professions; with recipes "
			                             "there is no walk, so leave it out");
			return NULL;
		}
	}

	list = blizzard_supplied_list(recipes, error);

	if (NULL == list)
		return NULL;

	memset(&import, 0, sizeof(import));

	if (!blizzard_import_products(params, &import, error))
		return NULL;

	g_object_get(entity, "instrument-namespace", &namespace_, NULL);
	reasons = g_ptr_array_new_with_free_func(g_free);
	categories = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

	import.context = context;
	import.database = venture_context_get_database(context);
	import.actor = actor;
	import.organization_id = venture_entity_get_organization_id(entity);
	import.source_id = venture_entity_get_id(entity);
	import.namespace_ = venture_string_is_empty(namespace_) ? (gchar *)BLIZZARD_INSTRUMENT_NAMESPACE
	                                                        : namespace_;
	import.reasons = reasons;
	import.categories = categories;

	if (!blizzard_supplied_run(&import, json_node_get_array(list), error))
	{
		g_prefix_error(error, "After %u recipes read (%u created, %u updated): ", import.read, import.created,
		               import.updated);
		return NULL;
	}

	result = g_string_new(NULL);
	g_string_append_printf(result, "Read %u recipes: %u created, %u updated, %u unchanged, %u skipped.",
	                       import.read, import.created, import.updated, import.unchanged, import.skipped);

	for (i = 0; i < reasons->len; i++)
		g_string_append_printf(result, " Skipped: %s.", (const gchar *)g_ptr_array_index(reasons, i));

	g_string_append(result, " Done: every recipe supplied has been read.");
	g_object_set(entity, "result", result->str, NULL);

	return g_object_ref(entity);
}

static VentureEntity *
blizzard_import_invoke(
	VentureAction		 *action,
	VentureEntity		 *entity,
	GHashTable		 *params,
	const VentureActor	 *actor,
	GError			**error
){
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(VentureFeedRequest) request = NULL;
	g_autoptr(GPtrArray) reasons = NULL;
	g_autoptr(GString) result = NULL;
	g_autofree gchar *next = NULL;
	g_autofree gchar *namespace_ = NULL;
	g_auto(GStrv) professions = NULL;
	g_autoptr(GHashTable) categories = NULL;
	BlizzardImport import;
	JsonNode *node;
	const gchar *skill_tier;
	const gchar *cursor;
	gint64 only_profession;
	gint64 budget;
	guint i;

	context = blizzard_context_for(VENTURE_DATABASE(venture_action_get_data(action)));

	if ((NULL == context) || (NULL == venture_context_get_feeds_service(context)))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		                    "Market data feeds are off (feeds.enabled)");
		return NULL;
	}

	/* The recipes themselves, handed in: nothing of Battle.net is read. */
	node = (NULL != params) ? g_hash_table_lookup(params, "recipes") : NULL;

	if ((NULL != node) && !JSON_NODE_HOLDS_NULL(node) &&
	    !((NULL != blizzard_node_text(node)) && venture_string_is_empty(blizzard_node_text(node))))
		return blizzard_supplied_invoke(context, entity, params, node, actor, error);

	/* The parameters: how far, from where, and whether products may be
	 * made (and for which venture, since a product needs one). */
	node = blizzard_param(params, "max_recipes");
	budget = (NULL != node) ? json_node_get_int(node) : BLIZZARD_RECIPES_DEFAULT;

	if ((budget < 1) || (budget > BLIZZARD_RECIPES_MAX))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "max_recipes is from 1 to %d", BLIZZARD_RECIPES_MAX);
		return NULL;
	}

	node = blizzard_param(params, "profession_id");
	only_profession = (NULL != node) ? json_node_get_int(node) : 0;
	node = blizzard_param(params, "cursor");
	cursor = (NULL != node) ? json_node_get_string(node) : NULL;
	node = blizzard_param(params, "skill_tier");
	skill_tier = (NULL != node) ? json_node_get_string(node) : NULL;
	node = blizzard_param(params, "professions");

	/* "Alchemy, Inscription" or "171,773": the professions a person
	 * has, by name or Blizzard's id, so one call reads just theirs. */
	if ((NULL != node) && !venture_string_is_empty(json_node_get_string(node)))
	{
		g_auto(GStrv) parts = g_strsplit(json_node_get_string(node), ",", -1);
		g_autoptr(GPtrArray) kept = g_ptr_array_new_with_free_func(g_free);
		guint k;

		for (k = 0; NULL != parts[k]; k++)
		{
			g_strstrip(parts[k]);

			if ('\0' != parts[k][0])
				g_ptr_array_add(kept, g_strdup(parts[k]));
		}

		g_ptr_array_add(kept, NULL);
		professions = (gchar **)g_ptr_array_free(g_steal_pointer(&kept), FALSE);
	}

	memset(&import, 0, sizeof(import));

	if (!blizzard_import_products(params, &import, error))
		return NULL;

	/* The request a run would make, frozen now: credentials, allowlist,
	 * deadline, cap. Nothing it does touches the store or the budget. */
	request = venture_feeds_service_open_request(venture_context_get_feeds_service(context),
	                                             venture_entity_get_id(entity), NULL, error);

	if (NULL == request)
		return NULL;

	g_object_get(entity, "instrument-namespace", &namespace_, NULL);
	reasons = g_ptr_array_new_with_free_func(g_free);

	import.context = context;
	import.database = venture_context_get_database(context);
	import.request = request;
	import.frozen = venture_feed_request_get_frozen(request);
	import.actor = actor;
	import.organization_id = venture_entity_get_organization_id(entity);
	import.source_id = venture_entity_get_id(entity);
	import.namespace_ = venture_string_is_empty(namespace_) ? (gchar *)BLIZZARD_INSTRUMENT_NAMESPACE
	                                                        : namespace_;
	import.reasons = reasons;
	categories = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	import.categories = categories;

	if (NULL == import.frozen)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
		                    "The source's provider did not freeze");
		return NULL;
	}

	{
		g_autoptr(GError) walk_error = NULL;

		next = blizzard_import_walk(&import, only_profession, professions, skill_tier, cursor, (guint)budget,
		                            &walk_error);

		if (NULL == next)
		{
			/* What Battle.net or a save refused -- redacted, so a
			 * far end that echoed a credential does not hand it on --
			 * and how far the import got before it. */
			g_autofree gchar *message = venture_feed_request_redact(request, walk_error->message);

			g_set_error(error, walk_error->domain, walk_error->code,
			            "%s (after %u recipes read: %u created, %u updated)", message, import.read,
			            import.created, import.updated);
			return NULL;
		}
	}

	result = g_string_new(NULL);
	g_string_append_printf(result, "Read %u recipes: %u created, %u updated, %u unchanged, %u skipped.",
	                       import.read, import.created, import.updated, import.unchanged, import.skipped);

	for (i = 0; i < reasons->len; i++)
		g_string_append_printf(result, " Skipped: %s.", (const gchar *)g_ptr_array_index(reasons, i));

	if ('\0' != next[0])
		g_string_append_printf(result, " More remain: run again with cursor=%s", next);
	else
		g_string_append(result, " Done: every recipe has been read.");

	g_object_set(entity, "result", result->str, NULL);

	return g_object_ref(entity);
}

/* ==========================================================================
 * The fees action: every realm's venue record, given wow_auction
 *
 * A scan nets the auction house's cut and deposit out only at a venue
 * whose record names a fee model, and a region has some eighty realms
 * and its commodity market. `set_venue_fees` promotes every venue the
 * source's store knows to a record (idempotent) and gives each one that
 * names no fee model `wow_auction` with the parameters asked. A record
 * that already names a model is a person's choice and is left alone
 * unless `replace` says otherwise.
 * ========================================================================== */

static gboolean
blizzard_fees_allowed(
	VentureAction		 *action,
	VentureEntity		 *entity,
	const VentureActor	 *actor,
	GError			**error
){
	g_autoptr(VentureContext) context = NULL;
	g_autofree gchar *provider = NULL;

	(void)actor;

	if (venture_entity_is_deleted(entity))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
		                    "A deleted data source has no venues to price");
		return FALSE;
	}

	g_object_get(entity, "provider", &provider, NULL);

	if (0 != g_strcmp0(provider, BLIZZARD_PROVIDER_NAME))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "set_venue_fees prices World of Warcraft auction houses; this source's provider is %s",
		            (NULL != provider) ? provider : "unset");
		return FALSE;
	}

	context = blizzard_context_for(VENTURE_DATABASE(venture_action_get_data(action)));

	if ((NULL == context) || (NULL == venture_context_get_feeds_service(context)) ||
	    !venture_context_module_enabled(context, "marketdata"))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		                    "Setting venue fees needs market data feeds (feeds.enabled) and the "
		                    "marketdata module on");
		return FALSE;
	}

	return TRUE;
}

static VentureEntity *
blizzard_fees_invoke(
	VentureAction		 *action,
	VentureEntity		 *entity,
	GHashTable		 *params,
	const VentureActor	 *actor,
	GError			**error
){
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(VentureSeriesStore) store = NULL;
	g_autoptr(GPtrArray) venues = NULL;
	g_autoptr(GString) yaml = NULL;
	g_autofree gchar *result = NULL;
	VentureDatabase *database;
	JsonNode *node;
	gboolean replace;
	guint set;
	guint kept;
	guint i;

	context = blizzard_context_for(VENTURE_DATABASE(venture_action_get_data(action)));

	if ((NULL == context) || (NULL == venture_context_get_feeds_service(context)))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		                    "Market data feeds are off (feeds.enabled)");
		return NULL;
	}

	/* The model's own parameters, written as YAML the venue's validator
	 * judges with the model: a bad one is refused there, by name. */
	yaml = g_string_new(NULL);
	node = blizzard_param(params, "cut_percent");
	g_string_append_printf(yaml, "cut_percent: %" G_GINT64_FORMAT "\n",
	                       (NULL != node) ? json_node_get_int(node) : (gint64)5);
	node = blizzard_param(params, "duration_hours");
	g_string_append_printf(yaml, "duration_hours: %" G_GINT64_FORMAT "\n",
	                       (NULL != node) ? json_node_get_int(node) : (gint64)48);
	node = blizzard_param(params, "deposit_factor_percent");

	if (NULL != node)
		g_string_append_printf(yaml, "deposit_factor_percent: %" G_GINT64_FORMAT "\n", json_node_get_int(node));

	node = blizzard_param(params, "replace");
	replace = (NULL != node) && json_node_get_boolean(node);

	store = venture_feeds_service_open_reader(venture_context_get_feeds_service(context),
	                                          venture_entity_get_id(entity), error);

	if (NULL == store)
		return NULL;

	venues = venture_series_store_list_venues(store, error);

	if (NULL == venues)
		return NULL;

	database = venture_context_get_database(context);
	set = 0;
	kept = 0;

	if (!venture_database_begin(database, error))
		return NULL;

	for (i = 0; i < venues->len; i++)
	{
		VentureSeriesVenueRow *row = g_ptr_array_index(venues, i);
		g_autoptr(VentureEntity) venue = NULL;
		g_autofree gchar *model = NULL;

		if (!venture_marketdata_promote_venue(context, venture_entity_get_organization_id(entity),
		                                      venture_entity_get_id(entity), row->key, actor, &venue, error))
		{
			venture_database_rollback(database);
			return NULL;
		}

		g_object_get(venue, "fee-model", &model, NULL);

		if (!venture_string_is_empty(model) && !replace)
		{
			kept++;
			continue;
		}

		g_object_set(venue, "fee-model", BLIZZARD_FEE_MODEL_NAME, "fee-params", yaml->str, NULL);

		if (!venture_database_save(database, venue, actor, error))
		{
			venture_database_rollback(database);
			return NULL;
		}

		set++;
	}

	if (!venture_database_commit(database, error))
		return NULL;

	result = g_strdup_printf("%u venue%s given wow_auction (%s), %u kept the fee model %s already had.",
	                         set, (1 == set) ? "" : "s", g_strstrip(g_strdelimit(yaml->str, "\n", ' ')), kept,
	                         (1 == kept) ? "it" : "they");
	g_object_set(entity, "result", result, NULL);

	return g_object_ref(entity);
}

static void
blizzard_weak_ref_free(gpointer data)
{
	g_weak_ref_clear(data);
	g_free(data);
}

gboolean
blizzard_recipes_register(
	VentureContext	 *context,
	GError		**error
){
	g_autoptr(GPtrArray) parameters = NULL;
	g_autoptr(VentureAction) action = NULL;
	VentureDatabase *database;
	VentureFieldSpec *field;
	GWeakRef *ref;

	database = venture_context_get_database(context);
	ref = g_object_get_data(G_OBJECT(database), BLIZZARD_CONTEXT_KEY);

	/* Tests build several contexts over one database: the action is
	 * registered once, and the last context is the one it uses. */
	if (NULL != ref)
	{
		g_weak_ref_set(ref, context);
		return TRUE;
	}

	ref = g_new0(GWeakRef, 1);
	g_weak_ref_init(ref, context);
	g_object_set_data_full(G_OBJECT(database), BLIZZARD_CONTEXT_KEY, ref, blizzard_weak_ref_free);

	parameters = g_ptr_array_new_with_free_func((GDestroyNotify)venture_field_spec_free);

	field = venture_field_spec_new("max_recipes", "Recipes", VENTURE_FIELD_KIND_INTEGER);
	field->help = g_strdup("How many recipes to read this time, 1 to 500; 50 when left empty");
	g_ptr_array_add(parameters, field);
	field = venture_field_spec_new("profession_id", "Profession", VENTURE_FIELD_KIND_INTEGER);
	field->help = g_strdup("Only this profession's recipes (Blizzard's id: 164 is Blacksmithing)");
	g_ptr_array_add(parameters, field);
	field = venture_field_spec_new("professions", "Professions", VENTURE_FIELD_KIND_STRING);
	field->help = g_strdup("Only these professions, by name or Blizzard's id, comma separated: "
	                       "\"Alchemy, Inscription\"");
	g_ptr_array_add(parameters, field);
	field = venture_field_spec_new("skill_tier", "Expansion",
	                               VENTURE_FIELD_KIND_STRING);
	field->help = g_strdup("Only skill tiers whose name has this in it: \"Khaz Algar\" reads this "
	                       "expansion's recipes and none of the older ones");
	g_ptr_array_add(parameters, field);
	field = venture_field_spec_new("cursor", "Continue from", VENTURE_FIELD_KIND_STRING);
	field->help = g_strdup("What the last import said to continue with; empty starts at the top");
	g_ptr_array_add(parameters, field);
	field = venture_field_spec_new("create_products", "Make products", VENTURE_FIELD_KIND_BOOLEAN);
	field->help = g_strdup("Make a product for an item that has none; otherwise its recipes are skipped");
	g_ptr_array_add(parameters, field);
	field = venture_field_spec_new("venture_id", "Venture", VENTURE_FIELD_KIND_REFERENCE);
	field->help = g_strdup("The venture new products and recipes belong to; needed to make products");
	field->reference_type = g_strdup("venture");
	g_ptr_array_add(parameters, field);
	field = venture_field_spec_new("recipes", "Recipes known", VENTURE_FIELD_KIND_JSON);
	field->help = g_strdup("The recipes themselves, a JSON array of at most 2000 (tsmctl venture recipes "
	                       "sends them): Battle.net is not read, and the five parameters above that steer "
	                       "its walk are refused");
	g_ptr_array_add(parameters, field);

	/* A record action on the source, judged in its organization. Not
	 * stageable: it reads another system, and approval would read it
	 * again at a different moment. */
	action = g_object_new(VENTURE_TYPE_ACTION, "data-class", VENTURE_DATA_CLASS_TENANT,
	                      "type-name", "data_source", "name", BLIZZARD_ACTION_NAME,
	                      "label", "Import recipes",
	                      "description", "Read Battle.net's profession recipes, or the recipes handed "
	                                     "in as recipes, into recipe records linked to this source's "
	                                     "instruments and their products",
	                      "parameters", parameters, "stageable", FALSE,
	                      "roles", VENTURE_USER_ROLE_EDITOR, NULL);

	if (!venture_action_registry_register(venture_database_get_action_registry(database), action,
	                                      blizzard_import_allowed, blizzard_import_invoke, database,
	                                      NULL, error))
		return FALSE;

	g_clear_object(&action);
	g_clear_pointer(&parameters, g_ptr_array_unref);
	parameters = g_ptr_array_new_with_free_func((GDestroyNotify)venture_field_spec_free);
	field = venture_field_spec_new("cut_percent", "Cut %", VENTURE_FIELD_KIND_INTEGER);
	field->help = g_strdup("The auction house's share of a sale; 5 when left empty");
	g_ptr_array_add(parameters, field);
	field = venture_field_spec_new("duration_hours", "Listing hours", VENTURE_FIELD_KIND_INTEGER);
	field->help = g_strdup("12, 24 or 48: how long an auction is posted for, which sets the deposit; 48 "
	                       "when left empty");
	g_ptr_array_add(parameters, field);
	field = venture_field_spec_new("deposit_factor_percent", "Deposit %", VENTURE_FIELD_KIND_INTEGER);
	field->help = g_strdup("The deposit as a percent of the vendor price, replacing the classic shares");
	g_ptr_array_add(parameters, field);
	field = venture_field_spec_new("replace", "Replace", VENTURE_FIELD_KIND_BOOLEAN);
	field->help = g_strdup("Also replace a fee model a venue already names");
	g_ptr_array_add(parameters, field);

	/* Venue records are an editor's to write, as promoting one by hand
	 * is; the source names which store's venues. */
	action = g_object_new(VENTURE_TYPE_ACTION, "data-class", VENTURE_DATA_CLASS_TENANT,
	                      "type-name", "data_source", "name", BLIZZARD_FEES_ACTION_NAME,
	                      "label", "Set venue fees",
	                      "description", "Give every venue this source's store knows a venue record with the "
	                                     "wow_auction fee model, so scans and crafting count the auction "
	                                     "house's cut and deposit",
	                      "parameters", parameters, "stageable", FALSE,
	                      "roles", VENTURE_USER_ROLE_EDITOR, NULL);

	return venture_action_registry_register(venture_database_get_action_registry(database), action,
	                                        blizzard_fees_allowed, blizzard_fees_invoke, database,
	                                        NULL, error);
}

#endif /* VENTURE_HAVE_SQLITE */
