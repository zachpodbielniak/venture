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
		"tenant_support_grant", "tenant_event", "tenant_signup"
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
	if (soup_message_get_status(message) == 429 && g_str_equal(path, "/fixture/admission")) {
		g_assert_cmpstr(soup_message_headers_get_one(soup_message_get_response_headers(message), "Cache-Control"), ==, "no-store");
		g_assert_cmpstr(soup_message_headers_get_one(soup_message_get_response_headers(message), "Retry-After"), ==, "60");
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

static SoupMessage *
booking_browser_request(SoupSession *session, VentureWebServer *server, const gchar *method, const gchar *origin, const gchar *form)
{
	g_autofree gchar *url = g_strconcat(venture_web_server_get_base_url(server), "/book/browser-booking", NULL);
	SoupMessage *message = soup_message_new(method, url);
	TenantHttpResult result = { FALSE, NULL, NULL };
	soup_message_headers_replace(soup_message_get_request_headers(message), "Host", "tenant.example.test");
	soup_message_headers_replace(soup_message_get_request_headers(message), "Accept", "text/html");
	if (origin) soup_message_headers_replace(soup_message_get_request_headers(message), "Origin", origin);
	if (form) {
		g_autoptr(GBytes) bytes = g_bytes_new(form, strlen(form));
		soup_session_set_timeout(session, 15);
		soup_message_set_request_body_from_bytes(message, "application/x-www-form-urlencoded", bytes);
	}
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL, tenant_http_done, &result);
	while (!result.done) g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(result.error);
	g_clear_pointer(&result.body, g_bytes_unref);
	return message;
}

/* A public page must let the browser preserve its origin on submission,
 * while an unrelated origin remains unable to reserve a meeting. */
static void
test_tenant_booking_browser(TenantFixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(VentureWebServer) server = NULL;
	g_autoptr(SoupSession) session = soup_session_new();
	g_autoptr(GInetAddress) loopback = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
	g_autofree gchar *bind_address = g_inet_address_to_string(loopback);
	g_autoptr(SoupMessage) viewed = NULL, refused = NULL, booked = NULL;
	g_autoptr(VentureEntity) page = NULL;
	g_autoptr(VentureBookingService) service = venture_booking_service_new(f->database);
	g_autoptr(JsonNode) slots = NULL;
	g_autoptr(VentureQuery) meetings = venture_query_new(VENTURE_TYPE_ACTIVITY);
	g_autofree gchar *state_dir = g_dir_make_tmp("venture-booking-origin-XXXXXX", &error), *start = NULL, *form = NULL;
	const gchar *policy, *origin;
	(void)data;
	g_assert_no_error(error);
	g_object_set(f->config, "state-dir", state_dir, "server-bind-address", bind_address, "server-port", (gint64)0, NULL);
	context = venture_context_new(f->config, f->database);
	page = g_object_new(VENTURE_TYPE_BOOKING_PAGE, "organization-id", f->organization_id,
		"title", "Browser consultation", "slug", "browser-booking", "owner", "administrator",
		"duration-minutes", (gint64)30, "timezone", "UTC", "horizon-days", (gint64)2, "active", TRUE,
		"availability", "{\"mon\":\"09:00-17:00\",\"tue\":\"09:00-17:00\",\"wed\":\"09:00-17:00\","
		"\"thu\":\"09:00-17:00\",\"fri\":\"09:00-17:00\",\"sat\":\"09:00-17:00\",\"sun\":\"09:00-17:00\"}", NULL);
	g_assert_true(venture_database_save(f->database, page, NULL, &error)); g_assert_no_error(error);
	slots = venture_booking_service_slots(service, page, NULL, &error); g_assert_no_error(error);
	g_assert_cmpuint(json_array_get_length(json_node_get_array(slots)), >, 0);
	start = g_uri_escape_string(json_object_get_string_member(json_array_get_object_element(json_node_get_array(slots), 0), "start"), NULL, FALSE);
	form = g_strdup_printf("start=%s&name=Visitor&email=visitor%%40example.test", start);
	server = venture_web_server_new(context, &error); g_assert_no_error(error);
	g_assert_true(venture_web_server_start(server, &error)); g_assert_no_error(error);
	viewed = booking_browser_request(session, server, "GET", NULL, NULL);
	g_assert_cmpuint(soup_message_get_status(viewed), ==, 200);
	policy = soup_message_headers_get_one(soup_message_get_response_headers(viewed), "Referrer-Policy");
	/* Browser form POST serializes an opaque origin under no-referrer. */
	origin = !g_strcmp0(policy, "no-referrer") ? "null" : "https://tenant.example.test";
	refused = booking_browser_request(session, server, "POST", "https://other.example.test", form);
	g_assert_cmpuint(soup_message_get_status(refused), ==, 403);
	booked = booking_browser_request(session, server, "POST", origin, form);
	g_assert_cmpuint(soup_message_get_status(booked), ==, 201);
	g_assert_cmpstr(policy, ==, "same-origin");
	venture_query_set_organization(meetings, f->organization_id);
	g_assert_cmpint(venture_database_count(f->database, meetings, &error), ==, 1); g_assert_no_error(error);
	venture_web_server_stop(server); g_clear_object(&server); g_clear_object(&context);
	venture_test_remove_tree(state_dir);
}

/* One JSON API call as a bearer token on the hosted origin. */
static guint
tenant_api(SoupSession *session, VentureWebServer *server, const gchar *method, const gchar *path,
	const gchar *token, const gchar *json, JsonNode **out)
{
	g_autofree gchar *url = g_strconcat(venture_web_server_get_base_url(server), path, NULL);
	g_autofree gchar *bearer = g_strconcat("Bearer ", token, NULL);
	g_autoptr(SoupMessage) message = soup_message_new(method, url);
	SoupMessageHeaders *headers = soup_message_get_request_headers(message);
	TenantHttpResult result = { FALSE, NULL, NULL };
	guint status;
	soup_message_headers_replace(headers, "Host", "tenant.example.test");
	soup_message_headers_replace(headers, "Authorization", bearer);
	soup_message_headers_replace(headers, "Accept", "application/json");
	if (json) {
		g_autoptr(GBytes) bytes = g_bytes_new(json, strlen(json));
		soup_message_set_request_body_from_bytes(message, "application/json", bytes);
	}
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL, tenant_http_done, &result);
	while (!result.done) g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(result.error);
	status = soup_message_get_status(message);
	if (out) {
		gsize length = 0;
		const gchar *bytes = g_bytes_get_data(result.body, &length);
		g_autofree gchar *text = g_strndup(bytes, length);
		*out = venture_json_parse(text, NULL);
		if (*out == NULL || !JSON_NODE_HOLDS_OBJECT(*out) || status >= 300)
			g_test_message("%s %s answered %u with: %s", method, path, status, text);
	}
	g_clear_pointer(&result.body, g_bytes_unref);
	return status;
}

/* A user's own token, as /account/tokens mints it: the user's role, and the
 * organization roles the user holds when it is minted. */
static gchar *
tenant_user_token(TenantFixture *f, gint64 user_id, const gchar *name, gint64 *token_id)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureApiToken) token = venture_api_token_new();
	gchar *secret;
	g_object_set(token, "name", name, "user-id", user_id, "role", VENTURE_USER_ROLE_EDITOR, "active", TRUE, NULL);
	secret = venture_api_token_generate(token);
	g_assert_true(venture_database_save(f->database, VENTURE_ENTITY(token), NULL, &error)); g_assert_no_error(error);
	if (token_id) *token_id = venture_entity_get_id(VENTURE_ENTITY(token));
	return secret;
}

/* A business made the way Lightsite makes one: the trusted sign-up, run as
 * a workspace administrator's service token. Returns the organization. */
static gint64
tenant_lightsite_business(TenantFixture *f, const VentureAuthPrincipal *service, const gchar *key,
	const gchar *email, const gchar *business, gint64 *owner_id)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureAccessScope) scope = venture_access_policy_enter(venture_database_get_access_policy(f->database), service);
	g_autoptr(JsonNode) signup = NULL;
	gboolean created = FALSE;
	signup = venture_tenant_service_lightsite_signup(f->service, key, email, "https://id.example.test", key,
		business, "Alex", "Example", NULL, &created, &error);
	g_assert_no_error(error); g_assert_nonnull(signup); g_assert_true(created);
	*owner_id = json_object_get_int_member(json_node_get_object(signup), "user_id");
	return json_object_get_int_member(json_node_get_object(signup), "organization_id");
}

static gchar *
tenant_page_body(gint64 organization_id, const gchar *slug, gint64 owner_user_id)
{
	return g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT ",\"title\":\"Estimate visit\","
		"\"slug\":\"%s\",\"owner_user_id\":%" G_GINT64_FORMAT ",\"duration_minutes\":30,\"timezone\":\"UTC\","
		"\"availability\":\"{\\\"mon\\\":\\\"09:00-17:00\\\"}\",\"active\":true}",
		organization_id, slug, owner_user_id);
}

/*
 * A booking page's owner is a username, and an organization owner's token
 * cannot read a username: /api/v1/user/:id is the platform owner's. Lightsite
 * makes the page with the business owner's own token, found the owner's
 * membership, and could not name it, so every page it made was owned by the
 * literal "owner" and the real owner's synced calendar never blocked a slot.
 * The page takes owner_user_id instead and Venture writes the username. If
 * this regresses, the page saves with an empty owner (the field is ignored)
 * or a stranger learns a username: only the organization's owner or
 * administrator may name an account, and only one of its own members.
 */
static void
test_tenant_booking_owner_by_account(TenantFixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(VentureWebServer) server = NULL;
	g_autoptr(SoupSession) session = soup_session_new();
	g_autoptr(GInetAddress) loopback = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
	g_autofree gchar *bind_address = g_inet_address_to_string(loopback);
	g_autofree gchar *state_dir = g_dir_make_tmp("venture-booking-owner-XXXXXX", &error);
	g_autofree gchar *service_secret = NULL, *owner_token = NULL, *other_token = NULL, *crew_token = NULL;
	g_autofree gchar *owner_username = NULL, *path = NULL, *body = NULL, *capability = NULL;
	g_autoptr(VentureEntity) owner = NULL;
	g_autoptr(VentureTenantInvitation) invitation = NULL;
	g_autoptr(VentureUser) crew = NULL;
	g_autoptr(JsonNode) answer = NULL;
	g_autoptr(VentureQuery) pages = venture_query_new(VENTURE_TYPE_BOOKING_PAGE);
	VentureAuthPrincipal service;
	gint64 service_token = 0, business, owner_id = 0, other_business, other_owner_id = 0, crew_id;
	JsonArray *records;
	(void)data;
	g_assert_no_error(error);
	service_secret = tenant_user_token(f, f->administrator.user_id, "Lightsite service", &service_token);
	service.user_id = f->administrator.user_id; service.token_id = service_token;
	service.role = VENTURE_USER_ROLE_EDITOR; service.name = (gchar *)"Lightsite service";
	service.authenticated = TRUE; service.provider_token = FALSE;
	business = tenant_lightsite_business(f, &service, "fixture-roofing", "dana@roofing.test", "Fixture Roofing", &owner_id);
	other_business = tenant_lightsite_business(f, &service, "fixture-plumbing", "sam@plumbing.test", "Fixture Plumbing", &other_owner_id);
	owner = venture_database_get(f->database, VENTURE_TYPE_USER, owner_id, &error); g_assert_no_error(error);
	g_object_get(owner, "username", &owner_username, NULL);
	g_assert_false(venture_string_is_empty(owner_username));
	g_assert_cmpstr(owner_username, !=, "owner");
	/* A plain member of the same business: an editor, not its owner. */
	{
		g_autoptr(VentureAccessScope) scope = venture_access_policy_enter(venture_database_get_access_policy(f->database), &f->administrator);
		invitation = venture_tenant_service_invite(f->service, "member", business, VENTURE_ORGANIZATION_ROLE_EDITOR,
			3600, "Roofing crew member", &error);
		g_assert_no_error(error); g_assert_nonnull(invitation);
	}
	g_object_get(invitation, "capability", &capability, NULL);
	crew = venture_tenant_service_accept_invitation(f->service, f->config, capability, "roofing-crew", "private-crew-password", &error);
	g_assert_no_error(error); g_assert_nonnull(crew);
	crew_id = venture_entity_get_id(VENTURE_ENTITY(crew));
	owner_token = tenant_user_token(f, owner_id, "Roofing owner", NULL);
	other_token = tenant_user_token(f, other_owner_id, "Plumbing owner", NULL);
	crew_token = tenant_user_token(f, crew_id, "Roofing crew", NULL);

	g_object_set(f->config, "state-dir", state_dir, "server-bind-address", bind_address, "server-port", (gint64)0, NULL);
	context = venture_context_new(f->config, f->database);
	server = venture_web_server_new(context, &error); g_assert_no_error(error);
	g_assert_true(venture_web_server_start(server, &error)); g_assert_no_error(error);
	soup_session_set_timeout(session, 15);

	/* The issue as reported: the owner's membership is readable, the
	 * account behind it is not -- and must stay that way. */
	path = g_strdup_printf("/api/v1/organization_membership?organization_id=%" G_GINT64_FORMAT "&role=owner", business);
	g_assert_cmpuint(tenant_api(session, server, "GET", path, owner_token, NULL, &answer), ==, 200);
	records = json_object_get_array_member(json_node_get_object(answer), "records");
	g_assert_cmpuint(json_array_get_length(records), ==, 1);
	g_assert_cmpint(json_object_get_int_member(json_array_get_object_element(records, 0), "user_id"), ==, owner_id);
	g_clear_pointer(&answer, json_node_unref); g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/api/v1/user/%" G_GINT64_FORMAT, owner_id);
	g_assert_cmpuint(tenant_api(session, server, "GET", path, owner_token, NULL, NULL), ==, 403);
	g_assert_cmpuint(tenant_api(session, server, "GET", path, crew_token, NULL, NULL), ==, 403);
	g_assert_cmpuint(tenant_api(session, server, "GET", path, other_token, NULL, NULL), ==, 403);

	/* The owner's token names the owner by account; Venture writes the
	 * username the meetings and the busy time are keyed by. */
	body = tenant_page_body(business, "estimate", owner_id);
	g_assert_cmpuint(tenant_api(session, server, "POST", "/api/v1/booking_page", owner_token, body, &answer), ==, 201);
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(answer), "owner"), ==, owner_username);
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(answer), "owner_user_id"), ==, owner_id);
	g_clear_pointer(&answer, json_node_unref); g_clear_pointer(&body, g_free);

	/* A plain member may make pages but may not turn an account id into
	 * a username. */
	body = tenant_page_body(business, "crew-page", owner_id);
	g_assert_cmpuint(tenant_api(session, server, "POST", "/api/v1/booking_page", crew_token, body, &answer), ==, 403);
	{ g_autofree gchar *said = answer ? venture_json_to_string(answer, FALSE) : NULL; g_assert_null(said ? strstr(said, owner_username) : NULL); }
	g_clear_pointer(&answer, json_node_unref); g_clear_pointer(&body, g_free);

	/* Another business's owner cannot name an account outside its own
	 * organization, in its own organization or in this one. */
	body = tenant_page_body(other_business, "borrowed-owner", owner_id);
	g_assert_cmpuint(tenant_api(session, server, "POST", "/api/v1/booking_page", other_token, body, &answer), ==, 422);
	{ g_autofree gchar *said = answer ? venture_json_to_string(answer, FALSE) : NULL; g_assert_null(said ? strstr(said, owner_username) : NULL); }
	g_clear_pointer(&answer, json_node_unref); g_clear_pointer(&body, g_free);
	body = tenant_page_body(business, "foreign-page", owner_id);
	g_assert_cmpuint(tenant_api(session, server, "POST", "/api/v1/booking_page", other_token, body, &answer), ==, 404);
	{ g_autofree gchar *said = answer ? venture_json_to_string(answer, FALSE) : NULL; g_assert_null(said ? strstr(said, owner_username) : NULL); }
	g_clear_pointer(&answer, json_node_unref); g_clear_pointer(&body, g_free);

	/* Only the owner's page was made. Its owner typed by hand later is no
	 * longer that account, so the id goes with it: the policy would
	 * otherwise keep giving the old account the page. */
	{
		g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(f->database), NULL);
		g_autoptr(GPtrArray) made = venture_database_find(f->database, pages, &error);
		g_autofree gchar *kept = NULL;
		VentureEntity *page;
		gint64 account = -1;
		g_assert_no_error(error); g_assert_cmpuint(made->len, ==, 1);
		page = g_ptr_array_index(made, 0);
		g_object_set(page, "title", "Estimate visit (renamed)", NULL);
		g_assert_true(venture_database_save(f->database, page, NULL, &error)); g_assert_no_error(error);
		g_object_get(page, "owner", &kept, "owner-user-id", &account, NULL);
		g_assert_cmpstr(kept, ==, owner_username); g_assert_cmpint(account, ==, owner_id);
		g_clear_pointer(&kept, g_free);
		g_object_set(page, "owner", "roofing-crew", NULL);
		g_assert_true(venture_database_save(f->database, page, NULL, &error)); g_assert_no_error(error);
		g_object_get(page, "owner", &kept, "owner-user-id", &account, NULL);
		g_assert_cmpstr(kept, ==, "roofing-crew"); g_assert_cmpint(account, ==, 0);
	}
	venture_web_server_stop(server); g_clear_object(&server); g_clear_object(&context);
	venture_test_remove_tree(state_dir);
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
	guint port = 0;	/* the kernel picks; read back after the start */
	(void)data;
	g_assert_no_error(error);
	g_object_set(f->config, "state-dir", state_dir, "server-bind-address", "127.0.0.1",
		"server-port", (gint64)port, NULL);
	context = venture_context_new(f->config, f->database);
	server = venture_web_server_new(context, &error); g_assert_no_error(error);
	venture_web_server_add_classified_route(server, HTMX_METHOD_GET, "/fixture/concurrent-suspension",
		VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, tenant_suspend_during_response, f);
	g_assert_true(venture_web_server_start(server, &error)); g_assert_no_error(error);
	port = venture_web_server_get_port(server);
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
	/* The same authority must reach a business write once the workspace is
	 * back in service: the approval decision used to demand an
	 * organization_membership row and answered 404 for a business record. */
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_CONTACT);
		g_autoptr(GPtrArray) contacts = NULL;
		g_autofree gchar *form = g_strdup_printf("name=Unrowed%%20organization%%20contact&organization_id=%" G_GINT64_FORMAT, f->organization_id);
		g_assert_true(venture_tenant_service_set_state_operator(f->service, "active", "Reviewed after HTTP restore", &error)); g_assert_no_error(error);
		g_assert_cmpuint(tenant_http(session, port, "POST", "/e/contact", "tenant.example.test", cookie, form, NULL), ==, 302);
		venture_query_add_filter_string(query, "name", VENTURE_FILTER_OP_EQ, "Unrowed organization contact", NULL);
		contacts = venture_database_find(f->database, query, &error); g_assert_no_error(error);
		g_assert_cmpuint(contacts->len, ==, 1);
		g_assert_cmpint(venture_entity_get_organization_id(g_ptr_array_index(contacts, 0)), ==, f->organization_id);
	}
	venture_web_server_stop(server);
	g_clear_object(&server); g_clear_object(&context);
	venture_test_remove_tree(state_dir);
}
typedef struct {
	SoupSession *session;
	guint port;
	guint writes;
	gboolean nested;
} AdmissionFixture;

static HtmxResponse *
tenant_admission_callback(HtmxRequest *request, GHashTable *params, gpointer data)
{
	AdmissionFixture *fixture = data;
	(void)request; (void)params;
	fixture->writes++;
	if (fixture->nested) {
		fixture->nested = FALSE;
		g_assert_cmpuint(tenant_http(fixture->session, fixture->port, "POST", "/fixture/admission",
			"tenant.example.test", NULL, "", NULL), ==, 429);
	}
	return htmx_response_new_with_content("admitted");
}

/* Rejected work must not dispatch, exhaust a neighbor's bucket or consume a
 * concurrency slot forever. A nested request models a provider's main loop. */
static void
test_tenant_http_admission(TenantFixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(VentureWebServer) server = NULL;
	g_autoptr(SoupSession) session = soup_session_new();
	g_autofree gchar *state_dir = g_dir_make_tmp("venture-admission-XXXXXX", &error);
	AdmissionFixture fixture;
	gboolean nested = GPOINTER_TO_INT(data) != 0;
	g_assert_no_error(error);
	fixture.session = session; fixture.port = 0;	/* the kernel picks; read back after the start */
	fixture.writes = 0; fixture.nested = nested;
	g_object_set(session, "timeout", 5, NULL);
	g_object_set(f->config, "state-dir", state_dir, "server-bind-address", "127.0.0.1",
		"server-port", (gint64)fixture.port, "hosted-http-requests-per-minute", (gint64)1,
		"hosted-http-burst", (gint64)2, "hosted-http-concurrency", (gint64)1, NULL);
	context = venture_context_new(f->config, f->database);
	server = venture_web_server_new(context, &error); g_assert_no_error(error); g_assert_nonnull(server);
	venture_web_server_add_classified_route(server, HTMX_METHOD_POST, "/fixture/admission",
		VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, tenant_admission_callback, &fixture);
	g_assert_true(venture_web_server_start(server, &error)); g_assert_no_error(error);
	fixture.port = venture_web_server_get_port(server);
	/* Host refusal happens before admission and cannot spend its two tokens. */
	g_assert_cmpuint(tenant_http(session, fixture.port, "POST", "/fixture/admission", "neighbor.example.test", NULL, "", NULL), ==, 403);
	g_assert_cmpuint(tenant_http(session, fixture.port, "POST", "/fixture/admission", "tenant.example.test", NULL, "", NULL), ==, 200);
	g_assert_cmpuint(fixture.writes, ==, 1);
	g_assert_cmpuint(tenant_http(session, fixture.port, "POST", "/fixture/admission", "tenant.example.test", NULL, "", NULL), ==, 200);
	g_assert_cmpuint(fixture.writes, ==, 2);
	g_assert_cmpuint(tenant_http(session, fixture.port, "POST", "/fixture/admission", "tenant.example.test", NULL, "", NULL), ==, 429);
	g_assert_cmpuint(fixture.writes, ==, 2);
	g_assert_cmpuint(tenant_http(session, fixture.port, "GET", "/api/v1/health", "tenant.example.test", NULL, NULL, NULL), ==, 200);
	venture_web_server_stop(server); g_clear_object(&server); g_clear_object(&context);
	venture_test_remove_tree(state_dir);
}

static void
test_tenant_http_invalid_limits(TenantFixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(VentureWebServer) server = NULL;
	g_autofree gchar *state_dir = g_dir_make_tmp("venture-admission-invalid-XXXXXX", &error);
	const gchar *properties[] = { "hosted-http-requests-per-minute", "hosted-http-burst", "hosted-http-concurrency" };
	guint i;
	(void)data;
	g_assert_no_error(error);
	g_object_set(f->config, "state-dir", state_dir, NULL);
	context = venture_context_new(f->config, f->database);
	for (i = 0; i < G_N_ELEMENTS(properties); i++) {
		gint64 previous;
		g_object_get(f->config, properties[i], &previous, NULL);
		g_object_set(f->config, properties[i], (gint64)0, NULL);
		server = venture_web_server_new(context, &error);
		g_assert_null(server); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
		g_clear_error(&error);
		g_object_set(f->config, properties[i], G_MAXINT64, NULL);
		server = venture_web_server_new(context, &error);
		g_assert_null(server); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
		g_clear_error(&error);
		g_object_set(f->config, properties[i], previous, NULL);
	}
	g_clear_object(&context); venture_test_remove_tree(state_dir);
}

static void
test_tenant_http_invalid_fresh(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDatabase) database = venture_test_accounting_database(&error);
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(VentureWebServer) server = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_TENANT_WORKSPACE);
	g_autoptr(GPtrArray) rows = NULL;
	g_autofree gchar *state_dir = NULL;
	g_assert_no_error(error);
	state_dir = g_dir_make_tmp("venture-admission-fresh-XXXXXX", &error); g_assert_no_error(error);
	g_assert_true(venture_database_migrate(database, venture_entity_registry_get_default(), &error)); g_assert_no_error(error);
	g_object_set(config, "state-dir", state_dir, "hosted-enabled", TRUE,
		"hosted-workspace-id", "b94b25be-7878-48c9-b941-98ca5d0d00d7", "hosted-origin", "https://fresh.example.test",
		"hosted-http-burst", (gint64)0, NULL);
	context = venture_context_new(config, database);
	server = venture_web_server_new(context, &error);
	g_assert_null(server); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG); g_clear_error(&error);
	/* A typo must not irreversibly bind a fresh workspace before refusal. */
	rows = venture_database_find(database, query, &error); g_assert_no_error(error);
	g_assert_nonnull(rows); g_assert_cmpuint(rows->len, ==, 0);
	g_clear_object(&context); venture_test_accounting_database_cleanup(database); g_clear_object(&database);
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
	g_test_add("/tenant/booking-browser-origin", TenantFixture, NULL, tenant_setup, test_tenant_booking_browser, tenant_teardown);
	g_test_add("/tenant/booking-owner-by-account", TenantFixture, NULL, tenant_setup, test_tenant_booking_owner_by_account, tenant_teardown);
	g_test_add("/tenant/http-admission", TenantFixture, NULL, tenant_setup, test_tenant_http_admission, tenant_teardown);
	g_test_add("/tenant/http-nested-admission", TenantFixture, GINT_TO_POINTER(1), tenant_setup, test_tenant_http_admission, tenant_teardown);
	g_test_add("/tenant/http-invalid-limits", TenantFixture, NULL, tenant_setup, test_tenant_http_invalid_limits, tenant_teardown);
	g_test_add_func("/tenant/http-invalid-fresh", test_tenant_http_invalid_fresh);
	g_test_add("/tenant/restore-quarantine", TenantFixture, NULL, tenant_setup, test_tenant_restore_quarantine, tenant_teardown);
	g_test_add("/tenant/member-recovery", TenantFixture, NULL, tenant_setup, test_tenant_member_recovery, tenant_teardown);
	g_test_add("/tenant/restore-without-oidc", TenantFixture, NULL, tenant_setup, test_tenant_restore_without_oidc, tenant_teardown);
	return g_test_run();
}
