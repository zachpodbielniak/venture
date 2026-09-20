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

	/* Every one of them: a changelog that stops at the five hundredth
	 * ticket, and a lead time measured over the oldest five hundred, are
	 * both wrong without saying so. */
	venture_query_set_limit(query, 0);

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
			VentureTicketStatus ticket_status;

			ticket = g_ptr_array_index(tickets, i);
			g_object_get(ticket, "issue-type", &issue_type, "title", &title,
			             "status", &ticket_status, NULL);

			if ((gint)issue_type != wanted)
				continue;

			/* Cancelled work did not ship, whatever release it was once
			 * meant for. */
			if (VENTURE_TICKET_STATUS_CANCELLED == ticket_status)
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
	VentureReleaseStatus status;
	gint64 external_id = 0;
	gint64 timeout = 30;
	gint attempt;

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

	/* Asked before the forge is touched, not after. The save at the end
	 * asks the same question, but by then the tag is cut and the release
	 * is out, and a refusal leaves both on the forge with a record that
	 * still reads as unpublished. */
	if (!venture_access_policy_check_write(
		venture_database_get_access_policy(database), release, "write",
		error))
		return FALSE;

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

	/* The tag is the record's, or "v" plus the version; with neither
	 * there is nothing to call it, and "v(null)" is not a tag anybody
	 * wants cut on their default branch. */
	if (venture_string_is_empty(tag) && venture_string_is_empty(version))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "The release has neither a version nor a tag, so "
		                    "there is nothing to publish it as");
		return FALSE;
	}

	g_object_get(release, "status", &status, NULL);

	if (VENTURE_RELEASE_STATUS_YANKED == status)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
		            "Release %s was yanked; publishing it would put it back "
		            "out. Set its status first if that is what is meant.",
		            venture_string_is_empty(version) ? tag : version);
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
	if (venture_entity_get_organization_id(release) != venture_entity_get_organization_id(repo) ||
		venture_entity_get_organization_id(repo) != venture_entity_get_organization_id(forge))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED, "Release, repository and forge must belong to one organization");
		return FALSE;
	}
	client = venture_forge_client_for_database(database, forge_id, (gint)timeout,
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
	                                                 ? name
	                                                 : (venture_string_is_empty(version)
	                                                    ? tag : version),
	                                         changelog, FALSE, prerelease,
	                                         &external_id, &url, error))
		return FALSE;

	if (!venture_forge_credentials_revalidate(venture_forge_client_get_credentials(client), database, error)) return FALSE;

	now = g_date_time_new_now_utc();

	/*
	 * The forge now has the release, and nothing undoes that, so the
	 * record must come to know it. The forge's own webhook can land while
	 * the call above is still waiting and save this row first; the save
	 * here is then a conflict, and giving up would leave a release that
	 * is out on the forge, reads as unpublished here, and gets a 409 from
	 * the forge on the retry. So a conflict re-reads the row and writes
	 * the same five facts onto what is there now.
	 */
	for (attempt = 0; attempt < 2; attempt++)
	{
		g_autoptr(GError) save_error = NULL;
		g_autoptr(VentureEntity) fresh = NULL;
		VentureEntity *target;

		target = release;

		if (attempt > 0)
		{
			fresh = venture_database_get(database, VENTURE_TYPE_RELEASE,
			                             venture_entity_get_id(release), error);

			if (NULL == fresh)
				return FALSE;

			target = fresh;
		}

		g_object_set(target, "tag", tag, "url", url,
		             "external-id", external_id, NULL);

		/* A release candidate is on the forge and is not the release:
		 * it is still being assembled, and no report counts it as
		 * shipped. */
		if (prerelease)
		{
			g_object_set(target, "status",
			             VENTURE_RELEASE_STATUS_IN_PROGRESS, NULL);
		}
		else
		{
			g_autoptr(GDateTime) released_before = NULL;

			g_object_get(target, "released-at", &released_before, NULL);
			g_object_set(target, "status", VENTURE_RELEASE_STATUS_RELEASED,
			             NULL);

			if (NULL == released_before)
				g_object_set(target, "released-at", now, NULL);
		}

		if (venture_database_save(database, target, actor, &save_error))
		{
			/* The caller's object is the one the page goes on to show. */
			if (target != release)
				g_object_set(release, "tag", tag, "url", url,
				             "external-id", external_id, NULL);

			return TRUE;
		}

		if ((0 == attempt) &&
		    g_error_matches(save_error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT))
			continue;

		g_propagate_error(error, g_steal_pointer(&save_error));
		return FALSE;
	}

	return FALSE;
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
		/* Being written up is fixed; it is not on fire. */
		venture_query_add_filter_string(query, "status", VENTURE_FILTER_OP_NE,
		                                "postmortem", NULL);
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

/* ==========================================================================
 * Budgets
 * ========================================================================== */

/*
 * The start of a budget's current window, or %NULL for all time.
 */
static GDateTime *
venture_factory_budget_window_start(
	VentureBudgetPeriod	 period,
	GDateTime		*now
){
	switch (period)
	{
	case VENTURE_BUDGET_PERIOD_MONTHLY:
		return g_date_time_new_utc(g_date_time_get_year(now),
		                           g_date_time_get_month(now), 1, 0, 0, 0.0);

	case VENTURE_BUDGET_PERIOD_WEEKLY:
	{
		g_autoptr(GDateTime) midnight = NULL;

		midnight = g_date_time_new_utc(g_date_time_get_year(now),
		                               g_date_time_get_month(now),
		                               g_date_time_get_day_of_month(now),
		                               0, 0, 0.0);
		/* Monday is 1. */
		return g_date_time_add_days(midnight,
		                            -(g_date_time_get_day_of_week(now) - 1));
	}

	case VENTURE_BUDGET_PERIOD_ALL_TIME:
	default:
		return NULL;
	}
}

/*
 * The runs that count against a budget: those started in its window, and
 * for a repository-scoped budget those whose ticket names that
 * repository. The ticket is the run's own idea of where it went, which is
 * what the rule keyed on.
 */
static void
venture_factory_budget_spend(
	VentureContext	*context,
	VentureEntity	*budget,
	GDateTime	*now,
	VentureMoney	**out_spent,
	gint64		*out_runs
){
	VentureDatabase *database;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) runs = NULL;
	g_autoptr(GPtrArray) amounts = NULL;
	g_autoptr(GDateTime) start = NULL;
	g_autoptr(GHashTable) repo_by_ticket = NULL;
	g_autofree gchar *currency = NULL;
	VentureMoney *limit = NULL;
	VentureBudgetPeriod period;
	gint64 repo_id = 0;
	guint i;

	*out_spent = NULL;
	*out_runs = 0;

	database = venture_context_get_database(context);
	g_object_get(budget, "period", &period, "repo-id", &repo_id,
	             "limit", &limit, NULL);
	currency = g_strdup((NULL != limit) ? venture_money_get_currency(limit)
	                                    : venture_money_get_default_currency());
	g_clear_pointer(&limit, venture_money_free);

	query = venture_query_new(VENTURE_TYPE_FORGE_RUN);
	start = venture_factory_budget_window_start(period, now);

	/* A budget is its organization's: what another organization's runs
	 * cost is neither this one's to count nor this one's to be stopped
	 * by. */
	if (0 != venture_entity_get_organization_id(budget))
		venture_query_set_organization(query,
			venture_entity_get_organization_id(budget));

	if (NULL != start)
	{
		g_autofree gchar *text = NULL;

		text = venture_time_to_string(start);
		venture_query_add_filter_string(query, "started-at",
		                                VENTURE_FILTER_OP_GTE, text, NULL);
	}

	venture_query_set_limit(query, 0);
	runs = venture_database_find(database, query, NULL);
	amounts = g_ptr_array_new_with_free_func((GDestroyNotify)venture_money_free);
	repo_by_ticket = g_hash_table_new(g_direct_hash, g_direct_equal);

	for (i = 0; (NULL != runs) && (i < runs->len); i++)
	{
		VentureEntity *run;
		VentureMoney *cost = NULL;
		gint64 ticket_id = 0;

		run = g_ptr_array_index(runs, i);
		g_object_get(run, "ticket-id", &ticket_id, "cost", &cost, NULL);

		if (0 != repo_id)
		{
			gpointer cached;
			gint64 run_repo;

			cached = g_hash_table_lookup(repo_by_ticket,
			                             GINT_TO_POINTER((gint)ticket_id));

			if (NULL != cached)
			{
				run_repo = GPOINTER_TO_INT(cached);
			}
			else
			{
				g_autoptr(VentureEntity) ticket = NULL;

				run_repo = 0;
				ticket = venture_database_get(database, VENTURE_TYPE_TICKET,
				                              ticket_id, NULL);

				if (NULL != ticket)
					g_object_get(ticket, "repo-id", &run_repo, NULL);

				/* Zero is stored as -1 so "unknown" and "not cached"
				 * stay distinguishable. */
				g_hash_table_insert(repo_by_ticket,
				                    GINT_TO_POINTER((gint)ticket_id),
				                    GINT_TO_POINTER((gint)((0 == run_repo)
				                                           ? -1 : run_repo)));
			}

			if (-1 == run_repo)
				run_repo = 0;

			if (run_repo != repo_id)
			{
				g_clear_pointer(&cost, venture_money_free);
				continue;
			}
		}

		(*out_runs)++;

		/* A run charged in another currency is a run that cannot be
		 * added to this budget; it is counted and its cost left out,
		 * which understates -- the honest direction for a cap is to
		 * say so in the note rather than to guess a rate. */
		if ((NULL != cost) &&
		    (0 == g_strcmp0(venture_money_get_currency(cost), currency)))
			g_ptr_array_add(amounts, g_steal_pointer(&cost));

		g_clear_pointer(&cost, venture_money_free);
	}

	*out_spent = venture_money_sum(amounts, currency, NULL);

	if (NULL == *out_spent)
		*out_spent = venture_money_new_zero(currency);
}

/*
 * One budget's standing, as JSON. Shared by the listing and the check so
 * the page and the refusal agree on the percentage.
 */
static void
venture_factory_budget_add(
	JsonBuilder	*builder,
	VentureEntity	*budget,
	VentureMoney	*spent,
	gint64		 runs,
	gint		 percent,
	gboolean	 comparable
){
	g_autofree gchar *name = NULL;
	g_autofree gchar *spent_text = NULL;
	g_autofree gchar *limit_text = NULL;
	g_autoptr(GDateTime) warned_at = NULL;
	g_autoptr(GDateTime) exhausted_at = NULL;
	VentureMoney *limit = NULL;
	VentureBudgetPeriod period;
	gint64 repo_id = 0;
	gint64 warn_percent = 0;
	gboolean hard_stop = FALSE;

	g_object_get(budget, "name", &name, "period", &period, "repo-id", &repo_id,
	             "limit", &limit, "warn-percent", &warn_percent,
	             "hard-stop", &hard_stop, "warned-at", &warned_at,
	             "exhausted-at", &exhausted_at, NULL);

	spent_text = venture_money_to_display_string(spent, TRUE);
	limit_text = (NULL != limit) ? venture_money_to_display_string(limit, TRUE)
	                             : g_strdup("");

	json_builder_begin_object(builder);
	venture_factory_add_identity(builder, budget);
	venture_factory_add_string(builder, "name", name);
	venture_factory_add_enum(builder, "period", VENTURE_TYPE_BUDGET_PERIOD,
	                         (gint)period);
	json_builder_set_member_name(builder, "repo_id");
	json_builder_add_int_value(builder, repo_id);
	json_builder_set_member_name(builder, "limit");
	json_builder_add_value(builder,
		(NULL != limit) ? venture_money_to_json(limit)
		                : json_node_new(JSON_NODE_NULL));
	venture_factory_add_string(builder, "limit_display", limit_text);
	json_builder_set_member_name(builder, "spent");
	json_builder_add_value(builder, venture_money_to_json(spent));
	venture_factory_add_string(builder, "spent_display", spent_text);
	json_builder_set_member_name(builder, "runs");
	json_builder_add_int_value(builder, runs);
	json_builder_set_member_name(builder, "percent");
	json_builder_add_int_value(builder, percent);
	json_builder_set_member_name(builder, "warn_percent");
	json_builder_add_int_value(builder, warn_percent);
	json_builder_set_member_name(builder, "hard_stop");
	json_builder_add_boolean_value(builder, hard_stop);
	json_builder_set_member_name(builder, "comparable");
	json_builder_add_boolean_value(builder, comparable);
	json_builder_set_member_name(builder, "warning");
	json_builder_add_boolean_value(builder,
		comparable && (warn_percent > 0) && (percent >= warn_percent));
	json_builder_set_member_name(builder, "exhausted");
	json_builder_add_boolean_value(builder, comparable && (percent >= 100));
	venture_factory_add_time(builder, "warned_at", warned_at);
	venture_factory_add_time(builder, "exhausted_at", exhausted_at);
	json_builder_end_object(builder);

	g_clear_pointer(&limit, venture_money_free);
}

/*
 * Spend against limit as a percentage, when the two can be compared.
 */
static gboolean
venture_factory_budget_percent(
	VentureEntity	*budget,
	VentureMoney	*spent,
	gint		*out_percent
){
	VentureMoney *limit = NULL;
	gdouble limit_value;
	gdouble spent_value;

	*out_percent = 0;
	g_object_get(budget, "limit", &limit, NULL);

	if ((NULL == limit) || venture_money_is_zero(limit) ||
	    (0 != g_strcmp0(venture_money_get_currency(limit),
	                    venture_money_get_currency(spent))))
	{
		g_clear_pointer(&limit, venture_money_free);
		return FALSE;
	}

	limit_value = venture_money_to_double(limit);
	spent_value = venture_money_to_double(spent);
	g_clear_pointer(&limit, venture_money_free);

	if (limit_value <= 0.0)
		return FALSE;

	*out_percent = (gint)((spent_value * 100.0) / limit_value);

	return TRUE;
}

static GPtrArray *
venture_factory_active_budgets(VentureContext *context)
{
	g_autoptr(VentureQuery) query = NULL;

	query = venture_query_new(VENTURE_TYPE_AGENT_BUDGET);
	venture_query_add_filter_string(query, "active", VENTURE_FILTER_OP_EQ,
	                                "true", NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	venture_query_set_limit(query, 0);

	return venture_database_find(venture_context_get_database(context), query,
	                             NULL);
}

JsonNode *
venture_factory_budgets_describe(
	VentureContext	 *context,
	GError		**error
){
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(GPtrArray) budgets = NULL;
	g_autoptr(GDateTime) now = NULL;
	guint i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	if (!venture_context_module_enabled(context, "forge"))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		                    "The forge module is disabled on this install");
		return NULL;
	}

	now = venture_time_now();
	budgets = venture_factory_active_budgets(context);
	builder = json_builder_new();
	json_builder_begin_array(builder);

	for (i = 0; (NULL != budgets) && (i < budgets->len); i++)
	{
		VentureEntity *budget;
		g_autoptr(VentureMoney) spent = NULL;
		gint64 runs;
		gint percent;
		gboolean comparable;

		budget = g_ptr_array_index(budgets, i);
		venture_factory_budget_spend(context, budget, now, &spent, &runs);
		comparable = venture_factory_budget_percent(budget, spent, &percent);
		venture_factory_budget_add(builder, budget, spent, runs, percent,
		                           comparable);
	}

	json_builder_end_array(builder);

	return json_builder_get_root(builder);
}

gboolean
venture_factory_budget_allows_run(
	VentureContext	 *context,
	gint64		  repo_id,
	GError		**error
){
	g_autoptr(GPtrArray) budgets = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autoptr(VentureAccessScope) internal = NULL;
	gint64 repo_organization = 0;
	guint i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), FALSE);

	/* A cap is the install's, not the asker's. Checked as whoever pressed
	 * Start, a budget filed where they cannot read is no budget, runs
	 * they cannot see cost nothing, and the stamp that says "admins have
	 * been told" is one they may not write -- so the admins are told
	 * again on every run. The whole check is trusted internal work. */
	internal = venture_access_policy_enter(
		venture_database_get_access_policy(
			venture_context_get_database(context)), NULL);

	now = venture_time_now();
	budgets = venture_factory_active_budgets(context);

	if (0 != repo_id)
	{
		g_autoptr(VentureEntity) repo = NULL;

		repo = venture_database_get(venture_context_get_database(context),
		                            VENTURE_TYPE_FORGE_REPO, repo_id, NULL);

		if (NULL != repo)
			repo_organization = venture_entity_get_organization_id(repo);
	}

	for (i = 0; (NULL != budgets) && (i < budgets->len); i++)
	{
		VentureEntity *budget;
		g_autoptr(VentureMoney) spent = NULL;
		g_autoptr(GDateTime) window = NULL;
		g_autoptr(GDateTime) warned_at = NULL;
		g_autoptr(GDateTime) exhausted_at = NULL;
		g_autofree gchar *name = NULL;
		g_autofree gchar *spent_text = NULL;
		VentureBudgetPeriod period;
		gint64 budget_repo = 0;
		gint64 warn_percent = 0;
		gint64 runs;
		gboolean hard_stop = FALSE;
		gboolean changed;
		gint percent;

		budget = g_ptr_array_index(budgets, i);
		g_object_get(budget, "repo-id", &budget_repo, "period", &period,
		             "warn-percent", &warn_percent, "hard-stop", &hard_stop,
		             "warned-at", &warned_at, "exhausted-at", &exhausted_at,
		             "name", &name, NULL);

		if ((0 != budget_repo) && (budget_repo != repo_id))
			continue;

		/* Another organization's budget does not cover this run. */
		if ((0 != repo_organization) &&
		    (0 != venture_entity_get_organization_id(budget)) &&
		    (repo_organization != venture_entity_get_organization_id(budget)))
			continue;

		venture_factory_budget_spend(context, budget, now, &spent, &runs);

		if (!venture_factory_budget_percent(budget, spent, &percent))
			continue;

		spent_text = venture_money_to_display_string(spent, TRUE);
		window = venture_factory_budget_window_start(period, now);
		changed = FALSE;

		/* A stamp from before this window is last window's; the line is
		 * crossed afresh each time the window turns. */
		if ((NULL != warned_at) && (NULL != window) &&
		    (g_date_time_compare(warned_at, window) < 0))
			g_clear_pointer(&warned_at, g_date_time_unref);

		if ((NULL != exhausted_at) && (NULL != window) &&
		    (g_date_time_compare(exhausted_at, window) < 0))
			g_clear_pointer(&exhausted_at, g_date_time_unref);

		if ((percent >= 100) && (NULL == exhausted_at))
		{
			g_autofree gchar *title = NULL;
			g_autofree gchar *body = NULL;

			title = g_strdup_printf("Agent budget exhausted: %s", name);
			body = g_strdup_printf("%s spent, %d%% of the limit. %s",
			                       spent_text, percent,
			                       hard_stop ? "New runs are refused until "
			                                   "the window resets or the "
			                                   "limit is raised."
			                                 : "Runs continue; the budget "
			                                   "is a warning, not a stop.");
			venture_notify_broadcast(context, VENTURE_USER_ROLE_ADMIN,
			                         VENTURE_NOTIFICATION_KIND_BUDGET, title,
			                         body, "agent_budget",
			                         venture_entity_get_id(budget), name,
			                         NULL, NULL);
			g_object_set(budget, "exhausted-at", now, NULL);
			changed = TRUE;
		}
		else if ((warn_percent > 0) && (percent >= warn_percent) &&
		         (percent < 100) && (NULL == warned_at))
		{
			g_autofree gchar *title = NULL;
			g_autofree gchar *body = NULL;

			title = g_strdup_printf("Agent budget at %d%%: %s", percent, name);
			body = g_strdup_printf("%s spent against the limit this window.",
			                       spent_text);
			venture_notify_broadcast(context, VENTURE_USER_ROLE_ADMIN,
			                         VENTURE_NOTIFICATION_KIND_BUDGET, title,
			                         body, "agent_budget",
			                         venture_entity_get_id(budget), name,
			                         NULL, NULL);
			g_object_set(budget, "warned-at", now, NULL);
			changed = TRUE;
		}

		if (changed)
			venture_database_save(venture_context_get_database(context),
			                      budget, NULL, NULL);

		if ((percent >= 100) && hard_stop)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
			            "The agent budget \"%s\" is exhausted: %s spent "
			            "against its limit. Raise the limit, switch off "
			            "the hard stop, or wait for the window to reset.",
			            name, spent_text);
			return FALSE;
		}
	}

	return TRUE;
}

/* ==========================================================================
 * Mission control
 * ========================================================================== */

JsonNode *
venture_factory_runs_describe(
	VentureContext	 *context,
	const gint64	 *organization_ids,
	gsize		  n_organizations,
	const gchar	 *state,
	guint		  limit,
	GError		**error
){
	VentureDatabase *database;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) runs = NULL;
	g_autoptr(GPtrArray) costs = NULL;
	g_autoptr(GPtrArray) success_costs = NULL;
	g_autoptr(VentureMoney) total_cost = NULL;
	g_autoptr(VentureMoney) success_cost = NULL;
	g_autoptr(GHashTable) ticket_titles = NULL;
	guint other_currency = 0;
	guint priced_successes = 0;
	gint64 live;
	gint64 succeeded;
	gint64 failed;
	gint64 pull_requests;
	gint64 input_tokens;
	gint64 output_tokens;
	gint64 total_seconds;
	gint64 timed;
	guint i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	if (!venture_context_module_enabled(context, "forge"))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		                    "The forge module is disabled on this install "
		                    "(modules.forge.enabled)");
		return NULL;
	}

	database = venture_context_get_database(context);
	query = venture_query_new(VENTURE_TYPE_FORGE_RUN);

	if (!venture_string_is_empty(state) && (0 != g_strcmp0(state, "all")))
	{
		if (!venture_query_add_filter_string(query, "state",
		                                     VENTURE_FILTER_OP_EQ, state,
		                                     error))
			return NULL;
	}

	venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, NULL);
	venture_query_set_limit(query, (0 == limit) ? 50 : limit);
	venture_factory_scope(query, organization_ids, n_organizations);
	runs = venture_database_find(database, query, error);

	if (NULL == runs)
		return NULL;

	costs = g_ptr_array_new_with_free_func((GDestroyNotify)venture_money_free);
	success_costs = g_ptr_array_new_with_free_func(
		(GDestroyNotify)venture_money_free);
	ticket_titles = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
	                                      g_free);
	live = succeeded = failed = pull_requests = 0;
	input_tokens = output_tokens = total_seconds = timed = 0;

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "runs");
	json_builder_begin_array(builder);

	for (i = 0; i < runs->len; i++)
	{
		VentureEntity *run;
		g_autofree gchar *branch = NULL;
		g_autofree gchar *provider = NULL;
		g_autofree gchar *model = NULL;
		g_autofree gchar *summary = NULL;
		g_autofree gchar *failure = NULL;
		g_autoptr(GDateTime) started = NULL;
		g_autoptr(GDateTime) finished = NULL;
		VentureMoney *cost = NULL;
		VentureForgeRunState run_state;
		VentureForgeRunner runner;
		VentureForgeRunOutcome outcome;
		const gchar *title;
		gint64 ticket_id = 0;
		gint64 rule_id = 0;
		gint64 pr = 0;
		gint64 in_tokens = 0;
		gint64 out_tokens = 0;
		gint64 turns = 0;
		gint64 seconds;

		run = g_ptr_array_index(runs, i);
		g_object_get(run,
		             "ticket-id", &ticket_id, "rule-id", &rule_id,
		             "state", &run_state, "runner", &runner,
		             "outcome", &outcome, "branch", &branch,
		             "pull-request-number", &pr, "started-at", &started,
		             "finished-at", &finished, "provider", &provider,
		             "model", &model, "input-tokens", &in_tokens,
		             "output-tokens", &out_tokens, "turns", &turns,
		             "cost", &cost, "summary", &summary,
		             "failure-reason", &failure, NULL);

		title = g_hash_table_lookup(ticket_titles,
		                            GINT_TO_POINTER((gint)ticket_id));

		if (NULL == title)
		{
			g_autoptr(VentureEntity) ticket = NULL;
			gchar *label;

			ticket = venture_database_get(database, VENTURE_TYPE_TICKET,
			                              ticket_id, NULL);
			label = (NULL != ticket) ? venture_entity_get_display_name(ticket)
			                         : g_strdup_printf("ticket #%" G_GINT64_FORMAT,
			                                           ticket_id);
			g_hash_table_insert(ticket_titles, GINT_TO_POINTER((gint)ticket_id),
			                    label);
			title = label;
		}

		seconds = 0;

		if ((NULL != started) && (NULL != finished))
		{
			seconds = g_date_time_difference(finished, started)
			          / G_TIME_SPAN_SECOND;
			total_seconds += seconds;
			timed++;
		}
		else if (NULL != started)
		{
			g_autoptr(GDateTime) now = NULL;

			now = venture_time_now();
			seconds = g_date_time_difference(now, started) / G_TIME_SPAN_SECOND;
		}

		if ((VENTURE_FORGE_RUN_STATE_QUEUED == run_state) ||
		    (VENTURE_FORGE_RUN_STATE_RUNNING == run_state))
			live++;
		else if (VENTURE_FORGE_RUN_STATE_SUCCEEDED == run_state)
			succeeded++;
		else if ((VENTURE_FORGE_RUN_STATE_FAILED == run_state) ||
		         (VENTURE_FORGE_RUN_STATE_INTERRUPTED == run_state))
			failed++;

		if (pr > 0)
			pull_requests++;

		input_tokens += in_tokens;
		output_tokens += out_tokens;

		json_builder_begin_object(builder);
		venture_factory_add_identity(builder, run);
		json_builder_set_member_name(builder, "ticket_id");
		json_builder_add_int_value(builder, ticket_id);
		venture_factory_add_string(builder, "ticket", title);
		json_builder_set_member_name(builder, "rule_id");
		json_builder_add_int_value(builder, rule_id);
		venture_factory_add_enum(builder, "state", VENTURE_TYPE_FORGE_RUN_STATE,
		                         (gint)run_state);
		venture_factory_add_enum(builder, "runner", VENTURE_TYPE_FORGE_RUNNER,
		                         (gint)runner);
		venture_factory_add_enum(builder, "outcome",
		                         VENTURE_TYPE_FORGE_RUN_OUTCOME, (gint)outcome);
		venture_factory_add_string(builder, "branch", branch);
		json_builder_set_member_name(builder, "pull_request_number");
		json_builder_add_int_value(builder, pr);
		venture_factory_add_string(builder, "provider", provider);
		venture_factory_add_string(builder, "model", model);
		json_builder_set_member_name(builder, "input_tokens");
		json_builder_add_int_value(builder, in_tokens);
		json_builder_set_member_name(builder, "output_tokens");
		json_builder_add_int_value(builder, out_tokens);
		json_builder_set_member_name(builder, "turns");
		json_builder_add_int_value(builder, turns);
		json_builder_set_member_name(builder, "cost");

		if (NULL != cost)
		{
			g_autofree gchar *display = NULL;

			json_builder_add_value(builder, venture_money_to_json(cost));
			display = venture_money_to_display_string(cost, TRUE);
			venture_factory_add_string(builder, "cost_display", display);

			g_ptr_array_add(costs, venture_money_copy(cost));

			if (VENTURE_FORGE_RUN_STATE_SUCCEEDED == run_state)
				g_ptr_array_add(success_costs, venture_money_copy(cost));

			venture_money_free(cost);
		}
		else
		{
			json_builder_add_null_value(builder);
			venture_factory_add_string(builder, "cost_display", NULL);
		}

		venture_factory_add_time(builder, "started_at", started);
		venture_factory_add_time(builder, "finished_at", finished);
		json_builder_set_member_name(builder, "seconds");
		json_builder_add_int_value(builder, seconds);
		venture_factory_add_string(builder, "summary", summary);
		venture_factory_add_string(builder, "failure_reason", failure);
		json_builder_end_object(builder);
	}

	json_builder_end_array(builder);

	/* Totals over what was listed; a filtered list totals the filter. */
	/* The totals are in one currency: the one most runs were charged in,
	 * which on any real install is the only one. A run charged in another
	 * cannot be added without a rate nobody gave, so it is left out and
	 * counted, where adding it used to turn the whole total into nothing. */
	total_cost = venture_money_sum_dominant(costs, &other_currency);

	{
		g_autoptr(GPtrArray) same = NULL;

		same = g_ptr_array_new();

		for (i = 0; i < success_costs->len; i++)
		{
			if (0 == g_strcmp0(venture_money_get_currency(
			                       g_ptr_array_index(success_costs, i)),
			                   venture_money_get_currency(total_cost)))
				g_ptr_array_add(same, g_ptr_array_index(success_costs, i));
		}

		priced_successes = same->len;
		success_cost = venture_money_sum(same,
			venture_money_get_currency(total_cost), NULL);
	}

	json_builder_set_member_name(builder, "totals");
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "runs");
	json_builder_add_int_value(builder, (gint64)runs->len);
	json_builder_set_member_name(builder, "live");
	json_builder_add_int_value(builder, live);
	json_builder_set_member_name(builder, "succeeded");
	json_builder_add_int_value(builder, succeeded);
	json_builder_set_member_name(builder, "failed");
	json_builder_add_int_value(builder, failed);
	json_builder_set_member_name(builder, "pull_requests");
	json_builder_add_int_value(builder, pull_requests);
	json_builder_set_member_name(builder, "input_tokens");
	json_builder_add_int_value(builder, input_tokens);
	json_builder_set_member_name(builder, "output_tokens");
	json_builder_add_int_value(builder, output_tokens);
	json_builder_set_member_name(builder, "average_seconds");
	json_builder_add_int_value(builder, (timed > 0) ? total_seconds / timed : 0);
	json_builder_set_member_name(builder, "cost");
	json_builder_add_value(builder,
		(NULL != total_cost) ? venture_money_to_json(total_cost)
		                     : json_node_new(JSON_NODE_NULL));

	{
		g_autofree gchar *display = NULL;

		display = (NULL != total_cost)
			? venture_money_to_display_string(total_cost, TRUE) : NULL;
		venture_factory_add_string(builder, "cost_display", display);
	}

	json_builder_set_member_name(builder, "cost_other_currency_runs");
	json_builder_add_int_value(builder, (gint64)other_currency);
	json_builder_set_member_name(builder, "cost_per_success");

	/* Over the successes that were priced: one that reported no cost is
	 * not a free one, and dividing by it would flatter the figure. */
	if ((NULL != success_cost) && (priced_successes > 0))
	{
		g_autoptr(VentureMoney) each = NULL;
		g_autoptr(GPtrArray) shares = NULL;

		/* Allocated rather than divided, so the shares add back up. */
		shares = venture_money_allocate_evenly(success_cost, priced_successes,
		                                       NULL);

		if ((NULL != shares) && (shares->len > 0))
			each = venture_money_copy(g_ptr_array_index(shares, 0));

		if (NULL != each)
		{
			g_autofree gchar *display = NULL;

			json_builder_add_value(builder, venture_money_to_json(each));
			display = venture_money_to_display_string(each, TRUE);
			venture_factory_add_string(builder, "cost_per_success_display",
			                           display);
		}
		else
		{
			json_builder_add_null_value(builder);
			venture_factory_add_string(builder, "cost_per_success_display",
			                           NULL);
		}
	}
	else
	{
		json_builder_add_null_value(builder);
		venture_factory_add_string(builder, "cost_per_success_display", NULL);
	}

	json_builder_end_object(builder);
	json_builder_end_object(builder);

	return json_builder_get_root(builder);
}

/* ==========================================================================
 * Incidents
 * ========================================================================== */

VentureEntity *
venture_factory_open_fix_ticket(
	VentureContext		 *context,
	VentureEntity		 *incident,
	const VentureActor	 *actor,
	GError			**error
){
	VentureDatabase *database;
	g_autoptr(VentureTicket) ticket = NULL;
	g_autoptr(VentureEntity) release = NULL;
	g_autofree gchar *title = NULL;
	g_autofree gchar *summary = NULL;
	g_autofree gchar *description = NULL;
	VentureIncidentSeverity severity;
	VenturePriority priority;
	gint64 existing = 0;
	gint64 release_id = 0;
	gint64 repo_id = 0;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	g_return_val_if_fail(VENTURE_IS_INCIDENT(incident), NULL);

	/* Here rather than in each caller: the pages and the API checked it
	 * and the assistant's tool did not. */
	if (!venture_context_module_enabled(context, "factory"))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		                    "The factory module is disabled on this install "
		                    "(modules.factory.enabled)");
		return NULL;
	}

	if (!venture_context_module_enabled(context, "tickets"))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		                    "The tickets module is off, so there is nowhere "
		                    "to open the fix");
		return NULL;
	}

	database = venture_context_get_database(context);
	g_object_get(incident, "ticket-id", &existing, "title", &title,
	             "summary", &summary, "severity", &severity,
	             "release-id", &release_id, NULL);

	if (0 != existing)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
		            "This incident already has ticket #%" G_GINT64_FORMAT,
		            existing);
		return NULL;
	}

	/* Everything is down is urgent; cosmetic is low; the two in between
	 * map to the two in between. */
	switch (severity)
	{
	case VENTURE_INCIDENT_SEVERITY_SEV1: priority = VENTURE_PRIORITY_URGENT; break;
	case VENTURE_INCIDENT_SEVERITY_SEV2: priority = VENTURE_PRIORITY_HIGH;   break;
	case VENTURE_INCIDENT_SEVERITY_SEV4: priority = VENTURE_PRIORITY_LOW;    break;
	case VENTURE_INCIDENT_SEVERITY_SEV3:
	default:                             priority = VENTURE_PRIORITY_NORMAL; break;
	}

	if (0 != release_id)
	{
		release = venture_database_get(database, VENTURE_TYPE_RELEASE,
		                               release_id, NULL);

		if (NULL != release)
			g_object_get(release, "repo-id", &repo_id, NULL);
	}

	description = g_strdup_printf("Opened from incident #%" G_GINT64_FORMAT
	                              ".\n\n%s",
	                              venture_entity_get_id(incident),
	                              venture_string_is_empty(summary) ? "" : summary);

	ticket = venture_ticket_new();
	g_object_set(ticket,
	             "title", title,
	             "kind", VENTURE_TICKET_KIND_INTERNAL,
	             "status", VENTURE_TICKET_STATUS_TODO,
	             "priority", priority,
	             "issue-type", VENTURE_ISSUE_TYPE_BUG,
	             "description", description,
	             "repo-id", repo_id,
	             NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(ticket),
		venture_entity_get_organization_id(incident));

	if (!venture_database_begin(database, error))
		return NULL;

	if (!venture_database_save(database, VENTURE_ENTITY(ticket), actor, error))
	{
		venture_database_rollback(database);
		return NULL;
	}

	g_object_set(incident, "ticket-id",
	             venture_entity_get_id(VENTURE_ENTITY(ticket)), NULL);

	if (!venture_database_save(database, incident, actor, error))
	{
		venture_database_rollback(database);
		return NULL;
	}

	if (!venture_database_commit(database, error))
		return NULL;

	/* The link both ways, so the ticket's page says what it fixes. Built
	 * checked, then saved like any record; a link that cannot be made is
	 * not worth refusing the ticket over. */
	{
		g_autoptr(VentureRecordLink) link = NULL;

		link = venture_record_link_create(database, "incident",
		                                  venture_entity_get_id(incident),
		                                  VENTURE_LINK_KIND_CAUSES, "ticket",
		                                  venture_entity_get_id(
		                                  	VENTURE_ENTITY(ticket)),
		                                  "The fix", NULL);

		if (NULL != link)
			venture_database_save(database, VENTURE_ENTITY(link), actor, NULL);
	}

	return VENTURE_ENTITY(g_steal_pointer(&ticket));
}

/* ==========================================================================
 * Lifecycle
 *
 * The reports measure the loop from its timestamps: hours to restore from
 * an incident's started and resolved, deployment frequency from deployed,
 * lead time from released. A form, the API, the CLI and the assistant can
 * all move a status without touching the matching timestamp, and a record
 * resolved with no resolved time is one every report silently leaves out.
 * So the stamps are derived here, on every writer, the way a ticket's
 * resolved time is.
 * ========================================================================== */

/*
 * Stamps a datetime property with now when it is empty.
 */
static void
venture_factory_stamp_if_empty(
	VentureEntity	*entity,
	const gchar	*property
){
	g_autoptr(GDateTime) existing = NULL;
	g_autoptr(GDateTime) now = NULL;

	g_object_get(entity, property, &existing, NULL);

	if (NULL != existing)
		return;

	now = venture_time_now();
	g_object_set(entity, property, now, NULL);
}

static gboolean
venture_factory_incident_is_over(VentureIncidentStatus status)
{
	return (VENTURE_INCIDENT_STATUS_RESOLVED == status) ||
	       (VENTURE_INCIDENT_STATUS_POSTMORTEM == status);
}

/*
 * An incident starts when it is raised unless somebody says otherwise, is
 * resolved when its status says so, stops being resolved when it is
 * reopened, and cannot end before it began.
 */
static gboolean
venture_factory_validate_incident(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	gpointer	  user_data,
	GError		**error
){
	g_autoptr(GDateTime) started_at = NULL;
	g_autoptr(GDateTime) resolved_at = NULL;
	VentureIncidentStatus status;

	(void)database;
	(void)user_data;

	g_object_get(entity, "status", &status, NULL);

	if (NULL == previous)
		venture_factory_stamp_if_empty(entity, "started-at");

	if (venture_factory_incident_is_over(status))
	{
		venture_factory_stamp_if_empty(entity, "resolved-at");
	}
	else if (NULL != previous)
	{
		VentureIncidentStatus was;

		/* Reopened: an incident that is happening again has not been
		 * resolved, and the time to restore runs until it is. Only the
		 * transition clears it, so a save that leaves the status alone
		 * leaves the field alone too. */
		g_object_get(previous, "status", &was, NULL);

		if (venture_factory_incident_is_over(was))
			g_object_set(entity, "resolved-at", NULL, NULL);
	}

	g_object_get(entity, "started-at", &started_at,
	             "resolved-at", &resolved_at, NULL);

	if ((NULL != started_at) && (NULL != resolved_at) &&
	    (g_date_time_compare(resolved_at, started_at) < 0))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "An incident cannot be resolved before it "
		                    "started");
		return FALSE;
	}

	return TRUE;
}

/*
 * A deployment that succeeded went live at some moment; with none given it
 * is this one. Without it the deployment sorts as never having happened
 * and the environment goes on saying it runs the release before.
 */
static gboolean
venture_factory_validate_deployment(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	gpointer	  user_data,
	GError		**error
){
	VentureDeploymentStatus status;

	(void)database;
	(void)previous;
	(void)user_data;
	(void)error;

	g_object_get(entity, "status", &status, NULL);

	if ((VENTURE_DEPLOYMENT_STATUS_SUCCEEDED == status) ||
	    (VENTURE_DEPLOYMENT_STATUS_ROLLED_BACK == status))
		venture_factory_stamp_if_empty(entity, "deployed-at");

	return TRUE;
}

static gboolean
venture_factory_validate_milestone(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	gpointer	  user_data,
	GError		**error
){
	VentureMilestoneStatus status;

	(void)database;
	(void)user_data;
	(void)error;

	g_object_get(entity, "status", &status, NULL);

	if (VENTURE_MILESTONE_STATUS_COMPLETED == status)
	{
		venture_factory_stamp_if_empty(entity, "completed-at");
	}
	else if (NULL != previous)
	{
		VentureMilestoneStatus was;

		g_object_get(previous, "status", &was, NULL);

		if (VENTURE_MILESTONE_STATUS_COMPLETED == was)
			g_object_set(entity, "completed-at", NULL, NULL);
	}

	return TRUE;
}

/*
 * Released means a release time. A release typed in as released with no
 * date is otherwise outside every period of every report.
 */
static gboolean
venture_factory_validate_release(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	gpointer	  user_data,
	GError		**error
){
	VentureReleaseStatus status;

	(void)database;
	(void)previous;
	(void)user_data;
	(void)error;

	g_object_get(entity, "status", &status, NULL);

	if (VENTURE_RELEASE_STATUS_RELEASED == status)
		venture_factory_stamp_if_empty(entity, "released-at");

	return TRUE;
}

/*
 * A build recorded by hand gets the times the webhook would have given it.
 */
static gboolean
venture_factory_validate_build(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	gpointer	  user_data,
	GError		**error
){
	g_autoptr(GDateTime) started_at = NULL;
	g_autoptr(GDateTime) finished_at = NULL;
	VentureBuildStatus status;
	VentureBuildTrigger trigger;

	(void)database;
	(void)previous;
	(void)user_data;

	g_object_get(entity, "status", &status, NULL);

	if (VENTURE_BUILD_STATUS_QUEUED != status)
		venture_factory_stamp_if_empty(entity, "started-at");

	if ((VENTURE_BUILD_STATUS_SUCCEEDED == status) ||
	    (VENTURE_BUILD_STATUS_FAILED == status) ||
	    (VENTURE_BUILD_STATUS_CANCELLED == status))
		venture_factory_stamp_if_empty(entity, "finished-at");

	g_object_get(entity, "started-at", &started_at,
	             "finished-at", &finished_at, "trigger", &trigger, NULL);

	/* Only a build a person typed is refused over its times. What the
	 * forge reports is the forge's to get wrong: refusing the delivery
	 * would lose the build, and its clock is not ours to argue with. */
	if ((VENTURE_BUILD_TRIGGER_MANUAL == trigger) &&
	    (NULL != started_at) && (NULL != finished_at) &&
	    (g_date_time_compare(finished_at, started_at) < 0))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "A build cannot finish before it started");
		return FALSE;
	}

	return TRUE;
}

void
venture_factory_install(VentureContext *context)
{
	VentureDatabase *database;

	g_return_if_fail(VENTURE_IS_CONTEXT(context));

	database = venture_context_get_database(context);

	venture_database_add_save_validator(database, VENTURE_TYPE_INCIDENT,
	                                    venture_factory_validate_incident,
	                                    context, NULL);
	venture_database_add_save_validator(database, VENTURE_TYPE_DEPLOYMENT,
	                                    venture_factory_validate_deployment,
	                                    context, NULL);
	venture_database_add_save_validator(database, VENTURE_TYPE_MILESTONE,
	                                    venture_factory_validate_milestone,
	                                    context, NULL);
	venture_database_add_save_validator(database, VENTURE_TYPE_RELEASE,
	                                    venture_factory_validate_release,
	                                    context, NULL);
	venture_database_add_save_validator(database, VENTURE_TYPE_BUILD,
	                                    venture_factory_validate_build,
	                                    context, NULL);
}

/* ==========================================================================
 * Readiness
 *
 * "Can this go out?" is a question somebody answers by opening six pages:
 * the tickets, the links, the builds, the incidents, the milestone, the
 * changelog. This answers it once, the same way for the page, the API, the
 * CLI and the assistant, as a list of checks that each pass, warn or fail.
 * It advises and never refuses: a release with a failing check can still
 * be published, because the person pressing the button may know why the
 * check is wrong.
 * ========================================================================== */

typedef struct
{
	JsonBuilder	*builder;
	guint		 passed;
	guint		 warned;
	guint		 failed;
} VentureFactoryChecks;

static gboolean
venture_factory_ticket_is_finished(VentureEntity *ticket)
{
	VentureTicketStatus status;

	g_object_get(ticket, "status", &status, NULL);

	return (VENTURE_TICKET_STATUS_DONE == status) ||
	       (VENTURE_TICKET_STATUS_CANCELLED == status);
}

/*
 * One check: a stable key for a program, a label and a sentence for a
 * person, and one of pass, warn or fail. Takes ownership of nothing.
 */
static void
venture_factory_check(
	VentureFactoryChecks	*checks,
	const gchar		*key,
	const gchar		*label,
	const gchar		*state,
	const gchar		*detail
){
	if (0 == g_strcmp0(state, "pass"))
		checks->passed++;
	else if (0 == g_strcmp0(state, "warn"))
		checks->warned++;
	else
		checks->failed++;

	json_builder_begin_object(checks->builder);
	venture_factory_add_string(checks->builder, "key", key);
	venture_factory_add_string(checks->builder, "label", label);
	venture_factory_add_string(checks->builder, "state", state);
	venture_factory_add_string(checks->builder, "detail", detail);
	json_builder_end_object(checks->builder);
}

/*
 * "#4, #9, #12 and 3 more", for the sentence that says which.
 */
static gchar *
venture_factory_id_list(
	GArray	*ids,
	guint	 show
){
	GString *out;
	guint i;

	out = g_string_new(NULL);

	for (i = 0; (i < ids->len) && (i < show); i++)
		g_string_append_printf(out, "%s#%" G_GINT64_FORMAT,
		                       (i > 0) ? ", " : "",
		                       g_array_index(ids, gint64, i));

	if (ids->len > show)
		g_string_append_printf(out, " and %u more", ids->len - show);

	return g_string_free(out, FALSE);
}

/*
 * The latest build that says anything about a release: one that names it,
 * else the newest on its repository.
 *
 * Returns: (transfer full) (nullable): the build
 */
static VentureEntity *
venture_factory_release_build(
	VentureDatabase	*database,
	gint64		 release_id,
	gint64		 repo_id,
	gboolean	*out_own
){
	gint pass;

	*out_own = FALSE;

	for (pass = 0; pass < 2; pass++)
	{
		g_autoptr(VentureQuery) query = NULL;
		VentureEntity *build;

		if ((1 == pass) && (0 == repo_id))
			continue;

		query = venture_query_new(VENTURE_TYPE_BUILD);

		if (!venture_query_add_filter_int(query,
		                                  (0 == pass) ? "release-id" : "repo-id",
		                                  VENTURE_FILTER_OP_EQ,
		                                  (0 == pass) ? release_id : repo_id,
		                                  NULL))
			return NULL;

		/* The repository speaks for a release through its default
		 * branch, which is what gets tagged. Somebody's feature branch
		 * being red says nothing about whether this can go out. */
		if (1 == pass)
		{
			g_autoptr(VentureEntity) repo = NULL;
			g_autofree gchar *default_branch = NULL;

			repo = venture_database_get(database, VENTURE_TYPE_FORGE_REPO,
			                            repo_id, NULL);

			if (NULL != repo)
				g_object_get(repo, "default-branch", &default_branch, NULL);

			if (!venture_string_is_empty(default_branch) &&
			    !venture_query_add_filter_string(query, "ref",
			                                     VENTURE_FILTER_OP_EQ,
			                                     default_branch, NULL))
				return NULL;
		}

		venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, NULL);
		venture_query_set_limit(query, 1);
		build = venture_database_find_one(database, query, NULL);

		if (NULL != build)
		{
			*out_own = (0 == pass);
			return build;
		}
	}

	return NULL;
}

JsonNode *
venture_factory_release_readiness(
	VentureContext	 *context,
	VentureEntity	 *release,
	GError		**error
){
	VentureDatabase *database;
	VentureFactoryChecks checks = { NULL, 0, 0, 0 };
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(GPtrArray) tickets = NULL;
	g_autoptr(GArray) unfinished = NULL;
	g_autofree gchar *number = NULL;
	g_autofree gchar *changelog = NULL;
	VentureReleaseStatus status;
	gint64 release_id;
	gint64 repo_id = 0;
	gint64 milestone_id = 0;
	guint shipping;
	guint total;
	guint i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	g_return_val_if_fail(VENTURE_IS_RELEASE(release), NULL);

	if (!venture_context_module_enabled(context, "factory"))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		                    "The factory module is disabled on this install "
		                    "(modules.factory.enabled)");
		return NULL;
	}

	database = venture_context_get_database(context);
	release_id = venture_entity_get_id(release);
	g_object_get(release, "number", &number, "changelog", &changelog,
	             "status", &status, "repo-id", &repo_id,
	             "milestone-id", &milestone_id, NULL);

	builder = json_builder_new();
	checks.builder = builder;
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "release_id");
	json_builder_add_int_value(builder, release_id);
	venture_factory_add_string(builder, "number", number);
	venture_factory_add_enum(builder, "status", VENTURE_TYPE_RELEASE_STATUS,
	                         (gint)status);
	json_builder_set_member_name(builder, "checks");
	json_builder_begin_array(builder);

	/* What it carries, and whether that is finished. */
	tickets = venture_factory_release_tickets(database, release_id);
	unfinished = g_array_new(FALSE, FALSE, sizeof(gint64));
	shipping = 0;

	for (i = 0; (NULL != tickets) && (i < tickets->len); i++)
	{
		VentureEntity *ticket;
		VentureTicketStatus ticket_status;
		gint64 id;

		ticket = g_ptr_array_index(tickets, i);
		g_object_get(ticket, "status", &ticket_status, NULL);

		if (VENTURE_TICKET_STATUS_CANCELLED == ticket_status)
			continue;

		shipping++;

		if (VENTURE_TICKET_STATUS_DONE != ticket_status)
		{
			id = venture_entity_get_id(ticket);
			g_array_append_val(unfinished, id);
		}
	}

	if (0 == shipping)
	{
		venture_factory_check(&checks, "tickets", "Tickets", "warn",
			"No tickets are marked as fixed in this release, so there is "
			"nothing to say it carries.");
	}
	else if (unfinished->len > 0)
	{
		g_autofree gchar *which = NULL;
		g_autofree gchar *detail = NULL;

		which = venture_factory_id_list(unfinished, 5);
		detail = g_strdup_printf("%u of %u tickets are not done: %s.",
		                         unfinished->len, shipping, which);
		venture_factory_check(&checks, "tickets", "Tickets", "fail", detail);
	}
	else
	{
		g_autofree gchar *detail = NULL;

		detail = g_strdup_printf("All %u tickets are done.", shipping);
		venture_factory_check(&checks, "tickets", "Tickets", "pass", detail);
	}

	/* What somebody said must happen first: a link that reads, from the
	 * release, as blocked by or depending on a ticket still open. */
	{
		g_autoptr(GPtrArray) links = NULL;
		g_autoptr(GArray) blocking = NULL;

		links = venture_record_link_find_for(database, "release", release_id,
		                                     NULL);
		blocking = g_array_new(FALSE, FALSE, sizeof(gint64));

		for (i = 0; (NULL != links) && (i < links->len); i++)
		{
			g_autoptr(VentureEntity) other = NULL;
			g_autofree gchar *other_type = NULL;
			g_autofree gchar *other_label = NULL;
			VentureLinkKind kind;
			gint64 other_id = 0;

			if (!venture_record_link_other_end(g_ptr_array_index(links, i),
			                                   "release", release_id,
			                                   &other_type, &other_id,
			                                   &other_label, &kind))
				continue;

			if ((VENTURE_LINK_KIND_BLOCKED_BY != kind) &&
			    (VENTURE_LINK_KIND_DEPENDS_ON != kind))
				continue;

			if (0 != g_strcmp0(other_type, "ticket"))
				continue;

			other = venture_record_link_resolve(database, other_type, other_id,
			                                    NULL);

			if ((NULL != other) && !venture_factory_ticket_is_finished(other))
				g_array_append_val(blocking, other_id);
		}

		if (blocking->len > 0)
		{
			g_autofree gchar *which = NULL;
			g_autofree gchar *detail = NULL;

			which = venture_factory_id_list(blocking, 5);
			detail = g_strdup_printf("Blocked by %u open ticket%s: %s.",
			                         blocking->len,
			                         (1 == blocking->len) ? "" : "s", which);
			venture_factory_check(&checks, "blockers", "Blockers", "fail",
			                      detail);
		}
		else
		{
			venture_factory_check(&checks, "blockers", "Blockers", "pass",
				"Nothing open is linked as blocking this release.");
		}
	}

	/* Whether it builds. */
	{
		g_autoptr(VentureEntity) build = NULL;
		gboolean own = FALSE;

		build = venture_factory_release_build(database, release_id, repo_id,
		                                      &own);

		if (NULL == build)
		{
			venture_factory_check(&checks, "build", "Build", "warn",
				"No build is recorded for this release or its repository.");
		}
		else
		{
			g_autofree gchar *label = NULL;
			g_autofree gchar *detail = NULL;
			VentureBuildStatus build_status;
			const gchar *state;

			label = venture_entity_get_display_name(build);
			g_object_get(build, "status", &build_status, NULL);

			if (VENTURE_BUILD_STATUS_SUCCEEDED == build_status)
				state = "pass";
			else if (VENTURE_BUILD_STATUS_FAILED == build_status)
				state = "fail";
			else
				state = "warn";

			detail = g_strdup_printf("The latest build %s (#%" G_GINT64_FORMAT
			                         " %s) %s.",
			                         own ? "of this release"
			                             : "of the repository's default "
			                               "branch",
			                         venture_entity_get_id(build), label,
			                         venture_enum_to_nick(VENTURE_TYPE_BUILD_STATUS,
			                                              (gint)build_status));
			venture_factory_check(&checks, "build", "Build", state, detail);
		}
	}

	/* What is on fire. One that names this release is its own problem;
	 * a serious one anywhere is a reason to think before adding to it. */
	{
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(GPtrArray) incidents = NULL;
		guint own_incidents = 0;
		guint serious = 0;

		query = venture_query_new(VENTURE_TYPE_INCIDENT);
		venture_query_add_filter_string(query, "status", VENTURE_FILTER_OP_NE,
		                                "resolved", NULL);
		venture_query_add_filter_string(query, "status", VENTURE_FILTER_OP_NE,
		                                "postmortem", NULL);

		if (0 != venture_entity_get_organization_id(release))
			venture_query_set_organization(query,
				venture_entity_get_organization_id(release));

		venture_query_set_limit(query, 0);
		incidents = venture_database_find(database, query, NULL);

		for (i = 0; (NULL != incidents) && (i < incidents->len); i++)
		{
			VentureIncidentSeverity severity;
			gint64 named = 0;

			g_object_get(g_ptr_array_index(incidents, i), "severity", &severity,
			             "release-id", &named, NULL);

			if (named == release_id)
				own_incidents++;
			else if ((VENTURE_INCIDENT_SEVERITY_SEV1 == severity) ||
			         (VENTURE_INCIDENT_SEVERITY_SEV2 == severity))
				serious++;
		}

		if (own_incidents > 0)
		{
			g_autofree gchar *detail = NULL;

			detail = g_strdup_printf("%u open incident%s name%s this release.",
			                         own_incidents,
			                         (1 == own_incidents) ? "" : "s",
			                         (1 == own_incidents) ? "s" : "");
			venture_factory_check(&checks, "incidents", "Incidents", "fail",
			                      detail);
		}
		else if (serious > 0)
		{
			g_autofree gchar *detail = NULL;

			detail = g_strdup_printf("%u sev1 or sev2 incident%s still open "
			                         "elsewhere.", serious,
			                         (1 == serious) ? " is" : "s are");
			venture_factory_check(&checks, "incidents", "Incidents", "warn",
			                      detail);
		}
		else
		{
			venture_factory_check(&checks, "incidents", "Incidents", "pass",
			                      "No open incident names this release, and "
			                      "no sev1 or sev2 is open anywhere.");
		}
	}

	if (0 != milestone_id)
	{
		g_autofree gchar *detail = NULL;
		gint64 planned;
		gint64 done;

		venture_factory_milestone_progress(database, milestone_id, &planned,
		                                   &done);
		detail = g_strdup_printf("Its milestone is %" G_GINT64_FORMAT " of %"
		                         G_GINT64_FORMAT " tickets along.", done,
		                         planned);
		venture_factory_check(&checks, "milestone", "Milestone",
		                      ((planned > 0) && (done < planned)) ? "warn"
		                                                          : "pass",
		                      detail);
	}

	venture_factory_check(&checks, "changelog", "Changelog",
		venture_string_is_empty(changelog) ? "warn" : "pass",
		venture_string_is_empty(changelog)
			? "The changelog is empty; Draft fills it from the tickets."
			: "The changelog is written.");

	venture_factory_check(&checks, "repository", "Repository",
		(0 == repo_id) ? "warn" : "pass",
		(0 == repo_id)
			? "The release names no repository, so it cannot be published "
			  "from here."
			: "The release names the repository its tag lives in.");

	if (VENTURE_RELEASE_STATUS_YANKED == status)
		venture_factory_check(&checks, "status", "Status", "fail",
		                      "The release was yanked.");

	json_builder_end_array(builder);

	total = checks.passed + checks.warned + checks.failed;
	json_builder_set_member_name(builder, "ready");
	json_builder_add_boolean_value(builder, 0 == checks.failed);
	json_builder_set_member_name(builder, "passed");
	json_builder_add_int_value(builder, checks.passed);
	json_builder_set_member_name(builder, "warnings");
	json_builder_add_int_value(builder, checks.warned);
	json_builder_set_member_name(builder, "blockers");
	json_builder_add_int_value(builder, checks.failed);

	/* A warning is half a pass: it is a reason to look, not to stop. */
	json_builder_set_member_name(builder, "score");
	json_builder_add_int_value(builder,
		(total > 0) ? ((checks.passed * 100) + (checks.warned * 50)) / total
		            : 0);
	json_builder_end_object(builder);

	return json_builder_get_root(builder);
}

/* ==========================================================================
 * Deploying and rolling back
 * ========================================================================== */

VentureEntity *
venture_factory_deploy_release(
	VentureContext		 *context,
	VentureEntity		 *release,
	gint64			  environment_id,
	const gchar		 *notes,
	const VentureActor	 *actor,
	GError			**error
){
	VentureDatabase *database;
	g_autoptr(VentureEntity) environment = NULL;
	g_autoptr(VentureEntity) deployment = NULL;
	g_autoptr(VentureEntity) build = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autofree gchar *number = NULL;
	VentureReleaseStatus status;
	gboolean own = FALSE;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	g_return_val_if_fail(VENTURE_IS_RELEASE(release), NULL);

	if (!venture_context_module_enabled(context, "factory"))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		                    "The factory module is disabled on this install "
		                    "(modules.factory.enabled)");
		return NULL;
	}

	database = venture_context_get_database(context);
	g_object_get(release, "status", &status, "number", &number, NULL);

	if (VENTURE_RELEASE_STATUS_YANKED == status)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
		            "Release %s was yanked, and a withdrawn release is not "
		            "one to put anywhere", number);
		return NULL;
	}

	if (0 == environment_id)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "Name the environment to deploy to");
		return NULL;
	}

	environment = venture_database_get(database, VENTURE_TYPE_ENVIRONMENT,
	                                   environment_id, error);

	if (NULL == environment)
		return NULL;

	/* The build that was deployed is the release's own latest green one,
	 * where there is one; a build of something else is not evidence. */
	build = venture_factory_release_build(database,
		venture_entity_get_id(release), 0, &own);

	if (NULL != build)
	{
		VentureBuildStatus build_status;

		g_object_get(build, "status", &build_status, NULL);

		if (VENTURE_BUILD_STATUS_SUCCEEDED != build_status)
			g_clear_object(&build);
	}

	now = venture_time_now();
	deployment = VENTURE_ENTITY(venture_deployment_new());
	venture_entity_set_organization_id(deployment,
		venture_entity_get_organization_id(environment));
	g_object_set(deployment,
	             "release-id", venture_entity_get_id(release),
	             "environment-id", environment_id,
	             "build-id", (NULL != build) ? venture_entity_get_id(build)
	                                         : (gint64)0,
	             "status", VENTURE_DEPLOYMENT_STATUS_SUCCEEDED,
	             "deployed-at", now,
	             "deployed-by", ((NULL != actor) && (NULL != actor->name))
	                                ? actor->name : "venture",
	             "notes", notes,
	             NULL);

	if (!venture_database_save(database, deployment, actor, error))
		return NULL;

	return g_steal_pointer(&deployment);
}

VentureEntity *
venture_factory_rollback_environment(
	VentureContext		 *context,
	VentureEntity		 *environment,
	const gchar		 *reason,
	const VentureActor	 *actor,
	GError			**error
){
	VentureDatabase *database;
	g_autoptr(VentureEntity) current = NULL;
	g_autoptr(VentureEntity) restored = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) earlier = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autofree gchar *existing_notes = NULL;
	g_autofree gchar *withdrawn_notes = NULL;
	g_autofree gchar *restored_notes = NULL;
	VentureEntity *previous = NULL;
	gint64 environment_id;
	gint64 current_release = 0;
	gint64 previous_release = 0;
	guint i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	g_return_val_if_fail(VENTURE_IS_ENVIRONMENT(environment), NULL);

	if (!venture_context_module_enabled(context, "factory"))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		                    "The factory module is disabled on this install "
		                    "(modules.factory.enabled)");
		return NULL;
	}

	database = venture_context_get_database(context);
	environment_id = venture_entity_get_id(environment);
	current = venture_factory_current_deployment(database, environment_id);

	if (NULL == current)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
		                    "Nothing is running in this environment, so there "
		                    "is nothing to roll back");
		return NULL;
	}

	g_object_get(current, "release-id", &current_release,
	             "notes", &existing_notes, NULL);

	/* What was running before: the newest deployment that went live here
	 * with a different release. One that was itself rolled back is not
	 * somewhere to go back to. */
	query = venture_query_new(VENTURE_TYPE_DEPLOYMENT);

	if (!venture_query_add_filter_int(query, "environment-id",
	                                  VENTURE_FILTER_OP_EQ, environment_id,
	                                  error) ||
	    !venture_query_add_filter_string(query, "status", VENTURE_FILTER_OP_EQ,
	                                     "succeeded", error))
		return NULL;

	venture_query_add_order(query, "deployed-at", VENTURE_SORT_DESCENDING,
	                        NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, NULL);
	venture_query_set_limit(query, 0);
	earlier = venture_database_find(database, query, error);

	if (NULL == earlier)
		return NULL;

	for (i = 0; i < earlier->len; i++)
	{
		VentureEntity *candidate;
		gint64 candidate_release = 0;

		candidate = g_ptr_array_index(earlier, i);
		g_object_get(candidate, "release-id", &candidate_release, NULL);

		if ((venture_entity_get_id(candidate) != venture_entity_get_id(current)) &&
		    (candidate_release != current_release))
		{
			previous = candidate;
			previous_release = candidate_release;
			break;
		}
	}

	if (NULL == previous)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
		                    "No earlier release ever went live in this "
		                    "environment, so there is nothing to go back to");
		return NULL;
	}

	now = venture_time_now();
	withdrawn_notes = g_strdup_printf("%s%sRolled back%s%s.",
		venture_string_is_empty(existing_notes) ? "" : existing_notes,
		venture_string_is_empty(existing_notes) ? "" : "\n\n",
		venture_string_is_empty(reason) ? "" : ": ",
		venture_string_is_empty(reason) ? "" : reason);
	restored_notes = g_strdup_printf("Rollback of deployment #%" G_GINT64_FORMAT
		"%s%s.", venture_entity_get_id(current),
		venture_string_is_empty(reason) ? "" : ": ",
		venture_string_is_empty(reason) ? "" : reason);

	restored = VENTURE_ENTITY(venture_deployment_new());
	venture_entity_set_organization_id(restored,
		venture_entity_get_organization_id(environment));
	g_object_set(restored,
	             "release-id", previous_release,
	             "environment-id", environment_id,
	             "status", VENTURE_DEPLOYMENT_STATUS_SUCCEEDED,
	             "deployed-at", now,
	             "deployed-by", ((NULL != actor) && (NULL != actor->name))
	                                ? actor->name : "venture",
	             "notes", restored_notes,
	             NULL);

	{
		gint64 build_id = 0;

		g_object_get(previous, "build-id", &build_id, NULL);
		g_object_set(restored, "build-id", build_id, NULL);
	}

	/* Both or neither: a deployment marked rolled back with nothing in
	 * its place would leave the environment saying it runs the release
	 * before by accident rather than on the record. */
	if (!venture_database_begin(database, error))
		return NULL;

	g_object_set(current, "status", VENTURE_DEPLOYMENT_STATUS_ROLLED_BACK,
	             "notes", withdrawn_notes, NULL);

	if (!venture_database_save(database, current, actor, error) ||
	    !venture_database_save(database, restored, actor, error))
	{
		venture_database_rollback(database);
		return NULL;
	}

	if (!venture_database_commit(database, error))
		return NULL;

	/* And say which replaced which. Worth having, not worth failing for. */
	{
		g_autoptr(VentureRecordLink) link = NULL;

		link = venture_record_link_create(database, "deployment",
			venture_entity_get_id(restored), VENTURE_LINK_KIND_SUPERSEDES,
			"deployment", venture_entity_get_id(current), "Rollback", NULL);

		if (NULL != link)
			venture_database_save(database, VENTURE_ENTITY(link), actor, NULL);
	}

	return g_steal_pointer(&restored);
}

/* ==========================================================================
 * Forecast
 * ========================================================================== */

/* How far back the pace is measured. Four weeks is long enough to smooth
 * over one quiet week and short enough to notice the team changed. */
#define VENTURE_FACTORY_VELOCITY_DAYS (28)

JsonNode *
venture_factory_milestone_forecast(
	VentureContext	 *context,
	VentureEntity	 *milestone,
	GError		**error
){
	VentureDatabase *database;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) tickets = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autoptr(GDateTime) since = NULL;
	g_autoptr(GDateTime) due = NULL;
	g_autoptr(GDateTime) projected = NULL;
	VentureMilestoneStatus status;
	const gchar *state;
	gdouble per_week;
	gint64 total = 0;
	gint64 done = 0;
	gint64 remaining;
	gint64 recent = 0;
	gint64 days_over = 0;
	guint i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	g_return_val_if_fail(VENTURE_IS_MILESTONE(milestone), NULL);

	database = venture_context_get_database(context);
	g_object_get(milestone, "due-on", &due, "status", &status, NULL);

	query = venture_query_new(VENTURE_TYPE_TICKET);

	if (!venture_query_add_filter_int(query, "milestone-id",
	                                  VENTURE_FILTER_OP_EQ,
	                                  venture_entity_get_id(milestone), error))
		return NULL;

	venture_query_set_limit(query, 0);
	tickets = venture_database_find(database, query, error);

	if (NULL == tickets)
		return NULL;

	now = venture_time_now();
	since = g_date_time_add_days(now, -VENTURE_FACTORY_VELOCITY_DAYS);

	for (i = 0; i < tickets->len; i++)
	{
		g_autoptr(GDateTime) resolved_at = NULL;
		VentureTicketStatus ticket_status;

		g_object_get(g_ptr_array_index(tickets, i), "status", &ticket_status,
		             "resolved-at", &resolved_at, NULL);

		/* Cancelled work is not work left and was not work done: it
		 * leaves the plan rather than counting towards the pace. */
		if (VENTURE_TICKET_STATUS_CANCELLED == ticket_status)
			continue;

		total++;

		if (VENTURE_TICKET_STATUS_DONE != ticket_status)
			continue;

		done++;

		if ((NULL != resolved_at) &&
		    (g_date_time_compare(resolved_at, since) >= 0))
			recent++;
	}

	remaining = total - done;
	per_week = ((gdouble)recent * 7.0) / (gdouble)VENTURE_FACTORY_VELOCITY_DAYS;

	if ((remaining > 0) && (per_week > 0.0))
	{
		gdouble days;

		days = ((gdouble)remaining / per_week) * 7.0;
		projected = g_date_time_add_seconds(now, days * 86400.0);
	}

	if ((VENTURE_MILESTONE_STATUS_COMPLETED == status) ||
	    ((total > 0) && (0 == remaining)))
		state = "done";
	else if (VENTURE_MILESTONE_STATUS_CANCELLED == status)
		state = "cancelled";
	else if (0 == total)
		state = "empty";
	else if ((NULL != due) && (g_date_time_compare(due, now) < 0))
		state = "overdue";
	else if (NULL == projected)
		state = "stalled";
	else if (NULL == due)
		state = "no_due_date";
	else if (g_date_time_compare(projected, due) > 0)
		state = "at_risk";
	else
		state = "on_track";

	if ((NULL != due) && (NULL != projected))
		days_over = g_date_time_difference(projected, due) / G_TIME_SPAN_DAY;
	else if ((NULL != due) && (0 == g_strcmp0(state, "overdue")))
		days_over = g_date_time_difference(now, due) / G_TIME_SPAN_DAY;

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "milestone_id");
	json_builder_add_int_value(builder, venture_entity_get_id(milestone));
	venture_factory_add_string(builder, "state", state);
	json_builder_set_member_name(builder, "tickets");
	json_builder_add_int_value(builder, total);
	json_builder_set_member_name(builder, "done");
	json_builder_add_int_value(builder, done);
	json_builder_set_member_name(builder, "remaining");
	json_builder_add_int_value(builder, remaining);
	json_builder_set_member_name(builder, "closed_recently");
	json_builder_add_int_value(builder, recent);
	json_builder_set_member_name(builder, "velocity_days");
	json_builder_add_int_value(builder, VENTURE_FACTORY_VELOCITY_DAYS);
	json_builder_set_member_name(builder, "per_week");
	json_builder_add_double_value(builder, per_week);
	venture_factory_add_time(builder, "due_on", due);
	venture_factory_add_time(builder, "projected_on", projected);
	json_builder_set_member_name(builder, "days_over");
	json_builder_add_int_value(builder, days_over);
	json_builder_end_object(builder);

	return json_builder_get_root(builder);
}

/* ==========================================================================
 * Builds
 * ========================================================================== */

VentureEntity *
venture_factory_open_build_ticket(
	VentureContext		 *context,
	VentureEntity		 *build,
	const VentureActor	 *actor,
	GError			**error
){
	VentureDatabase *database;
	g_autoptr(VentureTicket) ticket = NULL;
	g_autoptr(VentureEntity) repo = NULL;
	g_autoptr(GPtrArray) links = NULL;
	g_autoptr(GString) description = NULL;
	g_autofree gchar *title = NULL;
	g_autofree gchar *workflow = NULL;
	g_autofree gchar *ref = NULL;
	g_autofree gchar *commit = NULL;
	g_autofree gchar *url = NULL;
	g_autofree gchar *log_excerpt = NULL;
	g_autofree gchar *build_title = NULL;
	g_autofree gchar *default_branch = NULL;
	VentureBuildStatus status;
	gint64 repo_id = 0;
	gint64 release_id = 0;
	guint i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	g_return_val_if_fail(VENTURE_IS_BUILD(build), NULL);

	if (!venture_context_module_enabled(context, "factory"))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		                    "The factory module is disabled on this install "
		                    "(modules.factory.enabled)");
		return NULL;
	}

	if (!venture_context_module_enabled(context, "tickets"))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		                    "The tickets module is off, so there is nowhere "
		                    "to open the fix");
		return NULL;
	}

	database = venture_context_get_database(context);
	g_object_get(build, "status", &status, "workflow", &workflow, "ref", &ref,
	             "commit", &commit, "url", &url, "log-excerpt", &log_excerpt,
	             "title", &build_title, "repo-id", &repo_id,
	             "release-id", &release_id, NULL);

	if (VENTURE_BUILD_STATUS_FAILED != status)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
		                    "Only a failed build needs a ticket to fix it");
		return NULL;
	}

	/* A build has no ticket field, so the link is the record of there
	 * being one already. */
	links = venture_record_link_find_for(database, "build",
	                                     venture_entity_get_id(build), NULL);

	for (i = 0; (NULL != links) && (i < links->len); i++)
	{
		g_autofree gchar *other_type = NULL;
		g_autofree gchar *other_label = NULL;
		VentureLinkKind kind;
		gint64 other_id = 0;

		if (venture_record_link_other_end(g_ptr_array_index(links, i), "build",
		                                  venture_entity_get_id(build),
		                                  &other_type, &other_id, &other_label,
		                                  &kind) &&
		    (VENTURE_LINK_KIND_CAUSES == kind) &&
		    (0 == g_strcmp0(other_type, "ticket")))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
			            "This build already has ticket #%" G_GINT64_FORMAT,
			            other_id);
			return NULL;
		}
	}

	repo = venture_database_get(database, VENTURE_TYPE_FORGE_REPO, repo_id,
	                            NULL);

	if (NULL != repo)
		g_object_get(repo, "default-branch", &default_branch, NULL);

	title = g_strdup_printf("Build failed: %s on %s",
	                        venture_string_is_empty(workflow) ? "CI" : workflow,
	                        venture_string_is_empty(ref) ? "an unknown branch"
	                                                     : ref);

	description = g_string_new(NULL);
	g_string_append_printf(description, "Opened from build #%" G_GINT64_FORMAT
	                       ".\n", venture_entity_get_id(build));

	if (!venture_string_is_empty(build_title))
		g_string_append_printf(description, "\nRun: %s", build_title);

	if (!venture_string_is_empty(commit))
		g_string_append_printf(description, "\nCommit: %s", commit);

	if (!venture_string_is_empty(url))
		g_string_append_printf(description, "\nLog: %s", url);

	if (!venture_string_is_empty(log_excerpt))
		g_string_append_printf(description, "\n\n%s", log_excerpt);

	ticket = venture_ticket_new();
	g_object_set(ticket,
	             "title", title,
	             "kind", VENTURE_TICKET_KIND_INTERNAL,
	             "status", VENTURE_TICKET_STATUS_TODO,
	             /* The default branch being red stops everybody; a red
	              * feature branch stops whoever is on it. */
	             "priority", (!venture_string_is_empty(ref) &&
	                          (0 == g_strcmp0(ref, default_branch)))
	                             ? VENTURE_PRIORITY_HIGH
	                             : VENTURE_PRIORITY_NORMAL,
	             "issue-type", VENTURE_ISSUE_TYPE_BUG,
	             "description", description->str,
	             "repo-id", repo_id,
	             "release-id", release_id,
	             NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(ticket),
		venture_entity_get_organization_id(build));

	if (!venture_database_save(database, VENTURE_ENTITY(ticket), actor, error))
		return NULL;

	{
		g_autoptr(VentureRecordLink) link = NULL;

		link = venture_record_link_create(database, "build",
			venture_entity_get_id(build), VENTURE_LINK_KIND_CAUSES, "ticket",
			venture_entity_get_id(VENTURE_ENTITY(ticket)), "The fix", NULL);

		if (NULL != link)
			venture_database_save(database, VENTURE_ENTITY(link), actor, NULL);
	}

	return VENTURE_ENTITY(g_steal_pointer(&ticket));
}

/* ==========================================================================
 * What needs you
 *
 * The Factory page answers "where does the factory stand". This answers
 * the question after it: "and what should I do about it". Every entry is
 * something a person or an agent can act on now, names the record to act
 * on, and is derived from the records alone -- no model is asked, so it is
 * the same list every time and costs nothing to draw. The assistant's
 * briefing is this list, read aloud.
 * ========================================================================== */

typedef struct
{
	gint	 rank;		/* 0 urgent, 1 high, 2 normal */
	guint	 order;		/* the order found, to keep the sort stable */
	gchar	*key;
	gchar	*title;
	gchar	*detail;
	gchar	*record_type;
	gint64	 record_id;
	gchar	*action;
} VentureFactoryAction;

static void
venture_factory_action_free(gpointer data)
{
	VentureFactoryAction *action = data;

	g_free(action->key);
	g_free(action->title);
	g_free(action->detail);
	g_free(action->record_type);
	g_free(action->action);
	g_free(action);
}

static gint
venture_factory_action_compare(
	gconstpointer	a,
	gconstpointer	b
){
	const VentureFactoryAction *left = *(VentureFactoryAction *const *)a;
	const VentureFactoryAction *right = *(VentureFactoryAction *const *)b;

	if (left->rank != right->rank)
		return left->rank - right->rank;

	return (left->order < right->order) ? -1 : (left->order > right->order);
}

/*
 * Adds one entry. @title and @detail are taken; the rest are copied.
 */
static void
venture_factory_action_add(
	GPtrArray	*actions,
	gint		 rank,
	const gchar	*key,
	gchar		*title,
	gchar		*detail,
	const gchar	*record_type,
	gint64		 record_id,
	const gchar	*action
){
	VentureFactoryAction *entry;

	entry = g_new0(VentureFactoryAction, 1);
	entry->rank = rank;
	entry->order = actions->len;
	entry->key = g_strdup(key);
	entry->title = title;
	entry->detail = detail;
	entry->record_type = g_strdup(record_type);
	entry->record_id = record_id;
	entry->action = g_strdup(action);
	g_ptr_array_add(actions, entry);
}

static gint64
venture_factory_days_between(
	GDateTime	*from,
	GDateTime	*until
){
	return g_date_time_difference(until, from) / G_TIME_SPAN_DAY;
}

JsonNode *
venture_factory_next_actions(
	VentureContext	 *context,
	const gint64	 *organization_ids,
	gsize		  n_organizations,
	GError		**error
){
	VentureDatabase *database;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(GPtrArray) actions = NULL;
	g_autoptr(GDateTime) now = NULL;
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
	now = venture_time_now();
	actions = g_ptr_array_new_with_free_func(venture_factory_action_free);

	/* Incidents: one still happening with nobody on the fix, and a
	 * serious one over with nothing written down about why. */
	{
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(GPtrArray) rows = NULL;
		g_autoptr(GDateTime) since = NULL;

		query = venture_query_new(VENTURE_TYPE_INCIDENT);
		venture_query_add_order(query, "started-at", VENTURE_SORT_DESCENDING,
		                        NULL);
		venture_query_set_limit(query, 200);
		venture_factory_scope(query, organization_ids, n_organizations);
		rows = venture_database_find(database, query, error);

		if (NULL == rows)
			return NULL;

		since = g_date_time_add_days(now, -30);

		for (i = 0; i < rows->len; i++)
		{
			VentureEntity *incident;
			g_autofree gchar *title = NULL;
			g_autofree gchar *postmortem = NULL;
			g_autoptr(GDateTime) resolved_at = NULL;
			VentureIncidentSeverity severity;
			VentureIncidentStatus status;
			gboolean serious;
			gint64 ticket_id = 0;

			incident = g_ptr_array_index(rows, i);
			g_object_get(incident, "title", &title, "severity", &severity,
			             "status", &status, "ticket-id", &ticket_id,
			             "postmortem", &postmortem,
			             "resolved-at", &resolved_at, NULL);
			serious = (VENTURE_INCIDENT_SEVERITY_SEV1 == severity) ||
			          (VENTURE_INCIDENT_SEVERITY_SEV2 == severity);

			if (!venture_factory_incident_is_over(status))
			{
				if (0 != ticket_id)
					continue;

				venture_factory_action_add(actions, serious ? 0 : 1,
					"incident_without_fix",
					g_strdup_printf("Open the fix for \"%s\"", title),
					g_strdup_printf("A %s incident is %s and no ticket has "
					                "been raised to fix it.",
					                venture_enum_to_nick(
					                    VENTURE_TYPE_INCIDENT_SEVERITY,
					                    (gint)severity),
					                venture_enum_to_nick(
					                    VENTURE_TYPE_INCIDENT_STATUS,
					                    (gint)status)),
					"incident", venture_entity_get_id(incident), "fix_ticket");
			}
			else if (serious && venture_string_is_empty(postmortem) &&
			         (NULL != resolved_at) &&
			         (g_date_time_compare(resolved_at, since) >= 0))
			{
				venture_factory_action_add(actions, 2, "postmortem_missing",
					g_strdup_printf("Write the postmortem for \"%s\"", title),
					g_strdup_printf("Resolved %" G_GINT64_FORMAT " day(s) ago "
					                "with nothing recorded about why it "
					                "happened or what changes.",
					                venture_factory_days_between(resolved_at,
					                                             now)),
					"incident", venture_entity_get_id(incident), "postmortem");
			}
		}
	}

	/* Builds: the default branch of a repository being red. The newest
	 * build of each branch is the one that speaks for it. */
	{
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(GPtrArray) rows = NULL;
		g_autoptr(GHashTable) seen = NULL;
		g_autoptr(GDateTime) since = NULL;

		query = venture_query_new(VENTURE_TYPE_BUILD);
		venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, NULL);
		venture_query_set_limit(query, 200);
		venture_factory_scope(query, organization_ids, n_organizations);
		rows = venture_database_find(database, query, error);

		if (NULL == rows)
			return NULL;

		seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
		since = g_date_time_add_days(now, -14);

		for (i = 0; i < rows->len; i++)
		{
			VentureEntity *build;
			g_autoptr(VentureEntity) repo = NULL;
			g_autofree gchar *ref = NULL;
			g_autofree gchar *workflow = NULL;
			g_autofree gchar *repo_name = NULL;
			g_autofree gchar *default_branch = NULL;
			g_autofree gchar *key = NULL;
			VentureBuildStatus status;
			gint64 repo_id = 0;

			build = g_ptr_array_index(rows, i);
			g_object_get(build, "ref", &ref, "workflow", &workflow,
			             "status", &status, "repo-id", &repo_id, NULL);
			key = g_strdup_printf("%" G_GINT64_FORMAT "\n%s\n%s", repo_id,
			                      (NULL != ref) ? ref : "",
			                      (NULL != workflow) ? workflow : "");

			if (!g_hash_table_add(seen, g_steal_pointer(&key)))
				continue;

			if (VENTURE_BUILD_STATUS_FAILED != status)
				continue;

			if ((NULL != venture_entity_get_created_at(build)) &&
			    (g_date_time_compare(venture_entity_get_created_at(build),
			                         since) < 0))
				continue;

			repo = venture_database_get(database, VENTURE_TYPE_FORGE_REPO,
			                            repo_id, NULL);

			if (NULL == repo)
				continue;

			g_object_get(repo, "name", &repo_name,
			             "default-branch", &default_branch, NULL);

			if (venture_string_is_empty(ref) ||
			    (0 != g_strcmp0(ref, default_branch)))
				continue;

			venture_factory_action_add(actions, 1, "default_branch_red",
				g_strdup_printf("%s is red on %s", ref, repo_name),
				g_strdup_printf("The latest %s build of the default branch "
				                "failed.",
				                venture_string_is_empty(workflow) ? "CI"
				                                                  : workflow),
				"build", venture_entity_get_id(build), "build_ticket");
		}
	}

	/* Milestones: past due, or on course to be. */
	{
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(GPtrArray) rows = NULL;

		query = venture_query_new(VENTURE_TYPE_MILESTONE);
		venture_query_add_filter_string(query, "status", VENTURE_FILTER_OP_NE,
		                                "completed", NULL);
		venture_query_add_filter_string(query, "status", VENTURE_FILTER_OP_NE,
		                                "cancelled", NULL);
		venture_query_add_order(query, "due-on", VENTURE_SORT_ASCENDING, NULL);
		venture_query_set_limit(query, 50);
		venture_factory_scope(query, organization_ids, n_organizations);
		rows = venture_database_find(database, query, error);

		if (NULL == rows)
			return NULL;

		for (i = 0; i < rows->len; i++)
		{
			VentureEntity *milestone;
			g_autoptr(JsonNode) forecast = NULL;
			g_autofree gchar *name = NULL;
			JsonObject *object;
			const gchar *state;

			milestone = g_ptr_array_index(rows, i);
			forecast = venture_factory_milestone_forecast(context, milestone,
			                                              NULL);

			if (NULL == forecast)
				continue;

			object = json_node_get_object(forecast);
			state = json_object_get_string_member(object, "state");
			g_object_get(milestone, "name", &name, NULL);

			if (0 == g_strcmp0(state, "overdue"))
			{
				venture_factory_action_add(actions, 1, "milestone_overdue",
					g_strdup_printf("Milestone \"%s\" is overdue", name),
					g_strdup_printf("%" G_GINT64_FORMAT " day(s) past due "
					                "with %" G_GINT64_FORMAT " of %"
					                G_GINT64_FORMAT " tickets left.",
					                json_object_get_int_member(object, "days_over"),
					                json_object_get_int_member(object, "remaining"),
					                json_object_get_int_member(object, "tickets")),
					"milestone", venture_entity_get_id(milestone), "replan");
			}
			else if (0 == g_strcmp0(state, "at_risk"))
			{
				venture_factory_action_add(actions, 2, "milestone_at_risk",
					g_strdup_printf("Milestone \"%s\" will miss its date",
					                name),
					g_strdup_printf("At %.1f tickets a week the remaining %"
					                G_GINT64_FORMAT " land %" G_GINT64_FORMAT
					                " day(s) late.",
					                json_object_get_double_member(object,
					                                              "per_week"),
					                json_object_get_int_member(object, "remaining"),
					                json_object_get_int_member(object, "days_over")),
					"milestone", venture_entity_get_id(milestone), "replan");
			}
		}
	}

	/* Releases: one with everything done that has not gone out, and one
	 * that went out and never reached production. */
	{
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(VentureQuery) env_query = NULL;
		g_autoptr(GPtrArray) rows = NULL;
		g_autoptr(GPtrArray) environments = NULL;
		g_autoptr(GHashTable) production = NULL;
		g_autoptr(GDateTime) since = NULL;

		env_query = venture_query_new(VENTURE_TYPE_ENVIRONMENT);
		venture_query_add_filter_string(env_query, "kind", VENTURE_FILTER_OP_EQ,
		                                "production", NULL);
		venture_query_set_limit(env_query, 0);
		venture_factory_scope(env_query, organization_ids, n_organizations);
		environments = venture_database_find(database, env_query, error);

		if (NULL == environments)
			return NULL;

		production = g_hash_table_new(g_direct_hash, g_direct_equal);

		for (i = 0; i < environments->len; i++)
			g_hash_table_add(production, GINT_TO_POINTER((gint)
				venture_entity_get_id(g_ptr_array_index(environments, i))));

		query = venture_query_new(VENTURE_TYPE_RELEASE);
		venture_query_add_filter_string(query, "status", VENTURE_FILTER_OP_NE,
		                                "yanked", NULL);
		venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, NULL);
		venture_query_set_limit(query, 50);
		venture_factory_scope(query, organization_ids, n_organizations);
		rows = venture_database_find(database, query, error);

		if (NULL == rows)
			return NULL;

		since = g_date_time_add_days(now, -30);

		for (i = 0; i < rows->len; i++)
		{
			VentureEntity *release;
			g_autofree gchar *number = NULL;
			g_autoptr(GDateTime) released_at = NULL;
			VentureReleaseStatus status;

			release = g_ptr_array_index(rows, i);
			g_object_get(release, "number", &number, "status", &status,
			             "released-at", &released_at, NULL);

			if (VENTURE_RELEASE_STATUS_RELEASED != status)
			{
				g_autoptr(JsonNode) readiness = NULL;
				JsonObject *object;
				JsonArray *checks;
				gboolean tickets_done = FALSE;
				guint c;

				readiness = venture_factory_release_readiness(context, release,
				                                              NULL);

				if (NULL == readiness)
					continue;

				object = json_node_get_object(readiness);
				checks = json_object_get_array_member(object, "checks");

				for (c = 0; c < json_array_get_length(checks); c++)
				{
					JsonObject *check;

					check = json_array_get_object_element(checks, c);

					if ((0 == g_strcmp0("tickets",
					         json_object_get_string_member(check, "key"))) &&
					    (0 == g_strcmp0("pass",
					         json_object_get_string_member(check, "state"))))
						tickets_done = TRUE;
				}

				/* Ready with nothing in it is a plan, not a release
				 * waiting to go. */
				if (tickets_done &&
				    json_object_get_boolean_member(object, "ready"))
					venture_factory_action_add(actions, 2, "release_ready",
						g_strdup_printf("Release %s is ready to go out", number),
						g_strdup("Every ticket is done and nothing blocks "
						         "it."),
						"release", venture_entity_get_id(release), "publish");

				continue;
			}

			if ((0 == g_hash_table_size(production)) ||
			    (NULL == released_at) ||
			    (g_date_time_compare(released_at, since) < 0))
				continue;

			{
				g_autoptr(VentureQuery) deploy_query = NULL;
				g_autoptr(GPtrArray) deployments = NULL;
				gboolean reached = FALSE;
				guint d;

				deploy_query = venture_query_new(VENTURE_TYPE_DEPLOYMENT);
				venture_query_add_filter_int(deploy_query, "release-id",
				                             VENTURE_FILTER_OP_EQ,
				                             venture_entity_get_id(release),
				                             NULL);
				venture_query_set_limit(deploy_query, 0);
				deployments = venture_database_find(database, deploy_query,
				                                    NULL);

				for (d = 0; (NULL != deployments) && (d < deployments->len); d++)
				{
					VentureDeploymentStatus deploy_status;
					gint64 environment_id = 0;

					g_object_get(g_ptr_array_index(deployments, d),
					             "status", &deploy_status,
					             "environment-id", &environment_id, NULL);

					/* On its way counts: nobody needs telling to do
					 * what is already being done. */
					if ((VENTURE_DEPLOYMENT_STATUS_FAILED != deploy_status) &&
					    g_hash_table_contains(production,
					        GINT_TO_POINTER((gint)environment_id)))
						reached = TRUE;
				}

				if (!reached)
					venture_factory_action_add(actions, 2,
						"release_not_in_production",
						g_strdup_printf("Release %s is not in production",
						                number),
						g_strdup_printf("Released %" G_GINT64_FORMAT " day(s) "
						                "ago and never deployed to a "
						                "production environment.",
						                venture_factory_days_between(released_at,
						                                             now)),
						"release", venture_entity_get_id(release), "deploy");
			}
		}
	}

	/* Deployments nobody finished recording. */
	{
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(GPtrArray) rows = NULL;
		g_autoptr(GDateTime) since = NULL;

		query = venture_query_new(VENTURE_TYPE_DEPLOYMENT);
		venture_query_add_filter_string(query, "status", VENTURE_FILTER_OP_NE,
		                                "succeeded", NULL);
		venture_query_add_filter_string(query, "status", VENTURE_FILTER_OP_NE,
		                                "failed", NULL);
		venture_query_add_filter_string(query, "status", VENTURE_FILTER_OP_NE,
		                                "rolled_back", NULL);
		venture_query_set_limit(query, 50);
		venture_factory_scope(query, organization_ids, n_organizations);
		rows = venture_database_find(database, query, error);

		if (NULL == rows)
			return NULL;

		since = g_date_time_add_days(now, -1);

		for (i = 0; i < rows->len; i++)
		{
			VentureEntity *deployment;
			g_autofree gchar *label = NULL;
			GDateTime *created;

			deployment = g_ptr_array_index(rows, i);
			created = venture_entity_get_created_at(deployment);

			if ((NULL == created) || (g_date_time_compare(created, since) >= 0))
				continue;

			label = venture_entity_get_display_name(deployment);
			venture_factory_action_add(actions, 2, "deployment_stuck",
				g_strdup_printf("Deployment #%" G_GINT64_FORMAT " never "
				                "finished", venture_entity_get_id(deployment)),
				g_strdup_printf("%s has been pending or in progress for %"
				                G_GINT64_FORMAT " day(s). Say whether it "
				                "succeeded.", label,
				                venture_factory_days_between(created, now)),
				"deployment", venture_entity_get_id(deployment), "update");
		}
	}

	/* What the agents may spend. */
	if (venture_context_module_enabled(context, "forge"))
	{
		g_autoptr(JsonNode) budgets = NULL;
		JsonArray *array;

		budgets = venture_factory_budgets_describe(context, NULL);
		array = (NULL != budgets) ? json_node_get_array(budgets) : NULL;

		for (i = 0; (NULL != array) && (i < json_array_get_length(array)); i++)
		{
			JsonObject *budget;
			gboolean exhausted;

			budget = json_array_get_object_element(array, i);
			exhausted = json_object_get_boolean_member(budget, "exhausted");

			if (!exhausted && !json_object_get_boolean_member(budget, "warning"))
				continue;

			venture_factory_action_add(actions, exhausted ? 1 : 2,
				exhausted ? "budget_exhausted" : "budget_warning",
				g_strdup_printf("Agent budget \"%s\" is %s",
				                json_object_get_string_member(budget, "label"),
				                exhausted ? "exhausted" : "running low"),
				g_strdup_printf("%s spent of %s (%" G_GINT64_FORMAT "%%)%s.",
				                json_object_get_string_member(budget,
				                                              "spent_display"),
				                json_object_get_string_member(budget,
				                                              "limit_display"),
				                json_object_get_int_member(budget, "percent"),
				                (exhausted &&
				                 json_object_get_boolean_member(budget,
				                                                "hard_stop"))
				                    ? "; new runs are refused" : ""),
				"agent_budget", json_object_get_int_member(budget, "id"),
				"update");
		}
	}

	g_ptr_array_sort(actions, venture_factory_action_compare);

	builder = json_builder_new();
	json_builder_begin_array(builder);

	for (i = 0; i < actions->len; i++)
	{
		static const gchar *const ranks[] = { "urgent", "high", "normal" };
		VentureFactoryAction *entry;
		g_autofree gchar *href = NULL;

		entry = g_ptr_array_index(actions, i);
		href = g_strdup_printf("/e/%s/%" G_GINT64_FORMAT, entry->record_type,
		                       entry->record_id);

		json_builder_begin_object(builder);
		venture_factory_add_string(builder, "key", entry->key);
		venture_factory_add_string(builder, "priority", ranks[entry->rank]);
		venture_factory_add_string(builder, "title", entry->title);
		venture_factory_add_string(builder, "detail", entry->detail);
		venture_factory_add_string(builder, "record_type", entry->record_type);
		json_builder_set_member_name(builder, "record_id");
		json_builder_add_int_value(builder, entry->record_id);
		venture_factory_add_string(builder, "href", href);
		venture_factory_add_string(builder, "action", entry->action);
		json_builder_end_object(builder);
	}

	json_builder_end_array(builder);

	return json_builder_get_root(builder);
}
