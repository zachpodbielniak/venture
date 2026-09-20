/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>

static void
stripe_context_free(gpointer data)
{
	GWeakRef *reference = data;
	g_weak_ref_clear(reference);
	g_free(reference);
}

void
venture_stripe_actions_set_context(VentureDatabase *database, VentureContext *context)
{
	GWeakRef *reference = g_new0(GWeakRef, 1);
	g_weak_ref_init(reference, context);
	g_object_set_data_full(G_OBJECT(database), "venture-stripe-action-context", reference, stripe_context_free);
}

static gboolean
stripe_action_allowed(VentureAction *action, VentureEntity *entity,
	const VentureActor *actor, GError **error)
{
	static const gint roles[] = { VENTURE_ORGANIZATION_ROLE_OWNER,
		VENTURE_ORGANIZATION_ROLE_ADMIN, VENTURE_ORGANIZATION_ROLE_FINANCE };
	VentureDatabase *database = venture_action_get_data(action);
	VentureAccessPolicy *policy = venture_database_get_access_policy(database);
	const VentureAuthPrincipal *principal = venture_access_policy_get_actor(policy);
	(void)actor;
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "stripe_payment_link") == G_TYPE_INVALID)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Stripe module is disabled");
		return FALSE;
	}
	if (principal && !venture_access_policy_has_organization_role(policy, principal,
		venture_entity_get_organization_id(entity), roles, G_N_ELEMENTS(roles)))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED, "Organization finance authorization is required");
		return FALSE;
	}
	return TRUE;
}

static VentureEntity *
stripe_action_invoke(VentureAction *action, VentureEntity *entity, GHashTable *params,
	const VentureActor *actor, GError **error)
{
	VentureDatabase *database = venture_action_get_data(action);
	GWeakRef *reference = g_object_get_data(G_OBJECT(database), "venture-stripe-action-context");
	g_autoptr(VentureContext) context = reference ? g_weak_ref_get(reference) : NULL;
	g_autoptr(VentureStripeService) configured = NULL;
	VentureStripeService *service = context ? venture_context_get_stripe_service(context) : NULL;
	g_autofree gchar *name = NULL;
	gint64 organization = venture_entity_get_organization_id(entity), binding = 0;
	g_object_get(action, "name", &name, NULL);
	if (g_object_class_find_property(G_OBJECT_GET_CLASS(entity), "connection-id"))
		g_object_get(entity, "connection-id", &binding, NULL);
	if (!service)
	{
		configured = binding ? venture_stripe_service_for_connection(database, organization, binding, TRUE, NULL, error) :
			venture_stripe_service_for_organization(database, organization, NULL, error);
		service = configured;
	}
	if (!service) return NULL;
	if (!g_strcmp0(name, "payment_link"))
	{
		g_autofree gchar *base = NULL;
		g_autoptr(GDateTime) expires = NULL;
		JsonNode *date = g_hash_table_lookup(params, "expires_at");
		if (date && !JSON_NODE_HOLDS_NULL(date))
		{
			expires = venture_time_from_string(json_node_get_string(date), error);
			if (!expires) return NULL;
		}
		if (context) g_object_get(venture_context_get_config(context), "server-base-url", &base, NULL);
		return VENTURE_ENTITY(venture_stripe_service_create_payment_link(service,
			venture_entity_get_id(entity), base, expires, actor, error));
	}
	if (!g_strcmp0(name, "revoke"))
		return venture_stripe_service_revoke_payment_link(service, venture_entity_get_id(entity), actor, error) ?
			venture_database_get(database, VENTURE_TYPE_STRIPE_PAYMENT_LINK, venture_entity_get_id(entity), error) : NULL;
	if (!g_strcmp0(name, "retry"))
	{
		JsonNode *accept = g_hash_table_lookup(params, "accept_balance_change");
		return venture_stripe_service_retry_event(service, venture_entity_get_id(entity),
			accept && json_node_get_boolean(accept), actor, error) ?
			venture_database_get(database, VENTURE_TYPE_STRIPE_EVENT, venture_entity_get_id(entity), error) : NULL;
	}
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Unknown Stripe action");
	return NULL;
}

void
venture_stripe_actions_register(VentureDatabase *database)
{
	static const gchar *const types[] = { "invoice", "stripe_payment_link", "stripe_event" };
	static const gchar *const names[] = { "payment_link", "revoke", "retry" };
	static const gchar *const labels[] = { "Create payment link", "Revoke payment link", "Retry verified event" };
	static const gchar *const descriptions[] = {
		"Replace the invoice payment capability; copy its URL once from the result",
		"Revoke access and expire an open Checkout; processing bank payments remain pending",
		"Replay retained signed evidence through accounting guards; never alter the provider amount or date" };
	guint i;
	for (i = 0; i < G_N_ELEMENTS(types); i++)
	{
		g_autoptr(GPtrArray) parameters = g_ptr_array_new_with_free_func((GDestroyNotify)venture_field_spec_free);
		g_autoptr(VentureAction) action = NULL;
		g_autoptr(GError) error = NULL;
		if (i == 0) g_ptr_array_add(parameters, venture_field_spec_new("expires_at", "Expiry (default seven days)", VENTURE_FIELD_KIND_DATETIME));
		if (i == 2) g_ptr_array_add(parameters, venture_field_spec_new("accept_balance_change", "Accept original amount with excess as customer credit", VENTURE_FIELD_KIND_BOOLEAN));
		action = g_object_new(VENTURE_TYPE_ACTION, "type-name", types[i], "name", names[i],
			"label", labels[i], "description", descriptions[i], "parameters", parameters,
			"stageable", FALSE, "service-transaction", TRUE, "roles", VENTURE_USER_ROLE_EDITOR, NULL);
		if (!venture_action_registry_register(venture_database_get_action_registry(database), action,
			stripe_action_allowed, stripe_action_invoke, database, NULL, &error))
			g_error("Stripe action registration: %s", error->message);
	}
}
