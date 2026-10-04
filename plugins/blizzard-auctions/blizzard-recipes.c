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

/*
 * One recipe JSON as records, inside the caller's transaction. A recipe
 * that cannot be made -- it makes no item, an item has no product -- is
 * a skip with *@out_reason; FALSE is an error (the caller decides whether
 * it stops the import).
 */
static gboolean
blizzard_import_recipe(
	BlizzardImport		 *import,
	JsonObject		 *recipe,
	BlizzardRecipeOutcome	 *out_outcome,
	gchar			**out_reason,
	GError			**error
){
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(GArray) lines = NULL;
	JsonObject *crafted;
	JsonObject *quantity;
	JsonArray *reagents;
	const gchar *name;
	const gchar *locale;
	gint64 recipe_id;
	gint64 output_id;
	gint64 output_quantity;
	gboolean changed;
	guint i;
	guint j;

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
		BlizzardLine merged;
		gboolean found = FALSE;
		gint64 count;

		line = JSON_NODE_HOLDS_OBJECT(element) ? json_node_get_object(element) : NULL;
		reagent = ((NULL != line) && json_object_has_member(line, "reagent"))
			? json_object_get_object_member(line, "reagent") : NULL;
		count = blizzard_int(line, "quantity", 1);

		if ((NULL == reagent) || (blizzard_int(reagent, "id", 0) <= 0) || (count < 1))
			continue;

		if (!blizzard_item_product(import, blizzard_int(reagent, "id", 0),
		                           blizzard_text(reagent, "name", locale), &merged.product_id,
		                           out_reason, error))
			return FALSE;

		if (0 == merged.product_id)
			return TRUE;

		/* Two slots of one reagent are one line: the component
		 * validator allows one line per product. */
		for (j = 0; j < lines->len; j++)
		{
			BlizzardLine *existing = &g_array_index(lines, BlizzardLine, j);

			if (existing->product_id == merged.product_id)
			{
				existing->quantity += count;
				found = TRUE;
			}
		}

		if (!found)
		{
			merged.quantity = count;
			g_array_append_val(lines, merged);
		}
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

		if (!venture_database_save(import->database, record, import->actor, error))
			return FALSE;

		*out_outcome = BLIZZARD_RECIPE_CREATED;
	}
	else
	{
		gint64 stored;

		g_object_get(record, "output-quantity", &stored, NULL);

		if (stored != output_quantity)
		{
			g_object_set(record, "output-quantity", output_quantity, NULL);

			if (!venture_database_save(import->database, record, import->actor, error))
				return FALSE;

			changed = TRUE;
		}
	}

	/* The lines: one per reagent, a quantity brought up to date. A line
	 * someone added by hand (a tool) is left alone, and so is a deleted
	 * one -- a new line is made beside it. */
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

		changed = TRUE;
	}

	if (BLIZZARD_RECIPE_CREATED != *out_outcome)
		*out_outcome = changed ? BLIZZARD_RECIPE_UPDATED : BLIZZARD_RECIPE_UNCHANGED;

	return TRUE;
}

/*
 * One recipe in a transaction of its own, so a failure keeps every recipe
 * before it. A save the records refused (a validator, a missing target)
 * skips this recipe and says why; anything else stops the import.
 */
static gboolean
blizzard_import_one(
	BlizzardImport	 *import,
	JsonObject	 *recipe,
	GError		**error
){
	g_autoptr(GError) local_error = NULL;
	g_autofree gchar *reason = NULL;
	BlizzardRecipeOutcome outcome;

	if (!venture_database_begin(import->database, error))
		return FALSE;

	if (!blizzard_import_recipe(import, recipe, &outcome, &reason, &local_error))
	{
		venture_database_rollback(import->database);

		if (!g_error_matches(local_error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION) &&
		    !g_error_matches(local_error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND))
		{
			g_propagate_error(error, g_steal_pointer(&local_error));
			return FALSE;
		}

		g_clear_pointer(&reason, g_free);
		reason = g_strdup_printf("%s (recipe %" G_GINT64_FORMAT "): %s",
		                         venture_json_object_get_string(recipe, "name", "a recipe"),
		                         blizzard_int(recipe, "id", 0), local_error->message);
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
static gchar *
blizzard_import_walk(
	BlizzardImport	 *import,
	gint64		  only_profession,
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
		    ((start_profession > 0) && (profession_id < start_profession)))
			continue;

		path = g_strdup_printf("/data/wow/profession/%" G_GINT64_FORMAT, profession_id);
		profession = blizzard_import_require(import, path, "a profession", &profession_node, error);

		if (NULL == profession)
			return NULL;

		tiers = blizzard_import_ids(json_object_has_member(profession, "skill_tiers")
		                            ? json_object_get_array_member(profession, "skill_tiers") : NULL,
		                            TRUE);

		for (t = 0; t < tiers->len; t++)
		{
			g_autoptr(JsonNode) tier_node = NULL;
			g_autoptr(GArray) recipes = NULL;
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

			tier_path = g_strdup_printf("/data/wow/profession/%" G_GINT64_FORMAT "/skill-tier/%"
			                            G_GINT64_FORMAT, profession_id, tier_id);
			tier = blizzard_import_require(import, tier_path, "a skill tier", &tier_node, error);

			if (NULL == tier)
				return NULL;

			/* Every category's recipes, in Blizzard's order. */
			recipes = g_array_new(FALSE, FALSE, sizeof(gint64));
			categories = json_object_has_member(tier, "categories")
				? json_object_get_array_member(tier, "categories") : NULL;

			for (c = 0; (NULL != categories) && (c < json_array_get_length(categories)); c++)
			{
				JsonNode *element = json_array_get_element(categories, c);
				g_autoptr(GArray) some = NULL;

				if (!JSON_NODE_HOLDS_OBJECT(element) ||
				    !json_object_has_member(json_node_get_object(element), "recipes"))
					continue;

				some = blizzard_import_ids(json_object_get_array_member(json_node_get_object(element),
				                                                        "recipes"), FALSE);
				g_array_append_vals(recipes, some->data, some->len);
			}

			first = ((profession_id == start_profession) && (tier_id == start_tier))
				? (guint)MIN(start_offset, (gint64)recipes->len) : 0;

			for (r = first; r < recipes->len; r++)
			{
				g_autoptr(JsonNode) recipe_node = NULL;
				g_autofree gchar *recipe_path = NULL;
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

				if (!blizzard_import_one(import, recipe, error))
					return NULL;
			}
		}
	}

	return g_strdup("");
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
	BlizzardImport import;
	JsonNode *node;
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

	memset(&import, 0, sizeof(import));
	node = blizzard_param(params, "create_products");
	import.create_products = (NULL != node) && json_node_get_boolean(node);
	node = blizzard_param(params, "venture_id");
	import.venture_id = (NULL != node) ? json_node_get_int(node) : 0;

	if (import.create_products && (import.venture_id <= 0))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "create_products needs venture_id: a product belongs to a venture");
		return NULL;
	}

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

	if (NULL == import.frozen)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
		                    "The source's provider did not freeze");
		return NULL;
	}

	{
		g_autoptr(GError) walk_error = NULL;

		next = blizzard_import_walk(&import, only_profession, cursor, (guint)budget, &walk_error);

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

	/* A record action on the source, judged in its organization. Not
	 * stageable: it reads another system, and approval would read it
	 * again at a different moment. */
	action = g_object_new(VENTURE_TYPE_ACTION, "data-class", VENTURE_DATA_CLASS_TENANT,
	                      "type-name", "data_source", "name", BLIZZARD_ACTION_NAME,
	                      "label", "Import recipes",
	                      "description", "Read Battle.net's profession recipes into recipe records, "
	                                     "linked to this source's instruments and their products",
	                      "parameters", parameters, "stageable", FALSE,
	                      "roles", VENTURE_USER_ROLE_EDITOR, NULL);

	return venture_action_registry_register(venture_database_get_action_registry(database), action,
	                                        blizzard_import_allowed, blizzard_import_invoke, database,
	                                        NULL, error);
}

#endif /* VENTURE_HAVE_SQLITE */
