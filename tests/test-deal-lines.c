/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <libsoup/soup.h>
#include <string.h>
#include "venture-test-util.h"

typedef struct
{
	VentureDatabase *db;
	VentureConfig *config;
	VentureContext *context;
	VentureWebServer *server;
	SoupSession *session;
	gchar *state_dir;
	gchar *url;
} Fixture;

static void
setup(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GSocketListener) probe = g_socket_listener_new();
	guint port;
	(void)unused;
	port = g_socket_listener_add_any_inet_port(probe, NULL, &error);
	g_assert_no_error(error);
	g_clear_object(&probe);
	f->state_dir = g_dir_make_tmp("venture-deal-lines-XXXXXX", NULL);
	f->url = g_strdup_printf("http://127.0.0.1:%u", port);
	f->config = venture_config_new();
	g_object_set(f->config, "state-dir", f->state_dir, "server-bind-address", "127.0.0.1",
		"server-port", (gint64)port, "security-require-auth", FALSE, NULL);
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->server = venture_web_server_new(f->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(f->server, &error));
	g_assert_no_error(error);
	f->session = soup_session_new();
}

static void
teardown(Fixture *f, gconstpointer unused)
{
	(void)unused;
	venture_web_server_stop(f->server);
	g_clear_object(&f->session);
	g_clear_object(&f->server);
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
	venture_test_remove_tree(f->state_dir);
	g_free(f->state_dir);
	g_free(f->url);
}

static void
save(Fixture *f, VentureEntity *row)
{
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_database_save(f->db, row, NULL, &error));
	g_assert_no_error(error);
}

static VentureEntity *
deal(Fixture *f, const gchar *name)
{
	VentureEntity *row = VENTURE_ENTITY(venture_deal_new());
	g_autoptr(GError) error = NULL;
	g_object_set(row, "organization-id", (gint64)1, "name", name, NULL);
	g_assert_true(venture_entity_set_field_from_string(row, "value", "100.00 USD", &error));
	save(f, row);
	return row;
}

static VentureEntity *
line(Fixture *f, VentureEntity *d, const gchar *quantity, const gchar *price, const gchar *discount)
{
	VentureEntity *row = VENTURE_ENTITY(venture_deal_line_new());
	g_autoptr(GError) error = NULL;
	g_object_set(row, "organization-id", (gint64)1, "deal-id", venture_entity_get_id(d),
		"description", "Consulting", NULL);
	g_assert_true(venture_entity_set_field_from_string(row, "quantity", quantity, &error));
	g_assert_true(venture_entity_set_field_from_string(row, "unit-price", price, &error));
	g_assert_true(venture_entity_set_field_from_string(row, "discount-bp", discount, &error));
	g_assert_no_error(error);
	return row;
}

static gchar *
value_of(Fixture *f, VentureEntity *d)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) fresh = venture_database_get(f->db, VENTURE_TYPE_DEAL, venture_entity_get_id(d), &error);
	g_autoptr(VentureMoney) value = NULL;
	g_assert_no_error(error);
	g_object_get(fresh, "value", &value, NULL);
	return NULL != value ? venture_money_to_string(value) : g_strdup("(none)");
}

static gint
count(Fixture *f, GType type, const gchar *field, gint64 id)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	g_autoptr(GError) error = NULL;
	gint result;
	venture_query_set_organization(query, 1);
	if (NULL != field)
		venture_query_add_filter_int(query, field, VENTURE_FILTER_OP_EQ, id, NULL);
	result = venture_database_count(f->db, query, &error);
	g_assert_no_error(error);
	return result;
}

/* The line is a field-table record with the fields the contract names. */
static void
test_records(void)
{
	static const gchar *names[] = { "deal-id", "product-id", "description", "quantity", "unit-price", "discount-bp", "position" };
	g_autoptr(VentureDealLine) row = venture_deal_line_new();
	guint i;
	g_assert_cmpuint(venture_entity_registry_lookup(venture_entity_registry_get_default(), "deal_line"), !=, G_TYPE_INVALID);
	for (i = 0; i < G_N_ELEMENTS(names); i++)
		g_assert_nonnull(g_object_class_find_property(G_OBJECT_GET_CLASS(row), names[i]));
}

/* Value follows the lines while any exist and returns to manual when none do. */
static void
test_derived_value(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) d = deal(f, "Priced");
	g_autoptr(VentureEntity) first = line(f, d, "3", "19.99 USD", "1000");
	g_autoptr(VentureEntity) second = line(f, d, "1", "10.00 USD", "0");
	g_autoptr(VentureEntity) fresh = NULL;
	g_autofree gchar *value = NULL;
	g_autoptr(GError) error = NULL;
	(void)unused;
	value = value_of(f, d);
	g_assert_cmpstr(value, ==, "100.00 USD");
	g_clear_pointer(&value, g_free);
	save(f, first);
	value = value_of(f, d);
	g_assert_cmpstr(value, ==, "53.97 USD");
	g_clear_pointer(&value, g_free);
	save(f, second);
	value = value_of(f, d);
	g_assert_cmpstr(value, ==, "63.97 USD");
	g_clear_pointer(&value, g_free);
	g_object_set(first, "quantity", (gint64)1, NULL);
	save(f, first);
	value = value_of(f, d);
	g_assert_cmpstr(value, ==, "27.99 USD");
	g_clear_pointer(&value, g_free);
	g_assert_true(venture_database_delete(f->db, first, NULL, &error));
	g_assert_no_error(error);
	value = value_of(f, d);
	g_assert_cmpstr(value, ==, "10.00 USD");
	g_clear_pointer(&value, g_free);
	g_assert_true(venture_database_restore(f->db, first, NULL, &error));
	g_assert_no_error(error);
	value = value_of(f, d);
	g_assert_cmpstr(value, ==, "27.99 USD");
	g_clear_pointer(&value, g_free);
	g_assert_true(venture_database_delete(f->db, first, NULL, &error));
	g_assert_true(venture_database_delete(f->db, second, NULL, &error));
	g_assert_no_error(error);
	/* No lines: the last derived amount stays and the field is manual again. */
	fresh = venture_database_get(f->db, VENTURE_TYPE_DEAL, venture_entity_get_id(d), &error);
	g_assert_true(venture_entity_set_field_from_string(fresh, "value", "5.00 USD", &error));
	save(f, fresh);
	value = value_of(f, d);
	g_assert_cmpstr(value, ==, "5.00 USD");
}

/* A generic write of the value is refused while lines exist; other fields are not. */
static void
test_generic_value_refused(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) d = deal(f, "Guarded");
	g_autoptr(VentureEntity) first = line(f, d, "3", "19.99 USD", "1000");
	g_autoptr(VentureEntity) fresh = NULL;
	g_autofree gchar *value = NULL;
	g_autoptr(GError) error = NULL;
	(void)unused;
	save(f, first);
	fresh = venture_database_get(f->db, VENTURE_TYPE_DEAL, venture_entity_get_id(d), &error);
	g_assert_true(venture_entity_set_field_from_string(fresh, "value", "999.00 USD", &error));
	g_assert_false(venture_database_save(f->db, fresh, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_nonnull(strstr(error->message, "VentureDealService"));
	g_clear_error(&error);
	value = value_of(f, d);
	g_assert_cmpstr(value, ==, "53.97 USD");
	g_clear_object(&fresh);
	fresh = venture_database_get(f->db, VENTURE_TYPE_DEAL, venture_entity_get_id(d), &error);
	g_object_set(fresh, "notes", "Still editable", NULL);
	save(f, fresh);
}

/* Lines in two currencies cannot be summed; the second line is not persisted. */
static void
test_currency_mismatch(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) d = deal(f, "Mixed");
	g_autoptr(VentureEntity) first = line(f, d, "1", "10.00 USD", "0");
	g_autoptr(VentureEntity) second = line(f, d, "1", "5.00 EUR", "0");
	g_autofree gchar *value = NULL;
	g_autoptr(GError) error = NULL;
	(void)unused;
	save(f, first);
	g_assert_false(venture_database_save(f->db, second, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_nonnull(strstr(error->message, "currency"));
	g_assert_false(venture_entity_is_persisted(second));
	g_assert_cmpint(count(f, VENTURE_TYPE_DEAL_LINE, "deal-id", venture_entity_get_id(d)), ==, 1);
	value = value_of(f, d);
	g_assert_cmpstr(value, ==, "10.00 USD");
}

static void
test_invalid_lines(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) d = deal(f, "Strict");
	g_autoptr(VentureEntity) other = deal(f, "Other");
	g_autoptr(VentureEntity) zero = line(f, d, "0", "10.00 USD", "0");
	g_autoptr(VentureEntity) steep = line(f, d, "1", "10.00 USD", "10001");
	g_autoptr(VentureEntity) negative = line(f, d, "1", "-1.00 USD", "0");
	g_autoptr(VentureEntity) orphan = line(f, d, "1", "1.00 USD", "0");
	g_autoptr(VentureEntity) product = line(f, d, "1", "1.00 USD", "0");
	g_autoptr(VentureEntity) moved = line(f, d, "1", "1.00 USD", "0");
	g_autoptr(GError) error = NULL;
	(void)unused;
	g_assert_false(venture_database_save(f->db, zero, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	g_assert_false(venture_database_save(f->db, steep, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	g_assert_false(venture_database_save(f->db, negative, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	g_object_set(orphan, "deal-id", (gint64)424242, NULL);
	g_assert_false(venture_database_save(f->db, orphan, NULL, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_object_set(product, "product-id", (gint64)424242, NULL);
	g_assert_false(venture_database_save(f->db, product, NULL, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	save(f, moved);
	g_object_set(moved, "deal-id", venture_entity_get_id(other), NULL);
	g_assert_false(venture_database_save(f->db, moved, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_cmpint(count(f, VENTURE_TYPE_DEAL_LINE, "deal-id", venture_entity_get_id(d)), ==, 1);
}

/* One step copies the lines into a linked draft; a rerun revises, never duplicates. */
static void
test_create_quote(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) d = deal(f, "Quoted");
	g_autoptr(VentureEntity) first = line(f, d, "3", "19.99 USD", "1000");
	g_autoptr(VentureEntity) second = line(f, d, "2", "10.00 USD", "0");
	g_autoptr(VentureEntity) third = line(f, d, "1", "1.00 USD", "0");
	g_autoptr(VentureQuote) quote = NULL;
	g_autoptr(VentureQuote) again = NULL;
	g_autoptr(VentureEntity) superseded = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_QUOTE_LINE);
	g_autoptr(GPtrArray) lines = NULL;
	g_autoptr(VentureMoney) total = NULL;
	g_autofree gchar *number = NULL;
	g_autofree gchar *currency = NULL;
	g_autofree gchar *text = NULL;
	g_autoptr(GError) error = NULL;
	gint64 deal_id, revision, parent, discount, quantity;
	gint status;
	(void)unused;
	save(f, first);
	save(f, second);
	quote = venture_deal_service_create_quote(venture_database_get_deal_service(f->db), VENTURE_DEAL(d), NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(quote);
	g_object_get(quote, "number", &number, "deal-id", &deal_id, "currency", &currency,
		"revision", &revision, "total", &total, NULL);
	g_assert_cmpint(deal_id, ==, venture_entity_get_id(d));
	g_assert_cmpint(revision, ==, 1);
	g_assert_cmpstr(currency, ==, "USD");
	g_assert_true(g_str_has_prefix(number, "DEAL-"));
	text = venture_money_to_string(total);
	g_assert_cmpstr(text, ==, "73.97 USD");
	venture_query_set_organization(query, 1);
	venture_query_add_filter_int(query, "quote-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(VENTURE_ENTITY(quote)), NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	lines = venture_database_find(f->db, query, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(lines->len, ==, 2);
	g_object_get(g_ptr_array_index(lines, 0), "discount-percent", &discount, "quantity", &quantity, NULL);
	g_assert_cmpint(discount, ==, 10);
	g_assert_cmpint(quantity, ==, 3);
	/* Rerun after the deal changed: a new revision carries the current lines. */
	save(f, third);
	again = venture_deal_service_create_quote(venture_database_get_deal_service(f->db), VENTURE_DEAL(d), NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(again);
	g_object_get(again, "revision", &revision, "parent-id", &parent, NULL);
	g_assert_cmpint(revision, ==, 2);
	g_assert_cmpint(parent, ==, venture_entity_get_id(VENTURE_ENTITY(quote)));
	g_assert_cmpint(count(f, VENTURE_TYPE_QUOTE_LINE, "quote-id", venture_entity_get_id(VENTURE_ENTITY(again))), ==, 3);
	g_assert_cmpint(count(f, VENTURE_TYPE_QUOTE_LINE, "quote-id", venture_entity_get_id(VENTURE_ENTITY(quote))), ==, 2);
	g_assert_cmpint(count(f, VENTURE_TYPE_QUOTE, "deal-id", venture_entity_get_id(d)), ==, 2);
	superseded = venture_database_get(f->db, VENTURE_TYPE_QUOTE, venture_entity_get_id(VENTURE_ENTITY(quote)), &error);
	g_object_get(superseded, "status", &status, NULL);
	g_assert_cmpint(status, ==, VENTURE_QUOTE_SUPERSEDED);
}

/* Refusals leave nothing behind, including a failure after the quote header was written. */
static void
test_create_quote_refusals(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) empty = deal(f, "Empty");
	g_autoptr(VentureEntity) fractional = deal(f, "Fractional");
	g_autoptr(VentureEntity) stale = deal(f, "Stale product");
	g_autoptr(VentureEntity) frac_line = line(f, fractional, "1", "10.00 USD", "1250");
	g_autoptr(VentureEntity) venture = g_object_new(VENTURE_TYPE_VENTURE, "organization-id", (gint64)1, "name", "Shop", NULL);
	g_autoptr(VentureEntity) product = NULL;
	g_autoptr(VentureEntity) stale_line = NULL;
	g_autoptr(VentureQuote) quote = NULL;
	g_autoptr(GError) error = NULL;
	(void)unused;
	quote = venture_deal_service_create_quote(venture_database_get_deal_service(f->db), VENTURE_DEAL(empty), NULL, &error);
	g_assert_null(quote);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_nonnull(strstr(error->message, "at least one line"));
	g_clear_error(&error);
	save(f, frac_line);
	quote = venture_deal_service_create_quote(venture_database_get_deal_service(f->db), VENTURE_DEAL(fractional), NULL, &error);
	g_assert_null(quote);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_nonnull(strstr(error->message, "basis points"));
	g_clear_error(&error);
	/* A product deleted after the line was priced fails the quote line copy
	 * after the quote header exists; the header must not survive. */
	save(f, venture);
	product = g_object_new(VENTURE_TYPE_PRODUCT, "organization-id", (gint64)1,
		"venture-id", venture_entity_get_id(venture), "name", "Widget", NULL);
	save(f, product);
	stale_line = line(f, stale, "1", "10.00 USD", "0");
	g_object_set(stale_line, "product-id", venture_entity_get_id(product), NULL);
	save(f, stale_line);
	g_assert_true(venture_database_delete(f->db, product, NULL, &error));
	g_assert_no_error(error);
	quote = venture_deal_service_create_quote(venture_database_get_deal_service(f->db), VENTURE_DEAL(stale), NULL, &error);
	g_assert_null(quote);
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_assert_cmpint(count(f, VENTURE_TYPE_QUOTE, NULL, 0), ==, 0);
	g_assert_cmpint(count(f, VENTURE_TYPE_QUOTE_LINE, NULL, 0), ==, 0);
}

typedef struct { gboolean done; GBytes *body; GError *error; } Reply;

static void
reply_done(GObject *source, GAsyncResult *result, gpointer data)
{
	Reply *reply = data;
	reply->body = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &reply->error);
	reply->done = TRUE;
}

static guint
http(Fixture *f, const gchar *method, const gchar *path, const gchar *mime, const gchar *body, gchar **text, gchar **location)
{
	g_autofree gchar *url = g_strconcat(f->url, path, NULL);
	g_autoptr(SoupMessage) message = soup_message_new(method, url);
	Reply reply = { FALSE, NULL, NULL };
	guint status;
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	if (NULL != body)
	{
		g_autoptr(GBytes) bytes = g_bytes_new(body, strlen(body));
		soup_message_set_request_body_from_bytes(message, mime, bytes);
	}
	soup_session_send_and_read_async(f->session, message, G_PRIORITY_DEFAULT, NULL, reply_done, &reply);
	while (!reply.done)
		g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(reply.error);
	*text = g_strndup(g_bytes_get_data(reply.body, NULL), g_bytes_get_size(reply.body));
	if (NULL != location)
		*location = g_strdup(soup_message_headers_get_one(soup_message_get_response_headers(message), "Location"));
	status = soup_message_get_status(message);
	g_bytes_unref(reply.body);
	return status;
}

typedef struct { gboolean done; gchar *out; gchar *err; GError *error; } CliResult;

static void
cli_done(GObject *source, GAsyncResult *result, gpointer data)
{
	CliResult *outcome = data;
	g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &outcome->out, &outcome->err, &outcome->error);
	outcome->done = TRUE;
}

/* REST, the deal page button and the CLI all reach the same service. */
static void
test_surfaces(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) d = deal(f, "Surfaced");
	g_autoptr(VentureEntity) first = line(f, d, "2", "10.00 USD", "0");
	g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
	g_autoptr(GSubprocess) child = NULL;
	g_autoptr(GError) error = NULL;
	CliResult result = { FALSE, NULL, NULL, NULL };
	g_autofree gchar *id = NULL;
	g_autofree gchar *api = NULL;
	g_autofree gchar *page = NULL;
	g_autofree gchar *form = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *location = NULL;
	const gchar *args[] = { "build/debug/venturectl", "--server", NULL, "deal", "quote", NULL, NULL };
	(void)unused;
	save(f, first);
	id = g_strdup_printf("%" G_GINT64_FORMAT, venture_entity_get_id(d));
	api = g_strdup_printf("/api/v1/deals/%s/quote", id);
	page = g_strdup_printf("/e/deal/%s", id);
	form = g_strdup_printf("/deals/%s/quote", id);
	g_assert_cmpuint(http(f, "POST", api, "application/json", "{}", &text, NULL), ==, 201);
	g_assert_nonnull(strstr(text, "\"number\" : \"DEAL-"));
	g_clear_pointer(&text, g_free);
	g_assert_cmpuint(http(f, "GET", page, NULL, NULL, &text, NULL), ==, 200);
	g_assert_nonnull(strstr(text, "Create quote from lines"));
	g_clear_pointer(&text, g_free);
	g_assert_cmpuint(http(f, "POST", form, "application/x-www-form-urlencoded", "", &text, &location), ==, 302);
	g_assert_nonnull(location);
	g_assert_nonnull(strstr(location, "/e/quote/"));
	g_clear_pointer(&text, g_free);
	args[2] = f->url;
	args[5] = id;
	g_subprocess_launcher_unsetenv(launcher, "VENTURE_TOKEN");
	child = g_subprocess_launcher_spawnv(launcher, args, &error);
	g_assert_no_error(error);
	g_subprocess_communicate_utf8_async(child, NULL, NULL, cli_done, &result);
	while (!result.done)
		g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(result.error);
	g_test_message("CLI stderr: %s", result.err);
	g_assert_true(g_subprocess_get_successful(child));
	g_assert_nonnull(strstr(result.out, "DEAL-"));
	g_assert_nonnull(strstr(result.out, "-R3"));
	g_free(result.out);
	g_free(result.err);
	g_assert_cmpint(count(f, VENTURE_TYPE_QUOTE, "deal-id", venture_entity_get_id(d)), ==, 3);
	venture_config_set_module_enabled(f->config, "quotes", FALSE);
	g_assert_cmpuint(http(f, "POST", api, "application/json", "{}", &text, NULL), ==, 404);
	venture_config_set_module_enabled(f->config, "quotes", TRUE);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/deal-lines/records", test_records);
	g_test_add("/deal-lines/derived_value", Fixture, NULL, setup, test_derived_value, teardown);
	g_test_add("/deal-lines/generic_value_refused", Fixture, NULL, setup, test_generic_value_refused, teardown);
	g_test_add("/deal-lines/currency_mismatch", Fixture, NULL, setup, test_currency_mismatch, teardown);
	g_test_add("/deal-lines/invalid_lines", Fixture, NULL, setup, test_invalid_lines, teardown);
	g_test_add("/deal-lines/create_quote", Fixture, NULL, setup, test_create_quote, teardown);
	g_test_add("/deal-lines/create_quote_refusals", Fixture, NULL, setup, test_create_quote_refusals, teardown);
	g_test_add("/deal-lines/surfaces", Fixture, NULL, setup, test_surfaces, teardown);
	return g_test_run();
}
