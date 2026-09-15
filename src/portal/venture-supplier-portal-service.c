/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include <sys/random.h>
#include <errno.h>
#include <string.h>

static gboolean
refuse(GError **error, const gchar *message)
{
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "VenturePortalService: %s", message);
	return FALSE;
}

gboolean
venture_supplier_portal_check_write(VentureDatabase *database, VentureEntity *record, gboolean removal, GError **error)
{
	(void)removal;
	if (record == NULL || database == NULL || !VENTURE_IS_SUPPLIER_PORTAL_ACCESS(record))
		return TRUE;
	if (g_object_get_data(G_OBJECT(database), "venture-supplier-portal-writing") == record)
		return TRUE;
	return refuse(error, "supplier portal access is owned by VenturePortalService");
}

static gboolean
save_owned(VentureDatabase *database, VentureEntity *record, const VentureActor *actor, GError **error)
{
	gboolean ok;
	g_object_set_data(G_OBJECT(database), "venture-supplier-portal-writing", record);
	ok = venture_database_save(database, record, actor, error);
	g_object_set_data(G_OBJECT(database), "venture-supplier-portal-writing", NULL);
	return ok;
}

static gchar *
secret(GError **error)
{
	guchar bytes[32];
	gchar *result;
	gsize used = 0;
	guint i;
	while (used < sizeof(bytes))
	{
		ssize_t n = getrandom(bytes + used, sizeof(bytes) - used, 0);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
		{
			refuse(error, "cannot obtain secure random bytes");
			return NULL;
		}
		used += (gsize)n;
	}
	result = g_malloc(65);
	for (i = 0; i < sizeof(bytes); i++)
		g_snprintf(result + 2 * i, 3, "%02x", bytes[i]);
	return result;
}

static VentureDatabase *
service_database(VenturePortalService *self)
{
	VentureDatabase *database = NULL;
	g_object_get(self, "database", &database, NULL);
	return database;
}

VentureEntity *
venture_portal_service_invite_supplier(VenturePortalService *self, gint64 organization_id, gint64 company_id,
	const gchar *email, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureDatabase) database = NULL;
	g_autoptr(VentureSupplierPortalAccess) access = NULL;
	g_autoptr(VentureEntity) company = NULL;
	g_autofree gchar *token = NULL;
	gint kind;
	g_return_val_if_fail(VENTURE_IS_PORTAL_SERVICE(self), NULL);
	database = service_database(self);
	if (database == NULL)
		return NULL;
	company = venture_database_get(database, VENTURE_TYPE_COMPANY, company_id, error);
	if (company == NULL)
		return NULL;
	if (venture_entity_get_organization_id(company) != organization_id)
	{
		refuse(error, "supplier is not in this organization");
		return NULL;
	}
	g_object_get(company, "kind", &kind, NULL);
	if (kind != VENTURE_COMPANY_KIND_SUPPLIER)
	{
		refuse(error, "portal invitations are limited to supplier companies");
		return NULL;
	}
	token = secret(error);
	if (token == NULL)
		return NULL;
	access = venture_supplier_portal_access_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(access), organization_id);
	g_object_set(access, "company-id", company_id, "token", token, "email", email ? email : "",
		"revoked", FALSE, NULL);
	if (!save_owned(database, VENTURE_ENTITY(access), actor, error))
		return NULL;
	return VENTURE_ENTITY(g_steal_pointer(&access));
}

gboolean
venture_portal_service_revoke_supplier(VenturePortalService *self, VentureSupplierPortalAccess *access,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureDatabase) database = NULL;
	g_return_val_if_fail(VENTURE_IS_PORTAL_SERVICE(self), FALSE);
	database = service_database(self);
	g_object_set(access, "revoked", TRUE, NULL);
	return save_owned(database, VENTURE_ENTITY(access), actor, error);
}

VentureEntity *
venture_portal_service_lookup_supplier(VenturePortalService *self, const gchar *token, GError **error)
{
	g_autoptr(VentureDatabase) database = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) access = NULL;
	gboolean revoked;
	g_return_val_if_fail(VENTURE_IS_PORTAL_SERVICE(self), NULL);
	database = service_database(self);
	if (token == NULL || strlen(token) != 64)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Not found");
		return NULL;
	}
	query = venture_query_new(VENTURE_TYPE_SUPPLIER_PORTAL_ACCESS);
	venture_query_add_filter_string(query, "token", VENTURE_FILTER_OP_EQ, token, NULL);
	access = venture_database_find_one(database, query, error);
	if (access == NULL)
	{
		g_clear_error(error);
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Not found");
		return NULL;
	}
	g_object_get(access, "revoked", &revoked, NULL);
	if (revoked)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Not found");
		return NULL;
	}
	return g_steal_pointer(&access);
}

GPtrArray *
venture_portal_service_bills(VenturePortalService *self, VentureSupplierPortalAccess *access, GError **error)
{
	g_autoptr(VentureDatabase) database = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_VENDOR_BILL);
	gint64 company, org;
	database = service_database(self);
	g_object_get(access, "company-id", &company, NULL);
	org = venture_entity_get_organization_id(VENTURE_ENTITY(access));
	venture_query_set_organization(query, org);
	venture_query_set_limit(query, 0);
	venture_query_add_filter_int(query, "company-id", VENTURE_FILTER_OP_EQ, company, NULL);
	return venture_database_find(database, query, error);
}

static gboolean
supplier_allowed(VentureAction *action, VentureEntity *entity, const VentureActor *actor, GError **error)
{
	(void)action;
	(void)entity;
	(void)actor;
	(void)error;
	return TRUE;
}

static VentureEntity *
supplier_invoke(VentureAction *action, VentureEntity *entity, GHashTable *params,
	const VentureActor *actor, GError **error)
{
	g_autofree gchar *name = NULL;
	g_object_get(action, "name", &name, NULL);
	(void)params;
	if (g_strcmp0(name, "revoke") == 0)
		return venture_portal_service_revoke_supplier(venture_action_get_data(action),
			VENTURE_SUPPLIER_PORTAL_ACCESS(entity), actor, error) ? g_object_ref(entity) : NULL;
	return NULL;
}

void
venture_supplier_portal_actions_register(VentureDatabase *database)
{
	g_autoptr(VentureAction) action = g_object_new(VENTURE_TYPE_ACTION, "type-name", "supplier_portal_access",
		"name", "revoke", "label", "Revoke", "description", "Revoke supplier portal access without deleting history",
		"stageable", FALSE, "roles", VENTURE_USER_ROLE_EDITOR, NULL);
	g_autoptr(GError) error = NULL;
	venture_action_registry_register(venture_database_get_action_registry(database), action,
		supplier_allowed, supplier_invoke, venture_portal_service_get(database), NULL, &error);
}
