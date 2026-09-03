/*
 * venture-kb-crossref.c - Connecting knowledge to the records it bears on
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

#include <string.h>

gboolean
venture_kb_crossref_type_is_eligible(GType entity_type)
{
	g_autoptr(GObject) probe = NULL;
	g_autofree GParamSpec **properties = NULL;
	VentureEntityClass *klass;
	guint n_properties = 0;
	guint i;

	if (G_TYPE_INVALID == entity_type)
		return FALSE;

	/*
	 * The knowledge-base types are excluded. Cross-referencing an article
	 * against the corpus it belongs to finds itself, and linking chunks to
	 * chunks is noise with a score.
	 */
	if ((VENTURE_TYPE_KNOWLEDGE_BASE == entity_type) ||
	    (VENTURE_TYPE_KB_ARTICLE == entity_type) ||
	    (VENTURE_TYPE_KB_CHUNK == entity_type) ||
	    (VENTURE_TYPE_KB_LINK == entity_type))
		return FALSE;

	/*
	 * Users and tokens carry credentials and nothing worth matching, and
	 * the audit log is a record of changes rather than a thing knowledge
	 * bears on.
	 */
	if ((VENTURE_TYPE_USER == entity_type) ||
	    (VENTURE_TYPE_API_TOKEN == entity_type) ||
	    (VENTURE_TYPE_AUDIT_ENTRY == entity_type) ||
	    (VENTURE_TYPE_CHAT_MESSAGE == entity_type) ||
	    (VENTURE_TYPE_CHAT_THREAD == entity_type))
		return FALSE;

	probe = g_object_new(entity_type, NULL);

	if (!VENTURE_IS_ENTITY(probe))
		return FALSE;

	klass = VENTURE_ENTITY_GET_CLASS(probe);
	properties = g_object_class_list_properties(G_OBJECT_GET_CLASS(probe),
	                                            &n_properties);

	/*
	 * A long text field is the test. It is the same declaration the web
	 * form reads to choose a textarea over a one-line box, so this
	 * tracks what the type actually holds rather than a list that has to
	 * be remembered. A type with only short strings -- a tax category, a
	 * ledger line -- has nothing to compare.
	 */
	for (i = 0; i < n_properties; i++)
	{
		VentureFieldKind kind;

		if (!venture_entity_class_get_field_kind(klass,
		                                         properties[i]->name,
		                                         &kind))
			continue;

		if (VENTURE_FIELD_KIND_TEXT == kind)
			return TRUE;
	}

	return FALSE;
}

/*
 * The text of a record worth comparing against the corpus.
 *
 * Its display name, then every long text field. Short strings are left out
 * deliberately: a record's tags and its assignee say what it is filed under
 * rather than what it is about, and including them pulls every record with
 * the same assignee towards the same passages.
 */
static gchar *
venture_kb_crossref_text_of(VentureEntity *record)
{
	g_autoptr(GString) out = NULL;
	g_autoptr(GPtrArray) specs = NULL;
	g_autofree GParamSpec **properties = NULL;
	g_autofree gchar *name = NULL;
	VentureEntityClass *klass;
	guint n_properties = 0;
	guint i;

	out = g_string_new(NULL);
	name = venture_entity_get_display_name(record);

	if (!venture_string_is_empty(name))
	{
		g_string_append(out, name);
		g_string_append(out, "\n\n");
	}

	klass = VENTURE_ENTITY_GET_CLASS(record);
	properties = g_object_class_list_properties(G_OBJECT_GET_CLASS(record),
	                                            &n_properties);

	/* Derived once. It is built from every property on each call, so
	 * fetching it per property would be quadratic for no gain. */
	specs = venture_entity_get_field_specs(record);

	for (i = 0; i < n_properties; i++)
	{
		VentureFieldKind kind;
		g_autofree gchar *value = NULL;
		gboolean sensitive = FALSE;
		guint j;

		if (!venture_entity_class_get_field_kind(klass,
		                                         properties[i]->name,
		                                         &kind))
			continue;

		if (VENTURE_FIELD_KIND_TEXT != kind)
			continue;

		/*
		 * Sensitive fields never reach the AI, and an embedding
		 * request is the AI. A field flagged sensitive is skipped even
		 * though it is text -- and the vector would outlive the
		 * redaction, since nothing about a stored vector says what it
		 * was made from.
		 */
		for (j = 0; (NULL != specs) && (j < specs->len); j++)
		{
			const VentureFieldSpec *spec;

			spec = g_ptr_array_index(specs, j);

			if (0 != g_strcmp0(venture_field_spec_get_name(spec),
			                   properties[i]->name))
				continue;

			sensitive = (0 != (venture_field_spec_get_flags(spec) &
			                   VENTURE_COLUMN_FLAG_SENSITIVE));
			break;
		}

		if (sensitive)
			continue;

		g_object_get(record, properties[i]->name, &value, NULL);

		if (venture_string_is_empty(value))
			continue;

		g_string_append(out, value);
		g_string_append(out, "\n\n");
	}

	if (0 == out->len)
		return NULL;

	return g_strdup(out->str);
}

/*
 * Removes a record's existing links.
 *
 * Purged rather than soft deleted, for the reason chunks are: a link is
 * derived, it carries no history of its own, and a soft-deleted one would
 * still be read back by the query that lists them.
 */
static gboolean
venture_kb_crossref_clear(
	VentureKbService	 *service,
	const gchar		 *type_name,
	gint64			  record_id,
	GError			**error
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) links = NULL;
	VentureDatabase *database;
	guint i;

	database = venture_context_get_database(
		venture_kb_service_get_context(service));

	query = venture_query_new(VENTURE_TYPE_KB_LINK);
	venture_query_set_limit(query, 0);

	if (!venture_query_add_filter_string(query, "subject-type",
	                                     VENTURE_FILTER_OP_EQ, type_name,
	                                     error))
		return FALSE;

	if (!venture_query_add_filter_int(query, "subject-id",
	                                  VENTURE_FILTER_OP_EQ, record_id,
	                                  error))
		return FALSE;

	links = venture_database_find(database, query, error);

	if (NULL == links)
		return FALSE;

	for (i = 0; i < links->len; i++)
	{
		if (!venture_database_purge(database,
		                            g_ptr_array_index(links, i), NULL,
		                            error))
			return FALSE;
	}

	return TRUE;
}

gint
venture_kb_crossref_record(
	VentureKbService	 *service,
	const gchar		 *type_name,
	gint64			  record_id,
	const VentureActor	 *actor,
	GError			**error
){
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(GPtrArray) hits = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *label = NULL;
	VentureContext *context;
	VentureEntityRegistry *registry;
	VentureConfig *config;
	VentureDatabase *database;
	GType entity_type;
	gint64 min_score = 60;
	gint64 max_links = 5;
	gint written = 0;
	guint i;

	g_return_val_if_fail(VENTURE_IS_KB_SERVICE(service), -1);
	g_return_val_if_fail(!venture_string_is_empty(type_name), -1);

	context = venture_kb_service_get_context(service);
	database = venture_context_get_database(context);
	registry = venture_context_get_entity_registry(context);
	config = venture_context_get_config(context);

	g_object_get(config, "kb-crossref-min-score", &min_score,
	             "kb-crossref-max-links", &max_links, NULL);

	entity_type = venture_entity_registry_lookup(registry, type_name);

	if (G_TYPE_INVALID == entity_type)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "There is no record type called \"%s\"", type_name);
		return -1;
	}

	if (!venture_kb_crossref_type_is_eligible(entity_type))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_UNSUPPORTED,
		            "%s has no long text to cross-reference against",
		            type_name);
		return -1;
	}

	record = venture_database_get(database, entity_type, record_id, error);

	if (NULL == record)
		return -1;

	text = venture_kb_crossref_text_of(record);

	/*
	 * An empty record clears its links rather than keeping them. A
	 * description that has been emptied is not still about what it used
	 * to be about.
	 */
	if (venture_string_is_empty(text))
	{
		if (!venture_kb_crossref_clear(service, type_name, record_id,
		                               error))
			return -1;

		return 0;
	}

	hits = venture_kb_service_search(service, text, NULL, 0,
	                                 (guint)max_links, error);

	if (NULL == hits)
		return -1;

	label = venture_entity_get_display_name(record);
	now = venture_time_now();

	if (!venture_database_begin(database, error))
		return -1;

	if (!venture_kb_crossref_clear(service, type_name, record_id, error))
	{
		venture_database_rollback(database);
		return -1;
	}

	for (i = 0; i < hits->len; i++)
	{
		const VentureKbHit *hit = g_ptr_array_index(hits, i);
		g_autoptr(VentureKbLink) link = NULL;

		/*
		 * Below the threshold nothing is recorded. A weak match is not
		 * a weak signal: with a corpus of any size everything matches
		 * everything a little, and a page listing its ten
		 * least-irrelevant passages is worse than one listing none.
		 */
		if ((hit->score * 100.0) < (gdouble)min_score)
			continue;

		link = venture_kb_link_new();
		g_object_set(link,
		             "article-id", hit->article_id,
		             "kb-id", hit->kb_id,
		             "subject-type", type_name,
		             "subject-id", record_id,
		             "subject-label", label,
		             "score", hit->score,
		             "chunk-ordinal", hit->ordinal,
		             "excerpt", hit->text,
		             "embedding-model",
		             venture_embedder_get_model(
		                 venture_kb_service_get_embedder(service)),
		             "computed-at", now,
		             NULL);

		venture_entity_set_organization_id(VENTURE_ENTITY(link),
			venture_entity_get_organization_id(record));

		if (!venture_database_save(database, VENTURE_ENTITY(link), actor,
		                           error))
		{
			venture_database_rollback(database);
			return -1;
		}

		written++;
	}

	if (!venture_database_commit(database, error))
		return -1;

	return written;
}

GPtrArray *
venture_kb_crossref_links_for(
	VentureKbService	 *service,
	const gchar		 *type_name,
	gint64			  record_id,
	GError			**error
){
	g_autoptr(VentureQuery) query = NULL;

	g_return_val_if_fail(VENTURE_IS_KB_SERVICE(service), NULL);

	query = venture_query_new(VENTURE_TYPE_KB_LINK);
	venture_query_set_limit(query, 0);

	if (!venture_query_add_filter_string(query, "subject-type",
	                                     VENTURE_FILTER_OP_EQ, type_name,
	                                     error))
		return NULL;

	if (!venture_query_add_filter_int(query, "subject-id",
	                                  VENTURE_FILTER_OP_EQ, record_id,
	                                  error))
		return NULL;

	return venture_database_find(
		venture_context_get_database(
			venture_kb_service_get_context(service)),
		query, error);
}

gint
venture_kb_crossref_sweep(
	VentureKbService	 *service,
	const gchar		 *type_name,
	guint			  limit,
	const VentureActor	 *actor,
	GError			**error
){
	g_auto(GStrv) names = NULL;
	VentureContext *context;
	VentureEntityRegistry *registry;
	gint processed = 0;
	gsize i;

	g_return_val_if_fail(VENTURE_IS_KB_SERVICE(service), -1);

	context = venture_kb_service_get_context(service);
	registry = venture_context_get_entity_registry(context);

	if (!venture_string_is_empty(type_name))
	{
		names = g_new0(gchar *, 2);
		names[0] = g_strdup(type_name);
	}
	else
	{
		names = venture_entity_registry_list_names(registry);
	}

	for (i = 0; (NULL != names) && (NULL != names[i]); i++)
	{
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(GPtrArray) records = NULL;
		GType entity_type;
		guint j;

		entity_type = venture_entity_registry_lookup(registry, names[i]);

		if (!venture_kb_crossref_type_is_eligible(entity_type))
			continue;

		query = venture_query_new(entity_type);
		venture_query_set_limit(query, 0);
		records = venture_database_find(
			venture_context_get_database(context), query, error);

		if (NULL == records)
			return -1;

		for (j = 0; j < records->len; j++)
		{
			if ((limit > 0) && ((guint)processed >= limit))
				return processed;

			if (venture_kb_crossref_record(service, names[i],
				venture_entity_get_id(g_ptr_array_index(records, j)),
				actor, error) < 0)
				return -1;

			processed++;
		}
	}

	return processed;
}

gint64
venture_kb_article_from_record(
	VentureKbService	 *service,
	gint64			  kb_id,
	const gchar		 *type_name,
	gint64			  record_id,
	const VentureActor	 *actor,
	GError			**error
){
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(VentureEntity) article = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) existing = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *title = NULL;
	g_autofree gchar *slug = NULL;
	VentureContext *context;
	VentureDatabase *database;
	VentureEntityRegistry *registry;
	GType entity_type;
	gint64 article_id;

	g_return_val_if_fail(VENTURE_IS_KB_SERVICE(service), -1);
	g_return_val_if_fail(!venture_string_is_empty(type_name), -1);

	context = venture_kb_service_get_context(service);
	database = venture_context_get_database(context);
	registry = venture_context_get_entity_registry(context);

	entity_type = venture_entity_registry_lookup(registry, type_name);

	if (G_TYPE_INVALID == entity_type)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "There is no record type called \"%s\"", type_name);
		return -1;
	}

	record = venture_database_get(database, entity_type, record_id, error);

	if (NULL == record)
		return -1;

	text = venture_kb_crossref_text_of(record);

	if (venture_string_is_empty(text))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "%s %" G_GINT64_FORMAT " has no text to write an "
		            "article from", type_name, record_id);
		return -1;
	}

	title = venture_entity_get_display_name(record);

	if (venture_string_is_empty(title))
	{
		g_free(title);
		title = g_strdup_printf("%s %" G_GINT64_FORMAT, type_name,
		                        record_id);
	}

	slug = venture_kb_slugify(title);

	/*
	 * A second call updates the article the first one wrote rather than
	 * making another. Generating twice from a ticket that has since been
	 * resolved should correct the article, not leave two versions of it
	 * for the assistant to choose between.
	 */
	query = venture_query_new(VENTURE_TYPE_KB_ARTICLE);
	venture_query_set_limit(query, 1);

	if (!venture_query_add_filter_string(query, "origin-type",
	                                     VENTURE_FILTER_OP_EQ, type_name,
	                                     error))
		return -1;

	if (!venture_query_add_filter_int(query, "origin-id",
	                                  VENTURE_FILTER_OP_EQ, record_id,
	                                  error))
		return -1;

	existing = venture_database_find(database, query, error);

	if (NULL == existing)
		return -1;

	if (existing->len > 0)
		article = g_object_ref(g_ptr_array_index(existing, 0));
	else
		article = VENTURE_ENTITY(venture_kb_article_new());

	g_object_set(article,
	             "kb-id", kb_id,
	             "title", title,
	             "slug", slug,
	             "body", text,
	             "format", VENTURE_KB_FORMAT_TEXT,
	             "origin-type", type_name,
	             "origin-id", record_id,
	             NULL);

	if (0 == existing->len)
		g_object_set(article, "status",
		             VENTURE_KB_ARTICLE_STATUS_PUBLISHED, NULL);

	venture_entity_set_organization_id(article,
		venture_entity_get_organization_id(record));

	if (!venture_database_save(database, article, actor, error))
		return -1;

	article_id = venture_entity_get_id(article);

	if (venture_kb_service_index_article(service, article_id, actor,
	                                     error) < 0)
		return -1;

	return article_id;
}
