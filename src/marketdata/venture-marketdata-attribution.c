/*
 * venture-marketdata-attribution.c - Naming where the data shown came from
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Some providers' terms make naming them a condition of using their data:
 * Blizzard's API terms (section 2.13) require the application to identify
 * Blizzard as the source "clearly and conspicuously", in a way that does
 * not suggest Blizzard endorses it. A provider says so by declaring an
 * attribution line (venture_data_source_provider_get_attribution()), and
 * every answer that shows data carries the distinct lines of the sources
 * it actually drew on, in an `attribution` array.
 *
 * The answer carries them, not the page, for the reason every market
 * answer is one JSON object: the page, its /api/v1 twin and a dashboard
 * card read the same answer, so none of them can show a source's data
 * without its line. The line is looked up at answer time from the
 * provider registry, so it is whatever the loaded provider says today.
 *
 * Built with or without SQLite: without it there is no feeds service, no
 * provider registry and no store data to attribute, and every collector
 * here adds nothing.
 */

#include "venture.h"

#include <string.h>

/* How deep a walk for data_source_id members goes. Every answer here is a
 * few levels deep; the bound is only there so a walk always ends. */
#define MD_ATTRIBUTION_MAX_DEPTH (16)

#ifdef VENTURE_HAVE_SQLITE

static void
md_attribution_add_line(
	GPtrArray	*attributions,
	const gchar	*text
){
	g_autofree gchar *line = NULL;
	guint i;

	/* Normalised here, because a native provider's vfunc was judged by
	 * nobody when it registered. */
	line = venture_data_source_attribution_normalise(text);

	if (NULL == line)
		return;

	/* Distinct, in first-seen order: a page of a hundred Blizzard rows
	 * says so once. */
	for (i = 0; i < attributions->len; i++)
	{
		if (0 == g_strcmp0(g_ptr_array_index(attributions, i), line))
			return;
	}

	g_ptr_array_add(attributions, g_steal_pointer(&line));
}

#endif /* VENTURE_HAVE_SQLITE */

void
venture_marketdata_attribution_add_provider(
	VentureContext	*context,
	const gchar	*provider,
	GPtrArray	*attributions
){
	g_return_if_fail(VENTURE_IS_CONTEXT(context));
	g_return_if_fail(NULL != attributions);

#ifdef VENTURE_HAVE_SQLITE
	{
		VentureDataSourceProvider *found;

		if (venture_string_is_empty(provider))
			return;

		found = venture_data_source_provider_registry_lookup(
			venture_context_get_data_source_providers(context), provider);

		if (NULL != found)
			md_attribution_add_line(attributions,
			                        venture_data_source_provider_get_attribution(found));
	}
#else
	(void)provider;
#endif
}

void
venture_marketdata_attribution_add_source(
	VentureContext	*context,
	gint64		 organization_id,
	gint64		 data_source_id,
	GPtrArray	*attributions
){
	g_autoptr(VentureEntity) source = NULL;
	g_autofree gchar *provider = NULL;

	g_return_if_fail(VENTURE_IS_CONTEXT(context));
	g_return_if_fail(NULL != attributions);

	if (data_source_id <= 0)
		return;

	/*
	 * A deleted source still counts: an alert hit or a planned trade it
	 * fed is still on the page, and so is its data. Another
	 * organization's never does -- the answer was scoped already, and
	 * this must not be a way to learn another organization's providers.
	 */
	source = venture_database_get(venture_context_get_database(context),
	                              VENTURE_TYPE_DATA_SOURCE, data_source_id, NULL);

	if ((NULL == source) || (venture_entity_get_organization_id(source) != organization_id))
		return;

	g_object_get(source, "provider", &provider, NULL);
	venture_marketdata_attribution_add_provider(context, provider, attributions);
}

static void
md_attribution_walk(
	VentureContext	*context,
	gint64		 organization_id,
	JsonNode	*node,
	GHashTable	*seen,
	GPtrArray	*attributions,
	guint		 depth
){
	if ((NULL == node) || (depth > MD_ATTRIBUTION_MAX_DEPTH))
		return;

	if (JSON_NODE_HOLDS_ARRAY(node))
	{
		JsonArray *array = json_node_get_array(node);
		guint i;

		for (i = 0; i < json_array_get_length(array); i++)
			md_attribution_walk(context, organization_id, json_array_get_element(array, i),
			                    seen, attributions, depth + 1);
	}
	else if (JSON_NODE_HOLDS_OBJECT(node))
	{
		JsonObject *object = json_node_get_object(node);
		JsonObjectIter iter;
		const gchar *name;
		JsonNode *member;

		json_object_iter_init(&iter, object);

		while (json_object_iter_next(&iter, &name, &member))
		{
			if ((0 == g_strcmp0(name, "data_source_id")) && JSON_NODE_HOLDS_VALUE(member) &&
			    (G_TYPE_INT64 == json_node_get_value_type(member)))
			{
				gint64 id = json_node_get_int(member);

				/* One lookup per source, however many rows name it. */
				if ((id > 0) && !g_hash_table_contains(seen, &id))
				{
					gint64 *key = g_new(gint64, 1);

					*key = id;
					g_hash_table_add(seen, key);
					venture_marketdata_attribution_add_source(context, organization_id, id,
					                                          attributions);
				}
			}
			else
				md_attribution_walk(context, organization_id, member, seen, attributions,
				                    depth + 1);
		}
	}
}

void
venture_marketdata_attribution_collect(
	VentureContext	*context,
	gint64		 organization_id,
	JsonNode	*node,
	GPtrArray	*attributions
){
	g_autoptr(GHashTable) seen = NULL;

	g_return_if_fail(VENTURE_IS_CONTEXT(context));
	g_return_if_fail(NULL != attributions);

	seen = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	md_attribution_walk(context, organization_id, node, seen, attributions, 0);
}

void
venture_marketdata_attribution_set(
	JsonObject	*answer,
	GPtrArray	*attributions
){
	JsonArray *array;
	guint i;

	g_return_if_fail(NULL != answer);

	array = json_array_new();

	for (i = 0; (NULL != attributions) && (i < attributions->len); i++)
		json_array_add_string_element(array, g_ptr_array_index(attributions, i));

	json_object_set_array_member(answer, "attribution", array);
}

gchar **
venture_marketdata_attribution_dup(JsonObject *answer)
{
	g_autoptr(GPtrArray) lines = NULL;
	JsonNode *node;
	guint i;

	lines = g_ptr_array_new_with_free_func(g_free);
	node = (NULL != answer) ? json_object_get_member(answer, "attribution") : NULL;

	if ((NULL != node) && JSON_NODE_HOLDS_ARRAY(node))
	{
		JsonArray *array = json_node_get_array(node);

		for (i = 0; i < json_array_get_length(array); i++)
		{
			JsonNode *element = json_array_get_element(array, i);

			if (JSON_NODE_HOLDS_VALUE(element) &&
			    (G_TYPE_STRING == json_node_get_value_type(element)))
				g_ptr_array_add(lines, g_strdup(json_node_get_string(element)));
		}
	}

	g_ptr_array_add(lines, NULL);

	return (gchar **)g_ptr_array_free(g_steal_pointer(&lines), FALSE);
}

void
venture_marketdata_attribution_append_html(
	GString			*html,
	const gchar *const	*attributions
){
	guint i;

	g_return_if_fail(NULL != html);

	/*
	 * A visible sentence, not a tooltip or a footer link: "clearly and
	 * conspicuously" is the bar. Escaped like every other string a
	 * plugin hands over -- the line is text, never markup -- and a role
	 * of note so a screen reader announces it with the data it names.
	 */
	for (i = 0; (NULL != attributions) && (NULL != attributions[i]); i++)
	{
		g_string_append(html, "<p class=\"source-attribution\" role=\"note\">"
		                      "<span class=\"source-attribution-label\">Source:</span> ");
		venture_html_escape_append(html, attributions[i]);
		g_string_append(html, "</p>");
	}
}

void
venture_marketdata_attribution_append_answer_html(
	GString		*html,
	JsonObject	*answer
){
	g_auto(GStrv) lines = NULL;

	g_return_if_fail(NULL != html);

	lines = venture_marketdata_attribution_dup(answer);
	venture_marketdata_attribution_append_html(html, (const gchar *const *)lines);
}
