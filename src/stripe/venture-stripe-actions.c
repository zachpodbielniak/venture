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
	if (!venture_action_require_organization(action, entity, database, error)) return FALSE;
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
	if (!g_strcmp0(name, "authorize_payment"))
	{
		g_autoptr(VentureMoney) limit = venture_money_from_json(g_hash_table_lookup(params, "limit"), NULL, error);
		if (!limit) return NULL;
		return VENTURE_ENTITY(venture_stripe_service_authorize_subscription(service, venture_entity_get_id(entity), limit, actor, error));
	}
	if (!g_strcmp0(name, "verify") || !g_strcmp0(name, "revoke_authorization"))
	{
		gboolean ok = !g_strcmp0(name, "verify") ?
			venture_stripe_service_verify_authorization(service, venture_entity_get_id(entity), actor, error) :
			venture_stripe_service_revoke_authorization(service, venture_entity_get_id(entity), actor, error);
		return ok ? venture_database_get(database, VENTURE_TYPE_STRIPE_AUTHORIZATION, venture_entity_get_id(entity), error) : NULL;
	}
	if (!g_strcmp0(name, "collect"))
		return VENTURE_ENTITY(venture_stripe_service_collect_invoice(service, venture_entity_get_id(entity), actor, error));
	if (!g_strcmp0(name, "cancel_collection"))
		return venture_stripe_service_cancel_collection(service, venture_entity_get_id(entity), actor, error) ?
			venture_database_get(database, VENTURE_TYPE_STRIPE_CHECKOUT, venture_entity_get_id(entity), error) : NULL;
	if (!g_strcmp0(name, "reconcile_collection"))
		return VENTURE_ENTITY(venture_stripe_service_reconcile_collection(service, venture_entity_get_id(entity),
			json_node_get_string(g_hash_table_lookup(params, "provider_invoice_id")), actor, error));
	if (!g_strcmp0(name, "retry_collection") || !g_strcmp0(name, "collect_due"))
	{
		JsonNode *clock = g_hash_table_lookup(params, "now");
		g_autoptr(GDateTime) now = clock && !JSON_NODE_HOLDS_NULL(clock) ?
			venture_time_from_string(json_node_get_string(clock), error) : venture_time_now();
		if (!now) return NULL;
		if (!g_strcmp0(name, "retry_collection"))
			return VENTURE_ENTITY(venture_stripe_service_retry_collection(service, venture_entity_get_id(entity), now, actor, error));
		else
		{
			JsonNode *limit_node = g_hash_table_lookup(params, "limit");
			gint64 limit = limit_node ? json_node_get_int(limit_node) : 10;
			gint count;
			g_autoptr(VentureStripeAuthorization) result = NULL;
			g_autofree gchar *note = NULL;
			if (limit < 1 || limit > 100)
			{
				g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Collection limit must be from 1 to 100"); return NULL;
			}
			count = venture_stripe_service_collect_due(service, now, (guint)limit, actor, error);
			if (count < 0) return NULL;
			result = venture_stripe_authorization_new();
			venture_entity_set_organization_id(VENTURE_ENTITY(result), organization);
			note = g_strdup_printf("Examined %d due authorizations; inspect their collection outcomes and retained attempts", count);
			g_object_set(result, "status", "sweep complete", "collection-note", note, NULL);
			return VENTURE_ENTITY(g_steal_pointer(&result));
		}
	}
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Unknown Stripe action");
	return NULL;
}

void
venture_stripe_actions_register(VentureDatabase *database)
{
	static const gchar *const types[] = { "invoice", "stripe_payment_link", "stripe_event",
		"customer_subscription", "stripe_authorization", "stripe_authorization", "invoice",
		"stripe_checkout", "stripe_checkout", "stripe_checkout", "stripe_authorization" };
	static const gchar *const names[] = { "payment_link", "revoke", "retry", "authorize_payment", "verify",
		"revoke_authorization", "collect", "cancel_collection", "retry_collection", "reconcile_collection", "collect_due" };
	static const gchar *const labels[] = { "Create payment link", "Revoke payment link", "Retry verified event",
		"Authorize recurring payment", "Verify hosted authorization", "Revoke recurring permission", "Collect authorized invoice",
		"Cancel automatic collection", "Retry due collection", "Reconcile provider identity", "Check due recurring collections" };
	static const gchar *const descriptions[] = {
		"Replace the invoice payment capability; copy its URL once from the result",
		"Revoke access and expire an open Checkout; processing bank payments remain pending",
		"Replay retained signed evidence through accounting guards; never alter the provider amount or date",
		"Send hosted Setup with an exact per-invoice maximum; copy its URL once",
		"Verify the original account's successful hosted Setup; never enter a payment-method ID manually",
		"Stop future automatic collection while preserving historical settlement evidence",
		"Reserve one invoice-wide attempt and request payment using verified reusable permission",
		"Confirm zero received money and provider cancellation; processing payments remain reserved",
		"After the recorded delay, cancel the old failed invoice and reserve the next bounded attempt",
		"Verify opaque provider correlation after an uncertain create; then cancel or retry retained signed evidence",
		"Bounded organization sweep of verified permissions; outcomes remain on each permission" };
	guint i;
	for (i = 0; i < G_N_ELEMENTS(types); i++)
	{
		g_autoptr(GPtrArray) parameters = g_ptr_array_new_with_free_func((GDestroyNotify)venture_field_spec_free);
		g_autoptr(VentureAction) action = NULL;
		g_autoptr(GError) error = NULL;
		if (i == 0) g_ptr_array_add(parameters, venture_field_spec_new("expires_at", "Expiry (default seven days)", VENTURE_FIELD_KIND_DATETIME));
		if (i == 2) g_ptr_array_add(parameters, venture_field_spec_new("accept_balance_change", "Accept original amount with excess as customer credit", VENTURE_FIELD_KIND_BOOLEAN));
		if (i == 3 || i == 9 || i == 10)
		{
			VentureFieldSpec *required = venture_field_spec_new(i == 3 ? "limit" : i == 9 ? "provider_invoice_id" : "organization_id",
				i == 3 ? "Maximum per invoice, including currency" : i == 9 ? "Stripe invoice identity" : "Organization",
				i == 3 ? VENTURE_FIELD_KIND_MONEY : i == 9 ? VENTURE_FIELD_KIND_STRING : VENTURE_FIELD_KIND_INTEGER);
			required->required = TRUE; g_ptr_array_add(parameters, required);
		}
		if (i == 8 || i == 10) g_ptr_array_add(parameters, venture_field_spec_new("now", "Scheduling clock (default now)", VENTURE_FIELD_KIND_DATETIME));
		if (i == 10) g_ptr_array_add(parameters, venture_field_spec_new("limit", "Maximum permissions (default 10)", VENTURE_FIELD_KIND_INTEGER));
		action = g_object_new(VENTURE_TYPE_ACTION, "type-name", types[i], "name", names[i],
			"label", labels[i], "description", descriptions[i], "parameters", parameters,
			"stageable", FALSE, "service-transaction", TRUE, "type-level", i == 10, "roles", VENTURE_USER_ROLE_EDITOR, NULL);
		if (!venture_action_registry_register(venture_database_get_action_registry(database), action,
			stripe_action_allowed, stripe_action_invoke, database, NULL, &error))
			g_error("Stripe action registration: %s", error->message);
	}
}
