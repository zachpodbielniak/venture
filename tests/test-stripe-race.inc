/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * One Checkout per paying party, whichever way the customer pays: a Pay
 * now click, the customer portal's pay button, or a paid form (its first
 * post, or a retry of it with the same answers, which reuses its
 * invoice's reservation). Two of them for one invoice at once get one
 * Stripe customer and one Checkout, and both are answered with the same
 * session. The first is held on Stripe; the second is sent meanwhile, and
 * the first is let go only once the second is queued behind the first
 * one's hold or has answered without it (hold_race_start()), so a missing
 * hold fails the same way every run: the second reuses an idempotency key
 * still in flight, which the fake refuses as Stripe does.
 *
 * paynow-portal: a Pay now click held creating the customer; the portal.
 * paynow-form:   a Pay now click for a paid form's invoice, held on its
 *                Checkout; the form's retry.
 * portal-form:   the portal for that invoice, held; the form's retry.
 * form-form:     a paid form posted twice at once (a double click), the
 *                first held creating the customer.
 */

typedef struct
{
	const gchar	*method;
	const gchar	*path;		/* not owned */
	const gchar	*body;
} RaceClick;

/* A paid form like /stripe/forms/paid's: a name, an email, one price. */
static VentureEntity *
race_paid_form(Fixture *f)
{
	g_autoptr(VentureEntity) form = record_new(f, "form"), price = record_new(f, "form_price"), version = NULL;
	g_autoptr(VentureMoney) unit = venture_money_new_for_currency(10000, "USD");
	g_autoptr(GError) error = NULL;
	guint i;
	g_object_set(form, "name", "Paid intake", "title", "Admission", "state", VENTURE_FORM_LIVE,
		"payment-enabled", TRUE, "public-origin", "https://forms.example.test", NULL);
	save(f, form);
	for (i = 0; i < 2; i++)
	{
		g_autoptr(VentureEntity) field = record_new(f, "form_field");
		g_object_set(field, "form-id", venture_entity_get_id(form), "key", i == 0 ? "name" : "email",
			"label", i == 0 ? "Name" : "Email", "kind", i == 0 ? VENTURE_FORM_FIELD_SHORT_TEXT : VENTURE_FORM_FIELD_EMAIL,
			"required", TRUE, "position", (gint64)i, NULL);
		save(f, field);
	}
	g_object_set(price, "form-id", venture_entity_get_id(form), "key", "admission", "name", "Admission",
		"product-id", f->product_id, "unit-price", unit, NULL);
	save(f, price);
	version = venture_forms_publish(f->database, form, NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(version);
	return venture_database_get(f->database, VENTURE_TYPE_FORM, venture_forms_get_int(version, "form-id"), NULL);
}

/* The invoice the form's one retained payment is for. */
static gint64
race_form_invoice(Fixture *f)
{
	g_autoptr(GPtrArray) pending = rows(f, "form_payment");
	g_assert_cmpuint(pending->len, ==, 1);
	return venture_forms_get_int(g_ptr_array_index(pending, 0), "invoice-id");
}

static void
test_checkout_race(Fixture *f, gconstpointer data)
{
	const gchar *mode = data;
	gboolean paynow = g_str_has_prefix(mode, "paynow-");
	gboolean portal = strstr(mode, "portal") != NULL;
	gboolean form_first = !g_strcmp0(mode, "form-form");
	gboolean retry = !form_first && strstr(mode, "form") != NULL;	/* the form's invoice exists */
	g_autoptr(GError) error = NULL;
	g_autoptr(GObject) transport = g_object_new(fake_transport_get_type(), NULL);
	FakeTransport *fake = (FakeTransport *)transport;
	g_autoptr(JsonNode) settings = binding_settings("whsec_original");
	g_autoptr(GBytes) secret = g_bytes_new_static("01234567890123456789012345678901", 32);
	g_autoptr(VentureIntegrationConnection) connection = NULL;
	g_autoptr(VentureStripeService) service = NULL;
	g_autoptr(VentureEntity) form = NULL;
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureWebServer) server = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GDateTime) earlier = g_date_time_add_seconds(now, -10);
	g_autoptr(GDateTime) expires = g_date_time_add_hours(now, 2);
	g_autoptr(GPtrArray) customers = NULL, exceptions = NULL;
	g_autofree gchar *state_dir = NULL, *form_path = NULL, *form_body = NULL;
	g_autofree gchar *pay_path = NULL, *portal_path = NULL, *portal_body = NULL, *key = NULL;
	SlowCall first_call = { 0 }, second_call = { 0 };
	RaceClick first, second;
	gint64 invoice_id = 0;
	guint customers_before, checkouts_before, i;

	g_assert_true(venture_integration_service_set_key(venture_integration_service_get(f->database), secret, &error));
	connection = venture_stripe_settings_configure(f->database, f->organization_id, settings, 0, 0, STRIPE_TRANSPORT(transport), NULL, &error);
	g_assert_no_error(error);
	service = venture_stripe_service_for_organization(f->database, f->organization_id, STRIPE_TRANSPORT(transport), &error);
	g_assert_no_error(error);
	venture_context_set_stripe_service(f->context, service);

	/* The form, and what a browser posts to it: the same answers under
	 * the same payment identity every time. */
	if (!g_strcmp0(mode, "paynow-portal"))
	{
		invoice = invoice_new(f, "RACE-1", "2026-01-01", "100 USD");
		invoice_id = venture_entity_get_id(invoice);
	}
	else
	{
		g_autofree gchar *nonce = NULL, *ticket = NULL, *token = NULL;
		form = race_paid_form(f);
		nonce = venture_forms_payment_nonce(form);
		ticket = venture_forms_ticket_new_for_version(form, 1, earlier);
		token = venture_forms_get_string(form, "public-token");
		form_path = g_strdup_printf("/pub/form/%s", token);
		form_body = g_strdup_printf("name=Guest&email=guest%%40example.test&%s=%s&%s=%s",
			VENTURE_FORMS_TICKET, ticket, VENTURE_FORMS_PAYMENT_NONCE, nonce);
	}
	if (retry)
	{
		/* The form's first attempt lost Stripe's answer to its Checkout:
		 * its invoice, customer and reservation are kept, and the page
		 * asks for a retry with the same answers. */
		g_autoptr(GHashTable) answers = venture_forms_answers_from_urlencoded(form_body, strlen(form_body), &error);
		g_autoptr(VentureEntity) response = NULL;
		g_autoptr(JsonObject) refused = NULL;
		VentureFormsOutcome outcome;
		gboolean ok;
		g_assert_no_error(error);
		fake->checkout_drop = TRUE;
		g_test_expect_message("Venture", G_LOG_LEVEL_MESSAGE,
			"Form payment checkout failed: invoice=* cause=provider transport failure*");
		ok = venture_forms_submit(f->database, form, answers, NULL, now, &outcome, &response, &refused, &error);
		g_test_assert_expected_messages();
		g_assert_no_error(error); g_assert_true(ok);
		g_assert_cmpint(outcome, ==, VENTURE_FORMS_INVALID);
		fake->checkout_drop = FALSE;
		invoice_id = race_form_invoice(f);
	}
	if (paynow)
	{
		g_autoptr(VentureStripePaymentLink) link = venture_stripe_service_create_payment_link(service, invoice_id,
			"https://venture.example", expires, NULL, &error);
		g_autofree gchar *url = NULL;
		g_assert_no_error(error); g_assert_nonnull(link);
		g_object_get(link, "url", &url, NULL);
		pay_path = g_strdup_printf("/pay/%s/checkout", url + strlen("https://venture.example/pay/"));
	}
	if (portal)
	{
		g_autoptr(VentureEntity) paying = venture_database_get(f->database, VENTURE_TYPE_INVOICE, invoice_id, &error);
		g_autoptr(VentureEntity) access = NULL;
		g_autofree gchar *token = NULL;
		gint64 company_id = 0;
		g_assert_no_error(error);
		g_object_get(paying, "company-id", &company_id, NULL);
		access = venture_portal_service_invite(venture_portal_service_get(f->database), f->organization_id,
			company_id, "payer@example.test", NULL, &error);
		g_assert_no_error(error); g_assert_nonnull(access);
		g_object_get(access, "token", &token, NULL);
		portal_path = g_strdup_printf("/portal/%s", token);
		portal_body = g_strdup_printf("invoice_id=%" G_GINT64_FORMAT, invoice_id);
	}

	/* Who clicks first: Pay now, else the portal, else the form. The
	 * second is the form, but for paynow-portal. */
	if (paynow)
	{
		first.method = "GET"; first.path = pay_path; first.body = NULL;
	}
	else if (portal)
	{
		first.method = "POST"; first.path = portal_path; first.body = portal_body;
	}
	else
	{
		first.method = "POST"; first.path = form_path; first.body = form_body;
	}
	second.method = "POST";
	if (paynow && portal)
	{
		second.path = portal_path; second.body = portal_body;
	}
	else
	{
		second.path = form_path; second.body = form_body;
	}

	server = start_server(f, &state_dir);
	customers_before = fake->customers;
	checkouts_before = fake->checkouts;
	first_call.method = first.method; first_call.body = first.body;
	first_call.content_type = first.body ? "application/x-www-form-urlencoded" : NULL;
	second_call.method = second.method; second_call.body = second.body;
	second_call.content_type = "application/x-www-form-urlencoded";
	fake_hold(fake, 1);
	slow_call_start(&first_call, server, first.path);
	fake_wait_held(fake, 1);
	/* A form posted first made its invoice before it reached Stripe. */
	if (form_first)
		invoice_id = race_form_invoice(f);
	key = party_key(f, invoice_id);
	hold_race_start(&second_call, server, second.path, key);
	fake_release_all(fake);
	slow_call_finish(&first_call);
	slow_call_finish(&second_call);
	g_test_message("%s: first %u %s; second %u %s; customers +%u, checkouts +%u, keys refused %d", mode,
		first_call.status, first_call.location, second_call.status, second_call.location,
		fake->customers - customers_before, fake->checkouts - checkouts_before, g_atomic_int_get(&fake->keys_refused));

	g_assert_cmpint(g_atomic_int_get(&fake->keys_refused), ==, 0);
	g_assert_cmpuint(first_call.status, ==, 303);
	g_assert_cmpuint(second_call.status, ==, 303);
	g_assert_nonnull(first_call.location);
	g_assert_true(g_str_has_prefix(first_call.location, "https://checkout.stripe.com/"));
	g_assert_cmpstr(second_call.location, ==, first_call.location);
	/* A form's retry found the customer its first attempt made. */
	g_assert_cmpuint(fake->customers - customers_before, ==, retry ? 0 : 1);
	g_assert_cmpuint(fake->checkouts - checkouts_before, ==, 1);
	customers = rows(f, "stripe_customer_link");
	g_assert_cmpuint(customers->len, ==, 1);
	/* The lost first answer's exception, if any, is resolved; nothing new
	 * is left uncertain. */
	exceptions = rows(f, "processor_exception");
	for (i = 0; i < exceptions->len; i++)
	{
		gboolean resolved = FALSE;
		g_object_get(g_ptr_array_index(exceptions, i), "resolved", &resolved, NULL);
		g_assert_true(resolved);
	}
	g_assert_cmpuint(exceptions->len, ==, retry ? 1 : 0);
	slow_call_clear(&first_call); slow_call_clear(&second_call);
	venture_web_server_stop(server);
	venture_test_remove_tree(state_dir);
}

/* --- The record action route (ticket #40) ---------------------------------- */

/*
 * POST /api/v1/:type/:id/actions/:action answers a Stripe action from a
 * fiber, under the same party hold as Pay now. authorize_payment is the
 * action that meets a Pay now click: both create the company's Stripe
 * customer, under one idempotency key ("venture-customer-..."). On a plain
 * stack it went on beside a Pay now click held creating that customer and
 * sent the key the click still had in flight.
 *
 * paynow-action: a Pay now click held creating the customer; the action.
 * action-paynow: the action held creating the customer; Pay now.
 * action-action: the action clicked twice, the first held on its Setup
 *                (the customer already made): two fibers that could not
 *                race as plain stacks, and would reuse the Setup's key
 *                without the action's own hold.
 */

/* Which stack each action ran on, from the registry's "performed". */
typedef struct
{
	guint	on_fiber;
	guint	on_plain;
} ActionStacks;

static void
action_stacks_performed(VentureActionRegistry *registry, VentureAction *action,
	VentureEntity *result, gpointer data)
{
	ActionStacks *stacks = data;
	(void)registry; (void)action; (void)result;
	if (NULL != venture_fiber_current())
		stacks->on_fiber++;
	else
		stacks->on_plain++;
}

/* A Stripe connection on API 2024-06-20, as automatic collection needs, and
 * a started subscription of the fixture's customer; its id. */
static gint64
race_subscription(Fixture *f, FakeTransport *fake, VentureStripeService **service)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) settings = binding_settings("whsec_original");
	g_autoptr(GBytes) secret = g_bytes_new_static("01234567890123456789012345678901", 32);
	g_autoptr(VentureIntegrationConnection) connection = NULL;
	g_autoptr(VentureEntity) plan = record_new(f, "plan");
	g_autoptr(VentureEntity) price = record_new(f, "plan_price");
	g_autoptr(VentureEntity) start = record_new(f, "billing_request");
	gint64 subscription_id = 0;

	json_object_set_string_member(json_node_get_object(settings), "api_version", "2024-06-20");
	g_assert_true(venture_integration_service_set_key(venture_integration_service_get(f->database), secret, &error));
	connection = venture_stripe_settings_configure(f->database, f->organization_id, settings, 0, 0, STRIPE_TRANSPORT(fake), NULL, &error);
	g_assert_no_error(error);
	*service = venture_stripe_service_for_organization(f->database, f->organization_id, STRIPE_TRANSPORT(fake), &error);
	g_assert_no_error(error);
	venture_context_set_stripe_service(f->context, *service);
	g_object_set(plan, "name", "Managed plan", "active", TRUE, NULL); save(f, plan);
	g_object_set(price, "plan-id", venture_entity_get_id(plan), "currency", "USD", "active", TRUE,
		"product-id", f->product_id, NULL);
	money_field(price, "amount", "50 USD"); save(f, price);
	g_object_set(start, "action", "start", "company-id", f->customer_id,
		"plan-price-id", venture_entity_get_id(price), "seats", (gint64)1, NULL);
	money_field(start, "at", "2026-09-01");
	save(f, start);
	g_object_get(start, "subscription-id", &subscription_id, NULL);
	g_assert_cmpint(subscription_id, >, 0);
	return subscription_id;
}

static void
test_action_race(Fixture *f, gconstpointer data)
{
	const gchar *mode = data;
	gboolean action_first = g_str_has_prefix(mode, "action-");
	gboolean twice = !g_strcmp0(mode, "action-action");
	g_autoptr(GError) error = NULL;
	g_autoptr(GObject) transport = g_object_new(fake_transport_get_type(), NULL);
	FakeTransport *fake = (FakeTransport *)transport;
	g_autoptr(VentureStripeService) service = NULL;
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureWebServer) server = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GDateTime) expires = g_date_time_add_hours(now, 2);
	g_autoptr(GPtrArray) customers = NULL, authorizations = NULL;
	g_autofree gchar *state_dir = NULL, *pay_path = NULL, *action_path = NULL, *key = NULL;
	g_autofree gchar *linked = NULL, *authorized = NULL;
	SlowCall first = { 0 }, second = { 0 };
	SlowCall *action, *paynow;
	ActionStacks stacks = { 0, 0 };
	VentureActionRegistry *registry = venture_database_get_action_registry(f->database);
	gulong performed;
	gint64 subscription_id;
	guint customers_before, checkouts_before;

	subscription_id = race_subscription(f, fake, &service);
	invoice = invoice_new(f, "RACE-A", "2026-01-01", "100 USD");
	action_path = g_strdup_printf("/api/v1/customer_subscription/%" G_GINT64_FORMAT "/actions/authorize_payment", subscription_id);
	if (twice)
	{
		/* The customer is made already, so the first click's first call
		 * to Stripe -- the one held -- is its Setup. */
		g_autoptr(VentureStripeCheckout) made = venture_stripe_service_checkout(service, venture_entity_get_id(invoice), NULL, &error);
		g_assert_no_error(error); g_assert_nonnull(made);
	}
	else
	{
		g_autoptr(VentureStripePaymentLink) link = venture_stripe_service_create_payment_link(service,
			venture_entity_get_id(invoice), "https://venture.example", expires, NULL, &error);
		g_autofree gchar *url = NULL;
		g_assert_no_error(error); g_assert_nonnull(link);
		g_object_get(link, "url", &url, NULL);
		pay_path = g_strdup_printf("/pay/%s/checkout", url + strlen("https://venture.example/pay/"));
	}

	/* Who clicks first, and what each one sends. */
	action = action_first ? &first : &second;
	paynow = twice ? NULL : (action_first ? &second : &first);
	action->method = "POST"; action->content_type = "application/x-www-form-urlencoded"; action->body = "limit=100+USD";
	if (twice)
	{
		second.method = "POST"; second.content_type = "application/x-www-form-urlencoded"; second.body = "limit=100+USD";
	}
	else
		paynow->method = "GET";

	performed = g_signal_connect(registry, "performed", G_CALLBACK(action_stacks_performed), &stacks);
	server = start_server(f, &state_dir);
	customers_before = fake->customers;
	checkouts_before = fake->checkouts;
	key = party_key(f, venture_entity_get_id(invoice));
	fake_hold(fake, 1);
	slow_call_start(&first, server, (action_first ? action_path : pay_path));
	fake_wait_held(fake, 1);
	hold_race_start(&second, server, (twice || !action_first) ? action_path : pay_path, key);
	fake_release_all(fake);
	slow_call_finish(&first);
	slow_call_finish(&second);
	g_signal_handler_disconnect(registry, performed);
	g_test_message("%s: first %u %s; second %u %s; customers +%u, checkouts +%u, keys refused %d; actions on a fiber %u, on a plain stack %u",
		mode, first.status, first.location ? first.location : "-", second.status, second.location ? second.location : "-",
		fake->customers - customers_before, fake->checkouts - checkouts_before, g_atomic_int_get(&fake->keys_refused),
		stacks.on_fiber, stacks.on_plain);

	/* Nobody sent a key still in flight, and every click was answered. */
	g_assert_cmpint(g_atomic_int_get(&fake->keys_refused), ==, 0);
	g_assert_cmpuint(action->status, ==, 200);
	g_assert_nonnull(action->reply);
	g_assert_nonnull(strstr(action->reply, "https://checkout.stripe.com/setup"));
	if (twice)
	{
		/* The second click answered the first one's Setup session. */
		g_assert_cmpuint(first.status, ==, 200);
		g_assert_nonnull(strstr(first.reply, "https://checkout.stripe.com/setup\""));
		g_assert_nonnull(strstr(second.reply, "https://checkout.stripe.com/setup\""));
		g_assert_cmpuint(fake->checkouts - checkouts_before, ==, 0);
		g_assert_cmpuint(stacks.on_fiber, ==, 2);
	}
	else
	{
		g_assert_cmpuint(paynow->status, ==, 303);
		g_assert_nonnull(paynow->location);
		g_assert_true(g_str_has_prefix(paynow->location, "https://checkout.stripe.com/"));
		g_assert_cmpuint(fake->checkouts - checkouts_before, ==, 1);
		g_assert_cmpuint(stacks.on_fiber, ==, 1);
	}
	g_assert_cmpuint(stacks.on_plain, ==, 0);
	/* One Stripe customer, made once, and both used it. */
	g_assert_cmpuint(fake->customers - customers_before, ==, twice ? 0 : 1);
	customers = rows(f, "stripe_customer_link");
	g_assert_cmpuint(customers->len, ==, 1);
	authorizations = rows(f, "stripe_authorization");
	g_assert_cmpuint(authorizations->len, ==, 1);
	g_object_get(g_ptr_array_index(customers, 0), "stripe-customer-id", &linked, NULL);
	g_object_get(g_ptr_array_index(authorizations, 0), "customer-id", &authorized, NULL);
	g_assert_cmpstr(authorized, ==, linked);
	slow_call_clear(&first); slow_call_clear(&second);
	venture_web_server_stop(server);
	venture_test_remove_tree(state_dir);
}

/*
 * Every other action answers on the dispatch's own stack, exactly as it
 * did: the same status and body, the form post's redirect, an unknown
 * action's 404 -- and a Stripe action's refusal from its fiber is the
 * refusal the registry gives.
 */
static void
test_action_route_plain(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GError) refused = NULL;
	g_autoptr(GObject) transport = g_object_new(fake_transport_get_type(), NULL);
	FakeTransport *fake = (FakeTransport *)transport;
	g_autoptr(VentureStripeService) service = NULL;
	g_autoptr(VentureWebServer) server = NULL;
	g_autoptr(VentureEntity) company = NULL, again = NULL;
	g_autoptr(JsonNode) answered = NULL, expected = NULL;
	g_autoptr(GHashTable) nothing = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)json_node_unref);
	g_autofree gchar *state_dir = NULL, *body = NULL, *tag_path = NULL, *unknown_path = NULL;
	g_autofree gchar *stripe_path = NULL, *stripe_body = NULL, *location = NULL;
	VentureActionRegistry *registry = venture_database_get_action_registry(f->database);
	ActionStacks stacks = { 0, 0 };
	SlowCall form = { 0 };
	gulong performed;
	gint64 subscription_id;
	guint status, calls_before;
	(void)data;

	subscription_id = race_subscription(f, fake, &service);
	tag_path = g_strdup_printf("/api/v1/company/%" G_GINT64_FORMAT "/actions/add_tag", f->customer_id);
	unknown_path = g_strdup_printf("/api/v1/company/%" G_GINT64_FORMAT "/actions/no_such_action", f->customer_id);
	stripe_path = g_strdup_printf("/api/v1/customer_subscription/%" G_GINT64_FORMAT "/actions/authorize_payment", subscription_id);
	performed = g_signal_connect(registry, "performed", G_CALLBACK(action_stacks_performed), &stacks);
	server = start_server(f, &state_dir);

	/* JSON: 200 and the record as it now is, from this stack. */
	status = http_request(server, "POST", tag_path, "application/json", "{\"tag\":\"Priority\"}", &body);
	g_test_message("POST %s (JSON) -> %u", tag_path, status);
	g_assert_cmpuint(status, ==, 200);
	company = venture_database_get(f->database, VENTURE_TYPE_COMPANY, f->customer_id, &error);
	g_assert_no_error(error); g_assert_nonnull(company);
	answered = venture_json_parse(body, &error);
	g_assert_no_error(error); g_assert_nonnull(answered);
	expected = venture_serializable_to_json(VENTURE_SERIALIZABLE(company), FALSE);
	g_assert_true(json_node_equal(answered, expected));
	g_assert_cmpuint(stacks.on_plain, ==, 1);
	g_assert_cmpuint(stacks.on_fiber, ==, 0);

	/* A form post: 303 to the record. */
	form.method = "POST"; form.content_type = "application/x-www-form-urlencoded"; form.body = "tag=Renewal";
	slow_call_start(&form, server, tag_path);
	slow_call_finish(&form);
	location = g_strdup_printf("/e/company/%" G_GINT64_FORMAT, f->customer_id);
	g_test_message("POST %s (form) -> %u %s", tag_path, form.status, form.location ? form.location : "-");
	g_assert_cmpuint(form.status, ==, 303);
	g_assert_cmpstr(form.location, ==, location);
	again = venture_database_get(f->database, VENTURE_TYPE_COMPANY, f->customer_id, &error);
	g_assert_no_error(error);
	{
		g_autofree gchar *tags = NULL;
		g_object_get(again, "tags", &tags, NULL);
		g_assert_nonnull(strstr(tags, "Priority")); g_assert_nonnull(strstr(tags, "Renewal"));
	}
	g_assert_cmpuint(stacks.on_plain, ==, 2);
	g_assert_cmpuint(stacks.on_fiber, ==, 0);

	/* An unknown action is refused before any fiber is made. */
	g_clear_pointer(&body, g_free);
	status = http_request(server, "POST", unknown_path, "application/json", "{}", &body);
	g_assert_cmpuint(status, ==, 404);

	/* A Stripe action refused from its fiber says what the registry says:
	 * the limit is required, and Stripe is never asked. */
	g_assert_null(venture_action_registry_perform(registry, "customer_subscription", subscription_id, "authorize_payment",
		nothing, NULL, VENTURE_USER_ROLE_OWNER, &refused));
	g_assert_nonnull(refused);
	g_clear_pointer(&body, g_free);
	calls_before = fake->calls;
	status = http_request(server, "POST", stripe_path, "application/json", "{}", &body);
	stripe_body = g_strdup(body);
	g_test_message("POST %s without a limit -> %u %s (the registry: %s)", stripe_path, status, stripe_body, refused->message);
	g_assert_cmpuint(status, >=, 400); g_assert_cmpuint(status, <, 500);
	g_assert_nonnull(strstr(stripe_body, refused->message));
	g_assert_cmpuint(fake->calls, ==, calls_before);
	g_signal_handler_disconnect(registry, performed);
	slow_call_clear(&form);
	venture_web_server_stop(server);
	venture_test_remove_tree(state_dir);
}
