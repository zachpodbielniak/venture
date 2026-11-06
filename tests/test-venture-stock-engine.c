/*
 * test-venture-stock-engine.c - Venture's stock on the Libreventory engine
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Quantities, lots, cost layers and valuation are the engine's, kept in
 * Venture's own database and transaction. The inventory_txn and
 * inventory_cost_layer rows stay, as a mirror the service writes from the
 * engine's answer. Two things are pinned here (the inventory service's
 * own rules, the shared transaction among them, are in
 * test-venture-inventory-service.c):
 *
 * - The migration. A database written before the engine (receipts, a
 *   craft in two currencies, a hand count, stock below zero, a layer from
 *   before lots, a deleted movement) opens with the same quantity and the
 *   same value per currency, per item and per organization, and a second
 *   start imports nothing. On SQLite, and on PostgreSQL when
 *   VENTURE_TEST_MIGRATION_POSTGRES_URI names a throwaway server. What
 *   breaks if it regresses: an upgrade that loses stock, or values it
 *   differently from the books.
 * - The mirror. After each kind of movement the rows say what the engine
 *   says.
 */

#include <venture.h>

#include "goods/venture-stock-engine.h"
#include "venture-test-util.h"

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
setup_postgres(Fixture *f, gconstpointer data)
{
	(void)data;
	open_database(f, g_getenv("VENTURE_TEST_MIGRATION_POSTGRES_URI"));
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

static gint64
organization_value(Fixture *f, const gchar *currency)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) values = venture_inventory_service_valuation(venture_inventory_service_get(f->db),
		f->org, NULL, &error);
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

/* --- Rows as an older build wrote them --------------------------------- */

/* A movement, written the way the service wrote one before the engine:
 * a row of inventory_txns and nothing else. Gives its id. */
static gint64
old_txn(Fixture *f, gint64 item_id, const gchar *kind, gint64 quantity, const gchar *occurred,
	gboolean deleted)
{
	g_autofree gchar *uuid = g_uuid_string_random();
	g_autofree gchar *sql = g_strdup_printf(
		"INSERT INTO inventory_txns (uuid, organization_id, created_at, updated_at, deleted_at, version,"
		" inventory_item_id, kind, quantity, occurred_at, reference)"
		" VALUES ('%s', %" G_GINT64_FORMAT ", '%s', '%s', %s, 1, %" G_GINT64_FORMAT ", '%s', %"
		G_GINT64_FORMAT ", '%s', 'older build')",
		uuid, f->org, occurred, occurred, deleted ? "'2026-03-30T00:00:00Z'" : "NULL", item_id, kind,
		quantity, occurred);
	g_autofree gchar *select = g_strdup_printf("SELECT id FROM inventory_txns WHERE uuid = '%s'", uuid);
	g_autoptr(GError) error = NULL;
	g_autoptr(OrmResult) result = NULL;

	exec(f, sql);
	result = venture_database_query_raw(f->db, select, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(orm_result_next(result));
	return orm_row_get_integer(orm_result_get_row(result), 0);
}

/* A cost layer: @remaining of @original units at @unit minor units. */
static void
old_layer(Fixture *f, gint64 item_id, gint64 lot, gint64 original, gint64 remaining, gint64 unit,
	const gchar *currency, gint64 exponent, const gchar *received)
{
	g_autofree gchar *uuid = g_uuid_string_random();
	g_autofree gchar *sql = g_strdup_printf(
		"INSERT INTO inventory_cost_layers (uuid, organization_id, created_at, updated_at, version,"
		" inventory_item_id, goods_receipt_line_id, received_at, original_qty, remaining_qty,"
		" unit_cost_amount, unit_cost_currency, unit_cost_exponent, lot_txn_id)"
		" VALUES ('%s', %" G_GINT64_FORMAT ", '%s', '%s', 1, %" G_GINT64_FORMAT ", 0, '%s', %"
		G_GINT64_FORMAT ", %" G_GINT64_FORMAT ", %" G_GINT64_FORMAT ", '%s', %" G_GINT64_FORMAT ", %"
		G_GINT64_FORMAT ")",
		uuid, f->org, received, received, item_id, received, original, remaining, unit, currency, exponent, lot);

	exec(f, sql);
}

typedef struct
{
	gint64 receipts;	/* USD: two receipts, one used up */
	gint64 charm;		/* GOLD and TICKET: a craft's lot, partly sold */
	gint64 counted;		/* USD: a hand count beside a receipt */
	gint64 owed;		/* below zero, allowed */
	gint64 legacy;		/* a layer from before lots */
	gint64 deleted;		/* only a deleted movement */
} OldStock;

static void
old_stock(Fixture *f, OldStock *s)
{
	gint64 lot;

	s->receipts = item(f, "R-1", FALSE);
	lot = old_txn(f, s->receipts, "purchase", 2, "2026-04-01T09:00:00Z", FALSE);
	old_layer(f, s->receipts, lot, 2, 0, 1000, "USD", 2, "2026-04-01T09:00:00Z");
	lot = old_txn(f, s->receipts, "purchase", 3, "2026-04-02T09:00:00Z", FALSE);
	old_layer(f, s->receipts, lot, 3, 2, 1200, "USD", 2, "2026-04-02T09:00:00Z");
	old_txn(f, s->receipts, "sale", -3, "2026-04-03T09:00:00Z", FALSE);

	/* 3 charms made from 1.0000 GOLD and 50 TICKET, then one sold:
	 * 3334 GOLD and 17 TICKET left with it. */
	s->charm = item(f, "C-1", FALSE);
	lot = old_txn(f, s->charm, "production", 3, "2026-04-04T10:00:00Z", FALSE);
	old_layer(f, s->charm, lot, 1, 0, 3334, "GOLD", 4, "2026-04-04T10:00:00Z");
	old_layer(f, s->charm, lot, 2, 2, 3333, "GOLD", 4, "2026-04-04T10:00:00Z");
	old_layer(f, s->charm, lot, 2, 1, 17, "TICKET", 0, "2026-04-04T10:00:00Z");
	old_layer(f, s->charm, lot, 1, 1, 16, "TICKET", 0, "2026-04-04T10:00:00Z");
	old_txn(f, s->charm, "sale", -1, "2026-04-05T10:00:00Z", FALSE);

	s->counted = item(f, "H-1", FALSE);
	old_txn(f, s->counted, "adjustment", 5, "2026-04-01T08:00:00Z", FALSE);
	lot = old_txn(f, s->counted, "purchase", 2, "2026-04-02T08:00:00Z", FALSE);
	old_layer(f, s->counted, lot, 2, 2, 400, "USD", 2, "2026-04-02T08:00:00Z");

	s->owed = item(f, "N-1", TRUE);
	old_txn(f, s->owed, "sale", -2, "2026-04-06T08:00:00Z", FALSE);

	s->legacy = item(f, "L-1", FALSE);
	old_txn(f, s->legacy, "purchase", 4, "2026-03-01T08:00:00Z", FALSE);
	old_layer(f, s->legacy, 0, 4, 4, 250, "USD", 2, "2026-03-01T08:00:00Z");

	s->deleted = item(f, "D-1", FALSE);
	old_txn(f, s->deleted, "adjustment", 100, "2026-03-02T08:00:00Z", TRUE);
}

/* --- The migration ------------------------------------------------------ */

typedef struct
{
	gint64 item;
	gint64 quantity;
	const gchar *currency;
	gint64 value;
} Expected;

static void
check_migration(Fixture *f)
{
	OldStock s;
	g_autoptr(GError) error = NULL;
	guint movements;
	guint i;

	old_stock(f, &s);
	{
		/* What the older build said, from its own rows. */
		const Expected expected[] = {
			{ s.receipts, 2, "USD", 2400 },
			{ s.charm, 2, "GOLD", 6666 },
			{ s.charm, 2, "TICKET", 33 },
			{ s.counted, 7, "USD", 800 },
			{ s.owed, -2, "USD", 0 },
			{ s.legacy, 4, "USD", 1000 },
			{ s.deleted, 0, "USD", 0 },
		};

		g_assert_true(venture_stock_engine_migrate(f->db, &error));
		g_assert_no_error(error);
		for (i = 0; i < G_N_ELEMENTS(expected); i++)
		{
			g_assert_cmpfloat(engine_on_hand(f, expected[i].item), ==, (gdouble)expected[i].quantity);
			g_assert_cmpint(on_hand(f, expected[i].item), ==, expected[i].quantity);
			g_assert_cmpint(engine_value(f, expected[i].item, expected[i].currency), ==, expected[i].value);
			g_assert_cmpint(item_value(f, expected[i].item, expected[i].currency), ==, expected[i].value);
			assert_mirror(f, expected[i].item);
		}
	}
	g_assert_cmpint(organization_value(f, "USD"), ==, 2400 + 800 + 1000);
	g_assert_cmpint(organization_value(f, "GOLD"), ==, 6666);
	g_assert_cmpint(organization_value(f, "TICKET"), ==, 33);

	/* A restart imports nothing twice. */
	movements = engine_movements(f);
	g_assert_true(venture_stock_engine_migrate(f->db, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(engine_movements(f), ==, movements);
	g_assert_cmpint(organization_value(f, "USD"), ==, 2400 + 800 + 1000);

	/* The lots came across whole: the next charm leaves with both of
	 * its currencies, and the counted stock is used after the costed. */
	{
		g_autoptr(GPtrArray) costs = NULL;

		g_assert_true(venture_inventory_service_issue(venture_inventory_service_get(f->db), s.charm, 1,
			NULL, "inventory_txn", 0, NULL, &costs, &error));
		g_assert_no_error(error);
		g_assert_cmpint(venture_money_get_amount(venture_money_totals_lookup(costs, "GOLD")), ==, 3333);
		g_assert_cmpint(venture_money_get_amount(venture_money_totals_lookup(costs, "TICKET")), ==, 17);
		g_clear_pointer(&costs, g_ptr_array_unref);
		g_assert_true(venture_inventory_service_issue(venture_inventory_service_get(f->db), s.counted, 2,
			NULL, "inventory_txn", 0, NULL, &costs, &error));
		g_assert_no_error(error);
		g_assert_cmpint(venture_money_get_amount(venture_money_totals_lookup(costs, "USD")), ==, 800);
		assert_mirror(f, s.charm);
		assert_mirror(f, s.counted);
	}
}

static void
test_migration(Fixture *f, gconstpointer data)
{
	(void)data;
	check_migration(f);
}

static void
test_migration_postgres(Fixture *f, gconstpointer data)
{
	(void)data;
	check_migration(f);
}

/* --- The mirror --------------------------------------------------------- */

/* Every kind of movement, and the rows agree with the engine after each:
 * a receipt, a hand count, an issue, a transfer, a craft, a purchase paid
 * outright, an issue to an account, a return and a session's yield. */
static void
test_mirror(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureMoney) unit = venture_money_new(1000, "USD", 2);
	g_autoptr(VentureMoney) total = venture_money_new(10000, "USD", 2);
	g_autoptr(VentureEntity) count = NULL;
	VentureInventoryService *inventory = venture_inventory_service_get(f->db);
	gint64 shelf = item(f, "W-1", FALSE);
	gint64 shop = item(f, "W-2", FALSE);
	gint64 made = item(f, "M-1", FALSE);
	gint64 cash = 0;
	VentureInventoryDraw draws[1];
	gint64 all[3];
	guint i;

	(void)data;
	all[0] = shelf;
	all[1] = shop;
	all[2] = made;
	g_assert_true(venture_inventory_service_receive(inventory, shelf, 4, unit, NULL, 0, "bill:1", NULL, &error));
	g_assert_no_error(error);
	count = record(f, "inventory_txn");
	g_object_set(count, "inventory-item-id", shelf, "kind", VENTURE_INVENTORY_TXN_KIND_ADJUSTMENT,
		"quantity", (gint64)2, NULL);
	save(f, count);
	g_assert_cmpint(on_hand(f, shelf), ==, 6);
	g_assert_true(venture_inventory_service_issue(inventory, shelf, 1, NULL, "inventory_txn", 0, NULL, NULL,
		&error));
	g_assert_no_error(error);
	g_assert_true(venture_inventory_service_transfer(inventory, shelf, shop, 2, NULL, NULL, &error));
	g_assert_no_error(error);
	draws[0].inventory_item_id = shop;
	draws[0].quantity = 1;
	g_assert_true(venture_inventory_service_produce(inventory, draws, 1, made, 3, NULL, "recipe:1", NULL, NULL,
		NULL, &error));
	g_assert_no_error(error);
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNT);
		g_autoptr(VentureEntity) account = NULL;

		venture_query_set_organization(query, f->org);
		venture_query_set_limit(query, 1);
		account = venture_database_find_one(f->db, query, &error);
		g_assert_no_error(error);
		g_assert_nonnull(account);
		cash = venture_entity_get_id(account);
	}
	g_assert_true(venture_inventory_service_receive_from(inventory, made, 7, total, NULL, "leg:1", cash, NULL,
		NULL, &error));
	g_assert_no_error(error);
	g_assert_true(venture_inventory_service_issue_to(inventory, made, 2, NULL, "leg:2", cash,
		VENTURE_INVENTORY_TXN_KIND_SALE, NULL, NULL, NULL, &error));
	g_assert_no_error(error);
	g_assert_true(venture_inventory_service_restore(inventory, shelf, 1, unit, NULL, 0, "return", NULL, &error));
	g_assert_no_error(error);
	g_assert_true(venture_inventory_service_produce(inventory, NULL, 0, made, 2, NULL, "session:1", NULL, NULL,
		NULL, &error));
	g_assert_no_error(error);

	g_assert_cmpint(on_hand(f, shelf), ==, 2);
	g_assert_cmpint(on_hand(f, shop), ==, 1);
	g_assert_cmpint(on_hand(f, made), ==, 3 + 7 - 2 + 2);
	for (i = 0; i < G_N_ELEMENTS(all); i++)
	{
		g_assert_cmpfloat(engine_on_hand(f, all[i]), ==, (gdouble)on_hand(f, all[i]));
		assert_mirror(f, all[i]);
	}
}

/* A movement row cannot be deleted: it is the engine's history too. A
 * count is corrected with another count. */
static void
test_delete_refused(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) count = record(f, "inventory_txn");
	gint64 shelf = item(f, "W-1", FALSE);

	(void)data;
	g_object_set(count, "inventory-item-id", shelf, "kind", VENTURE_INVENTORY_TXN_KIND_ADJUSTMENT,
		"quantity", (gint64)5, NULL);
	save(f, count);
	g_assert_false(venture_database_delete(f->db, count, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_cmpint(on_hand(f, shelf), ==, 5);
	g_assert_cmpfloat(engine_on_hand(f, shelf), ==, 5);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/venture-stock-engine/migration/sqlite", Fixture, NULL, setup, test_migration, teardown);
	if (g_getenv("VENTURE_TEST_MIGRATION_POSTGRES_URI") != NULL)
		g_test_add("/venture-stock-engine/migration/postgresql", Fixture, NULL, setup_postgres,
			test_migration_postgres, teardown);
	g_test_add("/venture-stock-engine/mirror", Fixture, NULL, setup, test_mirror, teardown);
	g_test_add("/venture-stock-engine/delete-refused", Fixture, NULL, setup, test_delete_refused, teardown);
	return g_test_run();
}
