/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_REPORT_PACK_DELIVERY_H
#define VENTURE_REPORT_PACK_DELIVERY_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
#include "report/venture-report-pack-service.h"
#include "mail/venture-mail-records.h"
G_BEGIN_DECLS
/**
 * VENTURE_REPORT_PACK_MAIL_TEMPLATE:
 *
 * The name of the organization's optional =mail_template= that shapes the
 * subject and bodies of a delivered pack. Its placeholders are {name},
 * {period}, {run_at} and {reports}.
 */
#define VENTURE_REPORT_PACK_MAIL_TEMPLATE "report_pack"
/**
 * venture_report_pack_delivery_validate:
 * @pack: a report pack about to be saved
 * @error: (out) (optional): the refusal
 *
 * Normalizes and checks the delivery fields: =deliver= is =none= (the
 * default, written when empty) or =email=; every comma-separated address in
 * =recipients= is trimmed and must be syntactically valid; =email= requires
 * at least one recipient.
 *
 * Returns: whether the pack may be saved
 */
gboolean venture_report_pack_delivery_validate(VentureEntity *pack, GError **error);
/**
 * venture_report_pack_delivery_address_is_valid:
 * @address: one trimmed address
 *
 * Returns: whether @address has the shape of a mailbox: one @, a local part,
 *   a domain, and no whitespace, separators or control characters
 */
gboolean venture_report_pack_delivery_address_is_valid(const gchar *address);
/**
 * venture_report_pack_service_deliver:
 * @self: the service
 * @context: application context; its mail module and outbox are used
 * @pack: a pack whose last retained output is to be mailed
 * @now: (nullable): the delivery clock; now when omitted
 * @actor: (nullable): audit actor
 * @recorded: (out) (optional): whether the outcome was written to the pack;
 *   %FALSE only when the pack itself could not be saved
 * @error: (out) (optional): why nothing was queued
 *
 * Queues the pack's last retained output through the mail outbox: the
 * output JSON, a CSV per tabular report, and a subject and body from the
 * organization's =report_pack= mail template or the built-in one. Nothing
 * is sent here; the outbox sweep does that. The pack records the outcome
 * either way: =last-delivered-at=, =last-delivery-message-id= and
 * =last-delivery-mail-id= when queued, =last-delivery-error= when refused.
 * A refusal never touches =last-run-at= or =last-output=.
 *
 * Returns: (transfer full) (nullable): the queued mail_message row
 */
VentureMailMessage *venture_report_pack_service_deliver(VentureReportPackService *self, VentureContext *context,
	VentureReportPack *pack, GDateTime *now, const VentureActor *actor, gboolean *recorded, GError **error);
G_END_DECLS
#endif
