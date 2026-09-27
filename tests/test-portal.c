/*
 * test-portal.c - Isolated customer invoices, statements and pay.
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <venture.h>
#include <string.h>
#include <libsoup/soup.h>
#include "venture-test-util.h"

typedef struct
{
	VentureDatabase *db;
	VentureConfig *config;
	VentureContext *context;
	gint64 org;
	gint64 company;
	gint64 other;
} Fixture;

static void
save(Fixture *f, VentureEntity *record)
{
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_database_save(f->db, record, NULL, &error));
	g_assert_no_error(error);
}

static void
setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureCompany) company = NULL;
	g_autoptr(VentureCompany) other = NULL;
	(void)data;
	f->config = venture_config_new();
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
	company = venture_company_new();
	g_object_set(company, "name", "Customer", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(company), f->org);
	save(f, VENTURE_ENTITY(company));
	f->company = venture_entity_get_id(VENTURE_ENTITY(company));
	other = venture_company_new();
	g_object_set(other, "name", "Other", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(other), f->org);
	save(f, VENTURE_ENTITY(other));
	f->other = venture_entity_get_id(VENTURE_ENTITY(other));
}

static void
teardown(Fixture *f, gconstpointer data)
{
	(void)data;
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
}

static void
actor_init(VentureActor *actor)
{
	actor->kind = VENTURE_ACTOR_KIND_USER;
	actor->name = "portal";
	actor->prompt = NULL;
	actor->request_id = NULL;
	actor->approved_by = NULL;
}

static VentureEntity *
issued_invoice(Fixture *f, gint64 company, const gchar *number, const gchar *amount)
{
	g_autoptr(VentureInvoice) invoice = venture_invoice_new();
	g_autoptr(VentureInvoiceLine) line = venture_invoice_line_new();
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GError) error = NULL;
	venture_entity_set_organization_id(VENTURE_ENTITY(invoice), f->org);
	g_object_set(invoice, "number", number, "company-id", company, NULL);
	save(f, VENTURE_ENTITY(invoice));
	venture_entity_set_organization_id(VENTURE_ENTITY(line), f->org);
	g_object_set(line, "invoice-id", venture_entity_get_id(VENTURE_ENTITY(invoice)),
		"description", "Work", "quantity", 1.0, NULL);
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(line), "unit-price", amount, NULL));
	save(f, VENTURE_ENTITY(line));
	g_assert_true(venture_settlement_service_transition(venture_settlement_service_get(f->db),
		invoice, "sent", now, NULL, &error));
	g_assert_no_error(error);
	return VENTURE_ENTITY(g_object_ref(invoice));
}

static void
test_invite_pay_revoke(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) access = NULL;
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureEntity) stranger = NULL;
	g_autoptr(GPtrArray) invoices = NULL;
	g_autoptr(VentureMoney) amount = venture_money_new_for_currency(4000, "USD");
	g_autoptr(VentureMoney) balance = NULL;
	g_autofree gchar *token = NULL;
	VentureActor actor;
	(void)data;
	actor_init(&actor);
	invoice = issued_invoice(f, f->company, "INV-P1", "40 USD");
	stranger = issued_invoice(f, f->other, "INV-X", "99 USD");
	access = venture_portal_service_invite(venture_portal_service_get(f->db),
		f->org, f->company, "billing@example.org", &actor, &error);
	g_assert_no_error(error);
	g_object_get(access, "token", &token, NULL);
	g_assert_cmpint((int)strlen(token), ==, 64);
	invoices = venture_portal_service_invoices(venture_portal_service_get(f->db),
		VENTURE_CUSTOMER_PORTAL_ACCESS(access), &error);
	g_assert_cmpuint(invoices->len, ==, 1);
	g_assert_cmpint(venture_entity_get_id(g_ptr_array_index(invoices, 0)), ==,
		venture_entity_get_id(invoice));
	g_assert_false(venture_portal_service_pay(venture_portal_service_get(f->db),
		VENTURE_CUSTOMER_PORTAL_ACCESS(access), venture_entity_get_id(stranger),
		amount, &actor, &error));
	g_clear_error(&error);
	g_assert_false(venture_portal_service_pay(venture_portal_service_get(f->db),
		VENTURE_CUSTOMER_PORTAL_ACCESS(access), venture_entity_get_id(invoice),
		amount, &actor, &error));
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "Checkout"));
	g_clear_error(&error);
	balance = venture_settlement_service_invoice_balance(venture_settlement_service_get(f->db),
		venture_entity_get_id(invoice), NULL, &error);
	g_assert_cmpint(venture_money_get_amount(balance), ==, 4000);
	g_assert_true(venture_portal_service_revoke(venture_portal_service_get(f->db),
		VENTURE_CUSTOMER_PORTAL_ACCESS(access), &actor, &error));
	g_assert_null(venture_portal_service_lookup(venture_portal_service_get(f->db), token, &error));
	g_assert_cmpint(venture_entity_get_id(invoice), >, 0);
}

typedef struct
{
	gboolean done;
	GBytes *bytes;
	GError *error;
} PortalResponse;

static void
portal_http_done(GObject *source, GAsyncResult *result, gpointer data)
{
	PortalResponse *response = data;
	response->bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &response->error);
	response->done = TRUE;
}

static guint
http_request(VentureWebServer *server, const gchar *method, const gchar *path,
	const gchar *body, gchar **out)
{
	g_autoptr(SoupSession) session = soup_session_new_with_options("timeout", 15, NULL);
	g_autoptr(SoupMessage) message = NULL;

	g_autofree gchar *url = g_strconcat(venture_web_server_get_base_url(server), path, NULL);
	PortalResponse response;
	memset(&response, 0, sizeof(response));
	message = soup_message_new(method, url);
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	if (body != NULL)
	{
		g_autoptr(GBytes) payload = g_bytes_new(body, strlen(body));
		soup_message_set_request_body_from_bytes(message, "application/json", payload);
	}
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL, portal_http_done, &response);
	while (!response.done)
		g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(response.error);
	if (out && response.bytes)
		*out = g_strndup(g_bytes_get_data(response.bytes, NULL), g_bytes_get_size(response.bytes));
	g_clear_pointer(&response.bytes, g_bytes_unref);
	return soup_message_get_status(message);
}

static void
test_http_isolation(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureWebServer) server = NULL;
	g_autoptr(VentureEntity) access = NULL;
	g_autoptr(VentureEntity) invoice = NULL;
	g_autofree gchar *dir = g_dir_make_tmp("venture-portal-XXXXXX", NULL);
	g_autofree gchar *token = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *body = NULL;
	g_autoptr(GSocketListener) listener = g_socket_listener_new();
	guint16 port;
	VentureActor actor;
	(void)data;
	actor_init(&actor);
	invoice = issued_invoice(f, f->company, "INV-WEB", "10 USD");
	issued_invoice(f, f->other, "INV-HIDDEN", "50 USD");
	access = venture_portal_service_invite(venture_portal_service_get(f->db),
		f->org, f->company, "a@b.c", &actor, &error);
	g_object_get(access, "token", &token, NULL);
	port = g_socket_listener_add_any_inet_port(listener, NULL, &error);
	g_socket_listener_close(listener);
	g_object_set(f->config, "state-dir", dir, "server-bind-address", "127.0.0.1",
		"server-port", (gint64)port, "security-require-auth", FALSE, NULL);
	server = venture_web_server_new(f->context, &error);
	g_assert_true(venture_web_server_start(server, &error));
	g_assert_cmpuint(http_request(server, "GET", "/portal/deadbeef", NULL, NULL), ==, 404);
	path = g_strdup_printf("/portal/%s", token);
	g_assert_cmpuint(http_request(server, "GET", path, NULL, &body), ==, 200);
	g_assert_nonnull(strstr(body, "INV-WEB"));
	g_assert_null(strstr(body, "INV-HIDDEN"));
	g_assert_nonnull(strstr(body, "Checkout"));
	{
		g_autofree gchar *payload = g_strdup_printf("{\"company_id\":%" G_GINT64_FORMAT ",\"email\":\"billing@example.org\"}", f->company);
		g_autofree gchar *reply = NULL;
		g_object_set(f->config, "server-base-url", "https://venture.example.org", NULL);
		g_assert_cmpuint(http_request(server, "POST", "/api/v1/customer_portal/invite", payload, &reply), ==, 201);
		g_assert_null(strstr(reply, "\"token\""));
	}
	venture_config_set_module_enabled(f->config, "receivables", FALSE);
	g_assert_cmpuint(http_request(server, "GET", path, NULL, NULL), ==, 404);
	venture_config_set_module_enabled(f->config, "receivables", TRUE);
	venture_test_remove_tree(dir);
	(void)invoice;
}

static gint64
portal_price(Fixture *f, const gchar *plan_name, const gchar *code, const gchar *amount, const gchar *currency, gboolean active)
{
	g_autoptr(VenturePlan) plan = venture_plan_new();
	g_autoptr(VenturePlanPrice) price = venture_plan_price_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(plan), f->org);
	g_object_set(plan, "name", plan_name, "code", code, "active", TRUE, NULL);
	save(f, VENTURE_ENTITY(plan));
	venture_entity_set_organization_id(VENTURE_ENTITY(price), f->org);
	g_object_set(price, "plan-id", venture_entity_get_id(VENTURE_ENTITY(plan)), "currency", currency, "active", active, NULL);
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(price), "amount", amount, NULL));
	save(f, VENTURE_ENTITY(price));
	return venture_entity_get_id(VENTURE_ENTITY(price));
}

/* A subscription started yesterday, so a change today is inside its first period. */
static gint64
portal_subscribe(Fixture *f, gint64 company, gint64 price)
{
	g_autoptr(VentureBillingRequest) start = venture_billing_request_new();
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GDateTime) yesterday = g_date_time_add_days(now, -1);
	gint64 id = 0;
	venture_entity_set_organization_id(VENTURE_ENTITY(start), f->org);
	g_object_set(start, "action", "start", "company-id", company, "plan-price-id", price, "at", yesterday, NULL);
	save(f, VENTURE_ENTITY(start));
	g_object_get(start, "subscription-id", &id, NULL);
	g_assert_cmpint(id, >, 0);
	return id;
}

static VentureEntity *
portal_subscription(Fixture *f, gint64 id)
{
	return venture_database_get(f->db, VENTURE_TYPE_CUSTOMER_SUBSCRIPTION, id, NULL);
}

static guint
portal_form(VentureWebServer *server, const gchar *path, const gchar *form, gchar **out)
{
	g_autoptr(SoupSession) session = soup_session_new_with_options("timeout", 15, NULL);
	g_autoptr(SoupMessage) message = NULL;
	g_autoptr(GBytes) payload = g_bytes_new(form, strlen(form));
	g_autofree gchar *url = g_strconcat(venture_web_server_get_base_url(server), path, NULL);
	PortalResponse response;
	memset(&response, 0, sizeof(response));
	message = soup_message_new("POST", url);
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	soup_message_set_request_body_from_bytes(message, "application/x-www-form-urlencoded", payload);
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL, portal_http_done, &response);
	while (!response.done)
		g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(response.error);
	if (out != NULL && response.bytes != NULL)
		*out = g_strndup(g_bytes_get_data(response.bytes, NULL), g_bytes_get_size(response.bytes));
	g_clear_pointer(&response.bytes, g_bytes_unref);
	return soup_message_get_status(message);
}

/*
 * A signed-in customer sees their own subscriptions -- plan, price, next
 * renewal and amount, invoices -- switches to another price the same
 * venture sells or cancels at renewal, and can neither see nor touch
 * another customer's: every attempt is NOT_FOUND and changes nothing. If
 * this regresses, one customer's portal link manages another's plan, or a
 * customer is offered a price the business does not sell them.
 */
static void
test_subscriptions(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureWebServer) server = NULL;
	g_autoptr(VentureEntity) access = NULL;
	g_autoptr(VentureEntity) sub = NULL;
	g_autoptr(VentureEntity) theirs = NULL;
	g_autoptr(GPtrArray) offered = NULL;
	g_autoptr(GSocketListener) listener = g_socket_listener_new();
	g_autofree gchar *dir = g_dir_make_tmp("venture-portal-XXXXXX", NULL);
	g_autofree gchar *token = NULL, *page = NULL, *path = NULL, *mine = NULL, *other = NULL, *form = NULL, *body = NULL;
	gint64 starter, pro, retired, euro, own_id, their_id, pending = 0, their_version;
	gboolean ending = FALSE;
	guint16 port;
	VentureActor actor;
	guint i;
	(void)data;
	actor_init(&actor);
	starter = portal_price(f, "Starter", "starter", "30 USD", "USD", TRUE);
	pro = portal_price(f, "Pro", "pro", "60 USD", "USD", TRUE);
	retired = portal_price(f, "Legacy", "legacy", "20 USD", "USD", FALSE);
	euro = portal_price(f, "Euro", "euro", "30 EUR", "EUR", TRUE);
	own_id = portal_subscribe(f, f->company, starter);
	their_id = portal_subscribe(f, f->other, starter);
	theirs = portal_subscription(f, their_id);
	their_version = venture_entity_get_version(theirs);
	access = venture_portal_service_invite(venture_portal_service_get(f->db), f->org, f->company, "a@b.c", &actor, &error);
	g_assert_no_error(error);
	g_object_get(access, "token", &token, NULL);

	/* Offered: the other active plan in the same currency, nothing else. */
	offered = venture_portal_service_offered_prices(venture_portal_service_get(f->db),
		VENTURE_CUSTOMER_PORTAL_ACCESS(access), own_id, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(offered->len, ==, 1);
	g_assert_cmpint(venture_entity_get_id(g_ptr_array_index(offered, 0)), ==, pro);
	g_assert_null(venture_portal_service_offered_prices(venture_portal_service_get(f->db),
		VENTURE_CUSTOMER_PORTAL_ACCESS(access), their_id, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_clear_error(&error);

	port = g_socket_listener_add_any_inet_port(listener, NULL, &error);
	g_socket_listener_close(listener);
	g_object_set(f->config, "state-dir", dir, "server-bind-address", "127.0.0.1",
		"server-port", (gint64)port, "security-require-auth", FALSE, NULL);
	server = venture_web_server_new(f->context, &error);
	g_assert_true(venture_web_server_start(server, &error));

	path = g_strdup_printf("/portal/%s", token);
	g_assert_cmpuint(http_request(server, "GET", path, NULL, &page), ==, 200);
	sub = portal_subscription(f, own_id);
	g_assert_nonnull(strstr(page, "Your subscriptions"));
	g_assert_nonnull(strstr(page, "Starter, $30.00 a month"));
	g_assert_nonnull(strstr(page, "Next renewal: "));
	g_assert_nonnull(strstr(page, ", for $30.00."));
	g_assert_nonnull(strstr(page, venture_entity_get_uuid(sub)));
	g_assert_null(strstr(page, venture_entity_get_uuid(theirs)));
	g_assert_null(strstr(page, "Legacy"));
	g_assert_null(strstr(page, "Euro"));
	g_clear_object(&sub);

	/* Every door onto the other customer's subscription is NOT_FOUND. */
	other = g_strdup_printf("/portal/%s/subscriptions/%" G_GINT64_FORMAT, token, their_id);
	form = g_strdup_printf("action=change&plan_price_id=%" G_GINT64_FORMAT "&when=now", pro);
	g_assert_cmpuint(portal_form(server, other, "action=cancel", NULL), ==, 404);
	g_assert_cmpuint(portal_form(server, other, form, NULL), ==, 404);
	g_assert_false(venture_portal_service_manage_subscription(venture_portal_service_get(f->db), token, their_id,
		"cancel", 0, FALSE, &actor, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_clear_error(&error);
	g_clear_object(&theirs);
	theirs = portal_subscription(f, their_id);
	g_assert_cmpint(venture_entity_get_version(theirs), ==, their_version);

	/* Their own: a price not offered is refused, an offered one switches
	 * at renewal by default, and cancelling keeps it until renewal. */
	mine = g_strdup_printf("/portal/%s/subscriptions/%" G_GINT64_FORMAT, token, own_id);
	for (i = 0; i < 2; i++)
	{
		g_autofree gchar *bad = g_strdup_printf("action=change&plan_price_id=%" G_GINT64_FORMAT, i == 0 ? retired : euro);
		g_clear_pointer(&body, g_free);
		g_assert_cmpuint(portal_form(server, mine, bad, &body), ==, 422);
		g_assert_nonnull(strstr(body, "not one you can switch to"));
	}
	g_clear_pointer(&form, g_free);
	form = g_strdup_printf("action=change&plan_price_id=%" G_GINT64_FORMAT "&when=renewal", pro);
	g_assert_cmpuint(portal_form(server, mine, form, NULL), ==, 303);
	sub = portal_subscription(f, own_id);
	g_object_get(sub, "pending-plan-price-id", &pending, NULL);
	g_assert_cmpint(pending, ==, pro);
	g_clear_object(&sub);
	g_clear_pointer(&page, g_free);
	g_assert_cmpuint(http_request(server, "GET", path, NULL, &page), ==, 200);
	g_assert_nonnull(strstr(page, "Switches to Pro, $60.00 a month at renewal."));
	g_assert_nonnull(strstr(page, ", for $60.00."));
	g_assert_cmpuint(portal_form(server, mine, "action=cancel", NULL), ==, 303);
	sub = portal_subscription(f, own_id);
	g_object_get(sub, "cancel-at-period-end", &ending, NULL);
	g_assert_true(ending);

	/* Revoked is gone, including for the actions; so is billing switched off. */
	g_assert_true(venture_portal_service_revoke(venture_portal_service_get(f->db),
		VENTURE_CUSTOMER_PORTAL_ACCESS(access), &actor, &error));
	g_assert_cmpuint(portal_form(server, mine, "action=cancel", NULL), ==, 404);
	venture_web_server_stop(server);
	venture_test_remove_tree(dir);
}

/* Delivery retains its private link while every generic serialized view omits it. */
static void
test_private_invitation(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureLogMailer) mailer = venture_log_mailer_new();
	g_autoptr(VentureEntity) access = NULL;
	g_autoptr(JsonNode) output = NULL;
	g_autofree gchar *token = NULL;
	g_autofree gchar *private_body = NULL;
	g_autofree gchar *serialized = NULL;
	const GPtrArray *messages;
	(void)data;
	venture_context_set_mailer(f->context, VENTURE_MAILER(mailer));
	access = venture_portal_service_send_invitation(venture_portal_service_get(f->db),
		"https://venture.example.org/", FALSE, f->org, f->company, "billing@example.org", NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(access);
	g_object_get(access, "token", &token, NULL);
	g_assert_cmpint(venture_mail_outbox_deliver_due(venture_context_get_mail_outbox(f->context),
		f->org, 10, NULL, NULL, &error), ==, 1);
	g_assert_no_error(error);
	messages = venture_log_mailer_get_messages(mailer);
	g_assert_cmpuint(messages->len, ==, 1);
	g_object_get(g_ptr_array_index(messages, 0), "private-text-body", &private_body, NULL);
	g_assert_nonnull(strstr(private_body, "https://venture.example.org/portal/"));
	g_assert_nonnull(strstr(private_body, token));
	output = venture_serializable_to_json(VENTURE_SERIALIZABLE(g_ptr_array_index(messages, 0)), FALSE);
	serialized = venture_json_to_string(output, FALSE);
	g_assert_null(strstr(serialized, token));
	g_clear_object(&access);
	/* An invalid deployment URL must fail before creating unusable access. */
	access = venture_portal_service_send_invitation(venture_portal_service_get(f->db),
		"http://venture.example.org", FALSE, f->org, f->company, "billing@example.org", NULL, &error);
	g_assert_null(access);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/portal/invite-pay-revoke", Fixture, NULL, setup, test_invite_pay_revoke, teardown);
	g_test_add("/portal/http-isolation", Fixture, NULL, setup, test_http_isolation, teardown);
	g_test_add("/portal/private-invitation", Fixture, NULL, setup, test_private_invitation, teardown);
	g_test_add("/portal/subscriptions", Fixture, NULL, setup, test_subscriptions, teardown);
	return g_test_run();
}
