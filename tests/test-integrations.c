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
	plain = venture_integration_service_resolve_version(f->service, f->a, id, version, FALSE, &error);
	g_assert_null(plain);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	g_clear_error(&error);
	plain = venture_integration_service_resolve_version(f->service, f->a, id,
		venture_entity_get_version(VENTURE_ENTITY(rotated)), FALSE, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(plain), "secret_key"), ==, "secret-A2");
	g_clear_pointer(&plain, json_node_unref);
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


/* Organization administration grants neither neighboring credentials nor
 * permission after the membership that authorized a settings page is revoked. */
static void
test_organization_admin(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureUser) user = venture_user_new();
	g_autoptr(VentureOrganizationMembership) member = venture_organization_membership_new();
	g_autoptr(VentureIntegrationConnection) own = NULL, other = NULL;
	g_autoptr(JsonNode) node = settings("org-admin-secret");
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureAccessScope) scope = NULL;
	VentureAccessPolicy *policy = venture_database_get_access_policy(f->db);
	VentureAuthPrincipal principal;
	gint64 id, version;
	(void)data;
	g_object_set(user, "username", "integration-admin", "active", TRUE, "role", VENTURE_USER_ROLE_EDITOR, NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(user), NULL, &error));
	g_assert_no_error(error);
	venture_entity_set_organization_id(VENTURE_ENTITY(member), f->a);
	g_object_set(member, "user-id", venture_entity_get_id(VENTURE_ENTITY(user)), "active", TRUE,
		"role", VENTURE_ORGANIZATION_ROLE_ADMIN, NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(member), NULL, &error));
	g_assert_no_error(error);
	principal.user_id = venture_entity_get_id(VENTURE_ENTITY(user));
	principal.token_id = 0;
	principal.role = VENTURE_USER_ROLE_EDITOR;
	principal.name = (gchar *)"integration-admin";
	principal.authenticated = TRUE;
	scope = venture_access_policy_enter(policy, &principal);
	own = venture_integration_service_configure(f->service, f->a, "stripe", "acct_admin", "test", node, 0, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(own);
	other = venture_integration_service_configure(f->service, f->b, "stripe", "acct_admin", "test", node, 0, NULL, &error);
	g_assert_null(other);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);
	g_clear_object(&scope);
	g_object_set(member, "active", FALSE, NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(member), NULL, &error));
	g_assert_no_error(error);
	id = venture_entity_get_id(VENTURE_ENTITY(own));
	version = venture_entity_get_version(VENTURE_ENTITY(own));
	scope = venture_access_policy_enter(policy, &principal);
	g_assert_false(venture_integration_service_disable(f->service, f->a, id, version, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
}


/* A 64 KiB credential object is accepted; one byte more is refused without
 * rotating the saved version or leaking the rejected object in diagnostics. */
static void
test_bounds_audit_lifecycle(Fixture *f, gconstpointer data)
{
	g_autoptr(JsonNode) empty = settings(""), node = NULL, resolved = NULL;
	g_autofree gchar *encoded = venture_json_to_string(empty, FALSE), *value = NULL, *sql = NULL;
	g_autoptr(VentureIntegrationConnection) row = NULL, rejected = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_AUDIT_ENTRY);
	g_autoptr(GPtrArray) audits = NULL;
	g_autoptr(GError) error = NULL;
	gint64 id, version;
	guint i;
	(void)data;
	value = g_strnfill(65536 - strlen(encoded), 'x');
	node = settings(value);
	row = venture_integration_service_configure(f->service, f->a, "stripe", "acct_bound", "test", node, 0, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(row);
	id = venture_entity_get_id(VENTURE_ENTITY(row));
	version = venture_entity_get_version(VENTURE_ENTITY(row));
	g_clear_pointer(&node, json_node_unref);
	g_clear_pointer(&value, g_free);
	value = g_strnfill(65537 - strlen(encoded), 'x');
	node = settings(value);
	rejected = venture_integration_service_configure(f->service, f->a, "stripe", "acct_bound", "test", node, version, NULL, &error);
	g_assert_null(rejected);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_null(strstr(error->message, "xxxx"));
	g_clear_error(&error);
	g_assert_false(venture_database_restore(f->db, VENTURE_ENTITY(row), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);
	g_assert_false(venture_database_purge(f->db, VENTURE_ENTITY(row), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);
	venture_query_set_limit(query, 0);
	audits = venture_database_find(f->db, query, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(audits->len, >, 0);
	for (i = 0; i < audits->len; i++)
	{
		g_autofree gchar *diff = NULL;
		g_object_get(g_ptr_array_index(audits, i), "diff", &diff, NULL);
		if (diff)
		{
			g_assert_null(strstr(diff, "xxxx"));
			g_assert_null(strstr(diff, "sealed_settings"));
			g_assert_null(strstr(diff, "sealed-settings"));
		}
	}
	sql = g_strdup_printf("UPDATE integration_connections SET sealed_settings = 'v1:AA==' WHERE id = %" G_GINT64_FORMAT, id);
	g_assert_true(venture_database_execute(f->db, sql, NULL, &error));
	resolved = venture_integration_service_resolve(f->service, f->a, id, FALSE, &error);
	g_assert_null(resolved);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
}

/* Only the child changes the environment; the suite never borrows a live key. */
static void
test_environment_key(Fixture *f, gconstpointer data)
{
	(void)data;
	if (g_test_subprocess())
	{
		g_autoptr(JsonNode) node = settings("environment-fixture");
		g_autoptr(VentureIntegrationConnection) row = NULL;
		g_autoptr(GError) error = NULL;
		g_autofree gchar *valid = g_base64_encode((const guchar *)"01234567890123456789012345678901", 32);
		g_object_set_data(G_OBJECT(f->db), "venture-integration-service", NULL);
		f->service = venture_integration_service_get(f->db);
		g_setenv("VENTURE_INTEGRATION_KEY", "", TRUE);
		row = venture_integration_service_configure(f->service, f->a, "stripe", "acct_key", "test", node, 0, NULL, &error);
		g_assert_null(row);
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
		g_clear_error(&error);
		g_setenv("VENTURE_INTEGRATION_KEY", "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!", TRUE);
		row = venture_integration_service_configure(f->service, f->a, "stripe", "acct_key", "test", node, 0, NULL, &error);
		g_assert_null(row);
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
		g_clear_error(&error);
		g_setenv("VENTURE_INTEGRATION_KEY", valid, TRUE);
		row = venture_integration_service_configure(f->service, f->a, "stripe", "acct_key", "test", node, 0, NULL, &error);
		g_assert_no_error(error);
		g_assert_nonnull(row);
		return;
	}
	g_test_trap_subprocess(NULL, 120 * G_USEC_PER_SEC, G_TEST_SUBPROCESS_DEFAULT);
	g_test_trap_assert_passed();
}

/* A service-local lock cannot protect against another database connection.
 * Bypass the service to prove the database itself enforces the invariant. */
static void
test_active_uniqueness(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureIntegrationConnection) a = connect_account(f, f->a, "secret-A");
	g_autoptr(VentureIntegrationConnection) b = connect_account(f, f->b, "secret-B");
	g_autoptr(GError) error = NULL;
	g_autofree gchar *sql = NULL;
	(void)data;
	sql = g_strdup_printf("UPDATE integration_connections SET organization_id = %" G_GINT64_FORMAT
		" WHERE id = %" G_GINT64_FORMAT, f->a, venture_entity_get_id(VENTURE_ENTITY(b)));
	g_assert_false(venture_database_execute(f->db, sql, NULL, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_assert_true(venture_integration_service_disable(f->service, f->b,
		venture_entity_get_id(VENTURE_ENTITY(b)), venture_entity_get_version(VENTURE_ENTITY(b)), NULL, &error));
	g_assert_no_error(error);
	/* Historical bindings may share the active provider's organization. */
	g_assert_true(venture_database_execute(f->db, sql, NULL, &error));
	g_assert_no_error(error);
	g_clear_pointer(&sql, g_free);
	sql = g_strdup_printf("UPDATE integration_connections SET enabled = TRUE WHERE id = %" G_GINT64_FORMAT,
		venture_entity_get_id(VENTURE_ENTITY(b)));
	g_assert_false(venture_database_execute(f->db, sql, NULL, &error));
	g_assert_nonnull(error);
}

/* An upgrade must refuse ambiguous legacy accounts without selecting one,
 * disabling either binding, or destroying the ciphertext needed for repair. */
static void
test_ambiguous_upgrade(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureIntegrationConnection) a = connect_account(f, f->a, "secret-A");
	g_autoptr(VentureIntegrationConnection) b = connect_account(f, f->b, "secret-B");
	g_autoptr(VentureEntity) stored = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *sql = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	gboolean enabled = FALSE;
	(void)data;
	g_object_get(b, "sealed-settings", &before, NULL);
	g_assert_true(venture_database_execute(f->db,
		"DROP INDEX uq_integration_connections_organization_provider_when_enabled", NULL, &error));
	sql = g_strdup_printf("UPDATE integration_connections SET organization_id = %" G_GINT64_FORMAT
		" WHERE id = %" G_GINT64_FORMAT, f->a, venture_entity_get_id(VENTURE_ENTITY(b)));
	g_assert_true(venture_database_execute(f->db, sql, NULL, &error));
	g_assert_no_error(error);
	g_assert_false(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	stored = venture_database_get(f->db, VENTURE_TYPE_INTEGRATION_CONNECTION,
		venture_entity_get_id(VENTURE_ENTITY(b)), &error);
	g_assert_no_error(error);
	g_assert_nonnull(stored);
	g_object_get(stored, "enabled", &enabled, "sealed-settings", &after, NULL);
	g_assert_true(enabled);
	g_assert_cmpstr(before, ==, after);
	g_assert_cmpint(venture_entity_get_organization_id(stored), ==, f->a);
}

/* A master-key change must cover inactive history as well as current accounts;
 * otherwise an old payment's verified callback becomes unrecoverable. */
static void
test_master_key(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureIntegrationConnection) a = connect_account(f, f->a, "retained-A");
	g_autoptr(VentureIntegrationConnection) b = connect_account(f, f->b, "retained-B");
	g_autoptr(GBytes) old_key = g_bytes_new_static("01234567890123456789012345678901", 32);
	g_autoptr(GBytes) new_key = g_bytes_new_static("abcdefghijklmnopqrstuvwxyz123456", 32);
	g_autoptr(JsonNode) plain = NULL;
	g_autoptr(VentureEntity) checked = NULL;
	g_autofree gchar *before = NULL, *after = NULL;
	g_autoptr(GError) error = NULL;
	gint64 aid = venture_entity_get_id(VENTURE_ENTITY(a));
	gint64 bid = venture_entity_get_id(VENTURE_ENTITY(b));
	gint64 previous = venture_entity_get_version(VENTURE_ENTITY(a));
	(void)data;
	g_assert_true(venture_integration_service_disable(f->service, f->b, bid,
		venture_entity_get_version(VENTURE_ENTITY(b)), NULL, &error));
	g_object_get(a, "sealed-settings", &before, NULL);
	g_assert_true(venture_integration_service_verify_key(f->service, &error));
	g_assert_no_error(error);
	checked = venture_database_get(f->db, VENTURE_TYPE_INTEGRATION_CONNECTION, aid, &error);
	g_object_get(checked, "sealed-settings", &after, NULL);
	g_assert_cmpstr(before, ==, after);
	g_assert_cmpint(venture_entity_get_version(checked), ==, previous);
	g_assert_true(venture_integration_service_rekey(f->service, new_key, NULL, &error));
	g_assert_no_error(error);
	plain = venture_integration_service_resolve(f->service, f->a, aid, FALSE, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(plain), "secret_key"), ==, "retained-A");
	g_clear_pointer(&plain, json_node_unref);
	plain = venture_integration_service_resolve(f->service, f->b, bid, TRUE, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(plain), "secret_key"), ==, "retained-B");
	g_clear_pointer(&plain, json_node_unref);
	plain = venture_integration_service_resolve_version(f->service, f->a, aid, previous, FALSE, &error);
	g_assert_null(plain);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	g_clear_error(&error);
	g_object_set_data(G_OBJECT(f->db), "venture-integration-service", NULL);
	f->service = venture_integration_service_get(f->db);
	g_assert_true(venture_integration_service_set_key(f->service, old_key, &error));
	g_assert_false(venture_integration_service_verify_key(f->service, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
	g_clear_error(&error);
	plain = venture_integration_service_resolve(f->service, f->a, aid, FALSE, &error);
	g_assert_null(plain);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
	g_clear_error(&error);
	g_object_set_data(G_OBJECT(f->db), "venture-integration-service", NULL);
	f->service = venture_integration_service_get(f->db);
	g_assert_true(venture_integration_service_set_key(f->service, new_key, &error));
	g_assert_true(venture_integration_service_verify_key(f->service, &error));
	g_assert_no_error(error);
	plain = venture_integration_service_resolve(f->service, f->b, bid, TRUE, &error);
	g_assert_no_error(error);
	g_assert_nonnull(plain);
}

/* A bad later envelope must roll back earlier rewrites and retain the process
 * key; the operator must not end up with a database split between two keys. */
static void
test_master_key_rollback(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureIntegrationConnection) a = connect_account(f, f->a, "good-A");
	g_autoptr(VentureIntegrationConnection) b = connect_account(f, f->b, "bad-B");
	g_autoptr(GBytes) new_key = g_bytes_new_static("abcdefghijklmnopqrstuvwxyz123456", 32);
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) plain = NULL;
	g_autoptr(VentureEntity) current = NULL;
	g_autofree gchar *sql = NULL, *before = NULL, *after = NULL;
	gint64 aid = venture_entity_get_id(VENTURE_ENTITY(a));
	(void)data;
	g_object_get(a, "sealed-settings", &before, NULL);
	sql = g_strdup_printf("UPDATE integration_connections SET sealed_settings='broken' WHERE id=%" G_GINT64_FORMAT,
		venture_entity_get_id(VENTURE_ENTITY(b)));
	g_assert_true(venture_database_execute(f->db, sql, NULL, &error));
	g_assert_false(venture_integration_service_verify_key(f->service, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
	g_clear_error(&error);
	g_assert_false(venture_integration_service_rekey(f->service, new_key, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
	g_clear_error(&error);
	current = venture_database_get(f->db, VENTURE_TYPE_INTEGRATION_CONNECTION, aid, &error);
	g_assert_no_error(error);
	g_object_get(current, "sealed-settings", &after, NULL);
	g_assert_cmpstr(before, ==, after);
	g_assert_cmpint(venture_entity_get_version(current), ==, venture_entity_get_version(VENTURE_ENTITY(a)));
	plain = venture_integration_service_resolve(f->service, f->a, aid, FALSE, &error);
	g_assert_no_error(error);
	g_assert_nonnull(plain);
}

static void
test_master_key_scope(Fixture *f, gconstpointer data)
{
	g_autoptr(GBytes) new_key = g_bytes_new_static("abcdefghijklmnopqrstuvwxyz123456", 32);
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureAccessScope) scope = NULL;
	VentureAuthPrincipal principal;
	(void)data;
	principal.user_id = 0; principal.token_id = 0;
	principal.role = VENTURE_USER_ROLE_OWNER;
	principal.name = (gchar *)"owner"; principal.authenticated = TRUE;
	scope = venture_access_policy_enter(venture_database_get_access_policy(f->db), &principal);
	g_assert_false(venture_integration_service_rekey(f->service, new_key, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error); g_clear_object(&scope);
	g_assert_true(venture_database_begin(f->db, &error));
	g_assert_false(venture_integration_service_rekey(f->service, new_key, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);
	g_assert_true(venture_database_commit(f->db, &error));
	g_assert_no_error(error);
}

/* Ciphertext processing is paged, but commit is whole-workspace. Private
 * history must remain recoverable after its owner's membership is revoked. */
static void
test_master_key_pages(Fixture *f, gconstpointer data)
{
	g_autoptr(GBytes) new_key = g_bytes_new_static("abcdefghijklmnopqrstuvwxyz123456", 32);
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) user = g_object_new(VENTURE_TYPE_USER,
		"organization-id", f->a, "username", "retired-key-owner", "active", TRUE, NULL);
	g_autoptr(VentureEntity) member = NULL;
	g_autoptr(VentureIntegrationConnection) last = NULL;
	g_autoptr(JsonNode) values = settings("private-page-tail"), plain = NULL;
	guint i;
	(void)data;
	g_assert_true(venture_database_save(f->db, user, NULL, &error));
	member = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP, "organization-id", f->a,
		"user-id", venture_entity_get_id(user), "role", VENTURE_ORGANIZATION_ROLE_EDITOR,
		"active", TRUE, NULL);
	g_assert_true(venture_database_save(f->db, member, NULL, &error));
	for (i = 0; i < 129; i++)
	{
		g_autofree gchar *provider = g_strdup_printf("private%u", i);
		g_clear_object(&last);
		last = venture_integration_service_configure_for_owner(f->service, f->a, provider,
			"retained", "test", values, 0, venture_entity_get_id(user), NULL, &error);
		g_assert_no_error(error); g_assert_nonnull(last);
	}
	g_object_set(member, "active", FALSE, NULL);
	g_assert_true(venture_database_save(f->db, member, NULL, &error));
	g_object_set(user, "active", FALSE, NULL);
	g_assert_true(venture_database_save(f->db, user, NULL, &error));
	g_assert_true(venture_integration_service_rekey(f->service, new_key, NULL, &error));
	g_assert_no_error(error);
	plain = venture_integration_service_resolve(f->service, f->a,
		venture_entity_get_id(VENTURE_ENTITY(last)), FALSE, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(plain), "secret_key"), ==, "private-page-tail");
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	venture_entity_registry_register_builtins(venture_entity_registry_get_default());
	g_test_add_func("/integrations/registered", test_registered);
	g_test_add("/integrations/active-uniqueness", Fixture, NULL, setup, test_active_uniqueness, teardown);
	g_test_add("/integrations/ambiguous-upgrade", Fixture, NULL, setup, test_ambiguous_upgrade, teardown);
	g_test_add("/integrations/isolation-rotation", Fixture, NULL, setup, test_isolation_rotation, teardown);
	g_test_add("/integrations/disconnect-replacement", Fixture, NULL, setup, test_disconnect_replacement, teardown);
	g_test_add("/integrations/refusals", Fixture, NULL, setup, test_refusals, teardown);
	g_test_add("/integrations/ciphertext-binding", Fixture, NULL, setup, test_ciphertext_binding, teardown);
	g_test_add("/integrations/service-restart", Fixture, NULL, setup, test_service_restart, teardown);
	g_test_add("/integrations/organization-admin", Fixture, NULL, setup, test_organization_admin, teardown);
	g_test_add("/integrations/bounds-audit-lifecycle", Fixture, NULL, setup, test_bounds_audit_lifecycle, teardown);
	g_test_add("/integrations/master-key", Fixture, NULL, setup, test_master_key, teardown);
	g_test_add("/integrations/master-key-rollback", Fixture, NULL, setup, test_master_key_rollback, teardown);
	g_test_add("/integrations/master-key-pages", Fixture, NULL, setup, test_master_key_pages, teardown);
	g_test_add("/integrations/master-key-scope", Fixture, NULL, setup, test_master_key_scope, teardown);
	g_test_add("/integrations/environment-key", Fixture, NULL, setup, test_environment_key, teardown);
	return g_test_run();
}
