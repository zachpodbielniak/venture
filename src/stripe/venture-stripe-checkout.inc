/* SPDX-License-Identifier: AGPL-3.0-or-later */
static gboolean
checkout_pending(const gchar *state)
{
	return !g_strcmp0(state, "initiated") || !g_strcmp0(state, "open") ||
		!g_strcmp0(state, "processing") || !g_strcmp0(state, "exception");
}

/* Transport uncertainty is durable operational work even when no signed event
 * exists yet. It never grants authority to release the reserved debit. */
static gboolean
stripe_checkout_exception(VentureStripeService *self, VentureEntity *checkout,
	gboolean resolved, GError **error)
{
	g_autoptr(GPtrArray) rows = find_id(self, VENTURE_TYPE_PROCESSOR_EXCEPTION,
		"checkout-id", venture_entity_get_id(checkout), error);
	g_autoptr(VentureProcessorException) exception = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	gint64 invoice_id;
	guint i;
	if (!rows) return FALSE;
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		gint64 event_id;
		g_object_get(row, "event-id", &event_id, NULL);
		if (!event_id) { exception = g_object_ref(VENTURE_PROCESSOR_EXCEPTION(row)); break; }
	}
	if (!exception && resolved) return TRUE;
	if (!exception)
	{
		exception = venture_processor_exception_new();
		venture_entity_set_organization_id(VENTURE_ENTITY(exception), self->organization_id);
		g_object_get(checkout, "invoice-id", &invoice_id, NULL);
		g_object_set(exception, "connection-id", self->connection_id, "invoice-id", invoice_id,
			"checkout-id", venture_entity_get_id(checkout), "provider-id", venture_entity_get_uuid(checkout),
			"kind", "creation_uncertain", "occurred-at", now, NULL);
	}
	g_object_set(exception, "resolved", resolved,
		"reason", resolved ? "Provider Checkout identity recovered" : "Checkout creation is uncertain; retry the same reservation or reconcile provider evidence", NULL);
	return venture_stripe_save_owned(self->database, VENTURE_ENTITY(exception), NULL, error);
}

/* Defined with the signed completion path (venture-stripe-settlement.inc),
 * which a stale session found already paid records its payment through. */
static gboolean stripe_settle_checkout(VentureStripeService *self, VentureEntity *checkout,
	const VentureMoney *expected, const gchar *intent, GDateTime *settled_at,
	gboolean accept_balance_change, const VentureActor *actor, GError **error);

/* How many times one request retires a stale session and looks again
 * before it gives up: the balance moving under every attempt is a refusal,
 * not a loop. */
#define STRIPE_STALE_ROUNDS (3)

/* Asks Stripe to expire @checkout's session, through venture_stripe_execute()
 * like every other Stripe call (its transport, retries and deadline), under
 * a key of the attempt's own, so a retry is the same request and revoking
 * the link later repeats it rather than making a second. %TRUE only when
 * Stripe answers that this very session is expired. */
static gboolean
stripe_checkout_expire_remote(VentureStripeService *self, VentureEntity *checkout, GError **error)
{
	g_autoptr(StripeRequest) request = NULL;
	g_autoptr(StripeResource) response = NULL;
	g_autofree gchar *session = NULL;
	g_object_get(checkout, "session-id", &session, NULL);
	if (!session || !*session)
		return refuse(error, "Checkout has no provider identity to expire");
	request = stripe_request_new(STRIPE_CHECKOUT_EXPIRE);
	request->id = g_strdup(session);
	g_free(request->idempotency_key);
	request->idempotency_key = g_strdup_printf("venture-expire-%s", venture_entity_get_uuid(checkout));
	response = venture_stripe_execute(self->client, request, NULL, error);
	if (!response) return FALSE;
	if (g_strcmp0(stripe_resource_get_string(response, "id"), session) ||
		g_strcmp0(stripe_resource_get_string(response, "status"), "expired"))
		return refuse(error, "Stripe did not confirm expiration; retain the pending attempt");
	return TRUE;
}

/* Writes why a stale session was retired to the audit log, against the
 * attempt itself: the amount it was opened at, the balance it no longer
 * matches and what became of it (expired, or found paid). The service's
 * own record, so it is saved outside the caller's access scope, as
 * audit_event() saves what Stripe said. */
static gboolean
stripe_checkout_stale_audit(VentureStripeService *self, VentureEntity *checkout,
	const VentureMoney *opened, const VentureMoney *balance, const gchar *outcome, GError **error)
{
	g_autoptr(JsonObject) object = json_object_new();
	g_autoptr(JsonNode) diff = json_node_new(JSON_NODE_OBJECT);
	g_autoptr(VentureAuditEntry) entry = NULL;
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autofree gchar *session = NULL, *old_amount = NULL, *new_amount = NULL;
	g_object_get(checkout, "session-id", &session, NULL);
	old_amount = opened ? venture_money_to_string(opened) : g_strdup("unknown");
	new_amount = venture_money_to_string(balance);
	json_object_set_string_member(object, "reason", "Invoice open balance changed after this Checkout opened");
	json_object_set_string_member(object, "session_id", session ? session : "");
	json_object_set_string_member(object, "old_amount", old_amount);
	json_object_set_string_member(object, "new_amount", new_amount);
	json_object_set_string_member(object, "outcome", outcome);
	json_node_set_object(diff, object);
	entry = venture_audit_entry_new_for_change(VENTURE_AUDIT_ACTION_UPDATE, VENTURE_ACTOR_KIND_SYSTEM,
		"stripe", checkout, diff);
	g_object_set(entry, "source", "stripe.checkout.stale", NULL);
	internal = venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
	return venture_database_save(self->database, VENTURE_ENTITY(entry), NULL, error);
}

/*
 * @checkout_id is the invoice's one active attempt, an open session opened
 * at an amount the invoice no longer has (@balance is what eligible() reads
 * now). Retires it so the caller can open one at @balance, or finds the
 * customer already paid it.
 *
 * Stripe is asked to expire it first. If that fails, why is read from
 * Stripe's own record of the session, not from the refusal's wording:
 *
 *  - expired there (a lost answer, or expired by somebody else): retired.
 *  - complete and paid at the amount it was opened at: its payment is
 *    recorded here through the completion path's own stripe_settle_checkout().
 *    The balance has moved, which is the point, so it is accepted as moved
 *    when the payment is no more than the balance now (an ordinary part
 *    payment). A payment over the balance now would leave a credit, which
 *    is the operator's decision on the signed completion's settlement
 *    exception, so that one, an unpaid bank debit and anything this cannot
 *    verify are marked processing instead: still the one attempt, and the
 *    signed completion settles it or raises the exception.
 *  - anything else, or a session that cannot be read: refused with
 *    %VENTURE_ERROR_CONFLICT. The stale session is not reused and no other
 *    is opened, so the attempt stays the invoice's only one.
 *
 * Stripe is called with no transaction open; the local change is made in
 * one of its own after reading the attempt again, and only to one still
 * open, so a signed event that settled it meanwhile wins. On %TRUE with
 * *@kept_out set the attempt stands (paid or processing) and the caller
 * opens nothing; with it %NULL the caller looks again.
 */
static gboolean
stripe_checkout_retire_stale(VentureStripeService *self, gint64 checkout_id,
	const VentureMoney *balance, const VentureActor *actor,
	VentureStripeCheckout **kept_out, GError **error)
{
	g_autoptr(VentureEntity) stale = NULL, current = NULL;
	g_autoptr(VentureMoney) reserved = NULL;
	g_autoptr(GError) expire_error = NULL;
	g_autoptr(StripeResource) remote = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autofree gchar *state = NULL, *session = NULL, *intent = NULL;
	const gchar *outcome = "expired";
	gboolean settle = FALSE;

	*kept_out = NULL;
	stale = venture_database_get(self->database, VENTURE_TYPE_STRIPE_CHECKOUT, checkout_id, error);
	if (!stale) return FALSE;
	g_object_get(stale, "status", &state, "session-id", &session, "expected", &reserved, NULL);
	/* Moved on since the caller read it: the next look reads it as it is. */
	if (g_strcmp0(state, "open")) return TRUE;

	if (!stripe_checkout_expire_remote(self, stale, &expire_error))
	{
		g_autoptr(StripeRequest) request = stripe_request_new(STRIPE_CHECKOUT_RETRIEVE);
		g_autoptr(GError) read_error = NULL;
		const gchar *status;
		request->id = g_strdup(session);
		remote = venture_stripe_execute(self->client, request, NULL, &read_error);
		status = remote ? stripe_resource_get_string(remote, "status") : NULL;
		if (!remote || g_strcmp0(stripe_resource_get_string(remote, "id"), session) ||
			g_strcmp0(stripe_resource_get_string(remote, "client_reference_id"), venture_entity_get_uuid(stale)) ||
			(g_strcmp0(status, "expired") && g_strcmp0(status, "complete")))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
				"The invoice changed while its Checkout was open, and Stripe did not expire that session (%s); "
				"it is not reused and no other is opened", expire_error->message);
			return FALSE;
		}
		if (!g_strcmp0(status, "complete"))
		{
			g_autoptr(JsonObject) object = stripe_resource_get_json(remote);
			JsonNode *total = object ? json_object_get_member(object, "amount_total") : NULL;
			JsonNode *live = object ? json_object_get_member(object, "livemode") : NULL;
			const gchar *currency = stripe_resource_get_string(remote, "currency");
			const gchar *paid_with = stripe_resource_get_string(remote, "payment_intent");
			gint64 opened_units = 0;
			outcome = "complete";
			/* Recorded here only on the evidence the signed completion
			 * needs: paid, a payment intent, this environment, and exactly
			 * the amount and currency the attempt reserved. */
			if (reserved && to_stripe_amount(reserved, &opened_units, NULL) &&
				total && JSON_NODE_HOLDS_VALUE(total) && json_node_get_value_type(total) == G_TYPE_INT64 &&
				json_node_get_int(total) == opened_units &&
				currency && !g_ascii_strcasecmp(currency, venture_money_get_currency(reserved)) &&
				live && JSON_NODE_HOLDS_VALUE(live) && json_node_get_value_type(live) == G_TYPE_BOOLEAN &&
				json_node_get_boolean(live) == !g_strcmp0(self->environment, "live") &&
				!g_strcmp0(stripe_resource_get_string(remote, "payment_status"), "paid") &&
				paid_with && g_str_has_prefix(paid_with, "pi_"))
			{
				g_autoptr(VentureMoney) left = venture_money_subtract(balance, reserved, NULL);
				settle = left && venture_money_get_amount(left) >= 0;
				intent = g_strdup(paid_with);
			}
		}
	}

	if (!venture_database_begin_serializable(self->database, error)) return FALSE;
	current = venture_database_get(self->database, VENTURE_TYPE_STRIPE_CHECKOUT, checkout_id, error);
	if (!current) goto fail;
	g_clear_pointer(&state, g_free);
	g_object_get(current, "status", &state, NULL);
	if (!g_strcmp0(state, "open"))
	{
		if (!g_strcmp0(outcome, "expired"))
		{
			g_object_set(current, "status", "expired", "active", FALSE, NULL);
			if (!venture_stripe_save_owned(self->database, current, actor, error)) goto fail;
		}
		else if (settle)
		{
			if (!stripe_settle_checkout(self, current, reserved, intent, now, TRUE, actor, error)) goto fail;
		}
		else
		{
			outcome = "complete_unsettled";
			g_object_set(current, "status", "processing", NULL);
			if (!venture_stripe_save_owned(self->database, current, actor, error)) goto fail;
		}
		if (!stripe_checkout_stale_audit(self, current, reserved, balance, outcome, error)) goto fail;
	}
	if (!venture_database_commit(self->database, error)) goto fail;
	g_clear_pointer(&state, g_free);
	g_object_get(current, "status", &state, NULL);
	/* Paid or being paid: that attempt stands and nothing else opens. */
	if (!g_strcmp0(state, "complete") || !g_strcmp0(state, "processing"))
		*kept_out = g_object_ref(VENTURE_STRIPE_CHECKOUT(current));
	return TRUE;
fail:
	venture_database_rollback(self->database);
	return FALSE;
}

/* The reservation and provider creation of one Checkout. An open session
 * at an amount the invoice no longer has is not reused: this returns
 * %NULL with no error, nothing written, *@stale_out naming it and
 * *@balance_out the balance now, for stripe_checkout_create() to retire it
 * and call again. */
static VentureStripeCheckout *
stripe_checkout_reserve(VentureStripeService *self, gint64 invoice_id, GDateTime *deadline,
	VentureStripePaymentLink *payment_link, gboolean cards_only,
	const VentureActor *actor, gint64 *stale_out, VentureMoney **balance_out, GError **error)
{
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(GPtrArray) sessions = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureMoney) expected = NULL;
	g_autoptr(GDateTime) expires = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(StripeRequest) request = NULL;
	g_autoptr(StripeResource) resource = NULL;
	g_autoptr(VentureStripeCheckout) checkout = NULL;
	g_autofree gchar *customer_id = NULL;
	g_autofree gchar *success_url = NULL;
	g_autofree gchar *cancel_url = NULL;
	gint64 company_id, contact_id, checkout_id;
	gboolean transaction = FALSE;
	gboolean ach = FALSE;
	guint i;

	*stale_out = 0;
	*balance_out = NULL;
	/* No remote effect may be nested in another writer's rollback scope. */
	if (!venture_database_begin_serializable(self->database, error)) return NULL;
	transaction = TRUE;
	if (!eligible(self, invoice_id, &invoice, NULL, NULL, &expected, error)) goto fail;
	if (!venture_access_policy_check_write(venture_database_get_access_policy(self->database), invoice, "write", error)) goto fail;
	if (payment_link)
	{
		g_autoptr(VentureEntity) live = venture_database_get(self->database, VENTURE_TYPE_STRIPE_PAYMENT_LINK,
			venture_entity_get_id(VENTURE_ENTITY(payment_link)), error);
		gboolean enabled;
		gint64 version;
		if (!live) goto fail;
		g_object_get(live, "enabled", &enabled, "invoice-version", &version, NULL);
		if (!enabled || venture_entity_get_version(live) != venture_entity_get_version(VENTURE_ENTITY(payment_link)) ||
			version != venture_entity_get_version(invoice))
		{
			refuse(error, "Payment capability changed before provider reservation");
			goto fail;
		}
	}

	query = venture_query_new(VENTURE_TYPE_STRIPE_CHECKOUT);
	venture_query_set_limit(query, 0);
	if (!venture_query_add_filter_int(query, "organization-id", VENTURE_FILTER_OP_EQ, self->organization_id, error) ||
		!venture_query_add_filter_int(query, "invoice-id", VENTURE_FILTER_OP_EQ, invoice_id, error)) goto fail;
	sessions = venture_database_find(self->database, query, error);
	if (!sessions) goto fail;
	for (i = 0; i < sessions->len; i++)
	{
		VentureEntity *prior = g_ptr_array_index(sessions, i);
		gint64 binding;
		g_autofree gchar *state = NULL;
		g_object_get(prior, "connection-id", &binding, "status", &state, NULL);
		if (!checkout_pending(state)) continue;
		{
			g_autofree gchar *channel = NULL;
			g_object_get(prior, "channel", &channel, NULL);
			if (!g_strcmp0(channel, "automatic"))
			{
				refuse(error, "Automatic collection is pending; reconcile it before creating hosted Checkout"); goto fail;
			}
		}

		if (binding != self->connection_id)
		{
			refuse(error, "A pending Checkout belongs to an earlier Stripe account; reconcile it before creating another");
			goto fail;
		}
		/* An open session past its deadline cannot be paid at Stripe any
		 * more, so it is not reused: it is expired here, freeing the one
		 * active attempt the invoice may have, and a new session opens.
		 * Its own checkout.session.expired event, arriving later, then
		 * finds it expired and changes nothing; a completion for it, if
		 * one ever came, still settles it. One near its deadline but not
		 * past it is still reused, since the customer can still pay it. */
		if (!g_strcmp0(state, "open"))
		{
			g_autoptr(GDateTime) deadline_at = NULL;
			g_object_get(prior, "expires-at", &deadline_at, NULL);
			if (deadline_at && g_date_time_compare(now, deadline_at) >= 0)
			{
				g_object_set(prior, "status", "expired", "active", FALSE, NULL);
				if (!venture_stripe_save_owned(self->database, prior, actor, error)) goto fail;
				continue;
			}
		}
		/* An open session charges what the invoice owed when it opened.
		 * Reused after a late fee, an edit or a credit, the customer would
		 * pay that and not what is owed now (VENTURE #39), so a session
		 * whose reserved amount or currency is not the balance eligible()
		 * reads now is handed back to be expired at Stripe, which cannot be
		 * done from inside this transaction. Nothing is written: the caller
		 * comes back and reads it all again. A session in any other
		 * pending state has a payment in flight and is left as it is. */
		if (!g_strcmp0(state, "open"))
		{
			g_autoptr(VentureMoney) reserved = NULL;
			g_object_get(prior, "expected", &reserved, NULL);
			if (!money_equal(reserved, expected))
			{
				*stale_out = venture_entity_get_id(prior);
				*balance_out = g_steal_pointer(&expected);
				goto fail;
			}
		}
		checkout = g_object_ref(VENTURE_STRIPE_CHECKOUT(prior));
		if (g_strcmp0(state, "initiated"))
		{
			g_object_get(checkout, "expires-at", &expires, NULL);
			if (deadline && (!expires || g_date_time_compare(expires, deadline) > 0))
			{
				refuse(error, "Existing Checkout exceeds this payment link lifetime");
				goto fail;
			}
			if (payment_link)
			{
				g_object_set(payment_link, "checkout-id", venture_entity_get_id(VENTURE_ENTITY(checkout)), NULL);
				if (!venture_stripe_save_owned(self->database, VENTURE_ENTITY(payment_link), actor, error)) goto fail;
			}
			if (!venture_database_commit(self->database, error)) goto fail;
			return g_steal_pointer(&checkout);
		}
		break;
	}
	if (!checkout)
	{
		gint64 amount;
		expires = deadline ? g_date_time_ref(deadline) : g_date_time_add_hours(now, 23);
		if (g_date_time_difference(expires, now) < 30 * G_TIME_SPAN_MINUTE ||
			g_date_time_difference(expires, now) > 24 * G_TIME_SPAN_HOUR)
		{
			refuse(error, "Checkout deadline must be 30 minutes through 24 hours from creation");
			goto fail;
		}
		if (!to_stripe_amount(expected, &amount, error)) goto fail;
		/* USD on purpose: ACH debit is a US bank rail and Stripe offers it
		 * for dollar invoices only; any other currency gets cards. */
		ach = !cards_only && self->ach_enabled && !g_strcmp0(venture_money_get_currency(expected), "USD") && amount >= 50;
		checkout = venture_stripe_checkout_new();
		venture_entity_set_organization_id(VENTURE_ENTITY(checkout), self->organization_id);
		g_object_set(checkout, "connection-id", self->connection_id, "invoice-id", invoice_id, "channel", "hosted",
			"session-id", "", "url", "", "status", "initiated", "active", TRUE,
			"expected", expected, "success-url", self->success_url,
			"cancel-url", self->cancel_url, "ach-enabled", ach, "expires-at", expires, NULL);
		if (!venture_stripe_save_owned(self->database, VENTURE_ENTITY(checkout), actor, error)) goto fail;
	}
	else
	{
		g_autoptr(VentureMoney) reserved = NULL;
		g_autoptr(GDateTime) created = NULL;
		g_object_get(checkout, "expected", &reserved, "created-at", &created, "expires-at", &expires, NULL);
		/* Stripe may prune idempotency keys after 24h. An old uncertain
		 * request requires reconciliation, never a blind second debit. */
		if (!money_equal(expected, reserved) || !created || !expires ||
			g_date_time_difference(expires, now) < 30 * G_TIME_SPAN_MINUTE ||
			(deadline && g_date_time_compare(expires, deadline) > 0) ||
			g_date_time_difference(now, created) >= 23 * G_TIME_SPAN_HOUR)
		{
			refuse(error, "Uncertain Checkout requires reconciliation: balance changed or retry window elapsed");
			goto fail;
		}
	}
	checkout_id = venture_entity_get_id(VENTURE_ENTITY(checkout));
	if (payment_link)
	{
		g_object_set(payment_link, "checkout-id", checkout_id, NULL);
		if (!venture_stripe_save_owned(self->database, VENTURE_ENTITY(payment_link), actor, error)) goto fail;
	}

	if (!venture_database_commit(self->database, error)) goto fail;
	transaction = FALSE;
	g_object_get(checkout, "success-url", &success_url, "cancel-url", &cancel_url, "ach-enabled", &ach, NULL);
	g_object_get(invoice, "company-id", &company_id, "contact-id", &contact_id, NULL);
	customer_id = stripe_customer(self, company_id, contact_id, actor, error);
	if (!customer_id) goto fail;
	request = stripe_request_new(STRIPE_CHECKOUT_CREATE);
	request->customer = g_steal_pointer(&customer_id);
	{
		gint64 amount;
		if (!to_stripe_amount(expected, &amount, error)) goto fail;
		request->unit_amount = (guint64)amount;
	}
	request->currency = g_ascii_strdown(venture_money_get_currency(expected), -1);
	request->product_name = g_strdup("Invoice");
	request->quantity = 1;
	request->expires_at = (guint64)g_date_time_to_unix(expires);
	request->client_reference_id = g_strdup(venture_entity_get_uuid(VENTURE_ENTITY(checkout)));
	request->success_url = g_steal_pointer(&success_url);
	request->cancel_url = g_steal_pointer(&cancel_url);
	request->payment_methods = ach ? STRIPE_CHECKOUT_CARD_AND_US_BANK : STRIPE_CHECKOUT_CARD;
	request->statement_descriptor_suffix = g_strdup(self->descriptor_suffix);
	g_free(request->idempotency_key);
	request->idempotency_key = g_strdup_printf("venture-checkout-%s", venture_entity_get_uuid(VENTURE_ENTITY(checkout)));
	resource = venture_stripe_execute(self->client, request, NULL, error);
	if (!resource) goto fail;
	if (!stripe_resource_get_string(resource, "id") || !stripe_resource_get_string(resource, "url"))
	{
		refuse(error, "Stripe returned no Checkout identity or URL; retain the attempt for retry");
		goto fail;
	}
	if (!venture_database_begin(self->database, error)) goto fail;
	transaction = TRUE;
	g_clear_object(&checkout);
	checkout = (VentureStripeCheckout *)venture_database_get(self->database, VENTURE_TYPE_STRIPE_CHECKOUT, checkout_id, error);
	if (!checkout) goto fail;
	{
		g_autofree gchar *stored_id = NULL;
		g_autofree gchar *state = NULL;
		g_object_get(checkout, "session-id", &stored_id, "status", &state, NULL);
		if (stored_id && *stored_id && g_strcmp0(stored_id, stripe_resource_get_string(resource, "id")))
		{
			refuse(error, "Checkout response conflicts with retained webhook identity");
			goto fail;
		}
		g_object_set(checkout, "session-id", stripe_resource_get_string(resource, "id"),
			"url", stripe_resource_get_string(resource, "url"), NULL);
		/* A signed callback may already have settled this reservation. */
		if (!g_strcmp0(state, "initiated")) g_object_set(checkout, "status", "open", NULL);
	}
	if (!venture_stripe_save_owned(self->database, VENTURE_ENTITY(checkout), actor, error) ||
		!stripe_checkout_exception(self, VENTURE_ENTITY(checkout), TRUE, error)) goto fail;
	if (!venture_database_commit(self->database, error)) goto fail;
	return g_steal_pointer(&checkout);
fail:
	if (transaction) venture_database_rollback(self->database);
	if (checkout && venture_entity_is_persisted(VENTURE_ENTITY(checkout)))
	{
		g_autofree gchar *state = NULL;
		g_object_get(checkout, "status", &state, NULL);
		if (!g_strcmp0(state, "initiated"))
			stripe_checkout_exception(self, VENTURE_ENTITY(checkout), FALSE, NULL);
	}
	return NULL;
}

/*
 * Creates or reuses the invoice's one active Checkout. An open session at
 * an amount the invoice no longer has is retired first (expired at Stripe,
 * or found paid) and the reservation read again, at most
 * STRIPE_STALE_ROUNDS times.
 *
 * Two clicks at once still make one attempt. Held by the paying party
 * before anything is read (stripe_invoice_hold(), #34), the whole of it --
 * reading the stale session, expiring it at Stripe, marking it expired and
 * opening the new one -- is one holder's: a second click on a fiber waits
 * here for the first to finish, then finds the new session at the current
 * amount and reuses it. Where no hold is taken (a transaction already
 * open, or a plain stack while a fiber holds it) the stale session is only
 * marked expired after reading it again inside a transaction, and only if
 * it is still open, and the new reservation is refused by the one-active
 * index on stripe_checkout (#311) if another request made one first; so
 * neither path can leave two active rows.
 */
static VentureStripeCheckout *
stripe_checkout_create(VentureStripeService *self, gint64 invoice_id, GDateTime *deadline,
	VentureStripePaymentLink *payment_link, gboolean cards_only,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureFiberHold) hold = NULL;
	guint round;

	/* Before anything is read: a second Checkout for this customer waits
	 * here for the first, then finds its session (stripe_party_hold()). */
	hold = stripe_invoice_hold(self, invoice_id);
	for (round = 0; round < STRIPE_STALE_ROUNDS; round++)
	{
		g_autoptr(VentureMoney) balance = NULL;
		VentureStripeCheckout *checkout, *kept = NULL;
		gint64 stale = 0;
		checkout = stripe_checkout_reserve(self, invoice_id, deadline, payment_link, cards_only,
			actor, &stale, &balance, error);
		if (checkout || !stale) return checkout;
		if (!stripe_checkout_retire_stale(self, stale, balance, actor, &kept, error)) return NULL;
		if (kept) return kept;
	}
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
		"The invoice balance kept changing while Checkout was being opened; try again");
	return NULL;
}

VentureStripeCheckout *
venture_stripe_service_checkout(VentureStripeService *self, gint64 invoice_id,
	const VentureActor *actor, GError **error)
{
	return stripe_checkout_create(self, invoice_id, NULL, NULL, FALSE, actor, error);
}

VentureStripeCheckout *
venture_stripe_service_checkout_until(VentureStripeService *self, gint64 invoice_id,
	GDateTime *deadline, gboolean cards_only, const VentureActor *actor, GError **error)
{
	return stripe_checkout_create(self, invoice_id, deadline, NULL, cards_only, actor, error);
}
