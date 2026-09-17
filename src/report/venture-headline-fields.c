/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture-headline-private.h"

/*
 * A new expense or bill line inherits its category's acquisition default;
 * an existing one keeps whatever was chosen for it. Recategorising later
 * must not silently rewrite historical acquisition spend.
 */
static gboolean
acquisition_default(VentureDatabase *database, VentureEntity *record,
	VentureEntity *previous, gpointer unused, GError **error)
{
	g_autoptr(VentureEntity) category = NULL;
	g_autofree gchar *code = NULL;
	gboolean acquisition = FALSE;
	gint64 id = 0;
	if (previous != NULL)
		return TRUE;
	g_object_get(record, "acquisition", &acquisition, "category", &code, NULL);
	if (acquisition)
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
		g_object_get(category, "acquisition", &acquisition, NULL);
		g_object_set(record, "acquisition", acquisition, NULL);
	}
	return TRUE;
}

/* One settings row per organisation: a second would leave the home page
 * and the reports reading whichever sorted first. */
static gboolean
one_setting_per_organization(VentureDatabase *database, VentureEntity *record,
	VentureEntity *previous, gpointer unused, GError **error)
{
	g_autoptr(VentureEntity) existing = NULL;
	if (previous != NULL)
		return TRUE;
	existing = venture_headline_setting_find(database, venture_entity_get_organization_id(record));
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
	venture_database_add_save_validator(database, VENTURE_TYPE_EXPENSE, acquisition_default, NULL, NULL);
	venture_database_add_save_validator(database, VENTURE_TYPE_VENDOR_BILL_LINE, acquisition_default, NULL, NULL);
	venture_database_add_save_validator(database, VENTURE_TYPE_HEADLINE_SETTING, one_setting_per_organization, NULL, NULL);
}
