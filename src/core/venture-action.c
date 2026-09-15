/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

struct _VentureAction
{
	GObject parent_instance;
	gchar *type_name;
	gchar *name;
	gchar *label;
	gchar *description;
	gchar *subject_parameter;
	GPtrArray *parameters;
	gboolean stageable;
	gboolean type_level;
	gboolean service_transaction;
	VentureUserRole roles;
	VentureActionAllowed allowed;
	VentureActionInvoke invoke;
	gpointer data;
	GDestroyNotify destroy;
};
G_DEFINE_FINAL_TYPE(VentureAction, venture_action, G_TYPE_OBJECT)
enum { PROP_ZERO, PROP_TYPE_NAME, PROP_NAME, PROP_LABEL, PROP_DESCRIPTION,
	PROP_PARAMETERS, PROP_STAGEABLE, PROP_ROLES, PROP_TYPE_LEVEL, PROP_SUBJECT_PARAMETER, PROP_SERVICE_TRANSACTION };

static void
action_set_property(GObject *object, guint id, const GValue *value, GParamSpec *spec)
{
	VentureAction *self = VENTURE_ACTION(object);
	guint i;
	GPtrArray *parameters;
	switch (id)
	{
	case PROP_TYPE_NAME: self->type_name = g_value_dup_string(value); break;
	case PROP_NAME: self->name = g_value_dup_string(value); break;
	case PROP_LABEL: self->label = g_value_dup_string(value); break;
	case PROP_DESCRIPTION: self->description = g_value_dup_string(value); break;
	case PROP_SUBJECT_PARAMETER: self->subject_parameter = g_value_dup_string(value); break;
	case PROP_STAGEABLE: self->stageable = g_value_get_boolean(value); break;
	case PROP_ROLES: self->roles = g_value_get_enum(value); break;
	case PROP_TYPE_LEVEL: self->type_level = g_value_get_boolean(value); break;
	case PROP_SERVICE_TRANSACTION: self->service_transaction = g_value_get_boolean(value); break;
	case PROP_PARAMETERS:
		parameters = g_value_get_boxed(value);
		for (i = 0; parameters && i < parameters->len; i++)
			g_ptr_array_add(self->parameters, venture_field_spec_copy(g_ptr_array_index(parameters, i)));
		break;
	default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
	}
}
static void
action_get_property(GObject *object, guint id, GValue *value, GParamSpec *spec)
{
	VentureAction *self = VENTURE_ACTION(object);
	switch (id)
	{
	case PROP_TYPE_NAME: g_value_set_string(value, self->type_name); break;
	case PROP_NAME: g_value_set_string(value, self->name); break;
	case PROP_LABEL: g_value_set_string(value, self->label); break;
	case PROP_DESCRIPTION: g_value_set_string(value, self->description); break;
	case PROP_SUBJECT_PARAMETER: g_value_set_string(value, self->subject_parameter); break;
	case PROP_STAGEABLE: g_value_set_boolean(value, self->stageable); break;
	case PROP_ROLES: g_value_set_enum(value, self->roles); break;
	case PROP_TYPE_LEVEL: g_value_set_boolean(value, self->type_level); break;
	case PROP_SERVICE_TRANSACTION: g_value_set_boolean(value, self->service_transaction); break;
	case PROP_PARAMETERS:
		{
			GPtrArray *copy = g_ptr_array_new_with_free_func((GDestroyNotify)venture_field_spec_free);
			guint i;
			for (i = 0; i < self->parameters->len; i++)
				g_ptr_array_add(copy, venture_field_spec_copy(g_ptr_array_index(self->parameters, i)));
			g_value_take_boxed(value, copy);
			break;
		}
	default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
	}
}
static void
action_finalize(GObject *object)
{
	VentureAction *self = VENTURE_ACTION(object);
	g_free(self->type_name);
	g_free(self->name);
	g_free(self->label);
	g_free(self->description);
	g_free(self->subject_parameter);
	g_ptr_array_unref(self->parameters);
	if (self->destroy) self->destroy(self->data);
	G_OBJECT_CLASS(venture_action_parent_class)->finalize(object);
}
static void
venture_action_class_init(VentureActionClass *klass)
{
	GObjectClass *object = G_OBJECT_CLASS(klass);
	GParamFlags flags = G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS;
	object->set_property = action_set_property;
	object->get_property = action_get_property;
	object->finalize = action_finalize;
	g_object_class_install_property(object, PROP_TYPE_NAME, g_param_spec_string("type-name", "Type", "Canonical record type", NULL, flags));
	g_object_class_install_property(object, PROP_NAME, g_param_spec_string("name", "Name", "Action key", NULL, flags));
	g_object_class_install_property(object, PROP_LABEL, g_param_spec_string("label", "Label", "Button label", NULL, flags));
	g_object_class_install_property(object, PROP_DESCRIPTION, g_param_spec_string("description", "Description", "What this action does", NULL, flags));
	g_object_class_install_property(object, PROP_SUBJECT_PARAMETER, g_param_spec_string("subject-parameter", "Subject parameter", "JSON parameter containing the new record used for authorization", NULL, flags));
	g_object_class_install_property(object, PROP_PARAMETERS, g_param_spec_boxed("parameters", "Parameters", "VentureFieldSpec declarations", G_TYPE_PTR_ARRAY, flags));
	g_object_class_install_property(object, PROP_TYPE_LEVEL, g_param_spec_boolean("type-level", "Type level", "Creates a new record; use ID zero", FALSE, flags));
	g_object_class_install_property(object, PROP_SERVICE_TRANSACTION, g_param_spec_boolean("service-transaction", "Service transaction", "The service owns atomicity and any posting approval before its first write", FALSE, flags));
	g_object_class_install_property(object, PROP_STAGEABLE, g_param_spec_boolean("stageable", "Stageable", "Can await approval", FALSE, flags));
	g_object_class_install_property(object, PROP_ROLES, g_param_spec_enum("roles", "Roles", "Minimum authenticated role", VENTURE_TYPE_USER_ROLE, VENTURE_USER_ROLE_EDITOR, flags));
}
static void
venture_action_init(VentureAction *self)
{
	self->parameters = g_ptr_array_new_with_free_func((GDestroyNotify)venture_field_spec_free);
}
gpointer
venture_action_get_data(VentureAction *self)
{
	return self->data;
}

struct _VentureActionRegistry
{
	GObject parent_instance;
	GWeakRef database;
	GPtrArray *actions;
};
G_DEFINE_FINAL_TYPE(VentureActionRegistry, venture_action_registry, G_TYPE_OBJECT)
enum { PERFORMING, PERFORMED, N_SIGNALS };
static guint signals[N_SIGNALS];
static gboolean
first_error(GSignalInvocationHint *hint, GValue *accum, const GValue *value, gpointer data)
{
	if (g_value_get_boxed(value))
	{
		g_value_copy(value, accum);
		return FALSE;
	}
	return TRUE;
}
static void
registry_set_property(GObject *object, guint id, const GValue *value, GParamSpec *spec)
{
	if (1 == id) g_weak_ref_set(&VENTURE_ACTION_REGISTRY(object)->database, g_value_get_object(value));
	else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}
static void
registry_get_property(GObject *object, guint id, GValue *value, GParamSpec *spec)
{
	if (1 == id) g_value_take_object(value, g_weak_ref_get(&VENTURE_ACTION_REGISTRY(object)->database));
	else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}
static void
registry_finalize(GObject *object)
{
	VentureActionRegistry *self = VENTURE_ACTION_REGISTRY(object);
	g_weak_ref_clear(&self->database);
	g_ptr_array_unref(self->actions);
	G_OBJECT_CLASS(venture_action_registry_parent_class)->finalize(object);
}
static void
venture_action_registry_class_init(VentureActionRegistryClass *klass)
{
	GObjectClass *object = G_OBJECT_CLASS(klass);
	object->set_property = registry_set_property;
	object->get_property = registry_get_property;
	object->finalize = registry_finalize;
	g_object_class_install_property(object, 1, g_param_spec_object("database", "Database", "Owning database",
		VENTURE_TYPE_DATABASE, G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
	/**
	 * VentureActionRegistry::performing:
	 * @self: registry
	 * @action: declaration
	 * @entity: record snapshot
	 *
	 * RUN_LAST after eligibility and parameter validation, before invoke.
	 * First error vetoes. Eligibility is rechecked after signal handlers.
	 * Returns: (transfer full) (nullable): veto reason
	 */
	signals[PERFORMING] = g_signal_new("performing", G_TYPE_FROM_CLASS(klass),
		G_SIGNAL_RUN_LAST, 0, first_error, NULL, NULL, G_TYPE_ERROR, 2,
		VENTURE_TYPE_ACTION, VENTURE_TYPE_ENTITY);
	/**
	 * VentureActionRegistry::performed:
	 * @self: registry
	 * @action: declaration
	 * @entity: resulting record
	 *
	 * After invoke and this operation's successful commit. An enclosing
	 * transaction may still roll back; external effects must await its commit.
	 */
	signals[PERFORMED] = g_signal_new("performed", G_TYPE_FROM_CLASS(klass),
		G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 2,
		VENTURE_TYPE_ACTION, VENTURE_TYPE_ENTITY);
}
static void
venture_action_registry_init(VentureActionRegistry *self)
{
	g_weak_ref_init(&self->database, NULL);
	self->actions = g_ptr_array_new_with_free_func(g_object_unref);
}

gboolean
venture_action_registry_register(VentureActionRegistry *self, VentureAction *action,
	VentureActionAllowed allowed, VentureActionInvoke invoke, gpointer data,
	GDestroyNotify destroy, GError **error)
{
	guint i;
	GType type = venture_entity_registry_lookup_any(venture_entity_registry_get_default(), action->type_name);
	if (!type || !allowed || !invoke || action->invoke ||
		!action->name || !g_regex_match_simple("^[a-z][a-z0-9_]*$", action->name, 0, 0) ||
		venture_string_is_empty(action->label) || venture_string_is_empty(action->description))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "An action needs a registered type, name, label, description and service callbacks");
		return FALSE;
	}
	{
		g_autoptr(VentureEntity) prototype = g_object_new(type, NULL);
		g_autoptr(GHashTable) names = g_hash_table_new(g_str_hash, g_str_equal);
		if (0 != g_strcmp0(action->type_name, venture_entity_get_entity_name(prototype)))
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "Register actions under the canonical singular type name");
			return FALSE;
		}
		for (i = 0; i < action->parameters->len; i++)
		{
			VentureFieldSpec *spec = g_ptr_array_index(action->parameters, i);
			if (!spec || !spec->name || !g_regex_match_simple("^[a-z][a-z0-9_]*$", spec->name, 0, 0) ||
				0 == g_strcmp0(spec->name, "id") || g_hash_table_contains(names, spec->name) ||
				(spec->flags & VENTURE_COLUMN_FLAG_SENSITIVE))
			{
				g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "Action parameters need unique wire names; id is reserved and sensitive inputs need a dedicated service interface");
				return FALSE;
			}
			g_hash_table_add(names, spec->name);
		}
		if (action->subject_parameter && (!action->type_level || !g_hash_table_contains(names, action->subject_parameter)))
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "A subject parameter must name a declared parameter of a type-level action");
			return FALSE;
		}
	}
	for (i = 0; i < self->actions->len; i++)
	{
		VentureAction *existing = g_ptr_array_index(self->actions, i);
		if (0 == g_strcmp0(existing->type_name, action->type_name) && 0 == g_strcmp0(existing->name, action->name))
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_ALREADY_EXISTS, "Action already registered");
			return FALSE;
		}
	}
	action->allowed = allowed;
	action->invoke = invoke;
	action->data = data;
	action->destroy = destroy;
	g_ptr_array_add(self->actions, g_object_ref(action));
	return TRUE;
}
VentureAction *
venture_action_registry_lookup(VentureActionRegistry *self, const gchar *type_name, const gchar *name)
{
	GType type = venture_entity_registry_lookup(venture_entity_registry_get_default(), type_name);
	guint i;
	if (!type) return NULL;
	for (i = 0; i < self->actions->len; i++)
	{
		VentureAction *action = g_ptr_array_index(self->actions, i);
		if (type == venture_entity_registry_lookup(venture_entity_registry_get_default(), action->type_name) &&
			0 == g_strcmp0(name, action->name)) return action;
	}
	return NULL;
}
GPtrArray *
venture_action_registry_list_for_type(VentureActionRegistry *self, const gchar *type_name)
{
	GPtrArray *result = g_ptr_array_new();
	guint i;
	for (i = 0; i < self->actions->len; i++)
	{
		VentureAction *action = g_ptr_array_index(self->actions, i);
		if (venture_action_registry_lookup(self, type_name, action->name) == action)
			g_ptr_array_add(result, action);
	}
	return result;
}
gboolean
venture_action_registry_allowed(VentureActionRegistry *self, VentureAction *action,
	VentureEntity *entity, const VentureActor *actor, VentureUserRole role, GError **error)
{
	g_autoptr(GDateTime) deleted = NULL;
	if (venture_action_registry_lookup(self, action->type_name, action->name) != action ||
		G_OBJECT_TYPE(entity) != venture_entity_registry_lookup(venture_entity_registry_get_default(), action->type_name))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Action is unavailable for this record type");
		return FALSE;
	}
	if ((VENTURE_USER_ROLE_SERVICE == role ? VENTURE_USER_ROLE_EDITOR : role) >
		(VENTURE_USER_ROLE_SERVICE == action->roles ? VENTURE_USER_ROLE_EDITOR : action->roles))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED, "%s requires the %s role", action->label,
			venture_enum_to_nick(VENTURE_TYPE_USER_ROLE, action->roles));
		return FALSE;
	}
	if (action->type_level != (0 == venture_entity_get_id(entity)))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Type-level actions require ID zero; record actions require a saved record");
		return FALSE;
	}
	g_object_get(entity, "deleted-at", &deleted, NULL);
	if (deleted)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT, "Actions require a live record");
		return FALSE;
	}
	return action->allowed(action, entity, actor, error);
}

gboolean
venture_action_validate_parameters(VentureAction *self, GHashTable *params, GError **error)
{
	GHashTableIter iter;
	gpointer key;
	guint i;
	g_hash_table_iter_init(&iter, params);
	while (g_hash_table_iter_next(&iter, &key, NULL))
	{
		for (i = 0; i < self->parameters->len; i++)
			if (0 == g_strcmp0(key, venture_field_spec_get_name(g_ptr_array_index(self->parameters, i)))) break;
		if (i == self->parameters->len)
		{
			venture_set_error_validation(error, key, "Unknown action parameter");
			return FALSE;
		}
	}
	for (i = 0; i < self->parameters->len; i++)
	{
		VentureFieldSpec *spec = g_ptr_array_index(self->parameters, i);
		JsonNode *node = g_hash_table_lookup(params, spec->name);
		gboolean valid = TRUE;
		if (!node || JSON_NODE_HOLDS_NULL(node)) valid = !spec->required;
		else if (VENTURE_FIELD_KIND_JSON == spec->kind) valid = TRUE;
		else if (!JSON_NODE_HOLDS_VALUE(node)) valid = FALSE;
		else if (VENTURE_FIELD_KIND_INTEGER == spec->kind || VENTURE_FIELD_KIND_REFERENCE == spec->kind)
			valid = G_TYPE_INT64 == json_node_get_value_type(node);
		else if (VENTURE_FIELD_KIND_BOOLEAN == spec->kind)
			valid = G_TYPE_BOOLEAN == json_node_get_value_type(node);
		else if (VENTURE_FIELD_KIND_DOUBLE == spec->kind)
			valid = G_TYPE_DOUBLE == json_node_get_value_type(node) || G_TYPE_INT64 == json_node_get_value_type(node);
		else
		{
			valid = G_TYPE_STRING == json_node_get_value_type(node);
			if (valid && spec->required) valid = !venture_string_is_empty(json_node_get_string(node));
			if (valid && (VENTURE_FIELD_KIND_DATE == spec->kind || VENTURE_FIELD_KIND_DATETIME == spec->kind))
			{
				g_autoptr(GDateTime) date = venture_time_from_string(json_node_get_string(node), NULL);
				valid = NULL != date;
			}
			if (valid && VENTURE_FIELD_KIND_MONEY == spec->kind)
			{
				g_autoptr(VentureMoney) money = venture_money_from_string(json_node_get_string(node), "USD", NULL);
				valid = NULL != money;
			}
		}
		if (valid && node && JSON_NODE_HOLDS_VALUE(node))
		{
			GValue value = G_VALUE_INIT;
			json_node_get_value(node, &value);
			valid = venture_field_spec_validate(spec, &value, error);
			g_value_unset(&value);
			if (!valid) return FALSE;
		}
		if (!valid)
		{
			venture_set_error_validation(error, spec->name, "Missing or invalid action parameter");
			return FALSE;
		}
	}
	return TRUE;
}
gboolean
venture_action_prepare_target(VentureAction *self, VentureEntity *entity,
	GHashTable *params, GError **error)
{
	JsonNode *node;
	g_autoptr(JsonNode) parsed = NULL;
	if (!self->subject_parameter) return TRUE;
	/* A type-level action declares which nested record it will write so
	 * queue visibility and policy plugins see its real organization. */
	if (venture_entity_get_id(entity) != 0)
	{
		venture_set_error_validation(error, "id", "Type-level actions require a new record");
		return FALSE;
	}
	node = g_hash_table_lookup(params, self->subject_parameter);
	if (node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING)
	{
		parsed = venture_json_parse(json_node_get_string(node), error);
		if (!parsed) return FALSE;
		node = parsed;
	}
	if (!node || !JSON_NODE_HOLDS_OBJECT(node))
	{
		venture_set_error_validation(error, self->subject_parameter, "The action subject must be a record object");
		return FALSE;
	}
	if (!venture_serializable_from_json(VENTURE_SERIALIZABLE(entity), node, error)) return FALSE;
	if (venture_entity_get_id(entity) != 0 || venture_entity_get_version(entity) != 0)
	{
		venture_set_error_validation(error, "id", "Type-level actions require a new, unversioned record");
		return FALSE;
	}
	return TRUE;
}

static VentureEntity *
action_registry_perform_internal(VentureActionRegistry *self, const gchar *type_name,
	gint64 id, const gchar *name, GHashTable *params, const VentureActor *actor,
	VentureUserRole role, GError **error)
{
	g_autoptr(VentureDatabase) db = g_weak_ref_get(&self->database);
	g_autoptr(VentureEntity) entity = NULL;
	g_autoptr(VentureEntity) result = NULL;
	g_autoptr(GError) veto_error = NULL;
	VentureAction *action = venture_action_registry_lookup(self, type_name, name);
	GType type;
	if (!db || !action)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Unknown or unavailable record action");
		return NULL;
	}
	type = venture_entity_registry_lookup(venture_entity_registry_get_default(), type_name);
	if (!action->service_transaction && !venture_database_begin(db, error)) return NULL;
	entity = (0 == id && action->type_level) ? g_object_new(type, NULL) : venture_database_get(db, type, id, error);
	if (!entity || !venture_action_validate_parameters(action, params, error) ||
		!venture_action_prepare_target(action, entity, params, error) ||
		!venture_action_registry_allowed(self, action, entity, actor, role, error) ||
		!venture_access_policy_check_write(venture_database_get_access_policy(db), entity, "write", error)) goto fail;
	venture_accounting_operation_suspend(db);
	g_signal_emit(self, signals[PERFORMING], 0, action, entity, &veto_error);
	venture_accounting_operation_resume(db);
	if (veto_error)
	{
		g_propagate_error(error, g_steal_pointer(&veto_error));
		goto fail;
	}
	/* Handlers cannot change the selected record through a borrowed object. */
	g_clear_object(&entity);
	entity = (0 == id && action->type_level) ? g_object_new(type, NULL) : venture_database_get(db, type, id, error);
	if (!entity || !venture_action_prepare_target(action, entity, params, error) ||
		!venture_action_registry_allowed(self, action, entity, actor, role, error) ||
		!venture_access_policy_check_write(venture_database_get_access_policy(db), entity, "write", error)) goto fail;
	result = action->invoke(action, entity, params, actor, error);
	if (!result) goto fail;
	if (!action->service_transaction && !venture_database_commit(db, error)) return NULL;
	venture_accounting_operation_suspend(db);
	g_signal_emit(self, signals[PERFORMED], 0, action, result);
	venture_accounting_operation_resume(db);
	return g_steal_pointer(&result);
fail:
	if (!action->service_transaction) venture_database_rollback(db);
	return NULL;
}
VentureEntity *
venture_action_registry_perform(VentureActionRegistry *self, const gchar *type_name,
	gint64 id, const gchar *name, GHashTable *params, const VentureActor *actor,
	VentureUserRole role, GError **error)
{
	g_autoptr(VentureDatabase) database = g_weak_ref_get(&self->database);
	g_autoptr(VentureEntity) subject = NULL;
	g_autoptr(VentureEntity) result = NULL;
	g_autoptr(VentureAccountingOperation) operation = NULL;
	g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
	g_autofree gchar *json = NULL;
	g_autoptr(GList) keys = params != NULL ? g_hash_table_get_keys(params) : NULL;
	GList *item;
	VentureAction *action = venture_action_registry_lookup(self, type_name, name);
	GType type = venture_entity_registry_lookup(venture_entity_registry_get_default(), type_name);
	JsonObject *object = json_object_new();
	json_node_take_object(node, object);
	if (database == NULL || action == NULL || type == G_TYPE_INVALID)
		return action_registry_perform_internal(self, type_name, id, name, params, actor, role, error);
	subject = id == 0 && action->type_level ? g_object_new(type, NULL) : venture_database_get(database, type, id, error);
	if (subject == NULL || !venture_action_validate_parameters(action, params, error) ||
		!venture_action_prepare_target(action, subject, params, error) ||
		!venture_action_registry_allowed(self, action, subject, actor, role, error) ||
		!venture_access_policy_check_write(venture_database_get_access_policy(database), subject, "write", error))
		return NULL;
	if (!action->service_transaction && venture_accounting_operation_is_financial(subject))
	{
		/* Parameter maps are unordered; preserve array order inside each value. */
		keys = g_list_sort(keys, (GCompareFunc)g_strcmp0);
		for (item = keys; item != NULL; item = item->next)
			json_object_set_member(object, item->data, json_node_copy(g_hash_table_lookup(params, item->data)));
		json = venture_json_to_string(node, FALSE);
		operation = venture_accounting_operation_begin(database, "record.action", subject, NULL,
			g_variant_new("(sss)", type_name, name, json), venture_entity_get_organization_id(subject), actor, error);
		if (operation == NULL)
			return NULL;
	}
	result = action_registry_perform_internal(self, type_name, id, name, params, actor, role, error);
	if (result == NULL || (operation != NULL && !venture_accounting_operation_finish(operation, error)))
		return NULL;
	return g_steal_pointer(&result);
}

GHashTable *
venture_action_parameters_from_json(JsonNode *node, GError **error)
{
	GHashTable *result;
	g_autoptr(JsonParser) parser = NULL;
	g_autofree gchar *text = NULL;
	g_autoptr(GList) members = NULL;
	GList *item;
	JsonObject *object;
	if (!node || !JSON_NODE_HOLDS_OBJECT(node))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Action parameters must be a JSON object");
		return NULL;
	}
	/* JSON node copying shares nested containers; round-trip detaches them. */
	text = venture_json_to_string(node, FALSE);
	parser = json_parser_new();
	if (!json_parser_load_from_data(parser, text, -1, error)) return NULL;
	object = json_node_get_object(json_parser_get_root(parser));
	members = json_object_get_members(object);
	result = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)json_node_unref);
	for (item = members; item; item = item->next)
		g_hash_table_insert(result, g_strdup(item->data), json_node_ref(json_object_get_member(object, item->data)));
	return result;
}
JsonNode *
venture_action_registry_describe(VentureActionRegistry *self, const gchar *type_name)
{
	g_autoptr(GPtrArray) actions = venture_action_registry_list_for_type(self, type_name);
	JsonArray *array = json_array_new();
	JsonNode *result = json_node_new(JSON_NODE_ARRAY);
	guint i, j;
	for (i = 0; i < actions->len; i++)
	{
		VentureAction *action = g_ptr_array_index(actions, i);
		JsonObject *object = json_object_new();
		JsonArray *parameters = json_array_new();
		JsonObject *schema = json_object_new();
		JsonObject *properties = json_object_new();
		JsonArray *required = json_array_new();
		json_object_set_string_member(object, "name", action->name);
		json_object_set_string_member(object, "type_name", action->type_name);
		json_object_set_string_member(object, "label", action->label);
		json_object_set_string_member(object, "description", action->description);
		if (action->subject_parameter) json_object_set_string_member(object, "subject_parameter", action->subject_parameter);
		json_object_set_boolean_member(object, "stageable", action->stageable);
		json_object_set_boolean_member(object, "type_level", action->type_level);
		json_object_set_string_member(object, "roles", venture_enum_to_nick(VENTURE_TYPE_USER_ROLE, action->roles));
		for (j = 0; j < action->parameters->len; j++)
		{
			VentureFieldSpec *spec = g_ptr_array_index(action->parameters, j);
			json_array_add_element(parameters, venture_field_spec_to_json(spec));
			json_object_set_member(properties, spec->name, venture_field_spec_to_json_schema(spec));
			if (spec->required) json_array_add_string_element(required, spec->name);
		}
		json_object_set_string_member(schema, "type", "object");
		json_object_set_object_member(schema, "properties", properties);
		json_object_set_array_member(schema, "required", required);
		json_object_set_boolean_member(schema, "additionalProperties", FALSE);
		json_object_set_object_member(object, "input_schema", schema);
		json_object_set_array_member(object, "parameters", parameters);
		json_array_add_object_element(array, object);
	}
	json_node_take_array(result, array);
	return result;
}
