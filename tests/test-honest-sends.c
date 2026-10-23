/*
 * test-honest-sends.c - A button that says "sent" sent something, and a
 * business is not one tap from gone.
 *
 * Found walking a small trades business through a week in the web UI:
 * "Save and send" issued an invoice and emailed nobody, "Send quote" to a
 * customer with no email marked the quote sent, and the Delete under the
 * business's Save hid every record with no question asked and no way back
 * in the UI. What breaks if these regress: a customer never gets the bill
 * or the estimate while the owner believes they did, or the owner's whole
 * business vanishes from every list.
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
	VentureWebServer *server;
	gchar *dir;
	gint64 org;
	gint64 company;
} Fixture;

typedef struct
{
	GBytes *bytes;
	GError *error;
	gboolean done;
} Response;

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
	(void)data;
	f->config = venture_config_new();
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
	company = venture_company_new();
	g_object_set(company, "name", "Example Household", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(company), f->org);
	save(f, VENTURE_ENTITY(company));
	f->company = venture_entity_get_id(VENTURE_ENTITY(company));
	f->dir = g_dir_make_tmp("venture-honest-sends-XXXXXX", NULL);
	g_object_set(f->config, "state-dir", f->dir, "server-bind-address", "127.0.0.1",
		"server-port", (gint64)0, "security-require-auth", FALSE, NULL);
	f->server = venture_web_server_new(f->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(f->server, &error));
	g_assert_no_error(error);
}

static void
teardown(Fixture *f, gconstpointer data)
{
	(void)data;
	venture_web_server_stop(f->server);
	g_clear_object(&f->server);
	venture_test_remove_tree(f->dir);
	g_clear_pointer(&f->dir, g_free);
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
}

static void
response_done(GObject *source, GAsyncResult *result, gpointer data)
{
	Response *response = data;
	response->bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &response->error);
	response->done = TRUE;
}

/* The server shares this context, so the request runs on the loop. */
static guint
request(Fixture *f, const gchar *method, const gchar *path, const gchar *form,
	gchar **location, gchar **out)
{
	g_autoptr(SoupSession) session = soup_session_new_with_options("timeout", 15, NULL);
	g_autoptr(SoupMessage) message = NULL;
	g_autofree gchar *url = g_strconcat(venture_web_server_get_base_url(f->server), path, NULL);
	Response response;
	memset(&response, 0, sizeof(response));
	message = soup_message_new(method, url);
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	if (form != NULL)
	{
		g_autoptr(GBytes) payload = g_bytes_new(form, strlen(form));
		soup_message_set_request_body_from_bytes(message, "application/x-www-form-urlencoded", payload);
	}
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL, response_done, &response);
	while (!response.done)
		g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(response.error);
	if (out != NULL)
		*out = response.bytes != NULL
			? g_strndup(g_bytes_get_data(response.bytes, NULL), g_bytes_get_size(response.bytes))
			: g_strdup("");
	if (location != NULL)
		*location = g_strdup(soup_message_headers_get_one(soup_message_get_response_headers(message), "Location"));
	g_clear_pointer(&response.bytes, g_bytes_unref);
	return soup_message_get_status(message);
}

static GPtrArray *
rows(Fixture *f, GType type)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	g_autoptr(GError) error = NULL;
	GPtrArray *found;
	venture_query_set_limit(query, 0);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	found = venture_database_find(f->db, query, &error);
	g_assert_no_error(error);
	g_assert_nonnull(found);
	return found;
}

static void
customer_email(Fixture *f, const gchar *email)
{
	g_autoptr(VentureEntity) company = venture_database_get(f->db, VENTURE_TYPE_COMPANY, f->company, NULL);
	g_assert_nonnull(company);
	g_object_set(company, "email", email, NULL);
	save(f, company);
}

static gchar *
compose_form(Fixture *f, gboolean send)
{
	return g_strdup_printf("compose-form=1&company-id=%" G_GINT64_FORMAT
		"&line-0-description=Replace+water+heater&line-0-quantity=1&line-0-unit-price=1450+USD%s",
		f->company, send ? "&send=1" : "");
}

/* A draft quote with one line, made the way the sheet makes it. */
static gint64
draft_quote(Fixture *f)
{
	g_autofree gchar *form = compose_form(f, FALSE);
	g_autoptr(GPtrArray) quotes = NULL;
	g_assert_cmpuint(request(f, "POST", "/quotes/compose", form, NULL, NULL) / 100, ==, 3);
	quotes = rows(f, VENTURE_TYPE_QUOTE);
	g_assert_cmpuint(quotes->len, ==, 1);
	return venture_entity_get_id(g_ptr_array_index(quotes, 0));
}

static gint
quote_state(Fixture *f, gint64 id)
{
	g_autoptr(VentureEntity) quote = venture_database_get(f->db, VENTURE_TYPE_QUOTE, id, NULL);
	gint state = -1;
	g_assert_nonnull(quote);
	g_object_get(quote, "status", &state, NULL);
	return state;
}

/* "Save and send" with an address on file queues the invoice to it, and
 * the invoice page says where it went. */
static void
test_invoice_save_and_send_emails(Fixture *f, gconstpointer data)
{
	g_autofree gchar *form = compose_form(f, TRUE);
	g_autofree gchar *location = NULL, *page = NULL, *to = NULL;
	g_autoptr(GPtrArray) messages = NULL;
	(void)data;
	customer_email(f, "household@example.test");
	g_assert_cmpuint(request(f, "POST", "/invoices/compose", form, &location, NULL) / 100, ==, 3);
	g_assert_nonnull(location);
	messages = rows(f, VENTURE_TYPE_MAIL_MESSAGE);
	g_assert_cmpuint(messages->len, ==, 1);
	g_object_get(g_ptr_array_index(messages, 0), "to", &to, NULL);
	g_assert_cmpstr(to, ==, "household@example.test");
	g_assert_nonnull(strstr(location, "notice=emailed"));
	g_assert_cmpuint(request(f, "GET", location, NULL, NULL, &page), ==, 200);
	g_assert_nonnull(strstr(page, "Emailed to household@example.test"));
}

/* With no address on file the invoice is still issued, and the page says
 * plainly that nothing went out and how to fix it. */
static void
test_invoice_save_and_send_without_email(Fixture *f, gconstpointer data)
{
	g_autofree gchar *form = compose_form(f, TRUE);
	g_autofree gchar *location = NULL, *page = NULL;
	g_autoptr(GPtrArray) messages = NULL, invoices = NULL;
	gint status = -1;
	(void)data;
	g_assert_cmpuint(request(f, "POST", "/invoices/compose", form, &location, NULL) / 100, ==, 3);
	invoices = rows(f, VENTURE_TYPE_INVOICE);
	g_assert_cmpuint(invoices->len, ==, 1);
	g_object_get(g_ptr_array_index(invoices, 0), "status", &status, NULL);
	g_assert_cmpint(status, ==, VENTURE_INVOICE_STATUS_SENT);
	messages = rows(f, VENTURE_TYPE_MAIL_MESSAGE);
	g_assert_cmpuint(messages->len, ==, 0);
	g_assert_nonnull(location);
	g_assert_nonnull(strstr(location, "notice=not-emailed"));
	g_assert_cmpuint(request(f, "GET", location, NULL, NULL, &page), ==, 200);
	g_assert_nonnull(strstr(page, "Nothing was emailed"));
	g_assert_nonnull(strstr(page, "has no email address"));
}

/* "Send quote" to a customer with no email is refused like invoice Send,
 * and the quote stays a draft with nothing recorded as delivered. */
static void
test_quote_send_without_email_refused(Fixture *f, gconstpointer data)
{
	gint64 id = draft_quote(f);
	g_autofree gchar *path = g_strdup_printf("/quotes/%" G_GINT64_FORMAT "/send", id);
	g_autofree gchar *body = NULL;
	g_autoptr(GPtrArray) deliveries = NULL;
	(void)data;
	g_assert_cmpuint(request(f, "POST", path, "", NULL, &body), ==, 422);
	g_assert_nonnull(strstr(body, "email"));
	g_assert_cmpint(quote_state(f, id), ==, VENTURE_QUOTE_DRAFT);
	deliveries = rows(f, VENTURE_TYPE_QUOTE_DELIVERY);
	g_assert_cmpuint(deliveries->len, ==, 0);
}

/* The way out: mark it sent and hand over the customer's link to copy,
 * recorded as a link delivery rather than an email that never left. */
static void
test_quote_share_link(Fixture *f, gconstpointer data)
{
	gint64 id = draft_quote(f);
	g_autofree gchar *path = g_strdup_printf("/quotes/%" G_GINT64_FORMAT "/share", id);
	g_autofree gchar *record = g_strdup_printf("/e/quote/%" G_GINT64_FORMAT, id);
	g_autofree gchar *page = NULL, *channel = NULL, *token = NULL, *link = NULL, *draft = NULL;
	g_autoptr(GPtrArray) deliveries = NULL, messages = NULL;
	g_autoptr(VentureEntity) quote = NULL;
	(void)data;
	g_assert_cmpuint(request(f, "GET", record, NULL, NULL, &draft), ==, 200);
	g_assert_nonnull(strstr(draft, path));
	g_assert_cmpuint(request(f, "POST", path, "", NULL, NULL) / 100, ==, 3);
	g_assert_cmpint(quote_state(f, id), ==, VENTURE_QUOTE_SENT);
	deliveries = rows(f, VENTURE_TYPE_QUOTE_DELIVERY);
	g_assert_cmpuint(deliveries->len, ==, 1);
	g_object_get(g_ptr_array_index(deliveries, 0), "channel", &channel, NULL);
	g_assert_cmpstr(channel, ==, "link");
	messages = rows(f, VENTURE_TYPE_MAIL_MESSAGE);
	g_assert_cmpuint(messages->len, ==, 0);
	quote = venture_database_get(f->db, VENTURE_TYPE_QUOTE, id, NULL);
	g_object_get(quote, "acceptance-token", &token, NULL);
	g_assert_nonnull(token);
	link = g_strconcat("/q/", token, NULL);
	g_assert_cmpuint(request(f, "GET", record, NULL, NULL, &page), ==, 200);
	g_assert_nonnull(strstr(page, link));
	g_assert_nonnull(strstr(page, "Copy link"));
	g_assert_cmpuint(request(f, "GET", link, NULL, NULL, NULL), ==, 200);
}

/* A server with no public address keeps the quote's delivery for later
 * and sends nothing; the page has to say so rather than just "Sent". */
static void
test_quote_sent_not_emailed_says_so(Fixture *f, gconstpointer data)
{
	gint64 id = draft_quote(f);
	g_autofree gchar *path = g_strdup_printf("/quotes/%" G_GINT64_FORMAT "/send", id);
	g_autofree gchar *record = g_strdup_printf("/e/quote/%" G_GINT64_FORMAT, id);
	g_autofree gchar *page = NULL;
	g_autoptr(GPtrArray) messages = NULL;
	(void)data;
	customer_email(f, "household@example.test");
	g_assert_cmpuint(request(f, "POST", path, "", NULL, NULL) / 100, ==, 3);
	g_assert_cmpint(quote_state(f, id), ==, VENTURE_QUOTE_SENT);
	messages = rows(f, VENTURE_TYPE_MAIL_MESSAGE);
	g_assert_cmpuint(messages->len, ==, 0);
	g_assert_cmpuint(request(f, "GET", record, NULL, NULL, &page), ==, 200);
	g_assert_nonnull(strstr(page, "Not emailed"));
	g_assert_nonnull(strstr(page, "Copy link"));
}

static gint64
second_business(Fixture *f)
{
	g_autoptr(VentureOrganization) organization = g_object_new(VENTURE_TYPE_ORGANIZATION,
		"name", "Example Side Business", "active", TRUE, NULL);
	save(f, VENTURE_ENTITY(organization));
	return venture_entity_get_id(VENTURE_ENTITY(organization));
}

static gboolean
is_deleted(Fixture *f, GType type, gint64 id)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	g_autoptr(VentureEntity) record = NULL;
	venture_query_set_include_deleted(query, TRUE);
	g_assert_true(venture_query_add_filter_int(query, "id", VENTURE_FILTER_OP_EQ, id, NULL));
	record = venture_database_find_one(f->db, query, NULL);
	g_assert_nonnull(record);
	return venture_entity_is_deleted(record);
}

/* The default business can't be deleted at all, through the page or
 * through the database. */
static void
test_default_business_kept(Fixture *f, gconstpointer data)
{
	g_autofree gchar *path = g_strdup_printf("/e/organization/%" G_GINT64_FORMAT "/delete", f->org);
	g_autoptr(VentureEntity) organization = venture_database_get(f->db, VENTURE_TYPE_ORGANIZATION, f->org, NULL);
	g_autoptr(GError) error = NULL;
	(void)data;
	g_assert_cmpuint(request(f, "POST", path, "confirm=1", NULL, NULL), >=, 400);
	g_assert_false(is_deleted(f, VENTURE_TYPE_ORGANIZATION, f->org));
	g_assert_false(venture_database_delete(f->db, organization, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_false(is_deleted(f, VENTURE_TYPE_ORGANIZATION, f->org));
}

/* Deleting a business asks first: the form carries a required
 * confirmation, and a post without it changes nothing. */
static void
test_business_delete_asks(Fixture *f, gconstpointer data)
{
	gint64 id = second_business(f);
	g_autofree gchar *edit = g_strdup_printf("/e/organization/%" G_GINT64_FORMAT "/edit", id);
	g_autofree gchar *path = g_strdup_printf("/e/organization/%" G_GINT64_FORMAT "/delete", id);
	g_autofree gchar *page = NULL;
	(void)data;
	g_assert_cmpuint(request(f, "GET", edit, NULL, NULL, &page), ==, 200);
	g_assert_nonnull(strstr(page, "name=\"confirm\""));
	g_assert_cmpuint(request(f, "POST", path, "", NULL, NULL), >=, 400);
	g_assert_false(is_deleted(f, VENTURE_TYPE_ORGANIZATION, id));
	g_assert_cmpuint(request(f, "POST", path, "confirm=1", NULL, NULL) / 100, ==, 3);
	g_assert_true(is_deleted(f, VENTURE_TYPE_ORGANIZATION, id));
}

/* "Recoverable" means a Restore button on the deleted record's page. */
static void
test_restore_button(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureContact) contact = venture_contact_new();
	g_autofree gchar *record = NULL, *restore = NULL, *page = NULL, *location = NULL;
	gint64 id;
	(void)data;
	g_object_set(contact, "name", "Example Tenant", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(contact), f->org);
	save(f, VENTURE_ENTITY(contact));
	id = venture_entity_get_id(VENTURE_ENTITY(contact));
	g_assert_true(venture_database_delete(f->db, VENTURE_ENTITY(contact), NULL, NULL));
	record = g_strdup_printf("/e/contact/%" G_GINT64_FORMAT, id);
	restore = g_strdup_printf("/e/contact/%" G_GINT64_FORMAT "/restore", id);
	g_assert_cmpuint(request(f, "GET", record, NULL, NULL, &page), ==, 200);
	g_assert_nonnull(strstr(page, restore));
	g_assert_cmpuint(request(f, "POST", restore, "", &location, NULL) / 100, ==, 3);
	g_assert_cmpstr(location, ==, record);
	g_assert_false(is_deleted(f, VENTURE_TYPE_CONTACT, id));
	g_clear_pointer(&page, g_free);
	g_assert_cmpuint(request(f, "GET", record, NULL, NULL, &page), ==, 200);
	g_assert_null(strstr(page, restore));
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/honest-sends/invoice-save-and-send-emails", Fixture, NULL, setup, test_invoice_save_and_send_emails, teardown);
	g_test_add("/honest-sends/invoice-save-and-send-without-email", Fixture, NULL, setup, test_invoice_save_and_send_without_email, teardown);
	g_test_add("/honest-sends/quote-send-without-email-refused", Fixture, NULL, setup, test_quote_send_without_email_refused, teardown);
	g_test_add("/honest-sends/quote-share-link", Fixture, NULL, setup, test_quote_share_link, teardown);
	g_test_add("/honest-sends/quote-sent-not-emailed-says-so", Fixture, NULL, setup, test_quote_sent_not_emailed_says_so, teardown);
	g_test_add("/honest-sends/default-business-kept", Fixture, NULL, setup, test_default_business_kept, teardown);
	g_test_add("/honest-sends/business-delete-asks", Fixture, NULL, setup, test_business_delete_asks, teardown);
	g_test_add("/honest-sends/restore-button", Fixture, NULL, setup, test_restore_button, teardown);
	return g_test_run();
}
