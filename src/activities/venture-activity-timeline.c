/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

JsonNode *
venture_activity_call_timeline(VentureContext *context, const gchar *target_type,
	gint64 target_id, guint limit, GError **error)
{
	g_autoptr(JsonNode) result = json_node_new(JSON_NODE_ARRAY);
	g_autoptr(VentureEntity) target = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autofree gchar *field = NULL;
	VentureDatabase *database = venture_context_get_database(context);
	GType type;
	guint i;
	json_node_take_array(result, json_array_new());
	if (!venture_context_module_enabled(context, "activities") ||
		!(g_str_equal(target_type, "company") || g_str_equal(target_type, "contact") ||
		  g_str_equal(target_type, "lead") || g_str_equal(target_type, "deal")))
		return g_steal_pointer(&result);
	type = venture_entity_registry_lookup(venture_entity_registry_get_default(), target_type);
	if (type == G_TYPE_INVALID) return g_steal_pointer(&result);
	target = venture_database_get(database, type, target_id, error);
	if (target == NULL) return NULL;
	query = venture_query_new(VENTURE_TYPE_ACTIVITY);
	venture_query_set_organization(query, venture_entity_get_organization_id(target));
	venture_query_set_limit(query, MIN(limit != 0 ? limit : 100, 1000));
	field = g_strconcat(target_type, "-id", NULL);
	if (!venture_query_add_filter_int(query, field, VENTURE_FILTER_OP_EQ, target_id, error) ||
		!venture_query_add_filter_int(query, "kind", VENTURE_FILTER_OP_EQ, VENTURE_ACTIVITY_KIND_CALL, error) ||
		!venture_query_add_filter_int(query, "status", VENTURE_FILTER_OP_EQ, VENTURE_ACTIVITY_STATUS_DONE, error) ||
		!venture_query_add_filter_int(query, "call-interaction-id", VENTURE_FILTER_OP_GT, 0, error) ||
		!venture_query_add_order(query, "call-occurred-at", VENTURE_SORT_DESCENDING, error))
		return NULL;
	rows = venture_database_find(database, query, error);
	if (rows == NULL) return NULL;
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autoptr(VentureEntity) history = NULL;
		g_autoptr(GDateTime) occurred = NULL;
		g_autofree gchar *owner = NULL, *subject = NULL, *outcome = NULL, *body = NULL, *when = NULL;
		gint64 interaction_id;
		JsonObject *event;
		g_object_get(row, "call-interaction-id", &interaction_id, "owner", &owner,
			"subject", &subject, "outcome", &outcome, NULL);
		history = venture_database_get(database, VENTURE_TYPE_INTERACTION, interaction_id, error);
		if (history == NULL) return NULL;
		if (venture_entity_is_deleted(history) ||
			venture_entity_get_organization_id(history) != venture_entity_get_organization_id(target)) continue;
		g_object_get(history, "occurred-at", &occurred, NULL);
		when = occurred != NULL ? venture_time_to_string(occurred) : NULL;
		body = g_strdup_printf("%s%s%s", subject != NULL ? subject : "Call",
			venture_string_is_empty(outcome) ? "" : ": ", outcome != NULL ? outcome : "");
		event = json_object_new();
		json_object_set_string_member(event, "kind", "call");
		json_object_set_int_member(event, "id", interaction_id);
		json_object_set_int_member(event, "activity_id", venture_entity_get_id(row));
		json_object_set_string_member(event, "actor", owner != NULL ? owner : "Unassigned");
		json_object_set_string_member(event, "body", body);
		if (when != NULL) json_object_set_string_member(event, "when", when);
		json_array_add_object_element(json_node_get_array(result), event);
	}
	return g_steal_pointer(&result);
}
