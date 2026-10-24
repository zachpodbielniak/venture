/*
 * venture-deal-contact.h - Contact roles on deals
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Who on the customer's side is on a deal, and as what: the decision
 * maker, the economic buyer, a champion. A `deal_contact` names a deal, a
 * contact, a role, whether that person is the deal's primary contact (one
 * at most) and a note. The primary is who a sequence or a follow-up from
 * the deal goes to. See docs/pipelines.org.
 *
 * The record and its role enum are declared beside the deal in
 * model/venture-records.h, because venturectl builds the record types
 * without the database; the rules and lookups here are the server's.
 */

#ifndef VENTURE_DEAL_CONTACT_H
#define VENTURE_DEAL_CONTACT_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>

G_BEGIN_DECLS

/**
 * venture_deal_contact_role_label:
 * @role: a role
 *
 * Returns: (transfer none): what a person calls @role, "Decision maker"
 */
const gchar *
venture_deal_contact_role_label(VentureDealContactRole role);

/**
 * venture_deal_contact_install:
 * @context: application context
 *
 * Adds the save rule (a live deal and contact in the record's
 * organization, one row per person a deal, one primary a deal) and makes
 * deleting a contact delete its deal roles. Calling it again for the same
 * database adds nothing.
 */
void
venture_deal_contact_install(VentureContext *context);

/**
 * venture_deal_contact_list:
 * @database: the database to read
 * @field: "deal-id" or "contact-id"
 * @id: the deal or contact
 * @error: (out) (optional): return location for a #GError
 *
 * The live roles on a deal, or held by a contact, primary first.
 *
 * Returns: (transfer full) (element-type VentureEntity) (nullable): the
 *   rows, or %NULL with @error set
 */
GPtrArray *
venture_deal_contact_list(
	VentureDatabase	 *database,
	const gchar	 *field,
	gint64		  id,
	GError		**error
);

/**
 * venture_deal_contact_primary:
 * @database: the database to read
 * @deal: the deal
 * @error: (out) (optional): return location for a #GError
 *
 * Who a message about @deal goes to: its primary contact, or, when no
 * live person is marked primary, the deal's own `contact-id`.
 *
 * Returns: (transfer full) (nullable): the contact, or %NULL -- with
 *   @error unset when the deal simply has nobody
 */
VentureEntity *
venture_deal_contact_primary(
	VentureDatabase	 *database,
	VentureEntity	 *deal,
	GError		**error
);

/**
 * venture_deal_contact_is_on_deal:
 * @database: the database to read
 * @deal: the deal
 * @contact_id: the person
 *
 * Returns: %TRUE when @contact_id is the deal's own contact or holds a
 *   live role on it
 */
gboolean
venture_deal_contact_is_on_deal(
	VentureDatabase	*database,
	VentureEntity	*deal,
	gint64		 contact_id
);

/**
 * venture_deal_contact_register_reports:
 * @registry: the report registry
 *
 * Adds `deals_without_decision_maker`: open deals nobody on which is
 * recorded as the decision maker.
 */
void
venture_deal_contact_register_reports(VentureReportRegistry *registry);

G_END_DECLS

#endif /* VENTURE_DEAL_CONTACT_H */
