/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

/* The same class metadata the finance gate reads: a type owned by a module
 * that transitively requires finance, and the payroll tag on top of it. */
static gboolean
tagged(VentureEntity *entity, const gchar *tag)
{
	return NULL != g_type_get_qdata(G_OBJECT_TYPE(entity), g_quark_from_static_string(tag));
}

gboolean
venture_accountant_role_readable(VentureDatabase *database, VentureEntity *entity)
{
	g_return_val_if_fail(VENTURE_IS_ENTITY(entity), FALSE);
	(void)database;
	/* Payroll is deliberately outside the books an outside accountant
	 * pulls: it names people and their pay, not the ledger. */
	if (tagged(entity, "venture-access-payroll"))
		return FALSE;
	if (tagged(entity, "venture-access-financial"))
		return TRUE;
	/* Aging and 1099 summaries name customers and vendors; a company row
	 * is the name on an invoice or a bill, not a CRM timeline. */
	if (VENTURE_IS_ORGANIZATION(entity) || VENTURE_IS_COMPANY(entity))
		return TRUE;
	if (VENTURE_IS_DOCUMENT(entity))
	{
		gint64 expense_id = 0;
		g_object_get(entity, "expense-id", &expense_id, NULL);
		return expense_id > 0;
	}
	return FALSE;
}

gboolean
venture_accountant_role_refuse_write(const gchar *action, GError **error)
{
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED,
		"The accountant role is read-only; it cannot %s records",
		0 == g_strcmp0(action, "delete") ? "delete" : "write");
	return FALSE;
}

gboolean
venture_accountant_role_only(VentureDatabase *database, const VentureAuthPrincipal *actor)
{
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) memberships = NULL;
	guint i;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), FALSE);
	if (NULL == actor || !actor->authenticated || actor->user_id <= 0)
		return FALSE;
	if (actor->role == VENTURE_USER_ROLE_OWNER || actor->role == VENTURE_USER_ROLE_ADMIN)
		return FALSE;
	if (!venture_entity_registry_is_type_enabled(venture_entity_registry_get_default(), "organization_membership"))
		return FALSE;
	internal = venture_access_policy_enter(venture_database_get_access_policy(database), NULL);
	query = venture_query_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP);
	venture_query_set_limit(query, 0);
	venture_query_add_filter_int(query, "user-id", VENTURE_FILTER_OP_EQ, actor->user_id, NULL);
	venture_query_add_filter_string(query, "active", VENTURE_FILTER_OP_EQ, "true", NULL);
	memberships = venture_database_find(database, query, NULL);
	if (NULL == memberships || 0 == memberships->len)
		return FALSE;
	for (i = 0; i < memberships->len; i++)
	{
		gint role;
		g_object_get(g_ptr_array_index(memberships, i), "role", &role, NULL);
		if (role != VENTURE_ORGANIZATION_ROLE_ACCOUNTANT)
			return FALSE;
	}
	return TRUE;
}
