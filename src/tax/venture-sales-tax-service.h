/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_SALES_TAX_SERVICE_H
#define VENTURE_SALES_TAX_SERVICE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS

#define VENTURE_TYPE_SALES_TAX_SERVICE (venture_sales_tax_service_get_type())
G_DECLARE_FINAL_TYPE(VentureSalesTaxService, venture_sales_tax_service,
	VENTURE, SALES_TAX_SERVICE, GObject)

/**
 * venture_sales_tax_service_get:
 * @database: the owning repository
 * Returns: (transfer none): its sales-tax service
 */
VentureSalesTaxService *venture_sales_tax_service_get(VentureDatabase *database);

/**
 * venture_sales_tax_service_resolve:
 * @self: the service
 * @customer: the invoice's bill-to company
 * @date: the issue date
 * @error: (out) (optional): a database failure
 *
 * Picks the jurisdiction rate window in force at @date for the customer's
 * address codes: the most specific active tax_rule of the customer's
 * organization whose state, county and city match exactly. A customer with
 * no matching rule, or a rule whose jurisdiction has no window covering
 * @date, resolves to nothing; that is not an error.
 *
 * Returns: (transfer full) (nullable): the tax_jurisdiction row to apply
 */
VentureEntity *venture_sales_tax_service_resolve(VentureSalesTaxService *self,
	VentureEntity *customer, GDateTime *date, GError **error);

/**
 * venture_sales_tax_service_freeze_line:
 * @self: the service
 * @invoice: the invoice being issued
 * @line: one of its lines, not yet frozen
 * @date: the issue date
 * @customer_exempt: whether the invoice or its customer is tax exempt
 * @numerator: (inout): the rate numerator the line would otherwise use
 * @denominator: (inout): the rate denominator the line would otherwise use
 * @error: (out) (optional): a database failure
 *
 * Called by VentureSettlementService once per line at first issue, before
 * the line's parts are computed, for lines that name no explicit tax code.
 * Writes the jurisdiction, the scaled rate actually applied and the exempt
 * flag onto @line, and replaces the rate when a jurisdiction applies. An
 * exempt customer or a tax-exempt product zeroes the rate while still
 * recording the jurisdiction, so exempt sales land on the right return.
 * When the sales_tax module is off nothing is touched.
 *
 * Returns: %TRUE on success
 */
gboolean venture_sales_tax_service_freeze_line(VentureSalesTaxService *self,
	VentureEntity *invoice, VentureEntity *line, GDateTime *date,
	gboolean customer_exempt, gint64 *numerator, gint64 *denominator, GError **error);

/**
 * venture_sales_tax_service_export_csv:
 * @self: the service
 * @context: the running context
 * @period: the return period
 * @options: (nullable): report options; "jurisdiction" keeps one code,
 *   "organization_id" selects the legal entity
 * @error: (out) (optional): an unknown jurisdiction code or a report failure
 *
 * Renders the sales_tax_return report in the shape the 1099-NEC pack uses:
 * a header of keys, every cell quoted per RFC 4180, money as "7.10 USD".
 *
 * Returns: (transfer full) (nullable): CSV text
 */
gchar *venture_sales_tax_service_export_csv(VentureSalesTaxService *self,
	VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error);

/**
 * venture_sales_tax_check_write:
 * @database: database owning the records
 * @record: candidate record
 * @removal: whether this is a removal operation
 * @error: (out) (optional): return location for an error
 *
 * Refuses a tax_jurisdiction whose window overlaps another window of the
 * same code in the same organization, a negative rate, an unknown kind, an
 * empty or inverted window, and a tax_rule with no state or with a
 * jurisdiction outside its organization.
 *
 * Returns: %TRUE when the write may proceed
 */
gboolean venture_sales_tax_check_write(VentureDatabase *database, VentureEntity *record,
	gboolean removal, GError **error);

/**
 * venture_sales_tax_register_reports:
 * @registry: the report registry
 *
 * Registers the sales_tax_return report.
 */
void venture_sales_tax_register_reports(VentureReportRegistry *registry);

G_END_DECLS
#endif
