/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture-headline-private.h"

/*
 * A new expense or bill line inherits its category's acquisition and cost of
 * revenue defaults; an existing one keeps whatever was chosen for it.
 * Recategorising later must not silently rewrite historical acquisition
 * spend or a closed year's gross margin. Each flag is inherited on its own:
 * a line typed in as acquisition still picks up the category's cost of
 * revenue default, and the reports then refuse to count acquisition spend
 * as cost of revenue, so the two can never double count.
 */
static gboolean
classification_defaults(VentureDatabase *database, VentureEntity *record,
	VentureEntity *previous, gpointer unused, GError **error)
{
	g_autoptr(VentureEntity) category = NULL;
	g_autofree gchar *code = NULL;
	gboolean acquisition = FALSE;
	gboolean cost_of_revenue = FALSE;
	gint64 id = 0;

	(void)unused;
	if (previous != NULL)
		return TRUE;
	g_object_get(record, "acquisition", &acquisition, "cost-of-revenue", &cost_of_revenue,
		"category", &code, NULL);
	if (acquisition && cost_of_revenue)
		return TRUE;
	if (VENTURE_IS_EXPENSE(record))
		g_object_get(record, "tax-category-id", &id, NULL);
	if (id != 0)
		category = venture_database_get(database, VENTURE_TYPE_TAX_CATEGORY, id, error);
	else if (!venture_string_is_empty(code))
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_TAX_CATEGORY);
		venture_query_set_organization(query, venture_entity_get_organization_id(record));
		if (!venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, code, error))
			return FALSE;
		category = venture_database_find_one(database, query, error);
	}
	if (error != NULL && *error != NULL)
		return FALSE;
	if (category != NULL && venture_entity_get_organization_id(category) == venture_entity_get_organization_id(record))
	{
		gboolean category_acquisition = FALSE;
		gboolean category_cost = FALSE;

		g_object_get(category, "acquisition", &category_acquisition,
			"cost-of-revenue", &category_cost, NULL);
		/* Only ever turn a flag on: a flag the writer set explicitly is
		 * a decision, and the category default must not undo it. */
		if (!acquisition)
			g_object_set(record, "acquisition", category_acquisition, NULL);
		if (!cost_of_revenue)
			g_object_set(record, "cost-of-revenue", category_cost, NULL);
	}
	return TRUE;
}

/*
 * One settings row per organisation: a second would leave the home page and
 * the reports reading whichever sorted first. Checked on every save, not
 * only the first -- an update that moves a row to another organisation is
 * the same second row by another door. A restore skips validators, which is
 * why migration 000350 also backs this with a unique partial index.
 */
static gboolean
one_setting_per_organization(VentureDatabase *database, VentureEntity *record,
	VentureEntity *previous, gpointer unused, GError **error)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) existing = NULL;

	(void)unused;
	(void)previous;
	/* Retiring a row can only reduce the count; refusing it would make an
	 * old duplicate impossible to clean up. */
	if (venture_entity_is_deleted(record))
		return TRUE;
	query = venture_query_new(VENTURE_TYPE_HEADLINE_SETTING);
	venture_query_set_organization(query, venture_entity_get_organization_id(record));
	if (!venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, error))
		return FALSE;
	if (venture_entity_is_persisted(record) &&
		!venture_query_add_filter_int(query, "id", VENTURE_FILTER_OP_NE, venture_entity_get_id(record), error))
		return FALSE;
	existing = venture_database_find_one(database, query, error);
	if (error != NULL && *error != NULL)
		return FALSE;
	if (existing == NULL)
		return TRUE;
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		"This organization already has headline settings (headline_setting #%" G_GINT64_FORMAT "); edit that row",
		venture_entity_get_id(existing));
	return FALSE;
}

void
venture_headline_install_validators(VentureDatabase *database)
{
	venture_database_add_save_validator(database, VENTURE_TYPE_EXPENSE, classification_defaults, NULL, NULL);
	venture_database_add_save_validator(database, VENTURE_TYPE_VENDOR_BILL_LINE, classification_defaults, NULL, NULL);
	venture_database_add_save_validator(database, VENTURE_TYPE_HEADLINE_SETTING, one_setting_per_organization, NULL, NULL);
}
