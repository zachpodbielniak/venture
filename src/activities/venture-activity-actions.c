/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include "venture-activity-private.h"

GPtrArray *
venture_activity_call_parameters(void)
{
	static const gchar *const fields[] = {
		"subject", "owner", "outcome", "call-direction", "call-duration", "call-occurred-at",
		"call-outcome", "call-recording-url", "call-transcript", "call-external-source",
		"call-external-id", "call-followup-subject", "call-followup-owner",
		"call-followup-due-at", "call-followup-remind-at", NULL
	};
	g_autoptr(VentureEntity) prototype = VENTURE_ENTITY(venture_activity_new());
	g_autoptr(GPtrArray) all = venture_entity_get_field_specs(prototype);
	GPtrArray *parameters = g_ptr_array_new_with_free_func((GDestroyNotify)venture_field_spec_free);
	guint i;
	for (i = 0; i < all->len; i++)
	{
		VentureFieldSpec *field = g_ptr_array_index(all, i);
		VentureFieldSpec *spec;
		if (!g_strv_contains(fields, field->name))
			continue;
		spec = venture_field_spec_copy(field);
		g_strdelimit(spec->name, "-", '_');
		spec->required = g_str_equal(spec->name, "subject") || g_str_equal(spec->name, "call_direction") ||
			g_str_equal(spec->name, "call_duration") || g_str_equal(spec->name, "call_occurred_at") ||
			g_str_equal(spec->name, "call_outcome");
		if (spec->kind == VENTURE_FIELD_KIND_STRING || spec->kind == VENTURE_FIELD_KIND_TEXT)
			spec->max_length = g_str_equal(spec->name, "call_transcript") ? 65536 :
				g_str_equal(spec->name, "outcome") ? 4096 : 1024;
		if (spec->kind == VENTURE_FIELD_KIND_ENUM)
		{
			GParamSpec *property = g_object_class_find_property(G_OBJECT_GET_CLASS(prototype), field->name);
			GEnumClass *values = g_type_class_ref(G_PARAM_SPEC_VALUE_TYPE(property));
			GPtrArray *choices = g_ptr_array_new();
			GPtrArray *labels = g_ptr_array_new();
			guint j;
			/* Unknown preserves old records, but is not a new call result. */
			for (j = 0; j < values->n_values; j++)
			{
				if (g_str_equal(spec->name, "call_outcome") && values->values[j].value == VENTURE_CALL_OUTCOME_UNKNOWN)
					continue;
				g_ptr_array_add(choices, g_strdup(values->values[j].value_nick));
				g_ptr_array_add(labels, g_strdup(values->values[j].value_nick));
			}
			g_ptr_array_add(choices, NULL);
			g_ptr_array_add(labels, NULL);
			g_strfreev(spec->choices);
			g_strfreev(spec->choice_labels);
			spec->choices = (gchar **)g_ptr_array_free(choices, FALSE);
			spec->choice_labels = (gchar **)g_ptr_array_free(labels, FALSE);
			g_clear_pointer(&spec->default_text, g_free);
			g_type_class_unref(values);
		}
		if (g_str_equal(spec->name, "call_duration"))
		{
			spec->has_min = spec->has_max = TRUE;
			spec->min_value = 0;
			spec->max_value = 86400;
		}
		/* Validate before staging too: a rejected credential-bearing address
		 * must never enter a confirmation card or its retained parameters. */
		if (g_str_equal(spec->name, "call_recording_url"))
			spec->pattern = g_strdup("^(https://[A-Za-z0-9.-]+(:[0-9]+)?(/[^?#@[:space:]]*)?)?$");
		g_ptr_array_add(parameters, spec);
	}
	return parameters;
}

static gboolean
call_allowed(VentureAction *action, VentureEntity *entity, const VentureActor *actor, GError **error)
{
	(void)action;
	(void)actor;
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "activity") == G_TYPE_INVALID)
	{
		venture_set_error_validation(error, "module", "The activities module is disabled");
		return FALSE;
	}
	if (VENTURE_IS_ACTIVITY(entity))
	{
		gint kind;
		g_object_get(entity, "kind", &kind, NULL);
		if (kind != VENTURE_ACTIVITY_KIND_CALL)
		{
			venture_set_error_validation(error, "kind", "Log a call requires a call activity");
			return FALSE;
		}
	}
	return TRUE;
}

static VentureEntity *
call_invoke(VentureAction *action, VentureEntity *entity, GHashTable *params,
	const VentureActor *actor, GError **error)
{
	g_autoptr(JsonObject) details = json_object_new();
	GHashTableIter iter;
	gpointer key, value;
	g_hash_table_iter_init(&iter, params);
	while (g_hash_table_iter_next(&iter, &key, &value))
		json_object_set_member(details, key, json_node_copy(value));
	return venture_activity_service_log_call(venture_action_get_data(action), entity, details, actor, error);
}

void
venture_activity_actions_register(VentureDatabase *database)
{
	static const gchar *const types[] = { "company", "contact", "lead", "activity" };
	VentureActionRegistry *registry = venture_database_get_action_registry(database);
	VentureActivityService *service = venture_database_get_activity_service(database);
	g_autoptr(GPtrArray) parameters = venture_activity_call_parameters();
	guint i;
	for (i = 0; i < G_N_ELEMENTS(types); i++)
	{
		g_autoptr(VentureAction) action = g_object_new(VENTURE_TYPE_ACTION, "data-class", VENTURE_DATA_CLASS_TENANT,
			"type-name", types[i], "name", "log_call", "label", "Log a call",
			"description", "Record one call and optional followup atomically; exact retries reuse history",
			"parameters", parameters, "stageable", TRUE, "roles", VENTURE_USER_ROLE_EDITOR, NULL);
		g_autoptr(GError) error = NULL;
		if (!venture_action_registry_register(registry, action, call_allowed, call_invoke,
			g_object_ref(service), g_object_unref, &error))
			g_error("Call action registration: %s", error->message);
	}
}
