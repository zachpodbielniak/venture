/*
 * venture-routing.c - Who a new ticket goes to
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

/*
 * The usernames a rule names, in rota order, with the blanks dropped.
 */
static GStrv
venture_routing_assignees(VentureEntity *rule)
{
	g_autoptr(GPtrArray) names = NULL;
	g_auto(GStrv) parts = NULL;
	g_autofree gchar *assignees = NULL;
	gsize i;

	g_object_get(rule, "assignees", &assignees, NULL);
	names = g_ptr_array_new_with_free_func(g_free);
	parts = g_strsplit((NULL != assignees) ? assignees : "", ",", -1);

	for (i = 0; NULL != parts[i]; i++)
	{
		g_autofree gchar *name = NULL;

		name = g_strstrip(g_strdup(parts[i]));

		if (!venture_string_is_empty(name))
			g_ptr_array_add(names, g_steal_pointer(&name));
	}

	g_ptr_array_add(names, NULL);

	return (GStrv)g_ptr_array_free(g_steal_pointer(&names), FALSE);
}

/*
 * Whether a comma-separated tag list carries one tag, ignoring case and
 * the spaces people leave after commas.
 */
static gboolean
venture_routing_has_tag(
	const gchar	*tags,
	const gchar	*wanted
){
	g_auto(GStrv) parts = NULL;
	gsize i;

	if (venture_string_is_empty(wanted))
		return TRUE;

	if (venture_string_is_empty(tags))
		return FALSE;

	parts = g_strsplit(tags, ",", -1);

	for (i = 0; NULL != parts[i]; i++)
	{
		g_autofree gchar *tag = NULL;

		tag = g_strstrip(g_strdup(parts[i]));

		if (0 == g_ascii_strcasecmp(tag, wanted))
			return TRUE;
	}

	return FALSE;
}

VentureEntity *
venture_routing_find_rule(
	VentureContext		*context,
	VentureTicketKind	 kind,
	VenturePriority		 priority,
	const gchar		*tags
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rules = NULL;
	VentureEntity *best;
	gint best_score;
	guint i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	if (!venture_context_module_enabled(context, "tickets"))
		return NULL;

	query = venture_query_new(VENTURE_TYPE_ROUTING_RULE);
	venture_query_add_filter_string(query, "active", VENTURE_FILTER_OP_EQ,
	                                "true", NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	venture_query_set_limit(query, 0);
	rules = venture_database_find(venture_context_get_database(context), query,
	                              NULL);

	if (NULL == rules)
		return NULL;

	best = NULL;
	best_score = -1;

	for (i = 0; i < rules->len; i++)
	{
		VentureEntity *rule;
		g_autofree gchar *tag = NULL;
		VentureTicketKind rule_kind;
		VenturePriority rule_priority;
		gboolean all_kinds = FALSE;
		gboolean all_priorities = FALSE;
		gint score;

		rule = g_ptr_array_index(rules, i);
		g_object_get(rule, "kind", &rule_kind, "all-kinds", &all_kinds,
		             "priority", &rule_priority,
		             "all-priorities", &all_priorities, "tag", &tag, NULL);

		if (!all_kinds && (rule_kind != kind))
			continue;

		if (!all_priorities && (rule_priority != priority))
			continue;

		if (!venture_routing_has_tag(tags, tag))
			continue;

		/*
		 * A tag is the narrowest thing a rule can name -- it is the one
		 * the operator wrote by hand -- so it is worth two, and kind and
		 * priority one each. A catch-all scores nothing and only wins
		 * when nothing else matched.
		 */
		score = (venture_string_is_empty(tag) ? 0 : 2)
		        + (all_kinds ? 0 : 1) + (all_priorities ? 0 : 1);

		if (score > best_score)
		{
			best = rule;
			best_score = score;
		}
	}

	return (NULL != best) ? g_object_ref(best) : NULL;
}

/*
 * How many tickets a person is holding that are neither done nor
 * cancelled. The figure behind "least busy", counted rather than
 * cached: a cached count is one more thing to be wrong when somebody
 * closes a ticket in the database directly.
 */
static gint64
venture_routing_open_count(
	VentureContext	*context,
	const gchar	*username
){
	g_autoptr(VentureQuery) query = NULL;
	gint64 count;

	query = venture_query_new(VENTURE_TYPE_TICKET);

	if (!venture_query_add_filter_string(query, "assignee",
	                                     VENTURE_FILTER_OP_EQ, username,
	                                     NULL) ||
	    !venture_query_add_filter_string(query, "status", VENTURE_FILTER_OP_NE,
	                                     "done", NULL) ||
	    !venture_query_add_filter_string(query, "status", VENTURE_FILTER_OP_NE,
	                                     "cancelled", NULL))
		return G_MAXINT64;

	count = venture_database_count(venture_context_get_database(context),
	                               query, NULL);

	return (count < 0) ? G_MAXINT64 : count;
}

gchar *
venture_routing_choose(
	VentureContext	 *context,
	VentureEntity	 *rule,
	GError		**error
){
	g_auto(GStrv) names = NULL;
	VentureRoutingStrategy strategy;
	guint n;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	g_return_val_if_fail(VENTURE_IS_ROUTING_RULE(rule), NULL);

	names = venture_routing_assignees(rule);
	n = g_strv_length(names);

	if (0 == n)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "That routing rule names nobody to assign to");
		return NULL;
	}

	g_object_get(rule, "strategy", &strategy, NULL);

	if (VENTURE_ROUTING_STRATEGY_FIRST == strategy)
		return g_strdup(names[0]);

	if (VENTURE_ROUTING_STRATEGY_LEAST_BUSY == strategy)
	{
		const gchar *chosen;
		gint64 fewest;
		guint i;

		chosen = names[0];
		fewest = venture_routing_open_count(context, names[0]);

		for (i = 1; i < n; i++)
		{
			gint64 count;

			count = venture_routing_open_count(context, names[i]);

			/* Strictly fewer, so a tie keeps the earlier name and the
			 * rota order still means something. */
			if (count < fewest)
			{
				fewest = count;
				chosen = names[i];
			}
		}

		return g_strdup(chosen);
	}

	/*
	 * A rota. The cursor is stored on the rule and advanced here rather
	 * than derived from "who had the last one": derived would hand two
	 * tickets raised in the same second to the same person, and would
	 * skip somebody whose ticket was reassigned afterwards.
	 */
	{
		gint64 cursor = 0;
		guint index;

		g_object_get(rule, "cursor", &cursor, NULL);

		if (cursor < 0)
			cursor = 0;

		index = (guint)(cursor % n);
		g_object_set(rule, "cursor", (gint64)((index + 1) % n), NULL);

		/* Saved as the system: advancing a rota is bookkeeping, not an
		 * edit somebody made. Failure to save means the next ticket
		 * goes to the same person, which is a fair sight better than
		 * refusing the ticket. */
		venture_database_save(venture_context_get_database(context), rule, NULL,
		                      NULL);

		return g_strdup(names[index]);
	}
}

/*
 * A new ticket with nobody on it gets a name, if a rule covers it. A
 * validator rather than a signal handler because it has to happen
 * before the row is written -- assigning afterwards would be a second
 * save, a second audit entry, and a notification about a change nobody
 * made.
 */
static gboolean
venture_routing_validate_ticket(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	gpointer	  user_data,
	GError		**error
){
	VentureContext *context;
	g_autoptr(VentureEntity) rule = NULL;
	g_autofree gchar *assignee = NULL;
	g_autofree gchar *tags = NULL;
	g_autofree gchar *chosen = NULL;
	VentureTicketKind kind;
	VenturePriority priority;

	(void)database;
	(void)error;
	context = user_data;

	/* Only on the way in, and only when nobody was named. */
	if (NULL != previous)
		return TRUE;

	g_object_get(entity, "assignee", &assignee, "kind", &kind,
	             "priority", &priority, "tags", &tags, NULL);

	if (!venture_string_is_empty(assignee))
		return TRUE;

	rule = venture_routing_find_rule(context, kind, priority, tags);

	if (NULL == rule)
		return TRUE;

	chosen = venture_routing_choose(context, rule, NULL);

	if (!venture_string_is_empty(chosen))
		g_object_set(entity, "assignee", chosen, NULL);

	return TRUE;
}

void
venture_routing_install(VentureContext *context)
{
	g_return_if_fail(VENTURE_IS_CONTEXT(context));

	venture_database_add_save_validator(venture_context_get_database(context),
	                                    VENTURE_TYPE_TICKET,
	                                    venture_routing_validate_ticket,
	                                    context, NULL);
}
