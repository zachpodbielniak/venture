/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Pay now: the button in every sent invoice email. It is an expiring
 * bearer capability for one invoice (a random token, stored only as its
 * SHA-256 digest) that goes straight to Checkout, with no sign-in and no
 * page between. If it regresses, the customer
 * meets a sign-in page, a portal page or a 404, or a paid invoice is
 * charged a second time. */

typedef struct
{
	GObject *transport;
	VentureStripeService *service;
	VentureEntity *invoice;
	VentureMailMessage *message;
	gchar *token;
} PayNow;

static void
pay_now_clear(PayNow *p)
{
	g_clear_object(&p->message);
	g_clear_object(&p->invoice);
	g_clear_object(&p->service);
	g_clear_object(&p->transport);
	g_clear_pointer(&p->token, g_free);
}

/* A draft invoice with Stripe connected (ACH on when @ach) and a public
 * origin configured. The provider is handed to the context only when
 * @inject: without it the send finds its own, as production does. */
static void
pay_now_prepare(Fixture *f, PayNow *p, gboolean connect, gboolean ach, gboolean inject)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) key = g_bytes_new_static("01234567890123456789012345678901", 32);
	g_autoptr(VentureEntity) customer = NULL;
	memset(p, 0, sizeof(*p));
	p->transport = g_object_new(fake_transport_get_type(), NULL);
	g_object_set(f->config, "server-base-url", "https://venture.example", NULL);
	customer = venture_database_get(f->database, VENTURE_TYPE_COMPANY, f->customer_id, &error);
	g_assert_no_error(error);
	g_object_set(customer, "email", "customer@example.test", NULL);
	save(f, customer);
	if (connect)
	{
		g_autoptr(JsonNode) settings = binding_settings("whsec_original");
		g_autoptr(VentureIntegrationConnection) connection = NULL;
		json_object_set_boolean_member(json_node_get_object(settings), "ach_enabled", ach);
		g_assert_true(venture_integration_service_set_key(venture_integration_service_get(f->database), key, &error));
		connection = venture_stripe_settings_configure(f->database, f->organization_id, settings, 0, 0,
			STRIPE_TRANSPORT(p->transport), NULL, &error);
		g_assert_no_error(error);
		p->service = venture_stripe_service_for_organization(f->database, f->organization_id,
			STRIPE_TRANSPORT(p->transport), &error);
		g_assert_no_error(error);
		if (inject)
			venture_context_set_stripe_service(f->context, p->service);
	}
	/* A draft: Send issues it, exactly as the button on the invoice does. */
	p->invoice = record_new(f, "invoice");
	g_object_set(p->invoice, "number", "PAYNOW-1", "company-id", f->customer_id, NULL);
	money_field(p->invoice, "issued-at", "2026-01-01");
	money_field(p->invoice, "due-at", "2026-01-31");
	save(f, p->invoice);
	{
		g_autoptr(VentureEntity) line = record_new(f, "invoice_line");
		g_object_set(line, "invoice-id", venture_entity_get_id(p->invoice),
			"description", "Work", "quantity", 1.0, "product-id", f->product_id, NULL);
		money_field(line, "unit-price", "100 USD");
		save(f, line);
	}
}

/* Sends the prepared draft through the ordinary Send path. */
static void
pay_now_deliver(Fixture *f, PayNow *p)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *private_html = NULL;
	const gchar *at;
	p->message = venture_mail_send_invoice(f->context, f->organization_id,
		venture_entity_get_id(p->invoice), NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(p->message);
	g_object_get(p->message, "private-html-body", &private_html, NULL);
	at = private_html ? strstr(private_html, "https://venture.example/pay/") : NULL;
	if (at)
		p->token = g_strndup(at + strlen("https://venture.example/pay/"), 101);
}

/* An invoice sent by email through the ordinary Send path, with Stripe
 * connected (ACH on when @ach) and a public origin configured. */
static void
pay_now_send(Fixture *f, PayNow *p, gboolean connect, gboolean ach)
{
	pay_now_prepare(f, p, connect, ach, TRUE);
	pay_now_deliver(f, p);
}

static VentureWebServer *
pay_now_server(Fixture *f, gchar **state_directory, gboolean hosted)
{
	g_autoptr(GError) error = NULL;
	if (hosted)
	{
		VentureTenantService *tenant = venture_tenant_service_get(f->database);
		g_object_set(f->config, "hosted-enabled", TRUE,
			"hosted-workspace-id", "8f062b79-1d2b-4d7f-99e5-bd3bf588e05a",
			"hosted-origin", "https://venture.example", NULL);
		g_assert_true(venture_tenant_service_configure(tenant, f->config, &error));
		g_assert_no_error(error);
		g_assert_true(venture_tenant_service_initialize(tenant, &error));
		g_assert_no_error(error);
	}
	return start_server(f, state_directory);
}

/* The email carries the button; the stored public body does not carry the
 * bearer link, which only the transport sends. */
static void
test_pay_now_email(Fixture *f, gconstpointer data)
{
	PayNow p;
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) links = NULL;
	g_autoptr(VentureStripePaymentLink) resolved = NULL;
	g_autofree gchar *html = NULL, *text = NULL, *private_html = NULL, *private_text = NULL;
	g_autofree gchar *button = NULL;
	gint64 invoice_id;
	(void)data;
	pay_now_send(f, &p, TRUE, FALSE);
	g_object_get(p.message, "html-body", &html, "text-body", &text,
		"private-html-body", &private_html, "private-text-body", &private_text, NULL);
	g_assert_nonnull(p.token);
	g_assert_nonnull(strstr(private_html, "Pay now"));
	button = g_strdup_printf("https://venture.example/pay/%s/checkout", p.token);
	g_assert_nonnull(strstr(private_html, button));
	g_assert_nonnull(private_text);
	g_assert_nonnull(strstr(private_text, button));
	/* The invoice itself is still in the message the customer gets. */
	g_assert_nonnull(strstr(private_html, "100.00"));
	g_assert_null(strstr(html, "/pay/"));
	g_assert_null(strstr(text ? text : "", "/pay/"));
	links = rows(f, "stripe_payment_link");
	g_assert_cmpuint(links->len, ==, 1);
	g_object_get(g_ptr_array_index(links, 0), "invoice-id", &invoice_id, NULL);
	g_assert_cmpint(invoice_id, ==, venture_entity_get_id(p.invoice));
	/* Bound to the invoice as it was sent, so it resolves now. */
	resolved = venture_stripe_service_resolve_payment_link(f->database, p.token, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(resolved);
	/* No Stripe call is made to send an invoice. */
	g_assert_cmpuint(((FakeTransport *)p.transport)->checkouts, ==, 0);
	pay_now_clear(&p);
}

/* Without Stripe the invoice still goes out, without a button. */
static void
test_pay_now_without_stripe(Fixture *f, gconstpointer data)
{
	PayNow p;
	g_autofree gchar *html = NULL, *private_html = NULL;
	(void)data;
	pay_now_send(f, &p, FALSE, FALSE);
	g_object_get(p.message, "html-body", &html, "private-html-body", &private_html, NULL);
	g_assert_null(p.token);
	g_assert_null(strstr(html, "Pay now"));
	g_assert_true(!private_html || !strstr(private_html, "Pay now"));
	pay_now_clear(&p);
}

/* One click: the link opens Checkout for that invoice. Only a verified
 * completion records the payment; after it the link says paid. */
static void
test_pay_now_click(Fixture *f, gconstpointer data)
{
	PayNow p;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureWebServer) server = NULL;
	g_autoptr(VentureEntity) invoice = NULL;
	g_autofree gchar *state_directory = NULL, *checkout = NULL, *page = NULL, *body = NULL;
	gint status;
	(void)data;
	pay_now_send(f, &p, TRUE, FALSE);
	g_assert_nonnull(p.token);
	checkout = g_strdup_printf("/pay/%s/checkout", p.token);
	page = g_strdup_printf("/pay/%s", p.token);
	server = pay_now_server(f, &state_directory, FALSE);
	/* A plain GET, as a mail client makes it: no Origin, no form. */
	g_assert_cmpuint(http_request(server, "GET", checkout, NULL, NULL, NULL), ==, 303);
	g_assert_cmpuint(((FakeTransport *)p.transport)->checkouts, ==, 1);
	g_assert_false(((FakeTransport *)p.transport)->checkout_ach);
	/* A second click reuses the open session rather than opening another. */
	g_assert_cmpuint(http_request(server, "GET", checkout, NULL, NULL, NULL), ==, 303);
	g_assert_cmpuint(((FakeTransport *)p.transport)->checkouts, ==, 1);
	/* The invoice page carries the same button. */
	g_assert_cmpuint(http_request(server, "GET", page, NULL, NULL, &body), ==, 200);
	g_assert_nonnull(strstr(body, "Pay now"));
	g_assert_nonnull(strstr(body, checkout));
	g_clear_pointer(&body, g_free);
	/* Opening Checkout is not paying. */
	invoice = venture_database_get(f->database, VENTURE_TYPE_INVOICE, venture_entity_get_id(p.invoice), &error);
	g_assert_no_error(error);
	g_object_get(invoice, "status", &status, NULL);
	g_assert_cmpint(status, ==, VENTURE_INVOICE_STATUS_SENT);
	g_clear_object(&invoice);
	{
		const gchar *event = "{\"id\":\"evt_pay_now\",\"livemode\":false,\"type\":\"checkout.session.completed\",\"data\":{\"object\":{\"id\":\"cs_offline\",\"amount_total\":10000,\"currency\":\"usd\",\"payment_status\":\"paid\",\"payment_intent\":\"pi_pay_now\"}}}";
		g_autofree gchar *sig = binding_signature(event, "whsec_original");
		g_autoptr(GBytes) raw = g_bytes_new(event, strlen(event));
		g_assert_true(venture_stripe_service_handle_webhook(p.service, raw, sig, &error));
		g_assert_no_error(error);
	}
	invoice = venture_database_get(f->database, VENTURE_TYPE_INVOICE, venture_entity_get_id(p.invoice), &error);
	g_assert_no_error(error);
	g_object_get(invoice, "status", &status, NULL);
	g_assert_cmpint(status, ==, VENTURE_INVOICE_STATUS_PAID);
	/* Paid says so, and does not charge again. */
	g_assert_cmpuint(http_request(server, "GET", checkout, NULL, NULL, &body), ==, 200);
	g_assert_nonnull(strstr(body, "This invoice is paid"));
	g_assert_null(strstr(body, "Pay now"));
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(http_request(server, "GET", page, NULL, NULL, &body), ==, 200);
	g_assert_nonnull(strstr(body, "This invoice is paid"));
	g_assert_cmpuint(((FakeTransport *)p.transport)->checkouts, ==, 1);
	/* A tampered link is still the same unavailable answer. */
	g_clear_pointer(&body, g_free);
	{
		/* Still well formed, so the digest is what refuses it. */
		gchar *last = checkout + strlen(checkout) - strlen("/checkout") - 1;
		*last = *last == 'a' ? 'b' : 'a';
	}
	g_assert_cmpuint(http_request(server, "GET", checkout, NULL, NULL, &body), ==, 404);
	g_assert_null(strstr(body, "paid"));
	venture_web_server_stop(server);
	venture_test_remove_tree(state_directory);
	pay_now_clear(&p);
}

/* A voided invoice says so and never reaches Stripe. */
static void
test_pay_now_void(Fixture *f, gconstpointer data)
{
	PayNow p;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureWebServer) server = NULL;
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autofree gchar *state_directory = NULL, *checkout = NULL, *body = NULL;
	(void)data;
	pay_now_send(f, &p, TRUE, FALSE);
	/* Read after sending: a void may not predate the issue. */
	now = venture_time_now();
	g_assert_nonnull(p.token);
	invoice = venture_database_get(f->database, VENTURE_TYPE_INVOICE, venture_entity_get_id(p.invoice), &error);
	g_assert_no_error(error);
	{
		gboolean voided = venture_settlement_service_transition(venture_settlement_service_get(f->database),
			VENTURE_INVOICE(invoice), "void", now, NULL, &error);
		g_assert_no_error(error);
		g_assert_true(voided);
	}
	checkout = g_strdup_printf("/pay/%s/checkout", p.token);
	server = pay_now_server(f, &state_directory, FALSE);
	g_assert_cmpuint(http_request(server, "GET", checkout, NULL, NULL, &body), ==, 200);
	g_assert_nonnull(strstr(body, "This invoice was voided"));
	g_assert_null(strstr(body, "Pay now"));
	g_assert_cmpuint(((FakeTransport *)p.transport)->checkouts, ==, 0);
	venture_web_server_stop(server);
	venture_test_remove_tree(state_directory);
	pay_now_clear(&p);
}

/* Card always; ACH as well where the business has it on. */
static void
test_pay_now_ach(Fixture *f, gconstpointer data)
{
	PayNow p;
	g_autoptr(VentureWebServer) server = NULL;
	g_autofree gchar *state_directory = NULL, *checkout = NULL;
	(void)data;
	pay_now_send(f, &p, TRUE, TRUE);
	g_assert_nonnull(p.token);
	checkout = g_strdup_printf("/pay/%s/checkout", p.token);
	server = pay_now_server(f, &state_directory, FALSE);
	g_assert_cmpuint(http_request(server, "GET", checkout, NULL, NULL, NULL), ==, 303);
	g_assert_cmpuint(((FakeTransport *)p.transport)->checkouts, ==, 1);
	g_assert_true(((FakeTransport *)p.transport)->checkout_ach);
	venture_web_server_stop(server);
	venture_test_remove_tree(state_directory);
	pay_now_clear(&p);
}

/* On the public customer-link address: no session, no Origin, no Referer,
 * which is how a link from an email arrives. A form post from there came
 * with Origin null and was refused; a link must not depend on one. */
static void
test_pay_now_public_origin(Fixture *f, gconstpointer data)
{
	PayNow p;
	g_autoptr(VentureWebServer) server = NULL;
	g_autofree gchar *state_directory = NULL, *checkout = NULL, *page = NULL, *body = NULL;
	(void)data;
	pay_now_send(f, &p, TRUE, FALSE);
	g_assert_nonnull(p.token);
	checkout = g_strdup_printf("/pay/%s/checkout", p.token);
	page = g_strdup_printf("/pay/%s", p.token);
	server = pay_now_server(f, &state_directory, TRUE);
	g_assert_cmpuint(http_request_origin(server, "GET", page, NULL, NULL, &body,
		"venture.example", NULL, "same-origin"), ==, 200);
	g_assert_nonnull(strstr(body, "Pay now"));
	g_assert_cmpuint(http_request_origin(server, "GET", checkout, NULL, NULL, NULL,
		"venture.example", NULL, "same-origin"), ==, 303);
	g_assert_cmpuint(((FakeTransport *)p.transport)->checkouts, ==, 1);
	/* Nothing else of the workspace answers there without a session. */
	g_assert_cmpuint(http_request_origin(server, "GET", "/pay/invalid/checkout", NULL, NULL, NULL,
		"venture.example", NULL, NULL), ==, 404);
	venture_web_server_stop(server);
	venture_test_remove_tree(state_directory);
	pay_now_clear(&p);
}

/* The production path: no provider is handed to the context, so Send finds
 * its own through the organization's connection, and the click finds the
 * one for the link's connection. Only the Stripe call is not exercised
 * here, because the web path builds a real transport. */
static void
test_pay_now_production_path(Fixture *f, gconstpointer data)
{
	PayNow p;
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) links = NULL;
	g_autoptr(VentureStripePaymentLink) resolved = NULL;
	g_autoptr(VentureStripeService) found = NULL;
	gint64 connection_id = 0;
	(void)data;
	pay_now_prepare(f, &p, TRUE, FALSE, FALSE);
	g_assert_null(venture_context_get_stripe_service(f->context));
	pay_now_deliver(f, &p);
	g_assert_nonnull(p.token);
	links = rows(f, "stripe_payment_link");
	g_assert_cmpuint(links->len, ==, 1);
	g_object_get(g_ptr_array_index(links, 0), "connection-id", &connection_id, NULL);
	resolved = venture_stripe_service_resolve_payment_link(f->database, p.token, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(resolved);
	found = venture_stripe_service_for_connection(f->database, f->organization_id,
		connection_id, FALSE, STRIPE_TRANSPORT(p.transport), &error);
	g_assert_no_error(error);
	g_assert_nonnull(found);
	pay_now_clear(&p);
}

/* The bearer link reaches the mail transport only. The send API, the
 * preview and a stored message read never show it. */
static void
test_pay_now_never_shown(Fixture *f, gconstpointer data)
{
	PayNow p;
	g_autoptr(VentureWebServer) server = NULL;
	g_autoptr(GPtrArray) links = NULL;
	g_autoptr(GPtrArray) mail = NULL;
	g_autofree gchar *state_directory = NULL, *path = NULL, *body = NULL;
	gint64 invoice_id;
	guint i;
	(void)data;
	pay_now_prepare(f, &p, TRUE, FALSE, TRUE);
	invoice_id = venture_entity_get_id(p.invoice);
	server = pay_now_server(f, &state_directory, FALSE);
	/* Send through the API, as a client does. */
	path = g_strdup_printf("/api/v1/invoices/%" G_GINT64_FORMAT "/send", invoice_id);
	g_assert_cmpuint(http_request(server, "POST", path, "application/json", "{}", &body), <, 300);
	g_assert_null(strstr(body ? body : "", "/pay/"));
	g_clear_pointer(&body, g_free);
	/* A link was really made, so the checks below are not vacuous. */
	links = rows(f, "stripe_payment_link");
	g_assert_cmpuint(links->len, ==, 1);
	/* The preview, page and API. */
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/mail/preview/invoice/%" G_GINT64_FORMAT, invoice_id);
	g_assert_cmpuint(http_request(server, "GET", path, NULL, NULL, &body), ==, 200);
	g_assert_null(strstr(body, "/pay/"));
	g_clear_pointer(&body, g_free);
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/api/v1/mail/preview/invoice/%" G_GINT64_FORMAT, invoice_id);
	g_assert_cmpuint(http_request(server, "GET", path, NULL, NULL, &body), ==, 200);
	g_assert_null(strstr(body, "/pay/"));
	g_clear_pointer(&body, g_free);
	/* Every stored message, read back through the API. */
	mail = rows(f, "mail_message");
	g_assert_cmpuint(mail->len, >, 0);
	for (i = 0; i < mail->len; i++)
	{
		g_autofree gchar *message_path = g_strdup_printf("/api/v1/mail_message/%" G_GINT64_FORMAT,
			venture_entity_get_id(g_ptr_array_index(mail, i)));
		g_assert_cmpuint(http_request(server, "GET", message_path, NULL, NULL, &body), ==, 200);
		g_assert_null(strstr(body, "/pay/"));
		g_clear_pointer(&body, g_free);
	}
	venture_web_server_stop(server);
	venture_test_remove_tree(state_directory);
	pay_now_clear(&p);
}

/* An invoice edited after it was sent: the button in the email is for the
 * invoice as it was, and says unavailable. */
static void
test_pay_now_edited_invoice(Fixture *f, gconstpointer data)
{
	PayNow p;
	g_autoptr(VentureWebServer) server = NULL;
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *state_directory = NULL, *checkout = NULL;
	(void)data;
	pay_now_send(f, &p, TRUE, FALSE);
	g_assert_nonnull(p.token);
	checkout = g_strdup_printf("/pay/%s/checkout", p.token);
	server = pay_now_server(f, &state_directory, FALSE);
	invoice = venture_database_get(f->database, VENTURE_TYPE_INVOICE, venture_entity_get_id(p.invoice), &error);
	g_assert_no_error(error);
	g_object_set(invoice, "notes", "Revised terms", NULL);
	save(f, invoice);
	g_assert_cmpuint(http_request(server, "GET", checkout, NULL, NULL, NULL), ==, 404);
	g_assert_cmpuint(((FakeTransport *)p.transport)->checkouts, ==, 0);
	venture_web_server_stop(server);
	venture_test_remove_tree(state_directory);
	pay_now_clear(&p);
}

/* The button lasts thirty days, not a day longer. */
static void
test_pay_now_expiry(Fixture *f, gconstpointer data)
{
	PayNow p;
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GDateTime) inside = g_date_time_add_days(now, VENTURE_STRIPE_INVOICE_PAY_DAYS - 1);
	g_autoptr(GDateTime) past = g_date_time_add_days(now, VENTURE_STRIPE_INVOICE_PAY_DAYS + 1);
	g_autoptr(VentureStripePaymentLink) link = NULL;
	(void)data;
	pay_now_send(f, &p, TRUE, FALSE);
	g_assert_nonnull(p.token);
	link = venture_stripe_service_resolve_payment_link(f->database, p.token, inside, &error);
	g_assert_no_error(error);
	g_assert_nonnull(link);
	g_clear_object(&link);
	link = venture_stripe_service_resolve_payment_link(f->database, p.token, past, &error);
	g_assert_null(link);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_clear_error(&error);
	/* And opening Checkout with it is refused without a Stripe call. */
	g_assert_null(venture_stripe_service_pay_link(p.service, p.token, past, &error));
	g_assert_nonnull(error);
	g_assert_cmpuint(((FakeTransport *)p.transport)->checkouts, ==, 0);
	pay_now_clear(&p);
}

/* ACH is offered only where the business has it on. */
static void
test_pay_now_ach_off(Fixture *f, gconstpointer data)
{
	PayNow p;
	g_autoptr(VentureWebServer) server = NULL;
	g_autofree gchar *state_directory = NULL, *checkout = NULL;
	(void)data;
	pay_now_send(f, &p, TRUE, FALSE);
	g_assert_nonnull(p.token);
	checkout = g_strdup_printf("/pay/%s/checkout", p.token);
	server = pay_now_server(f, &state_directory, FALSE);
	g_assert_cmpuint(http_request(server, "GET", checkout, NULL, NULL, NULL), ==, 303);
	g_assert_cmpuint(((FakeTransport *)p.transport)->checkouts, ==, 1);
	g_assert_false(((FakeTransport *)p.transport)->checkout_ach);
	venture_web_server_stop(server);
	venture_test_remove_tree(state_directory);
	pay_now_clear(&p);
}
