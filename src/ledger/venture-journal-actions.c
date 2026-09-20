/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

static gboolean
journal_allowed(VentureAction *action, VentureEntity *entity, const VentureActor *actor, GError **error)
{
	g_autofree gchar *name = NULL;
	gint state;
	g_object_get(action, "name", &name, NULL);
	g_object_get(entity, "state", &state, NULL);
	if (0 == g_strcmp0(name, "create_and_post")) return TRUE;
	if (state == (0 == g_strcmp0(name, "post") ? VENTURE_JOURNAL_DRAFT : VENTURE_JOURNAL_POSTED)) return TRUE;
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
		"VenturePostingService requires a draft for post, or a posted journal for a single reversal");
	return FALSE;
}
static VentureEntity *
journal_post(VentureAction *action, VentureEntity *entity, GHashTable *params,
	const VentureActor *actor, GError **error)
{
	return VENTURE_ENTITY(venture_posting_service_post(venture_action_get_data(action),
		VENTURE_JOURNAL(entity), NULL, NULL, actor, error));
}
static VentureEntity *
journal_reverse(VentureAction *action, VentureEntity *entity, GHashTable *params,
	const VentureActor *actor, GError **error)
{
	JsonNode *date = g_hash_table_lookup(params, "occurred_at");
	JsonNode *memo = g_hash_table_lookup(params, "memo");
	g_autoptr(GDateTime) when = date && !JSON_NODE_HOLDS_NULL(date) ?
		venture_time_from_string(json_node_get_string(date), error) : venture_time_now();
	if (!when) return NULL;
	return VENTURE_ENTITY(venture_posting_service_reverse(venture_action_get_data(action),
		venture_entity_get_id(entity), when,
		memo && !JSON_NODE_HOLDS_NULL(memo) ? json_node_get_string(memo) : NULL, actor, error));
}
static VentureEntity *
journal_create_post(VentureAction *action, VentureEntity *entity, GHashTable *params,
	const VentureActor *actor, GError **error)
{
	JsonNode *body = g_hash_table_lookup(params, "journal");
	g_autoptr(JsonNode) parsed = NULL;
	JsonNode *lines_node;
	JsonArray *lines;
	g_autoptr(VentureJournal) journal = venture_journal_new();
	g_autoptr(GPtrArray) rows = g_ptr_array_new_with_free_func(g_object_unref);
	guint i;
	if (body && JSON_NODE_HOLDS_VALUE(body) && G_TYPE_STRING == json_node_get_value_type(body))
	{
		parsed = venture_json_parse(json_node_get_string(body), error);
		if (!parsed) return NULL;
		body = parsed;
	}
	if (!body || !JSON_NODE_HOLDS_OBJECT(body))
	{
		venture_set_error_validation(error, "journal", "A journal header with a lines array is required");
		return NULL;
	}
	lines_node = json_object_get_member(json_node_get_object(body), "lines");
	if (!lines_node || !JSON_NODE_HOLDS_ARRAY(lines_node))
	{
		venture_set_error_validation(error, "lines", "A journal lines array is required");
		return NULL;
	}
	if (!venture_serializable_from_json(VENTURE_SERIALIZABLE(journal), body, error)) return NULL;
	if (venture_entity_get_id(VENTURE_ENTITY(journal)))
	{
		venture_set_error_validation(error, "id", "Create-and-post requires a new journal");
		return NULL;
	}
	lines = json_node_get_array(lines_node);
	for (i = 0; i < json_array_get_length(lines); i++)
	{
		g_autoptr(VentureJournalLine) line = venture_journal_line_new();
		if (!venture_serializable_from_json(VENTURE_SERIALIZABLE(line), json_array_get_element(lines, i), error)) return NULL;
		g_ptr_array_add(rows, g_steal_pointer(&line));
	}
	/* The posting service owns the single transaction, validation, period
	 * guard, journal evidence and compatibility projections. */
	return VENTURE_ENTITY(venture_posting_service_post(venture_action_get_data(action),
		journal, rows, NULL, actor, error));
}
void
venture_journal_actions_register(VentureDatabase *database)
{
	VentureActionRegistry *registry = venture_database_get_action_registry(database);
	VenturePostingService *service = venture_database_get_posting_service(database);
	g_autoptr(GPtrArray) parameters = g_ptr_array_new_with_free_func((GDestroyNotify)venture_field_spec_free);
	g_autoptr(VentureAction) post = NULL;
	g_autoptr(VentureAction) reverse = NULL;
	g_autoptr(VentureAction) create = NULL;
	g_autoptr(GError) error = NULL;
	VentureFieldSpec *journal_spec;
	post = g_object_new(VENTURE_TYPE_ACTION, "data-class", VENTURE_DATA_CLASS_TENANT, "type-name", "journal", "name", "post",
		"label", "Post", "description", "Post this live draft journal", "stageable", TRUE,
		"roles", VENTURE_USER_ROLE_EDITOR, NULL);
	g_ptr_array_add(parameters, venture_field_spec_new("occurred_at", "Reversal date", VENTURE_FIELD_KIND_DATETIME));
	g_ptr_array_add(parameters, venture_field_spec_new("memo", "Reason", VENTURE_FIELD_KIND_TEXT));
	reverse = g_object_new(VENTURE_TYPE_ACTION, "data-class", VENTURE_DATA_CLASS_TENANT, "type-name", "journal", "name", "reverse",
		"label", "Reverse", "description", "Reverse this posted journal once", "parameters", parameters,
		"stageable", TRUE, "roles", VENTURE_USER_ROLE_EDITOR, NULL);
	g_ptr_array_set_size(parameters, 0);
	journal_spec = venture_field_spec_new("journal", "Journal header and lines", VENTURE_FIELD_KIND_JSON);
	journal_spec->required = TRUE;
	g_ptr_array_add(parameters, journal_spec);
	create = g_object_new(VENTURE_TYPE_ACTION, "data-class", VENTURE_DATA_CLASS_TENANT, "type-name", "journal", "name", "create_and_post",
		"label", "Create and post", "description", "Create and post a journal header and lines atomically",
		"parameters", parameters, "stageable", TRUE, "type-level", TRUE, "subject-parameter", "journal", "roles", VENTURE_USER_ROLE_EDITOR, NULL);
	if (!venture_action_registry_register(registry, post, journal_allowed, journal_post, g_object_ref(service), g_object_unref, &error) ||
		!venture_action_registry_register(registry, reverse, journal_allowed, journal_reverse, g_object_ref(service), g_object_unref, &error) ||
		!venture_action_registry_register(registry, create, journal_allowed, journal_create_post, g_object_ref(service), g_object_unref, &error))
		g_error("Journal action registration: %s", error->message);
}
