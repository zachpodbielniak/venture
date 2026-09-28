/*
 * test-production.c - Recipes, crafting and recipe margin
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Production turns stock into other stock, and its figures are only worth
 * reading if three things hold. A recipe must agree with itself (it makes
 * one product and never takes it). A craft must be all or nothing -- half
 * a craft is stock that vanished or appeared with no reason -- and must
 * carry the consumed cost to the made units exactly, or the cost of goods
 * sold drifts from what the materials cost. And the margin report must
 * keep to the books' rules: money per currency, a missing price named and
 * not read as zero. These tests hold all three.
 */

#include <venture.h>

#include <libsoup/soup.h>
#include <string.h>
#include <unistd.h>

#include "venture-test-util.h"

typedef struct
{
	VentureConfig	*config;
	VentureDatabase	*database;
	VentureContext	*context;
	gint64		 organization_id;
	gint64		 venture_id;
} Fixture;

#define ID(record) (venture_entity_get_id(VENTURE_ENTITY(record)))

/* Saves, expecting success; the refusal's reason is the failure. */
static void
save(
	Fixture		*fixture,
	gpointer	 record
){
	g_autoptr(GError) error = NULL;

	if (!venture_database_save(fixture->database, VENTURE_ENTITY(record), NULL, &error))
		g_error("save refused: %s", error->message);
}

/* Saves, expecting a validation refusal whose message holds @fragment. */
static void
save_refused(
	Fixture		*fixture,
	gpointer	 record,
	const gchar	*fragment
){
	g_autoptr(GError) error = NULL;
	gboolean saved;

	saved = venture_database_save(fixture->database, VENTURE_ENTITY(record), NULL, &error);
	g_assert_nonnull(error);
	g_assert_false(saved);

	if (NULL == strstr(error->message, fragment))
		g_error("expected \"%s\" in: %s", fragment, error->message);
}

static void
fixture_set_up(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureVenture) venture = NULL;

	(void)user_data;

	fixture->config = venture_config_new();
	fixture->database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	fixture->context = venture_context_new(fixture->config, fixture->database);
	g_assert_true(venture_database_migrate(fixture->database,
		venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);

	fixture->organization_id =
		venture_context_get_default_organization_id(fixture->context);

	/* A game's gold: four minor digits, never to be added to dollars. */
	g_assert_true(venture_currency_register("GOLD", 4, NULL, FALSE, NULL, &error));
	g_assert_no_error(error);

	venture = venture_venture_new();
	g_object_set(venture, "name", "Workshop", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(venture), fixture->organization_id);
	save(fixture, venture);
	fixture->venture_id = ID(venture);
}

static void
fixture_tear_down(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureConfig) everything = NULL;
	g_autoptr(VentureModuleRegistry) registry = NULL;

	(void)user_data;

	g_clear_object(&fixture->context);
	g_clear_object(&fixture->database);
	g_clear_object(&fixture->config);
	venture_currency_clear_registered();

	/* The entity registry is process-wide; a test that switched a module
	 * off must not leave it off for the next. */
	everything = venture_config_new();
	registry = venture_module_registry_new();
	venture_module_registry_register_builtins(registry);
	venture_module_registry_configure(registry, everything, NULL);
	venture_module_registry_apply(registry, venture_entity_registry_get_default());
}

static VentureMoney *
money_of(const gchar *text)
{
	VentureMoney *money;

	money = venture_money_from_string(text, NULL, NULL);
	g_assert_nonnull(money);

	return money;
}

/* --- Records --------------------------------------------------------------- */

static gint64
organization(
	Fixture		*fixture,
	const gchar	*slug
){
	g_autoptr(VentureEntity) other = NULL;

	other = VENTURE_ENTITY(venture_organization_new());
	g_object_set(other, "name", slug, "slug", slug, NULL);
	save(fixture, other);

	return ID(other);
}

/* A saved product in @organization_id (0 for the default), with an
 * optional list price and unit cost. */
static gint64
product_in(
	Fixture		*fixture,
	gint64		 organization_id,
	const gchar	*name,
	const gchar	*list_price,
	const gchar	*cost
){
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(VentureMoney) list = NULL;
	g_autoptr(VentureMoney) unit = NULL;

	list = (NULL != list_price) ? money_of(list_price) : NULL;
	unit = (NULL != cost) ? money_of(cost) : NULL;
	record = VENTURE_ENTITY(venture_product_new());
	g_object_set(record, "name", name, "list-price", list, "cost", unit, NULL);
	venture_entity_set_organization_id(record, (0 != organization_id)
		? organization_id : fixture->organization_id);
	save(fixture, record);

	return ID(record);
}

static gint64
product(
	Fixture		*fixture,
	const gchar	*name
){
	return product_in(fixture, 0, name, NULL, NULL);
}

static gint64
location_in(
	Fixture		*fixture,
	gint64		 organization_id,
	const gchar	*name
){
	g_autoptr(VentureEntity) record = NULL;

	record = VENTURE_ENTITY(venture_location_new());
	g_object_set(record, "name", name, "active", TRUE, NULL);
	venture_entity_set_organization_id(record, (0 != organization_id)
		? organization_id : fixture->organization_id);
	save(fixture, record);

	return ID(record);
}

/* A saved inventory item for @product_id, at @location_id (0: none). */
static gint64
item_in(
	Fixture		*fixture,
	gint64		 organization_id,
	gint64		 product_id,
	gint64		 location_id,
	const gchar	*unit_cost
){
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(VentureMoney) cost = NULL;

	cost = (NULL != unit_cost) ? money_of(unit_cost) : NULL;
	record = VENTURE_ENTITY(venture_inventory_item_new());
	g_object_set(record, "product-id", product_id, "location-id", location_id,
	             "unit-cost", cost, NULL);
	venture_entity_set_organization_id(record, (0 != organization_id)
		? organization_id : fixture->organization_id);
	save(fixture, record);

	return ID(record);
}

static gint64
item(
	Fixture		*fixture,
	gint64		 product_id
){
	return item_in(fixture, 0, product_id, 0, NULL);
}

/* Receives @quantity units at @unit_cost: one FIFO cost layer. */
static void
receive(
	Fixture		*fixture,
	gint64		 item_id,
	gint64		 quantity,
	const gchar	*unit_cost
){
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureMoney) cost = NULL;
	g_autoptr(GDateTime) when = NULL;

	cost = money_of(unit_cost);
	when = venture_time_from_string("2026-03-01", NULL);
	g_assert_true(venture_inventory_service_receive(
		venture_inventory_service_get(fixture->database), item_id, quantity,
		cost, when, 0, "seed", NULL, &error));
	g_assert_no_error(error);
}

static gint64
on_hand(
	Fixture		*fixture,
	gint64		 item_id
){
	g_autoptr(GError) error = NULL;
	gint64 units;

	units = venture_inventory_service_on_hand(
		venture_inventory_service_get(fixture->database), item_id, NULL, &error);
	g_assert_no_error(error);

	return units;
}

/* How many rows of @type exist, and the units left in every cost layer:
 * together, a fingerprint of the stock a refused craft must not touch. */
static guint
count_rows(
	Fixture	*fixture,
	GType	 type
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;

	query = venture_query_new(type);
	venture_query_set_limit(query, 0);
	rows = venture_database_find(fixture->database, query, NULL);
	g_assert_nonnull(rows);

	return rows->len;
}

static gint64
layer_units(Fixture *fixture)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	gint64 total;
	guint i;

	query = venture_query_new(VENTURE_TYPE_INVENTORY_COST_LAYER);
	venture_query_set_limit(query, 0);
	rows = venture_database_find(fixture->database, query, NULL);
	g_assert_nonnull(rows);
	total = 0;

	for (i = 0; i < rows->len; i++)
	{
		gint64 remaining;

		g_object_get(g_ptr_array_index(rows, i), "remaining-qty", &remaining, NULL);
		total += remaining;
	}

	return total;
}

static VentureEntity *
recipe_new_in(
	Fixture		*fixture,
	gint64		 organization_id,
	const gchar	*name,
	gint64		 output_id,
	gint64		 batch
){
	VentureEntity *record;

	record = VENTURE_ENTITY(venture_recipe_new());
	g_object_set(record, "name", name, "output-product-id", output_id,
	             "output-quantity", batch, "active", TRUE, NULL);
	venture_entity_set_organization_id(record, (0 != organization_id)
		? organization_id : fixture->organization_id);

	return record;
}

static gint64
recipe(
	Fixture		*fixture,
	const gchar	*name,
	gint64		 output_id,
	gint64		 batch
){
	g_autoptr(VentureEntity) record = NULL;

	record = recipe_new_in(fixture, 0, name, output_id, batch);
	g_object_set(record, "venture-id", fixture->venture_id, NULL);
	save(fixture, record);

	return ID(record);
}

static VentureEntity *
component_new(
	Fixture		*fixture,
	gint64		 recipe_id,
	gint64		 product_id,
	gint64		 quantity,
	gboolean	 reusable
){
	VentureEntity *record;
	g_autoptr(VentureEntity) parent = NULL;

	parent = venture_database_get(fixture->database, VENTURE_TYPE_RECIPE, recipe_id, NULL);
	record = VENTURE_ENTITY(venture_recipe_component_new());
	g_object_set(record, "recipe-id", recipe_id, "product-id", product_id,
	             "quantity", quantity, "reusable", reusable, NULL);
	venture_entity_set_organization_id(record, (NULL != parent)
		? venture_entity_get_organization_id(parent) : fixture->organization_id);

	return record;
}

static gint64
component(
	Fixture		*fixture,
	gint64		 recipe_id,
	gint64		 product_id,
	gint64		 quantity,
	gboolean	 reusable
){
	g_autoptr(VentureEntity) record = NULL;

	record = component_new(fixture, recipe_id, product_id, quantity, reusable);
	save(fixture, record);

	return ID(record);
}

/* Crafts, expecting success, and returns the output's transaction. */
static VentureEntity *
craft(
	Fixture		*fixture,
	gint64		 recipe_id,
	gint64		 times,
	gint64		 location_id
){
	g_autoptr(GError) error = NULL;
	VentureEntity *txn;

	txn = NULL;

	if (!venture_production_craft(fixture->database, recipe_id, times, location_id,
	                              NULL, NULL, &txn, &error))
		g_error("craft refused: %s", error->message);

	g_assert_nonnull(txn);

	return txn;
}

/* Crafts, expecting a refusal holding @fragment. */
static void
craft_refused(
	Fixture		*fixture,
	gint64		 recipe_id,
	gint64		 times,
	gint64		 location_id,
	const gchar	*fragment
){
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) txn = NULL;

	g_assert_false(venture_production_craft(fixture->database, recipe_id, times,
	                                        location_id, NULL, NULL, &txn, &error));
	g_assert_nonnull(error);
	g_assert_null(txn);

	if (NULL == strstr(error->message, fragment))
		g_error("expected \"%s\" in: %s", fragment, error->message);
}

/* ==========================================================================
 * Validators
 * ========================================================================== */

/*
 * A recipe makes a product, at least one unit per batch; a component
 * names a recipe and a product, at least one unit of it, never the
 * recipe's own output, and only once. What breaks: a recipe of nothing
 * crafts stock into no item; a self-loop issues and receives the same
 * stock; a duplicate line doubles a quantity nobody typed twice.
 */
static void
test_validators(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) nothing = NULL;
	g_autoptr(VentureEntity) empty_batch = NULL;
	g_autoptr(VentureEntity) own_output = NULL;
	g_autoptr(VentureEntity) twin = NULL;
	g_autoptr(VentureEntity) none = NULL;
	g_autoptr(VentureEntity) no_product = NULL;
	g_autoptr(VentureEntity) changed = NULL;
	g_autoptr(VentureEntity) edited = NULL;
	gint64 herb;
	gint64 vial;
	gint64 potion;
	gint64 potions;
	gint64 line;

	(void)user_data;

	herb = product(fixture, "Peacebloom");
	vial = product(fixture, "Empty Vial");
	potion = product(fixture, "Healing Potion");

	nothing = recipe_new_in(fixture, 0, "Nothing", 0, 1);
	save_refused(fixture, nothing, "Makes");

	empty_batch = recipe_new_in(fixture, 0, "Empty", potion, 0);
	save_refused(fixture, empty_batch, "Batch size");

	potions = recipe(fixture, "Healing Potion", potion, 2);
	line = component(fixture, potions, herb, 2, FALSE);
	component(fixture, potions, vial, 1, FALSE);

	none = component_new(fixture, potions, herb, 0, FALSE);
	g_object_set(none, "product-id", product(fixture, "Silverleaf"), NULL);
	save_refused(fixture, none, "Quantity");

	no_product = component_new(fixture, potions, 0, 1, FALSE);
	save_refused(fixture, no_product, "Product");

	own_output = component_new(fixture, potions, potion, 1, FALSE);
	save_refused(fixture, own_output, "what this recipe makes");

	twin = component_new(fixture, potions, herb, 5, TRUE);
	save_refused(fixture, twin, "change that line's quantity");

	/* The same line saved again is not its own twin. */
	edited = venture_database_get(fixture->database, VENTURE_TYPE_RECIPE_COMPONENT, line, NULL);
	g_object_set(edited, "quantity", (gint64)3, NULL);
	save(fixture, edited);

	/* The loop from the other side: the recipe changed to make a thing
	 * it already takes. */
	changed = venture_database_get(fixture->database, VENTURE_TYPE_RECIPE, potions, NULL);
	g_object_set(changed, "output-product-id", herb, NULL);
	save_refused(fixture, changed, "cannot make what it takes");
}

/*
 * Every reference a recipe or component writes stays in its own
 * organization, and a recipe's category must be one that files recipes.
 * What breaks: a recipe that crafts one organization's materials into
 * another's product, moving stock across two sets of books.
 */
static void
test_validators_organization_and_category(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) foreign_output = NULL;
	g_autoptr(VentureEntity) foreign_input = NULL;
	g_autoptr(VentureEntity) filed = NULL;
	g_autoptr(VentureEntity) wrong_tree = NULL;
	g_autoptr(VentureEntity) right_tree = NULL;
	gint64 elsewhere;
	gint64 theirs;
	gint64 potion;
	gint64 potions;

	(void)user_data;

	elsewhere = organization(fixture, "elsewhere");
	theirs = product_in(fixture, elsewhere, "Their herb", NULL, NULL);
	potion = product(fixture, "Healing Potion");

	foreign_output = recipe_new_in(fixture, 0, "Theirs", theirs, 1);
	save_refused(fixture, foreign_output, "another organization");

	potions = recipe(fixture, "Healing Potion", potion, 1);
	foreign_input = component_new(fixture, potions, theirs, 1, FALSE);
	save_refused(fixture, foreign_input, "another organization");

	/* The taxonomy's applies-to reaches recipes through the field table:
	 * nothing here lists recipe as a categorised type. */
	wrong_tree = VENTURE_ENTITY(venture_category_new());
	g_object_set(wrong_tree, "name", "Herbs", "applies-to", "product", NULL);
	venture_entity_set_organization_id(wrong_tree, fixture->organization_id);
	save(fixture, wrong_tree);
	right_tree = VENTURE_ENTITY(venture_category_new());
	g_object_set(right_tree, "name", "Alchemy", "applies-to", "recipe", NULL);
	venture_entity_set_organization_id(right_tree, fixture->organization_id);
	save(fixture, right_tree);

	filed = venture_database_get(fixture->database, VENTURE_TYPE_RECIPE, potions, NULL);
	g_object_set(filed, "category-id", ID(wrong_tree), NULL);
	save_refused(fixture, filed, "groups product records");
	g_object_set(filed, "category-id", ID(right_tree), NULL);
	save(fixture, filed);
}

/* ==========================================================================
 * Crafting
 * ========================================================================== */

typedef struct
{
	gint64 herb;
	gint64 vial;
	gint64 still;
	gint64 potion;
	gint64 herb_item;
	gint64 vial_item;
	gint64 still_item;
	gint64 potion_item;
	gint64 recipe;
} Workshop;

/*
 * Two herbs and a vial make three potions, on a still that is not used
 * up. Herbs arrive in two FIFO layers at different costs, so which ones a
 * craft takes is visible in what the potions cost.
 */
static void
workshop(
	Fixture		*fixture,
	Workshop	*shop
){
	shop->herb = product(fixture, "Peacebloom");
	shop->vial = product(fixture, "Empty Vial");
	shop->still = product(fixture, "Alchemy Still");
	shop->potion = product(fixture, "Healing Potion");

	shop->herb_item = item(fixture, shop->herb);
	shop->vial_item = item(fixture, shop->vial);
	shop->still_item = item(fixture, shop->still);
	shop->potion_item = item(fixture, shop->potion);

	receive(fixture, shop->herb_item, 5, "1.00 USD");
	receive(fixture, shop->herb_item, 5, "2.00 USD");
	receive(fixture, shop->vial_item, 4, "3.00 USD");
	receive(fixture, shop->still_item, 1, "50.00 USD");

	shop->recipe = recipe(fixture, "Healing Potion", shop->potion, 3);
	component(fixture, shop->recipe, shop->herb, 2, FALSE);
	component(fixture, shop->recipe, shop->vial, 1, FALSE);
	component(fixture, shop->recipe, shop->still, 1, TRUE);
}

/* The inventory account's balance, which a craft must not move. */
static gint64
inventory_balance(Fixture *fixture)
{
	g_autoptr(VentureMoney) value = NULL;
	g_autoptr(GError) error = NULL;

	value = venture_inventory_service_valuation(
		venture_inventory_service_get(fixture->database), fixture->organization_id,
		NULL, &error);
	g_assert_no_error(error);

	return venture_money_get_amount(value);
}

/*
 * Two batches take 4 herbs (the four 1.00 ones, FIFO), 2 vials at 3.00,
 * and leave the still where it was: 10.00 of cost for 6 potions. The
 * potions' layers carry exactly 10.00 -- four at 1.67 and two at 1.66 --
 * so selling all six costs exactly what went in. Every transaction is
 * PRODUCTION and names the recipe. What breaks: a rounded single layer
 * leaves a cent behind per craft; a cost read from the newest layer
 * rather than the oldest overstates it; a journal posted to cost of goods
 * sold books an expense for stock that is still on the shelf.
 */
static void
test_craft_success(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) txn = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) txns = NULL;
	g_autoptr(VentureMoney) cogs = NULL;
	g_autoptr(VentureMoney) unit = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *reference = NULL;
	g_autofree gchar *expected = NULL;
	g_autofree gchar *unit_text = NULL;
	g_autofree gchar *cogs_text = NULL;
	Workshop shop;
	gint64 value_before;
	gint64 quantity;
	gint kind;
	guint i;

	(void)user_data;

	workshop(fixture, &shop);
	value_before = inventory_balance(fixture);

	txn = craft(fixture, shop.recipe, 2, 0);

	g_assert_cmpint(on_hand(fixture, shop.herb_item), ==, 6);
	g_assert_cmpint(on_hand(fixture, shop.vial_item), ==, 2);
	g_assert_cmpint(on_hand(fixture, shop.still_item), ==, 1);
	g_assert_cmpint(on_hand(fixture, shop.potion_item), ==, 6);

	g_object_get(txn, "quantity", &quantity, "kind", &kind, "reference", &reference,
	             "unit-cost", &unit, NULL);
	g_assert_cmpint(quantity, ==, 6);
	g_assert_cmpint(kind, ==, VENTURE_INVENTORY_TXN_KIND_PRODUCTION);
	expected = g_strdup_printf("recipe:%" G_GINT64_FORMAT, shop.recipe);
	g_assert_cmpstr(reference, ==, expected);
	unit_text = venture_money_to_string(unit);
	g_assert_cmpstr(unit_text, ==, "1.67 USD");

	/* Three transactions -- two inputs and the output -- all
	 * production, all naming the recipe; the still has none. */
	query = venture_query_new(VENTURE_TYPE_INVENTORY_TXN);
	venture_query_set_limit(query, 0);
	g_assert_true(venture_query_add_filter_string(query, "reference",
		VENTURE_FILTER_OP_EQ, reference, NULL));
	txns = venture_database_find(fixture->database, query, NULL);
	g_assert_nonnull(txns);
	g_assert_cmpuint(txns->len, ==, 3);

	for (i = 0; i < txns->len; i++)
	{
		gint64 item_id;

		g_object_get(g_ptr_array_index(txns, i), "kind", &kind,
		             "inventory-item-id", &item_id, NULL);
		g_assert_cmpint(kind, ==, VENTURE_INVENTORY_TXN_KIND_PRODUCTION);
		g_assert_cmpint(item_id, !=, shop.still_item);
	}

	/* Stock moved from inventory to inventory: the value is unchanged. */
	g_assert_cmpint(inventory_balance(fixture), ==, value_before);

	/* Selling every potion costs exactly what the inputs did. */
	g_assert_true(venture_inventory_service_issue(
		venture_inventory_service_get(fixture->database), shop.potion_item, 6, NULL,
		"test", 0, NULL, &cogs, &error));
	g_assert_no_error(error);
	cogs_text = venture_money_to_string(cogs);
	g_assert_cmpstr(cogs_text, ==, "10.00 USD");
}

/*
 * A shortage names what is short and writes nothing. What breaks: a
 * craft that consumed the herbs before discovering there were no vials
 * leaves herbs gone and no potion made.
 */
static void
test_craft_shortage(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	Workshop shop;
	guint txns;
	gint64 layers;

	(void)user_data;

	workshop(fixture, &shop);
	txns = count_rows(fixture, VENTURE_TYPE_INVENTORY_TXN);
	layers = layer_units(fixture);

	/* Five batches need ten herbs (there are ten) and five vials (four). */
	craft_refused(fixture, shop.recipe, 5, 0, "Short of Empty Vial");

	g_assert_cmpuint(count_rows(fixture, VENTURE_TYPE_INVENTORY_TXN), ==, txns);
	g_assert_cmpint(layer_units(fixture), ==, layers);
	g_assert_cmpint(on_hand(fixture, shop.herb_item), ==, 10);
	g_assert_cmpint(on_hand(fixture, shop.potion_item), ==, 0);

	/* Neither zero nor absurd batches. */
	craft_refused(fixture, shop.recipe, 0, 0, "times");
	craft_refused(fixture, shop.recipe, VENTURE_PRODUCTION_MAX_TIMES + 1, 0, "times");
}

/*
 * An item that allows negative stock may be crafted past zero -- the
 * count lags the shelf -- and the units with no cost layer are consumed
 * at no cost. A reusable component is different: it has to be there
 * whatever its item allows. What breaks: allow-negative ignored makes a
 * sloppy count block real work; honoured for a tool, it crafts with a
 * hammer nobody owns.
 */
static void
test_craft_allow_negative_and_tools(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) vials = NULL;
	g_autoptr(VentureEntity) still = NULL;
	g_autoptr(VentureEntity) txn = NULL;
	g_autoptr(VentureMoney) unit = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *unit_text = NULL;
	Workshop shop;

	(void)user_data;

	workshop(fixture, &shop);

	vials = venture_database_get(fixture->database, VENTURE_TYPE_INVENTORY_ITEM,
	                             shop.vial_item, NULL);
	g_object_set(vials, "allow-negative", TRUE, NULL);
	save(fixture, vials);

	/* Five batches: ten herbs (1.00 x5 + 2.00 x5 = 15.00), five vials of
	 * which four carry 3.00 and one carries nothing -- 27.00 for 15. */
	txn = craft(fixture, shop.recipe, 5, 0);
	g_assert_cmpint(on_hand(fixture, shop.vial_item), ==, -1);
	g_assert_cmpint(on_hand(fixture, shop.herb_item), ==, 0);
	g_assert_cmpint(on_hand(fixture, shop.potion_item), ==, 15);
	g_object_get(txn, "unit-cost", &unit, NULL);
	unit_text = venture_money_to_string(unit);
	g_assert_cmpstr(unit_text, ==, "1.80 USD");

	/* The still leaves (sold, say); allow-negative on its item does not
	 * conjure it back for the next craft. */
	g_assert_true(venture_inventory_service_issue(
		venture_inventory_service_get(fixture->database), shop.still_item, 1, NULL,
		"test", 0, NULL, NULL, &error));
	g_assert_no_error(error);
	still = venture_database_get(fixture->database, VENTURE_TYPE_INVENTORY_ITEM,
	                             shop.still_item, NULL);
	g_object_set(still, "allow-negative", TRUE, NULL);
	save(fixture, still);
	receive(fixture, shop.herb_item, 2, "1.00 USD");
	craft_refused(fixture, shop.recipe, 1, 0, "has to be there");
}

/*
 * A product kept in two places is refused until the location is named,
 * and the location picks the stock the craft takes and where the output
 * lands. An output with nowhere to go names what to create. What breaks:
 * a craft that guessed a bag takes the wrong character's herbs.
 */
static void
test_craft_locations(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	gint64 bank;
	gint64 bag;
	gint64 ore;
	gint64 bar;
	gint64 bank_ore;
	gint64 bag_ore;
	gint64 bank_bar;
	gint64 smelt;
	g_autoptr(VentureEntity) txn = NULL;

	(void)user_data;

	bank = location_in(fixture, 0, "Bank");
	bag = location_in(fixture, 0, "Bag");
	ore = product(fixture, "Copper Ore");
	bar = product(fixture, "Copper Bar");
	bank_ore = item_in(fixture, 0, ore, bank, NULL);
	bag_ore = item_in(fixture, 0, ore, bag, NULL);
	receive(fixture, bank_ore, 4, "0.50 USD");
	receive(fixture, bag_ore, 4, "0.70 USD");

	smelt = recipe(fixture, "Smelt Copper", bar, 1);
	component(fixture, smelt, ore, 2, FALSE);

	craft_refused(fixture, smelt, 1, 0, "name the location_id");
	/* Named, but nothing to put the bar into there yet. */
	craft_refused(fixture, smelt, 1, bank, "create an inventory item");

	bank_bar = item_in(fixture, 0, bar, bank, NULL);
	txn = craft(fixture, smelt, 2, bank);
	g_assert_cmpint(on_hand(fixture, bank_ore), ==, 0);
	g_assert_cmpint(on_hand(fixture, bag_ore), ==, 4);
	g_assert_cmpint(on_hand(fixture, bank_bar), ==, 2);

	/* The bag has ore but nowhere for a bar. */
	craft_refused(fixture, smelt, 1, bag, "kept at Bag");
}

/* Refuses any positive production transaction: the output's, which is
 * written after every input's. */
static gboolean
refuse_output(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	gpointer	  user_data,
	GError		**error
){
	gint64 quantity;
	gint kind;
	gboolean *armed;

	(void)database;
	(void)previous;

	armed = user_data;
	g_object_get(entity, "quantity", &quantity, "kind", &kind, NULL);

	if (*armed && (quantity > 0) && (VENTURE_INVENTORY_TXN_KIND_PRODUCTION == kind))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "the shelf is full");
		return FALSE;
	}

	return TRUE;
}

/*
 * A failure on the last write -- after every input's transaction and cost
 * layer was saved -- rolls all of them back. What breaks: stock consumed
 * with nothing made, and FIFO layers emptied for units that never left.
 */
static void
test_craft_rollback(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	Workshop shop;
	guint txns;
	gint64 layers;
	gint64 value;
	gboolean armed;

	(void)user_data;

	workshop(fixture, &shop);
	armed = FALSE;
	venture_database_add_save_validator(fixture->database, VENTURE_TYPE_INVENTORY_TXN,
	                                    refuse_output, &armed, NULL);
	armed = TRUE;
	txns = count_rows(fixture, VENTURE_TYPE_INVENTORY_TXN);
	layers = layer_units(fixture);
	value = inventory_balance(fixture);

	craft_refused(fixture, shop.recipe, 1, 0, "the shelf is full");

	g_assert_cmpuint(count_rows(fixture, VENTURE_TYPE_INVENTORY_TXN), ==, txns);
	g_assert_cmpint(layer_units(fixture), ==, layers);
	g_assert_cmpint(inventory_balance(fixture), ==, value);
	g_assert_cmpint(on_hand(fixture, shop.herb_item), ==, 10);
	g_assert_cmpint(on_hand(fixture, shop.vial_item), ==, 4);

	/* And the database is usable afterwards: the next craft works. */
	armed = FALSE;
	g_object_unref(craft(fixture, shop.recipe, 1, 0));
	g_assert_cmpint(on_hand(fixture, shop.potion_item), ==, 3);
}

/*
 * Inputs whose FIFO costs are in two currencies cannot be one made
 * unit's cost; the craft is refused and nothing moves. What breaks: a
 * potion whose cost is gold and dollars added as though they were one.
 */
static void
test_craft_mixed_currency(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	Workshop shop;
	gint64 gold_item;
	gint64 gold;
	gint64 alloy;
	gint64 mix;

	(void)user_data;

	workshop(fixture, &shop);
	gold = product(fixture, "Gold Dust");
	alloy = product(fixture, "Alloy");
	gold_item = item(fixture, gold);
	item(fixture, alloy);
	receive(fixture, gold_item, 3, "1.0000 GOLD");

	mix = recipe(fixture, "Alloy", alloy, 1);
	component(fixture, mix, shop.herb, 1, FALSE);
	component(fixture, mix, gold, 1, FALSE);

	craft_refused(fixture, mix, 1, 0, "two currencies");
	g_assert_cmpint(on_hand(fixture, shop.herb_item), ==, 10);
	g_assert_cmpint(on_hand(fixture, gold_item), ==, 3);
}

/*
 * The action is how every door crafts: the page's form, the API,
 * `venturectl act`, the assistant. It is a record action on the recipe,
 * so it runs in the recipe's own organization and takes that
 * organization's stock; its parameters are typed, so "2" as a string is
 * refused rather than read. An inactive recipe offers no Craft. What
 * breaks: a craft run in the default organization while the recipe
 * belongs to another, or a CLI verb that forgot to type its arguments.
 */
static void
test_craft_action(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	VentureActionRegistry *registry;
	VentureAction *action;
	g_autoptr(GHashTable) params = NULL;
	g_autoptr(GHashTable) wrong = NULL;
	g_autoptr(VentureEntity) result = NULL;
	g_autoptr(VentureEntity) refused = NULL;
	g_autoptr(VentureEntity) inactive = NULL;
	g_autoptr(VentureEntity) theirs = NULL;
	g_autoptr(VentureEntity) their_txn = NULL;
	g_autoptr(GError) error = NULL;
	Workshop shop;
	gint64 elsewhere;
	gint64 their_ore;
	gint64 their_bar;
	gint64 their_ore_item;
	gint64 their_bar_item;
	gint64 our_ore_item;
	gint64 their_recipe;
	gboolean stageable;
	gboolean type_level;

	(void)user_data;

	workshop(fixture, &shop);
	registry = venture_database_get_action_registry(fixture->database);
	action = venture_action_registry_lookup(registry, "recipe", "craft");
	g_assert_nonnull(action);
	g_object_get(action, "stageable", &stageable, "type-level", &type_level, NULL);
	g_assert_true(stageable);
	g_assert_false(type_level);

	params = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                               (GDestroyNotify)json_node_unref);
	g_hash_table_insert(params, g_strdup("times"), json_node_init_int(json_node_alloc(), 2));
	result = venture_action_registry_perform(registry, "recipe", shop.recipe, "craft",
	                                         params, NULL, VENTURE_USER_ROLE_EDITOR, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_assert_true(VENTURE_IS_INVENTORY_TXN(result));
	g_assert_cmpint(on_hand(fixture, shop.potion_item), ==, 6);

	/* A string where an integer belongs is refused, not coerced. */
	wrong = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                              (GDestroyNotify)json_node_unref);
	g_hash_table_insert(wrong, g_strdup("times"), json_node_init_string(json_node_alloc(), "2"));
	refused = venture_action_registry_perform(registry, "recipe", shop.recipe, "craft",
	                                          wrong, NULL, VENTURE_USER_ROLE_EDITOR, &error);
	g_assert_null(refused);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	g_assert_cmpint(on_hand(fixture, shop.potion_item), ==, 6);

	/* Inactive: not offered, not performed. */
	inactive = venture_database_get(fixture->database, VENTURE_TYPE_RECIPE, shop.recipe, NULL);
	g_object_set(inactive, "active", FALSE, NULL);
	save(fixture, inactive);
	g_assert_false(venture_action_registry_allowed(registry, action, inactive, NULL,
	                                               VENTURE_USER_ROLE_EDITOR, NULL));
	g_clear_object(&refused);
	refused = venture_action_registry_perform(registry, "recipe", shop.recipe, "craft",
	                                          params, NULL, VENTURE_USER_ROLE_EDITOR, &error);
	g_assert_null(refused);
	g_assert_nonnull(error);
	g_clear_error(&error);

	/* Another organization's recipe takes that organization's ore, and
	 * leaves ours -- the same product name, a different set of books --
	 * untouched. */
	elsewhere = organization(fixture, "elsewhere");
	their_ore = product_in(fixture, elsewhere, "Copper Ore", NULL, NULL);
	their_bar = product_in(fixture, elsewhere, "Copper Bar", NULL, NULL);
	their_ore_item = item_in(fixture, elsewhere, their_ore, 0, NULL);
	their_bar_item = item_in(fixture, elsewhere, their_bar, 0, NULL);
	our_ore_item = item(fixture, product(fixture, "Copper Ore"));
	receive(fixture, their_ore_item, 2, "0.50 USD");
	receive(fixture, our_ore_item, 2, "0.50 USD");
	theirs = recipe_new_in(fixture, elsewhere, "Smelt", their_bar, 1);
	save(fixture, theirs);
	their_recipe = ID(theirs);
	component(fixture, their_recipe, their_ore, 2, FALSE);

	g_hash_table_replace(params, g_strdup("times"), json_node_init_int(json_node_alloc(), 1));
	their_txn = venture_action_registry_perform(registry, "recipe", their_recipe, "craft",
	                                            params, NULL, VENTURE_USER_ROLE_EDITOR, &error);
	g_assert_no_error(error);
	g_assert_nonnull(their_txn);
	g_assert_cmpint(venture_entity_get_organization_id(their_txn), ==, elsewhere);
	g_assert_cmpint(on_hand(fixture, their_ore_item), ==, 0);
	g_assert_cmpint(on_hand(fixture, their_bar_item), ==, 1);
	g_assert_cmpint(on_hand(fixture, our_ore_item), ==, 2);
}

/* ==========================================================================
 * recipe_margin
 * ========================================================================== */

static VentureReportResult *
margin(
	Fixture		 *fixture,
	const gchar	 *price_source,
	gint64		  category_id,
	GError		**error
){
	g_autoptr(JsonObject) options = NULL;
	VentureReport *report;

	options = json_object_new();

	if (NULL != price_source)
		json_object_set_string_member(options, "price_source", price_source);

	if (0 != category_id)
		json_object_set_int_member(options, "category_id", category_id);

	report = venture_report_registry_lookup(
		venture_context_get_report_registry(fixture->context), "recipe_margin");
	g_assert_nonnull(report);

	return venture_report_generate(report, fixture->context, NULL, options, error);
}

static guint
row_of(
	VentureReportResult	*result,
	const gchar		*recipe_name
){
	guint i;

	for (i = 0; i < venture_report_result_get_row_count(result); i++)
	{
		const GValue *value;

		value = venture_report_result_get_cell(result, i, "recipe");

		if ((NULL != value) && (0 == g_strcmp0(g_value_get_string(value), recipe_name)))
			return i;
	}

	g_error("no row for %s", recipe_name);
	return 0;
}

static gchar *
cell_money(
	VentureReportResult	*result,
	guint			 row,
	const gchar		*key
){
	const GValue *value;

	value = venture_report_result_get_cell(result, row, key);

	if ((NULL == value) || !G_VALUE_HOLDS(value, VENTURE_TYPE_MONEY) ||
	    (NULL == g_value_get_boxed(value)))
		return NULL;

	return venture_money_to_string(g_value_get_boxed(value));
}

static const gchar *
cell_text(
	VentureReportResult	*result,
	guint			 row,
	const gchar		*key
){
	const GValue *value;

	value = venture_report_result_get_cell(result, row, key);

	if ((NULL == value) || !G_VALUE_HOLDS_STRING(value))
		return NULL;

	return g_value_get_string(value);
}

static gdouble
cell_number(
	VentureReportResult	*result,
	guint			 row,
	const gchar		*key
){
	const GValue *value;

	value = venture_report_result_get_cell(result, row, key);
	g_assert_nonnull(value);
	g_assert_true(G_VALUE_HOLDS_DOUBLE(value));

	return g_value_get_double(value);
}

#define ASSERT_CELL_MONEY(result, row, key, expected) G_STMT_START { \
	g_autofree gchar *cell_money_text = cell_money(result, row, key); \
	g_assert_cmpstr(cell_money_text, ==, expected); \
} G_STMT_END

static void
observe(
	Fixture		*fixture,
	gint64		 product_id,
	const gchar	*source,
	const gchar	*price,
	const gchar	*when
){
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(GDateTime) at = NULL;

	amount = money_of(price);
	at = venture_time_from_string(when, NULL);
	record = VENTURE_ENTITY(venture_price_observation_new());
	g_object_set(record, "product-id", product_id, "source", source,
	             "price", amount, "observed-at", at, NULL);
	venture_entity_set_organization_id(record, fixture->organization_id);
	save(fixture, record);
}

/*
 * With the market on, every figure comes from the named source's latest
 * observation: 2 herbs at 1.50 and a vial at 0.40 cost 3.40 a batch; 3
 * potions at 2.00 are worth 6.00; 2.60 profit, 43.33% margin, 1.13 per
 * potion (3.40 / 3, half to even). The still is a tool: not in the cost,
 * but a gate on batches. Ten herbs and four vials allow four batches
 * (the vials run out first). A product never priced by that source is
 * named and its figures left blank. What breaks: a tool priced as a
 * material, an older observation beating a newer one, or a missing price
 * read as free.
 */
static void
test_margin_market(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureReportResult) other = NULL;
	g_autoptr(GError) error = NULL;
	Workshop shop;
	gint64 thorn;
	gint64 salve;
	gint64 salves;
	guint row;

	(void)user_data;

	workshop(fixture, &shop);
	observe(fixture, shop.herb, "market value", "9.00 USD", "2026-03-01");
	observe(fixture, shop.herb, "market value", "1.50 USD", "2026-03-10");
	observe(fixture, shop.vial, "market value", "0.40 USD", "2026-03-10");
	observe(fixture, shop.potion, "market value", "2.00 USD", "2026-03-10");
	observe(fixture, shop.potion, "vendor", "0.25 USD", "2026-03-12");

	/* A recipe with an input the source never priced. */
	thorn = product(fixture, "Thorn");
	salve = product(fixture, "Salve");
	item(fixture, thorn);
	observe(fixture, salve, "market value", "5.00 USD", "2026-03-10");
	salves = recipe(fixture, "Salve", salve, 1);
	component(fixture, salves, thorn, 1, FALSE);

	result = margin(fixture, "market value", 0, &error);
	g_assert_no_error(error);
	row = row_of(result, "Healing Potion");
	g_assert_cmpstr(cell_text(result, row, "currency"), ==, "USD");
	g_assert_cmpstr(cell_text(result, row, "priced_by"), ==, "market value");
	ASSERT_CELL_MONEY(result, row, "cost", "3.40 USD");
	ASSERT_CELL_MONEY(result, row, "value", "6.00 USD");
	ASSERT_CELL_MONEY(result, row, "profit", "2.60 USD");
	ASSERT_CELL_MONEY(result, row, "cost_per_unit", "1.13 USD");
	g_assert_cmpfloat_with_epsilon(cell_number(result, row, "margin"), 2.60 / 6.00, 1e-9);
	g_assert_cmpfloat(cell_number(result, row, "craftable_now"), ==, 4.0);

	row = row_of(result, "Salve");
	g_assert_null(cell_money(result, row, "cost"));
	g_assert_null(cell_money(result, row, "profit"));
	g_assert_null(venture_report_result_get_cell(result, row, "margin"));
	ASSERT_CELL_MONEY(result, row, "value", "5.00 USD");
	g_assert_nonnull(strstr(cell_text(result, row, "note"), "no price for Thorn"));
	g_assert_cmpfloat(cell_number(result, row, "craftable_now"), ==, 0.0);

	/* Another source is another answer: the vendor never priced herbs. */
	other = margin(fixture, "vendor", 0, &error);
	g_assert_no_error(error);
	row = row_of(other, "Healing Potion");
	ASSERT_CELL_MONEY(other, row, "value", "0.75 USD");
	g_assert_null(cell_money(other, row, "cost"));
	g_assert_nonnull(strstr(cell_text(other, row, "note"), "Peacebloom"));
}

/*
 * A recipe whose inputs are priced in gold and its output in dollars
 * gets a note naming both currencies and no figures. Without the tool
 * on hand, no batch can be made whatever the materials allow. What
 * breaks: a margin computed by adding gold to dollars; a craftable count
 * that ignores the missing still.
 */
static void
test_margin_currencies_and_tools(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(GError) error = NULL;
	Workshop shop;
	guint row;

	(void)user_data;

	workshop(fixture, &shop);
	observe(fixture, shop.herb, "market value", "1.5000 GOLD", "2026-03-10");
	observe(fixture, shop.vial, "market value", "0.4000 GOLD", "2026-03-10");
	observe(fixture, shop.potion, "market value", "2.00 USD", "2026-03-10");

	g_assert_true(venture_inventory_service_issue(
		venture_inventory_service_get(fixture->database), shop.still_item, 1, NULL,
		"test", 0, NULL, NULL, &error));
	g_assert_no_error(error);

	result = margin(fixture, "market value", 0, &error);
	g_assert_no_error(error);
	row = row_of(result, "Healing Potion");
	g_assert_null(cell_money(result, row, "cost"));
	g_assert_null(cell_money(result, row, "value"));
	g_assert_null(cell_money(result, row, "profit"));
	g_assert_null(cell_text(result, row, "currency"));
	g_assert_nonnull(strstr(cell_text(result, row, "note"), "GOLD"));
	g_assert_nonnull(strstr(cell_text(result, row, "note"), "USD"));
	g_assert_cmpfloat(cell_number(result, row, "craftable_now"), ==, 0.0);
}

/*
 * With the market module off, the report prices at recorded costs -- an
 * input at its inventory item's unit cost, else the product's own cost;
 * the output at its list price -- and says so. A price source asked for
 * without the market is refused rather than silently ignored. The
 * category filter takes the category and everything beneath it. What
 * breaks: a report that needs the market module to say anything, or one
 * that answers a price_source question from somewhere else.
 */
static void
test_margin_market_off(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureReportResult) filtered = NULL;
	g_autoptr(VentureReportResult) refused = NULL;
	g_autoptr(VentureEntity) alchemy = NULL;
	g_autoptr(VentureEntity) healing = NULL;
	g_autoptr(VentureEntity) filed = NULL;
	g_autoptr(GError) error = NULL;
	gint64 herb;
	gint64 vial;
	gint64 potion;
	gint64 potions;
	gint64 plain;
	guint row;

	(void)user_data;

	herb = product_in(fixture, 0, "Peacebloom", NULL, "0.80 USD");
	vial = product_in(fixture, 0, "Empty Vial", NULL, "0.30 USD");
	potion = product_in(fixture, 0, "Healing Potion", "2.50 USD", NULL);
	/* The item's carrying cost beats the product's own. */
	item_in(fixture, 0, herb, 0, "1.00 USD");
	potions = recipe(fixture, "Healing Potion", potion, 1);
	component(fixture, potions, herb, 1, FALSE);
	component(fixture, potions, vial, 1, FALSE);
	plain = recipe(fixture, "Plain", product(fixture, "Plain thing"), 1);
	component(fixture, plain, herb, 1, FALSE);

	venture_config_set_module_enabled(fixture->config, "market", FALSE);
	g_assert_false(venture_context_module_enabled(fixture->context, "market"));

	result = margin(fixture, NULL, 0, &error);
	g_assert_no_error(error);
	row = row_of(result, "Healing Potion");
	g_assert_cmpstr(cell_text(result, row, "priced_by"), ==, "recorded costs");
	ASSERT_CELL_MONEY(result, row, "cost", "1.30 USD");
	ASSERT_CELL_MONEY(result, row, "value", "2.50 USD");
	ASSERT_CELL_MONEY(result, row, "profit", "1.20 USD");

	refused = margin(fixture, "market value", 0, &error);
	g_assert_null(refused);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);

	/* Alchemy / Healing: filtering on Alchemy finds the potion filed in
	 * Healing, and leaves the unfiled recipe out. */
	alchemy = VENTURE_ENTITY(venture_category_new());
	g_object_set(alchemy, "name", "Alchemy", "applies-to", "recipe", NULL);
	venture_entity_set_organization_id(alchemy, fixture->organization_id);
	save(fixture, alchemy);
	healing = VENTURE_ENTITY(venture_category_new());
	g_object_set(healing, "name", "Healing", "applies-to", "recipe",
	             "parent-id", ID(alchemy), NULL);
	venture_entity_set_organization_id(healing, fixture->organization_id);
	save(fixture, healing);
	filed = venture_database_get(fixture->database, VENTURE_TYPE_RECIPE, potions, NULL);
	g_object_set(filed, "category-id", ID(healing), NULL);
	save(fixture, filed);

	filtered = margin(fixture, NULL, ID(alchemy), &error);
	g_assert_no_error(error);
	g_assert_cmpuint(venture_report_result_get_row_count(filtered), ==, 1);
	g_assert_cmpstr(cell_text(filtered, 0, "recipe"), ==, "Healing Potion");
}

/* recipe_margin narrowed by venture and category, as the options say. */
static VentureReportResult *
margin_narrowed(
	Fixture		 *fixture,
	gint64		  venture_id,
	gint64		  category_id,
	GError		**error
){
	g_autoptr(JsonObject) options = NULL;
	VentureReport *report;

	options = json_object_new();

	if (0 != venture_id)
		json_object_set_int_member(options, "venture_id", venture_id);

	if (0 != category_id)
		json_object_set_int_member(options, "category_id", category_id);

	report = venture_report_registry_lookup(
		venture_context_get_report_registry(fixture->context), "recipe_margin");
	g_assert_nonnull(report);

	return venture_report_generate(report, fixture->context, NULL, options, error);
}

/*
 * The row bound counts the recipes asked about. venture_id, category_id
 * and "active only" are part of the query, so an organization past the
 * bound is refused only when the question itself is: narrowed under it,
 * the report answers. What breaks: the three filters run in C after a
 * capped fetch, so every question in a large organization is refused --
 * by a message that tells the operator to narrow the question they just
 * narrowed. The bound is lowered to two rather than seeding twenty
 * thousand recipes.
 */
static void
test_margin_bound(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureReportResult) everything = NULL;
	g_autoptr(VentureReportResult) by_venture = NULL;
	g_autoptr(VentureReportResult) by_category = NULL;
	g_autoptr(VentureReportResult) active_only = NULL;
	g_autoptr(VentureVenture) other = NULL;
	g_autoptr(VentureEntity) alchemy = NULL;
	g_autoptr(VentureEntity) healing = NULL;
	g_autoptr(VentureEntity) filed = NULL;
	g_autoptr(VentureEntity) elsewhere = NULL;
	g_autoptr(VentureEntity) retired = NULL;
	g_autoptr(GError) error = NULL;
	gint64 potions;

	(void)user_data;

	other = venture_venture_new();
	g_object_set(other, "name", "Elsewhere", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(other), fixture->organization_id);
	save(fixture, other);

	alchemy = VENTURE_ENTITY(venture_category_new());
	g_object_set(alchemy, "name", "Alchemy", "applies-to", "recipe", NULL);
	venture_entity_set_organization_id(alchemy, fixture->organization_id);
	save(fixture, alchemy);
	healing = VENTURE_ENTITY(venture_category_new());
	g_object_set(healing, "name", "Healing", "applies-to", "recipe",
	             "parent-id", ID(alchemy), NULL);
	venture_entity_set_organization_id(healing, fixture->organization_id);
	save(fixture, healing);

	/* Two active recipes in the workshop, one filed beneath Alchemy;
	 * one active in another venture; one retired in the workshop. */
	potions = recipe(fixture, "Healing Potion", product(fixture, "Healing Potion"), 1);
	filed = venture_database_get(fixture->database, VENTURE_TYPE_RECIPE, potions, NULL);
	g_object_set(filed, "category-id", ID(healing), NULL);
	save(fixture, filed);
	recipe(fixture, "Plain", product(fixture, "Plain thing"), 1);
	elsewhere = recipe_new_in(fixture, 0, "Elsewhere", product(fixture, "Far thing"), 1);
	g_object_set(elsewhere, "venture-id", ID(other), NULL);
	save(fixture, elsewhere);
	retired = recipe_new_in(fixture, 0, "Retired", product(fixture, "Old thing"), 1);
	g_object_set(retired, "venture-id", fixture->venture_id, "active", FALSE, NULL);
	save(fixture, retired);

	/* Three active of four: the retired one is not counted. */
	venture_aggregate_set_max_rows(3);
	active_only = margin_narrowed(fixture, 0, 0, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(venture_report_result_get_row_count(active_only), ==, 3);

	/* Past two, the whole organization is refused... */
	venture_aggregate_set_max_rows(2);
	everything = margin_narrowed(fixture, 0, 0, &error);
	g_assert_null(everything);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);

	/* ...and the workshop's two, or Alchemy's one, are answered. */
	by_venture = margin_narrowed(fixture, fixture->venture_id, 0, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(venture_report_result_get_row_count(by_venture), ==, 2);

	by_category = margin_narrowed(fixture, 0, ID(alchemy), &error);
	g_assert_no_error(error);
	g_assert_cmpuint(venture_report_result_get_row_count(by_category), ==, 1);
	g_assert_cmpstr(cell_text(by_category, 0, "recipe"), ==, "Healing Potion");

	venture_aggregate_set_max_rows(0);
	g_assert_cmpint(venture_aggregate_get_max_rows(), ==, VENTURE_AGGREGATE_MAX_ROWS);
}

/* ==========================================================================
 * The module
 * ========================================================================== */

/*
 * Switching production off hides its types, its report and its action;
 * switching it back restores all three with nothing re-registered, and
 * sales off takes production with it. What breaks: a Craft button on an
 * install that turned the module off, or a report that vanishes for good.
 */
static void
test_module_off(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	VentureEntityRegistry *types;
	VentureReportRegistry *reports;
	VentureActionRegistry *actions;
	VentureModule *production;

	(void)user_data;

	production = venture_module_registry_lookup(venture_context_get_modules(fixture->context),
	                                            "production");
	g_assert_nonnull(production);
	g_assert_true(g_strv_contains(venture_module_get_requires(production), "sales"));
	g_assert_true(g_strv_contains(venture_module_get_suggests(production), "market"));
	g_assert_true(g_strv_contains(venture_module_get_reports(production), "recipe_margin"));
	g_assert_true(g_strv_contains(venture_module_get_entity_names(production), "recipe"));
	g_assert_true(g_strv_contains(venture_module_get_entity_names(production), "recipe_component"));

	types = venture_context_get_entity_registry(fixture->context);
	reports = venture_context_get_report_registry(fixture->context);
	actions = venture_database_get_action_registry(fixture->database);
	g_assert_nonnull(venture_action_registry_lookup(actions, "recipe", "craft"));

	venture_config_set_module_enabled(fixture->config, "production", FALSE);
	g_assert_false(venture_context_module_enabled(fixture->context, "production"));
	g_assert_true(G_TYPE_INVALID == venture_entity_registry_lookup(types, "recipe"));
	g_assert_true(G_TYPE_INVALID == venture_entity_registry_lookup(types, "recipe_component"));
	g_assert_null(venture_report_registry_lookup(reports, "recipe_margin"));
	g_assert_null(venture_action_registry_lookup(actions, "recipe", "craft"));

	venture_config_set_module_enabled(fixture->config, "production", TRUE);
	g_assert_nonnull(venture_report_registry_lookup(reports, "recipe_margin"));
	venture_config_set_module_enabled(fixture->config, "sales", FALSE);
	g_assert_false(venture_context_module_enabled(fixture->context, "production"));
	g_assert_null(venture_action_registry_lookup(actions, "recipe", "craft"));

	venture_config_set_module_enabled(fixture->config, "sales", TRUE);
	g_assert_true(venture_context_module_enabled(fixture->context, "production"));
	g_assert_true(VENTURE_TYPE_RECIPE == venture_entity_registry_lookup(types, "recipe"));
	g_assert_nonnull(venture_action_registry_lookup(actions, "recipe", "craft"));
}

typedef struct
{
	VentureConfig		*config;
	VentureDatabase		*database;
	VentureContext		*context;
	VentureWebServer	*server;
	SoupSession		*session;
	gchar			*state_dir;
	gchar			*cookie;
	guint16			 port;
} ServerFixture;

typedef struct
{
	gboolean	 done;
	GBytes		*body;
	GError		*error;
} RequestResult;

static void
request_done(
	GObject		*source,
	GAsyncResult	*result,
	gpointer	 user_data
){
	RequestResult *outcome;

	outcome = user_data;
	outcome->body = soup_session_send_and_read_finish(SOUP_SESSION(source),
	                                                  result, &outcome->error);
	outcome->done = TRUE;
}

static guint
server_request_full(
	ServerFixture	 *fixture,
	const gchar	 *method,
	const gchar	 *path,
	const gchar	 *content_type,
	const gchar	 *body,
	gboolean	  with_cookie,
	gchar		**out_body,
	gchar		**out_location
){
	g_autoptr(SoupMessage) message = NULL;
	g_autofree gchar *url = NULL;
	RequestResult outcome = { FALSE, NULL, NULL };

	url = g_strdup_printf("http://127.0.0.1:%u%s", fixture->port, path);
	message = soup_message_new(method, url);
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);

	if (with_cookie && (NULL != fixture->cookie))
		soup_message_headers_append(
			soup_message_get_request_headers(message), "Cookie",
			fixture->cookie);

	if (NULL != body)
	{
		g_autoptr(GBytes) bytes = NULL;

		bytes = g_bytes_new(body, strlen(body));
		soup_message_set_request_body_from_bytes(message, content_type, bytes);
	}

	soup_session_send_and_read_async(fixture->session, message,
	                                 G_PRIORITY_DEFAULT, NULL, request_done,
	                                 &outcome);

	while (!outcome.done)
		g_main_context_iteration(NULL, TRUE);

	if (NULL != outcome.error)
		g_error("%s %s: %s", method, path, outcome.error->message);

	if (NULL != out_body)
		*out_body = g_strndup(g_bytes_get_data(outcome.body, NULL),
		                      g_bytes_get_size(outcome.body));

	if (NULL != out_location)
		*out_location = g_strdup(soup_message_headers_get_one(
			soup_message_get_response_headers(message), "Location"));

	g_clear_pointer(&outcome.body, g_bytes_unref);
	g_clear_error(&outcome.error);

	return soup_message_get_status(message);
}

static guint
server_get(
	ServerFixture	 *fixture,
	const gchar	 *path,
	gchar		**out_body
){
	return server_request_full(fixture, "GET", path, NULL, NULL, TRUE,
	                           out_body, NULL);
}

static void
server_fixture_set_up(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureUser) user = NULL;
	g_autofree gchar *set_cookie = NULL;
	gchar *semicolon;

	(void)user_data;

	g_setenv("VENTURE_TEST_SESSION_SECRET", "production-test-secret", TRUE);

	fixture->state_dir = g_dir_make_tmp("venture-production-XXXXXX", NULL);
	fixture->port = (guint16)(20000 + ((getpid() + 8123) % 20000));

	fixture->config = venture_config_new();
	g_object_set(fixture->config,
	             "state-dir", fixture->state_dir,
	             "server-bind-address", "127.0.0.1",
	             "server-port", (gint64)fixture->port,
	             "security-session-secret-env", "VENTURE_TEST_SESSION_SECRET",
	             "security-password-iterations", (gint64)100000,
	             NULL);

	fixture->database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	fixture->context = venture_context_new(fixture->config, fixture->database);

	g_assert_true(venture_database_migrate(fixture->database,
		venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);

	fixture->server = venture_web_server_new(fixture->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(fixture->server, &error));
	g_assert_no_error(error);

	fixture->session = soup_session_new();

	user = venture_user_new();
	g_object_set(user, "username", "owner", "role", VENTURE_USER_ROLE_OWNER,
	             "active", TRUE, NULL);
	g_assert_true(venture_user_set_password(user, "owner-password-1", 100000,
	                                        NULL));
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(user), NULL, NULL));

	{
		g_autoptr(SoupMessage) message = NULL;
		g_autofree gchar *url = NULL;
		g_autoptr(GBytes) bytes = NULL;
		RequestResult outcome = { FALSE, NULL, NULL };

		url = g_strdup_printf("http://127.0.0.1:%u/login", fixture->port);
		message = soup_message_new("POST", url);
		soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
		bytes = g_bytes_new_static("username=owner&password=owner-password-1",
		                           strlen("username=owner&password=owner-password-1"));
		soup_message_set_request_body_from_bytes(message,
			"application/x-www-form-urlencoded", bytes);
		soup_session_send_and_read_async(fixture->session, message,
		                                 G_PRIORITY_DEFAULT, NULL,
		                                 request_done, &outcome);

		while (!outcome.done)
			g_main_context_iteration(NULL, TRUE);

		g_clear_pointer(&outcome.body, g_bytes_unref);
		g_clear_error(&outcome.error);

		set_cookie = g_strdup(soup_message_headers_get_one(
			soup_message_get_response_headers(message), "Set-Cookie"));
	}

	g_assert_nonnull(set_cookie);
	semicolon = strchr(set_cookie, ';');

	if (NULL != semicolon)
		*semicolon = '\0';

	fixture->cookie = g_steal_pointer(&set_cookie);
}

static void
server_fixture_tear_down(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureConfig) everything = NULL;
	g_autoptr(VentureModuleRegistry) registry = NULL;

	(void)user_data;

	if (NULL != fixture->server)
		venture_web_server_stop(fixture->server);

	g_clear_pointer(&fixture->cookie, g_free);
	g_clear_object(&fixture->session);
	g_clear_object(&fixture->server);
	g_clear_object(&fixture->context);
	g_clear_object(&fixture->database);
	g_clear_object(&fixture->config);

	if (NULL != fixture->state_dir)
	{
		venture_test_remove_tree(fixture->state_dir);
		g_clear_pointer(&fixture->state_dir, g_free);
	}

	g_unsetenv("VENTURE_TEST_SESSION_SECRET");

	everything = venture_config_new();
	registry = venture_module_registry_new();
	venture_module_registry_register_builtins(registry);
	venture_module_registry_configure(registry, everything, NULL);
	venture_module_registry_apply(registry,
	                              venture_entity_registry_get_default());
}


/*
 * Every door reaches production: the API forwards price_source to the
 * report (a price source dropped on the way would answer from any
 * source), the report page offers it, the recipe page offers Craft with
 * its typed parameters, and the action endpoint crafts from a JSON body.
 * What breaks: an option the page and the API never pass, or a recipe
 * page with no way to craft.
 */
static void
test_http(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) herb = NULL;
	g_autoptr(VentureEntity) potion = NULL;
	g_autoptr(VentureEntity) herb_item = NULL;
	g_autoptr(VentureEntity) potion_item = NULL;
	g_autoptr(VentureEntity) brew = NULL;
	g_autoptr(VentureEntity) line = NULL;
	g_autoptr(VentureEntity) seen = NULL;
	g_autoptr(VentureMoney) price = NULL;
	g_autoptr(VentureMoney) cost = NULL;
	g_autoptr(GDateTime) when = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *path = NULL;
	JsonArray *rows;
	gint64 organization_id;

	(void)user_data;

	organization_id = venture_context_get_default_organization_id(fixture->context);

	herb = VENTURE_ENTITY(venture_product_new());
	g_object_set(herb, "name", "Peacebloom", NULL);
	venture_entity_set_organization_id(herb, organization_id);
	g_assert_true(venture_database_save(fixture->database, herb, NULL, NULL));
	potion = VENTURE_ENTITY(venture_product_new());
	g_object_set(potion, "name", "Healing Potion", NULL);
	venture_entity_set_organization_id(potion, organization_id);
	g_assert_true(venture_database_save(fixture->database, potion, NULL, NULL));

	herb_item = VENTURE_ENTITY(venture_inventory_item_new());
	g_object_set(herb_item, "product-id", ID(herb), NULL);
	venture_entity_set_organization_id(herb_item, organization_id);
	g_assert_true(venture_database_save(fixture->database, herb_item, NULL, NULL));
	potion_item = VENTURE_ENTITY(venture_inventory_item_new());
	g_object_set(potion_item, "product-id", ID(potion), NULL);
	venture_entity_set_organization_id(potion_item, organization_id);
	g_assert_true(venture_database_save(fixture->database, potion_item, NULL, NULL));

	cost = venture_money_new(100, "USD", 2);
	g_assert_true(venture_inventory_service_receive(
		venture_inventory_service_get(fixture->database), ID(herb_item), 4, cost,
		NULL, 0, "seed", NULL, NULL));

	brew = VENTURE_ENTITY(venture_recipe_new());
	g_object_set(brew, "name", "Brew", "output-product-id", ID(potion),
	             "output-quantity", (gint64)1, "active", TRUE, NULL);
	venture_entity_set_organization_id(brew, organization_id);
	g_assert_true(venture_database_save(fixture->database, brew, NULL, NULL));
	line = VENTURE_ENTITY(venture_recipe_component_new());
	g_object_set(line, "recipe-id", ID(brew), "product-id", ID(herb),
	             "quantity", (gint64)2, NULL);
	venture_entity_set_organization_id(line, organization_id);
	g_assert_true(venture_database_save(fixture->database, line, NULL, NULL));

	price = venture_money_new(500, "USD", 2);
	when = g_date_time_new_utc(2026, 3, 2, 12, 0, 0);
	seen = VENTURE_ENTITY(venture_price_observation_new());
	g_object_set(seen, "product-id", ID(potion), "source", "vendor",
	             "price", price, "observed-at", when, NULL);
	venture_entity_set_organization_id(seen, organization_id);
	g_assert_true(venture_database_save(fixture->database, seen, NULL, NULL));

	g_assert_cmpuint(server_get(fixture,
		"/api/v1/reports/recipe_margin?price_source=vendor", &body), ==, SOUP_STATUS_OK);
	node = venture_json_parse(body, NULL);
	g_assert_nonnull(node);
	rows = json_object_get_array_member(json_node_get_object(node), "rows");
	g_assert_cmpuint(json_array_get_length(rows), ==, 1);
	g_assert_cmpstr(json_object_get_string_member(
		json_array_get_object_element(rows, 0), "priced_by"), ==, "vendor");

	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(server_get(fixture, "/reports/recipe_margin?price_source=vendor", &body),
		==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(body, "name=\"price_source\""));
	g_assert_nonnull(strstr(body, "Batches on hand"));

	/* The recipe page: its components among the related records, and a
	 * Craft form posting to the action with its typed parameters. */
	g_clear_pointer(&body, g_free);
	path = g_strdup_printf("/e/recipe/%" G_GINT64_FORMAT, ID(brew));
	g_assert_cmpuint(server_get(fixture, path, &body), ==, SOUP_STATUS_OK);
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/api/v1/recipe/%" G_GINT64_FORMAT "/actions/craft", ID(brew));
	g_assert_nonnull(strstr(body, path));
	g_assert_nonnull(strstr(body, "name=\"times\""));
	g_assert_nonnull(strstr(body, "name=\"location_id\""));

	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(server_request_full(fixture, "POST", path, "application/json",
		"{\"times\":2}", TRUE, &body, NULL), ==, SOUP_STATUS_OK);
	g_assert_cmpint(venture_inventory_service_on_hand(
		venture_inventory_service_get(fixture->database), ID(potion_item), NULL, NULL), ==, 2);
	g_assert_cmpint(venture_inventory_service_on_hand(
		venture_inventory_service_get(fixture->database), ID(herb_item), NULL, NULL), ==, 0);

	/* A form posts strings; the endpoint types them from the schema. */
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(server_request_full(fixture, "POST", path,
		"application/x-www-form-urlencoded", "times=1", TRUE, &body, NULL),
		==, 422);
	g_assert_nonnull(strstr(body, "Short of Peacebloom"));
}

int
main(
	int	 argc,
	char	**argv
){
	g_test_init(&argc, &argv, NULL);

#define ADD(path, func) \
	g_test_add(path, Fixture, NULL, fixture_set_up, func, fixture_tear_down)

	ADD("/production/validators", test_validators);
	ADD("/production/validators/organization-and-category",
	    test_validators_organization_and_category);
	ADD("/production/craft/success", test_craft_success);
	ADD("/production/craft/shortage", test_craft_shortage);
	ADD("/production/craft/allow-negative-and-tools", test_craft_allow_negative_and_tools);
	ADD("/production/craft/locations", test_craft_locations);
	ADD("/production/craft/rollback", test_craft_rollback);
	ADD("/production/craft/mixed-currency", test_craft_mixed_currency);
	ADD("/production/craft/action", test_craft_action);
	ADD("/production/margin/market", test_margin_market);
	ADD("/production/margin/currencies-and-tools", test_margin_currencies_and_tools);
	ADD("/production/margin/market-off", test_margin_market_off);
	ADD("/production/margin/bound", test_margin_bound);
	ADD("/production/module-off", test_module_off);
	g_test_add("/production/http", ServerFixture, NULL, server_fixture_set_up,
	           test_http, server_fixture_tear_down);

#undef ADD

	return g_test_run();
}
