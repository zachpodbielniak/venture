/*
 * venture-external-runs.c - Coding work done somewhere else, on the record
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

#include <string.h>

/* A member as text, whatever JSON type it arrived as; NULL when absent. */
static gchar *
member_text(JsonObject *fields, const gchar *name)
{
	JsonNode *node;

	if (NULL == fields || !json_object_has_member(fields, name))
		return NULL;

	node = json_object_get_member(fields, name);

	if (JSON_NODE_HOLDS_NULL(node))
		return g_strdup("");
	if (json_node_get_value_type(node) == G_TYPE_STRING)
		return g_strdup(json_node_get_string(node));
	if (json_node_get_value_type(node) == G_TYPE_INT64)
		return g_strdup_printf("%" G_GINT64_FORMAT, json_node_get_int(node));
	if (json_node_get_value_type(node) == G_TYPE_DOUBLE)
	{
		gchar buffer[G_ASCII_DTOSTR_BUF_SIZE];

		return g_strdup(g_ascii_dtostr(buffer, sizeof buffer,
		                               json_node_get_double(node)));
	}
	if (json_node_get_value_type(node) == G_TYPE_BOOLEAN)
		return g_strdup(json_node_get_boolean(node) ? "true" : "false");

	return NULL;
}

/*
 * Copies the named members of @fields onto @run, through the same parser
 * a form or the CLI uses, so an integer field refuses "lots" and a JSON
 * member name (pull_request_number) maps to its property.
 */
static gboolean
apply_fields(VentureEntity        *run,
             JsonObject           *fields,
             const gchar * const  *names,
             GError              **error)
{
	guint i;

	for (i = 0; NULL != names[i]; i++)
	{
		g_autofree gchar *text = member_text(fields, names[i]);
		g_autofree gchar *property = NULL;

		if (NULL == text)
			continue;

		property = g_strdelimit(g_strdup(names[i]), "_", '-');

		if (!venture_entity_set_field_from_string(run, property, text, error))
			return FALSE;
	}

	return TRUE;
}

/* The repository a ticket is against, for the budgets; 0 for none. */
static gint64
repository_of(VentureEntity *ticket)
{
	gint64 repo_id = 0;

	g_object_get(ticket, "repo-id", &repo_id, NULL);

	return repo_id;
}

VentureEntity *
venture_external_run_start(VentureContext      *context,
                           VentureEntity       *ticket,
                           JsonObject          *fields,
                           const VentureActor  *actor,
                           GError             **error)
{
	static const gchar *const names[] = {
		"provider", "model", "branch", "prompt", NULL
	};
	g_autoptr(VentureEntity) run = NULL;
	g_autoptr(GDateTime) now = venture_time_now();

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	g_return_val_if_fail(VENTURE_IS_ENTITY(ticket), NULL);

	/*
	 * The budget first, as for VENTURE's own runners: work refused for
	 * money is refused whatever else is true, and the refusal names the
	 * budget. Asked of the ticket's repository, or of the budgets that
	 * cover every repository when it names none.
	 */
	if (!venture_factory_budget_allows_run(context, repository_of(ticket),
	                                       error))
		return NULL;

	run = g_object_new(VENTURE_TYPE_FORGE_RUN,
		"ticket-id", venture_entity_get_id(ticket),
		"state", VENTURE_FORGE_RUN_STATE_RUNNING,
		"runner", VENTURE_FORGE_RUNNER_EXTERNAL,
		"started-at", now, NULL);
	venture_entity_set_organization_id(run,
		venture_entity_get_organization_id(ticket));

	if (!apply_fields(run, fields, names, error))
		return NULL;

	if (!venture_database_save(venture_context_get_database(context), run,
	                           actor, error))
		return NULL;

	return g_steal_pointer(&run);
}

gboolean
venture_external_run_report(VentureContext      *context,
                            VentureEntity       *run,
                            JsonObject          *fields,
                            const VentureActor  *actor,
                            GError             **error)
{
	static const gchar *const names[] = {
		"branch", "pull_request_number", "provider", "model",
		"input_tokens", "output_tokens", "turns", "summary",
		"failure_reason", "log", NULL
	};
	g_autoptr(GDateTime) finished = NULL;
	g_autofree gchar *state_text = NULL;
	g_autofree gchar *cost_text = NULL;
	VentureForgeRunner runner = VENTURE_FORGE_RUNNER_AGENT;
	VentureDatabase *database;
	gint64 ticket_id = 0;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), FALSE);
	g_return_val_if_fail(VENTURE_IS_ENTITY(run), FALSE);

	g_object_get(run, "runner", &runner, "finished-at", &finished,
	             "ticket-id", &ticket_id, NULL);

	/* VENTURE's own runs are written by its runners alone. */
	if (VENTURE_FORGE_RUNNER_EXTERNAL != runner)
	{
		g_set_error_literal(error, VENTURE_ERROR,
		                    VENTURE_ERROR_PERMISSION_DENIED,
		                    "Only an external run is reported here; this "
		                    "run belongs to one of VENTURE's own runners");
		return FALSE;
	}

	/* A finished run is evidence: it takes no more reports. */
	if (NULL != finished)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
		                    "That run has finished; start a new one for "
		                    "more work");
		return FALSE;
	}

	if (!apply_fields(run, fields, names, error))
		return FALSE;

	/*
	 * The cost with its currency, always: a bare number would be read in
	 * whatever currency the install happens to default to, and a budget
	 * compared in the wrong one refuses nothing.
	 */
	cost_text = member_text(fields, "cost");
	if (NULL != cost_text)
	{
		g_autoptr(VentureMoney) cost = NULL;
		gboolean named = FALSE;
		const gchar *c;

		/* A currency code in the text, not left to a default. */
		for (c = cost_text; '\0' != *c; c++)
			named = named || g_ascii_isalpha(*c);

		if (!named)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			            "cost: give the currency with the amount, e.g. "
			            "\"%s USD\"", cost_text);
			return FALSE;
		}

		cost = venture_money_from_string(cost_text, NULL, error);

		if (NULL == cost)
			return FALSE;
		g_object_set(run, "cost", cost, NULL);
	}

	state_text = member_text(fields, "state");
	if (NULL != state_text)
	{
		static const struct {
			const gchar *nick;
			VentureForgeRunState state;
			gboolean ends;
		} states[] = {
			{ "running",   VENTURE_FORGE_RUN_STATE_RUNNING,   FALSE },
			{ "succeeded", VENTURE_FORGE_RUN_STATE_SUCCEEDED, TRUE },
			{ "failed",    VENTURE_FORGE_RUN_STATE_FAILED,    TRUE },
			{ "cancelled", VENTURE_FORGE_RUN_STATE_CANCELLED, TRUE },
		};
		gboolean known = FALSE;
		guint i;

		for (i = 0; i < G_N_ELEMENTS(states); i++)
		{
			if (0 != g_strcmp0(state_text, states[i].nick))
				continue;

			known = TRUE;
			g_object_set(run, "state", states[i].state, NULL);

			if (states[i].ends)
			{
				g_autoptr(GDateTime) now = venture_time_now();

				g_object_set(run, "finished-at", now, NULL);
			}
		}

		if (!known)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			            "state: running, succeeded, failed or cancelled, "
			            "not \"%s\"", state_text);
			return FALSE;
		}
	}

	database = venture_context_get_database(context);

	if (!venture_database_save(database, run, actor, error))
		return FALSE;

	/*
	 * Asked again once it has cost something, so a line this run crossed
	 * is announced now, the once, as for an internal run. Its answer for
	 * the next run is not this run's business.
	 */
	if (NULL != cost_text)
	{
		g_autoptr(VentureEntity) ticket = venture_database_get(database,
			VENTURE_TYPE_TICKET, ticket_id, NULL);

		if (NULL != ticket)
			venture_factory_budget_allows_run(context, repository_of(ticket),
			                                  NULL);
	}

	return TRUE;
}
