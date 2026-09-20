/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <string.h>
#include "venture-test-accounting.h"

typedef struct {
	VentureDatabase *database;
	VentureConfig *config;
	gint64 organization;
} Fixture;
static void setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) key = g_bytes_new_static("01234567890123456789012345678901", 32);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ORGANIZATION);
	g_autoptr(VentureEntity) organization = NULL;
	(void)data;
	f->config = venture_config_new();
	g_object_set(f->config, "imap-allowed-endpoints", "imap.example.test:993",
		"calendar-allowed-origins", "https://dav.example.test", NULL);
	f->database = venture_test_accounting_database(&error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->database, venture_entity_registry_get_default(), &error));
	organization = venture_database_find_one(f->database, query, &error);
	g_assert_no_error(error);
	f->organization = venture_entity_get_id(organization);
	g_assert_true(venture_integration_service_set_key(venture_integration_service_get(f->database), key, &error));
	g_assert_no_error(error);
}
static void teardown(Fixture *f, gconstpointer data)
{
	(void)data;
	venture_test_accounting_database_cleanup(f->database);
	g_clear_object(&f->database);
	g_clear_object(&f->config);
}
static VentureEntity *account(Fixture *f, gboolean calendar)
{
	VentureEntity *record;
	g_autoptr(GError) error = NULL;
	if (calendar)
		record = g_object_new(VENTURE_TYPE_CALENDAR_ACCOUNT, "organization-id", f->organization,
			"url", "https://dav.example.test", "calendar-path", "/calendars/business/", "owner", "owner", "username", "business", NULL);
	else
		record = g_object_new(VENTURE_TYPE_MAIL_ACCOUNT, "organization-id", f->organization,
			"address", "business@example.test", "imap-host", "imap.example.test", "imap-port", (gint64)993,
			"imap-tls", "tls", "username", "business", "folders", "INBOX", NULL);
	g_assert_true(venture_database_save(f->database, record, NULL, &error));
	g_assert_no_error(error);
	return record;
}
/* Both consumers must re-resolve explicit credentials; a live snapshot may
 * not keep importing after rotation or disconnect. */
static void test_lifecycle(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) record = account(f, data != NULL);
	g_autoptr(VentureIntegrationConnection) binding = NULL, rotated = NULL;
	g_autoptr(VentureConnectorSession) session = NULL;
	g_autoptr(JsonObject) values = json_object_new();
	g_autoptr(GError) error = NULL;
	session = venture_connector_open(f->database, f->config, record, &error);
	g_assert_null(session);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
	g_clear_error(&error);
	json_object_set_string_member(values, "password", "synthetic-first-password");
	binding = venture_connector_configure(f->database, f->config, record, values, 0, 0, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(binding);
	session = venture_connector_open(f->database, f->config, record, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(venture_connector_session_get_password(session), ==, "synthetic-first-password");
	/* Disabling credentials stops consumers but keeps account history visible.
	 * Re-enabling the module restores access without reconfiguration. */
	venture_entity_registry_set_type_module(venture_entity_registry_get_default(), "integration_connection", "integrations", FALSE);
	g_assert_false(venture_connector_session_validate(session, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED); g_clear_error(&error);
	{
		g_autoptr(VentureEntity) history = venture_database_get(f->database, G_OBJECT_TYPE(record), venture_entity_get_id(record), &error);
		g_assert_no_error(error); g_assert_nonnull(history);
	}
	venture_entity_registry_set_type_module(venture_entity_registry_get_default(), "integration_connection", "integrations", TRUE);
	g_assert_true(venture_connector_session_validate(session, &error));
	g_assert_no_error(error);

	json_object_set_string_member(values, "password", "synthetic-rotated-password");
	rotated = venture_connector_configure(f->database, f->config, record, values,
		venture_entity_get_id(VENTURE_ENTITY(binding)), venture_entity_get_version(VENTURE_ENTITY(binding)), NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(rotated);
	g_assert_false(venture_connector_session_validate(session, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_clear_object(&session);
	session = venture_connector_open(f->database, f->config, record, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(venture_connector_session_get_password(session), ==, "synthetic-rotated-password");
	g_assert_true(venture_connector_disconnect(f->database, record,
		venture_entity_get_id(VENTURE_ENTITY(rotated)), venture_entity_get_version(VENTURE_ENTITY(rotated)), NULL, &error));
	g_assert_no_error(error);
	g_assert_false(venture_connector_session_validate(session, &error));
	g_assert_nonnull(error);
}
static void test_selection_and_identity(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) record = account(f, FALSE);
	g_autoptr(VentureIntegrationConnection) binding = NULL;
	g_autoptr(VentureConnectorSession) session = NULL;
	g_autoptr(JsonObject) settings = json_object_new();
	g_autoptr(GError) error = NULL;
	(void)data;
	json_object_set_string_member(settings, "password", "synthetic-password");
	binding = venture_connector_configure(f->database, f->config, record, settings, 0, 0, NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(binding);
	session = venture_connector_open(f->database, f->config, record, &error);
	g_assert_no_error(error); g_assert_nonnull(session);
	g_object_set(record, "folders", "Selected", NULL);
	g_assert_true(venture_database_save(f->database, record, NULL, &error));
	g_assert_false(venture_connector_session_validate(session, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT); g_clear_error(&error);
	g_clear_object(&session);
	session = venture_connector_open(f->database, f->config, record, &error);
	g_assert_no_error(error); g_assert_nonnull(session);
	g_assert_true(venture_connector_disconnect(f->database, record, venture_entity_get_id(VENTURE_ENTITY(binding)),
		venture_entity_get_version(VENTURE_ENTITY(binding)), NULL, &error));
	g_object_set(record, "username", "different-account", NULL);
	g_assert_false(venture_database_save(f->database, record, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

/* Organization administration does not publish a colleague's mailbox,
 * attachment or audit excerpt; reparenting must not remove that boundary. */
static void test_personal_boundary(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonObject) settings = json_object_new();
	g_autoptr(JsonNode) secret = NULL;
	g_autoptr(VentureIntegrationConnection) binding = NULL;
	g_autoptr(VentureConnectorSession) session = NULL;
	g_autoptr(VentureEntity) alice = g_object_new(VENTURE_TYPE_USER, "organization-id", f->organization,
		"username", "private-alice", "role", VENTURE_USER_ROLE_EDITOR, "active", TRUE, NULL);
	g_autoptr(VentureEntity) bob = g_object_new(VENTURE_TYPE_USER, "organization-id", f->organization,
		"username", "private-bob", "role", VENTURE_USER_ROLE_EDITOR, "active", TRUE, NULL);
	g_autoptr(VentureEntity) alice_member = NULL, bob_member = NULL, mailbox = NULL, inbound = NULL, document = NULL, found = NULL;
	g_autoptr(VentureAccessScope) scope = NULL;
	g_autoptr(VentureQuery) audit = venture_query_new(VENTURE_TYPE_AUDIT_ENTRY);
	g_autoptr(GPtrArray) evidence = NULL;
	VentureAccessPolicy *policy = venture_database_get_access_policy(f->database);
	VentureAuthPrincipal principal;
	gchar alice_name[] = "private-alice", bob_name[] = "private-bob";
	(void)data;
	g_assert_true(venture_database_save(f->database, alice, NULL, &error));
	g_assert_true(venture_database_save(f->database, bob, NULL, &error));
	alice_member = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP, "organization-id", f->organization,
		"user-id", venture_entity_get_id(alice), "role", VENTURE_ORGANIZATION_ROLE_EDITOR, "active", TRUE, NULL);
	bob_member = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP, "organization-id", f->organization,
		"user-id", venture_entity_get_id(bob), "role", VENTURE_ORGANIZATION_ROLE_OWNER, "active", TRUE, NULL);
	g_assert_true(venture_database_save(f->database, alice_member, NULL, &error));
	g_assert_true(venture_database_save(f->database, bob_member, NULL, &error));
	mailbox = g_object_new(VENTURE_TYPE_MAIL_ACCOUNT, "organization-id", f->organization,
		"address", "private@example.test", "imap-host", "imap.example.test", "imap-port", (gint64)993,
		"imap-tls", "tls", "username", "private", "private-owner-id", venture_entity_get_id(alice), NULL);
	g_assert_true(venture_database_save(f->database, mailbox, NULL, &error));
	document = g_object_new(VENTURE_TYPE_DOCUMENT, "organization-id", f->organization,
		"title", "PRIVATE_BODY_MARKER", "private-owner-id", venture_entity_get_id(alice), NULL);
	g_assert_true(venture_database_save(f->database, document, NULL, &error));
	inbound = g_object_new(VENTURE_TYPE_MAIL_INBOUND, "organization-id", f->organization,
		"account-id", venture_entity_get_id(mailbox), "subject", "PRIVATE_MAIL_MARKER", "document-id", venture_entity_get_id(document), NULL);
	g_assert_true(venture_database_save(f->database, inbound, NULL, &error));
	g_assert_no_error(error);
	json_object_set_string_member(settings, "password", "PRIVATE_CREDENTIAL_MARKER");
	binding = venture_connector_configure(f->database, f->config, mailbox, settings, 0, 0, NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(binding);
	session = venture_connector_open(f->database, f->config, mailbox, &error);
	g_assert_no_error(error); g_assert_nonnull(session);
	principal.user_id = venture_entity_get_id(bob); principal.token_id = 0;
	principal.role = VENTURE_USER_ROLE_EDITOR; principal.name = bob_name; principal.authenticated = TRUE;
	scope = venture_access_policy_enter(policy, &principal);
	secret = venture_integration_service_resolve(venture_integration_service_get(f->database), f->organization,
		venture_entity_get_id(VENTURE_ENTITY(binding)), FALSE, &error);
	g_assert_null(secret); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND); g_clear_error(&error);
	found = venture_database_get(f->database, VENTURE_TYPE_MAIL_ACCOUNT, venture_entity_get_id(mailbox), &error);
	g_assert_null(found); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND); g_clear_error(&error);
	found = venture_database_get(f->database, VENTURE_TYPE_MAIL_INBOUND, venture_entity_get_id(inbound), &error);
	g_assert_null(found); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND); g_clear_error(&error);
	found = venture_database_get(f->database, VENTURE_TYPE_DOCUMENT, venture_entity_get_id(document), &error);
	g_assert_null(found); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND); g_clear_error(&error);
	venture_query_set_organization(audit, f->organization);
	venture_query_add_filter_string(audit, "target-type", VENTURE_FILTER_OP_EQ, "document", &error);
	venture_query_add_filter_int(audit, "target-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(document), &error);
	evidence = venture_database_find(f->database, audit, &error);
	g_assert_no_error(error); g_assert_cmpuint(evidence->len, ==, 0);
	g_clear_object(&scope);
	principal.user_id = venture_entity_get_id(alice); principal.name = alice_name;
	scope = venture_access_policy_enter(policy, &principal);
	found = venture_database_get(f->database, VENTURE_TYPE_MAIL_INBOUND, venture_entity_get_id(inbound), &error);
	g_assert_no_error(error); g_assert_nonnull(found);
	g_object_set(found, "account-id", (gint64)0, NULL);
	g_assert_false(venture_database_save(f->database, found, NULL, &error));
	g_assert_nonnull(error); g_clear_error(&error);
	g_clear_object(&scope);
	g_object_set(alice_member, "active", FALSE, NULL);
	g_assert_true(venture_database_save(f->database, alice_member, NULL, &error));
	g_assert_false(venture_connector_session_validate(session, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED); g_clear_error(&error);
	scope = venture_access_policy_enter(policy, &principal);
	g_clear_object(&found);
	found = venture_database_get(f->database, VENTURE_TYPE_MAIL_ACCOUNT, venture_entity_get_id(mailbox), &error);
	g_assert_null(found); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
}

/* A populated pre-privacy installation must keep its public account identity
 * and existing ciphertext, without guessing a private owner from a login. */
static void test_populated_upgrade(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) mailbox = account(f, FALSE), restored = NULL;
	g_autoptr(VentureIntegrationConnection) binding = NULL;
	g_autoptr(JsonObject) settings = json_object_new();
	g_autoptr(VentureConnectorSession) session = NULL;
	g_autoptr(GError) error = NULL;
	gint64 owner = -1;
	(void)data;
	json_object_set_string_member(settings, "password", "old-shared-ciphertext");
	binding = venture_connector_configure(f->database, f->config, mailbox, settings, 0, 0, NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(binding);
	g_assert_true(venture_database_execute(f->database,
		"DROP INDEX IF EXISTS idx_mail_accounts_private_owner_id; "
		"DROP INDEX IF EXISTS idx_integration_connections_private_owner_id; "
		"ALTER TABLE mail_accounts DROP COLUMN private_owner_id; "
		"ALTER TABLE integration_connections DROP COLUMN private_owner_id; "
		"DELETE FROM schema_migrations WHERE version >= 570", NULL, &error));
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->database, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	restored = venture_database_get(f->database, VENTURE_TYPE_MAIL_ACCOUNT, venture_entity_get_id(mailbox), &error);
	g_assert_no_error(error); g_assert_nonnull(restored);
	g_assert_cmpstr(venture_entity_get_uuid(restored), ==, venture_entity_get_uuid(mailbox));
	g_object_get(restored, "private-owner-id", &owner, NULL);
	g_assert_cmpint(owner, ==, 0);
	session = venture_connector_open(f->database, f->config, restored, &error);
	g_assert_no_error(error); g_assert_nonnull(session);
	g_assert_cmpstr(venture_connector_session_get_password(session), ==, "old-shared-ciphertext");
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/connectors/imap-lifecycle", Fixture, NULL, setup, test_lifecycle, teardown);
	g_test_add("/connectors/caldav-lifecycle", Fixture, "caldav", setup, test_lifecycle, teardown);
	g_test_add("/connectors/selection-and-identity", Fixture, NULL, setup, test_selection_and_identity, teardown);
	g_test_add("/connectors/personal-boundary", Fixture, NULL, setup, test_personal_boundary, teardown);
	g_test_add("/connectors/populated-upgrade", Fixture, NULL, setup, test_populated_upgrade, teardown);
	return g_test_run();
}
