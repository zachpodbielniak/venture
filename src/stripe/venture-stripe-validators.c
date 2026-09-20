/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>

static gboolean
stripe_record_validate(VentureDatabase *database, VentureEntity *entity,
	VentureEntity *previous, gpointer data, GError **error)
{
	gint64 binding, old_binding = 0;
	g_autoptr(VentureEntity) connection = NULL;
	g_autofree gchar *provider = NULL;
	g_autofree GParamSpec **properties = NULL;
	guint n, i;
	(void)data;
	g_object_get(entity, "connection-id", &binding, NULL);
	if (previous) g_object_get(previous, "connection-id", &old_binding, NULL);
	if (previous && (binding != old_binding || venture_entity_get_organization_id(previous) != venture_entity_get_organization_id(entity)))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Stripe account evidence cannot be reassigned");
		return FALSE;
	}
	if (binding > 0)
	{
		connection = venture_database_get(database, VENTURE_TYPE_INTEGRATION_CONNECTION, binding, error);
		if (!connection) return FALSE;
		g_object_get(connection, "provider", &provider, NULL);
		if (g_strcmp0(provider, "stripe") || venture_entity_get_organization_id(connection) != venture_entity_get_organization_id(entity))
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Stripe records require their organization's Stripe connection");
			return FALSE;
		}
	}
	properties = venture_entity_class_list_persistent_properties(VENTURE_ENTITY_GET_CLASS(entity), &n);
	for (i = 0; i < n; i++)
	{
		const gchar *reference = venture_entity_class_get_reference(VENTURE_ENTITY_GET_CLASS(entity), properties[i]->name);
		gint64 id;
		g_autoptr(VentureEntity) target = NULL;
		GType type;
		if (!reference || !g_strcmp0(reference, "organization")) continue;
		g_object_get(entity, properties[i]->name, &id, NULL);
		if (id <= 0) continue;
		type = venture_entity_registry_lookup_any(venture_entity_registry_get_default(), reference);
		target = venture_database_get(database, type, id, error);
		if (!target) return FALSE;
		if (venture_entity_get_organization_id(target) != venture_entity_get_organization_id(entity))
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Stripe references must belong to the same organization");
			return FALSE;
		}
	}
	return TRUE;
}

void
venture_stripe_install_validators(VentureDatabase *database)
{
	GType types[] = { VENTURE_TYPE_STRIPE_PRICE_LINK, VENTURE_TYPE_STRIPE_CUSTOMER_LINK,
		VENTURE_TYPE_STRIPE_AUTHORIZATION,
		VENTURE_TYPE_STRIPE_PAYMENT_LINK, VENTURE_TYPE_STRIPE_CHECKOUT, VENTURE_TYPE_STRIPE_EVENT,
		VENTURE_TYPE_PROCESSOR_PAYOUT, VENTURE_TYPE_PROCESSOR_DISPUTE,
		VENTURE_TYPE_PROCESSOR_EXCEPTION };
	guint i;
	for (i = 0; i < G_N_ELEMENTS(types); i++)
		venture_database_add_save_validator(database, types[i], stripe_record_validate, NULL, NULL);
}
