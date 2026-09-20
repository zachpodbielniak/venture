/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <libsoup/soup.h>
#include <unistd.h>
#include "venture-test-accounting.h"
#include "venture-test-util.h"

/* Hosted administration must use the same declared model and audit surfaces
 * rather than a second ad-hoc identity database. */
static void
test_tenant_records(void)
{
	static const gchar *const names[] = {
		"tenant_workspace", "tenant_membership", "tenant_invitation",
		"tenant_support_grant", "tenant_event"
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(names); i++)
		g_assert_cmpuint(venture_entity_registry_lookup(
			venture_entity_registry_get_default(), names[i]), !=, G_TYPE_INVALID);
}

static void
test_tenant_identity(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDatabase) db = venture_test_accounting_database(&error);
	g_autoptr(VentureConfig) config = venture_config_new();
	VentureTenantService *service;
	gboolean result;
	g_assert_no_error(error);
	result = venture_database_migrate(db, venture_entity_registry_get_default(), &error);
	g_assert_no_error(error); g_assert_true(result);
	service = venture_tenant_service_get(db);
	g_object_set(config, "hosted-enabled", TRUE,
		"hosted-workspace-id", "0c505b10-4160-4e35-bda6-dd073544caed",
		"hosted-origin", "https://studio.example.test", NULL);
	g_assert_true(venture_tenant_service_configure(service, config, &error));
	g_assert_no_error(error);
	/* No provider can run between configuration and verified database binding. */
	g_assert_false(venture_tenant_service_check_operation(service, TRUE, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED); g_clear_error(&error);
	g_assert_true(venture_tenant_service_initialize(service, &error)); g_assert_no_error(error);
	g_assert_true(venture_tenant_service_check_operation(service, TRUE, &error)); g_assert_no_error(error);
	g_assert_cmpstr(venture_tenant_service_get_workspace_id(service), ==,
		"0c505b10-4160-4e35-bda6-dd073544caed");
	g_object_set(config, "hosted-workspace-id", "0c505b10-4160-4e35-bda6-dd073544caee", NULL);
	g_assert_false(venture_tenant_service_configure(service, config, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED); g_clear_error(&error);
	{
		g_autoptr(VentureQuery) users_query = venture_query_new(VENTURE_TYPE_USER);
		g_autoptr(GPtrArray) users = NULL;
		gint role = 0;
		g_assert_true(venture_tenant_service_bootstrap_admin(service, config, "workspace-admin",
			"test-administrator-password", FALSE, "Initial fixture bootstrap", &error));
		g_assert_no_error(error);
		users = venture_database_find(db, users_query, &error); g_assert_no_error(error);
		g_assert_cmpuint(users->len, ==, 1);
		g_object_get(g_ptr_array_index(users, 0), "role", &role, NULL);
		g_assert_cmpint(role, ==, VENTURE_USER_ROLE_EDITOR);
		g_assert_false(venture_tenant_service_bootstrap_admin(service, config, "workspace-admin",
			"test-administrator-password", FALSE, "Bootstrap retry", &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED); g_clear_error(&error);
		g_assert_true(venture_tenant_service_bootstrap_admin(service, config, "workspace-admin",
			"test-recovery-password", TRUE, "Explicit fixture recovery", &error));
		g_assert_no_error(error);
		{
			VentureAuthPrincipal actor;
			g_autoptr(VentureAccessScope) scope = NULL;
			g_autofree gchar *actor_name = g_strdup("workspace-admin");
			actor.user_id = venture_entity_get_id(g_ptr_array_index(users, 0));
			actor.token_id = 0; actor.role = VENTURE_USER_ROLE_EDITOR;
			actor.name = actor_name; actor.authenticated = TRUE;
			scope = venture_access_policy_enter(venture_database_get_access_policy(db), &actor);
			g_assert_false(venture_tenant_service_set_membership(service, actor.user_id, "member", FALSE, "Last administrator", &error));
			g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED); g_clear_error(&error);
			g_assert_true(venture_tenant_service_is_member(service, actor.user_id, TRUE));
			g_assert_true(venture_tenant_service_set_state(service, "read_only", "Fixture maintenance", &error)); g_assert_no_error(error);
			g_assert_true(venture_tenant_service_check_operation(service, FALSE, &error)); g_assert_no_error(error);
			g_assert_false(venture_tenant_service_check_operation(service, TRUE, &error));
			g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED); g_clear_error(&error);
			g_assert_true(venture_tenant_service_set_state(service, "suspended", "Fixture suspension", &error)); g_assert_no_error(error);
			g_assert_false(venture_tenant_service_check_operation(service, FALSE, &error));
			g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED); g_clear_error(&error);
			g_assert_true(venture_tenant_service_set_state(service, "active", "Fixture reactivation", &error)); g_assert_no_error(error);
		}
	}
	/* Operator-level tampering cannot turn an unknown lifecycle state into active. */
	g_assert_true(venture_database_execute(db, "UPDATE tenant_workspaces SET state = 'unknown'", NULL, &error));
	g_assert_no_error(error);
	g_assert_false(venture_tenant_service_check_operation(service, FALSE, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED); g_clear_error(&error);
	venture_test_accounting_database_cleanup(db);
}

static void
test_tenant_classification(void)
{
	g_autoptr(GObject) resource = g_object_new(G_TYPE_OBJECT, NULL);
	g_assert_cmpint(venture_data_class_for_type(VENTURE_TYPE_INVOICE), ==, VENTURE_DATA_CLASS_TENANT);
	g_assert_cmpint(venture_data_class_for_type(VENTURE_TYPE_ENTITY), ==, VENTURE_DATA_CLASS_UNKNOWN);
	g_assert_cmpint(venture_data_class_for_resource(resource), ==, VENTURE_DATA_CLASS_UNKNOWN);
	venture_data_class_declare_resource(resource, VENTURE_DATA_CLASS_PLATFORM);
	g_assert_cmpint(venture_data_class_for_resource(resource), ==, VENTURE_DATA_CLASS_PLATFORM);
}

typedef struct {
	VentureDatabase *database;
	VentureConfig *config;
	VentureTenantService *service;
	VentureAuthPrincipal administrator;
	gint64 organization_id;
} TenantFixture;

static void
tenant_setup(TenantFixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) organization = g_object_new(VENTURE_TYPE_ORGANIZATION, "name", "Hosted business", NULL);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_USER);
	g_autoptr(GPtrArray) users = NULL;
	gboolean ok;
	(void)data;
	/* A preceding HTTP context applies optional-module masks process-wide.
	 * The restored-identity fixture explicitly retains this optional table. */
	venture_entity_registry_set_type_module(venture_entity_registry_get_default(), "oidc_identity", "oidc", TRUE);
	f->database = venture_test_accounting_database(&error); g_assert_no_error(error);
	ok = venture_database_migrate(f->database, venture_entity_registry_get_default(), &error);
	g_assert_no_error(error); g_assert_true(ok);
	f->config = venture_config_new();
	g_object_set(f->config, "hosted-enabled", TRUE, "hosted-workspace-id", "8f062b79-1d2b-4d7f-99e5-bd3bf588e05a",
		"hosted-origin", "https://tenant.example.test", "security-password-iterations", (gint64)2000,
		"security-mfa-key-env", "VENTURE_TEST_TENANT_MFA_KEY", NULL);
	f->service = venture_tenant_service_get(f->database);
	g_assert_true(venture_tenant_service_configure(f->service, f->config, &error)); g_assert_no_error(error);
	g_assert_true(venture_tenant_service_initialize(f->service, &error)); g_assert_no_error(error);
	g_assert_true(venture_database_save(f->database, organization, NULL, &error)); g_assert_no_error(error);
	f->organization_id = venture_entity_get_id(organization);
	g_assert_true(venture_tenant_service_bootstrap_admin(f->service, f->config, "administrator",
		"private-initial-password", FALSE, "Fixture bootstrap", &error)); g_assert_no_error(error);
	users = venture_database_find(f->database, query, &error); g_assert_no_error(error);
	g_assert_cmpuint(users->len, ==, 1);
	f->administrator.user_id = venture_entity_get_id(g_ptr_array_index(users, 0));
	f->administrator.token_id = 0; f->administrator.role = VENTURE_USER_ROLE_EDITOR;
	f->administrator.name = g_strdup("administrator"); f->administrator.authenticated = TRUE;
}

static void
tenant_teardown(TenantFixture *f, gconstpointer data)
{
	(void)data;
	g_free(f->administrator.name);
	g_clear_object(&f->config);
	venture_test_accounting_database_cleanup(f->database); g_clear_object(&f->database);
}

static VentureTenantInvitation *
tenant_invitation(TenantFixture *f, const gchar *role)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureAccessScope) scope = venture_access_policy_enter(
		venture_database_get_access_policy(f->database), &f->administrator);
	VentureTenantInvitation *invitation = venture_tenant_service_invite(f->service, role,
		f->organization_id, VENTURE_ORGANIZATION_ROLE_EDITOR, 3600, "Invite fixture colleague", &error);
	g_assert_no_error(error); g_assert_nonnull(invitation);
	return invitation;
}

/* A capability binds to an explicit new account or the accepting session,
 * never a guessed email/username, and only one atomic acceptance may win. */
static void
test_tenant_invitations(TenantFixture *f, gconstpointer data)
{
	g_autoptr(VentureTenantInvitation) invitation = tenant_invitation(f, "member");
	g_autoptr(VentureTenantInvitation) promote = NULL, refused = NULL;
	g_autoptr(VentureUser) user = NULL, replay = NULL, promoted = NULL;
	g_autofree gchar *capability = NULL, *promotion = NULL, *refused_capability = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) stored = NULL, token = NULL;
	VentureAuthPrincipal accepting;
	gint role;
	gboolean active;
	(void)data;
	g_object_get(invitation, "capability", &capability, NULL); g_assert_nonnull(capability);
	stored = venture_database_get(f->database, VENTURE_TYPE_TENANT_INVITATION,
		venture_entity_get_id(VENTURE_ENTITY(invitation)), &error); g_assert_no_error(error);
	{ g_autofree gchar *persisted = NULL; g_object_get(stored, "capability", &persisted, NULL); g_assert_true(venture_string_is_empty(persisted)); }
	user = venture_tenant_service_accept_invitation(f->service, f->config, capability, "colleague", "private-colleague-password", &error);
	g_assert_no_error(error); g_assert_nonnull(user);
	g_object_get(user, "role", &role, NULL); g_assert_cmpint(role, ==, VENTURE_USER_ROLE_EDITOR);
	g_assert_true(venture_tenant_service_is_member(f->service, venture_entity_get_id(VENTURE_ENTITY(user)), FALSE));
	replay = venture_tenant_service_accept_invitation(f->service, f->config, capability, "replay-user", "private-replay-password", &error);
	g_assert_null(replay); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED); g_clear_error(&error);
	refused = tenant_invitation(f, "member"); g_object_get(refused, "capability", &refused_capability, NULL);
	replay = venture_tenant_service_accept_invitation(f->service, f->config, refused_capability, "colleague", "private-colleague-password", &error);
	g_assert_null(replay); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED); g_clear_error(&error);
	promote = tenant_invitation(f, "admin"); g_object_get(promote, "capability", &promotion, NULL);
	accepting.user_id = venture_entity_get_id(VENTURE_ENTITY(user)); accepting.token_id = 0;
	accepting.role = VENTURE_USER_ROLE_EDITOR; accepting.name = f->administrator.name; accepting.authenticated = TRUE;
	{
		g_autoptr(VentureAccessScope) scope = venture_access_policy_enter(venture_database_get_access_policy(f->database), &accepting);
		promoted = venture_tenant_service_accept_invitation(f->service, f->config, promotion, NULL, NULL, &error);
		g_assert_no_error(error); g_assert_nonnull(promoted);
		g_assert_cmpint(venture_entity_get_id(VENTURE_ENTITY(promoted)), ==, accepting.user_id);
	}
	g_assert_true(venture_tenant_service_is_member(f->service, accepting.user_id, TRUE));
	token = g_object_new(VENTURE_TYPE_API_TOKEN, "name", "Revocation fixture", "user-id", accepting.user_id,
		"role", VENTURE_USER_ROLE_EDITOR, "active", TRUE, NULL);
	g_assert_true(venture_database_save(f->database, token, NULL, &error)); g_assert_no_error(error);
	{
		g_autoptr(VentureAccessScope) scope = venture_access_policy_enter(venture_database_get_access_policy(f->database), &f->administrator);
		g_assert_true(venture_tenant_service_set_membership(f->service, accepting.user_id, "member", FALSE, "Immediate revocation", &error));
		g_assert_no_error(error);
		g_assert_false(venture_tenant_service_check_principal(f->service, &accepting, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED); g_clear_error(&error);
		g_assert_true(venture_tenant_service_set_membership(f->service, accepting.user_id, "member", TRUE, "Explicit reactivation", &error));
		g_assert_no_error(error);
	}
	g_clear_object(&stored);
	stored = venture_database_get(f->database, VENTURE_TYPE_API_TOKEN, venture_entity_get_id(token), &error); g_assert_no_error(error);
	g_object_get(stored, "active", &active, NULL); g_assert_false(active);
}
/* Support is an explicit per-request capability, never an OWNER bypass. A
 * nested dispatch must lose it, and suspension does not promote ordinary
 * support into emergency access. */
static void
test_tenant_support(TenantFixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) operator_user = g_object_new(VENTURE_TYPE_USER,
		"username", "support-operator", "role", VENTURE_USER_ROLE_VIEWER, "active", TRUE, NULL);
	g_autoptr(VentureEntity) invoice = g_object_new(VENTURE_TYPE_INVOICE,
		"organization-id", f->organization_id, NULL);
	g_autoptr(VentureEntity) other = g_object_new(VENTURE_TYPE_INVOICE,
		"organization-id", f->organization_id + 1, NULL);
	g_autoptr(VentureTenantSupportGrant) grant = NULL, emergency = NULL;
	g_autofree gchar *capability = NULL, *emergency_capability = NULL;
	VentureAuthPrincipal actor;
	(void)data;
	{
		g_autoptr(VentureTenantMaintenance) maintenance = venture_tenant_service_enter_maintenance(f->service, "Provision named support identity", &error);
		g_assert_no_error(error); g_assert_nonnull(maintenance);
		g_assert_true(venture_database_save(f->database, operator_user, NULL, &error)); g_assert_no_error(error);
		g_assert_true(venture_tenant_maintenance_finish(maintenance, &error)); g_assert_no_error(error);
	}
	actor.user_id = venture_entity_get_id(operator_user); actor.token_id = 0;
	actor.role = VENTURE_USER_ROLE_VIEWER; actor.name = f->administrator.name; actor.authenticated = TRUE;
	grant = venture_tenant_service_create_support_grant(f->service, actor.user_id,
		f->organization_id, FALSE, FALSE, FALSE, 3600, "Investigate one organization", &error);
	g_assert_no_error(error); g_assert_nonnull(grant); g_object_get(grant, "capability", &capability, NULL);
	{
		g_autoptr(VentureAccessScope) access = venture_access_policy_enter(venture_database_get_access_policy(f->database), &actor);
		g_autoptr(VentureTenantSupportScope) support = venture_tenant_service_enter_request(f->service, &actor, capability, "/e/invoice", &error);
		g_assert_no_error(error); g_assert_nonnull(support);
		g_assert_true(venture_tenant_service_support_allows(f->service, &actor, invoice, FALSE, FALSE, &error)); g_assert_no_error(error);
		g_assert_false(venture_tenant_service_support_allows(f->service, &actor, other, FALSE, FALSE, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED); g_clear_error(&error);
		g_assert_false(venture_tenant_service_support_allows(f->service, &actor, invoice, TRUE, FALSE, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED); g_clear_error(&error);
		{
			g_autoptr(VentureEntity) private_thread = g_object_new(VENTURE_TYPE_CHAT_THREAD,
				"organization-id", f->organization_id, "user-id", f->administrator.user_id, NULL);
			g_assert_false(venture_tenant_service_support_allows(f->service, &actor, private_thread, FALSE, FALSE, &error));
			g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED); g_clear_error(&error);
		}
		g_assert_false(venture_tenant_service_check_provider(f->service, "smtp", &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED); g_clear_error(&error);
		{
			g_autoptr(VentureTenantSupportScope) nested = venture_tenant_service_enter_request(f->service, &actor, NULL, "/e/invoice", &error);
			g_assert_no_error(error); g_assert_nonnull(nested);
			g_assert_false(venture_tenant_service_check_principal(f->service, &actor, &error));
			g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED); g_clear_error(&error);
		}
		g_assert_true(venture_tenant_service_check_principal(f->service, &actor, &error)); g_assert_no_error(error);
	}
	/* Expiry is reread durably on every request, independent of a cookie's
	 * lifetime or an earlier successful grant admission. */
	{
		g_autoptr(VentureTenantMaintenance) maintenance = venture_tenant_service_enter_maintenance(f->service, "Expire support fixture", &error);
		g_autoptr(GDateTime) expired = g_date_time_new_from_unix_utc(1);
		g_assert_no_error(error); g_assert_nonnull(maintenance);
		g_object_set(grant, "expires-at", expired, NULL);
		g_assert_true(venture_database_save(f->database, VENTURE_ENTITY(grant), NULL, &error)); g_assert_no_error(error);
		g_assert_true(venture_tenant_maintenance_finish(maintenance, &error)); g_assert_no_error(error);
	}
	{
		g_autoptr(VentureAccessScope) access = venture_access_policy_enter(venture_database_get_access_policy(f->database), &actor);
		g_autoptr(VentureTenantSupportScope) support = venture_tenant_service_enter_request(f->service, &actor, capability, "/e/invoice", &error);
		g_assert_null(support); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED); g_clear_error(&error);
	}
	g_clear_object(&grant); g_clear_pointer(&capability, g_free);
	grant = venture_tenant_service_create_support_grant(f->service, actor.user_id,
		f->organization_id, FALSE, FALSE, FALSE, 3600, "Fresh normal support", &error);
	g_assert_no_error(error); g_assert_nonnull(grant); g_object_get(grant, "capability", &capability, NULL);
	g_assert_true(venture_tenant_service_set_state_operator(f->service, "suspended", "Bounded support fixture", &error)); g_assert_no_error(error);
	{
		g_autoptr(VentureAccessScope) access = venture_access_policy_enter(venture_database_get_access_policy(f->database), &actor);
		g_autoptr(VentureTenantSupportScope) support = venture_tenant_service_enter_request(f->service, &actor, capability, "/e/invoice", &error);
		g_assert_no_error(error); g_assert_nonnull(support);
		g_assert_false(venture_tenant_service_support_allows(f->service, &actor, invoice, FALSE, FALSE, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED); g_clear_error(&error);
	}
	emergency = venture_tenant_service_create_support_grant(f->service, actor.user_id,
		f->organization_id, TRUE, FALSE, TRUE, 300, "Explicit emergency diagnosis", &error);
	g_assert_no_error(error); g_assert_nonnull(emergency); g_object_get(emergency, "capability", &emergency_capability, NULL);
	{
		g_autoptr(VentureAccessScope) access = venture_access_policy_enter(venture_database_get_access_policy(f->database), &actor);
		g_autoptr(VentureTenantSupportScope) support = venture_tenant_service_enter_request(f->service, &actor, emergency_capability, "/e/invoice", &error);
		g_assert_no_error(error); g_assert_nonnull(support);
		g_assert_true(venture_tenant_service_support_allows(f->service, &actor, invoice, TRUE, TRUE, &error)); g_assert_no_error(error);
	}
	{
		g_autoptr(VentureAccessScope) access = venture_access_policy_enter(venture_database_get_access_policy(f->database), &f->administrator);
		g_assert_true(venture_tenant_service_revoke_support(f->service, venture_entity_get_id(VENTURE_ENTITY(emergency)), "Revoke during suspension", &error)); g_assert_no_error(error);
	}
	{
		g_autoptr(VentureAccessScope) access = venture_access_policy_enter(venture_database_get_access_policy(f->database), &actor);
		g_autoptr(VentureTenantSupportScope) support = venture_tenant_service_enter_request(f->service, &actor, emergency_capability, "/e/invoice", &error);
		g_assert_null(support); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED); g_clear_error(&error);
	}
}
/* Declared administrative actions may invoke the checked service, but they
 * must not turn protected control records into generic writable rows. */
static void
test_tenant_actions(TenantFixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_TENANT_WORKSPACE);
	g_autoptr(GPtrArray) rows = venture_database_find(f->database, query, &error);
	g_autoptr(VentureEntity) result = NULL;
	g_autoptr(JsonNode) json = NULL;
	g_autoptr(GHashTable) parameters = NULL;
	g_autoptr(VentureAccessScope) scope = NULL;
	VentureActionRegistry *registry = venture_database_get_action_registry(f->database);
	VentureEntity *workspace;
	(void)data;
	g_assert_no_error(error); g_assert_cmpuint(rows->len, ==, 1);
	workspace = g_ptr_array_index(rows, 0);
	scope = venture_access_policy_enter(venture_database_get_access_policy(f->database), &f->administrator);
	g_assert_false(venture_access_policy_can(venture_database_get_access_policy(f->database),
		&f->administrator, "undeclared", workspace, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED); g_clear_error(&error);
	g_object_set(workspace, "state", VENTURE_TENANT_STATE_SUSPENDED, NULL);
	g_assert_false(venture_database_save(f->database, workspace, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED); g_clear_error(&error);
	json = venture_json_parse("{\"state\":\"suspended\",\"reason\":\"Declared action fixture\"}", &error); g_assert_no_error(error);
	parameters = venture_action_parameters_from_json(json, &error); g_assert_no_error(error);
	result = venture_action_registry_perform(registry, "tenant_workspace", venture_entity_get_id(workspace),
		"set_state", parameters, NULL, VENTURE_USER_ROLE_EDITOR, &error);
	g_assert_no_error(error); g_assert_nonnull(result);
	g_assert_false(venture_tenant_service_check_operation(f->service, FALSE, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED); g_clear_error(&error);
	json_node_set_string(g_hash_table_lookup(parameters, "state"), "active");
	g_clear_object(&result);
	result = venture_action_registry_perform(registry, "tenant_workspace", venture_entity_get_id(workspace),
		"set_state", parameters, NULL, VENTURE_USER_ROLE_EDITOR, &error);
	g_assert_no_error(error); g_assert_nonnull(result);
	g_assert_true(venture_tenant_service_check_operation(f->service, TRUE, &error)); g_assert_no_error(error);
}

static void
test_tenant_host_authority(TenantFixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) forge = g_object_new(VENTURE_TYPE_FORGE, "name", "Untrusted host machinery", NULL);
	g_autoptr(VentureEntity) base = g_object_new(VENTURE_TYPE_KNOWLEDGE_BASE, "name", "Host file claim",
		"organization-id", f->organization_id, "source-path", "/etc", NULL);
	g_autoptr(VentureEntity) user = NULL;
	g_autoptr(VentureAccessScope) scope = NULL;
	(void)data;
	{
		g_autoptr(VentureTenantMaintenance) maintenance = venture_tenant_service_enter_maintenance(f->service, "Explicit fixture maintenance", &error);
		g_assert_no_error(error); g_assert_nonnull(maintenance);
		g_assert_true(venture_tenant_service_check_write(f->service, forge, &error)); g_assert_no_error(error);
		{
			g_autoptr(VentureTenantSupportScope) nested = venture_tenant_service_enter_request(f->service, NULL, NULL, "/e/forge", &error);
			g_assert_no_error(error); g_assert_nonnull(nested);
			g_assert_false(venture_tenant_service_check_write(f->service, forge, &error));
			g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED); g_clear_error(&error);
		}
		g_assert_true(venture_tenant_service_check_write(f->service, forge, &error)); g_assert_no_error(error);
		g_assert_true(venture_tenant_maintenance_finish(maintenance, &error)); g_assert_no_error(error);
	}
	/* Even actorless jobs must not become implicit operator maintenance. */
	g_assert_false(venture_database_save(f->database, forge, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED); g_clear_error(&error);
	g_assert_false(venture_database_save(f->database, base, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED); g_clear_error(&error);
	user = venture_database_get(f->database, VENTURE_TYPE_USER, f->administrator.user_id, &error); g_assert_no_error(error);
	scope = venture_access_policy_enter(venture_database_get_access_policy(f->database), &f->administrator);
	g_object_set(user, "role", VENTURE_USER_ROLE_OWNER, NULL);
	g_assert_false(venture_database_save(f->database, user, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED); g_clear_error(&error);
}
typedef struct { gboolean done; GBytes *body; GError *error; } TenantHttpResult;

static void
tenant_http_done(GObject *source, GAsyncResult *result, gpointer data)
{
	TenantHttpResult *outcome = data;
	outcome->body = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &outcome->error);
	outcome->done = TRUE;
}

static guint
tenant_http(SoupSession *session, guint port, const gchar *method, const gchar *path,
	const gchar *host, const gchar *cookie, const gchar *form, gchar **out_cookie)
{
	g_autofree gchar *url = g_strdup_printf("http://127.0.0.1:%u%s", port, path);
	g_autoptr(SoupMessage) message = soup_message_new(method, url);
	TenantHttpResult outcome = { FALSE, NULL, NULL };
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	if (host) soup_message_headers_replace(soup_message_get_request_headers(message), "Host", host);
	if (cookie) soup_message_headers_replace(soup_message_get_request_headers(message), "Cookie", cookie);
	if (form) {
		g_autoptr(GBytes) body = g_bytes_new(form, strlen(form));
		soup_message_set_request_body_from_bytes(message, "application/x-www-form-urlencoded", body);
	}
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL, tenant_http_done, &outcome);
	while (!outcome.done) g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(outcome.error);
	if (g_str_equal(path, "/api/v1/tenant_workspace") && soup_message_get_status(message) == 200) {
		gsize length;
		const gchar *bytes = g_bytes_get_data(outcome.body, &length);
		g_autofree gchar *text = g_strndup(bytes, length);
		g_autoptr(JsonNode) json = venture_json_parse(text, &outcome.error);
		g_assert_no_error(outcome.error);
		g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(json), "records")), ==, 1);
	}
	if (out_cookie) *out_cookie = g_strdup(soup_message_headers_get_one(soup_message_get_response_headers(message), "Set-Cookie"));
	g_clear_pointer(&outcome.body, g_bytes_unref);
	return soup_message_get_status(message);
}

static void
tenant_restore_mfa_fixture(TenantFixture *f)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *sql = g_strdup_printf(
		"INSERT INTO user_mfas (uuid, version, user_id, enabled, secret_ref) VALUES ('restored-mfa', 1, %" G_GINT64_FORMAT ", TRUE, 'unavailable-restored-factor'); "
		"INSERT INTO mfa_recovery_codes (uuid, version, user_id, prefix, code_hash) VALUES ('restored-code', 1, %" G_GINT64_FORMAT ", 'restored', 'unavailable-restored-code'); "
		"INSERT INTO mfa_policies (uuid, version, organization_id, require_mfa_for_admins) VALUES ('restored-policy', 1, %" G_GINT64_FORMAT ", TRUE)",
		f->administrator.user_id, f->administrator.user_id, f->organization_id);
	{ gboolean result = orm_connection_execute(venture_database_get_connection(f->database), sql, &error); g_assert_no_error(error); g_assert_true(result); }
}

static HtmxResponse *
tenant_suspend_during_response(HtmxRequest *request, GHashTable *params, gpointer data)
{
	TenantFixture *f = data;
	g_autoptr(GError) error = NULL;
	HtmxResponse *response = htmx_response_new();
	(void)request; (void)params;
	g_assert_true(venture_tenant_service_set_state(f->service, "suspended", "Concurrent response fixture", &error));
	g_assert_no_error(error);
	htmx_response_set_status(response, 200);
	return response;
}

/* HTTPS authority is pinned even when the gateway reaches the backend over
 * HTTP. An authenticated workspace administrator still cannot reach host
 * settings; suspension preserves only classified control/health surfaces. */
static void
test_tenant_http(TenantFixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(VentureWebServer) server = NULL;
	g_autoptr(SoupSession) session = soup_session_new();
	g_autofree gchar *state_dir = g_dir_make_tmp("venture-tenant-http-XXXXXX", &error);
	g_autofree gchar *cookie = NULL;
	guint port = 43000 + (getpid() % 10000);
	(void)data;
	g_assert_no_error(error);
	g_object_set(f->config, "state-dir", state_dir, "server-bind-address", "127.0.0.1",
		"server-port", (gint64)port, NULL);
	context = venture_context_new(f->config, f->database);
	server = venture_web_server_new(context, &error); g_assert_no_error(error);
	venture_web_server_add_classified_route(server, HTMX_METHOD_GET, "/fixture/concurrent-suspension",
		VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, tenant_suspend_during_response, f);
	g_assert_true(venture_web_server_start(server, &error)); g_assert_no_error(error);
	soup_session_set_timeout(session, 15);
	g_assert_cmpuint(tenant_http(session, port, "GET", "/api/v1/health", NULL, NULL, NULL, NULL), ==, 200);
	g_assert_cmpuint(tenant_http(session, port, "GET", "/login", "other.example.test", NULL, NULL, NULL), ==, 403);
	g_assert_cmpuint(tenant_http(session, port, "GET", "/login", "tenant.example.test", NULL, NULL, NULL), ==, 200);
	g_assert_cmpuint(tenant_http(session, port, "GET", "/api/v1/invoice", "tenant.example.test", NULL, NULL, NULL), ==, 401);
	g_assert_cmpuint(tenant_http(session, port, "POST", "/login", "tenant.example.test", NULL,
		"username=administrator&password=private-initial-password", &cookie), ==, 302);
	g_assert_nonnull(cookie);
	/* TLS terminates at the configured public origin; backend HTTP must not
	 * downgrade the browser's session cookie to an insecure cookie. */
	g_assert_nonnull(strstr(cookie, "Secure"));
	g_assert_cmpuint(tenant_http(session, port, "GET", "/api/v1/invoice", "tenant.example.test", cookie, NULL, NULL), ==, 200);
	g_assert_cmpuint(tenant_http(session, port, "GET", "/api/v1/tenant_workspace", "tenant.example.test", cookie, NULL, NULL), ==, 200);
	g_assert_cmpuint(tenant_http(session, port, "GET", "/settings", "tenant.example.test", cookie, NULL, NULL), ==, 403);
	g_assert_cmpuint(tenant_http(session, port, "GET", "/api/v1/user", "tenant.example.test", cookie, NULL, NULL), ==, 403);
	g_assert_cmpuint(tenant_http(session, port, "GET", "/fixture/concurrent-suspension", "tenant.example.test", cookie, NULL, NULL), ==, 403);
	g_assert_cmpuint(tenant_http(session, port, "GET", "/api/v1/invoice", "tenant.example.test", cookie, NULL, NULL), ==, 403);
	g_assert_cmpuint(tenant_http(session, port, "GET", "/api/v1/health", NULL, NULL, NULL, NULL), ==, 200);
	g_assert_cmpuint(tenant_http(session, port, "GET", "/api/v1/tenant_workspace", "tenant.example.test", cookie, NULL, NULL), ==, 200);
	g_assert_cmpuint(tenant_http(session, port, "GET", "/account/support", "tenant.example.test", NULL, NULL, NULL), ==, 302);
	/* A restored mandatory factor must not strand the reviewing admin. The
	 * explicit recovery clears the old factor/codes, then suspended identity
	 * routes allow fresh enrolment before any membership/security review. */
	tenant_restore_mfa_fixture(f);
	g_assert_true(venture_tenant_service_revoke_credentials(f->service, "HTTP restore quarantine", &error)); g_assert_no_error(error);
	{ gboolean result = venture_tenant_service_bootstrap_admin(f->service, f->config, "administrator",
		"fresh-http-recovery-password", TRUE, "Lost restored authenticator", &error); g_assert_no_error(error); g_assert_true(result); }
	g_clear_pointer(&cookie, g_free);
	g_assert_cmpuint(tenant_http(session, port, "POST", "/login", "tenant.example.test", NULL,
		"username=administrator&password=fresh-http-recovery-password", &cookie), ==, 302);
	g_assert_nonnull(cookie);
	/* Workspace authority also covers newly created organizations without a
	 * historical organization-membership row; their MFA policy still applies. */
	g_assert_true(orm_connection_execute(venture_database_get_connection(f->database), "UPDATE organization_memberships SET active = FALSE", &error)); g_assert_no_error(error);
	g_assert_cmpuint(tenant_http(session, port, "GET", "/e/tenant_membership", "tenant.example.test", cookie, NULL, NULL), ==, 302);
	g_assert_cmpuint(tenant_http(session, port, "GET", "/account/mfa", "tenant.example.test", cookie, NULL, NULL), ==, 200);
	g_assert_cmpuint(tenant_http(session, port, "POST", "/account/mfa/enrol", "tenant.example.test", cookie, "", NULL), ==, 302);
	{
		VentureMfaService *mfa = venture_mfa_service_get(f->database);
		g_autoptr(VentureMfaEnrolment) enrolment = venture_mfa_service_pending_enrolment(mfa, f->administrator.user_id, "Fixture", "administrator", &error);
		g_autoptr(GDateTime) now = venture_mfa_service_now(mfa);
		g_autofree guchar *secret = NULL;
		g_autofree gchar *code = NULL, *form = NULL;
		gsize length = 0;
		g_assert_no_error(error); g_assert_nonnull(enrolment);
		secret = venture_base32_decode(enrolment->secret, &length);
		code = venture_totp_code(secret, length, venture_totp_counter(now), VENTURE_TOTP_DIGITS);
		form = g_strdup_printf("code=%s", code);
		g_assert_cmpuint(tenant_http(session, port, "POST", "/account/mfa/confirm", "tenant.example.test", cookie, form, NULL), ==, 200);
	}
	g_assert_cmpuint(tenant_http(session, port, "GET", "/e/tenant_membership", "tenant.example.test", cookie, NULL, NULL), ==, 200);
	venture_web_server_stop(server);
	g_clear_object(&server); g_clear_object(&context);
	venture_test_remove_tree(state_dir);
}
static gboolean
tenant_fail_identity(VentureDatabase *database, VentureEntity *entity, VentureEntity *previous,
	gpointer data, GError **error)
{
	gboolean *fail = data;
	(void)database; (void)entity; (void)previous;
	if (!*fail) return TRUE;
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Injected quarantine failure");
	return FALSE;
}

/* Restoring a snapshot must not revive a password, session, API token or
 * identity link that was revoked after the snapshot was made. */
static void
test_tenant_restore_quarantine(TenantFixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureTenantInvitation) invitation = tenant_invitation(f, "member");
	g_autofree gchar *capability = NULL, *sql = NULL;
	g_autoptr(VentureEntity) token = g_object_new(VENTURE_TYPE_API_TOKEN, "name", "Restored token",
		"user-id", f->administrator.user_id, "active", TRUE, NULL);
	g_autoptr(VentureEntity) user = NULL, stored_token = NULL;
	g_autoptr(VentureQuery) identities_query = venture_query_new(VENTURE_TYPE_OIDC_IDENTITY);
	g_autoptr(GPtrArray) identities = NULL;
	g_autoptr(VentureUser) accepted = NULL;
	gboolean active = TRUE;
	gboolean *fail = g_new(gboolean, 1);
	(void)data;
	g_object_get(invitation, "capability", &capability, NULL);
	g_assert_true(venture_database_save(f->database, token, NULL, &error)); g_assert_no_error(error);
	/* A restored legacy identity may no longer have its provider binding.
	 * Quarantine must still disable it, without contacting that provider. */
	sql = g_strdup_printf("INSERT INTO oidc_identities (uuid, version, title, user_id, organization_id, active) VALUES ('restored-identity', 1, 'Restored provider', %" G_GINT64_FORMAT ", %" G_GINT64_FORMAT ", TRUE)",
		f->administrator.user_id, f->organization_id);
	{ gboolean result = orm_connection_execute(venture_database_get_connection(f->database), sql, &error); g_assert_no_error(error); g_assert_true(result); }
	{
		g_autoptr(VentureAccessScope) scope = venture_access_policy_enter(venture_database_get_access_policy(f->database), &f->administrator);
		g_assert_false(venture_tenant_service_revoke_credentials(f->service, "Not a platform operator", &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED); g_clear_error(&error);
	}
	venture_entity_registry_set_type_module(venture_entity_registry_get_default(), "oidc_identity", "oidc", FALSE);
	*fail = TRUE;
	venture_database_add_save_validator(f->database, VENTURE_TYPE_OIDC_IDENTITY, tenant_fail_identity, fail, g_free);
	g_assert_false(venture_tenant_service_revoke_credentials(f->service, "Injected incomplete quarantine", &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION); g_clear_error(&error);
	g_assert_true(venture_tenant_service_check_operation(f->service, TRUE, &error)); g_assert_no_error(error);
	user = venture_database_get(f->database, VENTURE_TYPE_USER, f->administrator.user_id, &error); g_assert_no_error(error);
	g_assert_true(venture_user_check_password(VENTURE_USER(user), "private-initial-password"));
	g_clear_object(&user);
	*fail = FALSE;
	g_assert_true(venture_tenant_service_revoke_credentials(f->service, "Restored snapshot review", &error)); g_assert_no_error(error);
	g_assert_false(venture_tenant_service_check_operation(f->service, FALSE, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED); g_clear_error(&error);
	g_assert_false(venture_tenant_service_check_principal(f->service, &f->administrator, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED); g_clear_error(&error);
	user = venture_database_get(f->database, VENTURE_TYPE_USER, f->administrator.user_id, &error); g_assert_no_error(error);
	g_object_get(user, "active", &active, NULL); g_assert_false(active);
	/* Even an accidental later active toggle cannot restore the old verifier. */
	g_object_set(user, "active", TRUE, NULL);
	g_assert_false(venture_user_check_password(VENTURE_USER(user), "private-initial-password"));
	stored_token = venture_database_get(f->database, VENTURE_TYPE_API_TOKEN, venture_entity_get_id(token), &error); g_assert_no_error(error);
	g_object_get(stored_token, "active", &active, NULL); g_assert_false(active);
	identities = venture_database_find(f->database, identities_query, &error); g_assert_no_error(error); g_assert_cmpuint(identities->len, ==, 1);
	g_object_get(g_ptr_array_index(identities, 0), "active", &active, NULL); g_assert_false(active);
	g_assert_true(venture_tenant_service_bootstrap_admin(f->service, f->config, "administrator",
		"fresh-reviewed-password", TRUE, "Explicit post-restore recovery", &error)); g_assert_no_error(error);
	/* Credential recovery does not implicitly activate business work. */
	g_assert_false(venture_tenant_service_check_operation(f->service, TRUE, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED); g_clear_error(&error);
	g_assert_true(venture_tenant_service_set_state_operator(f->service, "active", "Reviewed restored memberships", &error)); g_assert_no_error(error);
	accepted = venture_tenant_service_accept_invitation(f->service, f->config, capability,
		"restored-invite-replay", "private-replayed-password", &error);
	g_assert_null(accepted); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED); g_clear_error(&error);
	g_clear_object(&user);
	user = venture_database_get(f->database, VENTURE_TYPE_USER, f->administrator.user_id, &error); g_assert_no_error(error);
	g_assert_true(venture_user_check_password(VENTURE_USER(user), "fresh-reviewed-password"));
}


/* Restored colleagues retain their identity and references. Recovery cannot
 * require temporary administration or silently revive a captured password. */
static void
test_tenant_member_recovery(TenantFixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureTenantInvitation) invitation = tenant_invitation(f, "member"), recovery = NULL;
	g_autofree gchar *capability = NULL, *recovery_capability = NULL;
	g_autoptr(VentureUser) member = NULL, recovered = NULL;
	gint64 user_id;
	gint role;
	(void)data;
	g_object_get(invitation, "capability", &capability, NULL);
	member = venture_tenant_service_accept_invitation(f->service, f->config, capability,
		"retained-colleague", "private-old-member-password", &error);
	g_assert_no_error(error); g_assert_nonnull(member); user_id = venture_entity_get_id(VENTURE_ENTITY(member));
	g_assert_true(venture_tenant_service_revoke_credentials(f->service, "Restored colleague quarantine", &error)); g_assert_no_error(error);
	g_assert_true(venture_tenant_service_bootstrap_admin(f->service, f->config, "administrator",
		"fresh-administrator-password", TRUE, "Explicit reviewing administrator", &error)); g_assert_no_error(error);
	{
		g_autoptr(VentureAccessScope) scope = venture_access_policy_enter(venture_database_get_access_policy(f->database), &f->administrator);
		g_assert_false(venture_tenant_service_set_membership(f->service, user_id, "member", TRUE, "No credential shortcut", &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED); g_clear_error(&error);
		recovery = venture_tenant_service_invite_recovery(f->service, user_id, 600, "Reviewed retained colleague", &error);
		g_assert_no_error(error); g_assert_nonnull(recovery); g_object_get(recovery, "capability", &recovery_capability, NULL);
	}
	recovered = venture_tenant_service_accept_invitation(f->service, f->config, recovery_capability,
		"guessed-other-colleague", "private-fresh-member-password", &error);
	g_assert_null(recovered); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED); g_clear_error(&error);
	recovered = venture_tenant_service_accept_invitation(f->service, f->config, recovery_capability,
		"retained-colleague", "private-fresh-member-password", &error);
	g_assert_no_error(error); g_assert_nonnull(recovered);
	g_assert_cmpint(venture_entity_get_id(VENTURE_ENTITY(recovered)), ==, user_id);
	g_object_get(recovered, "role", &role, NULL); g_assert_cmpint(role, ==, VENTURE_USER_ROLE_EDITOR);
	g_assert_true(venture_tenant_service_is_member(f->service, user_id, FALSE));
	g_assert_false(venture_tenant_service_is_member(f->service, user_id, TRUE));
	g_assert_true(venture_user_check_password(recovered, "private-fresh-member-password"));
	g_assert_false(venture_user_check_password(recovered, "private-old-member-password"));
	g_assert_false(venture_tenant_service_check_operation(f->service, FALSE, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED); g_clear_error(&error);
	g_clear_object(&recovered);
	recovered = venture_tenant_service_accept_invitation(f->service, f->config, recovery_capability,
		"retained-colleague", "another-fresh-member-password", &error);
	g_assert_null(recovered); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED); g_clear_error(&error);
}


/* A default hosted install never created disabled optional-provider tables.
 * Quarantine must still complete; a disabled but retained table is covered by
 * restore-quarantine, so module state cannot substitute for schema presence. */
static void
test_tenant_restore_without_oidc(TenantFixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) user = NULL;
	gboolean active = TRUE, result;
	(void)data;
	g_assert_true(orm_connection_execute(venture_database_get_connection(f->database), "DROP TABLE oidc_identities", &error)); g_assert_no_error(error);
	result = venture_tenant_service_revoke_credentials(f->service, "Default-install quarantine", &error);
	g_assert_no_error(error); g_assert_true(result);
	user = venture_database_get(f->database, VENTURE_TYPE_USER, f->administrator.user_id, &error); g_assert_no_error(error);
	g_object_get(user, "active", &active, NULL); g_assert_false(active);
	g_assert_false(venture_user_check_password(VENTURE_USER(user), "private-initial-password"));
}

int
main(int argc, char **argv)
{
	g_setenv("VENTURE_TEST_TENANT_MFA_KEY", "synthetic-tenant-factor-encryption-fixture", TRUE);
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/tenant/records", test_tenant_records);
	g_test_add_func("/tenant/identity", test_tenant_identity);
	g_test_add_func("/tenant/classification", test_tenant_classification);
	g_test_add("/tenant/invitations", TenantFixture, NULL, tenant_setup, test_tenant_invitations, tenant_teardown);
	g_test_add("/tenant/support", TenantFixture, NULL, tenant_setup, test_tenant_support, tenant_teardown);
	g_test_add("/tenant/actions", TenantFixture, NULL, tenant_setup, test_tenant_actions, tenant_teardown);
	g_test_add("/tenant/host-authority", TenantFixture, NULL, tenant_setup, test_tenant_host_authority, tenant_teardown);
	g_test_add("/tenant/http", TenantFixture, NULL, tenant_setup, test_tenant_http, tenant_teardown);
	g_test_add("/tenant/restore-quarantine", TenantFixture, NULL, tenant_setup, test_tenant_restore_quarantine, tenant_teardown);
	g_test_add("/tenant/member-recovery", TenantFixture, NULL, tenant_setup, test_tenant_member_recovery, tenant_teardown);
	g_test_add("/tenant/restore-without-oidc", TenantFixture, NULL, tenant_setup, test_tenant_restore_without_oidc, tenant_teardown);
	return g_test_run();
}
