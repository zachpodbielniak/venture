/*
 * test-venture-inventory-service.c - the inventory service on the stock engine
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The service keeps its rules on top of the Libreventory engine:
 *
 * - The shared transaction. A movement whose journal is refused leaves no
 *   movement in the engine, no lot and no row: stock and books commit or
 *   fail together. What breaks if it regresses: stock that moved with no
 *   journal, so the inventory account no longer ties to the valuation.
 * - Stock typed in by hand is used last, and only where a cost is not
 *   needed. An issue takes costed units first and refuses to sell units
 *   that carry no cost; a craft uses them at no cost; a count that finds
 *   fewer never takes a costed unit, so the value stays.
 */

#include <venture.h>

#include "goods/venture-stock-engine.h"
#include "venture-test-util.h"

#include <string.h>

typedef struct
{
	VentureConfig	*config;
	VentureDatabase	*db;
	VentureContext	*context;
	gchar		*schema;
	gint64		 org;
	gint64		 venture;
	gint64		 product;
} Fixture;

/* --- Fixture ------------------------------------------------------------ */

static void
save(Fixture *f, gpointer record)
{
	g_autoptr(GError) error = NULL;
	gboolean ok = venture_database_save(f->db, VENTURE_ENTITY(record), NULL, &error);

	g_assert_no_error(error);
	g_assert_true(ok);
}

static void
field(gpointer record, const gchar *name, const gchar *value)
{
	g_autoptr(GError) error = NULL;

	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(record), name, value, &error));
	g_assert_no_error(error);
}

static VentureEntity *
record(Fixture *f, const gchar *name)
{
	VentureEntity *entity = venture_entity_registry_create(venture_entity_registry_get_default(), name, NULL);

	g_assert_nonnull(entity);
	venture_entity_set_organization_id(entity, f->org);
	return entity;
}

static void
exec(Fixture *f, const gchar *sql)
{
	g_autoptr(GError) error = NULL;

	g_assert_true(venture_database_execute(f->db, sql, NULL, &error));
	g_assert_no_error(error);
}

static void
define_currency(Fixture *f, const gchar *code, gint64 exponent)
{
	g_autoptr(VentureEntity) currency = record(f, "currency");

	g_object_set(currency, "code", code, "name", code, "exponent", exponent, NULL);
	field(currency, "book-treatment", "valued");
	save(f, currency);
}

/* A database on @uri, in a schema of its own on PostgreSQL. */
static void
open_database(Fixture *f, const gchar *uri)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureVenture) venture = venture_venture_new();
	g_autoptr(VentureEntity) product = NULL;

	venture_currency_clear_registered();
	f->config = venture_config_new();
	f->db = venture_database_new(uri != NULL ? uri : "sqlite://:memory:", &error);
	g_assert_no_error(error);
	if (uri != NULL)
	{
		g_autofree gchar *uuid = g_uuid_string_random();
		g_autofree gchar *sql = NULL;

		g_strdelimit(uuid, "-", '_');
		f->schema = g_strconcat("venture_stock_test_", uuid, NULL);
		sql = g_strdup_printf("CREATE SCHEMA %s; SET search_path TO %s", f->schema, f->schema);
		exec(f, sql);
	}
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
	g_object_set(venture, "name", "Shop", "venture-type", "books", "organization-id", f->org, NULL);
	save(f, venture);
	f->venture = venture_entity_get_id(VENTURE_ENTITY(venture));
	define_currency(f, "GOLD", 4);
	define_currency(f, "TICKET", 0);
	product = record(f, "product");
	g_object_set(product, "name", "Widget", "venture-id", f->venture, NULL);
	save(f, product);
	f->product = venture_entity_get_id(product);
}

static void
setup(Fixture *f, gconstpointer data)
{
	(void)data;
	open_database(f, NULL);
}

static void
teardown(Fixture *f, gconstpointer data)
{
	(void)data;
	if (f->schema != NULL)
	{
		g_autofree gchar *sql = g_strdup_printf("DROP SCHEMA %s CASCADE", f->schema);

		exec(f, sql);
	}
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
	g_free(f->schema);
	venture_currency_clear_registered();
}

static gint64
item(Fixture *f, const gchar *sku, gboolean allow_negative)
{
	g_autoptr(VentureEntity) row = record(f, "inventory_item");

	g_object_set(row, "product-id", f->product, "sku", sku, "location", "WH",
		"allow-negative", allow_negative, NULL);
	save(f, row);
	return venture_entity_get_id(row);
}

static LvStore *
store(Fixture *f)
{
	g_autoptr(GError) error = NULL;
	LvStore *found = venture_stock_engine_get_store(f->db, &error);

	g_assert_no_error(error);
	g_assert_nonnull(found);
	return found;
}

static guint
engine_movements(Fixture *f)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) movements = lv_ledger_list_movements(store(f), 0, &error);

	g_assert_no_error(error);
	return movements->len;
}

/* What the engine holds of @item_id: on hand, and @currency's value. */
static gdouble
engine_on_hand(Fixture *f, gint64 item_id)
{
	g_autoptr(GError) error = NULL;
	gint64 engine_item = venture_stock_engine_find_item(f->db, item_id, &error);

	g_assert_no_error(error);
	g_assert_cmpint(engine_item, >, 0);
	return lv_ledger_on_hand(store(f), engine_item, NULL, NULL);
}

static gint64
engine_value(Fixture *f, gint64 item_id, const gchar *currency)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(LvCosts) costs = NULL;
	gint64 engine_item = venture_stock_engine_find_item(f->db, item_id, &error);
	gint64 amount = 0;

	g_assert_no_error(error);
	g_assert_cmpint(engine_item, >, 0);
	costs = lv_ledger_valuation(store(f), engine_item, NULL, &error);
	g_assert_no_error(error);
	return lv_costs_lookup(costs, currency, &amount) ? amount : 0;
}

/* What Venture's own service answers about @item_id. */
static gint64
on_hand(Fixture *f, gint64 item_id)
{
	g_autoptr(GError) error = NULL;
	gint64 units = venture_inventory_service_on_hand(venture_inventory_service_get(f->db), item_id, NULL, &error);

	g_assert_no_error(error);
	return units;
}

static gint64
item_value(Fixture *f, gint64 item_id, const gchar *currency)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) values = venture_inventory_service_item_value(venture_inventory_service_get(f->db),
		item_id, NULL, NULL, &error);
	const VentureMoney *found;

	g_assert_no_error(error);
	g_assert_nonnull(values);
	found = venture_money_totals_lookup(values, currency);
	return found != NULL ? venture_money_get_amount(found) : 0;
}

static void
assert_mirror(Fixture *f, gint64 item_id)
{
	g_autoptr(GError) error = NULL;

	g_assert_true(venture_stock_engine_check_mirror(f->db, item_id, &error));
	g_assert_no_error(error);
}

/* --- The shared transaction --------------------------------------------- */

static gboolean
refuse_journal(VentureDatabase *database, VentureEntity *entity, VentureEntity *previous, gpointer user_data,
	GError **error)
{
	gboolean *armed = user_data;

	(void)database;
	(void)entity;
	(void)previous;
	if (*armed)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "the books are closed");
		return FALSE;
	}
	return TRUE;
}

static guint
rows(Fixture *f, GType type)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	g_autoptr(GPtrArray) found = NULL;

	venture_query_set_limit(query, 0);
	found = venture_database_find(f->db, query, NULL);
	g_assert_nonnull(found);
	return found->len;
}

/* A receipt and an issue whose journals the books refuse leave nothing
 * in the engine: no movement, no lot, no layer used, and no row. */
static void
test_shared_transaction(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureMoney) unit = venture_money_new(1000, "USD", 2);
	VentureInventoryService *inventory = venture_inventory_service_get(f->db);
	gint64 widget = item(f, "W-1", FALSE);
	gboolean armed = FALSE;
	guint movements, txns, layers;

	(void)data;
	venture_database_add_save_validator(f->db, VENTURE_TYPE_JOURNAL, refuse_journal, &armed, NULL);
	g_assert_true(venture_inventory_service_receive(inventory, widget, 2, unit, NULL, 0, "bill:1", NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpfloat(engine_on_hand(f, widget), ==, 2);
	movements = engine_movements(f);
	txns = rows(f, VENTURE_TYPE_INVENTORY_TXN);
	layers = rows(f, VENTURE_TYPE_INVENTORY_COST_LAYER);

	armed = TRUE;
	g_assert_false(venture_inventory_service_receive(inventory, widget, 3, unit, NULL, 0, "bill:2", NULL,
		&error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_assert_false(venture_inventory_service_issue(inventory, widget, 1, NULL, "inventory_txn", 0, NULL, NULL,
		&error));
	g_assert_nonnull(error);
	g_clear_error(&error);

	g_assert_cmpuint(engine_movements(f), ==, movements);
	g_assert_cmpfloat(engine_on_hand(f, widget), ==, 2);
	g_assert_cmpint(engine_value(f, widget, "USD"), ==, 2000);
	g_assert_cmpuint(rows(f, VENTURE_TYPE_INVENTORY_TXN), ==, txns);
	g_assert_cmpuint(rows(f, VENTURE_TYPE_INVENTORY_COST_LAYER), ==, layers);
	assert_mirror(f, widget);

	armed = FALSE;
	g_assert_true(venture_inventory_service_issue(inventory, widget, 1, NULL, "inventory_txn", 0, NULL, NULL,
		&error));
	g_assert_no_error(error);
	g_assert_cmpfloat(engine_on_hand(f, widget), ==, 1);
	assert_mirror(f, widget);
}


/* --- Stock typed in by hand ---------------------------------------------- */

static gint64
count_in(Fixture *f, gint64 item_id, gint64 quantity)
{
	g_autoptr(VentureEntity) count = record(f, "inventory_txn");

	g_object_set(count, "inventory-item-id", item_id, "kind", VENTURE_INVENTORY_TXN_KIND_ADJUSTMENT,
		"quantity", quantity, NULL);
	save(f, count);
	return venture_entity_get_id(count);
}

/* Five counted, then two bought at 4.00: an issue of two takes the bought
 * ones, an issue of a third is refused (it would sell a unit with no
 * cost), a craft uses counted units at no cost, and a count that finds
 * three fewer leaves the bought units' value alone. */
static void
test_counted_stock(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) costs = NULL;
	g_autoptr(VentureMoney) unit = venture_money_new(400, "USD", 2);
	VentureInventoryService *inventory = venture_inventory_service_get(f->db);
	gint64 herb = item(f, "H-1", FALSE);
	gint64 potion = item(f, "P-1", FALSE);
	VentureInventoryDraw draw;

	(void)data;
	count_in(f, herb, 5);
	g_assert_true(venture_inventory_service_receive(inventory, herb, 2, unit, NULL, 0, "bill:1", NULL, &error));
	g_assert_no_error(error);
	g_assert_true(venture_inventory_service_receive(inventory, herb, 2, unit, NULL, 0, "bill:2", NULL, &error));
	g_assert_no_error(error);
	g_assert_true(venture_inventory_service_issue(inventory, herb, 2, NULL, "inventory_txn", 0, NULL, &costs,
		&error));
	g_assert_no_error(error);
	g_assert_cmpint(venture_money_get_amount(venture_money_totals_lookup(costs, "USD")), ==, 800);
	g_assert_cmpint(item_value(f, herb, "USD"), ==, 800);

	g_assert_false(venture_inventory_service_issue(inventory, herb, 3, NULL, "inventory_txn", 0, NULL, NULL,
		&error));
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "negative"));
	g_clear_error(&error);
	g_assert_cmpint(on_hand(f, herb), ==, 7);

	/* a count that finds three fewer: the value of the bought units stays */
	count_in(f, herb, -3);
	g_assert_cmpint(on_hand(f, herb), ==, 4);
	g_assert_cmpint(item_value(f, herb, "USD"), ==, 800);
	assert_mirror(f, herb);

	/* a craft takes the costed units first, then counted ones at no cost */
	draw.inventory_item_id = herb;
	draw.quantity = 3;
	g_assert_true(venture_inventory_service_produce(inventory, &draw, 1, potion, 1, NULL, "recipe:1", NULL, NULL,
		NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpint(on_hand(f, herb), ==, 1);
	g_assert_cmpint(item_value(f, herb, "USD"), ==, 0);
	g_assert_cmpint(item_value(f, potion, "USD"), ==, 800);
	assert_mirror(f, herb);
	assert_mirror(f, potion);

	/* a count below what is on hand, on an item that does not allow it */
	{
		g_autoptr(VentureEntity) over = record(f, "inventory_txn");

		g_object_set(over, "inventory-item-id", herb, "kind", VENTURE_INVENTORY_TXN_KIND_ADJUSTMENT,
			"quantity", (gint64)-2, NULL);
		g_assert_false(venture_database_save(f->db, over, NULL, &error));
		g_assert_nonnull(strstr(error->message, "negative"));
		g_clear_error(&error);
	}
	g_assert_cmpint(on_hand(f, herb), ==, 1);
	g_assert_cmpfloat(engine_on_hand(f, herb), ==, 1);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/venture-inventory-service/shared-transaction", Fixture, NULL, setup, test_shared_transaction,
		teardown);
	g_test_add("/venture-inventory-service/counted-stock", Fixture, NULL, setup, test_counted_stock, teardown);
	return g_test_run();
}
