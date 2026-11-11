/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * venture_integration_service_find_each: the active binding of many
 * organizations in one read, each exactly what
 * venture_integration_service_find() answers for that organization alone.
 *
 * The staff billing overview asks whether each customer business connected
 * its own Stripe. Asked one business at a time that is one statement per
 * customer, on the server's one loop. What breaks if this regresses: a
 * business reads as connected in the overview but not on its own page, or
 * the overview's statements grow with the number of customers again.
 */
#include <venture.h>
#include "venture-test-accounting.h"

typedef struct
{
	VentureDatabase *db;
	VentureIntegrationService *service;
	gint64 orgs[4];
} Fixture;

static gint64
organization_new(Fixture *f, const gchar *name)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureOrganization) organization = venture_organization_new();
	g_object_set(organization, "name", name, NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(organization), NULL, &error));
	g_assert_no_error(error);
	return venture_entity_get_id(VENTURE_ENTITY(organization));
}

static JsonNode *
settings(void)
{
	JsonNode *node = json_node_new(JSON_NODE_OBJECT);
	JsonObject *object = json_object_new();
	json_object_set_string_member(object, "secret_key", "fixture-secret");
	json_node_take_object(node, object);
	return node;
}

static VentureIntegrationConnection *
bind(Fixture *f, gint64 organization, const gchar *provider)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) node = settings();
	VentureIntegrationConnection *connection = venture_integration_service_configure(f->service, organization,
		provider, "acct_fixture", "test", node, 0, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(connection);
	return connection;
}

static void
setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) key = g_bytes_new_static("01234567890123456789012345678901", 32);
	g_autoptr(VentureIntegrationConnection) bound = NULL, disabled = NULL, other = NULL;
	gint64 version = 0;
	(void)data;
	f->db = venture_test_accounting_database(&error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->service = venture_integration_service_get(f->db);
	g_assert_true(venture_integration_service_set_key(f->service, key, &error));
	g_assert_no_error(error);
	/* bound; never bound; bound then disconnected; bound to another provider. */
	f->orgs[0] = organization_new(f, "Bound");
	f->orgs[1] = organization_new(f, "Never bound");
	f->orgs[2] = organization_new(f, "Disconnected");
	f->orgs[3] = organization_new(f, "Other provider");
	bound = bind(f, f->orgs[0], "stripe");
	disabled = bind(f, f->orgs[2], "stripe");
	g_object_get(disabled, "version", &version, NULL);
	g_assert_true(venture_integration_service_disable(f->service, f->orgs[2],
		venture_entity_get_id(VENTURE_ENTITY(disabled)), version, NULL, &error));
	g_assert_no_error(error);
	other = bind(f, f->orgs[3], "paypal");
}

static void
teardown(Fixture *f, gconstpointer data)
{
	(void)data;
	venture_test_accounting_database_cleanup(f->db);
	g_clear_object(&f->db);
}

static void
test_matches_find(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GHashTable) each = NULL, one = NULL;
	guint64 before, all_statements, one_statement;
	guint i, found = 0;
	(void)data;
	before = venture_database_get_statement_count(f->db);
	each = venture_integration_service_find_each(f->service, f->orgs, G_N_ELEMENTS(f->orgs), "stripe", &error);
	all_statements = venture_database_get_statement_count(f->db) - before;
	g_assert_no_error(error);
	g_assert_nonnull(each);
	for (i = 0; i < G_N_ELEMENTS(f->orgs); i++) {
		g_autoptr(GError) local = NULL;
		g_autoptr(VentureIntegrationConnection) alone = venture_integration_service_find(f->service, f->orgs[i],
			"stripe", &local);
		VentureIntegrationConnection *batch = g_hash_table_lookup(each, &f->orgs[i]);
		if (alone) {
			g_assert_nonnull(batch);
			g_assert_cmpint(venture_entity_get_id(VENTURE_ENTITY(batch)), ==, venture_entity_get_id(VENTURE_ENTITY(alone)));
			found++;
		} else {
			g_assert_error(local, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
			g_assert_null(batch);
		}
	}
	/* The fixture covers what it claims to. */
	g_assert_cmpuint(found, ==, 1);
	g_assert_cmpuint(g_hash_table_size(each), ==, 1);

	before = venture_database_get_statement_count(f->db);
	one = venture_integration_service_find_each(f->service, f->orgs, 1, "stripe", &error);
	one_statement = venture_database_get_statement_count(f->db) - before;
	g_assert_no_error(error);
	g_assert_cmpuint(g_hash_table_size(one), ==, 1);
	g_test_message("statements: %" G_GUINT64_FORMAT " for one organization, %" G_GUINT64_FORMAT " for four",
		one_statement, all_statements);
	g_assert_cmpuint(all_statements, ==, one_statement);
}

/* No organizations: nothing found, nothing read. Bad input is refused as
 * find refuses it. */
static void
test_edges(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GHashTable) none = NULL, refused = NULL;
	gint64 zero = 0;
	guint64 before;
	(void)data;
	before = venture_database_get_statement_count(f->db);
	none = venture_integration_service_find_each(f->service, f->orgs, 0, "stripe", &error);
	g_assert_no_error(error);
	g_assert_cmpuint(g_hash_table_size(none), ==, 0);
	g_assert_cmpuint(venture_database_get_statement_count(f->db), ==, before);
	refused = venture_integration_service_find_each(f->service, &zero, 1, "stripe", &error);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_null(refused);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/integration/find-each/matches-find", Fixture, NULL, setup, test_matches_find, teardown);
	g_test_add("/integration/find-each/edges", Fixture, NULL, setup, test_edges, teardown);
	return g_test_run();
}
