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

/* The capability alone: format, constant-time digest, enabled, unexpired,
 * not deleted. Says nothing about whether its invoice can be paid. On
 * success *@link_out and *@invoice_out are owned by the caller; a link that
 * does not authenticate is %FALSE with no error. */
static gboolean
link_authentic(VentureDatabase *database, const gchar *token, GDateTime *now,
	VentureEntity **link_out, VentureEntity **invoice_out, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_STRIPE_PAYMENT_LINK);
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GDateTime) expires = NULL;
	g_autofree gchar *uuid = NULL, *digest = NULL, *stored = NULL;
	VentureEntity *link;
	gint64 invoice_id;
	gboolean enabled;
	guint i;
	*link_out = NULL;
	*invoice_out = NULL;
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "stripe_payment_link") == G_TYPE_INVALID)
		return FALSE;
	if (!token || strlen(token) != 101 || token[36] != '.') return FALSE;
	uuid = g_strndup(token, 36);
	if (!g_uuid_string_is_valid(uuid)) return FALSE;
	for (i = 37; i < 101; i++) if (!g_ascii_isxdigit(token[i])) return FALSE;
	if (!venture_query_add_filter_string(query, "uuid", VENTURE_FILTER_OP_EQ, uuid, error)) return FALSE;
	rows = venture_database_find(database, query, error);
	if (!rows || rows->len != 1) return FALSE;
	link = g_ptr_array_index(rows, 0);
	g_object_get(link, "token-hash", &stored, "expires-at", &expires, "enabled", &enabled,
		"invoice-id", &invoice_id, NULL);
	digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, token, -1);
	if (!stored || !venture_constant_time_equal(stored, digest) || !enabled || !expires ||
		g_date_time_compare(now, expires) >= 0 || venture_entity_is_deleted(link)) return FALSE;
	*invoice_out = venture_database_get(database, VENTURE_TYPE_INVOICE, invoice_id, error);
	if (!*invoice_out) return FALSE;
	if (venture_entity_is_deleted(*invoice_out) ||
		venture_entity_get_organization_id(*invoice_out) != venture_entity_get_organization_id(link))
	{
		g_clear_object(invoice_out);
		return FALSE;
	}
	*link_out = g_object_ref(link);
	return TRUE;
}

const gchar *
venture_stripe_service_payment_link_closed(VentureDatabase *database, const gchar *token, GDateTime *clock)
{
	g_autoptr(VentureEntity) link = NULL, invoice = NULL;
	g_autoptr(GDateTime) now = clock ? g_date_time_ref(clock) : venture_time_now();
	gint status;
	/* Only the holder of a live capability learns the invoice's state. */
	if (!link_authentic(database, token, now, &link, &invoice, NULL)) return NULL;
	g_object_get(invoice, "status", &status, NULL);
	if (status == VENTURE_INVOICE_STATUS_PAID) return "paid";
	if (status == VENTURE_INVOICE_STATUS_VOID) return "void";
	return NULL;
}

VentureStripePaymentLink *
venture_stripe_service_resolve_payment_link(VentureDatabase *database,
	const gchar *token, GDateTime *clock, GError **error)
{
	g_autoptr(VentureEntity) invoice = NULL, connection = NULL, found = NULL;
	g_autoptr(GDateTime) now = clock ? g_date_time_ref(clock) : venture_time_now();
	g_autofree gchar *provider = NULL;
	VentureEntity *link;
	gint64 connection_id, version, organization;
	gint status;
	gboolean connected;
	if (!link_authentic(database, token, now, &found, &invoice, error))
	{
		if (error && *error) return NULL;
		goto unavailable;
	}
	link = found;
	g_object_get(link, "connection-id", &connection_id, "invoice-version", &version, NULL);
	organization = venture_entity_get_organization_id(VENTURE_ENTITY(link));
	connection = venture_database_get(database, VENTURE_TYPE_INTEGRATION_CONNECTION, connection_id, error);
	if (!connection) return NULL;
	g_object_get(invoice, "status", &status, NULL);
	g_object_get(connection, "enabled", &connected, "provider", &provider, NULL);
	if (!connected || g_strcmp0(provider, "stripe") || venture_entity_is_deleted(connection) ||
		venture_entity_get_organization_id(connection) != organization || venture_entity_get_version(invoice) != version ||
		(status != VENTURE_INVOICE_STATUS_SENT && status != VENTURE_INVOICE_STATUS_PARTIALLY_PAID)) goto unavailable;
	return VENTURE_STRIPE_PAYMENT_LINK(g_steal_pointer(&found));
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
	response = venture_stripe_execute(self->client, request, NULL, error);
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

/* A trusted HTTPS origin with nothing after it, and a lifetime from one
 * hour through thirty days. */
static gboolean
link_terms_valid(const gchar *base_url, GDateTime *now, GDateTime *expires)
{
	g_autoptr(GUri) origin = base_url ? g_uri_parse(base_url, G_URI_FLAGS_NONE, NULL) : NULL;
	return origin && strlen(base_url) <= 2048 && !g_strcmp0(g_uri_get_scheme(origin), "https") &&
		g_uri_get_host(origin) && !g_uri_get_userinfo(origin) && !g_uri_get_query(origin) &&
		!g_uri_get_fragment(origin) &&
		(!g_uri_get_path(origin) || !*g_uri_get_path(origin) || !strcmp(g_uri_get_path(origin), "/")) &&
		g_date_time_difference(expires, now) >= G_TIME_SPAN_HOUR &&
		g_date_time_difference(expires, now) <= 30 * G_TIME_SPAN_DAY;
}

/* Writes one capability of @origin for @invoice as it is now. The caller
 * has checked the authority and the terms, and has disabled any enabled link
 * of the same origin; the bearer URL exists only in the returned object's
 * transient url property. */
static VentureStripePaymentLink *
link_mint(VentureStripeService *self, VentureEntity *invoice, const gchar *base_url,
	GDateTime *expires, VentureStripePaymentLinkOrigin origin, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureStripePaymentLink) link = NULL;
	g_autofree gchar *nonce = NULL, *token = NULL, *digest = NULL, *url = NULL;
	nonce = stripe_link_nonce(error);
	if (!nonce) return NULL;
	link = venture_stripe_payment_link_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(link), self->organization_id);
	token = g_strdup_printf("%s.%s", venture_entity_get_uuid(VENTURE_ENTITY(link)), nonce);
	digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, token, -1);
	url = g_strdup_printf("%.*s/pay/%s", (gint)(strlen(base_url) - (g_str_has_suffix(base_url, "/") ? 1 : 0)), base_url, token);
	g_object_set(link, "connection-id", self->connection_id, "invoice-id", venture_entity_get_id(invoice),
		"invoice-version", venture_entity_get_version(invoice), "token-hash", digest,
		"expires-at", expires, "enabled", TRUE, "origin", origin, "url", url, NULL);
	if (!venture_stripe_save_owned(self->database, VENTURE_ENTITY(link), actor, error)) return NULL;
	return g_steal_pointer(&link);
}

/* Disables every enabled emailed button of @invoice, so the one about to be
 * minted is the only one. Local only: Send runs in a transaction and may not
 * call Stripe. A Checkout session the old button already opened stays as it
 * is; the new button's click finds it pending and reuses it, and its
 * payment is recorded either way. Shared links are not touched. */
static gboolean
link_retire_email(VentureStripeService *self, VentureEntity *invoice,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_STRIPE_PAYMENT_LINK);
	g_autoptr(GPtrArray) previous = NULL;
	guint i;
	venture_query_set_limit(query, 0);
	if (!venture_query_add_filter_int(query, "organization-id", VENTURE_FILTER_OP_EQ, self->organization_id, error) ||
		!venture_query_add_filter_int(query, "invoice-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(invoice), error))
		return FALSE;
	previous = venture_database_find(self->database, query, error);
	if (!previous) return FALSE;
	for (i = 0; i < previous->len; i++)
	{
		VentureEntity *old = g_ptr_array_index(previous, i);
		gint origin;
		gboolean enabled;
		g_object_get(old, "origin", &origin, "enabled", &enabled, NULL);
		if (!enabled || origin != VENTURE_STRIPE_PAYMENT_LINK_ORIGIN_EMAIL) continue;
		g_object_set(old, "enabled", FALSE, NULL);
		if (!venture_stripe_save_owned(self->database, old, actor, error)) return FALSE;
	}
	return TRUE;
}

gchar *
venture_stripe_service_invoice_pay_url(VentureStripeService *self, VentureEntity *invoice,
	const gchar *base_url, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) current = NULL;
	g_autoptr(VentureStripePaymentLink) link = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GDateTime) expires = g_date_time_add_days(now, VENTURE_STRIPE_INVOICE_PAY_DAYS);
	g_autoptr(GError) reason = NULL;
	g_autofree gchar *url = NULL;
	gboolean own_transaction;
	g_return_val_if_fail(VENTURE_IS_STRIPE_SERVICE(self), NULL);
	g_return_val_if_fail(VENTURE_IS_ENTITY(invoice), NULL);
	/* No button is better than a button that fails: an invoice Checkout
	 * cannot take, no account, or no public origin sends the email
	 * without one. Sending the invoice is the authority to offer it. */
	if (!self->connection_id || venture_entity_get_organization_id(invoice) != self->organization_id ||
		!link_terms_valid(base_url, now, expires))
		return NULL;
	if (!eligible(self, venture_entity_get_id(invoice), &current, NULL, NULL, NULL, &reason))
	{
		g_debug("No Pay now link for invoice %" G_GINT64_FORMAT ": %s",
			venture_entity_get_id(invoice), reason ? reason->message : "not eligible");
		return NULL;
	}
	/* Sending is the authority to offer the button, but this function is
	 * public: it writes a capability, so it asks for write access itself
	 * and does not rely on its callers having done so. */
	if (!venture_access_policy_check_write(venture_database_get_access_policy(self->database), current, "write", error))
		return NULL;
	/* A second send replaces the emailed button rather than failing on
	 * the one-per-origin index. Retiring and minting are one change: in
	 * Send's transaction, or in one of our own when called outside one. */
	own_transaction = !venture_database_has_transaction(self->database);
	if (own_transaction && !venture_database_begin(self->database, error)) return NULL;
	if (!link_retire_email(self, current, actor, error) ||
		!(link = link_mint(self, current, base_url, expires, VENTURE_STRIPE_PAYMENT_LINK_ORIGIN_EMAIL, actor, error)))
	{
		if (own_transaction) venture_database_rollback(self->database);
		return NULL;
	}
	if (own_transaction && !venture_database_commit(self->database, error)) return NULL;
	g_object_get(link, "url", &url, NULL);
	return g_strconcat(url, "/checkout", NULL);
}

VentureStripePaymentLink *
venture_stripe_service_create_payment_link(VentureStripeService *self,
	gint64 invoice_id, const gchar *base_url, GDateTime *deadline,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GDateTime) expires = deadline ? g_date_time_ref(deadline) : g_date_time_add_days(now, 7);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_STRIPE_PAYMENT_LINK);
	g_autoptr(GPtrArray) previous = NULL;
	guint i;
	if (!stripe_finance(self, error) || !eligible(self, invoice_id, &invoice, NULL, NULL, NULL, error)) return NULL;
	if (!self->connection_id) { refuse(error, "Payment links require a configured organization account"); return NULL; }
	if (!venture_access_policy_check_write(venture_database_get_access_policy(self->database), invoice, "write", error)) return NULL;
	if (!link_terms_valid(base_url, now, expires))
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
		gint origin;
		g_object_get(old, "connection-id", &binding, "checkout-id", &checkout_id, "enabled", &enabled,
			"origin", &origin, NULL);
		/* The emailed button is the customer's, not the operator's to
		 * replace here: a shared link replaces only a shared link. A
		 * Checkout the button opened is the invoice's one active attempt,
		 * which the new link's click reuses. */
		if (origin == VENTURE_STRIPE_PAYMENT_LINK_ORIGIN_EMAIL) continue;
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
	return link_mint(self, invoice, base_url, expires, VENTURE_STRIPE_PAYMENT_LINK_ORIGIN_SHARED, actor, error);
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
		g_date_time_compare(expires, maximum) < 0 ? expires : maximum, link, FALSE, NULL, error);
	if (!checkout) return NULL;
	return g_steal_pointer(&checkout);
}
