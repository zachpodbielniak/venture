/* SPDX-License-Identifier: AGPL-3.0-or-later */

static void forms_payment_context_free(gpointer data)
{
	GWeakRef *reference = data;
	g_weak_ref_clear(reference); g_free(reference);
}

gboolean
venture_forms_check_write(VentureDatabase *database, VentureEntity *entity, gboolean removal, GError **error)
{
	(void)database;
	if (!VENTURE_IS_FORM_PAYMENT(entity)) return TRUE;
	if (removal || g_object_get_data(G_OBJECT(entity), VENTURE_FORMS_PAYMENT_WRITE) == NULL)
	{
		venture_set_error_validation(error, "Payment intake", "pending payment evidence belongs to the forms service; use owner erasure or recovery actions");
		return FALSE;
	}
	return TRUE;
}

static gboolean forms_payment_validate(VentureDatabase *database, VentureEntity *entity,
	VentureEntity *previous, gpointer data, GError **error)
{
	(void)database; (void)previous; (void)data;
	if (g_object_get_data(G_OBJECT(entity), VENTURE_FORMS_PAYMENT_WRITE) == NULL)
	{
		venture_set_error_validation(error, "Payment intake", "only the forms payment service may write pending responses");
		return FALSE;
	}
	return TRUE;
}

static gboolean forms_payment_save(VentureDatabase *database, VentureEntity *pending, GError **error)
{
	g_object_set_data(G_OBJECT(pending), VENTURE_FORMS_PAYMENT_WRITE, GINT_TO_POINTER(1));
	g_object_set(pending, "generation", venture_forms_get_int(pending, "generation") + 1, NULL);
	return venture_database_save(database, pending, NULL, error);
}

static VentureEntity *forms_payment_restore(VentureEntity *pending, GError **error)
{
	g_autoptr(VentureEntity) response = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autofree gchar *payload = venture_forms_get_string(pending, "payload");
	if (venture_string_is_empty(payload) || !json_parser_load_from_data(parser, payload, -1, error)) return NULL;
	response = VENTURE_ENTITY(venture_form_submission_new());
	if (!venture_serializable_from_json(VENTURE_SERIALIZABLE(response), json_parser_get_root(parser), error)) return NULL;
	/* This is the service's private retained payload, not public JSON. The
	 * generic decoder correctly refuses sensitive fields on every public path. */
	{
		VentureEntityClass *klass = VENTURE_ENTITY_GET_CLASS(response);
		guint n, i;
		g_autofree GParamSpec **properties = venture_entity_class_list_persistent_properties(klass, &n);
		JsonObject *object = json_node_get_object(json_parser_get_root(parser));
		for (i = 0; i < n; i++)
		{
			g_autofree gchar *member = NULL;
			g_auto(GValue) value = G_VALUE_INIT;
			JsonNode *node;
			if (!(venture_entity_class_get_column_flags(klass, properties[i]->name) & VENTURE_COLUMN_FLAG_SENSITIVE)) continue;
			member = venture_entity_property_to_column(properties[i]->name);
			node = json_object_get_member(object, member); if (node == NULL) continue;
			if (!venture_json_value_from_node(node, properties[i]->value_type, &value, error)) return NULL;
			g_object_set_property(G_OBJECT(response), properties[i]->name, &value);
		}
	}
	return g_steal_pointer(&response);
}

static GPtrArray *forms_payment_rows(VentureDatabase *database, VentureEntity *form, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_FORM_PAYMENT);
	venture_query_set_organization(query, venture_entity_get_organization_id(form));
	venture_query_add_filter_int(query, "form-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(form), NULL);
	venture_query_add_filter_string(query, "state", VENTURE_FILTER_OP_NE, "completed", NULL);
	venture_query_add_filter_string(query, "state", VENTURE_FILTER_OP_NE, "expired", NULL);
	venture_query_add_filter_string(query, "state", VENTURE_FILTER_OP_NE, "failed", NULL);
	venture_query_add_filter_string(query, "state", VENTURE_FILTER_OP_NE, "erased", NULL);
	venture_query_add_filter_string(query, "state", VENTURE_FILTER_OP_NE, "paid_needs_capacity", NULL);
	venture_query_set_limit(query, 10001);
	return venture_database_find(database, query, error);
}

static gboolean forms_payment_limits(VentureDatabase *database, VentureEntity *form,
	VentureEntity *response, JsonObject *refused, GError **error)
{
	g_autoptr(GPtrArray) rows = forms_payment_rows(database, form, error);
	g_autofree gchar *email_key = venture_forms_get_string(form, "unique-email-field");
	g_autofree gchar *email = venture_string_is_empty(email_key) ? NULL : forms_response_email(response, email_key);
	g_autofree gchar *personal = venture_forms_get_string(response, "personal-hash");
	gint64 limit = venture_forms_get_int(form, "response-limit"), contact = venture_forms_get_int(response, "contact-id");
	guint i;
	if (rows == NULL) return FALSE;
	if (rows->len > 10000 || (limit > 0 && forms_response_count(database, form) + rows->len >= limit))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Form not found"); return FALSE;
	}
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *pending = g_ptr_array_index(rows, i);
		g_autoptr(VentureEntity) retained = email != NULL ? forms_payment_restore(pending, error) : NULL;
		g_autofree gchar *prior_email = retained != NULL ? forms_response_email(retained, email_key) : NULL;
		if (email != NULL && retained == NULL) return FALSE;
		g_autofree gchar *prior_personal = venture_forms_get_string(pending, "personal-hash");
		if ((email != NULL && g_strcmp0(email, prior_email) == 0) ||
		    (contact > 0 && venture_forms_get_bool(form, "one-per-contact") && contact == venture_forms_get_int(pending, "contact-id")) ||
		    (!venture_string_is_empty(personal) && venture_forms_get_bool(form, "one-per-link") && g_strcmp0(personal, prior_personal) == 0))
		{
			forms_refuse(refused, !venture_string_is_empty(email_key) ? email_key : "_form", "A payment for this response is already pending.");
			return FALSE;
		}
	}
	return TRUE;
}

static VentureStripeService *forms_payment_provider(VentureDatabase *database, gint64 organization, GError **error)
{
	GWeakRef *reference = g_object_get_data(G_OBJECT(database), "venture-forms-payment-context");
	g_autoptr(VentureContext) context = reference != NULL ? g_weak_ref_get(reference) : NULL;
	VentureStripeService *service = context != NULL ? venture_context_get_stripe_service(context) : NULL;
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "stripe_checkout") == G_TYPE_INVALID)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Payment service unavailable"); return NULL;
	}
	return service != NULL ? g_object_ref(service) : venture_stripe_service_for_organization(database, organization, NULL, error);
}

static VentureEntity *forms_payment_invoice(VentureDatabase *database, VentureEntity *form,
	GPtrArray *fields, JsonObject *answers, JsonArray *lines, VentureMoney *total, GDateTime *now, GError **error)
{
	JsonObject *payment = venture_forms_payment(fields);
	const gchar *name = json_object_get_string_member_with_default(answers, json_object_get_string_member(payment, "name_field"), "");
	const gchar *email = json_object_get_string_member_with_default(answers, json_object_get_string_member(payment, "email_field"), "");
	gint64 organization = venture_entity_get_organization_id(form);
	g_autoptr(VentureEntity) company = g_object_new(VENTURE_TYPE_COMPANY, "organization-id", organization, "name", name, NULL);
	g_autoptr(VentureEntity) contact = g_object_new(VENTURE_TYPE_CONTACT, "organization-id", organization, "name", name, "email", email, NULL);
	g_autoptr(VentureEntity) invoice = g_object_new(VENTURE_TYPE_INVOICE, "organization-id", organization, "issued-at", now, "due-at", now, NULL);
	g_autofree gchar *number = g_strdup_printf("FORM-%s", venture_entity_get_uuid(invoice));
	guint i;
	g_object_set(invoice, "number", number, NULL);
	/* A stranger knowing an email address cannot select a previous customer's
	 * Stripe identity or expose its saved Checkout details. Each intake is a
	 * fresh guest purchase; staff may consolidate the CRM records afterward. */
	if (venture_string_is_empty(name) || strlen(name) > 200 || venture_string_is_empty(email) || strlen(email) > 254)
	{ venture_set_error_validation(error, "Payer", "a bounded name and email are required"); return NULL; }
	if (!venture_database_save(database, company, NULL, error)) return NULL;
	g_object_set(contact, "company-id", venture_entity_get_id(company), NULL);
	if (!venture_database_save(database, contact, NULL, error)) return NULL;
	g_object_set(invoice, "company-id", venture_entity_get_id(company), "contact-id", venture_entity_get_id(contact), NULL);
	if (!venture_database_save(database, invoice, NULL, error)) return NULL;
	for (i = 0; i < json_array_get_length(lines); i++)
	{
		JsonObject *priced = json_array_get_object_element(lines, i);
		g_autoptr(VentureMoney) amount = venture_money_from_json(json_object_get_member(priced, "amount"), NULL, error);
		g_autoptr(VentureEntity) line = g_object_new(VENTURE_TYPE_INVOICE_LINE, "organization-id", organization,
			"invoice-id", venture_entity_get_id(invoice), "product-id", json_object_get_int_member(priced, "product_id"),
			"description", json_object_get_string_member(priced, "description"), "quantity", 1.0, "unit-price", amount, NULL);
		if (amount == NULL || !venture_database_save(database, line, NULL, error)) return NULL;
	}
	g_object_set(invoice, "status", VENTURE_INVOICE_STATUS_SENT, NULL);
	if (!venture_database_save(database, invoice, NULL, error)) return NULL;
	{
		g_autoptr(VentureMoney) balance = venture_settlement_service_invoice_balance(venture_settlement_service_get(database), venture_entity_get_id(invoice), NULL, error);
		if (balance == NULL) return NULL;
		if (!venture_money_equal(balance, total))
		{ venture_set_error_validation(error, "Form invoice", "the invoice balance must equal the server-computed price"); return NULL; }
	}
	return g_steal_pointer(&invoice);
}

static gboolean forms_payment_checkout(VentureDatabase *database, VentureEntity *pending,
	VentureEntity *response, GError **error)
{
	g_autoptr(VentureStripeService) service = forms_payment_provider(database, venture_entity_get_organization_id(pending), error);
	g_autoptr(VentureStripeCheckout) checkout = NULL;
	g_autoptr(GDateTime) expires = NULL;
	g_autofree gchar *url = NULL;
	if (service == NULL) return FALSE;
	g_object_get(pending, "expires-at", &expires, NULL);
	checkout = venture_stripe_service_checkout_until(service, venture_forms_get_int(pending, "invoice-id"), expires,
		venture_forms_get_int(pending, "reservation-id") > 0, NULL, error);
	if (checkout == NULL) return FALSE;
	/* The provider call may run a nested main loop and a settlement webhook.
	 * Never overwrite the pending row retained by that callback. */
	{
		g_autoptr(VentureEntity) fresh = venture_database_get(database, VENTURE_TYPE_FORM_PAYMENT, venture_entity_get_id(pending), error);
		if (fresh == NULL) return FALSE;
		g_object_set(fresh, "checkout-id", venture_entity_get_id(VENTURE_ENTITY(checkout)), NULL);
		if (!forms_payment_save(database, fresh, error)) return FALSE;
	}
	g_object_get(checkout, "url", &url, NULL);
	if (venture_string_is_empty(url)) { venture_set_error_validation(error, "Checkout", "the retained payment attempt needs reconciliation"); return FALSE; }
	g_object_set_data_full(G_OBJECT(response), VENTURE_FORMS_PAYMENT_REDIRECT, g_steal_pointer(&url), g_free);
	return TRUE;
}

/* Keep the original nonce and answers on the rendered retry form. A generic
 * error page would encourage a reload and a second invoice after uncertainty. */
static gboolean forms_payment_continue(VentureDatabase *database, VentureEntity *pending,
	VentureEntity *response, JsonObject *refused)
{
	g_autoptr(GError) error = NULL;
	if (forms_payment_checkout(database, pending, response, &error)) return TRUE;
	forms_refuse(refused, "_form", "Payment needs attention. Retry this form without changing its answers, or contact us.");
	return FALSE;
}

static gboolean forms_payment_begin(VentureDatabase *database, VentureEntity *form, VentureEntity *version,
	VentureEntity *response, GPtrArray *fields, JsonObject *answers, const gchar *nonce,
	JsonArray *lines, VentureMoney *total, GDateTime *now, JsonObject *refused, GError **error)
{
	g_autoptr(VentureEntity) fresh = NULL, pending = NULL, invoice = NULL, hold = NULL;
	g_autoptr(VentureStripeService) provider = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_FORM_PAYMENT);
	g_autoptr(JsonNode) packed = venture_serializable_to_json(VENTURE_SERIALIZABLE(response), TRUE);
	g_autoptr(GDateTime) expires = g_date_time_add_minutes(now, 45);
	g_autofree gchar *nonce_hash = NULL, *payload = NULL, *answer_hash = NULL;
	g_autofree gchar *ordinary = venture_forms_get_string(response, "answers"), *secret = venture_forms_get_string(response, "sensitive-answers");
	g_autofree gchar *identity = NULL;
	if (!venture_forms_payment_nonce_valid(form, nonce))
	{ forms_refuse(refused, "_form", "Reload the form before starting payment."); return FALSE; }
	provider = forms_payment_provider(database, venture_entity_get_organization_id(form), error);
	if (provider == NULL) return FALSE;
	nonce_hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, nonce, -1);
	identity = g_strdup_printf("%" G_GINT64_FORMAT "\n%s\n%s", venture_entity_get_id(version), ordinary, secret != NULL ? secret : "");
	answer_hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, identity, -1);
	if (!venture_database_begin(database, error)) return FALSE;
	venture_query_set_organization(query, venture_entity_get_organization_id(form));
	venture_query_add_filter_int(query, "form-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(form), NULL);
	venture_query_add_filter_string(query, "nonce-hash", VENTURE_FILTER_OP_EQ, nonce_hash, NULL);
	pending = venture_database_find_one(database, query, error);
	if (error != NULL && *error != NULL) goto fail;
	if (pending != NULL)
	{
		g_autofree gchar *prior = venture_forms_get_string(pending, "answers-hash"), *state = venture_forms_get_string(pending, "state");
		if (g_strcmp0(prior, answer_hash) != 0 || g_strcmp0(state, "pending_payment") != 0)
		{ forms_refuse(refused, "_form", "This payment intake has already been used. Start a fresh form for a new order."); goto fail; }
		if (!venture_database_commit(database, error)) return FALSE;
		return forms_payment_continue(database, pending, response, refused);
	}
	fresh = venture_database_get(database, VENTURE_TYPE_FORM, venture_entity_get_id(form), error);
	if (fresh == NULL) goto fail;
	if (!forms_is_open(database, fresh, now))
	{ g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Form not found"); goto fail; }
	if (!venture_forms_personal_bind(database, fresh, response, now, refused, error) ||
	    !forms_check_email_limit(database, fresh, response, fields, refused, error) ||
	    !forms_payment_limits(database, fresh, response, refused, error)) goto fail;
	g_object_set_data(G_OBJECT(form), VENTURE_FORMS_PAYMENT_WRITE, GINT_TO_POINTER(1));
	if (!venture_forms_booking_prepare(database, form, fields, answers, now, &hold, refused, error))
	{ g_object_set_data(G_OBJECT(form), VENTURE_FORMS_PAYMENT_WRITE, NULL); goto fail; }
	g_object_set_data(G_OBJECT(form), VENTURE_FORMS_PAYMENT_WRITE, NULL);
	invoice = forms_payment_invoice(database, form, fields, answers, lines, total, now, error);
	if (invoice == NULL) goto fail;
	g_object_set(response, "invoice-id", venture_entity_get_id(invoice), NULL);
	g_clear_pointer(&packed, json_node_unref); packed = venture_serializable_to_json(VENTURE_SERIALIZABLE(response), TRUE);
	payload = json_to_string(packed, FALSE);
	pending = g_object_new(VENTURE_TYPE_FORM_PAYMENT, "organization-id", venture_entity_get_organization_id(form),
		"name", "Pending form payment", "form-id", venture_entity_get_id(form), "version-id", venture_entity_get_id(version),
		"invoice-id", venture_entity_get_id(invoice), "reservation-id", hold != NULL ? venture_entity_get_id(hold) : (gint64)0,
		"state", "pending_payment", "expires-at", expires, "nonce-hash", nonce_hash, "answers-hash", answer_hash, "payload", payload,
		"contact-id", venture_forms_get_int(response, "contact-id"), NULL);
	{
		g_autofree gchar *key = venture_forms_get_string(fresh, "unique-email-field");
		g_autofree gchar *email = venture_string_is_empty(key) ? NULL : forms_response_email(response, key);
		g_autofree gchar *email_hash = email != NULL ? g_compute_checksum_for_string(G_CHECKSUM_SHA256, email, -1) : NULL;
		g_autofree gchar *personal = venture_forms_get_string(response, "personal-hash");
		g_object_set(pending, "email-hash", email_hash, "personal-hash", personal, NULL);
	}
	if (!forms_payment_save(database, pending, error)) goto fail;
	if (!venture_database_commit(database, error)) return FALSE;
	return forms_payment_continue(database, pending, response, refused);
fail:
	venture_database_rollback(database); return FALSE;
}

/* Settlement is already committed. Refusing completion leaves a durable
 * pending row and the paid invoice intact; a signed webhook retry can recover. */
gboolean
venture_forms_payment_reconcile(VentureDatabase *database, gint64 organization,
	gint64 checkout_id, GDateTime *now, GError **error)
{
	g_autoptr(VentureEntity) checkout = NULL, pending = NULL, form = NULL, version = NULL, response = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_FORM_PAYMENT);
	g_autoptr(GPtrArray) fields = NULL;
	g_autoptr(JsonParser) answer_parser = json_parser_new();
	g_autoptr(JsonObject) refused = json_object_new();
	g_autofree gchar *state = NULL, *prior_state = NULL, *answers = NULL, *language = NULL;
	gint64 invoice_id, hold_id;
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "form_payment") == G_TYPE_INVALID) return TRUE;
	if (venture_database_has_transaction(database))
	{ venture_set_error_validation(error, "Payment intake", "settlement must commit before completing a form"); return FALSE; }
	if (!venture_database_begin(database, error)) return FALSE;
	checkout = venture_database_get(database, VENTURE_TYPE_STRIPE_CHECKOUT, checkout_id, error);
	if (checkout == NULL) goto fail;
	if (venture_entity_get_organization_id(checkout) != organization)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Form payment not found");
		goto fail;
	}
	invoice_id = venture_forms_get_int(checkout, "invoice-id");
	venture_query_set_organization(query, organization);
	venture_query_add_filter_int(query, "invoice-id", VENTURE_FILTER_OP_EQ, invoice_id, NULL);
	pending = venture_database_find_one(database, query, error);
	if (pending == NULL)
	{
		if (error != NULL && *error != NULL) goto fail;
		return venture_database_commit(database, error);
	}
	prior_state = venture_forms_get_string(pending, "state");
	if (g_strcmp0(prior_state, "completed") == 0 || g_strcmp0(prior_state, "erased") == 0) return venture_database_commit(database, error);
	state = venture_forms_get_string(checkout, "status");
	hold_id = venture_forms_get_int(pending, "reservation-id");
	g_object_set(pending, "checkout-id", checkout_id, NULL);
	if (g_strcmp0(state, "failed") == 0 || g_strcmp0(state, "expired") == 0)
	{
		if (hold_id > 0)
		{
			g_autoptr(VentureBookingService) service = venture_booking_service_new(database);
			g_autoptr(VentureEntity) hold = venture_database_get(database, VENTURE_TYPE_BOOKING_RESERVATION, hold_id, error);
			if (hold == NULL || !venture_booking_service_release(service, hold, error)) goto fail;
		}
		g_object_set(pending, "state", state, NULL);
		if (!forms_payment_save(database, pending, error)) goto fail;
		return venture_database_commit(database, error);
	}
	if (g_strcmp0(state, "complete") != 0 || venture_forms_get_int(checkout, "payment-id") <= 0)
	{
		if (g_strcmp0(state, "processing") == 0) g_object_set(pending, "state", "processing", NULL);
		if (!forms_payment_save(database, pending, error)) goto fail;
		return venture_database_commit(database, error);
	}
	if (g_strcmp0(prior_state, "expired") == 0 || g_strcmp0(prior_state, "failed") == 0 || g_strcmp0(prior_state, "paid_needs_capacity") == 0)
	{
		g_autoptr(VentureEntity) live = venture_database_get(database, VENTURE_TYPE_FORM, venture_forms_get_int(pending, "form-id"), error);
		g_autoptr(VentureEntity) retained = NULL, frozen = NULL;
		g_autoptr(GPtrArray) questions = NULL;
		if (live == NULL) goto fail;
		retained = forms_payment_restore(pending, error); if (retained == NULL) goto fail;
		frozen = venture_database_get(database, VENTURE_TYPE_FORM_VERSION, venture_forms_get_int(pending, "version-id"), error); if (frozen == NULL) goto fail;
		questions = venture_forms_definition_for(database, live, frozen, error); if (questions == NULL) goto fail;
		/* A failure released the original cap. A later paid event must
		 * acquire it again; otherwise retain paid evidence for recovery. */
		if (!forms_is_open(database, live, now) || !forms_check_email_limit(database, live, retained, questions, refused, NULL) ||
		    !forms_payment_limits(database, live, retained, refused, NULL))
		{
			g_object_set(pending, "state", "paid_needs_capacity", NULL);
			if (!forms_payment_save(database, pending, error)) goto fail;
			return venture_database_commit(database, error);
		}
	}
	/* An expired hold cannot be resurrected over another customer's seat.
	 * Payment stays paid, and recovery selects a fresh available slot. */
	if (hold_id > 0)
	{
		g_autoptr(VentureEntity) hold = venture_database_get(database, VENTURE_TYPE_BOOKING_RESERVATION, hold_id, error);
		g_autoptr(GDateTime) expires = NULL;
		g_autofree gchar *hold_state = hold != NULL ? venture_forms_get_string(hold, "state") : NULL;
		if (hold == NULL) goto fail;
		g_object_get(hold, "expires-at", &expires, NULL);
		if (g_strcmp0(hold_state, "held") != 0 || expires == NULL || g_date_time_compare(now, expires) >= 0)
		{
			g_object_set(pending, "state", "paid_needs_booking", NULL);
			if (!forms_payment_save(database, pending, error)) goto fail;
			return venture_database_commit(database, error);
		}
	}
	form = venture_database_get(database, VENTURE_TYPE_FORM, venture_forms_get_int(pending, "form-id"), error);
	if (form == NULL) goto fail;
	version = venture_database_get(database, VENTURE_TYPE_FORM_VERSION, venture_forms_get_int(pending, "version-id"), error);
	if (form == NULL || version == NULL) goto fail;
	response = forms_payment_restore(pending, error); if (response == NULL) goto fail;
	g_object_set_data(G_OBJECT(response), VENTURE_FORMS_ACCEPTING_KEY, GINT_TO_POINTER(1));
	fields = venture_forms_definition_for(database, form, version, error); if (fields == NULL) goto fail;
	language = venture_forms_get_string(response, "language"); venture_forms_localize(fields, language);
	answers = venture_forms_get_string(response, "answers");
	if (!json_parser_load_from_data(answer_parser, answers, -1, error)) goto fail;
	g_object_set_data(G_OBJECT(form), VENTURE_FORMS_PAYMENT_SETTLING, pending);
	if (!forms_write(database, form, response, fields, json_node_get_object(json_parser_get_root(answer_parser)), TRUE, now, refused, error))
	{
		if (error != NULL && *error == NULL) venture_set_error_validation(error, "Paid response", "completion needs recovery; the invoice remains paid");
		goto fail;
	}
	g_object_set_data(G_OBJECT(form), VENTURE_FORMS_PAYMENT_SETTLING, NULL);
	g_object_set(pending, "state", "completed", "response-id", venture_entity_get_id(response), "payload", NULL, "answers-hash", NULL,
		"email-hash", NULL, "personal-hash", NULL, "contact-id", (gint64)0, NULL);
	if (!forms_payment_save(database, pending, error)) goto fail;
	return venture_database_commit(database, error);
fail:
	venture_database_rollback(database); return FALSE;
}

/* Recovery is an owner action over a form, never a generic write to a private
 * intake. Invoice selection is scoped before any provider or calendar call. */
static VentureEntity *forms_payment_recover_invoke(VentureAction *action, VentureEntity *form,
	GHashTable *params, const VentureActor *actor, GError **error)
{
	VentureDatabase *database = venture_action_get_data(action);
	JsonNode *invoice_node = g_hash_table_lookup(params, "invoice_id"), *start_node = g_hash_table_lookup(params, "booking_start");
	gint64 invoice_id = invoice_node != NULL ? json_node_get_int(invoice_node) : 0;
	const gchar *start = start_node != NULL && !JSON_NODE_HOLDS_NULL(start_node) ? json_node_get_string(start_node) : NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_FORM_PAYMENT);
	g_autoptr(VentureEntity) pending = NULL, checkout = NULL, answer = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autofree gchar *state = NULL, *result = NULL;
	(void)actor;
	venture_query_set_organization(query, venture_entity_get_organization_id(form));
	venture_query_add_filter_int(query, "form-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(form), NULL);
	venture_query_add_filter_int(query, "invoice-id", VENTURE_FILTER_OP_EQ, invoice_id, NULL);
	pending = venture_database_find_one(database, query, error);
	if (pending == NULL)
	{ if (error != NULL && *error == NULL) g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Form payment not found"); return NULL; }
	state = venture_forms_get_string(pending, "state");
	if (!venture_string_is_empty(start))
	{
		g_autoptr(VentureEntity) version = NULL, hold = NULL, old_hold = NULL, page = NULL;
		g_autoptr(VentureBookingService) service = venture_booking_service_new(database);
		g_autoptr(GPtrArray) fields = NULL;
		
		guint i;
		const VentureFormsField *booking = NULL;
		if (g_strcmp0(state, "paid_needs_booking") != 0)
		{ venture_set_error_validation(error, "Paid booking", "only a paid intake with an expired hold can select a replacement slot"); return NULL; }
		if (!venture_database_begin(database, error)) return NULL;
		version = venture_database_get(database, VENTURE_TYPE_FORM_VERSION, venture_forms_get_int(pending, "version-id"), error);
		if (version == NULL) goto fail;
		fields = venture_forms_definition_for(database, form, version, error); if (fields == NULL) goto fail;
		for (i = 0; i < fields->len; i++)
			if (((VentureFormsField *)g_ptr_array_index(fields, i))->kind == VENTURE_FORM_FIELD_BOOKING) booking = g_ptr_array_index(fields, i);
		if (booking == NULL) goto fail;
		page = venture_database_get(database, VENTURE_TYPE_BOOKING_PAGE, booking->booking_page_id, error);
		if (page == NULL) goto fail;
		hold = venture_booking_service_reserve(service, page, start, 30, now, error); if (hold == NULL) goto fail;
		old_hold = venture_database_get(database, VENTURE_TYPE_BOOKING_RESERVATION, venture_forms_get_int(pending, "reservation-id"), error);
		if (old_hold == NULL || !venture_booking_service_release(service, old_hold, error)) goto fail;
		/* Preserve the original answer as evidence. The linked meeting records
		 * the replacement arranged by the owner. */
		g_object_set(pending, "reservation-id", venture_entity_get_id(hold), "state", "pending_payment", NULL);
		if (!forms_payment_save(database, pending, error)) goto fail;
		if (!venture_database_commit(database, error)) return NULL;
	}
	{
		g_autoptr(VentureQuery) sessions = venture_query_new(VENTURE_TYPE_STRIPE_CHECKOUT);
		venture_query_set_organization(sessions, venture_entity_get_organization_id(form));
		venture_query_add_filter_int(sessions, "invoice-id", VENTURE_FILTER_OP_EQ, invoice_id, NULL);
		venture_query_add_order(sessions, "id", VENTURE_SORT_DESCENDING, NULL);
		venture_query_set_limit(sessions, 1);
		checkout = venture_database_find_one(database, sessions, error);
		if (error != NULL && *error != NULL) return NULL;
	}
	if (checkout != NULL && !venture_forms_payment_reconcile(database, venture_entity_get_organization_id(form), venture_entity_get_id(checkout), now, error)) return NULL;
	g_clear_object(&pending); pending = venture_database_find_one(database, query, error); if (pending == NULL) return NULL;
	g_clear_pointer(&state, g_free); state = venture_forms_get_string(pending, "state");
	result = g_strdup_printf("{\"invoice_id\":%" G_GINT64_FORMAT ",\"response_id\":%" G_GINT64_FORMAT ",\"state\":\"%s\"}", invoice_id, venture_forms_get_int(pending, "response-id"), state);
	answer = VENTURE_ENTITY(venture_form_new()); venture_entity_set_organization_id(answer, venture_entity_get_organization_id(form));
	g_object_set(answer, "name", "Payment recovery", "result", result, NULL);
	return g_steal_pointer(&answer);
fail:
	venture_database_rollback(database); return NULL;
}

static gboolean forms_payment_forget(VentureDatabase *database, VentureEntity *pending, GError **error)
{
	gint64 id = venture_forms_get_int(pending, "reservation-id");
	if (id > 0)
	{
		g_autoptr(VentureBookingService) service = venture_booking_service_new(database);
		g_autoptr(VentureEntity) hold = venture_database_get(database, VENTURE_TYPE_BOOKING_RESERVATION, id, error);
		if (hold == NULL || !venture_booking_service_release(service, hold, error)) return FALSE;
	}
	/* Accounting evidence has its own retention. Forgetting an intake never
	 * voids an invoice or manufactures a refund, even if settlement is late. */
	g_object_set(pending, "state", "erased", "payload", NULL, "answers-hash", NULL, "email-hash", NULL,
		"personal-hash", NULL, "contact-id", (gint64)0, "response-id", (gint64)0, NULL);
	return forms_payment_save(database, pending, error);
}

static gboolean forms_payment_forget_response(VentureDatabase *database, VentureEntity *response, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_FORM_PAYMENT);
	g_autoptr(GPtrArray) rows = NULL;
	guint i;
	venture_query_set_organization(query, venture_entity_get_organization_id(response));
	venture_query_add_filter_int(query, "response-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(response), NULL);
	rows = venture_database_find(database, query, error); if (rows == NULL) return FALSE;
	for (i = 0; i < rows->len; i++)
		if (!forms_payment_forget(database, g_ptr_array_index(rows, i), error)) return FALSE;
	return TRUE;
}

static gboolean forms_payment_mentions(VentureDatabase *database, VentureEntity *pending, const gchar *email, GError **error)
{
	g_autofree gchar *payload = venture_forms_get_string(pending, "payload");
	g_autoptr(VentureEntity) response = NULL;
	g_autofree gchar *open = NULL, *secret = NULL;
	if (venture_string_is_empty(payload)) return FALSE;
	response = forms_payment_restore(pending, error); if (response == NULL) return FALSE;
	open = venture_forms_get_string(response, "answers"); secret = venture_forms_get_string(response, "sensitive-answers");
	return forms_answers_mention(open, email) || forms_answers_mention(secret, email) || forms_bound_email(database, response, email);
}

static gint forms_payment_expire_payloads(VentureDatabase *database, gint64 organization,
	gint64 form_id, GDateTime *cutoff, guint budget, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_FORM_PAYMENT);
	g_autoptr(GPtrArray) rows = NULL;
	g_autofree gchar *before = venture_time_to_string(cutoff);
	guint i;
	if (budget == 0) return 0;
	venture_query_set_organization(query, organization); venture_query_set_limit(query, budget);
	if (form_id > 0) venture_query_add_filter_int(query, "form-id", VENTURE_FILTER_OP_EQ, form_id, NULL);
	venture_query_add_filter_string(query, "created-at", VENTURE_FILTER_OP_LT, before, NULL);
	venture_query_add_filter_string(query, "payload", VENTURE_FILTER_OP_NOT_NULL, NULL, NULL);
	venture_query_add_order(query, "created-at", VENTURE_SORT_ASCENDING, NULL);
	rows = venture_database_find(database, query, error); if (rows == NULL) return -1;
	for (i = 0; i < rows->len; i++)
		if (!forms_payment_forget(database, g_ptr_array_index(rows, i), error)) return -1;
	return (gint)rows->len;
}
