/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

struct _VentureTenantService {
	GObject parent_instance;
	VentureDatabase *database;
	gchar *workspace_id;
	gchar *origin;
	gboolean configured;
	gboolean enabled;
	gboolean initialized;
	guint maintenance_depth;
	VentureTenantSupportScope *support;
};
struct _VentureTenantSupportScope {
	GObject parent_instance;
	VentureTenantService *service;
	VentureTenantSupportScope *previous;
	gint64 grant_id;
	gint64 operator_id;
	gint64 organization_id;
	gboolean control;
	guint previous_maintenance_depth;
};
static VentureEntity *support_grant_read(VentureTenantService *self,
	const VentureAuthPrincipal *actor, GError **error);
G_DEFINE_FINAL_TYPE(VentureTenantService, venture_tenant_service, G_TYPE_OBJECT)

static gchar *
record_enum_nick(VentureEntity *entity, const gchar *field)
{
	GParamSpec *property = g_object_class_find_property(G_OBJECT_GET_CLASS(entity), field);
	gint value = 0;
	g_object_get(entity, field, &value, NULL);
	return g_strdup(venture_enum_to_nick(G_PARAM_SPEC_VALUE_TYPE(property), value));
}

static gint
role_value(const gchar *role)
{
	gint value = VENTURE_TENANT_ROLE_UNKNOWN;
	venture_enum_from_nick(VENTURE_TYPE_TENANT_ROLE, role, &value);
	return value;
}

static gboolean
tenant_fail(GError **error, const gchar *message)
{
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED, "Hosted workspace: %s", message);
	return FALSE;
}

static void
tenant_finalize(GObject *object)
{
	VentureTenantService *self = VENTURE_TENANT_SERVICE(object);
	if (self->database)
		g_object_remove_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	g_free(self->workspace_id);
	g_free(self->origin);
	G_OBJECT_CLASS(venture_tenant_service_parent_class)->finalize(object);
}

static void
venture_tenant_service_class_init(VentureTenantServiceClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = tenant_finalize;
}

static void
venture_tenant_service_init(VentureTenantService *self)
{
	(void)self;
}

VentureTenantService *
venture_tenant_service_get(VentureDatabase *database)
{
	VentureTenantService *self;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	self = g_object_get_data(G_OBJECT(database), "venture-tenant-service");
	if (!self) {
		self = g_object_new(VENTURE_TYPE_TENANT_SERVICE, NULL);
		self->database = database;
		g_object_add_weak_pointer(G_OBJECT(database), (gpointer *)&self->database);
		g_object_set_data_full(G_OBJECT(database), "venture-tenant-service", self, g_object_unref);
	}
	return self;
}

static gboolean
origin_valid(const gchar *origin)
{
	g_autoptr(GUri) uri = NULL;
	const gchar *host, *scheme, *path;
	uri = g_uri_parse(origin, G_URI_FLAGS_NONE, NULL);
	if (!uri) return FALSE;
	host = g_uri_get_host(uri);
	scheme = g_uri_get_scheme(uri);
	path = g_uri_get_path(uri);
	if (!host || !*host || g_uri_get_userinfo(uri) || g_uri_get_query(uri) ||
	    g_uri_get_fragment(uri) || (path && *path)) return FALSE;
	/* HTTP is available only for an explicitly loopback development origin. */
	return g_strcmp0(scheme, "https") == 0 ||
		(g_strcmp0(scheme, "http") == 0 &&
		 (g_strcmp0(host, "127.0.0.1") == 0 || g_strcmp0(host, "::1") == 0));
}

gboolean
venture_tenant_service_configure(VentureTenantService *self, VentureConfig *config, GError **error)
{
	g_autofree gchar *workspace = NULL, *origin = NULL;
	gboolean enabled, require_auth;
	g_object_get(config, "hosted-enabled", &enabled, "hosted-workspace-id", &workspace,
	             "hosted-origin", &origin, "security-require-auth", &require_auth, NULL);
	if (enabled && (!require_auth || !g_uuid_string_is_valid(workspace) || !origin_valid(origin)))
		return tenant_fail(error, "authentication, a workspace UUID and a canonical HTTPS origin are required");
	if (self->configured) {
		if (self->enabled != enabled || (enabled &&
		    (g_strcmp0(self->workspace_id, workspace) || g_strcmp0(self->origin, origin))))
			return tenant_fail(error, "running configuration is immutable");
		return TRUE;
	}
	self->configured = TRUE;
	self->enabled = enabled;
	self->workspace_id = g_steal_pointer(&workspace);
	self->origin = g_steal_pointer(&origin);
	return TRUE;
}

/* This narrow reader bypasses application gates to inspect the gate itself.
 * SQL is constant and remains on the owning main thread, never a provider worker. */
static VentureEntity *
workspace_read(VentureTenantService *self, GError **error)
{
	g_autoptr(OrmResult) result = NULL;
	VentureEntity *workspace;
	if (!self->database) { tenant_fail(error, "workspace repository is no longer available"); return NULL; }
	result = orm_connection_query(venture_database_get_connection(self->database),
		"SELECT * FROM tenant_workspaces ORDER BY id LIMIT 2", error);
	if (!result || !orm_result_next(result)) return NULL;
	workspace = g_object_new(VENTURE_TYPE_TENANT_WORKSPACE, NULL);
	venture_schema_populate_entity(workspace, orm_result_get_row(result));
	if (orm_result_next(result)) {
		g_object_unref(workspace);
		tenant_fail(error, "multiple workspace identities are forbidden");
		return NULL;
	}
	return workspace;
}

gboolean
venture_tenant_service_verify_existing(VentureTenantService *self, GError **error)
{
	g_autoptr(VentureEntity) prototype = g_object_new(VENTURE_TYPE_TENANT_WORKSPACE, NULL);
	g_autoptr(GHashTable) columns = NULL;
	g_autoptr(VentureEntity) existing = NULL;
	g_autoptr(GError) local_error = NULL;
	if (!self->configured) return tenant_fail(error, "startup configuration has not been verified");
	columns = venture_schema_get_existing_columns(venture_database_get_connection(self->database),
		venture_entity_get_table_name(prototype), error);
	if (!columns) return FALSE;
	if (g_hash_table_size(columns) == 0) return TRUE;
	existing = workspace_read(self, &local_error);
	if (local_error) { g_propagate_error(error, g_steal_pointer(&local_error)); return FALSE; }
	if (!existing) return TRUE;
	return venture_tenant_service_initialize(self, error);
}

gboolean
venture_tenant_service_initialize(VentureTenantService *self, GError **error)
{
	g_autoptr(VentureEntity) workspace = NULL;
	g_autofree gchar *identity = NULL, *origin = NULL;
	g_autoptr(GError) local_error = NULL;
	gboolean saved;
	workspace = workspace_read(self, &local_error);
	if (local_error) { g_propagate_error(error, g_steal_pointer(&local_error)); return FALSE; }
	if (!self->configured) return tenant_fail(error, "startup configuration has not been verified");
	if (workspace) {
		g_object_get(workspace, "workspace-id", &identity, "origin", &origin, NULL);
		if (!self->enabled || venture_entity_is_deleted(workspace) ||
		    g_strcmp0(identity, self->workspace_id) || g_strcmp0(origin, self->origin))
			return tenant_fail(error, "database workspace identity or public origin does not match configuration");
	} else if (self->enabled) {
		workspace = g_object_new(VENTURE_TYPE_TENANT_WORKSPACE, "binding-key", "workspace", "workspace-id", self->workspace_id,
		                        "origin", self->origin, "state", VENTURE_TENANT_STATE_ACTIVE, "reason", "Initial workspace binding", NULL);
		self->maintenance_depth++;
		saved = venture_database_save(self->database, workspace, NULL, error);
		self->maintenance_depth--;
		if (!saved) return FALSE;
	}
	self->initialized = TRUE;
	return TRUE;
}

gboolean
venture_tenant_service_is_enabled(VentureTenantService *self)
{
	return self->enabled;
}

const gchar *
venture_tenant_service_get_workspace_id(VentureTenantService *self)
{
	return self->enabled ? self->workspace_id : NULL;
}

const gchar *
venture_tenant_service_get_origin(VentureTenantService *self)
{
	return self->enabled ? self->origin : NULL;
}

gboolean
venture_tenant_service_check_operation(VentureTenantService *self, gboolean write, GError **error)
{
	g_autoptr(VentureEntity) workspace = NULL;
	g_autofree gchar *state = NULL, *identity = NULL, *origin = NULL;
	if (!self->enabled || self->maintenance_depth > 0) return TRUE;
	if (!self->initialized) return tenant_fail(error, "workspace is not initialized");
	workspace = workspace_read(self, error);
	if (!workspace) {
		if (!error || !*error) tenant_fail(error, "workspace identity is missing");
		return FALSE;
	}
	state = record_enum_nick(workspace, "state");
	g_object_get(workspace, "workspace-id", &identity, "origin", &origin, NULL);
	if (venture_entity_is_deleted(workspace) || g_strcmp0(identity, self->workspace_id) || g_strcmp0(origin, self->origin))
		return tenant_fail(error, "durable workspace binding changed");
	if (g_strcmp0(state, "active") == 0 || (!write && g_strcmp0(state, "read_only") == 0)) return TRUE;
	return tenant_fail(error, "workspace lifecycle refuses this operation");
}


gboolean
venture_tenant_service_check_write(VentureTenantService *self, VentureEntity *entity, GError **error)
{
	GType type = G_OBJECT_TYPE(entity);
	gboolean identity_control = FALSE;
	if (!self->enabled || self->maintenance_depth > 0) return TRUE;
	if (self->support && self->support->control && !self->support->grant_id) {
		identity_control = venture_data_class_for_type(type) == VENTURE_DATA_CLASS_PERSONAL;
		/* Login and MFA recovery must retain their audit trail while paused.
		 * Only the internal writer may append an identity-targeted audit row;
		 * this does not permit business writes from an identity route. */
		if (VENTURE_IS_AUDIT_ENTRY(entity) && !venture_access_policy_get_actor(
		    venture_database_get_access_policy(self->database))) {
			g_autofree gchar *target_type = NULL;
			GType target;
			g_object_get(entity, "target-type", &target_type, NULL);
			target = venture_entity_registry_lookup_any(venture_entity_registry_get_default(), target_type);
			identity_control = target != G_TYPE_INVALID &&
				venture_data_class_for_type(target) == VENTURE_DATA_CLASS_PERSONAL;
		}
	}
	if (!identity_control &&
	    !venture_tenant_service_check_operation(self, TRUE, NULL) &&
	    !venture_tenant_service_support_allows(self, venture_access_policy_get_actor(
	        venture_database_get_access_policy(self->database)), entity, TRUE, TRUE, NULL))
		return venture_tenant_service_check_operation(self, TRUE, error);
	if (venture_data_class_for_type(type) == VENTURE_DATA_CLASS_UNKNOWN)
		return tenant_fail(error, "record type has no hosted authority declaration");
	if (venture_data_class_for_type(type) == VENTURE_DATA_CLASS_PLATFORM)
		return tenant_fail(error, "platform records require operator maintenance");
	if (VENTURE_IS_TENANT_WORKSPACE(entity) || VENTURE_IS_TENANT_MEMBERSHIP(entity) ||
	    VENTURE_IS_TENANT_INVITATION(entity) || VENTURE_IS_TENANT_SUPPORT_GRANT(entity) ||
	    VENTURE_IS_TENANT_EVENT(entity))
		return tenant_fail(error, "hosted control records require a dedicated administrative operation");
	{
		g_autoptr(GPtrArray) fields = venture_entity_get_field_specs(entity);
		g_autoptr(VentureEntity) previous = NULL;
		g_autoptr(VentureAccessScope) internal = NULL;
		guint i;
		for (i = 0; i < fields->len; i++) {
			VentureFieldSpec *field = g_ptr_array_index(fields, i);
			g_auto(GValue) old_value = G_VALUE_INIT;
			g_auto(GValue) new_value = G_VALUE_INIT;
			GParamSpec *property;
			if (!(venture_field_spec_get_flags(field) & VENTURE_COLUMN_FLAG_HOST_RESOURCE)) continue;
			if (!previous && venture_entity_is_persisted(entity)) {
				internal = venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
				previous = venture_database_get(self->database, type, venture_entity_get_id(entity), error);
				if (!previous) return FALSE;
			}
			property = g_object_class_find_property(G_OBJECT_GET_CLASS(entity), field->name);
			g_value_init(&new_value, G_PARAM_SPEC_VALUE_TYPE(property));
			g_value_init(&old_value, G_PARAM_SPEC_VALUE_TYPE(property));
			g_object_get_property(G_OBJECT(entity), field->name, &new_value);
			if (previous) g_object_get_property(G_OBJECT(previous), field->name, &old_value);
			else g_param_value_set_default(property, &old_value);
			if (G_VALUE_HOLDS_STRING(&old_value) && venture_string_is_empty(g_value_get_string(&old_value)) &&
			    venture_string_is_empty(g_value_get_string(&new_value))) continue;
			if (g_param_values_cmp(property, &old_value, &new_value) != 0)
				return tenant_fail(error, "host resource fields require operator maintenance");
		}
	}
	if (VENTURE_IS_USER(entity)) {
		g_autoptr(VentureEntity) previous = NULL;
		g_autoptr(VentureAccessScope) internal = NULL;
		gint old_role, new_role;
		gboolean old_active, new_active;
		if (!venture_entity_is_persisted(entity)) return tenant_fail(error, "identities require explicit invitation or operator recovery");
		internal = venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
		previous = venture_database_get(self->database, type, venture_entity_get_id(entity), error);
		if (!previous) return FALSE;
		g_object_get(previous, "role", &old_role, "active", &old_active, NULL);
		g_object_get(entity, "role", &new_role, "active", &new_active, NULL);
		if (old_role != new_role || old_active != new_active || venture_entity_is_deleted(entity))
			return tenant_fail(error, "identity authority changes require tenant membership administration");
	}
	return TRUE;
}

static VentureEntity *
find_number(VentureTenantService *self, GType type, const gchar *field, gint64 value, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(
		venture_database_get_access_policy(self->database), NULL);
	venture_query_add_filter_int(query, field, VENTURE_FILTER_OP_EQ, value, NULL);
	venture_query_set_limit(query, 1);
	rows = venture_database_find(self->database, query, error);
	return rows && rows->len ? g_object_ref(g_ptr_array_index(rows, 0)) : NULL;
}

/* The write acquires a row lock on PostgreSQL, and its optimistic version
 * refuses a competing stale authority transaction before any cross-row edit. */
static gboolean
lock_administration(VentureTenantService *self, GError **error)
{
	g_autoptr(VentureEntity) workspace = workspace_read(self, error);
	gint64 sequence = 0;
	if (!workspace) return FALSE;
	g_object_get(workspace, "administration-sequence", &sequence, NULL);
	if (sequence == G_MAXINT64) return tenant_fail(error, "administration sequence is exhausted");
	g_object_set(workspace, "administration-sequence", sequence + 1, NULL);
	return venture_database_save(self->database, workspace, NULL, error);
}

static gboolean
record_event(VentureTenantService *self, const gchar *event, gint64 actor_id,
             gint64 grant_id, const gchar *resource, const gchar *reason, GError **error)
{
	g_autoptr(VentureEntity) row = g_object_new(VENTURE_TYPE_TENANT_EVENT,
		"event", event, "actor-user-id", actor_id, "grant-id", grant_id,
		"resource", resource, "reason", reason, NULL);
	return venture_database_save(self->database, row, NULL, error);
}

static gboolean
revoke_credentials(VentureTenantService *self, VentureEntity *user, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_API_TOKEN);
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GDateTime) now = g_date_time_new_now_utc();
	guint i;
	g_object_set(user, "sessions-invalidated-at", now, NULL);
	venture_query_add_filter_int(query, "user-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(user), NULL);
	venture_query_set_include_deleted(query, TRUE);
	rows = venture_database_find(self->database, query, error);
	if (!rows) return FALSE;
	for (i = 0; i < rows->len; i++) {
		VentureEntity *token = g_ptr_array_index(rows, i);
		g_object_set(token, "active", FALSE, NULL);
		if (!venture_database_save(self->database, token, NULL, error)) return FALSE;
	}
	return venture_database_save(self->database, user, NULL, error);
}

static gboolean
bootstrap_identity(VentureTenantService *self, VentureConfig *config,
	const gchar *username, const gchar *password, gboolean recover, gboolean administrator, const gchar *reason, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_USER);
	g_autoptr(GPtrArray) users = NULL, organizations = NULL;
	g_autoptr(VentureEntity) user = NULL, member = NULL;
	g_autoptr(VentureAccessScope) internal = NULL;
	gint64 iterations, minimum;
	gboolean ok = FALSE;
	guint i;
	if (!self->enabled || !self->initialized || !username || !*username ||
	    !password || !reason || !*reason || strlen(reason) > 1000)
		return tenant_fail(error, "operator bootstrap requires initialized hosted mode, username, password and bounded reason");
	if (venture_access_policy_get_actor(venture_database_get_access_policy(self->database)))
		return tenant_fail(error, "bootstrap is a local operator operation");
	g_object_get(config, "security-password-iterations", &iterations, "security-password-min-length", &minimum, NULL);
	if (!g_utf8_validate(password, -1, NULL) || g_utf8_strlen(password, -1) < minimum || strlen(password) > 4096)
		return tenant_fail(error, "password does not meet the configured policy");
	internal = venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
	self->maintenance_depth++;
	if (!venture_database_begin(self->database, error)) goto done;
	if (!lock_administration(self, error)) goto rollback;
	venture_query_add_filter_string(query, "username", VENTURE_FILTER_OP_EQ, username, NULL);
	users = venture_database_find(self->database, query, error);
	if (!users) goto rollback;
	if (users->len && !recover) {
		tenant_fail(error, "username already exists; explicit tenant recovery is required");
		goto rollback;
	}
	user = users->len ? g_object_ref(g_ptr_array_index(users, 0)) : g_object_new(VENTURE_TYPE_USER, "username", username, NULL);
	if (!administrator && venture_entity_is_persisted(user) &&
	    venture_tenant_service_is_member(self, venture_entity_get_id(user), FALSE)) {
		tenant_fail(error, "a tenant member cannot become a support operator"); goto rollback;
	}
	g_object_set(user, "role", administrator ? VENTURE_USER_ROLE_EDITOR : VENTURE_USER_ROLE_VIEWER, "active", TRUE, NULL);
	if (!venture_user_set_password(VENTURE_USER(user), password, (guint)iterations, error) ||
	    !venture_database_save(self->database, user, NULL, error)) goto rollback;
	if (recover && !revoke_credentials(self, user, error)) goto rollback;
	if (recover) {
		g_autoptr(GError) mfa_error = NULL;
		if (!venture_mfa_service_reset(venture_mfa_service_get(self->database), venture_entity_get_id(user), NULL, &mfa_error) &&
		    !g_error_matches(mfa_error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND)) {
			g_propagate_error(error, g_steal_pointer(&mfa_error)); goto rollback;
		}
	}
	if (!administrator) goto record_bootstrap;
	member = find_number(self, VENTURE_TYPE_TENANT_MEMBERSHIP, "user-id", venture_entity_get_id(user), error);
	if (error && *error) goto rollback;
	if (!member) member = g_object_new(VENTURE_TYPE_TENANT_MEMBERSHIP, "user-id", venture_entity_get_id(user), NULL);
	g_object_set(member, "name", username, "role", VENTURE_TENANT_ROLE_ADMIN, "active", TRUE, NULL);
	if (!venture_database_save(self->database, member, NULL, error)) goto rollback;
	g_clear_object(&query);
	query = venture_query_new(VENTURE_TYPE_ORGANIZATION);
	organizations = venture_database_find(self->database, query, error);
	if (!organizations) goto rollback;
	for (i = 0; i < organizations->len; i++) {
		g_autoptr(GPtrArray) memberships = NULL;
		g_autoptr(VentureEntity) organization_member = NULL;
		gint64 org = venture_entity_get_id(g_ptr_array_index(organizations, i));
		g_clear_object(&query);
		query = venture_query_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP);
		venture_query_set_organization(query, org);
		venture_query_add_filter_int(query, "user-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(user), NULL);
		memberships = venture_database_find(self->database, query, error);
		if (!memberships) goto rollback;
		organization_member = memberships->len ? g_object_ref(g_ptr_array_index(memberships, 0)) :
			g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP, "organization-id", org, "user-id", venture_entity_get_id(user), NULL);
		g_object_set(organization_member, "role", VENTURE_ORGANIZATION_ROLE_ADMIN, "active", TRUE, NULL);
		if (!venture_database_save(self->database, organization_member, NULL, error)) goto rollback;
	}
record_bootstrap:
	if (!record_event(self, administrator ? (recover ? "operator.recover_admin" : "operator.bootstrap_admin") :
	        (recover ? "operator.recover_support_identity" : "operator.bootstrap_support_identity"),
		0, 0, username, reason, error)) goto rollback;
	ok = venture_database_commit(self->database, error);
	goto done;
rollback:
	venture_database_rollback(self->database);
done:
	self->maintenance_depth--;
	return ok;
}

gboolean
venture_tenant_service_bootstrap_admin(VentureTenantService *self, VentureConfig *config,
	const gchar *username, const gchar *password, gboolean recover, const gchar *reason, GError **error)
{
	return bootstrap_identity(self, config, username, password, recover, TRUE, reason, error);
}

gboolean
venture_tenant_service_bootstrap_operator(VentureTenantService *self, VentureConfig *config,
	const gchar *username, const gchar *password, gboolean recover, const gchar *reason, GError **error)
{
	return bootstrap_identity(self, config, username, password, recover, FALSE, reason, error);
}

gboolean
venture_tenant_service_is_member(VentureTenantService *self, gint64 user_id, gboolean administrator)
{
	g_autoptr(VentureEntity) member = NULL;
	g_autofree gchar *role = NULL;
	gboolean active = FALSE;
	if (!self->enabled) return TRUE;
	if (!self->database || !self->initialized || user_id <= 0) return FALSE;
	member = find_number(self, VENTURE_TYPE_TENANT_MEMBERSHIP, "user-id", user_id, NULL);
	if (!member || venture_entity_is_deleted(member)) return FALSE;
	role = record_enum_nick(member, "role");
	g_object_get(member, "active", &active, NULL);
	return active && (g_strcmp0(role, "admin") == 0 || (!administrator && g_strcmp0(role, "member") == 0));
}

gboolean
venture_tenant_service_check_principal(VentureTenantService *self, const VentureAuthPrincipal *actor, GError **error)
{
	if (!self->enabled) return TRUE;
	if (self->support && self->support->grant_id > 0) {
		g_autoptr(VentureEntity) grant = support_grant_read(self, actor, error);
		return grant != NULL;
	}
	if (!actor || !actor->authenticated || actor->role == VENTURE_USER_ROLE_OWNER ||
	    actor->role == VENTURE_USER_ROLE_ADMIN || !venture_tenant_service_is_member(self, actor->user_id, FALSE))
		return tenant_fail(error, "active workspace membership is required; platform roles are not tenant authority");
	{
		g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
		g_autoptr(VentureEntity) user = venture_database_get(self->database, VENTURE_TYPE_USER, actor->user_id, NULL);
		gboolean active = FALSE;
		gint role = VENTURE_USER_ROLE_OWNER;
		if (user && !venture_entity_is_deleted(user)) g_object_get(user, "active", &active, "role", &role, NULL);
		if (!active || role == VENTURE_USER_ROLE_OWNER || role == VENTURE_USER_ROLE_ADMIN)
			return tenant_fail(error, "local identity was revoked or changed authority");
	}
	return TRUE;
}

struct _VentureTenantMaintenance {
	GObject parent_instance;
	VentureTenantService *service;
	VentureAccessScope *access;
	gchar *reason;
	gboolean finished;
	gboolean deferred_begin;
};
G_DEFINE_FINAL_TYPE(VentureTenantMaintenance, venture_tenant_maintenance, G_TYPE_OBJECT)

static void
maintenance_finalize(GObject *object)
{
	VentureTenantMaintenance *self = VENTURE_TENANT_MAINTENANCE(object);
	if (self->service && !self->finished) {
		/* The durable begin remains evidence even when a caller aborts. */
		if (self->service->database && self->service->enabled && self->service->initialized)
			record_event(self->service, "operator.maintenance_abandoned", 0, 0, "workspace", self->reason, NULL);
		self->service->maintenance_depth--;
	}
	g_clear_object(&self->access);
	g_clear_object(&self->service);
	g_free(self->reason);
	G_OBJECT_CLASS(venture_tenant_maintenance_parent_class)->finalize(object);
}

static void
venture_tenant_maintenance_class_init(VentureTenantMaintenanceClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = maintenance_finalize;
}

static void
venture_tenant_maintenance_init(VentureTenantMaintenance *self)
{
	(void)self;
}

static VentureTenantMaintenance *
enter_maintenance_full(VentureTenantService *self, const gchar *reason, gboolean initializing, GError **error)
{
	g_autoptr(VentureTenantMaintenance) scope = NULL;
	if (!reason || !*reason || strlen(reason) > 1000 ||
	    venture_access_policy_get_actor(venture_database_get_access_policy(self->database))) {
		tenant_fail(error, "maintenance requires a local operator and bounded reason");
		return NULL;
	}
	if (self->enabled && !self->initialized && !initializing) {
		tenant_fail(error, "maintenance requires a verified database identity");
		return NULL;
	}
	scope = g_object_new(VENTURE_TYPE_TENANT_MAINTENANCE, NULL);
	scope->service = g_object_ref(self);
	scope->reason = g_strdup(reason);
	scope->deferred_begin = !self->initialized;
	scope->access = venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
	self->maintenance_depth++;
	if (self->enabled && !scope->deferred_begin && !record_event(self, "operator.maintenance_begin", 0, 0, "workspace", reason, error)) return NULL;
	return g_steal_pointer(&scope);
}

VentureTenantMaintenance *
venture_tenant_service_enter_maintenance(VentureTenantService *self, const gchar *reason, GError **error)
{
	return enter_maintenance_full(self, reason, FALSE, error);
}

VentureTenantMaintenance *
venture_tenant_service_enter_migration(VentureTenantService *self, GError **error)
{
	if (!venture_tenant_service_verify_existing(self, error)) return NULL;
	return enter_maintenance_full(self, "Schema migration", TRUE, error);
}

gboolean
venture_tenant_maintenance_finish(VentureTenantMaintenance *self, GError **error)
{
	if (self->finished) return TRUE;
	if (!self->service->database) return tenant_fail(error, "maintenance repository is no longer available");
	if (self->service->enabled && self->deferred_begin &&
	    !record_event(self->service, "operator.initial_migration", 0, 0, "workspace", self->reason, error)) return FALSE;
	if (self->service->enabled && !record_event(self->service, "operator.maintenance_end", 0, 0, "workspace", self->reason, error)) return FALSE;
	self->service->maintenance_depth--;
	self->finished = TRUE;
	g_clear_object(&self->access);
	return TRUE;
}


gboolean
venture_tenant_service_check_resource(VentureTenantService *self, GObject *resource,
	gboolean write, GError **error)
{
	VentureDataClass classification;
	if (!self->enabled || self->maintenance_depth > 0) return TRUE;
	if (venture_tenant_service_get_support_organization(self) > 0)
		return tenant_fail(error, "support grants permit scoped repository access, not reports or actions");
	classification = venture_data_class_for_resource(resource);
	if (classification == VENTURE_DATA_CLASS_UNKNOWN && VENTURE_IS_ENTITY(resource))
		classification = venture_data_class_for_type(G_OBJECT_TYPE(resource));
	if (classification == VENTURE_DATA_CLASS_PERSONAL && self->support && self->support->control) return TRUE;
	if (classification == VENTURE_DATA_CLASS_TENANT_ADMIN) {
		const VentureAuthPrincipal *actor = venture_access_policy_get_actor(venture_database_get_access_policy(self->database));
		if (!actor || actor->token_id || !venture_tenant_service_check_principal(self, actor, error) ||
		    !venture_tenant_service_is_member(self, actor->user_id, TRUE)) {
			if (!error || !*error) tenant_fail(error, "interactive tenant administrator is required");
			return FALSE;
		}
		return TRUE;
	}
	if (!venture_tenant_service_check_operation(self, write, error)) return FALSE;
	if (classification == VENTURE_DATA_CLASS_UNKNOWN || classification == VENTURE_DATA_CLASS_PLATFORM)
		return tenant_fail(error, "resource has no tenant authority declaration");
	return TRUE;
}

static gboolean
require_administrator(VentureTenantService *self, const VentureAuthPrincipal **actor, GError **error)
{
	*actor = venture_access_policy_get_actor(venture_database_get_access_policy(self->database));
	if (!self->enabled || !self->initialized || !*actor || (*actor)->token_id != 0 ||
	    !venture_tenant_service_check_principal(self, *actor, error)) {
		if (!error || !*error) tenant_fail(error, "interactive tenant administration is required");
		return FALSE;
	}
	if (!venture_tenant_service_is_member(self, (*actor)->user_id, TRUE))
		return tenant_fail(error, "tenant administrator membership is required");
	return TRUE;
}

static gboolean
set_state_validated(VentureTenantService *self, const gchar *state,
	const gchar *reason, gint64 actor_id, GError **error)
{
	g_autoptr(VentureEntity) workspace = NULL;
	g_autoptr(VentureAccessScope) internal = NULL;
	gboolean ok = FALSE;
	if (g_strcmp0(state, "active") && g_strcmp0(state, "read_only") && g_strcmp0(state, "suspended"))
		return tenant_fail(error, "unknown lifecycle state");
	if (!reason || !*reason || strlen(reason) > 1000) return tenant_fail(error, "bounded lifecycle reason is required");
	internal = venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
	self->maintenance_depth++;
	if (!venture_database_begin(self->database, error)) goto done;
	if (!lock_administration(self, error)) goto rollback;
	if (actor_id > 0 && !venture_tenant_service_is_member(self, actor_id, TRUE)) {
		tenant_fail(error, "administrator membership changed"); goto rollback;
	}
	workspace = workspace_read(self, error);
	if (!workspace) goto rollback;
	{ gint state_value = VENTURE_TENANT_STATE_UNKNOWN;
	  venture_enum_from_nick(VENTURE_TYPE_TENANT_STATE, state, &state_value);
	  g_object_set(workspace, "state", state_value, "reason", reason, NULL); }
	if (!venture_database_save(self->database, workspace, NULL, error) ||
	    !record_event(self, actor_id > 0 ? "tenant.lifecycle" : "operator.lifecycle", actor_id, 0, state, reason, error)) goto rollback;
	ok = venture_database_commit(self->database, error);
	goto done;
rollback:
	venture_database_rollback(self->database);
done:
	self->maintenance_depth--;
	return ok;
}

gboolean
venture_tenant_service_set_state(VentureTenantService *self, const gchar *state,
	const gchar *reason, GError **error)
{
	const VentureAuthPrincipal *actor;
	if (!require_administrator(self, &actor, error)) return FALSE;
	return set_state_validated(self, state, reason, actor->user_id, error);
}

gboolean
venture_tenant_service_set_state_operator(VentureTenantService *self, const gchar *state,
	const gchar *reason, GError **error)
{
	g_autoptr(VentureTenantMaintenance) maintenance = NULL;
	if (!self->enabled || !self->initialized) return tenant_fail(error, "verified hosted mode is required");
	maintenance = venture_tenant_service_enter_maintenance(self, reason, error);
	if (!maintenance || !set_state_validated(self, state, reason, 0, error)) return FALSE;
	return venture_tenant_maintenance_finish(maintenance, error);
}

JsonNode *
venture_tenant_service_status(VentureTenantService *self, GError **error)
{
	g_autoptr(VentureEntity) workspace = NULL;
	g_autofree gchar *state = NULL;
	const VentureAuthPrincipal *actor = venture_access_policy_get_actor(venture_database_get_access_policy(self->database));
	JsonNode *node;
	JsonObject *object;
	if (!self->enabled || !self->initialized || (actor && !require_administrator(self, &actor, error))) {
		if (!error || !*error) tenant_fail(error, "workspace status requires verified operator or tenant administration");
		return NULL;
	}
	workspace = workspace_read(self, error);
	if (!workspace) return NULL;
	state = record_enum_nick(workspace, "state");
	object = json_object_new();
	json_object_set_string_member(object, "workspace_id", self->workspace_id);
	json_object_set_string_member(object, "origin", self->origin);
	json_object_set_string_member(object, "state", state ? state : "unknown");
	node = json_node_new(JSON_NODE_OBJECT);
	json_node_take_object(node, object);
	return node;
}

gboolean
venture_tenant_service_set_membership(VentureTenantService *self, gint64 user_id,
	const gchar *role, gboolean active, const gchar *reason, GError **error)
{
	const VentureAuthPrincipal *actor;
	g_autoptr(VentureEntity) member = NULL, user = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) administrators = NULL;
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autofree gchar *old_role = NULL;
	gboolean old_active = FALSE, ok = FALSE;
	gint64 actor_id;
	gint user_role;
	if ((g_strcmp0(role, "admin") && g_strcmp0(role, "member")) || !reason || !*reason || strlen(reason) > 1000)
		return tenant_fail(error, "valid tenant role and bounded reason are required");
	if (!require_administrator(self, &actor, error)) return FALSE;
	actor_id = actor->user_id;
	internal = venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
	self->maintenance_depth++;
	if (!venture_database_begin(self->database, error)) goto done;
	if (!lock_administration(self, error)) goto rollback;
	if (!venture_tenant_service_is_member(self, actor_id, TRUE)) {
		tenant_fail(error, "administrator membership changed"); goto rollback;
	}
	member = find_number(self, VENTURE_TYPE_TENANT_MEMBERSHIP, "user-id", user_id, error);
	if (!member) { if (!error || !*error) tenant_fail(error, "membership is unavailable"); goto rollback; }
	user = venture_database_get(self->database, VENTURE_TYPE_USER, user_id, error);
	if (!user || venture_entity_is_deleted(user)) goto rollback;
	if (active) {
		g_autofree gchar *password_hash = NULL;
		g_object_get(user, "password-hash", &password_hash, NULL);
		if (venture_string_is_empty(password_hash)) {
			tenant_fail(error, "quarantined credentials require explicit recovery before activation"); goto rollback;
		}
	}
	g_object_get(user, "role", &user_role, NULL);
	if (user_role == VENTURE_USER_ROLE_OWNER || user_role == VENTURE_USER_ROLE_ADMIN) {
		tenant_fail(error, "platform identities require explicit operator recovery"); goto rollback;
	}
	old_role = record_enum_nick(member, "role");
	g_object_get(member, "active", &old_active, NULL);
	if (old_active && g_strcmp0(old_role, "admin") == 0 && (!active || g_strcmp0(role, "admin"))) {
		query = venture_query_new(VENTURE_TYPE_TENANT_MEMBERSHIP);
		venture_query_add_filter_string(query, "role", VENTURE_FILTER_OP_EQ, "admin", NULL);
		venture_query_add_filter_string(query, "active", VENTURE_FILTER_OP_EQ, "true", NULL);
		administrators = venture_database_find(self->database, query, error);
		if (!administrators) goto rollback;
		if (administrators->len <= 1) { tenant_fail(error, "the last active tenant administrator cannot be removed"); goto rollback; }
	}
	g_object_set(member, "role", role_value(role), "active", active, NULL);
	g_object_set(user, "active", active, NULL);
	if (g_strcmp0(role, "admin") == 0) g_object_set(user, "role", VENTURE_USER_ROLE_EDITOR, NULL);
	if (!venture_database_save(self->database, member, NULL, error) || !revoke_credentials(self, user, error) ||
	    !record_event(self, "tenant.membership", actor_id, 0, role, reason, error)) goto rollback;
	ok = venture_database_commit(self->database, error);
	goto done;
rollback:
	venture_database_rollback(self->database);
done:
	self->maintenance_depth--;
	return ok;
}

static VentureTenantInvitation *
create_invitation(VentureTenantService *self, const gchar *role, gint64 organization_id,
	VentureOrganizationRole organization_role, guint lifetime_seconds, const gchar *reason,
	gint64 recovery_user_id, GError **error)
{
	const VentureAuthPrincipal *actor;
	g_autoptr(VentureEntity) user = NULL, membership = NULL, organization = NULL, recovery_user = NULL, recovery_member = NULL;
	g_autoptr(VentureTenantInvitation) invitation = NULL;
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(GDateTime) now = g_date_time_new_now_utc(), expires = NULL, issuer_generation = NULL;
	g_autofree gchar *capability = NULL, *hash = NULL;
	gint64 actor_id, recovery_version = 0;
	gboolean committed = FALSE;
	if ((g_strcmp0(role, "admin") && g_strcmp0(role, "member")) || (recovery_user_id == 0 && organization_id <= 0) ||
	    organization_role < VENTURE_ORGANIZATION_ROLE_VIEWER || organization_role > VENTURE_ORGANIZATION_ROLE_ACCOUNTANT ||
	    lifetime_seconds < 60 || lifetime_seconds > 604800 || !reason || !*reason || strlen(reason) > 1000)
		{ tenant_fail(error, "invitation requires explicit role, organization, bounded expiry and reason"); return NULL; }
	if (!require_administrator(self, &actor, error)) return NULL;
	actor_id = actor->user_id;
	internal = venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
	self->maintenance_depth++;
	if (!venture_database_begin(self->database, error)) goto done;
	if (!lock_administration(self, error)) goto rollback;
	if (!venture_tenant_service_is_member(self, actor_id, TRUE)) { tenant_fail(error, "issuing authority changed"); goto rollback; }
	user = venture_database_get(self->database, VENTURE_TYPE_USER, actor_id, error);
	membership = find_number(self, VENTURE_TYPE_TENANT_MEMBERSHIP, "user-id", actor_id, error);
	if (!user || !membership) goto rollback;
	if (recovery_user_id > 0) {
		g_autofree gchar *password_hash = NULL, *recovery_role = NULL;
		gboolean active = TRUE;
		gint global_role = VENTURE_USER_ROLE_OWNER;
		recovery_user = venture_database_get(self->database, VENTURE_TYPE_USER, recovery_user_id, error);
		recovery_member = find_number(self, VENTURE_TYPE_TENANT_MEMBERSHIP, "user-id", recovery_user_id, error);
		if (!recovery_user || !recovery_member || venture_entity_is_deleted(recovery_user)) goto rollback;
		g_object_get(recovery_user, "active", &active, "password-hash", &password_hash, "role", &global_role, NULL);
		recovery_role = record_enum_nick(recovery_member, "role");
		if (active || !venture_string_is_empty(password_hash) || g_strcmp0(recovery_role, "member") ||
		    global_role == VENTURE_USER_ROLE_OWNER || global_role == VENTURE_USER_ROLE_ADMIN) {
			tenant_fail(error, "targeted recovery requires a quarantined ordinary member"); goto rollback;
		}
		recovery_version = venture_entity_get_version(recovery_member);
	} else {
		organization = venture_database_get(self->database, VENTURE_TYPE_ORGANIZATION, organization_id, error);
		if (!organization || venture_entity_is_deleted(organization)) goto rollback;
	}
	g_object_get(user, "sessions-invalidated-at", &issuer_generation, NULL);
	capability = venture_generate_token(32);
	hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, capability, -1);
	expires = g_date_time_add_seconds(now, lifetime_seconds);
	invitation = g_object_new(VENTURE_TYPE_TENANT_INVITATION, "token-hash", hash, "role", role_value(role),
		"initial-organization-id", organization_id, "organization-role", organization_role,
		"recovery-user-id", recovery_user_id, "recovery-membership-version", recovery_version,
		"issuer-user-id", actor_id, "issuer-session-generation", issuer_generation,
		"issuer-membership-version", venture_entity_get_version(membership), "expires-at", expires, NULL);
	if (!venture_database_save(self->database, VENTURE_ENTITY(invitation), NULL, error) ||
	    !record_event(self, recovery_user_id > 0 ? "tenant.invite_recovery" : "tenant.invite", actor_id, 0, role, reason, error)) goto rollback;
	committed = venture_database_commit(self->database, error);
	goto done;
rollback:
	venture_database_rollback(self->database);
done:
	self->maintenance_depth--;
	if (!committed) { if (!error || !*error) tenant_fail(error, "invitation target is unavailable"); return NULL; }
	g_object_set(invitation, "capability", capability, NULL);
	return g_steal_pointer(&invitation);
}


VentureTenantInvitation *
venture_tenant_service_invite(VentureTenantService *self, const gchar *role, gint64 organization_id,
	VentureOrganizationRole organization_role, guint lifetime_seconds, const gchar *reason, GError **error)
{
	return create_invitation(self, role, organization_id, organization_role, lifetime_seconds, reason, 0, error);
}

VentureTenantInvitation *
venture_tenant_service_invite_recovery(VentureTenantService *self, gint64 user_id,
	guint lifetime_seconds, const gchar *reason, GError **error)
{
	if (user_id <= 0) { tenant_fail(error, "recovery requires an explicit retained identity"); return NULL; }
	return create_invitation(self, "member", 0, VENTURE_ORGANIZATION_ROLE_VIEWER,
		lifetime_seconds, reason, user_id, error);
}

static VentureEntity *
invitation_for_capability(VentureTenantService *self, const gchar *capability, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_TENANT_INVITATION);
	g_autoptr(GPtrArray) rows = NULL;
	g_autofree gchar *hash = NULL;
	if (!capability || strlen(capability) < 32 || strlen(capability) > 256) return NULL;
	hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, capability, -1);
	venture_query_add_filter_string(query, "token-hash", VENTURE_FILTER_OP_EQ, hash, NULL);
	venture_query_set_limit(query, 1);
	rows = venture_database_find(self->database, query, error);
	return rows && rows->len ? g_object_ref(g_ptr_array_index(rows, 0)) : NULL;
}

VentureUser *
venture_tenant_service_accept_invitation(VentureTenantService *self, VentureConfig *config,
	const gchar *capability, const gchar *new_username, const gchar *new_password, GError **error)
{
	const VentureAuthPrincipal *actor = venture_access_policy_get_actor(venture_database_get_access_policy(self->database));
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureEntity) invitation = NULL, issuer = NULL, issuer_member = NULL, member = NULL, organization = NULL, org_member = NULL;
	g_autoptr(VentureUser) user = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GDateTime) now = g_date_time_new_now_utc(), expires = NULL, consumed = NULL;
	g_autoptr(GDateTime) issuer_generation = NULL, current_generation = NULL;
	g_autofree gchar *role = NULL, *old_role = NULL, *accepted_username = NULL;
	gint64 issuer_id = 0, authority_version = 0, org_id = 0, user_id = 0, minimum = 0, iterations = 0;
	gint64 recovery_user_id = 0, recovery_version = 0;
	gint org_role = 0, user_role = VENTURE_USER_ROLE_EDITOR;
	gboolean existing = actor && actor->authenticated, committed = FALSE, user_active = FALSE;
	if (!self->enabled || !self->initialized) { tenant_fail(error, "verified hosted workspace is required"); return NULL; }
	if (existing) {
		if (actor->token_id || new_username || new_password || actor->role == VENTURE_USER_ROLE_OWNER || actor->role == VENTURE_USER_ROLE_ADMIN)
			{ tenant_fail(error, "existing identity acceptance requires its own interactive session"); return NULL; }
		user_id = actor->user_id;
	} else {
		g_object_get(config, "security-password-min-length", &minimum, "security-password-iterations", &iterations, NULL);
		if (!new_username || !*new_username || strlen(new_username) > 128 || !new_password ||
		    !g_utf8_validate(new_password, -1, NULL) || g_utf8_strlen(new_password, -1) < minimum || strlen(new_password) > 4096)
			{ tenant_fail(error, "new identity requires explicit username and a policy-compliant password"); return NULL; }
	}
	internal = venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
	self->maintenance_depth++;
	if (!venture_database_begin(self->database, error)) goto done;
	if (!lock_administration(self, error)) goto rollback;
	invitation = invitation_for_capability(self, capability, error);
	if (!invitation) goto unavailable;
	role = record_enum_nick(invitation, "role");
	g_object_get(invitation, "expires-at", &expires, "consumed-at", &consumed,
		"issuer-user-id", &issuer_id, "issuer-session-generation", &issuer_generation,
		"issuer-membership-version", &authority_version, "initial-organization-id", &org_id,
		"organization-role", &org_role, "recovery-user-id", &recovery_user_id,
		"recovery-membership-version", &recovery_version, NULL);
	if ((g_strcmp0(role, "admin") && g_strcmp0(role, "member")) || !expires || consumed || g_date_time_compare(expires, now) <= 0 || !venture_tenant_service_is_member(self, issuer_id, TRUE)) goto unavailable;
	issuer = venture_database_get(self->database, VENTURE_TYPE_USER, issuer_id, error);
	issuer_member = find_number(self, VENTURE_TYPE_TENANT_MEMBERSHIP, "user-id", issuer_id, error);
	if (!issuer || !issuer_member || venture_entity_get_version(issuer_member) != authority_version) goto unavailable;
	if (recovery_user_id == 0) {
		/* The internal write scope cannot be used to bypass the lifecycle for
		 * ordinary invitations; only pinned quarantined identities recover paused. */
		self->maintenance_depth--;
		user_active = venture_tenant_service_check_operation(self, TRUE, error);
		self->maintenance_depth++;
		if (!user_active) goto rollback;
		organization = venture_database_get(self->database, VENTURE_TYPE_ORGANIZATION, org_id, error);
		if (!organization || venture_entity_is_deleted(organization)) goto unavailable;
	}
	g_object_get(issuer, "sessions-invalidated-at", &current_generation, "active", &user_active, NULL);
	if (!user_active || ((issuer_generation == NULL) != (current_generation == NULL)) ||
	    (issuer_generation && !g_date_time_equal(issuer_generation, current_generation))) goto unavailable;
	if (recovery_user_id > 0) {
		g_autofree gchar *stored_name = NULL, *password_hash = NULL, *reviewed_role = NULL;
		g_autoptr(GError) mfa_error = NULL;
		if (existing || g_strcmp0(role, "member")) goto unavailable;
		user_id = recovery_user_id;
		user = VENTURE_USER(venture_database_get(self->database, VENTURE_TYPE_USER, user_id, error));
		member = find_number(self, VENTURE_TYPE_TENANT_MEMBERSHIP, "user-id", user_id, error);
		if (!user || !member || venture_entity_is_deleted(VENTURE_ENTITY(user)) ||
		    venture_entity_get_version(member) != recovery_version) goto unavailable;
		g_object_get(user, "username", &stored_name, "active", &user_active, "password-hash", &password_hash, "role", &user_role, NULL);
		reviewed_role = record_enum_nick(member, "role");
		if (g_strcmp0(stored_name, new_username) || user_active || !venture_string_is_empty(password_hash) ||
		    g_strcmp0(reviewed_role, "member") || user_role == VENTURE_USER_ROLE_OWNER || user_role == VENTURE_USER_ROLE_ADMIN) goto unavailable;
		g_object_set(user, "active", TRUE, NULL);
		if (!venture_user_set_password(user, new_password, (guint)iterations, error) ||
		    !revoke_credentials(self, VENTURE_ENTITY(user), error)) goto rollback;
		if (!venture_mfa_service_reset(venture_mfa_service_get(self->database), user_id, NULL, &mfa_error) &&
		    !g_error_matches(mfa_error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND)) {
			g_propagate_error(error, g_steal_pointer(&mfa_error)); goto rollback;
		}
	} else if (existing) {
		user = VENTURE_USER(venture_database_get(self->database, VENTURE_TYPE_USER, user_id, error));
		if (!user || venture_entity_is_deleted(VENTURE_ENTITY(user))) goto unavailable;
		g_object_get(user, "active", &user_active, "role", &user_role, NULL);
		if (!user_active || user_role == VENTURE_USER_ROLE_OWNER || user_role == VENTURE_USER_ROLE_ADMIN) goto unavailable;
	} else {
		query = venture_query_new(VENTURE_TYPE_USER);
		venture_query_set_include_deleted(query, TRUE);
		venture_query_add_filter_string(query, "username", VENTURE_FILTER_OP_EQ, new_username, NULL);
		rows = venture_database_find(self->database, query, error);
		if (!rows || rows->len) goto unavailable;
		user = g_object_new(VENTURE_TYPE_USER, "username", new_username, "role", VENTURE_USER_ROLE_EDITOR, "active", TRUE, NULL);
		if (!venture_user_set_password(user, new_password, (guint)iterations, error) ||
		    !venture_database_save(self->database, VENTURE_ENTITY(user), NULL, error)) goto rollback;
		user_id = venture_entity_get_id(VENTURE_ENTITY(user));
	}
	if (!member) member = find_number(self, VENTURE_TYPE_TENANT_MEMBERSHIP, "user-id", user_id, error);
	if (error && *error) goto rollback;
	if (member) old_role = record_enum_nick(member, "role");
	else member = g_object_new(VENTURE_TYPE_TENANT_MEMBERSHIP, "user-id", user_id, NULL);
	g_object_get(user, "username", &accepted_username, NULL);
	g_object_set(member, "name", accepted_username, "role", g_strcmp0(old_role, "admin") == 0 ? VENTURE_TENANT_ROLE_ADMIN : role_value(role), "active", TRUE, NULL);
	if (recovery_user_id == 0) {
		g_clear_object(&query); g_clear_pointer(&rows, g_ptr_array_unref);
		query = venture_query_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP);
		venture_query_set_organization(query, org_id);
		venture_query_add_filter_int(query, "user-id", VENTURE_FILTER_OP_EQ, user_id, NULL);
		rows = venture_database_find(self->database, query, error);
		if (!rows) goto rollback;
		org_member = rows->len ? g_object_ref(g_ptr_array_index(rows, 0)) :
			g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP, "user-id", user_id, "organization-id", org_id, NULL);
		/* Accepting an invitation never downgrades an existing organization role. */
		if (!rows->len) g_object_set(org_member, "role", org_role, NULL);
		g_object_set(org_member, "active", TRUE, NULL);
	}
	g_object_set(invitation, "consumed-at", now, "consumed-user-id", user_id, NULL);
	if (existing) {
		if (g_strcmp0(role, "admin") == 0) g_object_set(user, "role", VENTURE_USER_ROLE_EDITOR, NULL);
		if (!revoke_credentials(self, VENTURE_ENTITY(user), error)) goto rollback;
	}
	if (!venture_database_save(self->database, member, NULL, error) ||
	    (org_member && !venture_database_save(self->database, org_member, NULL, error)) ||
	    !venture_database_save(self->database, invitation, NULL, error) ||
	    !record_event(self, recovery_user_id > 0 ? "tenant.accept_recovery" : "tenant.accept_invitation", user_id, 0, "membership", "Explicit capability acceptance", error)) goto rollback;
	committed = venture_database_commit(self->database, error);
	goto done;
unavailable:
	if (!error || !*error) tenant_fail(error, "invitation or explicit identity is unavailable");
rollback:
	venture_database_rollback(self->database);
done:
	self->maintenance_depth--;
	return committed ? g_steal_pointer(&user) : NULL;
}

G_DEFINE_FINAL_TYPE(VentureTenantSupportScope, venture_tenant_support_scope, G_TYPE_OBJECT)

static void
support_scope_finalize(GObject *object)
{
	VentureTenantSupportScope *self = VENTURE_TENANT_SUPPORT_SCOPE(object);
	if (self->service && self->service->support == self) {
		self->service->support = self->previous;
		self->service->maintenance_depth = self->previous_maintenance_depth;
	}
	g_clear_object(&self->previous);
	g_clear_object(&self->service);
	G_OBJECT_CLASS(venture_tenant_support_scope_parent_class)->finalize(object);
}

static void
venture_tenant_support_scope_class_init(VentureTenantSupportScopeClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = support_scope_finalize;
}

static void
venture_tenant_support_scope_init(VentureTenantSupportScope *self)
{
	(void)self;
}

static VentureEntity *
support_grant_read(VentureTenantService *self, const VentureAuthPrincipal *actor, GError **error)
{
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureEntity) grant = NULL, user = NULL;
	g_autoptr(GDateTime) now = g_date_time_new_now_utc(), expires = NULL;
	gboolean revoked = TRUE, active = FALSE;
	gint64 owner = 0;
	if (!self->support || !self->support->grant_id || !actor || !actor->authenticated || actor->token_id != 0 ||
	    self->support->operator_id != actor->user_id) goto refused;
	internal = venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
	grant = venture_database_get(self->database, VENTURE_TYPE_TENANT_SUPPORT_GRANT, self->support->grant_id, error);
	if (!grant || venture_entity_is_deleted(grant)) goto refused;
	g_object_get(grant, "owner-user-id", &owner, "expires-at", &expires, "revoked", &revoked, NULL);
	if (owner != actor->user_id || !expires || g_date_time_compare(expires, now) <= 0 || revoked ||
	    venture_entity_get_organization_id(grant) != self->support->organization_id) goto refused;
	user = venture_database_get(self->database, VENTURE_TYPE_USER, owner, error);
	if (!user || venture_entity_is_deleted(user)) goto refused;
	g_object_get(user, "active", &active, NULL);
	if (!active || venture_tenant_service_is_member(self, owner, FALSE)) goto refused;
	return g_steal_pointer(&grant);
refused:
	if (!error || !*error) tenant_fail(error, "support authority is missing, expired or revoked");
	return NULL;
}

VentureTenantSupportGrant *
venture_tenant_service_create_support_grant(VentureTenantService *self, gint64 operator_user_id,
	gint64 organization_id, gboolean allow_write, gboolean include_private, gboolean emergency,
	guint lifetime_seconds, const gchar *reason, GError **error)
{
	g_autoptr(VentureTenantMaintenance) maintenance = NULL;
	g_autoptr(VentureTenantSupportGrant) grant = NULL;
	g_autoptr(VentureEntity) user = NULL, organization = NULL;
	g_autoptr(GDateTime) now = g_date_time_new_now_utc(), expires = NULL;
	g_autofree gchar *capability = NULL, *hash = NULL;
	gboolean active = FALSE, committed = FALSE;
	if (!self->enabled || !self->initialized || lifetime_seconds < 60 ||
	    lifetime_seconds > (emergency ? 900u : 3600u) || operator_user_id <= 0 || organization_id <= 0) {
		tenant_fail(error, "support requires a verified identity, organization and bounded expiry"); return NULL;
	}
	maintenance = venture_tenant_service_enter_maintenance(self, reason, error);
	if (!maintenance) return NULL;
	if (!venture_database_begin(self->database, error)) return NULL;
	if (!lock_administration(self, error)) goto rollback;
	user = venture_database_get(self->database, VENTURE_TYPE_USER, operator_user_id, error);
	organization = venture_database_get(self->database, VENTURE_TYPE_ORGANIZATION, organization_id, error);
	if (!user || !organization || venture_entity_is_deleted(user) || venture_entity_is_deleted(organization)) goto unavailable;
	g_object_get(user, "active", &active, NULL);
	if (!active || venture_tenant_service_is_member(self, operator_user_id, FALSE)) goto unavailable;
	capability = venture_generate_token(32);
	hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, capability, -1);
	expires = g_date_time_add_seconds(now, lifetime_seconds);
	grant = g_object_new(VENTURE_TYPE_TENANT_SUPPORT_GRANT, "organization-id", organization_id,
		"owner-user-id", operator_user_id, "token-hash", hash, "allow-write", allow_write,
		"include-private", include_private, "emergency", emergency, "reason", reason, "expires-at", expires, NULL);
	if (!venture_database_save(self->database, VENTURE_ENTITY(grant), NULL, error) ||
	    !record_event(self, emergency ? "operator.emergency_support" : "operator.support_grant", 0,
		venture_entity_get_id(VENTURE_ENTITY(grant)), "organization", reason, error)) goto rollback;
	committed = venture_database_commit(self->database, error);
	if (!committed || !venture_tenant_maintenance_finish(maintenance, error)) return NULL;
	g_object_set(grant, "capability", capability, NULL);
	return g_steal_pointer(&grant);
unavailable:
	if (!error || !*error) tenant_fail(error, "support operator and organization must be live; operator cannot be a tenant member");
rollback:
	venture_database_rollback(self->database);
	return NULL;
}

VentureTenantSupportScope *
venture_tenant_service_enter_request(VentureTenantService *self, const VentureAuthPrincipal *actor,
	const gchar *capability, const gchar *resource, GError **error)
{
	g_autoptr(VentureTenantSupportScope) scope = g_object_new(VENTURE_TYPE_TENANT_SUPPORT_SCOPE, NULL);
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) grants = NULL;
	g_autoptr(VentureEntity) verified = NULL;
	g_autofree gchar *hash = NULL, *reason = NULL;
	gboolean recorded;
	scope->service = g_object_ref(self);
	scope->previous = self->support ? g_object_ref(self->support) : NULL;
	scope->previous_maintenance_depth = self->maintenance_depth;
	/* A nested request is never part of an enclosing local maintenance
	 * operation, even if a signal handler drove a nested main loop. */
	self->maintenance_depth = 0;
	self->support = scope;
	if (!self->enabled || !capability || !*capability) return g_steal_pointer(&scope);
	if (!actor || !actor->authenticated || actor->token_id || strlen(capability) < 32 || strlen(capability) > 256 ||
	    !resource || strlen(resource) > 2048 || strchr(resource, '?')) {
		tenant_fail(error, "support requires an authenticated named operator and bounded capability"); return NULL;
	}
	internal = venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
	hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, capability, -1);
	query = venture_query_new(VENTURE_TYPE_TENANT_SUPPORT_GRANT);
	venture_query_add_filter_string(query, "token-hash", VENTURE_FILTER_OP_EQ, hash, NULL);
	venture_query_set_limit(query, 1);
	grants = venture_database_find(self->database, query, error);
	if (!grants || !grants->len) { if (!error || !*error) tenant_fail(error, "support capability is unavailable"); return NULL; }
	scope->grant_id = venture_entity_get_id(g_ptr_array_index(grants, 0));
	scope->organization_id = venture_entity_get_organization_id(g_ptr_array_index(grants, 0));
	scope->operator_id = actor->user_id;
	verified = support_grant_read(self, actor, error);
	if (!verified) return NULL;
	g_object_get(verified, "reason", &reason, NULL);
	/* Only the admission audit may bypass suspension here. No business
	 * operation inherits a maintenance scope from the support capability. */
	self->maintenance_depth++;
	recorded = record_event(self, "support.request", actor->user_id, scope->grant_id, resource, reason, error);
	self->maintenance_depth--;
	return recorded ? g_steal_pointer(&scope) : NULL;
}

gint64
venture_tenant_service_get_support_organization(VentureTenantService *self)
{
	return self->support ? self->support->organization_id : 0;
}

gboolean
venture_tenant_service_support_allows(VentureTenantService *self, const VentureAuthPrincipal *actor,
	VentureEntity *entity, gboolean write, gboolean require_emergency, GError **error)
{
	g_autoptr(VentureEntity) grant = support_grant_read(self, actor, error);
	gboolean allow_write = FALSE, include_private = FALSE, emergency = FALSE;
	gint64 organization_id;
	VentureDataClass classification;
	if (!grant) return FALSE;
	g_object_get(grant, "allow-write", &allow_write, "include-private", &include_private, "emergency", &emergency, NULL);
	organization_id = VENTURE_IS_ORGANIZATION(entity) ? venture_entity_get_id(entity) : venture_entity_get_organization_id(entity);
	classification = venture_data_class_for_type(G_OBJECT_TYPE(entity));
	if ((write && !allow_write) || (require_emergency && !emergency) || organization_id <= 0 ||
	    organization_id != venture_entity_get_organization_id(grant) ||
	    (classification != VENTURE_DATA_CLASS_TENANT && classification != VENTURE_DATA_CLASS_PERSONAL) ||
	    ((classification == VENTURE_DATA_CLASS_PERSONAL || venture_access_policy_record_is_personal(venture_database_get_access_policy(self->database), entity)) &&
	     (!include_private || venture_access_policy_get_personal_owner(venture_database_get_access_policy(self->database), entity) <= 0)))
		return tenant_fail(error, "support grant does not authorize this record and operation");
	if (!emergency && !venture_tenant_service_check_operation(self, write, error)) return FALSE;
	return TRUE;
}

gboolean
venture_tenant_service_revoke_support(VentureTenantService *self, gint64 grant_id, const gchar *reason, GError **error)
{
	const VentureAuthPrincipal *actor;
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureEntity) grant = NULL;
	gint64 actor_id;
	gboolean ok = FALSE;
	if (!reason || !*reason || strlen(reason) > 1000 || !require_administrator(self, &actor, error)) {
		if (!error || !*error) tenant_fail(error, "support revocation requires a bounded reason");
		return FALSE;
	}
	actor_id = actor->user_id;
	internal = venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
	self->maintenance_depth++;
	if (!venture_database_begin(self->database, error)) goto done;
	if (!lock_administration(self, error)) goto rollback;
	if (!venture_tenant_service_is_member(self, actor_id, TRUE)) { tenant_fail(error, "administrator membership changed"); goto rollback; }
	grant = venture_database_get(self->database, VENTURE_TYPE_TENANT_SUPPORT_GRANT, grant_id, error);
	if (!grant || venture_entity_is_deleted(grant)) goto rollback;
	g_object_set(grant, "revoked", TRUE, NULL);
	if (!venture_database_save(self->database, grant, NULL, error) ||
	    !record_event(self, "tenant.revoke_support", actor_id, grant_id, "support", reason, error)) goto rollback;
	ok = venture_database_commit(self->database, error);
	goto done;
rollback:
	venture_database_rollback(self->database);
done:
	self->maintenance_depth--;
	return ok;
}


gboolean
venture_tenant_service_check_support_request(VentureTenantService *self,
	const VentureAuthPrincipal *actor, gboolean write, GError **error)
{
	g_autoptr(VentureEntity) grant = support_grant_read(self, actor, error);
	gboolean allow_write = FALSE, emergency = FALSE;
	if (!grant) return FALSE;
	g_object_get(grant, "allow-write", &allow_write, "emergency", &emergency, NULL);
	if (write && !allow_write) return tenant_fail(error, "support grant is read-only");
	return emergency || venture_tenant_service_check_operation(self, write, error);
}


void
venture_tenant_support_scope_set_control(VentureTenantSupportScope *self, gboolean control)
{
	self->control = control;
}

gboolean
venture_tenant_service_check_provider(VentureTenantService *self, const gchar *provider, GError **error)
{
	if (!self->enabled || self->maintenance_depth > 0) return TRUE;
	if (venture_tenant_service_get_support_organization(self) > 0)
		return tenant_fail(error, "support record access does not authorize external providers");
	/* Interactive identity verification remains available for reactivation.
	 * This exception is neither ambient credentials nor business-provider work. */
	if (self->support && self->support->control && g_strcmp0(provider, "oidc") == 0) return self->initialized;
	return venture_tenant_service_check_operation(self, TRUE, error);
}

gboolean
venture_tenant_service_is_maintenance(VentureTenantService *self)
{
	return self->database && self->enabled && self->initialized && self->maintenance_depth > 0;
}

gboolean
venture_tenant_service_revoke_credentials(VentureTenantService *self, const gchar *reason, GError **error)
{
	g_autoptr(VentureTenantMaintenance) maintenance = NULL;
	g_autoptr(VentureEntity) workspace = NULL;
	g_autoptr(GDateTime) now = g_date_time_new_now_utc();
	GType types[] = { VENTURE_TYPE_USER, VENTURE_TYPE_API_TOKEN, VENTURE_TYPE_TENANT_MEMBERSHIP,
		VENTURE_TYPE_TENANT_INVITATION, VENTURE_TYPE_TENANT_SUPPORT_GRANT };
	guint t, i;
	if (!self->enabled || !self->initialized) return tenant_fail(error, "restore quarantine requires a verified hosted workspace");
	maintenance = venture_tenant_service_enter_maintenance(self, reason, error);
	if (!maintenance) return FALSE;
	if (!venture_database_begin(self->database, error)) return FALSE;
	if (!lock_administration(self, error)) goto rollback;
	workspace = workspace_read(self, error);
	if (!workspace) goto rollback;
	g_object_set(workspace, "state", VENTURE_TENANT_STATE_SUSPENDED, "reason", reason, NULL);
	if (!venture_database_save(self->database, workspace, NULL, error)) goto rollback;
	for (t = 0; t < G_N_ELEMENTS(types); t++) {
		g_autoptr(VentureQuery) query = venture_query_new(types[t]);
		g_autoptr(GPtrArray) rows = NULL;
		venture_query_set_include_deleted(query, TRUE);
		rows = venture_database_find(self->database, query, error);
		if (!rows) goto rollback;
		for (i = 0; i < rows->len; i++) {
			VentureEntity *row = g_ptr_array_index(rows, i);
			if (VENTURE_IS_USER(row)) {
				/* Clearing the verifier matters: merely disabling the row would
				 * let a membership toggle revive a password revoked after backup. */
				g_object_set(row, "active", FALSE, "password-hash", NULL, "sessions-invalidated-at", now, NULL);
			} else if (VENTURE_IS_API_TOKEN(row) || VENTURE_IS_TENANT_MEMBERSHIP(row)) g_object_set(row, "active", FALSE, NULL);
			else if (VENTURE_IS_TENANT_INVITATION(row)) g_object_set(row, "expires-at", now, NULL);
			else g_object_set(row, "revoked", TRUE, NULL);
			if (!venture_database_save(self->database, row, NULL, error)) goto rollback;
		}
	}
	if (!venture_oidc_service_quarantine(venture_oidc_service_get(self->database), error) ||
	    !record_event(self, "operator.restore_quarantine", 0, 0, "workspace", reason, error)) goto rollback;
	if (!venture_database_commit(self->database, error)) return FALSE;
	return venture_tenant_maintenance_finish(maintenance, error);
rollback:
	venture_database_rollback(self->database);
	return FALSE;
}
