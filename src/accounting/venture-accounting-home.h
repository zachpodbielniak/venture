/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_ACCOUNTING_HOME_H
#define VENTURE_ACCOUNTING_HOME_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS

/**
 * venture_accounting_home:
 * @context: the application context
 * @organization_id: the legal entity, or 0 for the default
 * @error: (out) (optional): failure
 *
 * Next actions for the daily books: unmatched bank lines, overdue invoices,
 * bills to pay, the close checklist and the capture inbox. Each row names
 * the count, a link and the reason it is on the list.
 *
 * Returns: (transfer full) (nullable): a JSON array of action objects
 */
JsonNode *venture_accounting_home(VentureContext *context, gint64 organization_id, GError **error);

G_END_DECLS
#endif
