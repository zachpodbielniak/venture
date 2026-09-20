/* SPDX-License-Identifier: AGPL-3.0-or-later */
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
	gchar *customer_id = NULL;
	if (!customer) return NULL;
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
	resource = stripe_client_execute(self->client, request, NULL, error);
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
