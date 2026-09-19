/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_SALES_TAX_RECORDS_H
#define VENTURE_SALES_TAX_RECORDS_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

G_BEGIN_DECLS

/**
 * VENTURE_TAX_RATE_SCALE:
 *
 * A jurisdiction rate is a percent with up to four decimals, stored as an
 * integer multiplied by this scale: 88750 is 8.875%.
 */
#define VENTURE_TAX_RATE_SCALE 10000

/**
 * VENTURE_TAX_RATE_DENOMINATOR:
 *
 * The exact denominator that turns a scaled rate into a fraction of the net
 * amount: rate_scaled / 1000000 is the levy per unit of net.
 */
#define VENTURE_TAX_RATE_DENOMINATOR (VENTURE_TAX_RATE_SCALE * 100)

#define VENTURE_TYPE_TAX_JURISDICTION (venture_tax_jurisdiction_get_type())
VENTURE_DECLARE_ENTITY(VentureTaxJurisdiction, venture_tax_jurisdiction, TAX_JURISDICTION)
#define VENTURE_TYPE_TAX_RULE (venture_tax_rule_get_type())
VENTURE_DECLARE_ENTITY(VentureTaxRule, venture_tax_rule, TAX_RULE)

/**
 * venture_tax_jurisdiction_new:
 * Returns: (transfer full): one rate window of a sales or use tax jurisdiction
 */
/**
 * venture_tax_rule_new:
 * Returns: (transfer full): an address-code rule that selects a jurisdiction
 */

/**
 * venture_tax_jurisdiction_get_rate:
 * @self: the jurisdiction rate row
 * @numerator: (out) (optional): the scaled rate
 * @denominator: (out) (optional): %VENTURE_TAX_RATE_DENOMINATOR
 * @error: (out) (optional): a negative rate
 *
 * Reads the exact rate as a fraction, ready for venture_quote_rate_parts().
 *
 * Returns: %TRUE when the rate is usable
 */
gboolean venture_tax_jurisdiction_get_rate(VentureTaxJurisdiction *self,
	gint64 *numerator, gint64 *denominator, GError **error);

/**
 * venture_tax_jurisdiction_covers:
 * @self: the jurisdiction rate row
 * @date: the instant to test
 *
 * Returns: %TRUE when @date falls in [effective-from, effective-to)
 */
gboolean venture_tax_jurisdiction_covers(VentureTaxJurisdiction *self, GDateTime *date);

/**
 * venture_tax_rate_percent_string:
 * @rate_scaled: a scaled rate
 *
 * Returns: (transfer full): the percent with four decimals, "8.8750"
 */
gchar *venture_tax_rate_percent_string(gint64 rate_scaled);

G_END_DECLS
#endif
