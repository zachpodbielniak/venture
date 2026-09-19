/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

/* One row is one rate window. The code is the jurisdiction's identity, so a
 * rate change is a new row with a new window, never an edit to history. */
static const VentureFieldDecl jurisdiction_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", "What the return calls it, for example New York City"),
	VENTURE_FIELD("code", "Code", "Jurisdiction code shared by every rate window, for example US-NY-NYC",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED | VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD("kind", "Kind", "sales or use; empty means sales",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("rate-scaled", "Rate", "Percent times 10000: 88750 is 8.875%",
		VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("effective-from", "Effective from", "Inclusive; the rate applies to invoices issued on or after this instant",
		VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("effective-to", "Effective to", "Exclusive; empty means open-ended. Windows for one code never overlap",
		VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED)
};
VENTURE_DEFINE_ENTITY(VentureTaxJurisdiction, venture_tax_jurisdiction, jurisdiction_fields)

/* Codes are matched exactly against the customer's address codes after
 * trimming and upper-casing. An empty county or city matches any; the rule
 * naming the most parts wins. */
static const VentureFieldDecl rule_fields[] = {
	VENTURE_FIELD_REF("jurisdiction-id", "Jurisdiction", "Any rate window of the jurisdiction; the window in force at issue is applied",
		"tax_jurisdiction", VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("state", "State", "Required address state code, for example NY",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("county", "County", "Optional county code; empty matches any",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("city", "City", "Optional city code; empty matches any",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("active", "Active", NULL, VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_INDEXED)
};
VENTURE_DEFINE_ENTITY(VentureTaxRule, venture_tax_rule, rule_fields)

gboolean
venture_tax_jurisdiction_get_rate(VentureTaxJurisdiction *self, gint64 *numerator,
	gint64 *denominator, GError **error)
{
	gint64 rate = 0;

	g_return_val_if_fail(VENTURE_IS_TAX_JURISDICTION(self), FALSE);
	g_object_get(self, "rate-scaled", &rate, NULL);
	if (rate < 0)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"A jurisdiction rate cannot be negative");
		return FALSE;
	}
	if (numerator != NULL)
		*numerator = rate;
	if (denominator != NULL)
		*denominator = VENTURE_TAX_RATE_DENOMINATOR;
	return TRUE;
}

gboolean
venture_tax_jurisdiction_covers(VentureTaxJurisdiction *self, GDateTime *date)
{
	g_autoptr(GDateTime) from = NULL;
	g_autoptr(GDateTime) to = NULL;

	g_return_val_if_fail(VENTURE_IS_TAX_JURISDICTION(self), FALSE);
	g_return_val_if_fail(date != NULL, FALSE);
	g_object_get(self, "effective-from", &from, "effective-to", &to, NULL);
	if (from == NULL || g_date_time_compare(date, from) < 0)
		return FALSE;
	return to == NULL || g_date_time_compare(date, to) < 0;
}

gchar *
venture_tax_rate_percent_string(gint64 rate_scaled)
{
	gint64 magnitude = rate_scaled < 0 ? -rate_scaled : rate_scaled;

	return g_strdup_printf("%s%" G_GINT64_FORMAT ".%04" G_GINT64_FORMAT,
		rate_scaled < 0 ? "-" : "", magnitude / VENTURE_TAX_RATE_SCALE,
		magnitude % VENTURE_TAX_RATE_SCALE);
}
