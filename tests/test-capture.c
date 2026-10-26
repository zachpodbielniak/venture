/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <libsoup/soup.h>
#include <string.h>
#include "venture-test-util.h"
#include "venture-test-accounting.h"

typedef struct
{
	VentureDatabase *db;
	VentureContext *context;
	VentureConfig *config;
	gint64 org;
	gint64 vendor;
} Fixture;

static void
save(Fixture *f, VentureEntity *e)
{
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_database_save(f->db, e, NULL, &error));
	g_assert_no_error(error);
}

static void
setup(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) vendor = NULL;
	f->config = venture_config_new();
	f->db = venture_test_accounting_database(&error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
	vendor = g_object_new(VENTURE_TYPE_COMPANY, "organization-id", f->org,
		"name", "Office Supply", "kind", VENTURE_COMPANY_KIND_SUPPLIER, NULL);
	save(f, vendor);
	f->vendor = venture_entity_get_id(vendor);
}

static void
teardown(Fixture *f, gconstpointer unused)
{
	g_clear_object(&f->context);
	venture_test_accounting_database_cleanup(f->db); g_clear_object(&f->db);
	g_clear_object(&f->config);
}

static void
test_records(void)
{
	g_assert_cmpuint(venture_entity_registry_lookup(
		venture_entity_registry_get_default(), "capture_item"), !=, G_TYPE_INVALID);
}

static gint64
count_type(Fixture *f, const gchar *name)
{
	g_autoptr(VentureQuery) q = venture_query_new(
		venture_entity_registry_lookup(venture_entity_registry_get_default(), name));
	venture_query_set_organization(q, f->org);
	return venture_database_count(f->db, q, NULL);
}

static void
test_receipt_becomes_expense(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureMoney) amount = venture_money_new_for_currency(1250, "USD");
	g_autoptr(GDateTime) when = NULL;
	g_autoptr(VentureEntity) item = NULL;
	g_autoptr(VentureEntity) result = NULL;
	g_autoptr(JsonObject) options = json_object_new();
	g_autofree gchar *status = NULL;
	g_autofree gchar *type = NULL;
	gint64 result_id = 0;
	when = g_date_time_new_from_iso8601("2026-01-15T00:00:00Z", NULL);
	item = venture_capture_service_ingest(venture_capture_service_get(f->db),
		"receipt", "Toner", "upload", 0, "Office Supply", amount, when,
		"front desk", NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(item);
	g_assert_cmpint(count_type(f, "capture_item"), ==, 1);
	json_object_set_string_member(options, "category", "office");
	result = venture_capture_service_convert(venture_capture_service_get(f->db),
		item, "expense", options, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(VENTURE_IS_EXPENSE(result));
	g_object_get(item, "status", &status, "result-type", &type, "result-id", &result_id, NULL);
	g_assert_cmpstr(status, ==, "converted");
	g_assert_cmpstr(type, ==, "expense");
	g_assert_cmpint(result_id, ==, venture_entity_get_id(result));
	g_assert_cmpint(count_type(f, "expense"), >=, 1);
}

static void
test_invoice_becomes_bill(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureMoney) amount = venture_money_new_for_currency(4000, "USD");
	g_autoptr(GDateTime) when = NULL;
	g_autoptr(VentureEntity) item = NULL;
	g_autoptr(VentureEntity) result = NULL;
	g_autoptr(JsonObject) options = json_object_new();
	g_autofree gchar *bill_status = NULL;
	when = g_date_time_new_from_iso8601("2026-01-10T00:00:00Z", NULL);
	item = venture_capture_service_ingest(venture_capture_service_get(f->db),
		"supplier_invoice", "January paper", "email", 0, "Office Supply",
		amount, when, NULL, NULL, &error);
	g_assert_no_error(error);
	json_object_set_int_member(options, "company_id", f->vendor);
	result = venture_capture_service_convert(venture_capture_service_get(f->db),
		item, "vendor_bill", options, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(VENTURE_IS_VENDOR_BILL(result));
	g_object_get(result, "status", &bill_status, NULL);
	g_assert_cmpstr(bill_status, ==, "draft");
	g_assert_cmpint(count_type(f, "vendor_bill_line"), ==, 1);
}

static void
test_reject_and_generic_convert_refused(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureMoney) amount = venture_money_new_for_currency(100, "USD");
	g_autoptr(VentureEntity) item = NULL;
	g_autofree gchar *status = NULL;
	item = venture_capture_service_ingest(venture_capture_service_get(f->db),
		"receipt", "Coffee", "upload", 0, "Cafe", amount, NULL, NULL, NULL, &error);
	g_assert_true(venture_capture_service_reject(venture_capture_service_get(f->db),
		item, "personal", NULL, &error));
	g_object_get(item, "status", &status, NULL);
	g_assert_cmpstr(status, ==, "rejected");
	g_object_set(item, "status", "converted", NULL);
	g_assert_false(venture_database_save(f->db, item, NULL, &error));
	g_assert_nonnull(strstr(error->message, "VentureCaptureService"));
}

static void
test_module_off(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureMoney) amount = venture_money_new_for_currency(100, "USD");
	g_autoptr(VentureEntity) item = NULL;
	venture_config_set_module_enabled(f->config, "capture", FALSE);
	item = venture_capture_service_ingest(venture_capture_service_get(f->db),
		"receipt", "X", "upload", 0, NULL, amount, NULL, NULL, NULL, &error);
	g_assert_null(item);
	g_assert_nonnull(error);
	venture_config_set_module_enabled(f->config, "capture", TRUE);
}

typedef struct
{
	gboolean done;
	GError *error;
	GBytes *bytes;
} HttpResult;

typedef struct
{
	Fixture base;
	VentureWebServer *server;
	gchar *directory;
	gboolean require_auth;
} WebFixture;

static void
http_done(GObject *source, GAsyncResult *result, gpointer data)
{
	HttpResult *out = data;

	out->bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &out->error);
	out->done = TRUE;
}

static guint
web_request_bytes(WebFixture *f, const gchar *method, const gchar *path,
	const gchar *content_type, GBytes *body, GBytes **out, gchar **location)
{
	g_autoptr(SoupSession) session = soup_session_new_with_options("timeout", 15, NULL);
	g_autofree gchar *url = g_strconcat(venture_web_server_get_base_url(f->server), path, NULL);
	g_autoptr(SoupMessage) message = soup_message_new(method, url);
	HttpResult result;

	memset(&result, 0, sizeof(result));
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	if (body != NULL)
		soup_message_set_request_body_from_bytes(message, content_type, body);
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL, http_done, &result);
	while (!result.done)
		g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(result.error);
	if (out != NULL)
		*out = g_bytes_ref(result.bytes);
	if (location != NULL)
	{
		const gchar *header = soup_message_headers_get_one(
			soup_message_get_response_headers(message), "Location");

		*location = header != NULL ? g_strdup(header) : NULL;
	}
	g_bytes_unref(result.bytes);
	return soup_message_get_status(message);
}

static guint
web_request(WebFixture *f, const gchar *method, const gchar *path,
	const gchar *content_type, GBytes *body, gchar **out, gchar **location)
{
	g_autoptr(GBytes) bytes = NULL;
	guint status = web_request_bytes(f, method, path, content_type, body, out != NULL ? &bytes : NULL, location);

	if (out != NULL)
		*out = g_strndup(g_bytes_get_data(bytes, NULL), g_bytes_get_size(bytes));
	return status;
}

static void
web_setup(WebFixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;

	memset(f, 0, sizeof(*f));
	f->require_auth = unused != NULL && g_strcmp0(unused, "auth") == 0;
	f->directory = g_dir_make_tmp("venture-capture-snap-XXXXXX", &error);
	g_assert_no_error(error);
	f->base.config = venture_config_new();
	g_object_set(f->base.config, "state-dir", f->directory,
		"server-bind-address", "127.0.0.1", "server-port", (gint64)0,
		"security-require-auth", f->require_auth, NULL);
	f->base.db = venture_test_accounting_database(&error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->base.db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->base.context = venture_context_new(f->base.config, f->base.db);
	f->base.org = venture_context_get_default_organization_id(f->base.context);
	f->server = venture_web_server_new(f->base.context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(f->server, &error));
	g_assert_no_error(error);
}

static void
web_teardown(WebFixture *f, gconstpointer unused)
{
	(void)unused;
	venture_web_server_stop(f->server);
	g_clear_object(&f->server);
	g_clear_object(&f->base.context);
	venture_test_accounting_database_cleanup(f->base.db);
	g_clear_object(&f->base.db);
	g_clear_object(&f->base.config);
	venture_test_remove_tree(f->directory);
	g_free(f->directory);
}

/* A one-pixel PNG. The NUL in the signature is why this is bytes, not a string. */
static const guchar snap_png[] = {
	0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00,
	0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01,
	0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1f,
	0x15, 0xc4, 0x89, 0x00, 0x00, 0x00, 0x0a, 0x49, 0x44, 0x41,
	0x54, 0x78, 0x9c, 0x63, 0x00, 0x01, 0x00, 0x00, 0x05, 0x00,
	0x01, 0x0d, 0x0a, 0x2d, 0xb4, 0x00, 0x00, 0x00, 0x00, 0x49,
	0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82
};

static GBytes *
snap_body(const gchar *amount)
{
	GByteArray *raw = g_byte_array_new();
	gchar *head;
	GBytes *body;

	head = g_strdup_printf(
		"--SNAPBND\r\nContent-Disposition: form-data; name=\"title\"\r\n\r\nCedar Hardware tape\r\n"
		"--SNAPBND\r\nContent-Disposition: form-data; name=\"vendor\"\r\n\r\nCedar Hardware\r\n"
		"--SNAPBND\r\nContent-Disposition: form-data; name=\"amount\"\r\n\r\n%s\r\n"
		"--SNAPBND\r\nContent-Disposition: form-data; name=\"kind\"\r\n\r\nreceipt\r\n"
		"--SNAPBND\r\nContent-Disposition: form-data; name=\"photo\"; filename=\"receipt.png\"\r\n"
		"Content-Type: image/png\r\n\r\n", amount);
	g_byte_array_append(raw, (const guint8 *)head, strlen(head));
	g_free(head);
	g_byte_array_append(raw, snap_png, sizeof(snap_png));
	g_byte_array_append(raw, (const guint8 *)"\r\n--SNAPBND--\r\n", strlen("\r\n--SNAPBND--\r\n"));
	body = g_bytes_new(raw->data, raw->len);
	g_byte_array_unref(raw);
	return body;
}

/* The owner entry is a photo, not the lifecycle form. */
static void
test_owner_snap(WebFixture *f, gconstpointer unused)
{
	g_autofree gchar *inbox = NULL;
	g_autofree gchar *form = NULL;
	g_autofree gchar *filed = NULL;
	g_autoptr(GBytes) photo = NULL;
	g_autoptr(GBytes) expense_body = NULL;
	g_autofree gchar *after = NULL;
	g_autofree gchar *location = NULL;
	g_autoptr(GBytes) upload = NULL;
	g_autoptr(VentureOrganization) other = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) foreign = NULL;
	g_autofree gchar *foreign_path = NULL;
	gchar *slash;

	(void)unused;
	g_assert_cmpuint(web_request(f, "GET", "/capture", NULL, NULL, &inbox, NULL), ==, 200);
	g_assert_nonnull(strstr(inbox, "href=\"/capture/file\""));
	g_assert_nonnull(strstr(inbox, "Snap a receipt"));
	g_assert_null(strstr(inbox, "/e/capture_item/new"));
	g_assert_cmpuint(web_request(f, "GET", "/capture/file", NULL, NULL, &form, NULL), ==, 200);
	g_assert_nonnull(strstr(form, "type=\"file\""));
	g_assert_nonnull(strstr(form, "name=\"photo\""));
	g_assert_nonnull(strstr(form, "capture=\"environment\""));
	g_assert_nonnull(strstr(form, "id=\"receipt-preview\""));
	g_assert_null(strstr(form, "name=\"status\""));
	g_assert_null(strstr(form, "name=\"result-type\""));
	g_assert_null(strstr(form, "name=\"result_type\""));
	g_assert_null(strstr(form, "name=\"document_id\""));
	g_assert_null(strstr(form, "name=\"source\""));
	upload = snap_body("12.50");
	g_assert_cmpuint(web_request(f, "POST", "/capture/file",
		"multipart/form-data; boundary=SNAPBND", upload, NULL, &location), ==, 303);
	g_assert_nonnull(location);
	g_assert_true(g_str_has_prefix(location, "/capture/items/"));
	g_assert_cmpuint(web_request(f, "GET", location, NULL, NULL, &filed, NULL), ==, 200);
	g_assert_nonnull(strstr(filed, "Receipt filed"));
	g_assert_nonnull(strstr(filed, "alt=\"Receipt photo\""));
	g_assert_nonnull(strstr(filed, "File it as an expense"));
	g_assert_null(strstr(filed, "name=\"status\""));
	g_assert_null(strstr(filed, "name=\"result-type\""));
	slash = g_strconcat(location, "/photo", NULL);
	g_assert_cmpuint(web_request_bytes(f, "GET", slash, NULL, NULL, &photo, NULL), ==, 200);
	g_assert_cmpmem(g_bytes_get_data(photo, NULL), g_bytes_get_size(photo), snap_png, sizeof(snap_png));
	g_free(slash);
	slash = g_strconcat(location, "/file-as", NULL);
	expense_body = g_bytes_new_static("as=expense", strlen("as=expense"));
	g_assert_cmpuint(web_request(f, "POST", slash, "application/x-www-form-urlencoded",
		expense_body, NULL, NULL), ==, 303);
	g_free(slash);
	g_assert_cmpuint(web_request(f, "GET", location, NULL, NULL, &after, NULL), ==, 200);
	g_assert_nonnull(strstr(after, "Filed as an expense"));
	g_assert_cmpint(count_type(&f->base, "expense"), >=, 1);
	g_assert_cmpint(count_type(&f->base, "capture_item"), ==, 1);
	g_assert_cmpint(count_type(&f->base, "document"), ==, 1);
	other = venture_organization_new();
	g_object_set(other, "name", "Other snap", "legal-name", "Other snap", "default-currency", "USD", NULL);
	save(&f->base, VENTURE_ENTITY(other));
	foreign = venture_capture_service_ingest_for_organization(venture_capture_service_get(f->base.db),
		venture_entity_get_id(VENTURE_ENTITY(other)), "receipt", "Not yours", "upload",
		0, NULL, NULL, NULL, NULL, NULL, &error);
	g_assert_no_error(error);
	foreign_path = g_strdup_printf("/capture/items/%" G_GINT64_FORMAT, venture_entity_get_id(foreign));
	g_assert_cmpuint(web_request(f, "GET", foreign_path, NULL, NULL, NULL, NULL), ==, 404);
	{
		g_autoptr(GBytes) svg = NULL;
		g_autofree gchar *refused = NULL;
		const gchar *svg_head =
			"--SNAPBND\r\nContent-Disposition: form-data; name=\"photo\"; filename=\"receipt.svg\"\r\n"
			"Content-Type: image/svg+xml\r\n\r\n<svg xmlns=\"http://www.w3.org/2000/svg\"></svg>\r\n"
			"--SNAPBND--\r\n";

		svg = g_bytes_new(svg_head, strlen(svg_head));
		g_assert_cmpuint(web_request(f, "POST", "/capture/file",
			"multipart/form-data; boundary=SNAPBND", svg, &refused, NULL), ==, 422);
		g_assert_nonnull(strstr(refused, "SVG"));
		g_assert_cmpint(count_type(&f->base, "capture_item"), ==, 1);
	}
}

static void
test_owner_snap_signed_out(WebFixture *f, gconstpointer unused)
{
	g_autofree gchar *location = NULL;

	(void)unused;
	g_assert_true(f->require_auth);
	g_assert_cmpuint(web_request(f, "GET", "/capture/file", NULL, NULL, NULL, &location), ==, 302);
	g_assert_cmpstr(location, ==, "/login");
	g_free(location);
	location = NULL;
	g_assert_cmpuint(web_request(f, "POST", "/capture/file", "multipart/form-data; boundary=SNAPBND",
		g_bytes_new_static("", 0), NULL, &location), ==, 302);
	g_assert_cmpstr(location, ==, "/login");
	g_free(location);
	location = NULL;
	g_assert_cmpuint(web_request(f, "GET", "/capture/items/1", NULL, NULL, NULL, &location), ==, 302);
	g_assert_cmpstr(location, ==, "/login");
	g_free(location);
	location = NULL;
	g_assert_cmpuint(web_request(f, "GET", "/capture/items/1/photo", NULL, NULL, NULL, &location), ==, 302);
	g_assert_cmpstr(location, ==, "/login");
	g_free(location);
	location = NULL;
	g_assert_cmpuint(web_request(f, "POST", "/capture/items/1/file-as",
		"application/x-www-form-urlencoded", g_bytes_new_static("as=expense", 10), NULL, &location), ==, 302);
	g_assert_cmpstr(location, ==, "/login");
}

/* A receipt follows the selected books through capture and conversion. */
static void
test_scoped_capture(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureOrganization) other = venture_organization_new();
	g_autoptr(VentureMoney) amount = venture_money_new_for_currency(1200, "USD");
	g_autoptr(GDateTime) when = venture_time_from_string("2026-01-15", NULL);
	g_autoptr(VentureEntity) item = NULL;
	g_autoptr(VentureEntity) expense = NULL;
	gint64 org;
	(void)unused;
	g_object_set(other, "name", "Other capture", "legal-name", "Other capture", "default-currency", "USD", NULL);
	save(f, VENTURE_ENTITY(other));
	org = venture_entity_get_id(VENTURE_ENTITY(other));
	item = venture_capture_service_ingest_for_organization(venture_capture_service_get(f->db),
		org, "receipt", "Supplies", "upload", 0, NULL, amount, when, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(item);
	g_assert_cmpint(venture_entity_get_organization_id(item), ==, org);
	expense = venture_capture_service_convert(venture_capture_service_get(f->db), item, "expense", NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(expense);
	g_assert_cmpint(venture_entity_get_organization_id(expense), ==, org);
	g_assert_cmpint(count_type(f, "capture_item"), ==, 0);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	venture_entity_registry_register_builtins(venture_entity_registry_get_default());
	g_test_add_func("/capture/records", test_records);
	g_test_add("/capture/expense", Fixture, NULL, setup, test_receipt_becomes_expense, teardown);
	g_test_add("/capture/bill", Fixture, NULL, setup, test_invoice_becomes_bill, teardown);
	g_test_add("/capture/reject", Fixture, NULL, setup, test_reject_and_generic_convert_refused, teardown);
	g_test_add("/capture/module-off", Fixture, NULL, setup, test_module_off, teardown);
	g_test_add("/capture/scoped", Fixture, NULL, setup, test_scoped_capture, teardown);
	g_test_add("/capture/owner-snap", WebFixture, NULL, web_setup, test_owner_snap, web_teardown);
	g_test_add("/capture/owner-snap-signed-out", WebFixture, "auth", web_setup, test_owner_snap_signed_out, web_teardown);
	return g_test_run();
}
