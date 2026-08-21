/*
 * venture-ticket-relation.h - Pointing a ticket at anything
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A #VentureTicketRelation names its subject by type and id rather than by
 * a foreign key, because the set of things a ticket can be about is the set
 * of registered record types -- which is not known when the field table is
 * written, and grows with every plugin.
 *
 * That shape has no database constraint behind it. Nothing stops a row
 * naming a type that was never registered or an id that was never there,
 * and such a row is not a broken link so much as a link to nothing at all:
 * it cannot be rendered, followed or explained. So the check happens here,
 * before the row is written, and this is the only function that should
 * create one.
 */

#ifndef VENTURE_TICKET_RELATION_H
#define VENTURE_TICKET_RELATION_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>

G_BEGIN_DECLS

/**
 * venture_ticket_relation_create:
 * @database: the database
 * @ticket_id: the ticket doing the pointing
 * @subject_type: the registered entity name of the subject, e.g. `invoice`
 * @subject_id: the subject's id
 * @note: (nullable): what the connection is, in a few words
 * @error: (out) (optional): return location for a #GError
 *
 * Builds a relation after checking it can mean something.
 *
 * The type must be registered and the record must exist; either failing is
 * an error rather than a row nobody can follow. The subject's display name
 * is captured as it is now, so the relation still reads correctly once the
 * subject has been renamed or deleted -- which is exactly when somebody is
 * trying to work out what a ticket was about.
 *
 * The relation inherits the ticket's organisation, so an organisation-scoped
 * list sees its own relations and nobody else's.
 *
 * Returns %VENTURE_ERROR_ALREADY_EXISTS when the ticket already points at
 * that record. Linking the same thing twice is a slip, not an intention,
 * and two identical rows are indistinguishable afterwards.
 *
 * Returns: (transfer full) (nullable): the unsaved relation, or %NULL
 */
VentureTicketRelation *
venture_ticket_relation_create(
	VentureDatabase	 *database,
	gint64		  ticket_id,
	const gchar	 *subject_type,
	gint64		  subject_id,
	const gchar	 *note,
	GError		**error
);

/**
 * venture_ticket_relation_resolve:
 * @database: the database
 * @relation: the relation
 * @error: (out) (optional): return location for a #GError
 *
 * Loads the record a relation points at.
 *
 * Returns %NULL with no error set when the subject has been deleted, which
 * is a normal state rather than a failure: the relation keeps its label so
 * the ticket can still say what it was about.
 *
 * Returns: (transfer full) (nullable): the subject, or %NULL
 */
VentureEntity *
venture_ticket_relation_resolve(
	VentureDatabase		 *database,
	VentureTicketRelation	 *relation,
	GError			**error
);

/**
 * venture_ticket_relation_find_for_subject:
 * @database: the database
 * @subject_type: the registered entity name
 * @subject_id: the subject's id
 * @error: (out) (optional): return location for a #GError
 *
 * Finds every relation pointing at one record, so its page can show which
 * tickets are about it.
 *
 * This is what a declared reference would have got for nothing from
 * venture_web_append_related(); a polymorphic pair has to ask.
 *
 * Returns: (transfer full) (element-type VentureTicketRelation) (nullable):
 *   the relations, or %NULL on error
 */
GPtrArray *
venture_ticket_relation_find_for_subject(
	VentureDatabase	 *database,
	const gchar	 *subject_type,
	gint64		  subject_id,
	GError		**error
);

G_END_DECLS

#endif /* VENTURE_TICKET_RELATION_H */
