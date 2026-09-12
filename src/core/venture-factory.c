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
	guint i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), FALSE);

	now = venture_time_now();
	budgets = venture_factory_active_budgets(context);

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
	total_cost = venture_money_sum(costs, NULL, NULL);
	success_cost = venture_money_sum(success_costs, NULL, NULL);

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

	json_builder_set_member_name(builder, "cost_per_success");

	if ((NULL != success_cost) && (succeeded > 0))
	{
		g_autoptr(VentureMoney) each = NULL;
		g_autoptr(GPtrArray) shares = NULL;

		/* Allocated rather than divided, so the shares add back up. */
		shares = venture_money_allocate_evenly(success_cost, (guint)succeeded,
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
