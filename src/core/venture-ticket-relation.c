/*
 * venture-ticket-relation.c - Validating a polymorphic link
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

VentureTicketRelation *
venture_ticket_relation_create(
	VentureDatabase	 *database,
	gint64		  ticket_id,
	const gchar	 *subject_type,
	gint64		  subject_id,
	const gchar	 *note,
	GError		**error
){
	g_autoptr(VentureTicketRelation) relation = NULL;
	g_autoptr(VentureEntity) ticket = NULL;
	g_autoptr(VentureEntity) subject = NULL;
	g_autoptr(VentureQuery) existing = NULL;
	g_autofree gchar *label = NULL;
	VentureEntityRegistry *registry;
	GType subject_gtype;
	gint64 already;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);

	if (venture_string_is_empty(subject_type) || (0 == subject_id))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "A relation needs a record type and an id");
		return NULL;
	}

	registry = venture_entity_registry_get_default();
	subject_gtype = venture_entity_registry_lookup(registry, subject_type);

	/*
	 * An unregistered type is refused with the alternatives listed. The
	 * name is usually a near miss -- singular for plural, or a type that
	 * belongs to a plugin that is not loaded -- and a bare "unknown
	 * type" leaves the caller guessing which.
	 */
	if (G_TYPE_INVALID == subject_gtype)
	{
		venture_entity_registry_set_unknown_type_error(registry, subject_type,
		                                               error);
		return NULL;
	}

	/* A ticket cannot be related to itself: the relation would render as
	 * a link back to the page it is on, and "part of" already exists for
	 * the real ticket-to-ticket relationship. */
	if ((VENTURE_TYPE_TICKET == subject_gtype) && (subject_id == ticket_id))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "A ticket cannot be related to itself");
		return NULL;
	}

	ticket = venture_database_get(database, VENTURE_TYPE_TICKET, ticket_id,
	                              error);

	if (NULL == ticket)
		return NULL;

	subject = venture_database_get(database, subject_gtype, subject_id, error);

	if (NULL == subject)
	{
		if ((NULL != error) && (NULL == *error))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
			            "There is no %s with id %" G_GINT64_FORMAT,
			            subject_type, subject_id);
		}

		return NULL;
	}

	/* The same pair twice is a slip. Two identical rows cannot be told
	 * apart afterwards, so neither can be the one to remove. */
	existing = venture_query_new(VENTURE_TYPE_TICKET_RELATION);

	if (!venture_query_add_filter_int(existing, "ticket-id",
	                                  VENTURE_FILTER_OP_EQ, ticket_id, error))
		return NULL;

	if (!venture_query_add_filter_string(existing, "subject-type",
	                                     VENTURE_FILTER_OP_EQ, subject_type,
	                                     error))
		return NULL;

	if (!venture_query_add_filter_int(existing, "subject-id",
	                                  VENTURE_FILTER_OP_EQ, subject_id, error))
		return NULL;

	already = venture_database_count(database, existing, error);

	if (already < 0)
		return NULL;

	if (already > 0)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_ALREADY_EXISTS,
		            "That ticket is already related to %s #%" G_GINT64_FORMAT,
		            subject_type, subject_id);
		return NULL;
	}

	label = venture_entity_get_display_name(subject);

	relation = venture_ticket_relation_new();
	g_object_set(relation,
	             "ticket-id", ticket_id,
	             "subject-type", subject_type,
	             "subject-id", subject_id,
	             "subject-label", label,
	             "note", (NULL != note) ? note : "",
	             NULL);

	/* The ticket's organisation, not the subject's: the relation is part
	 * of the ticket's story, and a list scoped to one entity should see
	 * its own tickets' relations. */
	venture_entity_set_organization_id(VENTURE_ENTITY(relation),
		venture_entity_get_organization_id(ticket));

	return g_steal_pointer(&relation);
}

VentureEntity *
venture_ticket_relation_resolve(
	VentureDatabase		 *database,
	VentureTicketRelation	 *relation,
	GError			**error
){
	g_autofree gchar *subject_type = NULL;
	GType subject_gtype;
	gint64 subject_id = 0;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	g_return_val_if_fail(VENTURE_IS_TICKET_RELATION(relation), NULL);

	g_object_get(relation, "subject-type", &subject_type,
	             "subject-id", &subject_id, NULL);

	if (venture_string_is_empty(subject_type) || (0 == subject_id))
		return NULL;

	subject_gtype = venture_entity_registry_lookup(
		venture_entity_registry_get_default(), subject_type);

	if (G_TYPE_INVALID == subject_gtype)
		return NULL;

	/*
	 * A subject that has been deleted is not an error. The relation kept
	 * its label for exactly this case, so the ticket can still say what
	 * it was about after the thing itself has gone.
	 */
	return venture_database_get(database, subject_gtype, subject_id, error);
}

GPtrArray *
venture_ticket_relation_find_for_subject(
	VentureDatabase	 *database,
	const gchar	 *subject_type,
	gint64		  subject_id,
	GError		**error
){
	g_autoptr(VentureQuery) query = NULL;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);

	query = venture_query_new(VENTURE_TYPE_TICKET_RELATION);

	if (!venture_query_add_filter_string(query, "subject-type",
	                                     VENTURE_FILTER_OP_EQ, subject_type,
	                                     error))
		return NULL;

	if (!venture_query_add_filter_int(query, "subject-id",
	                                  VENTURE_FILTER_OP_EQ, subject_id, error))
		return NULL;

	if (!venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, error))
		return NULL;

	venture_query_set_limit(query, 50);

	return venture_database_find(database, query, error);
}
