/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_HEADLINE_PRIVATE_H
#define VENTURE_HEADLINE_PRIVATE_H
#include "venture.h"

/**
 * venture_headline_install_validators:
 * @database: the repository receiving classification validators
 *
 * Applies category defaults (acquisition and cost of revenue) only on
 * creation, preserving subsequent choices, and keeps the headline settings
 * to one living row per organisation on every save.
 */
void venture_headline_install_validators(VentureDatabase *database);

/**
 * venture_headline_register_reports:
 * @registry: the report registry
 *
 * Registers cac, customer_churn, ltv, ltv_cac and customer_cohorts.
 */
void venture_headline_register_reports(VentureReportRegistry *registry);

/**
 * VentureHeadlineSnapshot:
 *
 * Everything the headline reports read, fetched once per question and then
 * derived in memory for as many periods as the caller asks about: customer
 * cash, the first cash per company, receipt cadence, acquisition and cost of
 * revenue spend, converted leads, recurring schedules and subscriptions,
 * support hours and company names. Each part is loaded the first time a
 * report needs it. The home page builds one and reads both its periods from
 * it; a single report builds its own.
 */
typedef struct _VentureHeadlineSnapshot VentureHeadlineSnapshot;

/**
 * venture_headline_snapshot_new:
 * @context: the wiring
 * @options: (nullable): report options: organization_id, venture_id, as_of
 *   and days are read
 * @error: (out) (optional): an invalid as_of
 *
 * Returns: (transfer full) (nullable): an empty snapshot scoped by @options
 */
VentureHeadlineSnapshot *venture_headline_snapshot_new(VentureContext *context, JsonObject *options, GError **error);

/**
 * venture_headline_snapshot_free:
 * @snapshot: (nullable): the snapshot
 */
void venture_headline_snapshot_free(VentureHeadlineSnapshot *snapshot);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(VentureHeadlineSnapshot, venture_headline_snapshot_free)

/**
 * venture_headline_snapshot_get_options:
 * @snapshot: the snapshot
 *
 * Returns: (transfer none): the scoping options every report run from this
 *   snapshot passes on: organization_id always, venture_id and as_of when
 *   given, and currency (the book currency)
 */
JsonObject *venture_headline_snapshot_get_options(VentureHeadlineSnapshot *snapshot);

/**
 * venture_headline_snapshot_billing_in_use:
 * @snapshot: the snapshot
 * @error: (out) (optional): a failed read
 * @out_in_use: (out): whether billing is on and the organisation has
 *   subscription history
 *
 * Returns: %TRUE when the question could be answered
 */
gboolean venture_headline_snapshot_billing_in_use(VentureHeadlineSnapshot *snapshot, gboolean *out_in_use, GError **error);

/**
 * venture_headline_snapshot_cac:
 * @snapshot: the snapshot
 * @period: the period
 * @error: (out) (optional): a failed read
 *
 * Returns: (transfer full) (nullable): the cac report for @period
 */
VentureReportResult *venture_headline_snapshot_cac(VentureHeadlineSnapshot *snapshot, VentureDateRange *period, GError **error);

/**
 * venture_headline_snapshot_churn:
 * @snapshot: the snapshot
 * @period: the period
 * @error: (out) (optional): a failed read
 *
 * Returns: (transfer full) (nullable): the customer_churn report for @period
 */
VentureReportResult *venture_headline_snapshot_churn(VentureHeadlineSnapshot *snapshot, VentureDateRange *period, GError **error);

/**
 * venture_headline_snapshot_ltv:
 * @snapshot: the snapshot
 * @period: the period
 * @error: (out) (optional): a failed read
 *
 * Returns: (transfer full) (nullable): the ltv report for @period
 */
VentureReportResult *venture_headline_snapshot_ltv(VentureHeadlineSnapshot *snapshot, VentureDateRange *period, GError **error);

/**
 * venture_headline_snapshot_ltv_cac:
 * @snapshot: the snapshot
 * @period: the period
 * @ltv: (nullable): the ltv report already computed for @period, reused
 * @cac: (nullable): the cac report already computed for @period, reused
 * @error: (out) (optional): a failed read
 *
 * Returns: (transfer full) (nullable): the ltv_cac report for @period
 */
VentureReportResult *venture_headline_snapshot_ltv_cac(VentureHeadlineSnapshot *snapshot, VentureDateRange *period,
	VentureReportResult *ltv, VentureReportResult *cac, GError **error);
/**
 * venture_headline_snapshot_get_currency:
 * @snapshot: the snapshot
 *
 * Returns: (transfer none): the upper-case ISO 4217 code every headline
 *   total is carried in: the organisation's book currency, or the process
 *   default when it has none
 */
const gchar *venture_headline_snapshot_get_currency(VentureHeadlineSnapshot *snapshot);

/**
 * venture_headline_snapshot_customer_cash:
 * @snapshot: the snapshot
 * @company_id: the company
 * @from: (nullable): the first instant that counts, inclusive
 * @until: (nullable): the last instant that counts, inclusive
 * @out_total: (out) (transfer full) (nullable): the sum, or %NULL when the
 *   company paid nothing in the window
 * @error: (out) (optional): a failed read
 *
 * One company's paid revenue between two instants, as every headline
 * figure counts it: cash sales derived from applied receipts less those
 * derived from refunds, in the book currency, foreign sales left out. This
 * is what customer health's trailing year is read from, so the health
 * report and the LTV report cannot disagree about what a customer paid.
 *
 * Returns: %TRUE when the question could be answered
 */
gboolean venture_headline_snapshot_customer_cash(VentureHeadlineSnapshot *snapshot, gint64 company_id,
	GDateTime *from, GDateTime *until, VentureMoney **out_total, GError **error);
#endif
