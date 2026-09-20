/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <string.h>
#include "venture-test-accounting.h"

/* Every integration must enter the same metadata and permission machinery. */
static void
test_registered(void)
{
	g_assert_cmpuint(venture_entity_registry_lookup(
		venture_entity_registry_get_default(), "integration_connection"), !=, G_TYPE_INVALID);
}


typedef struct
{
	VentureDatabase *db;
	VentureIntegrationService *service;
	gint64 a;
	gint64 b;
} Fixture;

static void
setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureOrganization) a = venture_organization_new();
	g_autoptr(VentureOrganization) b = venture_organization_new();
	g_autoptr(GBytes) key = g_bytes_new_static("01234567890123456789012345678901", 32);
	(void)data;
	f->db = venture_test_accounting_database(&error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	g_object_set(a, "name", "A", NULL);
	g_object_set(b, "name", "B", NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(a), NULL, &error));
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(b), NULL, &error));
	g_assert_no_error(error);
	f->a = venture_entity_get_id(VENTURE_ENTITY(a));
	f->b = venture_entity_get_id(VENTURE_ENTITY(b));
	f->service = venture_integration_service_get(f->db);
	g_assert_true(venture_integration_service_set_key(f->service, key, &error));
	g_assert_no_error(error);
}

static void
teardown(Fixture *f, gconstpointer data)
{
	(void)data;
	venture_test_accounting_database_cleanup(f->db);
	g_clear_object(&f->db);
}

static JsonNode *
settings(const gchar *secret)
{
	JsonNode *node = json_node_new(JSON_NODE_OBJECT);
	JsonObject *object = json_object_new();
	json_object_set_string_member(object, "secret_key", secret);
	json_node_take_object(node, object);
	return node;
}

static VentureIntegrationConnection *
connect_account(Fixture *f, gint64 org, const gchar *secret)
{
	g_autoptr(JsonNode) node = settings(secret);
	g_autoptr(GError) error = NULL;
	VentureIntegrationConnection *result = venture_integration_service_configure(f->service,
		org, "stripe", "acct_same", "test", node, 0, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	return result;
}

/* Identical provider IDs in two organizations must never select authority. */
static void
test_isolation_rotation(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureIntegrationConnection) a = connect_account(f, f->a, "secret-A");
	g_autoptr(VentureIntegrationConnection) b = connect_account(f, f->b, "secret-B");
	g_autoptr(VentureIntegrationConnection) rotated = NULL;
	g_autoptr(JsonNode) plain = NULL, public = NULL, replacement = settings("secret-A2");
	g_autofree gchar *stored = NULL, *serialized = NULL;
	g_autoptr(GError) error = NULL;
	gint64 id = venture_entity_get_id(VENTURE_ENTITY(a));
	gint64 version = venture_entity_get_version(VENTURE_ENTITY(a));
	(void)data;
	plain = venture_integration_service_resolve(f->service, f->a, id, FALSE, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(plain), "secret_key"), ==, "secret-A");
	g_clear_pointer(&plain, json_node_unref);
	plain = venture_integration_service_resolve(f->service, f->b, id, FALSE, &error);
	g_assert_null(plain);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_clear_error(&error);
	g_object_get(a, "sealed-settings", &stored, NULL);
	g_assert_nonnull(stored);
	g_assert_null(strstr(stored, "secret-A"));
	public = venture_serializable_to_json(VENTURE_SERIALIZABLE(a), FALSE);
	serialized = venture_json_to_string(public, FALSE);
	g_assert_null(strstr(serialized, "sealed_settings"));
	g_assert_null(strstr(serialized, "secret-A"));
	rotated = venture_integration_service_configure(f->service, f->a, "stripe", "acct_same", "test",
		replacement, version, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(rotated);
	g_assert_cmpint(venture_entity_get_version(VENTURE_ENTITY(rotated)), >, version);
	plain = venture_integration_service_resolve(f->service, f->a, id, FALSE, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(plain), "secret_key"), ==, "secret-A2");
	g_clear_pointer(&plain, json_node_unref);
	plain = venture_integration_service_resolve(f->service, f->b, venture_entity_get_id(VENTURE_ENTITY(b)), FALSE, &error);
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(plain), "secret_key"), ==, "secret-B");
	g_clear_object(&rotated);
	rotated = venture_integration_service_configure(f->service, f->a, "stripe", "acct_same", "test",
		replacement, version, NULL, &error);
	g_assert_null(rotated);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
}

/* Disconnect stops new work, but verified late callbacks can name old identity.
 * Replacing an account never makes an old callback use new credentials. */
static void
test_disconnect_replacement(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureIntegrationConnection) old = connect_account(f, f->a, "old-secret");
	g_autoptr(VentureIntegrationConnection) next = NULL, active = NULL;
	g_autoptr(JsonNode) node = settings("new-secret"), plain = NULL;
	g_autoptr(GError) error = NULL;
	gint64 id = venture_entity_get_id(VENTURE_ENTITY(old));
	gint64 version = venture_entity_get_version(VENTURE_ENTITY(old));
	(void)data;
	next = venture_integration_service_configure(f->service, f->a, "stripe", "acct_next", "live", node, version, NULL, &error);
	g_assert_null(next);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	g_clear_error(&error);
	g_assert_true(venture_integration_service_disable(f->service, f->a, id, version, NULL, &error));
	g_assert_no_error(error);
	plain = venture_integration_service_resolve(f->service, f->a, id, FALSE, &error);
	g_assert_null(plain);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
	g_clear_error(&error);
	active = venture_integration_service_find(f->service, f->a, "stripe", &error);
	g_assert_null(active);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
	g_clear_error(&error);
	next = venture_integration_service_configure(f->service, f->a, "stripe", "acct_next", "live", node, 0, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(next);
	g_assert_cmpint(venture_entity_get_id(VENTURE_ENTITY(next)), !=, id);
	plain = venture_integration_service_resolve(f->service, f->a, id, TRUE, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(plain), "secret_key"), ==, "old-secret");
	g_assert_false(venture_database_delete(f->db, VENTURE_ENTITY(old), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);
	g_object_set(next, "account-id", "forged", NULL);
	g_assert_false(venture_database_save(f->db, VENTURE_ENTITY(next), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
}

/* An API/editor scope must not turn generic data-write permission into access
 * management, and missing configuration never borrows another org's account. */
static void
test_refusals(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureIntegrationConnection) connection = NULL;
	g_autoptr(JsonNode) node = settings("never-log-this");
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureAccessScope) scope = NULL;
	g_autoptr(GBytes) short_key = g_bytes_new_static("bad", 3);
	g_autoptr(GBytes) other_key = g_bytes_new_static("abcdefghijklmnopqrstuvwxyz123456", 32);
	VentureAuthPrincipal principal;
	(void)data;
	g_assert_false(venture_integration_service_set_key(f->service, short_key, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
	g_clear_error(&error);
	g_assert_false(venture_integration_service_set_key(f->service, other_key, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	g_clear_error(&error);
	connection = venture_integration_service_find(f->service, f->b, "stripe", &error);
	g_assert_null(connection);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
	g_clear_error(&error);
	connection = venture_integration_service_configure(f->service, 0, "stripe", "acct", "test", node, 0, NULL, &error);
	g_assert_null(connection);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	principal.user_id = 0;
	principal.token_id = 0;
	principal.role = VENTURE_USER_ROLE_EDITOR;
	principal.name = (gchar *)"editor";
	principal.authenticated = TRUE;
	scope = venture_access_policy_enter(venture_database_get_access_policy(f->db), &principal);
	connection = venture_integration_service_configure(f->service, f->a, "stripe", "acct", "test", node, 0, NULL, &error);
	g_assert_null(connection);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_assert_null(strstr(error->message, "never-log-this"));
}


/* Even a storage-level ciphertext transplant cannot cross the authenticated
 * record identity. The raw SQL here deliberately models tampered storage. */
static void
test_ciphertext_binding(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureIntegrationConnection) a = connect_account(f, f->a, "secret-A");
	g_autoptr(VentureIntegrationConnection) b = connect_account(f, f->b, "secret-B");
	g_autoptr(JsonNode) plain = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *sql = NULL;
	gint64 aid = venture_entity_get_id(VENTURE_ENTITY(a));
	gint64 bid = venture_entity_get_id(VENTURE_ENTITY(b));
	(void)data;
	sql = g_strdup_printf("UPDATE integration_connections SET sealed_settings = "
		"(SELECT sealed_settings FROM integration_connections WHERE id = %" G_GINT64_FORMAT ") WHERE id = %" G_GINT64_FORMAT, aid, bid);
	g_assert_true(venture_database_execute(f->db, sql, NULL, &error));
	g_assert_no_error(error);
	plain = venture_integration_service_resolve(f->service, f->b, bid, FALSE, &error);
	g_assert_null(plain);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
	g_assert_null(strstr(error->message, "secret-A"));
	g_clear_error(&error);
	plain = venture_integration_service_resolve(f->service, f->a, aid, FALSE, &error);
	g_assert_no_error(error);
	g_assert_nonnull(plain);
	g_clear_pointer(&plain, json_node_unref);
	g_clear_pointer(&sql, g_free);
	sql = g_strdup_printf("UPDATE integration_connections SET environment = 'live' WHERE id = %" G_GINT64_FORMAT, aid);
	g_assert_true(venture_database_execute(f->db, sql, NULL, &error));
	plain = venture_integration_service_resolve(f->service, f->a, aid, FALSE, &error);
	g_assert_null(plain);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
}

/* Stored data survives the service's lifetime; a new instance still requires
 * the matching key rather than returning an empty or guessed credential. */
static void
test_service_restart(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureIntegrationConnection) a = connect_account(f, f->a, "survives");
	g_autoptr(GBytes) key = g_bytes_new_static("01234567890123456789012345678901", 32);
	g_autoptr(GBytes) other = g_bytes_new_static("abcdefghijklmnopqrstuvwxyz123456", 32);
	g_autoptr(JsonNode) plain = NULL;
	g_autoptr(GError) error = NULL;
	gint64 id = venture_entity_get_id(VENTURE_ENTITY(a));
	(void)data;
	g_object_set_data(G_OBJECT(f->db), "venture-integration-service", NULL);
	f->service = venture_integration_service_get(f->db);
	g_assert_true(venture_integration_service_set_key(f->service, key, &error));
	plain = venture_integration_service_resolve(f->service, f->a, id, FALSE, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(plain), "secret_key"), ==, "survives");
	g_clear_pointer(&plain, json_node_unref);
	g_object_set_data(G_OBJECT(f->db), "venture-integration-service", NULL);
	f->service = venture_integration_service_get(f->db);
	g_assert_true(venture_integration_service_set_key(f->service, other, &error));
	plain = venture_integration_service_resolve(f->service, f->a, id, FALSE, &error);
	g_assert_null(plain);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	venture_entity_registry_register_builtins(venture_entity_registry_get_default());
	g_test_add_func("/integrations/registered", test_registered);
	g_test_add("/integrations/isolation-rotation", Fixture, NULL, setup, test_isolation_rotation, teardown);
	g_test_add("/integrations/disconnect-replacement", Fixture, NULL, setup, test_disconnect_replacement, teardown);
	g_test_add("/integrations/refusals", Fixture, NULL, setup, test_refusals, teardown);
	g_test_add("/integrations/ciphertext-binding", Fixture, NULL, setup, test_ciphertext_binding, teardown);
	g_test_add("/integrations/service-restart", Fixture, NULL, setup, test_service_restart, teardown);
	return g_test_run();
}
