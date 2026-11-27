/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * One Stripe customer's Checkouts, and the customer itself, are made one
 * at a time. A request that waits on Stripe yields to the others
 * (core/venture-fiber.c), so two clicks of one Pay now link, or two of a
 * new customer's invoices paid at once, would otherwise both be part-way
 * through: the second reusing the first one's idempotency key while it
 * is still in flight, which Stripe refuses, or saving a second link to
 * the customer the first one is creating. Held by the paying party, the
 * one thing both cases share: the second waits for the first and then
 * finds its session, or its customer, already there.
 *
 * Not taken with a transaction open (a fiber does not yield inside one),
 * nor by a plain stack while a fiber holds it (a plain stack never waits);
 * the caller then goes on as before.
 */
/* A fiber handed the hold is resumed only outside a transaction: a nested
 * loop elsewhere may be dispatching it, inside its caller's. */
static gboolean
stripe_hold_ready(gpointer database)
{
	return !venture_database_has_transaction(database);
}

static VentureFiberHold *
stripe_party_hold(VentureStripeService *self, gint64 company_id, gint64 contact_id)
{
	g_autofree gchar *key = NULL;
	if ((!company_id && !contact_id) || venture_database_has_transaction(self->database)) return NULL;
	key = g_strdup_printf("venture-stripe-party:%" G_GINT64_FORMAT ":%s:%" G_GINT64_FORMAT, self->organization_id,
		company_id ? "company" : "contact", company_id ? company_id : contact_id);
	return venture_fiber_hold(key, stripe_hold_ready, self->database);
}

/* The same, for the party @invoice_id bills; none for an unreadable one,
 * which the caller then refuses. */
static VentureFiberHold *
stripe_invoice_hold(VentureStripeService *self, gint64 invoice_id)
{
	g_autoptr(VentureEntity) invoice = NULL;
	gint64 company_id = 0, contact_id = 0;
	if (venture_database_has_transaction(self->database)) return NULL;
	invoice = venture_database_get(self->database, VENTURE_TYPE_INVOICE, invoice_id, NULL);
	if (!invoice) return NULL;
	g_object_get(invoice, "company-id", &company_id, "contact-id", &contact_id, NULL);
	return stripe_party_hold(self, company_id, contact_id);
}

static gchar *
stripe_customer(VentureStripeService *self, gint64 company_id, gint64 contact_id,
	const VentureActor *actor, GError **error)
{
	gint64 party_id = company_id ? company_id : contact_id;
	const gchar *field = company_id ? "company-id" : "contact-id";
	g_autoptr(VentureEntity) customer = owned_get(self,
		company_id ? VENTURE_TYPE_COMPANY : VENTURE_TYPE_CONTACT, party_id, error);
	g_autoptr(GPtrArray) links = NULL;
	g_autoptr(StripeRequest) request = NULL;
	g_autoptr(StripeResource) resource = NULL;
	g_autoptr(VentureStripeCustomerLink) link = NULL;
	g_autoptr(VentureFiberHold) hold = NULL;
	gchar *customer_id = NULL;
	if (!customer) return NULL;
	hold = stripe_party_hold(self, company_id, contact_id);
	links = find_id(self, VENTURE_TYPE_STRIPE_CUSTOMER_LINK, field, party_id, error);
	if (!links) return NULL;
	if (links->len)
	{
		g_object_get(g_ptr_array_index(links, 0), "stripe-customer-id", &customer_id, NULL);
		return customer_id;
	}
	request = stripe_request_new(STRIPE_CUSTOMER_CREATE);
	request->name = venture_entity_get_display_name(customer);
	request->metadata_key = g_strdup(company_id ? "venture_company_uuid" : "venture_contact_uuid");
	request->metadata_value = g_strdup(venture_entity_get_uuid(customer));
	g_free(request->idempotency_key);
	request->idempotency_key = g_strdup_printf("venture-customer-%s-%s",
		self->connection_uuid ? self->connection_uuid : "fixture", venture_entity_get_uuid(customer));
	resource = venture_stripe_execute(self->client, request, NULL, error);
	if (!resource) return NULL;
	customer_id = g_strdup(stripe_resource_get_string(resource, "id"));
	if (!customer_id || !g_str_has_prefix(customer_id, "cus_"))
	{
		g_free(customer_id);
		refuse(error, "Provider returned no customer identity"); return NULL;
	}
	link = venture_stripe_customer_link_new();
	g_object_set(link, field, party_id, "stripe-customer-id", customer_id, "connection-id", self->connection_id, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(link), self->organization_id);
	if (!venture_stripe_save_owned(self->database, VENTURE_ENTITY(link), actor, error))
	{
		g_free(customer_id); return NULL;
	}
	return customer_id;
}
