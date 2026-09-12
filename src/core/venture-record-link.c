/*
 * venture-record-link.c - Linking any record to any other
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

/*
 * Loads one end of a link, refusing a type that is not offered and a
 * record that is not there. The registry's own message is used for the
 * type, so "the crm module is disabled" and "no such type" come out
 * different.
 *
 * Returns: (transfer full) (nullable): the record
 */
static VentureEntity *
venture_record_link_load_end(
	VentureDatabase	 *database,
	const gchar	 *role,
	const gchar	 *entity_type,
	gint64		  entity_id,
	GError		**error
){
	g_autoptr(VentureEntity) record = NULL;
	VentureEntityRegistry *registry;
	GType gtype;

	if (venture_string_is_empty(entity_type) || (0 == entity_id))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "A link needs a record type and an id at its %s", role);
		return NULL;
	}

	registry = venture_entity_registry_get_default();
	gtype = venture_entity_registry_lookup(registry, entity_type);

	if (G_TYPE_INVALID == gtype)
	{
		venture_entity_registry_set_unknown_type_error(registry, entity_type,
		                                               error);
		return NULL;
	}

	/* The audit log is not linkable: it is the record of what happened,
	 * not a thing that happens. */
	if (VENTURE_TYPE_AUDIT_ENTRY == gtype)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "An audit entry cannot be linked");
		return NULL;
	}

	record = venture_database_get(database, gtype, entity_id, NULL);

	if ((NULL == record) || venture_entity_is_deleted(record))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "There is no %s with id %" G_GINT64_FORMAT "%s",
		            entity_type, entity_id,
		            (NULL != record) ? " (it has been deleted)" : "");
		return NULL;
	}

	return g_steal_pointer(&record);
}

/*
 * Whether the pair is already linked, in either direction. The same two
 * records linked twice cannot be told apart afterwards, so neither can be
 * the one to remove; and "A blocks B" beside "B blocked by A" is one fact
 * written twice.
 *
 * Returns: the id of the existing link, or 0
 */
static gint64
venture_record_link_find_existing(
	VentureDatabase	 *database,
	const gchar	 *source_type,
	gint64		  source_id,
	const gchar	 *target_type,
	gint64		  target_id,
	gint64		  ignore_id,
	GError		**error
){
	gint pass;

	for (pass = 0; pass < 2; pass++)
	{
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(GPtrArray) found = NULL;
		const gchar *from_type;
		const gchar *to_type;
		gint64 from_id;
		gint64 to_id;
		guint i;

		/* Forwards, then backwards. */
		from_type = (0 == pass) ? source_type : target_type;
		from_id = (0 == pass) ? source_id : target_id;
		to_type = (0 == pass) ? target_type : source_type;
		to_id = (0 == pass) ? target_id : source_id;

		query = venture_query_new(VENTURE_TYPE_RECORD_LINK);

		if (!venture_query_add_filter_string(query, "source-type",
		                                     VENTURE_FILTER_OP_EQ, from_type,
		                                     error) ||
		    !venture_query_add_filter_int(query, "source-id",
		                                  VENTURE_FILTER_OP_EQ, from_id,
		                                  error) ||
		    !venture_query_add_filter_string(query, "target-type",
		                                     VENTURE_FILTER_OP_EQ, to_type,
		                                     error) ||
		    !venture_query_add_filter_int(query, "target-id",
		                                  VENTURE_FILTER_OP_EQ, to_id, error))
			return -1;

		venture_query_set_limit(query, 10);
		found = venture_database_find(database, query, error);

		if (NULL == found)
			return -1;

		for (i = 0; i < found->len; i++)
		{
			VentureEntity *existing;

			existing = g_ptr_array_index(found, i);

			if (venture_entity_get_id(existing) != ignore_id)
				return venture_entity_get_id(existing);
		}
	}

	return 0;
}

/*
 * The checks, shared by the constructor and the save validator. Fills the
 * labels and the organisation as a side effect, so a link written through
 * the generic API without them still reads correctly.
 */
static gboolean
venture_record_link_check(
	VentureDatabase		 *database,
	VentureRecordLink	 *link,
	GError			**error
){
	g_autoptr(VentureEntity) source = NULL;
	g_autoptr(VentureEntity) target = NULL;
	g_autofree gchar *source_type = NULL;
	g_autofree gchar *target_type = NULL;
	g_autofree gchar *source_label = NULL;
	g_autofree gchar *target_label = NULL;
	gint64 source_id = 0;
	gint64 target_id = 0;
	gint64 existing;

	g_object_get(link,
	             "source-type", &source_type, "source-id", &source_id,
	             "source-label", &source_label,
	             "target-type", &target_type, "target-id", &target_id,
	             "target-label", &target_label,
	             NULL);

	source = venture_record_link_load_end(database, "source", source_type,
	                                      source_id, error);

	if (NULL == source)
		return FALSE;

	target = venture_record_link_load_end(database, "target", target_type,
	                                      target_id, error);

	if (NULL == target)
		return FALSE;

	/* A record linked to itself renders as a link back to the page it
	 * is on, and means nothing. Compared by type and id rather than by
	 * the spelling given, so `ticket` and `tickets` are the same end. */
	if ((G_OBJECT_TYPE(source) == G_OBJECT_TYPE(target)) &&
	    (source_id == target_id))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "A record cannot be linked to itself");
		return FALSE;
	}

	existing = venture_record_link_find_existing(
		database, venture_entity_get_entity_name(source), source_id,
		venture_entity_get_entity_name(target), target_id,
		venture_entity_get_id(VENTURE_ENTITY(link)), error);

	if (existing < 0)
		return FALSE;

	if (existing > 0)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_ALREADY_EXISTS,
		            "%s #%" G_GINT64_FORMAT " and %s #%" G_GINT64_FORMAT
		            " are already linked (link #%" G_GINT64_FORMAT ")",
		            venture_entity_get_entity_name(source), source_id,
		            venture_entity_get_entity_name(target), target_id,
		            existing);
		return FALSE;
	}

	/*
	 * Canonical spellings and fresh labels. The labels are what the row
	 * displays as -- and what it keeps displaying once an end has been
	 * deleted, which is exactly when somebody is working out what the
	 * link was about.
	 */
	{
		g_autofree gchar *fresh_source = NULL;
		g_autofree gchar *fresh_target = NULL;

		fresh_source = venture_entity_get_display_name(source);
		fresh_target = venture_entity_get_display_name(target);

		g_object_set(link,
		             "source-type", venture_entity_get_entity_name(source),
		             "target-type", venture_entity_get_entity_name(target),
		             "source-label", fresh_source,
		             "target-label", fresh_target,
		             NULL);
	}

	/* The source's organisation, not the target's: the link is part of
	 * the source's story, and a list scoped to one entity should see its
	 * own records' links. */
	if (0 == venture_entity_get_organization_id(VENTURE_ENTITY(link)))
	{
		venture_entity_set_organization_id(VENTURE_ENTITY(link),
			venture_entity_get_organization_id(source));
	}

	return TRUE;
}

VentureRecordLink *
venture_record_link_create(
	VentureDatabase	 *database,
	const gchar	 *source_type,
	gint64		  source_id,
	VentureLinkKind	  kind,
	const gchar	 *target_type,
	gint64		  target_id,
	const gchar	 *note,
	GError		**error
){
	g_autoptr(VentureRecordLink) link = NULL;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);

	link = venture_record_link_new();
	g_object_set(link,
	             "source-type", (NULL != source_type) ? source_type : "",
	             "source-id", source_id,
	             "kind", kind,
	             "target-type", (NULL != target_type) ? target_type : "",
	             "target-id", target_id,
	             "note", (NULL != note) ? note : "",
	             NULL);

	if (!venture_record_link_check(database, link, error))
		return NULL;

	return g_steal_pointer(&link);
}

/*
 * The save validator. A link is checked on every save, not only on the
 * first: an update that moves an end is a new link for these purposes.
 */
static gboolean
venture_record_link_validate_save(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	gpointer	  user_data,
	GError		**error
){
	(void)previous;
	(void)user_data;

	return venture_record_link_check(database, VENTURE_RECORD_LINK(entity),
	                                 error);
}

void
venture_record_link_install_validator(VentureDatabase *database)
{
	g_return_if_fail(VENTURE_IS_DATABASE(database));

	venture_database_add_save_validator(database, VENTURE_TYPE_RECORD_LINK,
	                                    venture_record_link_validate_save,
	                                    NULL, NULL);
}

static gint
venture_record_link_compare_by_id(
	gconstpointer	a,
	gconstpointer	b
){
	gint64 left;
	gint64 right;

	left = venture_entity_get_id(*(VentureEntity *const *)a);
	right = venture_entity_get_id(*(VentureEntity *const *)b);

	return (left < right) ? -1 : (left > right) ? 1 : 0;
}

GPtrArray *
venture_record_link_find_for(
	VentureDatabase	 *database,
	const gchar	 *entity_type,
	gint64		  entity_id,
	GError		**error
){
	g_autoptr(GPtrArray) links = NULL;
	g_autoptr(GHashTable) seen = NULL;
	VentureEntityRegistry *registry;
	GType gtype;
	const gchar *canonical;
	gint pass;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	g_return_val_if_fail(NULL != entity_type, NULL);

	/* Rows store the canonical name; the caller may hold the plural. */
	registry = venture_entity_registry_get_default();
	gtype = venture_entity_registry_lookup_any(registry, entity_type);
	canonical = entity_type;

	if (G_TYPE_INVALID != gtype)
	{
		VentureEntity *prototype;

		prototype = venture_entity_registry_get_prototype(registry, entity_type);

		if (NULL != prototype)
			canonical = venture_entity_get_entity_name(prototype);
	}

	links = g_ptr_array_new_with_free_func(g_object_unref);
	seen = g_hash_table_new(g_direct_hash, g_direct_equal);

	for (pass = 0; pass < 2; pass++)
	{
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(GPtrArray) found = NULL;
		guint i;

		query = venture_query_new(VENTURE_TYPE_RECORD_LINK);

		if (!venture_query_add_filter_string(
			query, (0 == pass) ? "source-type" : "target-type",
			VENTURE_FILTER_OP_EQ, canonical, error) ||
		    !venture_query_add_filter_int(
			query, (0 == pass) ? "source-id" : "target-id",
			VENTURE_FILTER_OP_EQ, entity_id, error))
			return NULL;

		venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
		venture_query_set_limit(query, 200);

		found = venture_database_find(database, query, error);

		if (NULL == found)
			return NULL;

		for (i = 0; i < found->len; i++)
		{
			VentureEntity *link;

			link = g_ptr_array_index(found, i);

			/* A row cannot match both passes unless it links a record
			 * to itself, which the validator forbids -- but a row that
			 * predates the validator could, and one entry is enough. */
			if (g_hash_table_contains(seen,
				GSIZE_TO_POINTER(venture_entity_get_id(link))))
				continue;

			g_hash_table_add(seen, GSIZE_TO_POINTER(venture_entity_get_id(link)));
			g_ptr_array_add(links, g_object_ref(link));
		}
	}

	g_ptr_array_sort(links, venture_record_link_compare_by_id);

	return g_steal_pointer(&links);
}

gboolean
venture_record_link_other_end(
	VentureRecordLink	 *link,
	const gchar		 *entity_type,
	gint64			  entity_id,
	gchar			**out_type,
	gint64			 *out_id,
	gchar			**out_label,
	VentureLinkKind		 *out_kind
){
	g_autofree gchar *source_type = NULL;
	g_autofree gchar *target_type = NULL;
	g_autofree gchar *source_label = NULL;
	g_autofree gchar *target_label = NULL;
	VentureEntityRegistry *registry;
	VentureLinkKind kind;
	GType standing;
	gint64 source_id = 0;
	gint64 target_id = 0;
	gboolean at_source;

	g_return_val_if_fail(VENTURE_IS_RECORD_LINK(link), FALSE);
	g_return_val_if_fail(NULL != entity_type, FALSE);

	g_object_get(link,
	             "source-type", &source_type, "source-id", &source_id,
	             "source-label", &source_label,
	             "target-type", &target_type, "target-id", &target_id,
	             "target-label", &target_label,
	             "kind", &kind,
	             NULL);

	/* Compared by type rather than by spelling, for the plural. */
	registry = venture_entity_registry_get_default();
	standing = venture_entity_registry_lookup_any(registry, entity_type);

	if ((source_id == entity_id) &&
	    (venture_entity_registry_lookup_any(registry, source_type) == standing))
		at_source = TRUE;
	else if ((target_id == entity_id) &&
	         (venture_entity_registry_lookup_any(registry, target_type) == standing))
		at_source = FALSE;
	else
		return FALSE;

	{
		gchar **other_type;
		gchar **other_label;

		other_type = at_source ? &target_type : &source_type;
		other_label = at_source ? &target_label : &source_label;

		if (NULL != out_type)
			*out_type = g_steal_pointer(other_type);

		if (NULL != out_label)
			*out_label = g_steal_pointer(other_label);
	}

	if (NULL != out_id)
		*out_id = at_source ? target_id : source_id;

	if (NULL != out_kind)
		*out_kind = at_source ? kind : venture_link_kind_inverse(kind);

	return TRUE;
}

VentureEntity *
venture_record_link_resolve(
	VentureDatabase	 *database,
	const gchar	 *entity_type,
	gint64		  entity_id,
	GError		**error
){
	GType gtype;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);

	if (venture_string_is_empty(entity_type) || (0 == entity_id))
		return NULL;

	/* A hidden type resolves to nothing, so a link into a module that is
	 * off keeps its label and loses its link -- the same as a deleted
	 * record, and for the same reason. */
	gtype = venture_entity_registry_lookup(venture_entity_registry_get_default(),
	                                       entity_type);

	if (G_TYPE_INVALID == gtype)
		return NULL;

	return venture_database_get(database, gtype, entity_id, error);
}

JsonNode *
venture_record_link_describe_for(
	VentureDatabase	 *database,
	const gchar	 *entity_type,
	gint64		  entity_id,
	GError		**error
){
	g_autoptr(GPtrArray) links = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	guint i;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);

	links = venture_record_link_find_for(database, entity_type, entity_id,
	                                     error);

	if (NULL == links)
		return NULL;

	builder = json_builder_new();
	json_builder_begin_array(builder);

	for (i = 0; i < links->len; i++)
	{
		g_autoptr(VentureEntity) other = NULL;
		g_autofree gchar *other_type = NULL;
		g_autofree gchar *other_label = NULL;
		g_autofree gchar *note = NULL;
		VentureRecordLink *link;
		VentureLinkKind kind;
		gint64 other_id = 0;
		gboolean exists;

		link = g_ptr_array_index(links, i);

		if (!venture_record_link_other_end(link, entity_type, entity_id,
		                                   &other_type, &other_id,
		                                   &other_label, &kind))
			continue;

		other = venture_record_link_resolve(database, other_type, other_id,
		                                    NULL);
		exists = (NULL != other) && !venture_entity_is_deleted(other);

		if (exists)
		{
			g_free(other_label);
			other_label = venture_entity_get_display_name(other);
		}

		g_object_get(link, "note", &note, NULL);

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "id");
		json_builder_add_int_value(builder,
		                           venture_entity_get_id(VENTURE_ENTITY(link)));
		json_builder_set_member_name(builder, "kind");
		json_builder_add_string_value(builder,
			venture_enum_to_nick(VENTURE_TYPE_LINK_KIND, (gint)kind));
		json_builder_set_member_name(builder, "kind_label");
		json_builder_add_string_value(builder, venture_link_kind_to_label(kind));
		json_builder_set_member_name(builder, "other_type");
		json_builder_add_string_value(builder, other_type);
		json_builder_set_member_name(builder, "other_id");
		json_builder_add_int_value(builder, other_id);
		json_builder_set_member_name(builder, "other_label");
		json_builder_add_string_value(builder,
		                              (NULL != other_label) ? other_label : "");
		json_builder_set_member_name(builder, "other_exists");
		json_builder_add_boolean_value(builder, exists);
		json_builder_set_member_name(builder, "note");
		json_builder_add_string_value(builder, (NULL != note) ? note : "");
		json_builder_end_object(builder);
	}

	json_builder_end_array(builder);

	return json_builder_get_root(builder);
}
