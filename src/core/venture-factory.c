/*
 * venture-factory.c - The software factory's operations
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

/*
 * "in_progress" as "In progress", for a changelog heading.
 */
static gchar *
venture_factory_label(const gchar *nick)
{
	gchar *label;
	gchar *cursor;

	label = g_strdup(nick);

	for (cursor = label; '\0' != *cursor; cursor++)
	{
		if (('_' == *cursor) || ('-' == *cursor))
			*cursor = ' ';
	}

	if (g_ascii_islower(label[0]))
		label[0] = g_ascii_toupper(label[0]);

	return label;
}

GPtrArray *
venture_factory_release_tickets(
	VentureDatabase	*database,
	gint64		 release_id
){
	g_autoptr(VentureQuery) query = NULL;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);

	query = venture_query_new(VENTURE_TYPE_TICKET);

	if (!venture_query_add_filter_int(query, "release-id",
	                                  VENTURE_FILTER_OP_EQ, release_id, NULL))
		return NULL;

	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	venture_query_set_limit(query, 500);

	return venture_database_find(database, query, NULL);
}

gchar *
venture_factory_draft_changelog(
	VentureContext	*context,
	VentureEntity	*release
){
	g_autoptr(GString) text = NULL;
	g_autoptr(GPtrArray) tickets = NULL;
	g_auto(GStrv) types = NULL;
	g_autofree gchar *version = NULL;
	g_autofree gchar *name = NULL;
	gsize t;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	g_return_val_if_fail(VENTURE_IS_RELEASE(release), NULL);

	g_object_get(release, "number", &version, "name", &name, NULL);

	text = g_string_new(NULL);
	g_string_append_printf(text, "## %s", (NULL != version) ? version : "");

	if (!venture_string_is_empty(name))
		g_string_append_printf(text, " \xe2\x80\x94 %s", name);

	g_string_append(text, "\n");

	tickets = venture_factory_release_tickets(
		venture_context_get_database(context), venture_entity_get_id(release));

	if ((NULL == tickets) || (0 == tickets->len))
	{
		g_string_append(text, "\nNo tickets are marked as fixed in this "
		                      "release yet.\n");
		return g_string_free(g_steal_pointer(&text), FALSE);
	}

	types = venture_enum_list_nicks(VENTURE_TYPE_ISSUE_TYPE);

	for (t = 0; NULL != types[t]; t++)
	{
		gboolean heading = FALSE;
		gint wanted;
		guint i;

		if (!venture_enum_from_nick(VENTURE_TYPE_ISSUE_TYPE, types[t], &wanted))
			continue;

		for (i = 0; i < tickets->len; i++)
		{
			g_autofree gchar *title = NULL;
			VentureEntity *ticket;
			VentureIssueType issue_type;

			ticket = g_ptr_array_index(tickets, i);
			g_object_get(ticket, "issue-type", &issue_type, "title", &title,
			             NULL);

			if ((gint)issue_type != wanted)
				continue;

			if (!heading)
			{
				g_autofree gchar *label = NULL;

				label = venture_factory_label(types[t]);
				g_string_append_printf(text, "\n### %s\n\n", label);
				heading = TRUE;
			}

			g_string_append_printf(text, "- #%" G_GINT64_FORMAT " %s\n",
			                       venture_entity_get_id(ticket),
			                       (NULL != title) ? title : "");
		}
	}

	return g_string_free(g_steal_pointer(&text), FALSE);
}

gboolean
venture_factory_apply_changelog(
	VentureContext	*context,
	VentureEntity	*release,
	gboolean	 replace
){
	g_autofree gchar *existing = NULL;
	g_autofree gchar *draft = NULL;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), FALSE);
	g_return_val_if_fail(VENTURE_IS_RELEASE(release), FALSE);

	g_object_get(release, "changelog", &existing, NULL);

	/* A changelog somebody edited is kept unless they asked otherwise:
	 * the draft is a starting point, not the truth. */
	if (!venture_string_is_empty(existing) && !replace)
		return FALSE;

	draft = venture_factory_draft_changelog(context, release);
	g_object_set(release, "changelog", draft, NULL);

	return TRUE;
}

gboolean
venture_factory_publish_release(
	VentureContext		 *context,
	VentureEntity		 *release,
	gboolean		  prerelease,
	const VentureActor	 *actor,
	GError			**error
){
	g_autoptr(VentureEntity) repo = NULL;
	g_autoptr(VentureEntity) forge = NULL;
	g_autoptr(VentureForgeClient) client = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autofree gchar *tag = NULL;
	g_autofree gchar *version = NULL;
	g_autofree gchar *name = NULL;
	g_autofree gchar *changelog = NULL;
	g_autofree gchar *existing_url = NULL;
	g_autofree gchar *repo_name = NULL;
	g_autofree gchar *url = NULL;
	VentureDatabase *database;
	gint64 repo_id = 0;
	gint64 forge_id = 0;
	gint64 external_id = 0;
	gint64 timeout = 30;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), FALSE);
	g_return_val_if_fail(VENTURE_IS_RELEASE(release), FALSE);

	if (!venture_context_module_enabled(context, "forge"))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		                    "The forge module is disabled on this install "
		                    "(modules.forge.enabled)");
		return FALSE;
	}

	database = venture_context_get_database(context);

	g_object_get(release,
	             "tag", &tag, "number", &version, "name", &name,
	             "changelog", &changelog, "url", &existing_url,
	             "repo-id", &repo_id,
	             NULL);

	if (!venture_string_is_empty(existing_url))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_ALREADY_EXISTS,
		            "Release %s is already on the forge at %s", version,
		            existing_url);
		return FALSE;
	}

	if (0 == repo_id)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "The release names no repository to publish to");
		return FALSE;
	}

	repo = venture_database_get(database, VENTURE_TYPE_FORGE_REPO, repo_id,
	                            error);

	if (NULL == repo)
		return FALSE;

	g_object_get(repo, "name", &repo_name, "forge-id", &forge_id, NULL);

	forge = venture_database_get(database, VENTURE_TYPE_FORGE, forge_id, error);

	if (NULL == forge)
		return FALSE;

	g_object_get(venture_context_get_config(context),
	             "forge-request-timeout", &timeout, NULL);
	client = venture_forge_client_for_forge(VENTURE_FORGE(forge), (gint)timeout,
	                                        error);

	if (NULL == client)
		return FALSE;

	if (venture_string_is_empty(tag))
	{
		g_free(tag);
		tag = g_strdup_printf("v%s", version);
	}

	if (!venture_forge_client_create_release(client, repo_name, tag, NULL,
	                                         !venture_string_is_empty(name)
	                                                 ? name : version,
	                                         changelog, FALSE, prerelease,
	                                         &external_id, &url, error))
		return FALSE;

	now = g_date_time_new_now_utc();

	g_object_set(release,
	             "tag", tag,
	             "url", url,
	             "external-id", external_id,
	             "status", VENTURE_RELEASE_STATUS_RELEASED,
	             "released-at", now,
	             NULL);

	return venture_database_save(database, release, actor, error);
}

void
venture_factory_milestone_progress(
	VentureDatabase	*database,
	gint64		 milestone_id,
	gint64		*out_total,
	gint64		*out_done
){
	g_autoptr(VentureQuery) all = NULL;
	g_autoptr(GPtrArray) tickets = NULL;
	guint i;

	g_return_if_fail(VENTURE_IS_DATABASE(database));

	*out_total = 0;
	*out_done = 0;

	all = venture_query_new(VENTURE_TYPE_TICKET);

	if (!venture_query_add_filter_int(all, "milestone-id",
	                                  VENTURE_FILTER_OP_EQ, milestone_id,
	                                  NULL))
		return;

	venture_query_set_limit(all, 0);
	tickets = venture_database_find(database, all, NULL);

	if (NULL == tickets)
		return;

	*out_total = (gint64)tickets->len;

	for (i = 0; i < tickets->len; i++)
	{
		VentureTicketStatus status;

		g_object_get(g_ptr_array_index(tickets, i), "status", &status, NULL);

		if ((VENTURE_TICKET_STATUS_DONE == status) ||
		    (VENTURE_TICKET_STATUS_CANCELLED == status))
			(*out_done)++;
	}
}

VentureEntity *
venture_factory_current_deployment(
	VentureDatabase	*database,
	gint64		 environment_id
){
	g_autoptr(VentureQuery) query = NULL;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);

	query = venture_query_new(VENTURE_TYPE_DEPLOYMENT);

	if (!venture_query_add_filter_int(query, "environment-id",
	                                  VENTURE_FILTER_OP_EQ, environment_id,
	                                  NULL) ||
	    !venture_query_add_filter_string(query, "status", VENTURE_FILTER_OP_EQ,
	                                     "succeeded", NULL))
		return NULL;

	venture_query_add_order(query, "deployed-at", VENTURE_SORT_DESCENDING,
	                        NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, NULL);
	venture_query_set_limit(query, 1);

	return venture_database_find_one(database, query, NULL);
}

/* --- The loop at a glance ------------------------------------------------- */

static void
venture_factory_scope(
	VentureQuery	*query,
	const gint64	*organization_ids,
	gsize		 n_organizations
){
	if ((NULL != organization_ids) && (n_organizations > 0))
		venture_query_set_organization_tree(query, organization_ids,
		                                    n_organizations);
}

/*
 * A record's identity and label, the two members every entry carries.
 */
static void
venture_factory_add_identity(
	JsonBuilder	*builder,
	VentureEntity	*record
){
	g_autofree gchar *label = NULL;

	label = venture_entity_get_display_name(record);
	json_builder_set_member_name(builder, "id");
	json_builder_add_int_value(builder, venture_entity_get_id(record));
	json_builder_set_member_name(builder, "label");
	json_builder_add_string_value(builder, label);
}

static void
venture_factory_add_enum(
	JsonBuilder	*builder,
	const gchar	*member,
	GType		 enum_type,
	gint		 value
){
	json_builder_set_member_name(builder, member);
	json_builder_add_string_value(builder,
		venture_enum_to_nick(enum_type, value));
}

static void
venture_factory_add_time(
	JsonBuilder	*builder,
	const gchar	*member,
	GDateTime	*when
){
	json_builder_set_member_name(builder, member);

	if (NULL == when)
	{
		json_builder_add_null_value(builder);
	}
	else
	{
		g_autofree gchar *iso = NULL;

		iso = venture_time_to_string(when);
		json_builder_add_string_value(builder, iso);
	}
}

static void
venture_factory_add_string(
	JsonBuilder	*builder,
	const gchar	*member,
	const gchar	*value
){
	json_builder_set_member_name(builder, member);

	if (venture_string_is_empty(value))
		json_builder_add_null_value(builder);
	else
		json_builder_add_string_value(builder, value);
}

JsonNode *
venture_factory_describe(
	VentureContext	 *context,
	const gint64	 *organization_ids,
	gsize		  n_organizations,
	GError		**error
){
	g_autoptr(JsonBuilder) builder = NULL;
	VentureDatabase *database;
	guint i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	if (!venture_context_module_enabled(context, "factory"))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		                    "The factory module is disabled on this install "
		                    "(modules.factory.enabled)");
		return NULL;
	}

	database = venture_context_get_database(context);
	builder = json_builder_new();
	json_builder_begin_object(builder);

	/* Open milestones, soonest due first, with progress. */
	{
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(GPtrArray) rows = NULL;

		query = venture_query_new(VENTURE_TYPE_MILESTONE);
		venture_query_add_filter_string(query, "status", VENTURE_FILTER_OP_NE,
		                                "completed", NULL);
		venture_query_add_filter_string(query, "status", VENTURE_FILTER_OP_NE,
		                                "cancelled", NULL);
		venture_query_add_order(query, "due-on", VENTURE_SORT_ASCENDING, NULL);
		venture_query_set_limit(query, 20);
		venture_factory_scope(query, organization_ids, n_organizations);
		rows = venture_database_find(database, query, error);

		if (NULL == rows)
			return NULL;

		json_builder_set_member_name(builder, "milestones");
		json_builder_begin_array(builder);

		for (i = 0; i < rows->len; i++)
		{
			VentureEntity *milestone;
			g_autoptr(GDateTime) due = NULL;
			VentureMilestoneStatus status;
			gint64 total;
			gint64 done;

			milestone = g_ptr_array_index(rows, i);
			g_object_get(milestone, "status", &status, "due-on", &due, NULL);
			venture_factory_milestone_progress(database,
				venture_entity_get_id(milestone), &total, &done);

			json_builder_begin_object(builder);
			venture_factory_add_identity(builder, milestone);
			venture_factory_add_enum(builder, "status",
				VENTURE_TYPE_MILESTONE_STATUS, (gint)status);
			venture_factory_add_time(builder, "due_on", due);
			json_builder_set_member_name(builder, "tickets");
			json_builder_add_int_value(builder, total);
			json_builder_set_member_name(builder, "done");
			json_builder_add_int_value(builder, done);
			json_builder_set_member_name(builder, "percent");
			json_builder_add_int_value(builder,
				(total > 0) ? (done * 100) / total : 0);
			json_builder_end_object(builder);
		}

		json_builder_end_array(builder);
	}

	/* The newest releases. */
	{
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(GPtrArray) rows = NULL;

		query = venture_query_new(VENTURE_TYPE_RELEASE);
		venture_query_add_order(query, "released-at", VENTURE_SORT_DESCENDING,
		                        NULL);
		venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, NULL);
		venture_query_set_limit(query, 10);
		venture_factory_scope(query, organization_ids, n_organizations);
		rows = venture_database_find(database, query, error);

		if (NULL == rows)
			return NULL;

		json_builder_set_member_name(builder, "releases");
		json_builder_begin_array(builder);

		for (i = 0; i < rows->len; i++)
		{
			VentureEntity *release;
			g_autofree gchar *number = NULL;
			g_autofree gchar *tag = NULL;
			g_autofree gchar *url = NULL;
			g_autoptr(GDateTime) released = NULL;
			VentureReleaseStatus status;

			release = g_ptr_array_index(rows, i);
			g_object_get(release, "number", &number, "tag", &tag,
			             "url", &url, "status", &status,
			             "released-at", &released, NULL);

			json_builder_begin_object(builder);
			venture_factory_add_identity(builder, release);
			venture_factory_add_string(builder, "number", number);
			venture_factory_add_enum(builder, "status",
				VENTURE_TYPE_RELEASE_STATUS, (gint)status);
			venture_factory_add_string(builder, "tag", tag);
			venture_factory_add_string(builder, "url", url);
			venture_factory_add_time(builder, "released_at", released);
			json_builder_end_object(builder);
		}

		json_builder_end_array(builder);
	}

	/* The latest builds. */
	{
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(GPtrArray) rows = NULL;

		query = venture_query_new(VENTURE_TYPE_BUILD);
		venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, NULL);
		venture_query_set_limit(query, 10);
		venture_factory_scope(query, organization_ids, n_organizations);
		rows = venture_database_find(database, query, error);

		if (NULL == rows)
			return NULL;

		json_builder_set_member_name(builder, "builds");
		json_builder_begin_array(builder);

		for (i = 0; i < rows->len; i++)
		{
			VentureEntity *build;
			g_autofree gchar *workflow = NULL;
			g_autofree gchar *ref = NULL;
			g_autofree gchar *url = NULL;
			g_autoptr(GDateTime) finished = NULL;
			VentureBuildStatus status;
			gint64 release_id = 0;

			build = g_ptr_array_index(rows, i);
			g_object_get(build, "workflow", &workflow, "ref", &ref,
			             "url", &url, "status", &status,
			             "finished-at", &finished, "release-id", &release_id,
			             NULL);

			json_builder_begin_object(builder);
			venture_factory_add_identity(builder, build);
			venture_factory_add_enum(builder, "status",
				VENTURE_TYPE_BUILD_STATUS, (gint)status);
			venture_factory_add_string(builder, "workflow", workflow);
			venture_factory_add_string(builder, "ref", ref);
			venture_factory_add_string(builder, "url", url);
			json_builder_set_member_name(builder, "release_id");
			json_builder_add_int_value(builder, release_id);
			venture_factory_add_time(builder, "finished_at", finished);
			json_builder_end_object(builder);
		}

		json_builder_end_array(builder);
	}

	/* Each environment and what it is running. */
	{
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(GPtrArray) rows = NULL;

		query = venture_query_new(VENTURE_TYPE_ENVIRONMENT);
		venture_query_add_order(query, "kind", VENTURE_SORT_DESCENDING, NULL);
		venture_query_add_order(query, "name", VENTURE_SORT_ASCENDING, NULL);
		venture_query_set_limit(query, 50);
		venture_factory_scope(query, organization_ids, n_organizations);
		rows = venture_database_find(database, query, error);

		if (NULL == rows)
			return NULL;

		json_builder_set_member_name(builder, "environments");
		json_builder_begin_array(builder);

		for (i = 0; i < rows->len; i++)
		{
			VentureEntity *environment;
			g_autoptr(VentureEntity) deployment = NULL;
			g_autoptr(VentureEntity) release = NULL;
			g_autoptr(GDateTime) deployed_at = NULL;
			VentureEnvironmentKind kind;
			gint64 release_id = 0;

			environment = g_ptr_array_index(rows, i);
			g_object_get(environment, "kind", &kind, NULL);
			deployment = venture_factory_current_deployment(database,
				venture_entity_get_id(environment));

			if (NULL != deployment)
			{
				g_object_get(deployment, "release-id", &release_id,
				             "deployed-at", &deployed_at, NULL);

				if (0 != release_id)
					release = venture_database_get(database,
						VENTURE_TYPE_RELEASE, release_id, NULL);
			}

			json_builder_begin_object(builder);
			venture_factory_add_identity(builder, environment);
			venture_factory_add_enum(builder, "kind",
				VENTURE_TYPE_ENVIRONMENT_KIND, (gint)kind);
			json_builder_set_member_name(builder, "release_id");
			json_builder_add_int_value(builder, release_id);
			json_builder_set_member_name(builder, "release");

			if (NULL != release)
			{
				g_autofree gchar *label = NULL;

				label = venture_entity_get_display_name(release);
				json_builder_add_string_value(builder, label);
			}
			else
			{
				json_builder_add_null_value(builder);
			}

			json_builder_set_member_name(builder, "deployment_id");
			json_builder_add_int_value(builder, (NULL != deployment)
				? venture_entity_get_id(deployment) : 0);
			venture_factory_add_time(builder, "deployed_at", deployed_at);
			json_builder_end_object(builder);
		}

		json_builder_end_array(builder);
	}

	/* What is on fire. */
	{
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(GPtrArray) rows = NULL;

		query = venture_query_new(VENTURE_TYPE_INCIDENT);
		venture_query_add_filter_string(query, "status", VENTURE_FILTER_OP_NE,
		                                "resolved", NULL);
		venture_query_add_order(query, "started-at", VENTURE_SORT_DESCENDING,
		                        NULL);
		venture_query_set_limit(query, 20);
		venture_factory_scope(query, organization_ids, n_organizations);
		rows = venture_database_find(database, query, error);

		if (NULL == rows)
			return NULL;

		json_builder_set_member_name(builder, "incidents");
		json_builder_begin_array(builder);

		for (i = 0; i < rows->len; i++)
		{
			VentureEntity *incident;
			g_autoptr(GDateTime) started = NULL;
			VentureIncidentSeverity severity;
			VentureIncidentStatus status;
			gint64 environment_id = 0;
			gint64 release_id = 0;
			gint64 ticket_id = 0;

			incident = g_ptr_array_index(rows, i);
			g_object_get(incident, "severity", &severity, "status", &status,
			             "environment-id", &environment_id,
			             "release-id", &release_id, "ticket-id", &ticket_id,
			             "started-at", &started, NULL);

			json_builder_begin_object(builder);
			venture_factory_add_identity(builder, incident);
			venture_factory_add_enum(builder, "severity",
				VENTURE_TYPE_INCIDENT_SEVERITY, (gint)severity);
			venture_factory_add_enum(builder, "status",
				VENTURE_TYPE_INCIDENT_STATUS, (gint)status);
			json_builder_set_member_name(builder, "environment_id");
			json_builder_add_int_value(builder, environment_id);
			json_builder_set_member_name(builder, "release_id");
			json_builder_add_int_value(builder, release_id);
			json_builder_set_member_name(builder, "ticket_id");
			json_builder_add_int_value(builder, ticket_id);
			venture_factory_add_time(builder, "started_at", started);
			json_builder_end_object(builder);
		}

		json_builder_end_array(builder);
	}

	json_builder_end_object(builder);

	return json_builder_get_root(builder);
}
