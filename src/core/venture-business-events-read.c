/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>

JsonNode *
venture_business_events_read(VentureDatabase *database, gint64 organization,
	gint64 after, guint limit, GError **error)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(VentureEntity) owner = NULL;
	g_autoptr(VentureBusinessEvent) authority = NULL;
	guint i;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	if (venture_database_has_transaction(database))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
			"Read business events only after commit");
		return NULL;
	}
	if (organization <= 0 || after < 0 || limit == 0 || limit > 1000)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			"Use one organization, a nonnegative cursor and a page size from 1 to 1000");
		return NULL;
	}
	owner = venture_database_get(database, VENTURE_TYPE_ORGANIZATION, organization, error);
	if (owner == NULL || venture_entity_is_deleted(owner))
	{
		if (error == NULL || *error == NULL)
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Organization not found");
		return NULL;
	}
	authority = g_object_new(VENTURE_TYPE_BUSINESS_EVENT, "organization-id", organization, NULL);
	if (!venture_access_policy_check_read(venture_database_get_access_policy(database), VENTURE_ENTITY(authority), error))
		return NULL;
	query = venture_query_new(VENTURE_TYPE_BUSINESS_EVENT);
	venture_query_set_organization(query, organization);
	venture_query_add_filter_int(query, "id", VENTURE_FILTER_OP_GT, after, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	venture_query_set_limit(query, limit);
	rows = venture_database_find(database, query, error);
	if (rows == NULL) return NULL;
	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "events");
	json_builder_begin_array(builder);
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *event = g_ptr_array_index(rows, i);
		g_autofree gchar *name = NULL, *source = NULL, *timestamp = NULL;
		g_autoptr(GDateTime) when = NULL;
		gint64 source_id = 0, version = 0;
		g_object_get(event, "event", &name, "source-type", &source, "source-id", &source_id,
			"source-version", &version, "occurred-at", &when, NULL);
		timestamp = venture_time_to_string(when);
		after = venture_entity_get_id(event);
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "id"); json_builder_add_string_value(builder, venture_entity_get_uuid(event));
		json_builder_set_member_name(builder, "cursor"); json_builder_add_int_value(builder, after);
		json_builder_set_member_name(builder, "organization_id"); json_builder_add_int_value(builder, organization);
		json_builder_set_member_name(builder, "event"); json_builder_add_string_value(builder, name);
		json_builder_set_member_name(builder, "occurred_at"); json_builder_add_string_value(builder, timestamp);
		json_builder_set_member_name(builder, "source_type"); json_builder_add_string_value(builder, source);
		json_builder_set_member_name(builder, "source_id"); json_builder_add_int_value(builder, source_id);
		json_builder_set_member_name(builder, "source_version"); json_builder_add_int_value(builder, version);
		json_builder_end_object(builder);
	}
	json_builder_end_array(builder);
	json_builder_set_member_name(builder, "next_cursor"); json_builder_add_int_value(builder, after);
	json_builder_end_object(builder);
	return json_builder_get_root(builder);
}
