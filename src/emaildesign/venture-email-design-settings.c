/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * venture-email-design-settings.c - The email brand's defaults as settings
 * (docs/email-design.org, docs/settings.org)
 *
 * They live on the organization's one email_brand record. Changing one
 * when there is no brand yet makes it, named after the organization, the
 * way the designer does when it first asks for a logo.
 */
#include "venture.h"

#include <string.h>

static VentureEntity *locate_brand(VentureContext *context, gint64 organization_id, gint64 member, gboolean for_write,
	GError **error)
{
	VentureDatabase *database = venture_context_get_database(context);
	g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(database), NULL);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_EMAIL_BRAND);
	g_autoptr(VentureEntity) org = NULL;
	g_autofree gchar *name = NULL;
	VentureEntity *brand;
	(void)member;
	venture_query_set_organization(query, organization_id);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	brand = venture_database_find_one(database, query, error);
	if (brand != NULL || (error != NULL && *error != NULL) || !for_write)
		return brand;
	org = venture_database_get(database, VENTURE_TYPE_ORGANIZATION, organization_id, NULL);
	if (org != NULL)
		g_object_get(org, "name", &name, NULL);
	return g_object_new(VENTURE_TYPE_EMAIL_BRAND, "organization-id", organization_id,
		"name", !venture_string_is_empty(name) ? name : "My business", NULL);
}

static gboolean colour(const gchar *value, GError **error)
{
	gsize i;
	if (value[0] == '#' && strlen(value) == 7) {
		for (i = 1; i < 7 && g_ascii_isxdigit(value[i]); i++)
			;
		if (i == 7)
			return TRUE;
	}
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Write a colour as #rrggbb, for example #c2410c");
	return FALSE;
}

#define BRAND_COLOUR(key, label, help, value, field) { key, "Email", "mail", label, help, VENTURE_SETTING_TEXT, value, \
	0, 0, NULL, VENTURE_SETTING_PERMISSION_ADMIN, "email_brand: " field, field, "", locate_brand, NULL, colour }

static const VentureSettingInfo brand[] = {
	BRAND_COLOUR("email-brand.primary-color", "Brand colour", "Headings and buttons in designed email.", "#1f5fbf", "primary-color"),
	BRAND_COLOUR("email-brand.accent-color", "Accent colour", "Links and a second button colour in designed email.", "#0f766e", "accent-color"),
	BRAND_COLOUR("email-brand.text-color", "Text colour", "The words of designed email.", "#1f2933", "text-color"),
	BRAND_COLOUR("email-brand.background-color", "Background colour", "The surround of designed email.", "#f3f4f6", "background-color"),
	{ "email-brand.font", "Email", "mail", "Font",
	  "A web-safe face with a fallback, because mail apps cannot load web fonts reliably.",
	  VENTURE_SETTING_CHOICE, "sans", 0, 0, "sans,serif,rounded,system,mono", VENTURE_SETTING_PERMISSION_ADMIN,
	  "email_brand: font", "font", "", locate_brand, NULL, NULL }
};

void venture_email_design_settings_register(void)
{
	gsize i;
	for (i = 0; i < G_N_ELEMENTS(brand); i++)
		venture_settings_register(&brand[i]);
}
