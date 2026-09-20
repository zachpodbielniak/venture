/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <errno.h>
#include <sys/random.h>

static gboolean
stripe_finance(VentureStripeService *self, GError **error)
{
	static const gint roles[] = { VENTURE_ORGANIZATION_ROLE_OWNER,
		VENTURE_ORGANIZATION_ROLE_ADMIN, VENTURE_ORGANIZATION_ROLE_FINANCE };
	VentureAccessPolicy *policy = venture_database_get_access_policy(self->database);
	const VentureAuthPrincipal *principal = venture_access_policy_get_actor(policy);
	if (principal && !venture_access_policy_has_organization_role(policy, principal,
		self->organization_id, roles, G_N_ELEMENTS(roles)))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED,
			"Organization finance authorization is required");
		return FALSE;
	}
	return TRUE;
}

static gchar *
stripe_link_nonce(GError **error)
{
	guchar bytes[32];
	gchar *result;
	gsize used = 0;
	guint i;
	while (used < sizeof(bytes))
	{
		ssize_t n = getrandom(bytes + used, sizeof(bytes) - used, 0);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) { refuse(error, "Secure random capability generation failed"); return NULL; }
		used += (gsize)n;
	}
	result = g_malloc(65);
	for (i = 0; i < sizeof(bytes); i++) g_snprintf(result + 2 * i, 3, "%02x", bytes[i]);
	return result;
}

VentureStripePaymentLink *
venture_stripe_service_resolve_payment_link(VentureDatabase *database,
	const gchar *token, GDateTime *clock, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_STRIPE_PAYMENT_LINK);
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(VentureEntity) invoice = NULL, connection = NULL;
	g_autoptr(GDateTime) now = clock ? g_date_time_ref(clock) : venture_time_now();
	g_autoptr(GDateTime) expires = NULL;
	g_autofree gchar *uuid = NULL, *digest = NULL, *stored = NULL, *provider = NULL;
	VentureStripePaymentLink *link;
	gint64 invoice_id, connection_id, version, organization;
	gint status;
	gboolean enabled, connected;
	guint i;
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "stripe_payment_link") == G_TYPE_INVALID)
		goto unavailable;
	if (!token || strlen(token) != 101 || token[36] != '.') goto unavailable;
	uuid = g_strndup(token, 36);
	if (!g_uuid_string_is_valid(uuid)) goto unavailable;
	for (i = 37; i < 101; i++) if (!g_ascii_isxdigit(token[i])) goto unavailable;
	if (!venture_query_add_filter_string(query, "uuid", VENTURE_FILTER_OP_EQ, uuid, error)) return NULL;
	rows = venture_database_find(database, query, error);
	if (!rows) return NULL;
	if (rows->len != 1) goto unavailable;
	link = g_ptr_array_index(rows, 0);
	g_object_get(link, "token-hash", &stored, "expires-at", &expires, "enabled", &enabled,
		"invoice-id", &invoice_id, "connection-id", &connection_id, "invoice-version", &version, NULL);
	digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, token, -1);
	if (!stored || !venture_constant_time_equal(stored, digest) || !enabled || !expires ||
		g_date_time_compare(now, expires) >= 0 || venture_entity_is_deleted(VENTURE_ENTITY(link))) goto unavailable;
	organization = venture_entity_get_organization_id(VENTURE_ENTITY(link));
	invoice = venture_database_get(database, VENTURE_TYPE_INVOICE, invoice_id, error);
	if (!invoice) return NULL;
	connection = venture_database_get(database, VENTURE_TYPE_INTEGRATION_CONNECTION, connection_id, error);
	if (!connection) return NULL;
	g_object_get(invoice, "status", &status, NULL);
	g_object_get(connection, "enabled", &connected, "provider", &provider, NULL);
	if (!connected || g_strcmp0(provider, "stripe") || venture_entity_is_deleted(connection) ||
		venture_entity_get_organization_id(connection) != organization || venture_entity_is_deleted(invoice) ||
		venture_entity_get_organization_id(invoice) != organization || venture_entity_get_version(invoice) != version ||
		(status != VENTURE_INVOICE_STATUS_SENT && status != VENTURE_INVOICE_STATUS_PARTIALLY_PAID)) goto unavailable;
	return g_object_ref(link);
unavailable:
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Payment link is unavailable");
	return NULL;
}

gboolean
venture_stripe_service_revoke_payment_link(VentureStripeService *self,
	gint64 link_id, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) link = NULL;
	g_autoptr(VentureEntity) checkout = NULL;
	g_autoptr(StripeRequest) request = NULL;
	g_autoptr(StripeResource) response = NULL;
	g_autofree gchar *state = NULL, *session = NULL;
	gint64 checkout_id;
	if (!stripe_finance(self, error) || !stripe_refresh(self, TRUE, error)) return FALSE;
	if (venture_database_has_transaction(self->database))
		return refuse(error, "Payment link revocation cannot be nested in another transaction");
	link = owned_get(self, VENTURE_TYPE_STRIPE_PAYMENT_LINK, link_id, error);
	if (!link || !venture_access_policy_check_write(venture_database_get_access_policy(self->database), link, "write", error)) return FALSE;
	g_object_set(link, "enabled", FALSE, NULL);
	if (!venture_stripe_save_owned(self->database, link, actor, error)) return FALSE;
	g_object_get(link, "checkout-id", &checkout_id, NULL);
	if (!checkout_id) return TRUE;
	checkout = owned_get(self, VENTURE_TYPE_STRIPE_CHECKOUT, checkout_id, error);
	if (!checkout) return FALSE;
	g_object_get(checkout, "status", &state, "session-id", &session, NULL);
	if (!g_strcmp0(state, "initiated"))
		return refuse(error, "Link revoked; uncertain provider creation must be reconciled before replacement");
	if (g_strcmp0(state, "open")) return TRUE;
	request = stripe_request_new(STRIPE_CHECKOUT_EXPIRE);
	request->id = g_strdup(session);
	g_free(request->idempotency_key);
	request->idempotency_key = g_strdup_printf("venture-expire-%s", venture_entity_get_uuid(checkout));
	response = stripe_client_execute(self->client, request, NULL, error);
	if (!response) return FALSE;
	if (g_strcmp0(stripe_resource_get_string(response, "id"), session) ||
		g_strcmp0(stripe_resource_get_string(response, "status"), "expired"))
		return refuse(error, "Stripe did not confirm expiration; retain the pending attempt");
	g_clear_object(&checkout);
	checkout = owned_get(self, VENTURE_TYPE_STRIPE_CHECKOUT, checkout_id, error);
	if (!checkout) return FALSE;
	g_clear_pointer(&state, g_free);
	g_object_get(checkout, "status", &state, NULL);
	/* A concurrently completed debit has stronger evidence than expiry. */
	if (!g_strcmp0(state, "open"))
	{
		g_object_set(checkout, "status", "expired", "active", FALSE, NULL);
		if (!venture_stripe_save_owned(self->database, checkout, actor, error)) return FALSE;
	}
	return TRUE;
}

VentureStripePaymentLink *
venture_stripe_service_create_payment_link(VentureStripeService *self,
	gint64 invoice_id, const gchar *base_url, GDateTime *deadline,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureStripePaymentLink) link = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GDateTime) expires = deadline ? g_date_time_ref(deadline) : g_date_time_add_days(now, 7);
	g_autoptr(GUri) origin = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_STRIPE_PAYMENT_LINK);
	g_autoptr(GPtrArray) previous = NULL;
	g_autofree gchar *nonce = NULL, *token = NULL, *digest = NULL, *url = NULL;
	guint i;
	if (!stripe_finance(self, error) || !eligible(self, invoice_id, &invoice, NULL, NULL, NULL, error)) return NULL;
	if (!self->connection_id) { refuse(error, "Payment links require a configured organization account"); return NULL; }
	if (!venture_access_policy_check_write(venture_database_get_access_policy(self->database), invoice, "write", error)) return NULL;
	origin = base_url ? g_uri_parse(base_url, G_URI_FLAGS_NONE, NULL) : NULL;
	if (!origin || strlen(base_url) > 2048 || g_strcmp0(g_uri_get_scheme(origin), "https") || !g_uri_get_host(origin) ||
		g_uri_get_userinfo(origin) || g_uri_get_query(origin) || g_uri_get_fragment(origin) ||
		(g_uri_get_path(origin) && *g_uri_get_path(origin) && strcmp(g_uri_get_path(origin), "/")) ||
		g_date_time_difference(expires, now) < G_TIME_SPAN_HOUR ||
		g_date_time_difference(expires, now) > 30 * G_TIME_SPAN_DAY)
	{ refuse(error, "Payment links require a trusted HTTPS origin and expiry from one hour through thirty days"); return NULL; }
	if (venture_database_has_transaction(self->database))
	{ refuse(error, "Payment link replacement cannot be nested in another transaction"); return NULL; }
	venture_query_set_limit(query, 0);
	if (!venture_query_add_filter_int(query, "organization-id", VENTURE_FILTER_OP_EQ, self->organization_id, error) ||
		!venture_query_add_filter_int(query, "invoice-id", VENTURE_FILTER_OP_EQ, invoice_id, error)) return NULL;
	previous = venture_database_find(self->database, query, error);
	if (!previous) return NULL;
	for (i = 0; i < previous->len; i++)
	{
		VentureEntity *old = g_ptr_array_index(previous, i);
		gint64 binding, checkout_id;
		gboolean enabled, pending = FALSE;
		g_object_get(old, "connection-id", &binding, "checkout-id", &checkout_id, "enabled", &enabled, NULL);
		if (checkout_id)
		{
			g_autoptr(VentureEntity) attempt = venture_database_get(self->database, VENTURE_TYPE_STRIPE_CHECKOUT, checkout_id, error);
			g_autofree gchar *state = NULL;
			if (!attempt) return NULL;
			g_object_get(attempt, "status", &state, NULL);
			pending = checkout_pending(state);
		}
		if (!enabled && !pending) continue;
		if (binding != self->connection_id)
		{ refuse(error, "Revoke the historical account's payment link before replacement"); return NULL; }
		if (!venture_stripe_service_revoke_payment_link(self, venture_entity_get_id(old), actor, error)) return NULL;
	}
	nonce = stripe_link_nonce(error);
	if (!nonce) return NULL;
	link = venture_stripe_payment_link_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(link), self->organization_id);
	token = g_strdup_printf("%s.%s", venture_entity_get_uuid(VENTURE_ENTITY(link)), nonce);
	digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, token, -1);
	url = g_strdup_printf("%.*s/pay/%s", (gint)(strlen(base_url) - (g_str_has_suffix(base_url, "/") ? 1 : 0)), base_url, token);
	g_object_set(link, "connection-id", self->connection_id, "invoice-id", invoice_id,
		"invoice-version", venture_entity_get_version(invoice), "token-hash", digest,
		"expires-at", expires, "enabled", TRUE, "url", url, NULL);
	if (!venture_stripe_save_owned(self->database, VENTURE_ENTITY(link), actor, error)) return NULL;
	return g_steal_pointer(&link);
}

VentureStripeCheckout *
venture_stripe_service_pay_link(VentureStripeService *self,
	const gchar *token, GDateTime *clock, GError **error)
{
	g_autoptr(VentureStripePaymentLink) link = NULL;
	g_autoptr(VentureStripeCheckout) checkout = NULL;
	g_autoptr(GDateTime) now = clock ? g_date_time_ref(clock) : venture_time_now();
	g_autoptr(GDateTime) expires = NULL;
	g_autoptr(GDateTime) maximum = g_date_time_add_hours(now, 23);
	gint64 invoice_id, binding;
	link = venture_stripe_service_resolve_payment_link(self->database, token, now, error);
	if (!link) return NULL;
	g_object_get(link, "connection-id", &binding, "invoice-id", &invoice_id, "expires-at", &expires, NULL);
	if (binding != self->connection_id || venture_entity_get_organization_id(VENTURE_ENTITY(link)) != self->organization_id)
	{ refuse(error, "Payment link belongs to another Stripe account"); return NULL; }
	checkout = stripe_checkout_create(self, invoice_id,
		g_date_time_compare(expires, maximum) < 0 ? expires : maximum, link, NULL, error);
	if (!checkout) return NULL;
	return g_steal_pointer(&checkout);
}
