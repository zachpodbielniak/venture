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

/* --- The emailed button beside a shared link -------------------------------- */

/* Issues the prepared draft without sending it, as marking it sent does. */
static void
pay_now_issue(Fixture *f, PayNow *p)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	gboolean issued;
	issued = venture_settlement_service_transition(venture_settlement_service_get(f->database),
		VENTURE_INVOICE(p->invoice), "sent", now, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(issued);
}

/* Creates a shared link for the invoice, as Create payment link does, and
 * returns its token. */
static gchar *
pay_now_share(Fixture *f, PayNow *p)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureStripePaymentLink) link = NULL;
	g_autofree gchar *url = NULL;
	gint origin;
	(void)f;
	link = venture_stripe_service_create_payment_link(p->service, venture_entity_get_id(p->invoice),
		"https://venture.example", NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(link);
	g_object_get(link, "url", &url, "origin", &origin, NULL);
	g_assert_cmpint(origin, ==, VENTURE_STRIPE_PAYMENT_LINK_ORIGIN_SHARED);
	g_assert_true(g_str_has_prefix(url, "https://venture.example/pay/"));
	return g_strdup(url + strlen("https://venture.example/pay/"));
}

/* Enabled links of @origin for the prepared invoice. */
static guint
pay_now_enabled(Fixture *f, VentureStripePaymentLinkOrigin origin)
{
	g_autoptr(GPtrArray) links = rows(f, "stripe_payment_link");
	guint i, count = 0;
	for (i = 0; i < links->len; i++)
	{
		gboolean enabled;
		gint kind;
		g_object_get(g_ptr_array_index(links, i), "enabled", &enabled, "origin", &kind, NULL);
		if (enabled && kind == (gint)origin) count++;
	}
	return count;
}

static gboolean
pay_now_resolves(Fixture *f, const gchar *token)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureStripePaymentLink) link = venture_stripe_service_resolve_payment_link(f->database, token, NULL, &error);
	return link != NULL;
}

/* A shared link is enabled when the invoice is sent. Sending used to fail
 * on the one-link-per-invoice index; now both links work. */
static void
test_pay_now_shared_then_send(Fixture *f, gconstpointer data)
{
	PayNow p;
	g_autoptr(VentureWebServer) server = NULL;
	g_autofree gchar *state_directory = NULL, *shared = NULL, *emailed = NULL, *shared_path = NULL;
	(void)data;
	pay_now_prepare(f, &p, TRUE, FALSE, TRUE);
	pay_now_issue(f, &p);
	shared = pay_now_share(f, &p);
	pay_now_deliver(f, &p);
	g_assert_nonnull(p.token);
	g_assert_cmpstr(p.token, !=, shared);
	g_assert_cmpuint(pay_now_enabled(f, VENTURE_STRIPE_PAYMENT_LINK_ORIGIN_SHARED), ==, 1);
	g_assert_cmpuint(pay_now_enabled(f, VENTURE_STRIPE_PAYMENT_LINK_ORIGIN_EMAIL), ==, 1);
	g_assert_true(pay_now_resolves(f, shared));
	g_assert_true(pay_now_resolves(f, p.token));
	/* Both open Checkout; the second click reuses the one open session. */
	emailed = g_strdup_printf("/pay/%s/checkout", p.token);
	shared_path = g_strdup_printf("/pay/%s/checkout", shared);
	server = pay_now_server(f, &state_directory, FALSE);
	g_assert_cmpuint(http_request(server, "GET", emailed, NULL, NULL, NULL), ==, 303);
	g_assert_cmpuint(http_request(server, "GET", shared_path, NULL, NULL, NULL), ==, 303);
	g_assert_cmpuint(((FakeTransport *)p.transport)->checkouts, ==, 1);
	venture_web_server_stop(server);
	venture_test_remove_tree(state_directory);
	pay_now_clear(&p);
}

/* Sharing a link after the email went out used to revoke every enabled
 * link, killing the customer's button without a word. It revokes only an
 * earlier shared link now. */
static void
test_pay_now_send_then_share(Fixture *f, gconstpointer data)
{
	PayNow p;
	g_autoptr(VentureWebServer) server = NULL;
	g_autofree gchar *state_directory = NULL, *first = NULL, *second = NULL, *checkout = NULL;
	(void)data;
	pay_now_send(f, &p, TRUE, FALSE);
	g_assert_nonnull(p.token);
	first = pay_now_share(f, &p);
	second = pay_now_share(f, &p);
	/* The second shared link replaced the first, and only the first. */
	g_assert_false(pay_now_resolves(f, first));
	g_assert_true(pay_now_resolves(f, second));
	g_assert_true(pay_now_resolves(f, p.token));
	g_assert_cmpuint(pay_now_enabled(f, VENTURE_STRIPE_PAYMENT_LINK_ORIGIN_SHARED), ==, 1);
	g_assert_cmpuint(pay_now_enabled(f, VENTURE_STRIPE_PAYMENT_LINK_ORIGIN_EMAIL), ==, 1);
	checkout = g_strdup_printf("/pay/%s/checkout", p.token);
	server = pay_now_server(f, &state_directory, FALSE);
	g_assert_cmpuint(http_request(server, "GET", checkout, NULL, NULL, NULL), ==, 303);
	g_assert_cmpuint(((FakeTransport *)p.transport)->checkouts, ==, 1);
	/* A shared link made after the button opened Checkout leaves that
	 * session alone: it is the invoice's one attempt, and both reuse it. */
	g_clear_pointer(&second, g_free);
	second = pay_now_share(f, &p);
	g_assert_cmpuint(http_request(server, "GET", checkout, NULL, NULL, NULL), ==, 303);
	g_assert_cmpuint(((FakeTransport *)p.transport)->checkouts, ==, 1);
	venture_web_server_stop(server);
	venture_test_remove_tree(state_directory);
	pay_now_clear(&p);
}

/* Minting the button again replaces the emailed one and leaves a shared
 * link be. Send itself never mints twice: a second Send of the same invoice
 * returns the message already queued. */
static void
test_pay_now_resend(Fixture *f, gconstpointer data)
{
	PayNow p;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureMailMessage) again = NULL;
	g_autoptr(GPtrArray) links = NULL;
	g_autofree gchar *shared = NULL, *url = NULL, *token = NULL;
	(void)data;
	pay_now_send(f, &p, TRUE, FALSE);
	g_assert_nonnull(p.token);
	shared = pay_now_share(f, &p);
	again = venture_mail_send_invoice(f->context, f->organization_id, venture_entity_get_id(p.invoice), NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_entity_get_id(VENTURE_ENTITY(again)), ==, venture_entity_get_id(VENTURE_ENTITY(p.message)));
	links = rows(f, "stripe_payment_link");
	g_assert_cmpuint(links->len, ==, 2);
	/* Outside any transaction, as a caller of the public function may be. */
	url = venture_stripe_service_invoice_pay_url(p.service, p.invoice, "https://venture.example", NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(url);
	g_assert_true(g_str_has_prefix(url, "https://venture.example/pay/"));
	token = g_strndup(url + strlen("https://venture.example/pay/"), 101);
	g_assert_cmpstr(token, !=, p.token);
	g_assert_false(pay_now_resolves(f, p.token));
	g_assert_true(pay_now_resolves(f, token));
	g_assert_true(pay_now_resolves(f, shared));
	g_assert_cmpuint(pay_now_enabled(f, VENTURE_STRIPE_PAYMENT_LINK_ORIGIN_EMAIL), ==, 1);
	g_assert_cmpuint(pay_now_enabled(f, VENTURE_STRIPE_PAYMENT_LINK_ORIGIN_SHARED), ==, 1);
	pay_now_clear(&p);
}

static gint64
pay_now_count(Fixture *f, const gchar *sql)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(OrmResult) result = orm_connection_query(venture_database_get_connection(f->database), sql, &error);
	g_assert_no_error(error);
	g_assert_true(orm_result_next(result));
	return orm_row_get_integer(orm_result_get_row(result), 0);
}

/* A database from before origins: links have no origin, and the old index
 * allows one enabled link per invoice. Reconciliation adds the column and
 * the new index but leaves the old index, which still refuses the button;
 * the migration drops it, and the old link reads as shared. */
static void
test_pay_now_origin_migration(Fixture *f, gconstpointer data)
{
	PayNow p;
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) links = NULL;
	g_autofree gchar *shared = NULL, *migration = NULL, *url = NULL;
	OrmConnection *connection = venture_database_get_connection(f->database);
	OrmDialectType dialect = orm_engine_get_dialect_type(orm_connection_get_engine(connection));
	const gchar *old_index = dialect == ORM_DIALECT_SQLITE
		? "SELECT COUNT(*) FROM sqlite_master WHERE type = 'index' AND name = 'uq_stripe_payment_links_organization_invoice_id_when_enabled'"
		: "SELECT CAST(COUNT(*) AS BIGINT) FROM pg_indexes WHERE schemaname = current_schema() AND indexname = 'uq_stripe_payment_links_organization_invoice_id_when_enabled'";
	gint origin;
	gboolean ok;
	(void)data;
	pay_now_prepare(f, &p, TRUE, FALSE, TRUE);
	pay_now_issue(f, &p);
	shared = pay_now_share(f, &p);
	/* Back to the old schema: no origin in the row, the old index. */
	g_assert_cmpint(pay_now_count(f, old_index), ==, 0);
	g_assert_true(venture_database_execute(f->database, "UPDATE stripe_payment_links SET origin = NULL", NULL, &error));
	g_assert_no_error(error);
	g_assert_true(venture_database_execute(f->database,
		"CREATE UNIQUE INDEX uq_stripe_payment_links_organization_invoice_id_when_enabled ON stripe_payment_links "
		"(\"organization_id\", \"invoice_id\") WHERE \"invoice_id\" IS NOT NULL AND \"enabled\"", NULL, &error));
	g_assert_no_error(error);
	/* A NULL origin already reads as shared. */
	links = rows(f, "stripe_payment_link");
	g_assert_cmpuint(links->len, ==, 1);
	g_object_get(g_ptr_array_index(links, 0), "origin", &origin, NULL);
	g_assert_cmpint(origin, ==, VENTURE_STRIPE_PAYMENT_LINK_ORIGIN_SHARED);
	g_clear_pointer(&links, g_ptr_array_unref);
	/* Reconciliation alone keeps the old index, and the button fails. */
	ok = venture_schema_create_table(connection, VENTURE_TYPE_STRIPE_PAYMENT_LINK, &error);
	g_assert_no_error(error);
	g_assert_true(ok);
	g_assert_cmpint(pay_now_count(f, old_index), ==, 1);
	url = venture_stripe_service_invoice_pay_url(p.service, p.invoice, "https://venture.example", NULL, &error);
	g_assert_null(url);
	g_assert_nonnull(error);
	g_clear_error(&error);
	/* The migration, as startup runs it. */
	g_assert_true(g_file_get_contents(dialect == ORM_DIALECT_SQLITE
		? "migrations/sqlite/001081_stripe_payment_link_origin.sql"
		: "migrations/postgresql/001081_stripe_payment_link_origin.sql", &migration, NULL, &error));
	g_assert_no_error(error);
	g_assert_true(venture_database_begin(f->database, &error));
	g_assert_true(venture_migrations_execute_sql(connection, migration, &error));
	g_assert_no_error(error);
	g_assert_true(venture_database_commit(f->database, &error));
	g_assert_no_error(error);
	g_assert_cmpint(pay_now_count(f, old_index), ==, 0);
	g_assert_cmpint(pay_now_count(f, "SELECT COUNT(*) FROM stripe_payment_links WHERE origin = 'shared'"), ==, 1);
	/* The old link and the button now stand side by side. */
	pay_now_deliver(f, &p);
	g_assert_nonnull(p.token);
	g_assert_true(pay_now_resolves(f, shared));
	g_assert_true(pay_now_resolves(f, p.token));
	/* And the new index still holds one enabled link per origin. */
	g_assert_false(venture_database_execute(f->database,
		"UPDATE stripe_payment_links SET enabled = TRUE, origin = 'email'", NULL, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	pay_now_clear(&p);
}

/* --- An open Checkout past its deadline ------------------------------------- */

static void
pay_now_webhook(PayNow *p, const gchar *event, gboolean expect)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *sig = binding_signature(event, "whsec_original");
	g_autoptr(GBytes) raw = g_bytes_new(event, strlen(event));
	gboolean handled = venture_stripe_service_handle_webhook(p->service, raw, sig, &error);
	if (expect) g_assert_no_error(error);
	else g_assert_nonnull(error);
	g_assert_true(handled == expect);
}

static VentureEntity *
pay_now_checkout(Fixture *f, const gchar *session)
{
	g_autoptr(GPtrArray) attempts = rows(f, "stripe_checkout");
	guint i;
	for (i = 0; i < attempts->len; i++)
	{
		g_autofree gchar *id = NULL;
		g_object_get(g_ptr_array_index(attempts, i), "session-id", &id, NULL);
		if (!g_strcmp0(id, session)) return g_object_ref(g_ptr_array_index(attempts, i));
	}
	return NULL;
}

static void
pay_now_assert_checkout(Fixture *f, const gchar *session, const gchar *status, gboolean active)
{
	g_autoptr(VentureEntity) row = pay_now_checkout(f, session);
	g_autofree gchar *state = NULL;
	gboolean live;
	g_assert_nonnull(row);
	g_object_get(row, "status", &state, "active", &live, NULL);
	g_assert_cmpstr(state, ==, status);
	g_assert_true(live == active);
}

/* Moves a stored session's deadline. Stripe evidence is written only by the
 * service, so this goes under it, as the clock passing would. */
static void
pay_now_set_deadline(Fixture *f, const gchar *session, GDateTime *when)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = venture_time_to_string(when);
	g_autofree gchar *sql = g_strdup_printf("UPDATE stripe_checkouts SET expires_at = '%s' WHERE session_id = '%s'", text, session);
	gboolean ok = venture_database_execute(f->database, sql, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(ok);
}

/* The customer opened Checkout and came back after it expired, before
 * Stripe's expiry event arrived. The stored session cannot be paid, so the
 * click expires it here and opens a new one instead of sending the customer
 * to a dead page. The late expiry event then changes nothing, and a
 * completion for the old session still records the payment once no other
 * attempt can collect. */
static void
test_pay_now_expired_session(Fixture *f, gconstpointer data)
{
	PayNow p;
	g_autoptr(VentureWebServer) server = NULL;
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GDateTime) gone = g_date_time_add_minutes(now, -5);
	g_autoptr(GDateTime) soon = g_date_time_add_minutes(now, 5);
	g_autofree gchar *state_directory = NULL, *checkout = NULL;
	gint64 checkout_id;
	gint status;
	(void)data;
	pay_now_send(f, &p, TRUE, FALSE);
	g_assert_nonnull(p.token);
	checkout = g_strdup_printf("/pay/%s/checkout", p.token);
	server = pay_now_server(f, &state_directory, FALSE);
	g_assert_cmpuint(http_request(server, "GET", checkout, NULL, NULL, NULL), ==, 303);
	g_assert_cmpuint(((FakeTransport *)p.transport)->checkouts, ==, 1);
	/* Near its deadline but not past it: still payable, still reused. */
	pay_now_set_deadline(f, "cs_offline", soon);
	g_assert_cmpuint(http_request(server, "GET", checkout, NULL, NULL, NULL), ==, 303);
	g_assert_cmpuint(((FakeTransport *)p.transport)->checkouts, ==, 1);
	pay_now_assert_checkout(f, "cs_offline", "open", TRUE);
	/* Past it: expired here, and a new session opens. */
	pay_now_set_deadline(f, "cs_offline", gone);
	g_assert_cmpuint(http_request(server, "GET", checkout, NULL, NULL, NULL), ==, 303);
	g_assert_cmpuint(((FakeTransport *)p.transport)->checkouts, ==, 2);
	pay_now_assert_checkout(f, "cs_offline", "expired", FALSE);
	pay_now_assert_checkout(f, "cs_second", "open", TRUE);
	{
		g_autoptr(GPtrArray) links = rows(f, "stripe_payment_link");
		g_autoptr(VentureEntity) second = pay_now_checkout(f, "cs_second");
		g_object_get(g_ptr_array_index(links, 0), "checkout-id", &checkout_id, NULL);
		g_assert_cmpint(checkout_id, ==, venture_entity_get_id(second));
	}
	/* Stripe's own expiry event for the old session arrives late. */
	pay_now_webhook(&p, "{\"id\":\"evt_old_expired\",\"livemode\":false,\"type\":\"checkout.session.expired\",\"data\":{\"object\":{\"id\":\"cs_offline\",\"amount_total\":10000,\"currency\":\"usd\",\"payment_status\":\"unpaid\"}}}", TRUE);
	pay_now_assert_checkout(f, "cs_offline", "expired", FALSE);
	pay_now_assert_checkout(f, "cs_second", "open", TRUE);
	/* A completion for the old session while the new one can still collect
	 * is contradictory evidence: kept for retry, never dropped, not yet
	 * allocated, as for any second live attempt. */
	{
		const gchar *paid = "{\"id\":\"evt_old_paid\",\"livemode\":false,\"type\":\"checkout.session.completed\",\"data\":{\"object\":{\"id\":\"cs_offline\",\"amount_total\":10000,\"currency\":\"usd\",\"payment_status\":\"paid\",\"payment_intent\":\"pi_old_session\"}}}";
		pay_now_webhook(&p, paid, FALSE);
		invoice = venture_database_get(f->database, VENTURE_TYPE_INVOICE, venture_entity_get_id(p.invoice), &error);
		g_assert_no_error(error);
		g_object_get(invoice, "status", &status, NULL);
		g_assert_cmpint(status, ==, VENTURE_INVOICE_STATUS_SENT);
		g_clear_object(&invoice);
		/* Once the new session has gone, Stripe's retry records it. */
		pay_now_webhook(&p, "{\"id\":\"evt_second_expired\",\"livemode\":false,\"type\":\"checkout.session.expired\",\"data\":{\"object\":{\"id\":\"cs_second\",\"amount_total\":10000,\"currency\":\"usd\",\"payment_status\":\"unpaid\"}}}", TRUE);
		pay_now_assert_checkout(f, "cs_second", "expired", FALSE);
		pay_now_webhook(&p, paid, TRUE);
	}
	invoice = venture_database_get(f->database, VENTURE_TYPE_INVOICE, venture_entity_get_id(p.invoice), &error);
	g_assert_no_error(error);
	g_object_get(invoice, "status", &status, NULL);
	g_assert_cmpint(status, ==, VENTURE_INVOICE_STATUS_PAID);
	pay_now_assert_checkout(f, "cs_offline", "complete", FALSE);
	g_assert_cmpuint(((FakeTransport *)p.transport)->checkouts, ==, 2);
	venture_web_server_stop(server);
	venture_test_remove_tree(state_directory);
	pay_now_clear(&p);
}

/* --- The production path, clicked ------------------------------------------- */

/* No provider is handed to the context: Send finds the organization's own,
 * and the click builds one from the link's connection, both from the sealed
 * settings, as in production. Only their HTTP goes to the fake transport,
 * through the context's test hook. */
static void
test_pay_now_production_click(Fixture *f, gconstpointer data)
{
	PayNow p;
	g_autoptr(VentureWebServer) server = NULL;
	g_autofree gchar *state_directory = NULL, *checkout = NULL;
	(void)data;
	pay_now_prepare(f, &p, TRUE, FALSE, FALSE);
	g_assert_null(venture_context_get_stripe_service(f->context));
	venture_context_set_stripe_transport(f->context, STRIPE_TRANSPORT(p.transport));
	pay_now_deliver(f, &p);
	g_assert_nonnull(p.token);
	checkout = g_strdup_printf("/pay/%s/checkout", p.token);
	server = pay_now_server(f, &state_directory, FALSE);
	g_assert_cmpuint(http_request(server, "GET", checkout, NULL, NULL, NULL), ==, 303);
	g_assert_cmpuint(((FakeTransport *)p.transport)->checkouts, ==, 1);
	pay_now_assert_checkout(f, "cs_offline", "open", TRUE);
	venture_context_set_stripe_transport(f->context, NULL);
	venture_web_server_stop(server);
	venture_test_remove_tree(state_directory);
	pay_now_clear(&p);
}
