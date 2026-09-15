/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include <sys/random.h>
#include <errno.h>
#include <string.h>

struct _VenturePortalService
{
	GObject parent_instance;
	VentureDatabase *database;
	VentureEntity *writing;
};
G_DEFINE_FINAL_TYPE(VenturePortalService, venture_portal_service, G_TYPE_OBJECT)

static gboolean
refuse(GError **error, const gchar *message)
{
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "VenturePortalService: %s", message);
	return FALSE;
}

static void
get_property(GObject *object, guint id, GValue *value, GParamSpec *spec)
{
	if (id == 1)
		g_value_set_object(value, VENTURE_PORTAL_SERVICE(object)->database);
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

static void
set_property(GObject *object, guint id, const GValue *value, GParamSpec *spec)
{
	VenturePortalService *self = VENTURE_PORTAL_SERVICE(object);
	if (id == 1)
	{
		self->database = g_value_get_object(value);
		if (self->database != NULL)
			g_object_add_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	}
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

static void
finalize(GObject *object)
{
	VenturePortalService *self = VENTURE_PORTAL_SERVICE(object);
	if (self->database != NULL)
		g_object_remove_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	G_OBJECT_CLASS(venture_portal_service_parent_class)->finalize(object);
}

static void
venture_portal_service_class_init(VenturePortalServiceClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	object_class->get_property = get_property;
	object_class->set_property = set_property;
	object_class->finalize = finalize;
	g_object_class_install_property(object_class, 1,
		g_param_spec_object("database", "Database", "Owning database", VENTURE_TYPE_DATABASE,
			G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
}

static void
venture_portal_service_init(VenturePortalService *self)
{
	(void)self;
}

VenturePortalService *
venture_portal_service_get(VentureDatabase *database)
{
	VenturePortalService *self;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	self = g_object_get_data(G_OBJECT(database), "venture-portal-service");
	if (self == NULL)
	{
		self = g_object_new(VENTURE_TYPE_PORTAL_SERVICE, "database", database, NULL);
		g_object_set_data_full(G_OBJECT(database), "venture-portal-service", self, g_object_unref);
	}
	return self;
}

gboolean
venture_portal_check_write(VentureDatabase *database, VentureEntity *record, gboolean removal, GError **error)
{
	VenturePortalService *self;
	(void)removal;
	if (record == NULL || database == NULL || !VENTURE_IS_CUSTOMER_PORTAL_ACCESS(record))
		return TRUE;
	self = venture_portal_service_get(database);
	if (self->writing == record)
		return TRUE;
	return refuse(error, "customer portal access is owned by VenturePortalService");
}

static gboolean
save_owned(VenturePortalService *self, VentureEntity *record, const VentureActor *actor, GError **error)
{
	gboolean ok;
	self->writing = record;
	ok = venture_database_save(self->database, record, actor, error);
	self->writing = NULL;
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

VentureEntity *
venture_portal_service_invite(VenturePortalService *self, gint64 organization_id, gint64 company_id,
	const gchar *email, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureCustomerPortalAccess) access = NULL;
	g_autofree gchar *token = NULL;
	g_return_val_if_fail(VENTURE_IS_PORTAL_SERVICE(self), NULL);
	token = secret(error);
	if (token == NULL)
		return NULL;
	access = venture_customer_portal_access_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(access), organization_id);
	g_object_set(access, "company-id", company_id, "token", token, "email", email ? email : "",
		"revoked", FALSE, NULL);
	if (!save_owned(self, VENTURE_ENTITY(access), actor, error))
		return NULL;
	return VENTURE_ENTITY(g_steal_pointer(&access));
}

gboolean
venture_portal_service_revoke(VenturePortalService *self, VentureCustomerPortalAccess *access,
	const VentureActor *actor, GError **error)
{
	g_return_val_if_fail(VENTURE_IS_PORTAL_SERVICE(self), FALSE);
	g_object_set(access, "revoked", TRUE, NULL);
	return save_owned(self, VENTURE_ENTITY(access), actor, error);
}

VentureEntity *
venture_portal_service_lookup(VenturePortalService *self, const gchar *token, GError **error)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) access = NULL;
	gboolean revoked;
	g_return_val_if_fail(VENTURE_IS_PORTAL_SERVICE(self), NULL);
	if (token == NULL || strlen(token) != 64)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Not found");
		return NULL;
	}
	query = venture_query_new(VENTURE_TYPE_CUSTOMER_PORTAL_ACCESS);
	venture_query_add_filter_string(query, "token", VENTURE_FILTER_OP_EQ, token, NULL);
	access = venture_database_find_one(self->database, query, error);
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
venture_portal_service_invoices(VenturePortalService *self, VentureCustomerPortalAccess *access, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_INVOICE);
	gint64 company, org;
	g_object_get(access, "company-id", &company, NULL);
	org = venture_entity_get_organization_id(VENTURE_ENTITY(access));
	venture_query_set_organization(query, org);
	venture_query_set_limit(query, 0);
	venture_query_add_filter_int(query, "company-id", VENTURE_FILTER_OP_EQ, company, NULL);
	return venture_database_find(self->database, query, error);
}

gboolean
venture_portal_service_pay(VenturePortalService *self, VentureCustomerPortalAccess *access,
	gint64 invoice_id, const VentureMoney *amount, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VenturePayment) payment = NULL;
	g_autoptr(GDateTime) now = NULL;
	gint64 company, invoice_company, org;
	gint status;
	g_return_val_if_fail(VENTURE_IS_PORTAL_SERVICE(self), FALSE);
	g_object_get(access, "company-id", &company, NULL);
	org = venture_entity_get_organization_id(VENTURE_ENTITY(access));
	invoice = venture_database_get(self->database, VENTURE_TYPE_INVOICE, invoice_id, error);
	if (invoice == NULL)
		return FALSE;
	if (venture_entity_get_organization_id(invoice) != org)
		return refuse(error, "invoice is not in this organization");
	g_object_get(invoice, "company-id", &invoice_company, "status", &status, NULL);
	if (invoice_company != company)
		return refuse(error, "invoice does not belong to this customer");
	if (status != VENTURE_INVOICE_STATUS_SENT && status != VENTURE_INVOICE_STATUS_PARTIALLY_PAID)
		return refuse(error, "only issued open invoices can be paid");
	payment = venture_payment_new();
	now = venture_time_now();
	g_object_set(payment, "customer-id", company, "invoice-id", invoice_id, "date", now,
		"method", "portal", "amount", amount, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(payment), org);
	return venture_settlement_service_apply_payment(venture_settlement_service_get(self->database),
		payment, NULL, actor, error);
}

static gboolean
portal_allowed(VentureAction *action, VentureEntity *entity, const VentureActor *actor, GError **error)
{
	(void)action;
	(void)entity;
	(void)actor;
	(void)error;
	return TRUE;
}

static VentureEntity *
portal_invoke(VentureAction *action, VentureEntity *entity, GHashTable *params,
	const VentureActor *actor, GError **error)
{
	g_autofree gchar *name = NULL;
	g_object_get(action, "name", &name, NULL);
	(void)params;
	if (g_strcmp0(name, "revoke") == 0)
		return venture_portal_service_revoke(venture_action_get_data(action),
			VENTURE_CUSTOMER_PORTAL_ACCESS(entity), actor, error) ? g_object_ref(entity) : NULL;
	return NULL;
}

void
venture_portal_actions_register(VentureDatabase *database)
{
	g_autoptr(VentureAction) action = g_object_new(VENTURE_TYPE_ACTION, "type-name", "customer_portal_access",
		"name", "revoke", "label", "Revoke", "description", "Revoke portal access without deleting history",
		"stageable", FALSE, "roles", VENTURE_USER_ROLE_EDITOR, NULL);
	g_autoptr(GError) error = NULL;
	venture_action_registry_register(venture_database_get_action_registry(database), action,
		portal_allowed, portal_invoke, venture_portal_service_get(database), NULL, &error);
}
