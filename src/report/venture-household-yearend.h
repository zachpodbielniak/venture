/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_HOUSEHOLD_YEAREND_H
#define VENTURE_HOUSEHOLD_YEAREND_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
/**
 * venture_household_yearend_register_reports:
 * @registry: the report registry
 *
 * Registers =household_tax_year= (purchases tagged for tax, with their
 * receipts, as the export for whoever does the taxes) and
 * =household_warranties= (documents with a warranty end date).
 */
void venture_household_yearend_register_reports(VentureReportRegistry *registry);
/**
 * venture_household_yearend_install: (skip)
 * @database: the repository
 *
 * Installs the warranty reminder check: a reminder needs an end date, and
 * a private purchase's warranty reminds only its owner.
 */
void venture_household_yearend_install(VentureDatabase *database);
/**
 * venture_household_warranty_remind:
 * @context: the wiring
 * @organization_id: one organization, or 0 for every organization
 * @now: (nullable): the clock, or %NULL for now
 * @error: return location for a #GError
 *
 * Sends one inbox reminder per warranty end date to the document's
 * reminder person, within the last 30 days before it ends, and only while
 * that person may still read the document.
 *
 * Returns: reminders sent, or -1 on error
 */
gint venture_household_warranty_remind(VentureContext *context, gint64 organization_id, GDateTime *now, GError **error);
G_END_DECLS
#endif
