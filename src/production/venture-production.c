/*
 * venture-production.c - The production module's rules and crafting
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

#include <string.h>

#define VENTURE_PRODUCTION_STATE_KEY "venture-production-installed"

/* ==========================================================================
 * Shared
 * ========================================================================== */

/* "Healing Potion", or "product #7" when it cannot be read. */
static gchar *
production_name_of(
	VentureDatabase	*database,
	GType		 type,
	const gchar	*noun,
	gint64		 id
){
	g_autoptr(VentureEntity) record = NULL;

	if (id > 0)
		record = venture_database_get(database, type, id, NULL);

	if (NULL == record)
		return g_strdup_printf("%s #%" G_GINT64_FORMAT, noun, id);

	return venture_entity_get_display_name(record);
}

/*
 * Refuses a reference into another organization. The generic reference
 * check only asks whether the target exists; a recipe in one organization
 * that makes another's product would craft into books that are not its
 * own. Only a value being written is judged, the rule every reference
 * follows, so a row pointing somewhere since moved stays editable.
 */
static gboolean
production_same_organization(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	const gchar	 *property,
	GType		  target_type,
	const gchar	 *label,
	GError		**error
){
	g_autoptr(VentureEntity) target = NULL;
	gint64 target_id;

	target_id = 0;
	g_object_get(entity, property, &target_id, NULL);

	if (target_id <= 0)
		return TRUE;

	if (NULL != previous)
	{
		gint64 was;

		was = 0;
		g_object_get(previous, property, &was, NULL);

		if (was == target_id)
			return TRUE;
	}

	/* A target that does not exist is the reference check's to refuse,
	 * with its own message; this one only judges where it lives. */
	target = venture_database_get(database, target_type, target_id, NULL);

	if ((NULL != target) &&
	    (venture_entity_get_organization_id(target) !=
	     venture_entity_get_organization_id(entity)))
	{
		venture_set_error_validation(error, label,
			"#%" G_GINT64_FORMAT " belongs to another organization",
			target_id);
		return FALSE;
	}

	return TRUE;
}

/* The live components of @recipe_id, oldest first. */
static GPtrArray *
production_components(
	VentureDatabase	 *database,
	gint64		  recipe_id,
	GError		**error
){
	g_autoptr(VentureQuery) query = NULL;

	query = venture_query_new(VENTURE_TYPE_RECIPE_COMPONENT);
	venture_query_set_limit(query, 0);

	if (!venture_query_add_filter_int(query, "recipe-id", VENTURE_FILTER_OP_EQ,
	                                  recipe_id, error) ||
	    !venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, error))
		return NULL;

	return venture_database_find(database, query, error);
}

/* ==========================================================================
 * Recipes
 * ========================================================================== */

/*
 * A recipe makes a product, at least one unit of it per batch, in its own
 * organization, and is not one of its own ingredients. The output is
 * declared NOT_NULL, but the generic check covers strings and times only:
 * an integer reference reads 0 when unset, and a recipe that makes
 * nothing would craft stock out of thin air into no item at all.
 */
static gboolean
venture_production_validate_recipe(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	gpointer	  user_data,
	GError		**error
){
	gint64 output_id;
	gint64 output_quantity;

	(void)user_data;

	g_object_get(entity, "output-product-id", &output_id,
	             "output-quantity", &output_quantity, NULL);

	if (output_id <= 0)
	{
		venture_set_error_validation(error, "Makes",
			"is required: a recipe makes a product");
		return FALSE;
	}

	if (output_quantity < 1)
	{
		venture_set_error_validation(error, "Batch size",
			"must be at least 1: one batch makes something");
		return FALSE;
	}

	if (!production_same_organization(database, entity, previous, "output-product-id",
	                                  VENTURE_TYPE_PRODUCT, "Makes", error) ||
	    !production_same_organization(database, entity, previous, "venture-id",
	                                  VENTURE_TYPE_VENTURE, "Venture", error))
		return FALSE;

	/* The loop the component validator cannot see: an existing recipe
	 * changed to make something it already takes. A craft of it would
	 * issue and receive the same stock in one breath. */
	if (venture_entity_is_persisted(entity))
	{
		g_autoptr(GPtrArray) components = NULL;
		guint i;

		components = production_components(database,
		                                   venture_entity_get_id(entity), error);

		if (NULL == components)
			return FALSE;

		for (i = 0; i < components->len; i++)
		{
			gint64 product_id;

			product_id = 0;
			g_object_get(g_ptr_array_index(components, i), "product-id",
			             &product_id, NULL);

			if (product_id == output_id)
			{
				g_autofree gchar *name = NULL;

				name = production_name_of(database, VENTURE_TYPE_PRODUCT,
				                          "product", output_id);
				venture_set_error_validation(error, "Makes",
					"%s is one of this recipe's components; a recipe cannot "
					"make what it takes", name);
				return FALSE;
			}
		}
	}

	return TRUE;
}

/* ==========================================================================
 * Components
 * ========================================================================== */

/*
 * A component's rules, in the order a person would fix them:
 *
 *  - a recipe and a product (NOT_NULL integers read 0 unset, as above);
 *  - at least one unit;
 *  - the recipe's organization, and a product in it;
 *  - not the recipe's own output: a craft would consume what it makes;
 *  - one line per product. A second line is refused rather than merged:
 *    merging would silently change a quantity somebody typed, and which of
 *    two lines' reusable flags wins is a guess. The refusal says to change
 *    the existing line instead.
 */
static gboolean
venture_production_validate_component(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	gpointer	  user_data,
	GError		**error
){
	g_autoptr(VentureEntity) recipe = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) twins = NULL;
	gint64 recipe_id;
	gint64 product_id;
	gint64 quantity;
	gint64 output_id;
	guint i;

	(void)user_data;

	g_object_get(entity, "recipe-id", &recipe_id, "product-id", &product_id,
	             "quantity", &quantity, NULL);

	if (recipe_id <= 0)
	{
		venture_set_error_validation(error, "Recipe", "is required");
		return FALSE;
	}

	if (product_id <= 0)
	{
		venture_set_error_validation(error, "Product", "is required");
		return FALSE;
	}

	if (quantity < 1)
	{
		venture_set_error_validation(error, "Quantity",
			"must be at least 1: a line takes something");
		return FALSE;
	}

	recipe = venture_database_get(database, VENTURE_TYPE_RECIPE, recipe_id, NULL);

	if (NULL == recipe)
	{
		venture_set_error_validation(error, "Recipe",
			"#%" G_GINT64_FORMAT " does not exist", recipe_id);
		return FALSE;
	}

	/* A line lives where its recipe does. Held always, not only when
	 * written: a component is part of its recipe, not a reference out. */
	if (venture_entity_get_organization_id(recipe) !=
	    venture_entity_get_organization_id(entity))
	{
		venture_set_error_validation(error, "Recipe",
			"#%" G_GINT64_FORMAT " belongs to another organization",
			recipe_id);
		return FALSE;
	}

	if (!production_same_organization(database, entity, previous, "product-id",
	                                  VENTURE_TYPE_PRODUCT, "Product", error))
		return FALSE;

	g_object_get(recipe, "output-product-id", &output_id, NULL);

	if (output_id == product_id)
	{
		g_autofree gchar *name = NULL;

		name = production_name_of(database, VENTURE_TYPE_PRODUCT, "product",
		                          product_id);
		venture_set_error_validation(error, "Product",
			"%s is what this recipe makes; a recipe cannot take its own "
			"output", name);
		return FALSE;
	}

	query = venture_query_new(VENTURE_TYPE_RECIPE_COMPONENT);
	venture_query_set_limit(query, 0);

	if (!venture_query_add_filter_int(query, "recipe-id", VENTURE_FILTER_OP_EQ,
	                                  recipe_id, error) ||
	    !venture_query_add_filter_int(query, "product-id", VENTURE_FILTER_OP_EQ,
	                                  product_id, error))
		return FALSE;

	twins = venture_database_find(database, query, error);

	if (NULL == twins)
		return FALSE;

	for (i = 0; i < twins->len; i++)
	{
		VentureEntity *twin;

		twin = g_ptr_array_index(twins, i);

		if (venture_entity_get_id(twin) != venture_entity_get_id(entity))
		{
			g_autofree gchar *name = NULL;

			name = production_name_of(database, VENTURE_TYPE_PRODUCT,
			                          "product", product_id);
			venture_set_error_validation(error, "Product",
				"this recipe already takes %s (component #%" G_GINT64_FORMAT
				"); change that line's quantity instead of adding another",
				name, venture_entity_get_id(twin));
			return FALSE;
		}
	}

	return TRUE;
}

/* ==========================================================================
 * Finding stock
 * ========================================================================== */

gboolean
venture_production_find_stock(
	VentureDatabase	 *database,
	gint64		  organization_id,
	gint64		  product_id,
	gint64		  location_id,
	gint64		 *out_item_id,
	GError		**error
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) items = NULL;
	g_autofree gchar *product = NULL;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), FALSE);
	g_return_val_if_fail(NULL != out_item_id, FALSE);

	*out_item_id = 0;

	query = venture_query_new(VENTURE_TYPE_INVENTORY_ITEM);
	venture_query_set_organization(query, organization_id);
	venture_query_set_limit(query, 0);

	if (!venture_query_add_filter_int(query, "product-id", VENTURE_FILTER_OP_EQ,
	                                  product_id, error))
		return FALSE;

	/* Exactly this location, not its children: "the bank" and "a bag in
	 * the bank" are two places, and taking from the one not named is the
	 * mistake a location parameter exists to prevent. */
	if ((location_id > 0) &&
	    !venture_query_add_filter_int(query, "location-id", VENTURE_FILTER_OP_EQ,
	                                  location_id, error))
		return FALSE;

	if (!venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, error))
		return FALSE;

	items = venture_database_find(database, query, error);

	if (NULL == items)
		return FALSE;

	if (1 == items->len)
	{
		*out_item_id = venture_entity_get_id(g_ptr_array_index(items, 0));
		return TRUE;
	}

	product = production_name_of(database, VENTURE_TYPE_PRODUCT, "product",
	                             product_id);

	if (0 == items->len)
	{
		if (location_id > 0)
		{
			g_autofree gchar *place = NULL;

			place = production_name_of(database, VENTURE_TYPE_LOCATION,
			                           "location", location_id);
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			            "No stock of %s is kept at %s; create an inventory item "
			            "for it there (product_id=%" G_GINT64_FORMAT
			            " location_id=%" G_GINT64_FORMAT ")",
			            product, place, product_id, location_id);
		}
		else
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			            "No stock of %s is kept anywhere; create an inventory "
			            "item for it (product_id=%" G_GINT64_FORMAT ")",
			            product, product_id);

		return FALSE;
	}

	/* More than one: name them, so the fix is one parameter away. */
	{
		g_autoptr(GString) places = NULL;
		guint i;

		places = g_string_new(NULL);

		for (i = 0; i < items->len; i++)
		{
			gint64 where;

			where = 0;
			g_object_get(g_ptr_array_index(items, i), "location-id", &where, NULL);

			if (i > 0)
				g_string_append(places, ", ");

			if (where > 0)
			{
				g_autofree gchar *place = NULL;

				place = production_name_of(database, VENTURE_TYPE_LOCATION,
				                           "location", where);
				g_string_append_printf(places, "%s (location_id=%" G_GINT64_FORMAT ")",
				                       place, where);
			}
			else
				g_string_append_printf(places, "item #%" G_GINT64_FORMAT " with no location",
				                       venture_entity_get_id(g_ptr_array_index(items, i)));
		}

		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "%s is kept in %u places -- %s; name the location_id to "
		            "craft from", product, items->len, places->str);
	}

	return FALSE;
}

/* ==========================================================================
 * Crafting
 * ========================================================================== */

static gboolean
production_item_allows_negative(
	VentureDatabase	*database,
	gint64		 item_id
){
	g_autoptr(VentureEntity) item = NULL;
	gboolean allow;

	allow = FALSE;
	item = venture_database_get(database, VENTURE_TYPE_INVENTORY_ITEM, item_id, NULL);

	if (NULL != item)
		g_object_get(item, "allow-negative", &allow, NULL);

	return allow;
}

/* Everything a craft checks before it writes, with the draws it will make. */
static gboolean
production_plan(
	VentureDatabase	 *database,
	VentureEntity	 *recipe,
	gint64		  times,
	gint64		  location_id,
	GArray		 *draws,
	gint64		 *out_output_item,
	gint64		 *out_output_quantity,
	GError		**error
){
	VentureInventoryService *inventory;
	g_autoptr(GPtrArray) components = NULL;
	g_autofree gchar *recipe_name = NULL;
	gint64 organization_id;
	gint64 output_id;
	gint64 output_quantity;
	gboolean active;
	guint i;

	inventory = venture_inventory_service_get(database);
	organization_id = venture_entity_get_organization_id(recipe);
	recipe_name = venture_entity_get_display_name(recipe);

	g_object_get(recipe, "output-product-id", &output_id,
	             "output-quantity", &output_quantity, "active", &active, NULL);

	/* Inactive is a decision somebody made; crafting one anyway would
	 * make it not one. */
	if (!active)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "%s is not active; tick Active to craft it", recipe_name);
		return FALSE;
	}

	if ((output_id <= 0) || (output_quantity < 1))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "%s makes nothing; set what it makes and its batch size",
		            recipe_name);
		return FALSE;
	}

	if (output_quantity > (G_MAXINT64 / times))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "That many batches is more units than can be counted");
		return FALSE;
	}

	components = production_components(database, venture_entity_get_id(recipe), error);

	if (NULL == components)
		return FALSE;

	/* Something from nothing is not a craft -- it is stock arriving, and
	 * an adjustment says so honestly. */
	if (0 == components->len)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "%s has no components; add what it takes first (or record "
		            "stock that simply arrived as an adjustment)", recipe_name);
		return FALSE;
	}

	for (i = 0; i < components->len; i++)
	{
		VentureEntity *component;
		g_autoptr(GError) local_error = NULL;
		gint64 product_id;
		gint64 quantity;
		gint64 item_id;
		gint64 on_hand;
		gboolean reusable;

		component = g_ptr_array_index(components, i);
		g_object_get(component, "product-id", &product_id, "quantity", &quantity,
		             "reusable", &reusable, NULL);

		if (!venture_production_find_stock(database, organization_id, product_id,
		                                   location_id, &item_id, error))
			return FALSE;

		on_hand = venture_inventory_service_on_hand(inventory, item_id, NULL,
		                                            &local_error);

		if (NULL != local_error)
		{
			g_propagate_error(error, g_steal_pointer(&local_error));
			return FALSE;
		}

		if (reusable)
		{
			/* A tool is needed once however many batches are made, and
			 * it has to really be there: allow-negative is for counts
			 * that lag behind the shelf, and a hammer nobody has is not
			 * a hammer that has not been counted yet. */
			if (on_hand < quantity)
			{
				g_autofree gchar *name = NULL;

				name = production_name_of(database, VENTURE_TYPE_PRODUCT,
				                          "product", product_id);
				g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
				            "%s needs %s on hand (%" G_GINT64_FORMAT "), and "
				            "%" G_GINT64_FORMAT " %s there; it is not used up, "
				            "but it has to be there", recipe_name, name,
				            quantity, on_hand, (1 == on_hand) ? "is" : "are");
				return FALSE;
			}

			continue;
		}

		if (quantity > (G_MAXINT64 / times))
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			                    "That many batches is more units than can be counted");
			return FALSE;
		}

		if ((on_hand < quantity * times) &&
		    !production_item_allows_negative(database, item_id))
		{
			g_autofree gchar *name = NULL;

			name = production_name_of(database, VENTURE_TYPE_PRODUCT,
			                          "product", product_id);
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			            "Short of %s: %" G_GINT64_FORMAT " %s of %s needs %"
			            G_GINT64_FORMAT " and %" G_GINT64_FORMAT " %s on hand",
			            name, times, (1 == times) ? "batch" : "batches",
			            recipe_name, quantity * times, on_hand,
			            (1 == on_hand) ? "is" : "are");
			return FALSE;
		}

		{
			VentureInventoryDraw draw;

			draw.inventory_item_id = item_id;
			draw.quantity = quantity * times;
			g_array_append_val(draws, draw);
		}
	}

	if (!venture_production_find_stock(database, organization_id, output_id,
	                                   location_id, out_output_item, error))
	{
		g_prefix_error(error, "Nowhere to put what %s makes: ", recipe_name);
		return FALSE;
	}

	*out_output_quantity = output_quantity * times;

	return TRUE;
}

gboolean
venture_production_craft(
	VentureDatabase		 *database,
	gint64			  recipe_id,
	gint64			  times,
	gint64			  location_id,
	GDateTime		 *occurred_at,
	const VentureActor	 *actor,
	VentureEntity		**out_txn,
	GError			**error
){
	g_autoptr(VentureEntity) recipe = NULL;
	g_autoptr(GArray) draws = NULL;
	g_autoptr(VentureEntity) txn = NULL;
	g_autofree gchar *reference = NULL;
	gint64 output_item;
	gint64 output_quantity;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), FALSE);

	if (NULL != out_txn)
		*out_txn = NULL;

	if ((times < 1) || (times > VENTURE_PRODUCTION_MAX_TIMES))
	{
		venture_set_error_validation(error, "times",
			"must be 1 to %d batches", VENTURE_PRODUCTION_MAX_TIMES);
		return FALSE;
	}

	/* One transaction for the reads and the writes: the stock counted
	 * above the writes is the stock the writes take, and a refusal or a
	 * failure halfway leaves nothing behind. Inside the action framework
	 * this joins the transaction it already holds. */
	if (!venture_database_begin(database, error))
		return FALSE;

	recipe = venture_database_get(database, VENTURE_TYPE_RECIPE, recipe_id, error);

	if (NULL == recipe)
		goto fail;

	if (venture_entity_is_deleted(recipe))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "Recipe #%" G_GINT64_FORMAT " was deleted", recipe_id);
		goto fail;
	}

	draws = g_array_new(FALSE, FALSE, sizeof(VentureInventoryDraw));
	output_item = 0;
	output_quantity = 0;

	if (!production_plan(database, recipe, times, location_id, draws,
	                     &output_item, &output_quantity, error))
		goto fail;

	/* The recipe, not the craft: every craft of recipe 12 is found by
	 * "recipe:12", which is the question anybody asks of it later. */
	reference = g_strdup_printf("recipe:%" G_GINT64_FORMAT, recipe_id);

	if (!venture_inventory_service_produce(venture_inventory_service_get(database),
	                                       (const VentureInventoryDraw *)(gpointer)draws->data,
	                                       draws->len, output_item, output_quantity,
	                                       occurred_at, reference, actor, &txn, NULL,
	                                       error))
		goto fail;

	if (!venture_database_commit(database, error))
		return FALSE;

	if (NULL != out_txn)
		*out_txn = g_steal_pointer(&txn);

	return TRUE;

fail:
	venture_database_rollback(database);
	return FALSE;
}

/* ==========================================================================
 * The craft action
 * ========================================================================== */

/*
 * Offered only on an active recipe, so the page's action panel does not
 * show a Craft button that can only be refused. Must not write.
 */
static gboolean
production_craft_allowed(
	VentureAction		 *action,
	VentureEntity		 *entity,
	const VentureActor	 *actor,
	GError			**error
){
	gboolean active;

	(void)action;
	(void)actor;

	active = FALSE;
	g_object_get(entity, "active", &active, NULL);

	if (!active)
	{
		venture_set_error_validation(error, "active",
			"An inactive recipe cannot be crafted");
		return FALSE;
	}

	return TRUE;
}

static VentureEntity *
production_craft_invoke(
	VentureAction		 *action,
	VentureEntity		 *entity,
	GHashTable		 *params,
	const VentureActor	 *actor,
	GError			**error
){
	g_autoptr(GDateTime) occurred_at = NULL;
	VentureEntity *txn;
	JsonNode *node;
	gint64 times;
	gint64 location_id;

	times = 1;
	location_id = 0;
	txn = NULL;

	/* The framework has checked every kind already: an integer is a JSON
	 * integer here, never a string of digits. */
	node = g_hash_table_lookup(params, "times");

	if ((NULL != node) && !JSON_NODE_HOLDS_NULL(node))
		times = json_node_get_int(node);

	node = g_hash_table_lookup(params, "location_id");

	if ((NULL != node) && !JSON_NODE_HOLDS_NULL(node))
		location_id = json_node_get_int(node);

	node = g_hash_table_lookup(params, "occurred_at");

	if ((NULL != node) && !JSON_NODE_HOLDS_NULL(node) &&
	    !venture_string_is_empty(json_node_get_string(node)))
	{
		occurred_at = venture_time_from_string(json_node_get_string(node), error);

		if (NULL == occurred_at)
			return NULL;
	}

	if (!venture_production_craft(venture_action_get_data(action),
	                              venture_entity_get_id(entity), times, location_id,
	                              occurred_at, actor, &txn, error))
		return NULL;

	return txn;
}

static void
production_register_craft(VentureDatabase *database)
{
	g_autoptr(GPtrArray) parameters = NULL;
	g_autoptr(VentureAction) action = NULL;
	g_autoptr(GError) error = NULL;
	VentureFieldSpec *field;

	parameters = g_ptr_array_new_with_free_func((GDestroyNotify)venture_field_spec_free);

	field = venture_field_spec_new("times", "Batches", VENTURE_FIELD_KIND_INTEGER);
	field->help = g_strdup("How many batches to make; one when left empty");
	field->has_min = TRUE;
	field->min_value = 1;
	field->has_max = TRUE;
	field->max_value = VENTURE_PRODUCTION_MAX_TIMES;
	g_ptr_array_add(parameters, field);

	field = venture_field_spec_new("location_id", "Location", VENTURE_FIELD_KIND_REFERENCE);
	field->help = g_strdup("Where to take the components from and put what is made; "
	                       "needed when a product is kept in more than one place");
	field->reference_type = g_strdup("location");
	g_ptr_array_add(parameters, field);

	field = venture_field_spec_new("occurred_at", "Made at", VENTURE_FIELD_KIND_DATETIME);
	field->help = g_strdup("When it was made; now when left empty");
	g_ptr_array_add(parameters, field);

	/*
	 * A record action, not a type-level one: the recipe is the subject,
	 * so the access policy judges the recipe's own organization and no
	 * organization_id parameter is needed to place it.
	 *
	 * Stageable, unlike the factory's rollback: that is two writes the
	 * queue would have to hold as records, while a staged action holds
	 * only its parameters and approval performs the whole craft afresh in
	 * one transaction -- recounting the stock as it is at approval, not as
	 * it was when proposed.
	 */
	action = g_object_new(VENTURE_TYPE_ACTION, "data-class", VENTURE_DATA_CLASS_TENANT,
		"type-name", "recipe", "name", "craft", "label", "Craft",
		"description", "Make batches of this recipe: consume its components and "
		"add what it makes to stock, carrying their cost, in one transaction",
		"parameters", parameters, "stageable", TRUE,
		"roles", VENTURE_USER_ROLE_EDITOR, NULL);

	if (!venture_action_registry_register(venture_database_get_action_registry(database),
	                                      action, production_craft_allowed,
	                                      production_craft_invoke, database, NULL, &error))
		g_error("Craft action registration: %s", error->message);
}

void
venture_production_install(VentureContext *context)
{
	VentureDatabase *database;

	g_return_if_fail(VENTURE_IS_CONTEXT(context));

	database = venture_context_get_database(context);

	/* The tests build several contexts over one database; validators and
	 * actions are per database, so the second one must add nothing. */
	if (NULL != g_object_get_data(G_OBJECT(database), VENTURE_PRODUCTION_STATE_KEY))
		return;

	g_object_set_data(G_OBJECT(database), VENTURE_PRODUCTION_STATE_KEY,
	                  GINT_TO_POINTER(1));

	venture_database_add_save_validator(database, VENTURE_TYPE_RECIPE,
	                                    venture_production_validate_recipe,
	                                    NULL, NULL);
	venture_database_add_save_validator(database, VENTURE_TYPE_RECIPE_COMPONENT,
	                                    venture_production_validate_component,
	                                    NULL, NULL);
	production_register_craft(database);
}
