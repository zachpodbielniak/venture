/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

/*
 * Who is reading, and the answers kept for one list read. The home thread
 * (and any thread that never bound one) uses the policy's own; a worker
 * serving a parallel read binds a state of its own for the job
 * (venture_access_policy_bind_thread()), so readers on several threads
 * never see each other's actor or caches.
 */
typedef struct
{
	const VentureAuthPrincipal *actor;
	const gchar *read_action;
	gint64 organization_id;
	/*
	 * Answers that hold for the whole of one list read by one actor, so
	 * its rows do not ask them again one by one: the workspace standing of
	 * the reader, their memberships by organization, and the workspace's
	 * consent per record type. Only set inside venture_access_policy_find().
	 */
	const VentureAuthPrincipal *batch_actor;
	gboolean batch_principal;
	gint batch_tenant_administrator;
	GHashTable *batch_members;
	GHashTable *batch_resources;
	gboolean shared_only;
} PolicyState;

struct _VentureAccessPolicy
{
	GObject parent_instance;
	VentureDatabase *database;
	guint decide_signal;
	PolicyState state;
};

/* A thread's own states, one per policy it has bound. */
typedef struct ThreadBinding ThreadBinding;
struct ThreadBinding
{
	VentureAccessPolicy *policy;
	PolicyState state;
	ThreadBinding *next;
};
static GPrivate thread_bindings;

static PolicyState *
st(VentureAccessPolicy *self)
{
	ThreadBinding *binding;
	for (binding = g_private_get(&thread_bindings); NULL != binding; binding = binding->next)
		if (binding->policy == self)
			return &binding->state;
	return &self->state;
}

struct _VentureAccessScope
{
	GObject parent_instance;
	VentureAccessPolicy *policy;
	PolicyState *state;	/* the state this scope changed, on its thread */
	const VentureAuthPrincipal *previous;
	VentureAuthPrincipal *actor;
	const gchar *previous_action;
	gint64 previous_organization;
	gboolean previous_shared_only;
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
	self->state.read_action = "read";
}
static void
scope_finalize(GObject *object)
{
	VentureAccessScope *self = VENTURE_ACCESS_SCOPE(object);
	self->state->actor = self->previous;
	self->state->read_action = self->previous_action;
	self->state->organization_id = self->previous_organization;
	self->state->shared_only = self->previous_shared_only;
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
/*
 * A worker thread serving a parallel read gets a state of its own, so its
 * actor, its organization pin and its list caches are never another
 * thread's. Unbound threads share the policy's own state, as every thread
 * always did.
 */
void
venture_access_policy_bind_thread(VentureAccessPolicy *self)
{
	ThreadBinding *binding;

	g_return_if_fail(VENTURE_IS_ACCESS_POLICY(self));
	for (binding = g_private_get(&thread_bindings); NULL != binding; binding = binding->next)
		g_return_if_fail(binding->policy != self);

	binding = g_new0(ThreadBinding, 1);
	binding->policy = g_object_ref(self);
	binding->state.read_action = "read";
	binding->state.batch_tenant_administrator = -1;
	binding->next = g_private_get(&thread_bindings);
	g_private_set(&thread_bindings, binding);
}

void
venture_access_policy_unbind_thread(VentureAccessPolicy *self)
{
	ThreadBinding *head = g_private_get(&thread_bindings);
	ThreadBinding *previous = NULL;
	ThreadBinding *binding;

	g_return_if_fail(VENTURE_IS_ACCESS_POLICY(self));
	for (binding = head; NULL != binding; previous = binding, binding = binding->next)
	{
		if (binding->policy != self)
			continue;
		/* A scope still open would restore into freed state. */
		g_warn_if_fail(NULL == binding->state.actor && NULL == binding->state.batch_actor);
		if (NULL == previous)
			g_private_set(&thread_bindings, binding->next);
		else
			previous->next = binding->next;
		g_clear_pointer(&binding->state.batch_members, g_hash_table_unref);
		g_clear_pointer(&binding->state.batch_resources, g_hash_table_unref);
		g_object_unref(binding->policy);
		g_free(binding);
		return;
	}
	g_critical("venture_access_policy_unbind_thread(): this thread never bound the policy");
}

struct _VentureAccessSnapshot
{
	VentureAuthPrincipal *actor;	/* NULL: trusted internal work */
	const gchar *read_action;	/* a static string */
	gint64 organization_id;
	gboolean shared_only;
};

/*
 * What this thread's reader is right now, to be read again on another
 * thread: the request is authenticated and scoped on the main thread and
 * answered on a worker, which must judge as exactly the same reader.
 */
VentureAccessSnapshot *
venture_access_policy_snapshot(VentureAccessPolicy *self)
{
	VentureAccessSnapshot *snapshot;
	PolicyState *state;

	g_return_val_if_fail(VENTURE_IS_ACCESS_POLICY(self), NULL);
	state = st(self);
	snapshot = g_new0(VentureAccessSnapshot, 1);
	if (NULL != state->actor)
	{
		snapshot->actor = g_new(VentureAuthPrincipal, 1);
		*snapshot->actor = *state->actor;
		snapshot->actor->name = g_strdup(state->actor->name);
	}
	snapshot->read_action = state->read_action;
	snapshot->organization_id = state->organization_id;
	snapshot->shared_only = state->shared_only;
	return snapshot;
}

void
venture_access_snapshot_free(VentureAccessSnapshot *snapshot)
{
	if (NULL == snapshot)
		return;
	g_clear_pointer(&snapshot->actor, venture_auth_principal_free);
	g_free(snapshot);
}

VentureAccessScope *
venture_access_policy_enter_snapshot(VentureAccessPolicy *self, const VentureAccessSnapshot *snapshot)
{
	VentureAccessScope *scope;
	PolicyState *state;

	g_return_val_if_fail(VENTURE_IS_ACCESS_POLICY(self), NULL);
	g_return_val_if_fail(NULL != snapshot, NULL);
	scope = venture_access_policy_enter(self, snapshot->actor);
	state = st(self);
	state->read_action = snapshot->read_action;
	state->organization_id = snapshot->organization_id;
	state->shared_only = snapshot->shared_only;
	return scope;
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
	scope->state = st(self);
	scope->previous = st(self)->actor;
	scope->previous_action = st(self)->read_action;
	scope->previous_organization = st(self)->organization_id;
	scope->previous_shared_only = st(self)->shared_only;
	/* An internal boundary starts independent work. Nested user scopes
	 * retain restrictions instead of widening a paid organization's tools. */
	if (!actor) { st(self)->organization_id = 0; st(self)->shared_only = FALSE; }
	st(self)->read_action = "read";
	if (NULL != actor)
	{
		scope->actor = g_new(VentureAuthPrincipal, 1);
		*scope->actor = *actor;
		scope->actor->name = g_strdup(actor->name);
	}
	st(self)->actor = scope->actor;
	return scope;
}
VentureAccessScope *
venture_access_policy_enter_organization(VentureAccessPolicy *self,
	const VentureAuthPrincipal *actor, gint64 organization_id)
{
	VentureAccessScope *scope = venture_access_policy_enter(self, actor);
	st(self)->organization_id = organization_id > 0 &&
		(scope->previous_organization == 0 || scope->previous_organization == organization_id) ? organization_id : -1;
	return scope;
}
VentureAccessScope *
venture_access_policy_enter_shared(VentureAccessPolicy *self, gint64 organization_id)
{
	VentureAccessScope *scope = venture_access_policy_enter_organization(self, st(self)->actor, organization_id);
	st(self)->shared_only = TRUE;
	return scope;
}
gint64
venture_access_policy_get_organization(VentureAccessPolicy *self)
{
	return st(self)->organization_id;
}
const VentureAuthPrincipal *
venture_access_policy_get_actor(VentureAccessPolicy *self)
{
	return st(self)->actor;
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
/* An interactive hosted workspace administrator. By design an EDITOR with an
 * organization ADMIN membership only in the organizations that existed at
 * bootstrap; workspace authority must also cover later-created ones, so
 * every site that shortcuts on administrator() for the platform roles has
 * to ask this too. API tokens never inherit it. */
static gboolean
tenant_administrator_uncached(VentureAccessPolicy *self, const VentureAuthPrincipal *actor)
{
	return NULL != actor && actor->authenticated && actor->token_id == 0 &&
		actor->role == VENTURE_USER_ROLE_EDITOR &&
		venture_tenant_service_is_enabled(venture_tenant_service_get(self->database)) &&
		venture_tenant_service_is_member(venture_tenant_service_get(self->database), actor->user_id, TRUE);
}
static gboolean
batched(VentureAccessPolicy *self, const VentureAuthPrincipal *actor)
{
	return NULL != actor && actor == st(self)->batch_actor;
}
static gboolean
tenant_administrator(VentureAccessPolicy *self, const VentureAuthPrincipal *actor)
{
	if (!batched(self, actor))
		return tenant_administrator_uncached(self, actor);
	if (st(self)->batch_tenant_administrator < 0)
		st(self)->batch_tenant_administrator = tenant_administrator_uncached(self, actor);
	return st(self)->batch_tenant_administrator;
}
/* The workspace accepts this reader. A refusal is never kept, so its
 * error is always the service's own. */
static gboolean
principal_allowed(VentureAccessPolicy *self, const VentureAuthPrincipal *actor, GError **error)
{
	if (batched(self, actor) && st(self)->batch_principal)
		return TRUE;
	if (!venture_tenant_service_check_principal(venture_tenant_service_get(self->database), actor, error))
		return FALSE;
	if (batched(self, actor))
		st(self)->batch_principal = TRUE;
	return TRUE;
}
/* The workspace's consent to reading or writing this kind of record. */
static gboolean
resource_allowed(VentureAccessPolicy *self, const VentureAuthPrincipal *actor, VentureEntity *entity,
	gboolean write, GError **error)
{
	/* GType values are consecutive, so the write bit has to sit below the
	 * type. OR-ing it in would make a write of one type look like a read
	 * of the next. */
	gpointer key = GSIZE_TO_POINTER(((gsize)G_OBJECT_TYPE(entity) << 1) | (write ? 1u : 0u));
	if (batched(self, actor) && g_hash_table_contains(st(self)->batch_resources, key))
		return TRUE;
	if (!venture_tenant_service_check_resource(venture_tenant_service_get(self->database), G_OBJECT(entity), write, error))
		return FALSE;
	if (batched(self, actor))
		g_hash_table_add(st(self)->batch_resources, key);
	return TRUE;
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
static VentureEntity *membership_uncached(VentureAccessPolicy *self, const VentureAuthPrincipal *actor, gint64 org);
static VentureEntity *
membership(VentureAccessPolicy *self, const VentureAuthPrincipal *actor, gint64 org)
{
	VentureEntity *member;
	gpointer known;
	if (!batched(self, actor))
		return membership_uncached(self, actor, org);
	if (g_hash_table_lookup_extended(st(self)->batch_members, &org, NULL, &known))
		return known ? g_object_ref(known) : NULL;
	member = membership_uncached(self, actor, org);
	g_hash_table_insert(st(self)->batch_members, g_memdup2(&org, sizeof org), member ? g_object_ref(member) : NULL);
	return member;
}
static VentureEntity *
membership_uncached(VentureAccessPolicy *self, const VentureAuthPrincipal *actor, gint64 org)
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
	if (venture_tenant_service_is_enabled(venture_tenant_service_get(self->database)))
		return venture_tenant_service_check_principal(venture_tenant_service_get(self->database), actor, NULL);
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
	if (venture_tenant_service_is_enabled(venture_tenant_service_get(self->database)) &&
	    !venture_tenant_service_check_principal(venture_tenant_service_get(self->database), actor, NULL)) return FALSE;
	if (organization_id > 0 && tenant_administrator(self, actor)) {
		g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(self, NULL);
		g_autoptr(VentureEntity) organization = venture_database_get(self->database, VENTURE_TYPE_ORGANIZATION, organization_id, NULL);
		return organization != NULL && !venture_entity_is_deleted(organization);
	}
	if (venture_tenant_service_get_support_organization(venture_tenant_service_get(self->database)) > 0)
		return FALSE;
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
/* Zero is explicitly shared only for optional ownership. An unresolved
 * positive reference is private and inaccessible, never a shared fallback. */
static gint64 privacy_owner_id(VentureAccessPolicy *self, VentureEntity *entity, guint depth, gboolean declared)
{
	g_autoptr(GPtrArray) fields = venture_entity_get_field_specs(entity);
	gint64 resolved = 0;
	guint i;
	if (depth > 8 || !self->database) return -1;
	/* Ownership is retained when sharing, so only the owner can revoke it.
	 * Every dependent record resolves this same account boundary. */
	if (VENTURE_IS_ACCOUNT(entity)) {
		gboolean shared = FALSE;
		g_object_get(entity, "household-shared", &shared, NULL);
		if (shared && !declared) return 0;
	}
	for (i = 0; i < fields->len; i++)
	{
		VentureFieldSpec *field = g_ptr_array_index(fields, i);
		VentureColumnFlags flags = venture_field_spec_get_flags(field);
		g_autoptr(VentureEntity) parent = NULL;
		g_autoptr(VentureAccessScope) internal = NULL;
		GType type;
		gint64 id, owner;
		gboolean strict = (flags & VENTURE_COLUMN_FLAG_PERSONAL_OWNER) != 0;
		if (flags & VENTURE_COLUMN_FLAG_PRIVATE_PARTICIPANT) return -2;
		if (!(flags & (VENTURE_COLUMN_FLAG_PERSONAL_OWNER | VENTURE_COLUMN_FLAG_OPTIONAL_PERSONAL_OWNER |
			VENTURE_COLUMN_FLAG_PRIVACY_PARENT))) continue;
		id = reference(entity, venture_field_spec_get_name(field));
		if (id <= 0) {
			if (strict || id < 0) return -1;
			continue;
		}
		type = venture_entity_registry_lookup_any(venture_entity_registry_get_default(), venture_field_spec_get_reference_type(field));
		if (type == VENTURE_TYPE_USER) {
			owner = id;
			/* An attached receipt owned by the same personal account follows
			 * that account's explicit sharing choice. Standalone private files
			 * keep their independent owner boundary. */
			if (!declared && VENTURE_IS_DOCUMENT(entity) && reference(entity, "expense-id") > 0) {
				internal = venture_access_policy_enter(self, NULL);
				parent = venture_database_get(self->database, VENTURE_TYPE_EXPENSE, reference(entity, "expense-id"), NULL);
				if (parent && privacy_owner_id(self, parent, depth + 1, TRUE) == id) owner = 0;
			}
		}
		else {
			if (type == G_TYPE_INVALID) return -1;
			internal = venture_access_policy_enter(self, NULL);
			parent = venture_database_get(self->database, type, id, NULL);
			if (!parent || (venture_entity_is_deleted(parent) && !(flags & VENTURE_COLUMN_FLAG_PRIVACY_PARENT)) ||
				venture_entity_get_organization_id(parent) != venture_entity_get_organization_id(entity)) return -1;
			owner = privacy_owner_id(self, parent, depth + 1, declared);
		}
		if (owner < 0 || (strict && owner == 0) || (resolved > 0 && owner > 0 && resolved != owner)) return -1;
		if (owner > 0) resolved = owner;
	}
	/* Links retain labels from both endpoints. They inherit the private
	 * boundary too, rather than publishing a personal account's name. */
	if (VENTURE_IS_RECORD_LINK(entity)) {
		static const gchar *const names[] = { "source", "target" };
		g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(self, NULL);
		for (i = 0; i < G_N_ELEMENTS(names); i++) {
			g_autofree gchar *type_field = g_strconcat(names[i], "-type", NULL);
			g_autofree gchar *id_field = g_strconcat(names[i], "-id", NULL);
			g_autofree gchar *name = NULL;
			g_autoptr(VentureEntity) target = NULL;
			GType type;
			gint64 owner;
			g_object_get(entity, type_field, &name, NULL);
			if (venture_string_is_empty(name)) {
				if (venture_entity_get_id(entity) <= 0 && reference(entity, id_field) <= 0) continue;
				return -1;
			}
			type = venture_entity_registry_lookup_any(venture_entity_registry_get_default(), name);
			if (type == G_TYPE_INVALID) return -1;
			target = venture_database_get(self->database, type, reference(entity, id_field), NULL);
			if (!target) return -1;
			owner = privacy_owner_id(self, target, depth + 1, declared);
			if (owner != 0 && venture_entity_get_organization_id(target) != venture_entity_get_organization_id(entity)) return -1;
			if (owner < 0 || (resolved > 0 && owner > 0 && resolved != owner)) return -1;
			if (owner > 0) resolved = owner;
		}
	}
	/* A journal discloses both sides. Its source and every account determine
	 * privacy, so hiding just the bank-side line cannot expose the purchase
	 * through the expense-side line or the general ledger export. */
	if (VENTURE_IS_JOURNAL(entity)) {
		g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(self, NULL);
		g_autofree gchar *source_type = NULL;
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(GPtrArray) lines = NULL;
		gint64 source_id = reference(entity, "source-id");
		g_object_get(entity, "source-type", &source_type, NULL);
		if (source_id > 0 && !venture_string_is_empty(source_type)) {
			GType type = venture_entity_registry_lookup_any(venture_entity_registry_get_default(), source_type);
			if (type != G_TYPE_INVALID && type != VENTURE_TYPE_JOURNAL &&
				type != VENTURE_TYPE_JOURNAL_LINE && type != VENTURE_TYPE_LEDGER_ENTRY) {
				g_autoptr(VentureEntity) source = venture_database_get(self->database, type, source_id, NULL);
				gint64 owner;
				if (!source) return -1;
				owner = privacy_owner_id(self, source, depth + 1, declared);
				if (owner < 0 || (resolved > 0 && owner > 0 && resolved != owner)) return -1;
				if (owner > 0) resolved = owner;
			}
		}
		if (venture_entity_get_id(entity) > 0) {
			query = venture_query_new(VENTURE_TYPE_JOURNAL_LINE);
			venture_query_set_organization(query, venture_entity_get_organization_id(entity));
			venture_query_set_limit(query, 0);
			venture_query_set_include_deleted(query, TRUE);
			venture_query_add_filter_int(query, "journal-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(entity), NULL);
			lines = venture_database_find(self->database, query, NULL);
			if (!lines) return -1;
			for (i = 0; i < lines->len; i++) {
				VentureEntity *line = g_ptr_array_index(lines, i);
				g_autoptr(VentureEntity) account = venture_database_get(self->database, VENTURE_TYPE_ACCOUNT,
					reference(line, "account-id"), NULL);
				gint64 owner;
				if (!account) return -1;
				owner = privacy_owner_id(self, account, depth + 1, declared);
				if (owner < 0 || (resolved > 0 && owner > 0 && resolved != owner)) return -1;
				if (owner > 0) resolved = owner;
			}
		}
	}
	return resolved;
}
static gint64 personal_owner_id(VentureAccessPolicy *self, VentureEntity *entity, guint depth)
{
	return privacy_owner_id(self, entity, depth, FALSE);
}
static gint64 declared_owner_id(VentureAccessPolicy *self, VentureEntity *entity)
{
	return privacy_owner_id(self, entity, 0, TRUE);
}
static gboolean personal_owner(VentureAccessPolicy *self, const VentureAuthPrincipal *actor, VentureEntity *entity, guint depth)
{
	gint64 owner = personal_owner_id(self, entity, depth);
	if (owner == -2) {
		g_autoptr(GPtrArray) fields = venture_entity_get_field_specs(entity);
		guint i;
		for (i = 0; i < fields->len; i++) {
			VentureFieldSpec *field = g_ptr_array_index(fields, i);
			if ((venture_field_spec_get_flags(field) & VENTURE_COLUMN_FLAG_PRIVATE_PARTICIPANT) &&
				reference(entity, field->name) > 0 && reference(entity, field->name) == actor->user_id) return TRUE;
		}
		return FALSE;
	}
	return owner > 0 && owner == actor->user_id;
}
gint64 venture_access_policy_get_personal_owner(VentureAccessPolicy *self, VentureEntity *entity)
{
	g_return_val_if_fail(VENTURE_IS_ACCESS_POLICY(self), -1);
	g_return_val_if_fail(VENTURE_IS_ENTITY(entity), -1);
	return personal_owner_id(self, entity, 0);
}
gint64 venture_access_policy_get_declared_owner(VentureAccessPolicy *self, VentureEntity *entity)
{
	g_return_val_if_fail(VENTURE_IS_ACCESS_POLICY(self), -1);
	g_return_val_if_fail(VENTURE_IS_ENTITY(entity), -1);
	return declared_owner_id(self, entity);
}
gboolean venture_access_policy_record_is_personal(VentureAccessPolicy *self, VentureEntity *entity)
{
	g_return_val_if_fail(VENTURE_IS_ACCESS_POLICY(self), TRUE);
	g_return_val_if_fail(VENTURE_IS_ENTITY(entity), TRUE);
	return personal_owner_id(self, entity, 0) != 0;
}
static gboolean legacy_personal_spine(VentureEntity *entity)
{
	g_autoptr(GPtrArray) fields = venture_entity_get_field_specs(entity);
	guint i;
	for (i = 0; i < fields->len; i++)
		if (venture_field_spec_get_flags(g_ptr_array_index(fields, i)) & VENTURE_COLUMN_FLAG_PERSONAL_OWNER) return TRUE;
	return FALSE;
}
static gboolean personal_record(VentureAccessPolicy *self, VentureEntity *entity)
{
	return venture_access_policy_record_is_personal(self, entity);
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
/*
 * A comment is read and written on its record's terms: whoever may read the
 * record may read its discussion and add to it -- a viewer included, which
 * is the one place a viewer writes without a proposal, because a comment
 * changes no business record and a conversation that waited for an approver
 * would arrive under the approver's name. Only the author may change the
 * words; the author or an organization owner or administrator may delete
 * them. A read-only token stays read-only.
 *
 * Applied to administrators too: an owner may delete anybody's comment but
 * may not put words in it.
 */
static gboolean
comment_allows(VentureAccessPolicy *self, const VentureAuthPrincipal *actor,
	const gchar *action, VentureEntity *comment, gboolean read, GError **error)
{
	static const gint managers[] = { VENTURE_ORGANIZATION_ROLE_OWNER, VENTURE_ORGANIZATION_ROLE_ADMIN };
	g_autofree gchar *subject_type = NULL;
	g_autofree gchar *author = NULL;
	g_autoptr(VentureEntity) subject = NULL;
	gint64 subject_id = 0;
	gint64 author_user_id = 0;
	gboolean own;
	GType type;
	g_object_get(comment, "subject-type", &subject_type, "subject-id", &subject_id,
		"author", &author, "author-user-id", &author_user_id, NULL);
	type = venture_entity_registry_lookup_any(venture_entity_registry_get_default(),
		NULL != subject_type ? subject_type : "");
	/* A comment on a comment would make this question recursive; none is
	 * ever written, and a row claiming to be one belongs to nobody. */
	if (type == G_TYPE_INVALID || type == VENTURE_TYPE_COMMENT || subject_id <= 0)
		return refuse(error, TRUE);
	{
		g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(self, NULL);
		subject = venture_database_get(self->database, type, subject_id, NULL);
	}
	if (subject == NULL)
		return refuse(error, TRUE);
	/* Not found, never forbidden: the same answer the record's page gives. */
	if (!venture_access_policy_can(self, actor, "read", subject, NULL))
		return refuse(error, TRUE);
	if (read)
		return TRUE;
	if (actor->token_id > 0 && actor->role == VENTURE_USER_ROLE_VIEWER)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED,
			"A read-only token cannot comment");
		return FALSE;
	}
	/* A new comment's author is stamped by its save validator from this
	 * same principal, so there is nobody else it could be written as. */
	if (!venture_entity_is_persisted(comment))
		return TRUE;
	/* With sign-in off every request is "local" with no account, and the
	 * string is all there is to compare. */
	own = author_user_id == actor->user_id &&
		(actor->user_id > 0 || 0 == g_strcmp0(author, actor->name));
	if (own)
		return TRUE;
	if (0 == g_strcmp0(action, "delete") &&
		(administrator(actor) || tenant_administrator(self, actor) ||
		 venture_access_policy_has_organization_role(self, actor,
			venture_entity_get_organization_id(comment), managers, G_N_ELEMENTS(managers))))
		return TRUE;
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED,
		0 == g_strcmp0(action, "delete")
			? "Only whoever wrote a comment, or an owner or administrator, may delete it"
			: "Only whoever wrote a comment may change it");
	return FALSE;
}
static gboolean
finance_type(VentureEntity *entity)
{
	return NULL != g_type_get_qdata(G_OBJECT_TYPE(entity),
		g_quark_from_static_string("venture-access-financial"));
}
static gboolean
private_financial_record(VentureEntity *entity)
{
	g_autoptr(GPtrArray) fields = venture_entity_get_field_specs(entity);
	guint i;
	if (finance_type(entity) || VENTURE_IS_RECORD_LINK(entity)) return TRUE;
	for (i = 0; i < fields->len; i++) {
		VentureFieldSpec *field = g_ptr_array_index(fields, i);
		if ((venture_field_spec_get_flags(field) &
			(VENTURE_COLUMN_FLAG_PRIVACY_PARENT | VENTURE_COLUMN_FLAG_PRIVATE_PARTICIPANT)) &&
			reference(entity, field->name) > 0) return TRUE;
	}
	return FALSE;
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
	/* Settings kept as records (an email brand, a hosted image): every
	 * member reads them, only an owner or administrator changes them. */
	if (!read && g_type_get_qdata(G_OBJECT_TYPE(entity), g_quark_from_static_string("venture-access-manager-write")))
		return FALSE;
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
	org = VENTURE_IS_ORGANIZATION(entity) ? venture_entity_get_id(entity) : venture_entity_get_organization_id(entity);
	if (st(self)->organization_id != 0 && org != st(self)->organization_id)
		return refuse(error, TRUE);
	/* This feed combines outcomes from several permission domains. Only
	 * the organization's administrators may consume its complete stream. */
	if (VENTURE_IS_BUSINESS_EVENT(entity) || VENTURE_IS_BUSINESS_EVENT_DELIVERY(entity))
	{
		static const gint roles[] = { VENTURE_ORGANIZATION_ROLE_OWNER, VENTURE_ORGANIZATION_ROLE_ADMIN };
		if (!venture_access_policy_has_organization_role(self, actor, org, roles, G_N_ELEMENTS(roles)))
			return refuse(error, TRUE);
	}
	if (venture_tenant_service_is_enabled(venture_tenant_service_get(self->database))) {
		VentureDataClass classification = venture_data_class_for_type(G_OBJECT_TYPE(entity));
		if (!principal_allowed(self, actor, error)) return FALSE;
		/* An audit body inherits its exact target's hosted authority, including
		 * platform records and private connectors, before a support shortcut. */
		if (VENTURE_IS_AUDIT_ENTRY(entity)) {
			g_autofree gchar *target_type = NULL;
			g_autoptr(VentureEntity) target = NULL;
			gint64 target_id = reference(entity, "target-id");
			GType type;
			if (!read) return refuse(error, FALSE);
			g_object_get(entity, "target-type", &target_type, NULL);
			type = venture_entity_registry_lookup_any(venture_entity_registry_get_default(), target_type);
			if (type == G_TYPE_INVALID || type == VENTURE_TYPE_AUDIT_ENTRY || target_id <= 0) return refuse(error, TRUE);
			{
				g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(self, NULL);
				target = venture_database_get(self->database, type, target_id, NULL);
			}
			if (!target || !venture_access_policy_can(self, actor, "read", target, NULL)) return refuse(error, TRUE);
		}
		if (venture_tenant_service_get_support_organization(venture_tenant_service_get(self->database)) > 0) {
			if (!venture_tenant_service_support_allows(venture_tenant_service_get(self->database), actor, entity, !read, FALSE, error)) return FALSE;
			goto allowed;
		}
		if (!resource_allowed(self, actor, entity, !read, error)) return FALSE;
		if (classification == VENTURE_DATA_CLASS_UNKNOWN || classification == VENTURE_DATA_CLASS_PLATFORM) return refuse(error, read);
		if (classification == VENTURE_DATA_CLASS_REFERENCE) {
			if (!read) return refuse(error, FALSE);
			goto allowed;
		}
		if (classification == VENTURE_DATA_CLASS_PERSONAL && !personal_record(self, entity)) {
			gint64 owner = VENTURE_IS_USER(entity) ? venture_entity_get_id(entity) : reference(entity, "owner-user-id");
			if (owner <= 0) owner = reference(entity, "user-id");
			if (owner <= 0 || owner != actor->user_id) return refuse(error, read);
			goto allowed;
		}
		if (classification == VENTURE_DATA_CLASS_TENANT_ADMIN) {
			if (actor->token_id != 0 || !venture_tenant_service_is_member(venture_tenant_service_get(self->database), actor->user_id, TRUE)) return refuse(error, read);
			goto allowed;
		}
	}
	if (g_type_get_qdata(G_OBJECT_TYPE(entity), g_quark_from_static_string("venture-access-platform")) && !administrator(actor))
		return refuse(error, TRUE);
	/* Install-wide configuration kept as records (a user-defined currency):
	 * anybody may read it, only an administrator may change it, whichever
	 * door the write came through. */
	if (!read && g_type_get_qdata(G_OBJECT_TYPE(entity), g_quark_from_static_string("venture-access-admin-write")) &&
	    !administrator(actor) && !tenant_administrator(self, actor))
		return refuse(error, FALSE);
	if (VENTURE_IS_INTEGRATION_CONNECTION(entity) && !administrator(actor)) {
		g_autofree gchar *provider = NULL;
		g_object_get(entity, "provider", &provider, NULL);
		/* Platform offer credentials use a reserved, service-owned binding
		 * identity. The billing organization's members do not own it. */
		if (provider && g_str_has_prefix(provider, "platform-ai-")) return refuse(error, TRUE);
	}
	if (VENTURE_IS_COMMENT(entity))
	{
		if (!comment_allows(self, actor, action, entity, read, error))
			return FALSE;
		goto allowed;
	}
	if (!administrator(actor))
	{
		/* Authentication and the account page need the caller's own row.
		 * The generic web type gate still requires the global owner role. */
		if (VENTURE_IS_USER(entity) && venture_entity_get_id(entity) == actor->user_id && read)
			goto allowed;
		/* Audit excerpts inherit the target's privacy, including after soft
		 * deletion. A missing target is retained for platform review only. */
		if (VENTURE_IS_AUDIT_ENTRY(entity))
		{
			g_autofree gchar *target_type = NULL;
			g_autoptr(VentureEntity) target = NULL;
			gint64 target_id = reference(entity, "target-id");
			GType type;
			g_object_get(entity, "target-type", &target_type, NULL);
			type = venture_entity_registry_lookup_any(venture_entity_registry_get_default(), target_type);
			if (type == G_TYPE_INVALID || type == VENTURE_TYPE_AUDIT_ENTRY || target_id <= 0) return refuse(error, read);
			{
				g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(self, NULL);
				target = venture_database_get(self->database, type, target_id, NULL);
			}
			if (!target || (personal_record(self, target) && !venture_access_policy_can(self, actor, "read", target, NULL))) return refuse(error, read);
		}
		if (personal_record(self, entity))
		{
			/* Privacy narrows access; it never upgrades a financial reader to
			 * a writer or bypasses a token's organization role. */
			if (private_financial_record(entity)) {
				member = membership(self, actor, venture_entity_get_organization_id(entity));
				if (!member) return refuse(error, read);
				g_object_get(member, "role", &role, NULL);
				if (!role_allows(self, actor, action, entity, role) ||
					(actor->token_id > 0 && !role_allows(self, actor, action, entity, token_role(self, actor, org))) ||
					(!read && actor->role == VENTURE_USER_ROLE_VIEWER)) return refuse(error, read);
				g_clear_object(&member);
			}
			/* The generated new-entry form asks about an empty prototype.
			 * Submitted shares still need both participants and must include
			 * their writer; the save validator refuses an empty prototype. */
			if (VENTURE_IS_HOUSEHOLD_SHARE(entity) && !venture_entity_is_persisted(entity) &&
				reference(entity, "payer-id") == 0 && reference(entity, "member-id") == 0) {
				static const gint roles[] = { VENTURE_ORGANIZATION_ROLE_OWNER, VENTURE_ORGANIZATION_ROLE_ADMIN, VENTURE_ORGANIZATION_ROLE_FINANCE };
				if (venture_access_policy_has_organization_role(self, actor, venture_entity_get_organization_id(entity), roles, G_N_ELEMENTS(roles))) goto allowed;
			}

			/* Assigning a new private connector is an organization admin's
			 * delegation. Later reads and writes belong to its private owner. */
			if (!venture_entity_is_persisted(entity) && !g_strcmp0(action, "write") &&
				(VENTURE_IS_MAIL_ACCOUNT(entity) || VENTURE_IS_CALENDAR_ACCOUNT(entity))) {
				static const gint roles[] = { VENTURE_ORGANIZATION_ROLE_OWNER, VENTURE_ORGANIZATION_ROLE_ADMIN };
				if (venture_access_policy_has_organization_role(self, actor, venture_entity_get_organization_id(entity), roles, G_N_ELEMENTS(roles))) goto allowed;
			}
			/* Hosted membership is the identity boundary for one's own legacy
			 * account/MFA rows. A workspace admin may also own a private row
			 * in a later-created legal organization without a historical org
			 * membership; this never grants access to another private owner. */
			if (venture_tenant_service_is_enabled(venture_tenant_service_get(self->database)) &&
			    personal_owner(self, actor, entity, 0) &&
			    ((venture_entity_get_organization_id(entity) == 0 && legacy_personal_spine(entity)) ||
			     (venture_entity_get_organization_id(entity) > 0 && actor->token_id == 0 &&
			      venture_tenant_service_is_member(venture_tenant_service_get(self->database), actor->user_id, TRUE)))) goto allowed;
			/* Older chat and MFA rows predate organization placement. Their
			 * strict user ownership remains usable; optional private imports
			 * always require membership in their explicit organization. */
			member = venture_entity_get_organization_id(entity) > 0 ? membership(self, actor, venture_entity_get_organization_id(entity)) :
				(legacy_personal_spine(entity) ? membership(self, actor, 0) : NULL);
			if (member != NULL && personal_owner(self, actor, entity, 0))
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
						if (target == NULL || (personal_record(self, target) &&
						!VENTURE_IS_RECURRING_SCHEDULE(target) && !VENTURE_IS_DOCUMENT(target)) ||
							!venture_access_policy_can(self, actor, "read", target, NULL))
							return refuse(error, read);
					}
				}
				goto allowed;
			}
			return refuse(error, read);
		}
		if (tenant_administrator(self, actor)) goto allowed;
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
		/* A comment is conversation, not a change to the books: it is
		 * never proposed (comment_allows() says why). */
		(role == VENTURE_ORGANIZATION_ROLE_VIEWER && !venture_entity_is_persisted(entity) && 0 == g_strcmp0(action, "write") &&
		 !VENTURE_IS_COMMENT(entity));
}

gboolean
venture_access_policy_requires_approval(VentureAccessPolicy *self, const VentureAuthPrincipal *actor,
	const gchar *action, VentureEntity *entity, GError **error)
{
	g_autoptr(VentureEntity) member = NULL;
	g_autoptr(GError) veto = NULL;
	gint role;
	/* An organization is judged in itself, as venture_access_policy_can()
	 * judges it; its own organization column says nothing about who runs
	 * it, and reading it refused an owner her own profile as not found. */
	gint64 org = VENTURE_IS_ORGANIZATION(entity) ? venture_entity_get_id(entity) : venture_entity_get_organization_id(entity);
	if (st(self)->organization_id != 0 && org != st(self)->organization_id)
	{
		refuse(error, TRUE);
		return FALSE;
	}
	/* Workspace authority is not proposed for approval, and does not need a
	 * membership row in a later-created organization to write there. */
	if (administrator(actor) || tenant_administrator(self, actor))
		return FALSE;
	/* A new organization belongs to no organization yet. Its membership
	 * lookup would match any of the caller's, letting a viewer elsewhere
	 * propose one; the save's own guard refuses it instead. */
	if (VENTURE_IS_ORGANIZATION(entity) && org <= 0)
		return FALSE;
	member = membership(self, actor, org);
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
		gint minted = token_role(self, actor, org);
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
/*
 * The organizations a list read may keep rows from, or %NULL when every row
 * must go through the full check. For an ordinary business record, read by
 * somebody who is neither an administrator nor a workspace administrator,
 * venture_access_policy_can() ends in membership(): a row outside the
 * reader's active memberships is always refused. Reading those memberships
 * once lets a list pass over every other business's rows on a shared install
 * instead of refusing them one at a time, which cost each page several
 * statements per business. Whatever may be allowed some other way --
 * personal, reference and workspace records, comments, audit entries, users,
 * inbox rows -- is still decided row by row.
 */
static GHashTable *
member_organizations(VentureAccessPolicy *self, GType type)
{
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(VentureEntity) prototype = NULL;
	g_autoptr(GPtrArray) fields = NULL;
	VentureTenantService *tenant = venture_tenant_service_get(self->database);
	const VentureAuthPrincipal *actor = st(self)->actor;
	GHashTable *organizations;
	guint i;
	if (NULL == actor || !actor->authenticated || actor->user_id <= 0 || administrator(actor))
		return NULL;
	if (type == VENTURE_TYPE_COMMENT || type == VENTURE_TYPE_USER || type == VENTURE_TYPE_AUDIT_ENTRY ||
	    type == VENTURE_TYPE_NOTIFICATION || type == VENTURE_TYPE_WATCH)
		return NULL;
	if (!venture_entity_registry_is_type_enabled(venture_entity_registry_get_default(), "organization_membership"))
		return NULL;
	if (venture_tenant_service_is_enabled(tenant) &&
	    (venture_data_class_for_type(type) != VENTURE_DATA_CLASS_TENANT ||
	     venture_tenant_service_get_support_organization(tenant) > 0))
		return NULL;
	prototype = g_object_new(type, NULL);
	fields = venture_entity_get_field_specs(prototype);
	for (i = 0; i < fields->len; i++)
		if (venture_field_spec_get_flags(g_ptr_array_index(fields, i)) &
		    (VENTURE_COLUMN_FLAG_PERSONAL_OWNER | VENTURE_COLUMN_FLAG_OPTIONAL_PERSONAL_OWNER))
			return NULL;
	if (tenant_administrator(self, actor))
		return NULL;
	internal = venture_access_policy_enter(self, NULL);
	query = venture_query_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP);
	venture_query_set_limit(query, 0);
	venture_query_add_filter_int(query, "user-id", VENTURE_FILTER_OP_EQ, actor->user_id, NULL);
	venture_query_add_filter_string(query, "active", VENTURE_FILTER_OP_EQ, "true", NULL);
	rows = venture_database_find(self->database, query, NULL);
	if (NULL == rows)
		return NULL;
	organizations = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	for (i = 0; i < rows->len; i++) {
		gint64 *org = g_new(gint64, 1);
		*org = venture_entity_get_organization_id(g_ptr_array_index(rows, i));
		g_hash_table_add(organizations, org);
	}
	return organizations;
}
static void
unref_member(gpointer member)
{
	if (NULL != member)
		g_object_unref(member);
}
/* --- Paging a list a reader may only partly see ------------------------ */

/*
 * The organization a row belongs to for reading, as a column: an
 * organization row is its own organization.
 */
static const gchar *
organization_column(GType type)
{
	return g_type_is_a(type, VENTURE_TYPE_ORGANIZATION) ? "id" : "organization-id";
}

/*
 * Whether every read of this type by this reader is decided by the row's
 * organization alone, and if so which organizations. This is
 * venture_access_policy_check_read() worked out once for the whole type,
 * so a list can be bounded and counted in SQL instead of building every
 * row of the table to judge it in C (which is what made a fifty-row page
 * of a large table cost the whole table, twice: once for the page and
 * once for its total).
 *
 * It answers only where can() provably never looks past the
 * organization: no privacy field, none of the types with a rule of their
 * own, no hosted workspace, no "decide" handler that might veto a row,
 * and a role whose read rule is a property of the type (owner, admin,
 * editor, finance). Anything else -- a viewer's "owned" rows, an
 * accountant's documents -- returns FALSE and the caller judges row by
 * row as before. find_visible() still runs check_read() on every row the
 * fast path returns and falls back on the first refusal, so a mistake
 * here can hide a row but never show one.
 *
 * On TRUE, *@organizations is NULL for "every organization" or the set
 * of organization ids whose rows are visible (possibly empty).
 */
static gboolean
decided_by_organization(VentureAccessPolicy *self, GType type, GArray **organizations)
{
	const VentureAuthPrincipal *actor = st(self)->actor;
	g_autoptr(VentureEntity) prototype = NULL;
	g_autoptr(GPtrArray) fields = NULL;
	g_autoptr(GArray) visible = NULL;
	g_autoptr(GArray) candidates = NULL;
	guint i;

	*organizations = NULL;

	/* Types with a rule of their own in can() or in personal_owner_id(). */
	if (type == VENTURE_TYPE_COMMENT || type == VENTURE_TYPE_USER ||
	    type == VENTURE_TYPE_AUDIT_ENTRY || type == VENTURE_TYPE_NOTIFICATION ||
	    type == VENTURE_TYPE_WATCH || type == VENTURE_TYPE_BUSINESS_EVENT ||
	    type == VENTURE_TYPE_BUSINESS_EVENT_DELIVERY ||
	    type == VENTURE_TYPE_INTEGRATION_CONNECTION || type == VENTURE_TYPE_RECORD_LINK ||
	    type == VENTURE_TYPE_JOURNAL || type == VENTURE_TYPE_ACCOUNT)
		return FALSE;
	if (0 != g_strcmp0(st(self)->read_action, "read") && 0 != g_strcmp0(st(self)->read_action, "export"))
		return FALSE;
	if (venture_tenant_service_is_enabled(venture_tenant_service_get(self->database)))
		return FALSE;
	if (g_signal_has_handler_pending(self, self->decide_signal, 0, FALSE))
		return FALSE;

	prototype = g_object_new(type, NULL);
	fields = venture_entity_get_field_specs(prototype);
	for (i = 0; i < fields->len; i++)
		if (venture_field_spec_get_flags(g_ptr_array_index(fields, i)) &
		    (VENTURE_COLUMN_FLAG_PERSONAL_OWNER | VENTURE_COLUMN_FLAG_OPTIONAL_PERSONAL_OWNER |
		     VENTURE_COLUMN_FLAG_PRIVACY_PARENT | VENTURE_COLUMN_FLAG_PRIVATE_PARTICIPANT))
			return FALSE;

	/* No reader: only the organization pin applies. An administrator
	 * passes every row-level rule in can() for these types. */
	if (NULL == actor)
		return TRUE;
	if (!actor->authenticated)
		return FALSE;
	if (g_type_get_qdata(type, g_quark_from_static_string("venture-access-platform")) && !administrator(actor))
		return FALSE;
	if (administrator(actor))
		return TRUE;
	if (actor->user_id <= 0 ||
	    !venture_entity_registry_is_type_enabled(venture_entity_registry_get_default(), "organization_membership"))
		return FALSE;

	/* The organizations the reader is an active member of, or just the
	 * pinned one; membership() then says whether each really counts. */
	candidates = g_array_new(FALSE, FALSE, sizeof(gint64));
	if (0 != st(self)->organization_id)
		g_array_append_val(candidates, st(self)->organization_id);
	else
	{
		g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(self, NULL);
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP);
		g_autoptr(GPtrArray) rows = NULL;
		g_autoptr(GHashTable) seen = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);

		venture_query_set_limit(query, 0);
		venture_query_add_filter_int(query, "user-id", VENTURE_FILTER_OP_EQ, actor->user_id, NULL);
		venture_query_add_filter_string(query, "active", VENTURE_FILTER_OP_EQ, "true", NULL);
		rows = venture_database_find(self->database, query, NULL);
		if (NULL == rows)
			return FALSE;
		for (i = 0; i < rows->len; i++)
		{
			gint64 org = venture_entity_get_organization_id(g_ptr_array_index(rows, i));
			if (org > 0 && !g_hash_table_contains(seen, &org))
			{
				g_array_append_val(candidates, org);
				g_hash_table_add(seen, g_memdup2(&org, sizeof org));
			}
		}
	}

	visible = g_array_new(FALSE, FALSE, sizeof(gint64));
	for (i = 0; i < candidates->len; i++)
	{
		gint64 org = g_array_index(candidates, gint64, i);
		g_autoptr(VentureEntity) member = NULL;
		gint role = -1;
		gint token = -1;

		if (org <= 0)
			continue;
		member = membership(self, actor, org);
		if (NULL == member)
			continue;
		g_object_get(member, "role", &role, NULL);
		if (role != VENTURE_ORGANIZATION_ROLE_OWNER && role != VENTURE_ORGANIZATION_ROLE_ADMIN &&
		    role != VENTURE_ORGANIZATION_ROLE_EDITOR && role != VENTURE_ORGANIZATION_ROLE_FINANCE)
			return FALSE;
		if (!role_allows(self, actor, st(self)->read_action, prototype, role))
			continue;
		if (actor->token_id > 0)
		{
			token = token_role(self, actor, org);
			if (token < 0)
				continue;
			if (token != VENTURE_ORGANIZATION_ROLE_OWNER && token != VENTURE_ORGANIZATION_ROLE_ADMIN &&
			    token != VENTURE_ORGANIZATION_ROLE_EDITOR && token != VENTURE_ORGANIZATION_ROLE_FINANCE)
				return FALSE;
			if (!role_allows(self, actor, st(self)->read_action, prototype, token))
				continue;
		}
		g_array_append_val(visible, org);
	}

	*organizations = g_steal_pointer(&visible);
	return TRUE;
}

/*
 * The caller's query narrowed to what decided_by_organization() allows,
 * as a copy: the caller's own query is never changed. NULL when nothing
 * is visible at all.
 */
static VentureQuery *
narrowed_query(VentureAccessPolicy *self, VentureQuery *query, GArray *organizations)
{
	g_autoptr(VentureQuery) copy = venture_query_copy(query);
	GType type = venture_query_get_entity_type(query);
	guint i;

	if (0 != st(self)->organization_id &&
	    !venture_query_add_filter_int(copy, organization_column(type), VENTURE_FILTER_OP_EQ,
	                                  st(self)->organization_id, NULL))
		return NULL;
	if (NULL != organizations)
	{
		g_autoptr(GPtrArray) values = g_ptr_array_new_with_free_func(g_free);

		if (0 == organizations->len)
			return NULL;
		for (i = 0; i < organizations->len; i++)
			g_ptr_array_add(values, g_strdup_printf("%" G_GINT64_FORMAT,
				g_array_index(organizations, gint64, i)));
		if (!venture_query_add_filter(copy, organization_column(type), VENTURE_FILTER_OP_IN, values, NULL))
			return NULL;
	}
	return g_steal_pointer(&copy);
}

typedef struct
{
	VentureAccessPolicy		*policy;
	GHashTable			*members;
	GPtrArray			*result;	/* NULL when only counting */
	guint				 offset;
	guint				 limit;
	gint64				 visible;
	/* The reader the rows are judged as, kept while the cursor itself
	 * runs in the internal scope. */
	const VentureAuthPrincipal	*actor;
	gint64				 organization_id;
	gboolean			 shared_only;
	const gchar			*read_action;
} VisibleWalk;

/*
 * One row of the row-by-row path: the judgement find_visible() always
 * made, as the reader, stopping as soon as the page is full. The cursor
 * reads in the internal scope (venture_database_find_each() refuses any
 * other), so the reader is put back for the judgement and the internal
 * scope restored after it; a nested scope inside check_read() saves and
 * restores around whatever is current, so this balances.
 */
static gboolean
visible_walk_row(VentureEntity *entity, gpointer user_data)
{
	VisibleWalk *walk = user_data;
	VentureAccessPolicy *self = walk->policy;
	const VentureAuthPrincipal *inner_actor = st(self)->actor;
	gint64 inner_organization = st(self)->organization_id;
	gboolean inner_shared = st(self)->shared_only;
	const gchar *inner_action = st(self)->read_action;
	gint64 org = VENTURE_IS_ORGANIZATION(entity) ? venture_entity_get_id(entity) : venture_entity_get_organization_id(entity);
	gboolean readable;

	if (NULL != walk->members && !g_hash_table_contains(walk->members, &org))
		return TRUE;

	st(self)->actor = walk->actor;
	st(self)->organization_id = walk->organization_id;
	st(self)->shared_only = walk->shared_only;
	st(self)->read_action = walk->read_action;
	readable = venture_access_policy_check_read(self, entity, NULL);
	st(self)->actor = inner_actor;
	st(self)->organization_id = inner_organization;
	st(self)->shared_only = inner_shared;
	st(self)->read_action = inner_action;

	if (!readable)
		return TRUE;
	if (walk->visible++ < walk->offset)
		return TRUE;
	if (NULL == walk->result)
		return TRUE;
	g_ptr_array_add(walk->result, g_object_ref(entity));
	return walk->limit == 0 || walk->result->len < walk->limit;
}

/*
 * Reads the query row by row through one cursor, judging each row, until
 * the page is full -- not every row of the table first, as it used to.
 * Counting (result NULL) still reads every row, but keeps none of them.
 */
static gboolean
walk_visible(VentureAccessPolicy *self, VentureQuery *query, GPtrArray *result, gint64 *count, GError **error)
{
	g_autoptr(GHashTable) members = NULL;
	g_autoptr(VentureQuery) all = venture_query_copy(query);
	g_autoptr(VentureAccessScope) internal = NULL;
	VisibleWalk walk;
	gboolean ok;

	members = member_organizations(self, venture_query_get_entity_type(query));
	venture_query_set_limit(all, 0);
	venture_query_set_offset(all, 0);

	walk.policy = self;
	walk.members = members;
	walk.result = result;
	walk.offset = (NULL != result) ? venture_query_get_offset(query) : 0;
	walk.limit = (NULL != result) ? venture_query_get_limit(query) : 0;
	walk.visible = 0;
	walk.actor = st(self)->actor;
	walk.organization_id = st(self)->organization_id;
	walk.shared_only = st(self)->shared_only;
	walk.read_action = st(self)->read_action;

	internal = venture_access_policy_enter(self, NULL);
	ok = venture_database_find_each(self->database, all, visible_walk_row, &walk, error);
	g_clear_object(&internal);

	if (NULL != count)
		*count = walk.visible;
	return ok;
}

static GPtrArray *
find_visible(VentureAccessPolicy *self, VentureQuery *query, GError **error)
{
	g_autoptr(GArray) organizations = NULL;
	g_autoptr(GPtrArray) result = g_ptr_array_new_with_free_func(g_object_unref);
	GType type = venture_query_get_entity_type(query);

	if (decided_by_organization(self, type, &organizations))
	{
		g_autoptr(VentureQuery) narrowed = narrowed_query(self, query, organizations);
		g_autoptr(GPtrArray) rows = NULL;
		gboolean agreed = TRUE;
		guint i;

		if (NULL == narrowed)
			return g_steal_pointer(&result);
		{
			g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(self, NULL);
			rows = venture_database_find(self->database, narrowed, error);
		}
		if (NULL == rows)
			return NULL;
		/* Every row is still judged as it always was. A refusal means the
		 * shortcut was wrong for this reader: answer the slow way. */
		for (i = 0; i < rows->len && agreed; i++)
			agreed = venture_access_policy_check_read(self, g_ptr_array_index(rows, i), NULL);
		if (agreed)
			return g_steal_pointer(&rows);
		g_debug("access policy: organization shortcut disagreed for %s; judging row by row",
		        g_type_name(type));
	}

	if (!walk_visible(self, query, result, NULL, error))
		return NULL;
	return g_steal_pointer(&result);
}
/* The outermost list read by an actor keeps its answers until it ends;
 * a read nested inside it shares them. */
static void
begin_batch(VentureAccessPolicy *self)
{
	st(self)->batch_actor = st(self)->actor;
	st(self)->batch_principal = FALSE;
	st(self)->batch_tenant_administrator = -1;
	st(self)->batch_members = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, unref_member);
	st(self)->batch_resources = g_hash_table_new(g_direct_hash, g_direct_equal);
}

static void
end_batch(VentureAccessPolicy *self)
{
	st(self)->batch_actor = NULL;
	g_clear_pointer(&st(self)->batch_members, g_hash_table_unref);
	g_clear_pointer(&st(self)->batch_resources, g_hash_table_unref);
}

GPtrArray *
venture_access_policy_find(VentureAccessPolicy *self, VentureQuery *query, GError **error)
{
	GPtrArray *result;
	if (NULL == st(self)->actor || NULL != st(self)->batch_actor)
		return find_visible(self, query, error);
	begin_batch(self);
	result = find_visible(self, query, error);
	end_batch(self);
	return result;
}
/* Where other people's records land (the default organization) and what
 * a parent's reports roll up (the hierarchy) are workspace decisions. An
 * organization's own owner or admin keeps its profile, not these. */
static gboolean
organization_structure_changed(VentureEntity *entity, VentureEntity *previous)
{
	gboolean was_default = FALSE;
	gboolean is_default = FALSE;
	if (!VENTURE_IS_ORGANIZATION(entity))
		return FALSE;
	g_object_get(entity, "is-default", &is_default, NULL);
	if (NULL == previous)
		return is_default || reference(entity, "parent-id") != 0;
	g_object_get(previous, "is-default", &was_default, NULL);
	return !was_default != !is_default || reference(entity, "parent-id") != reference(previous, "parent-id");
}
static gboolean
check_mutation_authority(VentureAccessPolicy *self, VentureEntity *entity, const gchar *action, gboolean record_write, GError **error)
{
	g_autoptr(VentureEntity) previous = NULL;
	g_autoptr(VentureAccessScope) internal = NULL;
	if (record_write && !venture_tenant_service_check_write(venture_tenant_service_get(self->database), entity, error))
		return FALSE;
	if (NULL == st(self)->actor && st(self)->organization_id == 0)
		return TRUE;
	if (venture_entity_is_persisted(entity))
	{
		internal = venture_access_policy_enter(self, NULL);
		previous = venture_database_get(self->database, G_OBJECT_TYPE(entity), venture_entity_get_id(entity), error);
		g_clear_object(&internal);
		if (NULL == previous)
			return refuse(error, TRUE);
		if (!(st(self)->actor ? venture_access_policy_can(self, st(self)->actor, action, previous, error) :
			venture_access_policy_check_read(self, previous, error)))
			return FALSE;
	}
	if (NULL != st(self)->actor && !administrator(st(self)->actor) && !tenant_administrator(self, st(self)->actor) &&
	    organization_structure_changed(entity, previous))
		return refuse(error, FALSE);
	return st(self)->actor ? venture_access_policy_can(self, st(self)->actor, action, entity, error) :
		venture_access_policy_check_read(self, entity, error);
}

gboolean
venture_access_policy_check_write(VentureAccessPolicy *self, VentureEntity *entity, const gchar *action, GError **error)
{
	return check_mutation_authority(self, entity, action, TRUE, error);
}

gboolean
venture_access_policy_check_action(VentureAccessPolicy *self, VentureEntity *entity, VentureAction *action, GError **error)
{
	/* An action authorizes its declared service operation; every actual save
	 * still runs the stronger record-mutation guard inside the repository. */
	if (!venture_tenant_service_check_resource(venture_tenant_service_get(self->database), G_OBJECT(action), TRUE, error))
		return FALSE;
	return check_mutation_authority(self, entity, "write", FALSE, error);
}

gboolean
venture_access_policy_check_read(VentureAccessPolicy *self, VentureEntity *entity, GError **error)
{
	gint64 org = VENTURE_IS_ORGANIZATION(entity) ? venture_entity_get_id(entity) : venture_entity_get_organization_id(entity);
	if (st(self)->shared_only && personal_record(self, entity)) return refuse(error, TRUE);
	if (st(self)->organization_id != 0 && org != st(self)->organization_id) return refuse(error, TRUE);
	return NULL == st(self)->actor || venture_access_policy_can(self, st(self)->actor, st(self)->read_action, entity, error);
}
gint64
venture_access_policy_count(VentureAccessPolicy *self, VentureQuery *query, GError **error)
{
	g_autoptr(GArray) organizations = NULL;
	gint64 count = 0;
	GType type = venture_query_get_entity_type(query);

	if (decided_by_organization(self, type, &organizations))
	{
		g_autoptr(VentureQuery) narrowed = narrowed_query(self, query, organizations);
		g_autoptr(VentureAccessScope) internal = NULL;

		if (NULL == narrowed)
			return 0;
		venture_query_set_limit(narrowed, 0);
		venture_query_set_offset(narrowed, 0);
		internal = venture_access_policy_enter(self, NULL);
		return venture_database_count(self->database, narrowed, error);
	}

	/* Row by row, as a list read: the same batch answers, no rows kept. */
	if (NULL == st(self)->actor || NULL != st(self)->batch_actor)
		return walk_visible(self, query, NULL, &count, error) ? count : -1;
	begin_batch(self);
	if (!walk_visible(self, query, NULL, &count, error))
		count = -1;
	end_batch(self);
	return count;
}
/* Only these public routes authenticate with capabilities instead of local
 * sessions. Keep method and path shape exact as more routes are added. */
static gboolean
public_capability_request(HtmxRequest *request)
{
	const gchar *path = htmx_request_get_path(request);
	HtmxMethod method = htmx_request_get_method(request);
	const gchar *suffix;
	if (g_str_has_prefix(path, "/pay/"))
	{
		gchar uuid[37];
		guint i;
		if (strlen(path + 5) != 101 || path[41] != '.') return FALSE;
		memcpy(uuid, path + 5, 36); uuid[36] = '\0';
		if (!g_uuid_string_is_valid(uuid)) return FALSE;
		for (i = 42; i < 106; i++) if (!g_ascii_isxdigit(path[i])) return FALSE;
		return method == HTMX_METHOD_GET || method == HTMX_METHOD_POST;
	}
	if (!g_strcmp0(path, "/webhooks/stripe")) return method == HTMX_METHOD_POST;
	/* The platform's one Connect endpoint: it verifies the platform
	 * signature, then finds the business by the event's account. */
	if (!g_strcmp0(path, "/webhooks/stripe/connect")) return method == HTMX_METHOD_POST;
	if (g_str_has_prefix(path, "/webhooks/stripe/"))
	{
		const gchar *digit = path + strlen("/webhooks/stripe/");
		if (!*digit) return FALSE;
		while (g_ascii_isdigit(*digit)) digit++;
		return !*digit && method == HTMX_METHOD_POST;
	}
	if (g_str_has_prefix(path, "/f/") && path[3] != '\0')
		return method == HTMX_METHOD_POST && strchr(path + 3, '/') == NULL;
	if (g_str_has_prefix(path, "/book/") && path[6] != '\0' && path[6] != '/')
	{
		suffix = strchr(path + 6, '/');
		if (suffix != NULL && g_str_has_prefix(suffix, "/manage/") && suffix[8] != '\0' && strchr(suffix + 8, '/') == NULL)
			return method == HTMX_METHOD_GET || method == HTMX_METHOD_POST;
	}
	/* The forms door: /pub/form/<token> takes GET and POST; its /fragment
	 * and /schema only GET. Nothing deeper and nothing else. */
	if (g_str_has_prefix(path, "/pub/form/") && path[10] != '\0' && path[10] != '/')
	{
		suffix = strchr(path + 10, '/');
		if (!suffix) return method == HTMX_METHOD_GET || method == HTMX_METHOD_POST;
		if (((g_str_has_prefix(suffix, "/resume/") && suffix[8] != '\0') ||
		     (g_str_has_prefix(suffix, "/confirm/") && suffix[9] != '\0')) &&
		    strchr(suffix + (g_str_has_prefix(suffix, "/resume/") ? 8 : 9), '/') == NULL)
			return method == HTMX_METHOD_GET || method == HTMX_METHOD_POST;
		return method == HTMX_METHOD_GET && (!g_strcmp0(suffix, "/fragment") || !g_strcmp0(suffix, "/schema"));
	}
	if (!g_strcmp0(path, "/pub/forms.js")) return method == HTMX_METHOD_GET;
	/* One-click sequence stop. The token is the credential; a signed-in
	 * user with no membership must not be sent to /account instead. */
	if (g_str_has_prefix(path, "/sequences/u/") && path[13] != '\0' && strchr(path + 13, '/') == NULL)
		return method == HTMX_METHOD_GET || method == HTMX_METHOD_POST;
	/* The interface's stylesheets and scripts: /pub/ui/<version>/<name>. */
	if (g_str_has_prefix(path, "/pub/ui/") && path[8] != '\0' && path[8] != '/')
	{
		suffix = strchr(path + 8, '/');
		return method == HTMX_METHOD_GET && suffix != NULL && suffix[1] != '\0' && strchr(suffix + 1, '/') == NULL;
	}
	if (!g_str_has_prefix(path, "/q/") || path[3] == '\0' || path[3] == '/') return FALSE;
	suffix = strchr(path + 3, '/');
	if (!suffix) return method == HTMX_METHOD_GET || method == HTMX_METHOD_POST;
	return method == HTMX_METHOD_POST && !g_strcmp0(suffix, "/accept");
}

static gboolean
schedule_token_route(VentureDatabase *database, const VentureAuthPrincipal *actor, HtmxRequest *request)
{
	g_autoptr(VentureEntity) token = NULL;
	g_autofree gchar *scopes = NULL;
	const gchar *path = htmx_request_get_path(request);
	if (actor->token_id <= 0) return TRUE;
	token = venture_database_get(database, VENTURE_TYPE_API_TOKEN, actor->token_id, NULL);
	/* A missing row is not "some other token". Letting it through would
	 * open every editor route of the business the principal still names. */
	if (token == NULL) return FALSE;
	g_object_get(token, "scopes", &scopes, NULL);
	if (g_strcmp0(scopes, "lightsite:schedule")) return TRUE;
	return htmx_request_get_method(request) == HTMX_METHOD_POST &&
		(!g_strcmp0(path, "/api/v1/dunning_policy/0/actions/sweep") ||
		 !g_strcmp0(path, "/api/v1/mail/deliver"));
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
			if (!schedule_token_route(venture_context_get_database(context), actor, request)) {
				HtmxResponse *response = htmx_response_new();
				htmx_response_set_status(response, 403);
				htmx_context_set_response(http, response);
				return;
			}
			scope = venture_access_policy_enter(policy, actor);
			if (venture_tenant_service_get_support_organization(venture_tenant_service_get(venture_context_get_database(context))) > 0)
				st(policy)->organization_id = venture_tenant_service_get_support_organization(venture_tenant_service_get(venture_context_get_database(context)));
			/* /metrics belongs to no organization: a scraper's token has
			 * no membership and is answered there, not sent to /account. */
			if (!g_object_get_data(G_OBJECT(request), "venture-hosted-control") &&
			    !venture_access_policy_has_membership(policy, actor) &&
				0 != g_strcmp0(path, "/account") && 0 != g_strcmp0(path, "/look") &&
				0 != g_strcmp0(path, "/metrics") &&
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
		scope->state->read_action = "export";
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
		g_autofree gchar *snapshot = NULL, *scopes = NULL;
		guint i;
		g_object_get(entity, "scopes", &scopes, NULL);
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
			if (!g_strcmp0(scopes, "lightsite:schedule") &&
			    venture_entity_get_organization_id(member) != venture_entity_get_organization_id(entity)) continue;
			g_object_get(member, "role", &role, NULL);
			json_builder_set_member_name(builder, key);
			json_builder_add_int_value(builder, role);
		}
		/* Workspace administration is captured too, so an operation that
		 * needs it can require it both now and when the token was minted.
		 * The key cannot collide with an organization ID. */
		if (g_strcmp0(scopes, "lightsite:schedule") &&
		    venture_tenant_service_is_enabled(venture_tenant_service_get(database)) &&
			venture_tenant_service_is_member(venture_tenant_service_get(database), reference(entity, "user-id"), TRUE))
		{
			json_builder_set_member_name(builder, "workspace_role");
			json_builder_add_string_value(builder, "admin");
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

/* This validator runs under the repository lock for every writer. A generic
 * update cannot turn a private record into a shared one by moving its parent. */
gboolean venture_access_policy_validate_privacy(VentureDatabase *database, VentureEntity *entity,
	VentureEntity *previous, GError **error)
{
	g_autoptr(GPtrArray) fields = venture_entity_get_field_specs(entity);
	VentureAccessPolicy *policy = venture_database_get_access_policy(database);
	guint i;
	gboolean found = FALSE;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), FALSE);
	for (i = 0; i < fields->len; i++)
	{
		VentureFieldSpec *field = g_ptr_array_index(fields, i);
		if (venture_field_spec_get_flags(field) & VENTURE_COLUMN_FLAG_PRIVACY_PARENT) {
			found = TRUE;
			if (previous && declared_owner_id(policy, entity) != declared_owner_id(policy, previous)) {
				g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
					"A reference change cannot change private ownership");
				return FALSE;
			}
			continue;
		}
		if (!(venture_field_spec_get_flags(field) & VENTURE_COLUMN_FLAG_OPTIONAL_PERSONAL_OWNER)) continue;
		found = TRUE;
		if (previous && (reference(entity, field->name) != reference(previous, field->name) ||
			(personal_record(policy, previous) && venture_entity_get_organization_id(entity) != venture_entity_get_organization_id(previous))))
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Saved private ownership cannot be reassigned or made public");
			return FALSE;
		}
	}
	if (VENTURE_IS_RECORD_LINK(entity) || VENTURE_IS_JOURNAL(entity)) {
		found = TRUE;
		if (previous && declared_owner_id(policy, entity) != declared_owner_id(policy, previous)) {
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Saved private ownership cannot change");
			return FALSE;
		}
	}
	if (VENTURE_IS_ACCOUNT(entity)) {
		gint kind;
		gint64 owner = reference(entity, "private-owner-id");
		gint64 parent_id = reference(entity, "parent-id");
		gboolean shared = FALSE, was_shared = FALSE;
		const VentureAuthPrincipal *actor = venture_access_policy_get_actor(policy);
		g_object_get(entity, "household-shared", &shared, NULL);
		if (previous) g_object_get(previous, "household-shared", &was_shared, NULL);
		if (shared != was_shared && (owner <= 0 || (actor && actor->user_id != owner))) {
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED,
				"Only the personal account owner can change household sharing");
			return FALSE;
		}
		g_object_get(entity, "kind", &kind, NULL);
		if (owner > 0 && kind != VENTURE_ACCOUNT_KIND_ASSET && kind != VENTURE_ACCOUNT_KIND_LIABILITY) {
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
				"Personal accounts must be assets or liabilities; spending categories remain shared");
			return FALSE;
		}
		if (parent_id > 0) {
			g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(policy, NULL);
			g_autoptr(VentureEntity) parent = venture_database_get(database, VENTURE_TYPE_ACCOUNT, parent_id, NULL);
			gint64 parent_owner = parent ? declared_owner_id(policy, parent) : -1;
			if (parent_owner != 0 && parent_owner != owner) {
				g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
					"An account cannot belong beneath another member's private account");
				return FALSE;
			}
		}
	}
	if (!found || previous) return TRUE;
	{
		gint64 owner = declared_owner_id(policy, entity);
		g_autoptr(VentureEntity) user = NULL, member = NULL;
		g_autoptr(VentureAccessScope) internal = NULL;
		VentureAuthPrincipal principal;
		gboolean active = FALSE;
		if (owner == 0) return TRUE;
		if (owner < 0) goto invalid;
		internal = venture_access_policy_enter(policy, NULL);
		user = venture_database_get(database, VENTURE_TYPE_USER, owner, NULL);
		if (!user || venture_entity_is_deleted(user)) goto invalid;
		g_object_get(user, "active", &active, NULL);
		if (!active) goto invalid;
		principal.user_id = owner; principal.token_id = 0; principal.role = VENTURE_USER_ROLE_EDITOR;
		principal.name = NULL; principal.authenticated = TRUE;
		member = membership(policy, &principal, venture_entity_get_organization_id(entity));
		if (member) return TRUE;
	}
invalid:
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "A private owner must be an active member of this organization");
	return FALSE;
}
