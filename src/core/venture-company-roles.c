/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

static gboolean
flag(VentureEntity *company, const gchar *name)
{
	gboolean value = FALSE;
	g_object_get(company, name, &value, NULL);
	return value;
}

static gint
kind(VentureEntity *company)
{
	gint value = VENTURE_COMPANY_KIND_OTHER;
	g_object_get(company, "kind", &value, NULL);
	return value;
}

gboolean
venture_company_is_customer(VentureEntity *company)
{
	g_return_val_if_fail(VENTURE_IS_COMPANY(company), FALSE);
	return flag(company, "customer") || kind(company) == VENTURE_COMPANY_KIND_CUSTOMER;
}

gboolean
venture_company_is_supplier(VentureEntity *company)
{
	g_return_val_if_fail(VENTURE_IS_COMPANY(company), FALSE);
	return flag(company, "supplier") || kind(company) == VENTURE_COMPANY_KIND_SUPPLIER;
}

/* The relationship sets its own flag on every save, from any writer, so
 * the Customers and Suppliers lists can filter on the flags alone. */
static gboolean
roles_validate(VentureDatabase *database, VentureEntity *entity, VentureEntity *previous,
	gpointer user_data, GError **error)
{
	(void)database; (void)previous; (void)user_data; (void)error;
	if (!VENTURE_IS_COMPANY(entity))
		return TRUE;
	if (kind(entity) == VENTURE_COMPANY_KIND_CUSTOMER && !flag(entity, "customer"))
		g_object_set(entity, "customer", TRUE, NULL);
	if (kind(entity) == VENTURE_COMPANY_KIND_SUPPLIER && !flag(entity, "supplier"))
		g_object_set(entity, "supplier", TRUE, NULL);
	return TRUE;
}

void
venture_company_roles_install(VentureDatabase *database)
{
	g_return_if_fail(VENTURE_IS_DATABASE(database));
	if (g_object_get_data(G_OBJECT(database), "venture-company-roles"))
		return;
	g_object_set_data(G_OBJECT(database), "venture-company-roles", GINT_TO_POINTER(1));
	venture_database_add_save_validator(database, VENTURE_TYPE_COMPANY, roles_validate, NULL, NULL);
}
