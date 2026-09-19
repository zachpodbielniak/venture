/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

struct _VentureAccessPolicy
{
	GObject parent_instance;
	VentureDatabase *database;
	const VentureAuthPrincipal *actor;
	guint decide_signal;
	const gchar *read_action;
};
struct _VentureAccessScope
{
	GObject parent_instance;
	VentureAccessPolicy *policy;
	const VentureAuthPrincipal *previous;
	VentureAuthPrincipal *actor;
	const gchar *previous_action;
};
G_DEFINE_FINAL_TYPE(VentureAccessPolicy, venture_access_policy, G_TYPE_OBJECT)
G_DEFINE_FINAL_TYPE(VentureAccessScope, venture_access_scope, G_TYPE_OBJECT)

enum { PROP_0, PROP_DATABASE };

static void
policy_set_property(GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
	VentureAccessPolicy *self = VENTURE_ACCESS_POLICY(object);
	if (PROP_DATABASE == id)
	{
		self->database = g_value_get_object(value);
		g_object_add_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	}
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}
static void
policy_get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
	if (PROP_DATABASE == id)
		g_value_set_object(value, VENTURE_ACCESS_POLICY(object)->database);
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}
static void
policy_finalize(GObject *object)
{
	VentureAccessPolicy *self = VENTURE_ACCESS_POLICY(object);
	if (NULL != self->database)
		g_object_remove_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	G_OBJECT_CLASS(venture_access_policy_parent_class)->finalize(object);
}
static gboolean
first_veto(GSignalInvocationHint *hint, GValue *accumulator, const GValue *value, gpointer data)
{
	if (NULL == g_value_get_boxed(value))
		return TRUE;
	g_value_set_boxed(accumulator, g_value_get_boxed(value));
	return FALSE;
}
static void
venture_access_policy_class_init(VentureAccessPolicyClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	object_class->set_property = policy_set_property;
	object_class->get_property = policy_get_property;
	object_class->finalize = policy_finalize;
	g_object_class_install_property(object_class, PROP_DATABASE,
		g_param_spec_object("database", "Database", "Repository whose records are protected",
			VENTURE_TYPE_DATABASE, G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
	/**
	 * VentureAccessPolicy::decide:
	 * @self: policy
	 * @actor: principal, borrowed for this emission
	 * @action: requested operation
	 * @entity: candidate record
	 *
	 * Emitted only after built-in checks pass. RUN_LAST; the first owned
	 * GError returned vetoes access. A plugin can restrict, never grant it.
	 * Returns: (transfer full) (nullable): a veto, or NULL
	 */
	g_signal_new("decide", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
		0, first_veto, NULL, NULL, G_TYPE_ERROR, 3,
		G_TYPE_POINTER, G_TYPE_STRING, VENTURE_TYPE_ENTITY);
}
static void
venture_access_policy_init(VentureAccessPolicy *self)
{
	self->decide_signal = g_signal_lookup("decide", VENTURE_TYPE_ACCESS_POLICY);
	self->read_action = "read";
}
static void
scope_finalize(GObject *object)
{
	VentureAccessScope *self = VENTURE_ACCESS_SCOPE(object);
	self->policy->actor = self->previous;
	self->policy->read_action = self->previous_action;
	g_clear_pointer(&self->actor, venture_auth_principal_free);
	g_clear_object(&self->policy);
	G_OBJECT_CLASS(venture_access_scope_parent_class)->finalize(object);
}
static void
venture_access_scope_class_init(VentureAccessScopeClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = scope_finalize;
}
static void
venture_access_scope_init(VentureAccessScope *self)
{
}
VentureAccessPolicy *
venture_access_policy_new(VentureDatabase *database)
{
	g_autoptr(VentureModuleRegistry) modules = venture_module_registry_new();
	venture_module_registry_register_builtins(modules);
	return g_object_new(VENTURE_TYPE_ACCESS_POLICY, "database", database, NULL);
}
VentureAccessScope *
venture_access_policy_enter(VentureAccessPolicy *self, const VentureAuthPrincipal *actor)
{
	VentureAccessScope *scope = g_object_new(VENTURE_TYPE_ACCESS_SCOPE, NULL);
	scope->policy = g_object_ref(self);
	scope->previous = self->actor;
	scope->previous_action = self->read_action;
	self->read_action = "read";
	if (NULL != actor)
	{
		scope->actor = g_new(VentureAuthPrincipal, 1);
		*scope->actor = *actor;
		scope->actor->name = g_strdup(actor->name);
	}
	self->actor = scope->actor;
	return scope;
}
const VentureAuthPrincipal *
venture_access_policy_get_actor(VentureAccessPolicy *self)
{
	return self->actor;
}
static gboolean
administrator(const VentureAuthPrincipal *actor)
{
	return NULL != actor && actor->authenticated &&
		(actor->role == VENTURE_USER_ROLE_OWNER || actor->role == VENTURE_USER_ROLE_ADMIN);
}
gboolean
venture_access_policy_is_administrator(const VentureAuthPrincipal *actor)
{
	return administrator(actor);
}
static gint
token_role(VentureAccessPolicy *self, const VentureAuthPrincipal *actor, gint64 org)
{
	g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(self, NULL);
	g_autoptr(VentureEntity) token = NULL;
	g_autofree gchar *snapshot = NULL;
	g_autofree gchar *key = NULL;
	g_autoptr(JsonNode) node = NULL;
	if (actor->token_id <= 0)
		return -1;
	token = venture_database_get(self->database, VENTURE_TYPE_API_TOKEN, actor->token_id, NULL);
	if (NULL == token)
		return -1;
	g_object_get(token, "membership-snapshot", &snapshot, NULL);
	if (NULL == snapshot)
		return -1;
	node = venture_json_parse(snapshot, NULL);
	key = g_strdup_printf("%" G_GINT64_FORMAT, org);
	if (NULL == node || !JSON_NODE_HOLDS_OBJECT(node) || !json_object_has_member(json_node_get_object(node), key))
		return -1;
	return (gint)json_object_get_int_member(json_node_get_object(node), key);
}
static VentureEntity *
membership(VentureAccessPolicy *self, const VentureAuthPrincipal *actor, gint64 org)
{
	g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(self, NULL);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP);
	if (NULL == actor || !actor->authenticated || actor->user_id <= 0)
		return NULL;
	if (!venture_entity_registry_is_type_enabled(venture_entity_registry_get_default(), "organization_membership"))
		return NULL;
	venture_query_add_filter_int(query, "user-id", VENTURE_FILTER_OP_EQ, actor->user_id, NULL);
	venture_query_add_filter_string(query, "active", VENTURE_FILTER_OP_EQ, "true", NULL);
	if (org > 0 && actor->token_id > 0 && token_role(self, actor, org) < 0)
		return NULL;
	if (org > 0)
		venture_query_add_filter_int(query, "organization-id", VENTURE_FILTER_OP_EQ, org, NULL);
	return venture_database_find_one(self->database, query, NULL);
}
gboolean
venture_access_policy_has_membership(VentureAccessPolicy *self, const VentureAuthPrincipal *actor)
{
	g_autoptr(VentureEntity) member = NULL;
	if (administrator(actor))
		return TRUE;
	member = membership(self, actor, 0);
	return NULL != member;
}
gboolean
venture_access_policy_has_organization_role(VentureAccessPolicy *self, const VentureAuthPrincipal *actor,
	gint64 organization_id, const gint *roles, gsize n_roles)
{
	g_autoptr(VentureEntity) member = NULL;
	gboolean member_allowed = FALSE;
	gboolean token_allowed = FALSE;
	gint role = -1;
	gint token = -1;
	gsize i;
	if (administrator(actor))
		return TRUE;
	if (NULL == actor || !actor->authenticated || organization_id <= 0)
		return FALSE;
	member = membership(self, actor, organization_id);
	if (NULL == member)
		return FALSE;
	g_object_get(member, "role", &role, NULL);
	if (actor->token_id > 0)
		token = token_role(self, actor, organization_id);
	for (i = 0; i < n_roles; i++)
	{
		member_allowed = member_allowed || (roles[i] == role);
		token_allowed = token_allowed || (roles[i] == token);
	}
	return member_allowed && ((actor->token_id <= 0) || token_allowed);
}
static gint64
reference(VentureEntity *entity, const gchar *name)
{
	gint64 value = 0;
	if (NULL != g_object_class_find_property(G_OBJECT_GET_CLASS(entity), name))
		g_object_get(entity, name, &value, NULL);
	return value;
}
/* Personal records declare their owner reference in the same field table
 * as the rest of their schema. Parent chains are bounded and fail closed. */
static gboolean
personal_owner(VentureAccessPolicy *self, const VentureAuthPrincipal *actor, VentureEntity *entity, guint depth)
{
	g_autoptr(GPtrArray) fields = venture_entity_get_field_specs(entity);
	guint i;
	if (depth > 8)
		return FALSE;
	for (i = 0; i < fields->len; i++)
	{
		VentureFieldSpec *field = g_ptr_array_index(fields, i);
		g_autoptr(VentureEntity) parent = NULL;
		g_autoptr(VentureAccessScope) internal = NULL;
		GType type;
		gint64 id;
		if (!(venture_field_spec_get_flags(field) & VENTURE_COLUMN_FLAG_PERSONAL_OWNER))
			continue;
		id = reference(entity, venture_field_spec_get_name(field));
		type = venture_entity_registry_lookup(venture_entity_registry_get_default(), venture_field_spec_get_reference_type(field));
		if (type == VENTURE_TYPE_USER)
			return id > 0 && id == actor->user_id;
		if (type == G_TYPE_INVALID || id <= 0)
			return FALSE;
		internal = venture_access_policy_enter(self, NULL);
		parent = venture_database_get(self->database, type, id, NULL);
		return NULL != parent && !venture_entity_is_deleted(parent) && personal_owner(self, actor, parent, depth + 1);
	}
	return FALSE;
}

/* Personal ownership is a boundary, not an additional way to grant access.
 * Falling through to organization membership exposes a colleague's thread. */
static gboolean
personal_record(VentureEntity *entity)
{
	g_autoptr(GPtrArray) fields = venture_entity_get_field_specs(entity);
	guint i;
	for (i = 0; i < fields->len; i++)
		if (venture_field_spec_get_flags(g_ptr_array_index(fields, i)) & VENTURE_COLUMN_FLAG_PERSONAL_OWNER)
			return TRUE;
	return FALSE;
}

static gboolean
assigned_username(VentureAccessPolicy *self, const VentureAuthPrincipal *actor, VentureEntity *entity)
{
	g_autoptr(GPtrArray) fields = venture_entity_get_field_specs(entity);
	g_autoptr(VentureEntity) user = NULL;
	g_autofree gchar *username = NULL;
	guint i;
	gboolean active = FALSE;
	for (i = 0; i < fields->len; i++)
	{
		VentureFieldSpec *field = g_ptr_array_index(fields, i);
		g_autofree gchar *assigned = NULL;
		GParamSpec *property;
		if (!(venture_field_spec_get_flags(field) & VENTURE_COLUMN_FLAG_ASSIGNED_USERNAME)) continue;
		property = g_object_class_find_property(G_OBJECT_GET_CLASS(entity), field->name);
		if (!property || G_PARAM_SPEC_VALUE_TYPE(property) != G_TYPE_STRING) continue;
		/* Bearer display names are token labels, not usernames. Resolve the
		 * authenticated account id instead of granting on a display string. */
		if (!user)
		{
			user = venture_database_get(self->database, VENTURE_TYPE_USER, actor->user_id, NULL);
			if (!user || venture_entity_is_deleted(user)) return FALSE;
			g_object_get(user, "username", &username, "active", &active, NULL);
			if (!active || venture_string_is_empty(username)) return FALSE;
		}
		g_object_get(entity, field->name, &assigned, NULL);
		if (!venture_string_is_empty(assigned) && !g_strcmp0(username, assigned)) return TRUE;
	}
	return FALSE;
}

static gboolean
owned(VentureAccessPolicy *self, const VentureAuthPrincipal *actor, VentureEntity *entity)
{
	g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(self, NULL);
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) team = NULL;
	g_autoptr(VentureEntity) parent = NULL;
	gint64 team_id = reference(entity, "team-id");
	gint64 venture_id = reference(entity, "venture-id");
	if (assigned_username(self, actor, entity))
		return TRUE;
	if (actor->user_id == reference(entity, "owner-user-id"))
		return TRUE;
	if (team_id > 0)
	{
		team = venture_database_get(self->database, VENTURE_TYPE_TEAM, team_id, NULL);
		if (NULL != team && !venture_entity_is_deleted(team) &&
			venture_entity_get_organization_id(team) == venture_entity_get_organization_id(entity))
		{
			query = venture_query_new(VENTURE_TYPE_TEAM_MEMBERSHIP);
			venture_query_add_filter_int(query, "team-id", VENTURE_FILTER_OP_EQ, team_id, NULL);
			venture_query_add_filter_int(query, "user-id", VENTURE_FILTER_OP_EQ, actor->user_id, NULL);
			venture_query_add_filter_int(query, "organization-id", VENTURE_FILTER_OP_EQ, venture_entity_get_organization_id(entity), NULL);
			venture_query_add_filter_string(query, "active", VENTURE_FILTER_OP_EQ, "true", NULL);
			if (venture_database_count(self->database, query, NULL) > 0)
				return TRUE;
		}
	}
	if (venture_id > 0 && !VENTURE_IS_VENTURE(entity))
	{
		parent = venture_database_get(self->database, VENTURE_TYPE_VENTURE, venture_id, NULL);
		if (NULL != parent && !venture_entity_is_deleted(parent) &&
			venture_entity_get_organization_id(parent) == venture_entity_get_organization_id(entity))
			return owned(self, actor, parent);
	}
	return FALSE;
}
static gboolean
refuse(GError **error, gboolean hidden)
{
	g_set_error_literal(error, VENTURE_ERROR,
		hidden ? VENTURE_ERROR_NOT_FOUND : VENTURE_ERROR_PERMISSION_DENIED,
		hidden ? "No such record" : "Your organization role does not permit this action");
	return FALSE;
}
static gboolean
finance_type(VentureEntity *entity)
{
	return NULL != g_type_get_qdata(G_OBJECT_TYPE(entity),
		g_quark_from_static_string("venture-access-financial"));
}
static gboolean
payroll_type(VentureEntity *entity)
{
	return NULL != g_type_get_qdata(G_OBJECT_TYPE(entity),
		g_quark_from_static_string("venture-access-payroll"));
}
static gboolean
role_allows(VentureAccessPolicy *self, const VentureAuthPrincipal *actor,
	const gchar *action, VentureEntity *entity, gint role)
{
	gboolean read = 0 == g_strcmp0(action, "read") || 0 == g_strcmp0(action, "export");
	gboolean manager = role == VENTURE_ORGANIZATION_ROLE_OWNER || role == VENTURE_ORGANIZATION_ROLE_ADMIN;
	if (role < 0)
		return FALSE;
	if (manager)
		return TRUE;
	/* An outside accountant reads and exports the books; the write refusal
	 * that names the role is raised by the caller before reaching here. */
	if (role == VENTURE_ORGANIZATION_ROLE_ACCOUNTANT)
		return read && venture_accountant_role_readable(self->database, entity);
	/* A payer must not disable the second-person rule that constrains it. */
	if (VENTURE_IS_ACCOUNTING_APPROVAL_RULE(entity) && !read)
		return FALSE;
	if (payroll_type(entity) && role != VENTURE_ORGANIZATION_ROLE_FINANCE)
		return FALSE;
	if (finance_type(entity) && role != VENTURE_ORGANIZATION_ROLE_FINANCE)
		return FALSE;
	if (VENTURE_IS_ORGANIZATION_MEMBERSHIP(entity) || VENTURE_IS_TEAM_MEMBERSHIP(entity) || VENTURE_IS_TEAM(entity))
		return read;
	if (role == VENTURE_ORGANIZATION_ROLE_VIEWER && !read)
		return FALSE;
	if (role == VENTURE_ORGANIZATION_ROLE_VIEWER || role == VENTURE_ORGANIZATION_ROLE_SALES || role == VENTURE_ORGANIZATION_ROLE_SUPPORT)
	{
		if (VENTURE_IS_ORGANIZATION(entity))
			return read;
		return owned(self, actor, entity);
	}
	return TRUE;
}
gboolean
venture_access_policy_can(VentureAccessPolicy *self, const VentureAuthPrincipal *actor,
	const gchar *action, VentureEntity *entity, GError **error)
{
	g_autoptr(VentureEntity) member = NULL;
	g_autoptr(GError) veto = NULL;
	gint role;
	gint64 org;
	gboolean read = 0 == g_strcmp0(action, "read") || 0 == g_strcmp0(action, "export");
	if (NULL == actor || !actor->authenticated)
		return refuse(error, TRUE);
	if (!read && 0 != g_strcmp0(action, "write") && 0 != g_strcmp0(action, "delete"))
		return refuse(error, FALSE);
	if (!administrator(actor))
	{
		/* Authentication and the account page need the caller's own row.
		 * The generic web type gate still requires the global owner role. */
		if (VENTURE_IS_USER(entity) && venture_entity_get_id(entity) == actor->user_id && read)
			goto allowed;
		if (personal_record(entity))
		{
			if (venture_access_policy_has_membership(self, actor) && personal_owner(self, actor, entity, 0))
			{
				/* Inbox rows retain labels and excerpts: ownership alone must
				 * not expose a business record after its access is revoked. */
				if (VENTURE_IS_NOTIFICATION(entity) || VENTURE_IS_WATCH(entity))
				{
					g_autofree gchar *target_type = NULL;
					gint64 target_id = reference(entity, "target-id");
					g_object_get(entity, "target-type", &target_type, NULL);
					if (target_id > 0 && !venture_string_is_empty(target_type))
					{
						g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(self, NULL);
						g_autoptr(VentureEntity) target = NULL;
						GType type = venture_entity_registry_lookup(venture_entity_registry_get_default(), target_type);
						if (type != G_TYPE_INVALID)
							target = venture_database_get(self->database, type, target_id, NULL);
						/* Personal targets are not published by the notifier;
						 * refusing them also bounds malicious reference cycles. */
						if (target == NULL || personal_record(target) ||
							!venture_access_policy_can(self, actor, "read", target, NULL))
							return refuse(error, read);
					}
				}
				goto allowed;
			}
			return refuse(error, read);
		}
		org = VENTURE_IS_ORGANIZATION(entity) ? venture_entity_get_id(entity) : venture_entity_get_organization_id(entity);
		if (org <= 0)
			return refuse(error, TRUE);
		member = membership(self, actor, org);
		if (NULL == member)
			return refuse(error, TRUE);
		g_object_get(member, "role", &role, NULL);
		/* Outside the books a write is refused as not found, like the read;
		 * inside them the refusal names the role so the answer is actionable. */
		if (!read && (role == VENTURE_ORGANIZATION_ROLE_ACCOUNTANT ||
			(actor->token_id > 0 && token_role(self, actor, org) == VENTURE_ORGANIZATION_ROLE_ACCOUNTANT)))
			return venture_accountant_role_readable(self->database, entity) ?
				venture_accountant_role_refuse_write(action, error) : refuse(error, TRUE);
		if (!role_allows(self, actor, action, entity, role) ||
			(actor->token_id > 0 && !role_allows(self, actor, action, entity, token_role(self, actor, org))))
			return refuse(error, read);
		if (!read && actor->role == VENTURE_USER_ROLE_VIEWER)
			return refuse(error, FALSE);
	}
allowed:
	g_signal_emit(self, self->decide_signal, 0, actor, action, entity, &veto);
	if (NULL != veto)
	{
		g_propagate_error(error, g_steal_pointer(&veto));
		return FALSE;
	}
	return TRUE;
}
static gboolean
role_proposes(gint role, const gchar *action, VentureEntity *entity)
{
	return (role == VENTURE_ORGANIZATION_ROLE_EDITOR && VENTURE_IS_JOURNAL(entity) && 0 == g_strcmp0(action, "post")) ||
		(role == VENTURE_ORGANIZATION_ROLE_VIEWER && !venture_entity_is_persisted(entity) && 0 == g_strcmp0(action, "write"));
}

gboolean
venture_access_policy_requires_approval(VentureAccessPolicy *self, const VentureAuthPrincipal *actor,
	const gchar *action, VentureEntity *entity, GError **error)
{
	g_autoptr(VentureEntity) member = NULL;
	g_autoptr(GError) veto = NULL;
	gint role;
	if (administrator(actor))
		return FALSE;
	member = membership(self, actor, venture_entity_get_organization_id(entity));
	if (NULL == member)
	{
		refuse(error, TRUE);
		return FALSE;
	}
	g_object_get(member, "role", &role, NULL);
	if (!role_proposes(role, action, entity))
		return FALSE;
	if (actor->token_id > 0)
	{
		gint minted = token_role(self, actor, venture_entity_get_organization_id(entity));
		if (!role_proposes(minted, action, entity) && !role_allows(self, actor, "write", entity, minted))
			return refuse(error, FALSE);
	}
	g_signal_emit(self, self->decide_signal, 0, actor, "write", entity, &veto);
	if (NULL != veto)
	{
		g_propagate_error(error, g_steal_pointer(&veto));
		return FALSE;
	}
	return TRUE;
}
GPtrArray *
venture_access_policy_find(VentureAccessPolicy *self, VentureQuery *query, GError **error)
{
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(GPtrArray) all = NULL;
	GPtrArray *result = g_ptr_array_new_with_free_func(g_object_unref);
	guint limit = venture_query_get_limit(query);
	guint offset = venture_query_get_offset(query);
	guint i;
	guint visible = 0;
	internal = venture_access_policy_enter(self, NULL);
	venture_query_set_limit(query, 0);
	venture_query_set_offset(query, 0);
	all = venture_database_find(self->database, query, error);
	venture_query_set_limit(query, limit);
	venture_query_set_offset(query, offset);
	g_clear_object(&internal);
	if (NULL == all)
	{
		g_ptr_array_unref(result);
		return NULL;
	}
	for (i = 0; i < all->len; i++)
	{
		VentureEntity *entity = g_ptr_array_index(all, i);
		if (!venture_access_policy_can(self, self->actor, self->read_action, entity, NULL))
			continue;
		if (visible++ < offset)
			continue;
		if (limit > 0 && result->len >= limit)
			break;
		g_ptr_array_add(result, g_object_ref(entity));
	}
	return result;
}
gboolean
venture_access_policy_check_write(VentureAccessPolicy *self, VentureEntity *entity, const gchar *action, GError **error)
{
	g_autoptr(VentureEntity) previous = NULL;
	g_autoptr(VentureAccessScope) internal = NULL;
	if (NULL == self->actor)
		return TRUE;
	if (venture_entity_is_persisted(entity))
	{
		internal = venture_access_policy_enter(self, NULL);
		previous = venture_database_get(self->database, G_OBJECT_TYPE(entity), venture_entity_get_id(entity), error);
		g_clear_object(&internal);
		if (NULL == previous)
			return refuse(error, TRUE);
		if (!venture_access_policy_can(self, self->actor, action, previous, error))
			return FALSE;
	}
	return venture_access_policy_can(self, self->actor, action, entity, error);
}

gboolean
venture_access_policy_check_read(VentureAccessPolicy *self, VentureEntity *entity, GError **error)
{
	return NULL == self->actor || venture_access_policy_can(self, self->actor, self->read_action, entity, error);
}
gint64
venture_access_policy_count(VentureAccessPolicy *self, VentureQuery *query, GError **error)
{
	g_autoptr(GPtrArray) rows = NULL;
	guint limit = venture_query_get_limit(query);
	guint offset = venture_query_get_offset(query);
	venture_query_set_limit(query, 0);
	venture_query_set_offset(query, 0);
	rows = venture_access_policy_find(self, query, error);
	venture_query_set_limit(query, limit);
	venture_query_set_offset(query, offset);
	return NULL != rows ? (gint64)rows->len : -1;
}
/* Only these public routes authenticate with capabilities instead of local
 * sessions. Keep method and path shape exact as more routes are added. */
static gboolean
public_capability_request(HtmxRequest *request)
{
	const gchar *path = htmx_request_get_path(request);
	HtmxMethod method = htmx_request_get_method(request);
	const gchar *suffix;
	if (!g_strcmp0(path, "/webhooks/stripe")) return method == HTMX_METHOD_POST;
	if (g_str_has_prefix(path, "/f/") && path[3] != '\0')
		return method == HTMX_METHOD_POST && strchr(path + 3, '/') == NULL;
	if (!g_str_has_prefix(path, "/q/") || path[3] == '\0' || path[3] == '/') return FALSE;
	suffix = strchr(path + 3, '/');
	if (!suffix) return method == HTMX_METHOD_GET || method == HTMX_METHOD_POST;
	return method == HTMX_METHOD_POST && !g_strcmp0(suffix, "/accept");
}

void
venture_orgaccess_web_dispatch(VentureAuth *auth, VentureContext *context,
	HtmxContext *http, HtmxMiddlewareNext next, gpointer next_data)
{
	g_autoptr(VentureAuthPrincipal) actor = NULL;
	g_autoptr(VentureAccessScope) boundary = NULL;
	g_autoptr(VentureAccessScope) scope = NULL;
	HtmxRequest *request = htmx_context_get_request(http);
	const gchar *path = htmx_request_get_path(request);

	/* A nested main loop can dispatch a webhook underneath an authenticated
	 * request. Protocol authority must never inherit the enclosing caller. */
	boundary = venture_access_policy_enter(venture_database_get_access_policy(
		venture_context_get_database(context)), NULL);
	/* These protocols authenticate themselves, before accessing business
	 * records. They do not acquire authority from browser credentials. */
	if (!public_capability_request(request) && !g_str_has_prefix(path, "/hooks/") && !g_str_has_prefix(path, "/federation/") &&
		0 != g_strcmp0(path, "/login") && 0 != g_strcmp0(path, "/logout") && 0 != g_strcmp0(path, "/account/password"))
	{
		VentureAccessPolicy *policy = venture_database_get_access_policy(venture_context_get_database(context));
		actor = venture_auth_authenticate(auth, request);
		if (actor->authenticated)
		{
			scope = venture_access_policy_enter(policy, actor);
			if (!venture_access_policy_has_membership(policy, actor) &&
				0 != g_strcmp0(path, "/account") && 0 != g_strcmp0(path, "/look") &&
				!g_str_has_prefix(path, "/api/") && !g_str_has_prefix(path, "/ui/") && !g_str_has_prefix(path, "/e/"))
			{
				HtmxResponse *response = htmx_response_new();
				htmx_response_set_status(response, 302);
				htmx_response_add_header(response, "Location", "/account");
				htmx_context_set_response(http, response);
				return;
			}
			/* The books are the accountant's home; the dashboard is not
			 * theirs to see, so the root lands on /books instead. */
			if (0 == g_strcmp0(path, "/") && venture_accountant_role_only(venture_context_get_database(context), actor))
			{
				HtmxResponse *response = htmx_response_new();
				htmx_response_set_status(response, 302);
				htmx_response_add_header(response, "Location", "/books");
				htmx_context_set_response(http, response);
				return;
			}
		}
	}
	if (venture_mfa_web_gate(context, http, actor))
		return;
	if (NULL != scope && (g_str_has_suffix(path, "/export") || 0 == g_strcmp0(htmx_request_get_query_param(request, "format"), "csv")))
		scope->policy->read_action = "export";
	next(http, next_data);
}

gboolean
venture_orgaccess_bootstrap_owner(VentureDatabase *database, VentureUser *user, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ORGANIZATION);
	g_autoptr(GPtrArray) organizations = NULL;
	guint i;
	if (!venture_database_begin(database, error))
		return FALSE;
	if (!venture_database_save(database, VENTURE_ENTITY(user), NULL, error))
		goto failed;
	organizations = venture_database_find(database, query, error);
	if (NULL == organizations)
		goto failed;
	for (i = 0; i < organizations->len; i++)
	{
		g_autoptr(VentureEntity) member = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP,
			"user-id", venture_entity_get_id(VENTURE_ENTITY(user)),
			"organization-id", venture_entity_get_id(g_ptr_array_index(organizations, i)),
			"role", VENTURE_ORGANIZATION_ROLE_OWNER, "active", TRUE, NULL);
		if (!venture_database_save(database, member, NULL, error))
			goto failed;
	}
	return venture_database_commit(database, error);
failed:
	venture_database_rollback(database);
	return FALSE;
}

gboolean
venture_orgaccess_prepare(VentureDatabase *database, VentureEntity *entity, GError **error)
{
	g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(database), NULL);
	g_autoptr(VentureEntity) team = NULL;
	g_autoptr(VentureEntity) previous = NULL;
	gint64 team_id = reference(entity, "team-id");
	gint64 org = venture_entity_get_organization_id(entity);
	if (venture_entity_is_persisted(entity))
		previous = venture_database_get(database, G_OBJECT_TYPE(entity), venture_entity_get_id(entity), NULL);
	if (team_id > 0 && (NULL == previous || team_id != reference(previous, "team-id") || org != venture_entity_get_organization_id(previous)))
	{
		team = venture_database_get(database, VENTURE_TYPE_TEAM, team_id, error);
		if (NULL == team || venture_entity_is_deleted(team) || venture_entity_get_organization_id(team) != org)
		{
			g_clear_error(error);
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "The team must belong to the record's organization");
			return FALSE;
		}
	}
	if (VENTURE_IS_ORGANIZATION_MEMBERSHIP(entity) || VENTURE_IS_TEAM_MEMBERSHIP(entity))
	{
		if (org <= 0 || reference(entity, "user-id") <= 0)
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "A membership requires a user and an organization");
			return FALSE;
		}
	}
	if (VENTURE_IS_TEAM_MEMBERSHIP(entity))
	{
		gboolean active;
		VentureAuthPrincipal actor;
		g_autoptr(VentureEntity) member = NULL;
		actor.user_id = reference(entity, "user-id");
		actor.token_id = 0;
		actor.role = VENTURE_USER_ROLE_EDITOR;
		actor.name = NULL;
		actor.authenticated = TRUE;
		member = membership(venture_database_get_access_policy(database), &actor, org);
		g_object_get(entity, "active", &active, NULL);
		if (active && NULL == member)
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Team membership requires an active organization membership");
			return FALSE;
		}
	}
	if (VENTURE_IS_API_TOKEN(entity) && !venture_entity_is_persisted(entity))
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP);
		g_autoptr(GPtrArray) members = NULL;
		g_autoptr(JsonBuilder) builder = json_builder_new();
		g_autoptr(JsonNode) node = NULL;
		g_autofree gchar *snapshot = NULL;
		guint i;
		venture_query_add_filter_int(query, "user-id", VENTURE_FILTER_OP_EQ, reference(entity, "user-id"), NULL);
		venture_query_add_filter_string(query, "active", VENTURE_FILTER_OP_EQ, "true", NULL);
		members = venture_database_find(database, query, error);
		if (NULL == members)
			return FALSE;
		json_builder_begin_object(builder);
		for (i = 0; i < members->len; i++)
		{
			VentureEntity *member = g_ptr_array_index(members, i);
			g_autofree gchar *key = g_strdup_printf("%" G_GINT64_FORMAT, venture_entity_get_organization_id(member));
			gint role;
			g_object_get(member, "role", &role, NULL);
			json_builder_set_member_name(builder, key);
			json_builder_add_int_value(builder, role);
		}
		json_builder_end_object(builder);
		node = json_builder_get_root(builder);
		snapshot = venture_json_to_string(node, FALSE);
		g_object_set(entity, "membership-snapshot", snapshot, NULL);
	}
	return TRUE;
}

VentureAccessScope *
venture_orgaccess_enter_ai(VentureContext *context, const VentureAuthPrincipal *principal)
{
	VentureAuthPrincipal anonymous;
	anonymous.user_id = 0;
	anonymous.token_id = 0;
	anonymous.role = VENTURE_USER_ROLE_VIEWER;
	anonymous.name = NULL;
	anonymous.authenticated = FALSE;
	return venture_access_policy_enter(venture_database_get_access_policy(
		venture_context_get_database(context)), NULL != principal ? principal : &anonymous);
}
gboolean
venture_orgaccess_check_proposal(VentureDatabase *database, VentureEntity *staged,
	VentureAuditAction action, const gchar *via, GError **error)
{
	VentureAccessPolicy *policy = venture_database_get_access_policy(database);
	const VentureAuthPrincipal *actor = venture_access_policy_get_actor(policy);
	g_autoptr(GError) local_error = NULL;
	if (NULL == actor)
		return TRUE;
	if (venture_access_policy_requires_approval(policy, actor,
		(NULL != via && g_str_has_prefix(via, "orgaccess:journal-post:")) ? "post" : "write", staged, &local_error))
		return TRUE;
	if (NULL != local_error)
	{
		g_propagate_error(error, g_steal_pointer(&local_error));
		return FALSE;
	}
	return venture_access_policy_check_write(policy, staged,
		action == VENTURE_AUDIT_ACTION_DELETE ? "delete" : "write", error);
}

gboolean
venture_orgaccess_confirmation_visible(VentureDatabase *database, VentureEntity *staged,
	gint64 proposer, const gchar *via)
{
	VentureAccessPolicy *policy = venture_database_get_access_policy(database);
	const VentureAuthPrincipal *actor = venture_access_policy_get_actor(policy);
	if (NULL == actor)
		return TRUE;
	if (venture_access_policy_can(policy, actor, "read", staged, NULL))
		return TRUE;
	return proposer > 0 && proposer == actor->user_id &&
		venture_access_policy_requires_approval(policy, actor,
			(NULL != via && g_str_has_prefix(via, "orgaccess:journal-post:")) ? "post" : "write", staged, NULL);
}

/* A bearer cannot outlive the user who minted it or retain a demoted role. */
gboolean
venture_orgaccess_limit_token(VentureAuth *auth, VentureDatabase *database, VentureAuthPrincipal *principal)
{
	g_autoptr(VentureEntity) user = NULL;
	VentureAuthPrincipal current;
	gboolean active;
	if (principal->user_id <= 0)
		return TRUE;
	user = venture_database_get(database, VENTURE_TYPE_USER, principal->user_id, NULL);
	if (NULL == user || venture_entity_is_deleted(user))
		return FALSE;
	current = *principal;
	g_object_get(user, "active", &active, "role", &current.role, NULL);
	if (!active)
		return FALSE;
	if (!venture_auth_require(auth, &current, principal->role, NULL))
		principal->role = current.role;
	return TRUE;
}
