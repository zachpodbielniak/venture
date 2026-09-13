/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>

/* Every surface must share the database's registry, rather than maintaining
 * its own declarations or a process-global action catalogue. */
static void
test_database_registry(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDatabase) database = venture_database_new("sqlite://:memory:", &error);
	GParamSpec *property;
	g_assert_no_error(error);
	property = g_object_class_find_property(G_OBJECT_GET_CLASS(database), "action-registry");
	g_assert_nonnull(property);
	g_assert_true(g_type_is_a(G_PARAM_SPEC_VALUE_TYPE(property), G_TYPE_OBJECT));
}

static gboolean
allow(VentureAction *action, VentureEntity *entity, const VentureActor *actor, GError **error)
{
	return TRUE;
}

static VentureEntity *
invoke(VentureAction *action, VentureEntity *entity, GHashTable *params,
	const VentureActor *actor, GError **error)
{
	guint *calls = venture_action_get_data(action);
	(*calls)++;
	return g_object_ref(entity);
}

static GError *
veto(VentureActionRegistry *registry, VentureAction *action, VentureEntity *entity, gpointer data)
{
	return g_error_new_literal(VENTURE_ERROR, VENTURE_ERROR_CONFLICT, "held by reviewer");
}

static void
test_dispatch(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDatabase) database = venture_database_new("sqlite://:memory:", &error);
	g_autoptr(VentureAction) action = g_object_new(VENTURE_TYPE_ACTION,
		"type-name", "organization", "name", "review", "label", "Review",
		"description", "Review this organization", "stageable", TRUE,
		"roles", VENTURE_USER_ROLE_EDITOR, NULL);
	g_autoptr(VentureEntity) entity = NULL;
	g_autoptr(GHashTable) params = g_hash_table_new(g_str_hash, g_str_equal);
	g_autoptr(GPtrArray) actions = NULL;
	g_autoptr(JsonNode) description = NULL;
	g_autoptr(VentureConfirmationStore) store = NULL;
	VentureConfirmation *confirmation;
	g_autofree gchar *confirmation_id = NULL;
	VentureActionRegistry *registry;
	guint calls = 0;
	gulong handler;
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(database, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	registry = venture_database_get_action_registry(database);
	g_assert_true(venture_action_registry_register(registry, action, allow, invoke, &calls, NULL, &error));
	g_assert_no_error(error);
	g_assert_true(venture_action_registry_lookup(registry, "organizations", "review") == action);
	actions = venture_action_registry_list_for_type(registry, "organization");
	g_assert_cmpuint(actions->len, ==, 1);
	description = venture_action_registry_describe(registry, "organization");
	g_assert_cmpuint(json_array_get_length(json_node_get_array(description)), ==, 1);
	g_assert_null(venture_action_registry_perform(registry, "organization", 1, "missing", params, NULL, VENTURE_USER_ROLE_OWNER, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_clear_error(&error);
	g_assert_null(venture_action_registry_perform(registry, "organization", 1, "review", params, NULL, VENTURE_USER_ROLE_VIEWER, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);
	handler = g_signal_connect(registry, "performing", G_CALLBACK(veto), NULL);
	g_assert_null(venture_action_registry_perform(registry, "organization", 1, "review", params, NULL, VENTURE_USER_ROLE_OWNER, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	g_assert_cmpuint(calls, ==, 0);
	g_clear_error(&error);
	g_signal_handler_disconnect(registry, handler);
	entity = venture_action_registry_perform(registry, "organization", 1, "review", params, NULL, VENTURE_USER_ROLE_OWNER, &error);
	g_assert_no_error(error);
	g_assert_nonnull(entity);
	g_assert_cmpuint(calls, ==, 1);
	store = venture_confirmation_store_new(database, 3600, 10);
	confirmation = venture_confirmation_store_stage_action(store, action, entity, params, NULL,
		VENTURE_USER_ROLE_OWNER, "test", &error);
	g_assert_no_error(error);
	g_assert_nonnull(confirmation);
	g_assert_cmpuint(calls, ==, 1);
	confirmation_id = g_strdup(venture_confirmation_get_id(confirmation));
	handler = g_signal_connect(registry, "performing", G_CALLBACK(veto), NULL);
	g_assert_false(venture_confirmation_store_approve_as(store, confirmation_id, "reviewer", VENTURE_USER_ROLE_OWNER, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	g_assert_cmpuint(calls, ==, 1);
	g_clear_error(&error);
	g_signal_handler_disconnect(registry, handler);
	g_assert_null(venture_confirmation_store_find(store, confirmation_id));
}

static void
test_parameter_contract(void)
{
	g_autoptr(GPtrArray) specs = g_ptr_array_new_with_free_func((GDestroyNotify)venture_field_spec_free);
	g_autoptr(GPtrArray) exposed = NULL;
	g_autoptr(VentureAction) action = NULL;
	g_autoptr(GHashTable) values = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)json_node_unref);
	g_autoptr(GError) error = NULL;
	VentureFieldSpec *choice = venture_field_spec_new("decision", "Decision", VENTURE_FIELD_KIND_ENUM);
	JsonNode *value = json_node_new(JSON_NODE_VALUE);
	choice->required = TRUE;
	choice->choices = g_strsplit("yes,no", ",", -1);
	g_ptr_array_add(specs, choice);
	action = g_object_new(VENTURE_TYPE_ACTION, "type-name", "organization", "name", "decide",
		"label", "Decide", "description", "Record a decision", "parameters", specs, NULL);
	json_node_set_string(value, "invented");
	g_hash_table_insert(values, g_strdup("decision"), value);
	g_assert_false(venture_action_validate_parameters(action, values, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	g_object_get(action, "parameters", &exposed, NULL);
	g_ptr_array_set_size(exposed, 0);
	g_hash_table_remove_all(values);
	g_assert_false(venture_action_validate_parameters(action, values, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

static void
test_parameter_registration(void)
{
	g_autoptr(VentureDatabase) db = venture_database_new("sqlite://:memory:", NULL);
	g_autoptr(GPtrArray) specs = g_ptr_array_new_with_free_func((GDestroyNotify)venture_field_spec_free);
	g_autoptr(VentureAction) action = NULL;
	g_autoptr(GError) error = NULL;
	guint calls = 0;
	g_ptr_array_add(specs, venture_field_spec_new("id", "Identity", VENTURE_FIELD_KIND_INTEGER));
	action = g_object_new(VENTURE_TYPE_ACTION, "type-name", "organization", "name", "bad",
		"label", "Bad", "description", "Cannot override the target ID", "parameters", specs, NULL);
	g_assert_false(venture_action_registry_register(venture_database_get_action_registry(db), action,
		allow, invoke, &calls, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	venture_entity_registry_register_builtins(venture_entity_registry_get_default());
	g_test_add_func("/actions/database-registry", test_database_registry);
	g_test_add_func("/actions/dispatch-role-veto", test_dispatch);
	g_test_add_func("/actions/parameter-contract", test_parameter_contract);
	g_test_add_func("/actions/parameter-registration", test_parameter_registration);
	return g_test_run();
}
