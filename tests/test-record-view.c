/*
 * test-record-view.c - A record reads like a record, not like its table.
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Every record page, list and form is derived from the field table through
 * one classification, venture_field_spec_get_role(). These tests pin the
 * classification itself and what a person sees because of it: no internal
 * name, no raw identifier and no empty row on a page by default, and the
 * full field list still one click away.
 */

#include <venture.h>

#include <string.h>
#include <libsoup/soup.h>

#include "venture-test-util.h"
#include "venture-test-accounting.h"

/* --- The classification ---------------------------------------------------- */

static VentureFieldRole
role_of(
	const gchar		*name,
	VentureFieldKind	 kind,
	VentureColumnFlags	 flags
){
	g_autoptr(VentureFieldSpec) spec = NULL;

	spec = venture_field_spec_new(name, NULL, kind);
	spec->flags = flags;

	return venture_field_spec_get_role(spec);
}

/*
 * If this regresses, machinery comes back onto every record page: replay
 * keys, digests and provider ids read as facts, and a record's status is
 * lost among its details instead of sitting beside its title.
 */
static void
test_roles(void)
{
	/* Declared outright. */
	g_assert_cmpint(role_of("lookback-days", VENTURE_FIELD_KIND_INTEGER,
		VENTURE_COLUMN_FLAG_TECHNICAL), ==, VENTURE_FIELD_ROLE_TECHNICAL);
	g_assert_cmpint(role_of("token", VENTURE_FIELD_KIND_STRING,
		VENTURE_COLUMN_FLAG_SENSITIVE), ==, VENTURE_FIELD_ROLE_TECHNICAL);

	/* Named like machinery. */
	g_assert_cmpint(role_of("submission-key", VENTURE_FIELD_KIND_STRING,
		VENTURE_COLUMN_FLAG_NONE), ==, VENTURE_FIELD_ROLE_TECHNICAL);
	g_assert_cmpint(role_of("payload-hash", VENTURE_FIELD_KIND_STRING,
		VENTURE_COLUMN_FLAG_NONE), ==, VENTURE_FIELD_ROLE_TECHNICAL);
	g_assert_cmpint(role_of("connection-version", VENTURE_FIELD_KIND_INTEGER,
		VENTURE_COLUMN_FLAG_NONE), ==, VENTURE_FIELD_ROLE_TECHNICAL);
	g_assert_cmpint(role_of("stripe-customer-id", VENTURE_FIELD_KIND_STRING,
		VENTURE_COLUMN_FLAG_NONE), ==, VENTURE_FIELD_ROLE_TECHNICAL);

	/* A reference to one of our own records is followable, and stays. */
	g_assert_cmpint(role_of("company-id", VENTURE_FIELD_KIND_REFERENCE,
		VENTURE_COLUMN_FLAG_NONE), ==, VENTURE_FIELD_ROLE_FACT);

	g_assert_cmpint(role_of("notes", VENTURE_FIELD_KIND_TEXT,
		VENTURE_COLUMN_FLAG_NONE), ==, VENTURE_FIELD_ROLE_CONTENT);
	g_assert_cmpint(role_of("status", VENTURE_FIELD_KIND_ENUM,
		VENTURE_COLUMN_FLAG_NONE), ==, VENTURE_FIELD_ROLE_STATUS);
	g_assert_cmpint(role_of("stage", VENTURE_FIELD_KIND_ENUM,
		VENTURE_COLUMN_FLAG_NONE), ==, VENTURE_FIELD_ROLE_STATUS);
	g_assert_cmpint(role_of("kind", VENTURE_FIELD_KIND_ENUM,
		VENTURE_COLUMN_FLAG_NONE), ==, VENTURE_FIELD_ROLE_FACT);
	g_assert_cmpint(role_of("fields", VENTURE_FIELD_KIND_JSON,
		VENTURE_COLUMN_FLAG_NONE), ==, VENTURE_FIELD_ROLE_STRUCTURED);
	g_assert_cmpint(role_of("email", VENTURE_FIELD_KIND_STRING,
		VENTURE_COLUMN_FLAG_NONE), ==, VENTURE_FIELD_ROLE_FACT);
}

/*
 * A declarative type says "technical" the same way a built-in does, and
 * the flag survives into the spec -- the field table's declarations are
 * what every surface reads.
 */
static void
test_declared_technical(void)
{
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(VentureFieldSpec) spec = NULL;
	g_autoptr(GError) error = NULL;

	node = venture_json_parse("{\"type\":\"integer\",\"technical\":true}",
	                          &error);
	g_assert_no_error(error);
	spec = venture_field_spec_new_from_json("sync-window", node, &error);
	g_assert_no_error(error);
	g_assert_nonnull(spec);
	g_assert_true(0 != (venture_field_spec_get_flags(spec) &
	                    VENTURE_COLUMN_FLAG_TECHNICAL));
	g_assert_cmpint(venture_field_spec_get_role(spec), ==,
	                VENTURE_FIELD_ROLE_TECHNICAL);
}

/*
 * A type is named the way a person names it, singular and plural, and a
 * type whose internal name describes machinery says what it is instead.
 */
static void
test_type_labels(void)
{
	static const struct {
		GType (*type)(void);
		const gchar *singular;
		const gchar *plural;
	} cases[] = {
		{ venture_contact_get_type, "Contact", "Contacts" },
		{ venture_company_get_type, "Company", "Companies" },
		{ venture_tax_category_get_type, "Tax category", "Tax categories" },
		{ venture_journal_line_get_type, "Journal line", "Journal lines" },
		{ venture_attribution_submission_get_type, "Form submission",
		  "Form submissions" },
	};
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++)
	{
		g_autofree gchar *singular = NULL;
		g_autofree gchar *plural = NULL;

		singular = venture_entity_type_dup_label(cases[i].type(), FALSE);
		plural = venture_entity_type_dup_label(cases[i].type(), TRUE);

		g_assert_cmpstr(singular, ==, cases[i].singular);
		g_assert_cmpstr(plural, ==, cases[i].plural);
	}
}

/* --- What a person sees ---------------------------------------------------- */

typedef struct
{
	VentureConfig *config;
	VentureDatabase *database;
	VentureContext *context;
	VentureWebServer *server;
	gchar *state_dir;
	gint64 organization_id;
} Fixture;

static void
save(Fixture *f, VentureEntity *record)
{
	g_autoptr(GError) error = NULL;

	venture_entity_set_organization_id(record, f->organization_id);
	if (!venture_database_save(f->database, record, NULL, &error))
		g_error("save %s: %s", G_OBJECT_TYPE_NAME(record), error->message);
}

static void
set_up(Fixture *f, gconstpointer data)
{
	g_autoptr(GSocketListener) listener = NULL;
	g_autoptr(GError) error = NULL;
	guint16 port;

	(void)data;

	f->config = venture_config_new();
	f->database = venture_test_accounting_database(&error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->database,
		venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->database);
	f->organization_id = venture_context_get_default_organization_id(f->context);

	f->state_dir = g_dir_make_tmp("venture-record-view-XXXXXX", &error);
	g_assert_no_error(error);
	listener = g_socket_listener_new();
	port = g_socket_listener_add_any_inet_port(listener, NULL, &error);
	g_assert_no_error(error);
	g_socket_listener_close(listener);
	g_object_set(f->config, "state-dir", f->state_dir,
	             "server-bind-address", "127.0.0.1",
	             "server-port", (gint64)port,
	             "security-require-auth", FALSE, NULL);
	f->server = venture_web_server_new(f->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(f->server, &error));
	g_assert_no_error(error);
}

static void
tear_down(Fixture *f, gconstpointer data)
{
	(void)data;

	venture_web_server_stop(f->server);
	g_clear_object(&f->server);
	g_clear_object(&f->context);
	venture_test_accounting_database_cleanup(f->database);
	g_clear_object(&f->database);
	g_clear_object(&f->config);
	venture_test_remove_tree(f->state_dir);
	g_free(f->state_dir);
}

typedef struct
{
	gboolean done;
	GBytes *bytes;
	GError *error;
} Reply;

static void
reply_done(GObject *source, GAsyncResult *result, gpointer data)
{
	Reply *reply = data;

	reply->bytes = soup_session_send_and_read_finish(SOUP_SESSION(source),
	                                                 result, &reply->error);
	reply->done = TRUE;
}

/*
 * GETs @path and returns the page. Async on this thread's main context,
 * because the server answering it runs on the same one.
 */
static gchar *
get(Fixture *f, const gchar *path)
{
	g_autoptr(SoupSession) session = NULL;
	g_autoptr(SoupMessage) message = NULL;
	g_autofree gchar *url = NULL;
	Reply reply;
	gchar *body;

	memset(&reply, 0, sizeof(reply));
	session = soup_session_new_with_options("timeout", 15, NULL);
	url = g_strconcat(venture_web_server_get_base_url(f->server), path, NULL);
	message = soup_message_new("GET", url);
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT,
	                                 NULL, reply_done, &reply);

	while (!reply.done)
		g_main_context_iteration(NULL, TRUE);

	g_assert_no_error(reply.error);
	g_assert_cmpuint(soup_message_get_status(message), ==, 200);
	body = g_strndup(g_bytes_get_data(reply.bytes, NULL),
	                 g_bytes_get_size(reply.bytes));
	g_bytes_unref(reply.bytes);

	return body;
}

/*
 * The page up to where "All fields" opens: what a person sees without
 * clicking anything. The folded list after it is allowed everything.
 */
static gchar *
visible_part(const gchar *page)
{
	const gchar *fold;

	fold = strstr(page, "<details class=\"all-fields\">");
	g_assert_nonnull(fold);

	return g_strndup(page, fold - page);
}

/*
 * A form submission: who wrote in and what they wrote, the contact it
 * belongs to by name, and no replay key, digest or UUID anywhere a person
 * looks by default. If this regresses, the page is the database again:
 * "SUBMISSION REPLAY IDENTITY" in capitals and a message nowhere.
 */
static void
test_submission_page(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) contact = NULL;
	g_autoptr(VentureEntity) form = NULL;
	g_autoptr(VentureEntity) site = NULL;
	g_autoptr(VentureEntity) submission = NULL;
	g_autoptr(JsonObject) payload = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autofree gchar *key = NULL;
	JsonObject *fields;
	g_autofree gchar *path = NULL;
	g_autofree gchar *page = NULL;
	g_autofree gchar *seen = NULL;

	(void)data;

	contact = g_object_new(VENTURE_TYPE_CONTACT, "name", "Existing Customer",
	                       "email", "form@example.test", NULL);
	save(f, contact);

	/* Through the capture service, the only writer a submission has --
	 * and into a contact who already exists, the case whose message used
	 * to be lost. */
	form = g_object_new(VENTURE_TYPE_LEAD_FORM, "name", "Site inquiry",
	                    "active", TRUE, NULL);
	save(f, form);
	site = g_object_new(VENTURE_TYPE_ATTRIBUTION_SITE, "name", "Portfolio site",
		"origin", "https://site.example.test", "external-site-id", "site_one",
		"external-tenant-id", "tenant_one",
		"lead-form-id", venture_entity_get_id(form),
		"consent-policy", "analytics-v1", "active", TRUE, NULL);
	save(f, site);

	payload = json_object_new();
	fields = json_object_new();
	json_object_set_int_member(payload, "version", 1);
	json_object_set_string_member(payload, "submission_id", "replay-key-0001");
	json_object_set_string_member(fields, "name", "Form Visitor");
	json_object_set_string_member(fields, "email", "form@example.test");
	json_object_set_string_member(fields, "message",
	                              "Please call me about the spring order.");
	json_object_set_object_member(payload, "fields", fields);

	submission = VENTURE_ENTITY(venture_attribution_service_capture(
		venture_attribution_service_get(f->database),
		venture_entity_get_uuid(site), "https://site.example.test", payload,
		now, &error));
	g_assert_no_error(error);
	g_assert_nonnull(submission);

	path = g_strdup_printf("/e/attribution_submission/%" G_GINT64_FORMAT,
	                       venture_entity_get_id(submission));
	page = get(f, path);
	seen = visible_part(page);

	/* Who wrote in, titled and linked; what they wrote, in full. */
	g_assert_nonnull(strstr(seen, "<h1>Form Visitor</h1>"));
	g_assert_nonnull(strstr(seen, "Please call me about the spring order."));
	g_assert_nonnull(strstr(seen, "mailto:form@example.test"));
	g_assert_nonnull(strstr(seen, ">Existing Customer</a>"));

	/* Named as a person would name it, never by its table. */
	g_assert_nonnull(strstr(seen, "Form submission"));
	g_assert_null(strstr(seen, ">attribution_submission<"));

	/* Machinery is folded; the identity spine is folded; a sensitive
	 * value is nowhere at all. */
	g_object_get(submission, "submission-key", &key, NULL);
	g_assert_nonnull(strstr(key, "replay-key-0001"));
	g_assert_null(strstr(seen, "replay-key-0001"));
	g_assert_null(strstr(seen, "Submission replay identity"));
	g_assert_null(strstr(seen, venture_entity_get_uuid(submission)));
	g_assert_nonnull(strstr(page, "replay-key-0001"));
	g_assert_nonnull(strstr(page, venture_entity_get_uuid(submission)));
	{
		g_autofree gchar *digest = NULL;

		/* payload-hash is sensitive: not in view, not folded, not
		 * anywhere in the page. */
		g_object_get(submission, "payload-hash", &digest, NULL);
		g_assert_false(venture_string_is_empty(digest));
		g_assert_null(strstr(page, digest));
	}

	/* No empty rows: the lead was never set, so there is no Lead row
	 * until somebody opens All fields. */
	g_assert_null(strstr(seen, "<dt>Lead</dt>"));
	g_assert_nonnull(strstr(page, "<dt>Lead</dt>"));
}

/*
 * A status is a badge beside the title, in words; a list's columns are
 * what somebody scans for, and a reference reads as the other record's
 * name rather than its number.
 */
static void
test_status_and_list(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) company = NULL;
	g_autoptr(VentureEntity) deal = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *page = NULL;
	g_autofree gchar *list = NULL;
	const gchar *head;
	const gchar *body;

	(void)data;

	company = g_object_new(VENTURE_TYPE_COMPANY, "name", "Bellhaven Books",
	                       "external-id", "ext-9f2c", NULL);
	save(f, company);

	deal = g_object_new(VENTURE_TYPE_DEAL, "name", "Spring catalogue",
	                    "company-id", venture_entity_get_id(company), NULL);
	g_assert_true(venture_entity_set_field_from_string(deal, "stage",
	                                                   "proposal", &error));
	g_assert_no_error(error);
	save(f, deal);

	path = g_strdup_printf("/e/deal/%" G_GINT64_FORMAT,
	                       venture_entity_get_id(deal));
	page = get(f, path);
	g_assert_nonnull(strstr(page, "status-badge"));
	g_assert_nonnull(strstr(page, "data-status=\"proposal\">Proposal</span>"));

	list = get(f, "/e/company");
	head = strstr(list, "<thead>");
	body = strstr(list, "<tbody>");
	g_assert_nonnull(head);
	g_assert_nonnull(body);

	/* A provider's id is machinery, not a column. */
	g_assert_null(g_strstr_len(head, body - head, "External ID"));
	g_assert_null(strstr(body, "ext-9f2c"));

	/* A column nobody filled in on this page is not drawn. */
	g_assert_null(g_strstr_len(head, body - head, "Legal name"));

	/* The list is titled in the plural, and its button says what it
	 * makes. */
	g_assert_nonnull(strstr(list, "<h1>Companies</h1>"));
	g_assert_nonnull(strstr(list, ">New company</a>"));
}

/*
 * A form shows what is needed to make a useful record, gives prose its
 * room, and folds the rest: optional details under "More details" and
 * machinery under "Advanced". Folded inputs are still in the form, so
 * nothing stops posting. Choices read as words, and post as the value.
 */
static void
test_grouped_form(Fixture *f, gconstpointer data)
{
	g_autofree gchar *form = NULL;
	const gchar *advanced;

	(void)data;

	form = get(f, "/e/company/new");

	g_assert_nonnull(strstr(form, "<h1>New company</h1>"));
	g_assert_nonnull(strstr(form, "form-section form-prose"));
	g_assert_nonnull(strstr(form, "<summary>More details"));

	advanced = strstr(form, "form-section form-advanced");
	g_assert_nonnull(advanced);
	g_assert_nonnull(strstr(advanced, "name=\"external-id\""));

	/* The required name is in view, never folded. */
	g_assert_true(strstr(form, "name=\"name\"") <
	              strstr(form, "<summary>More details"));

	g_assert_nonnull(strstr(form, "<option value=\"supplier\">Supplier</option>"));
}

/*
 * POSTs a form and returns the status; the Location lands in @location and
 * the body in @reply_body. @accept, when set, is sent as the Accept header,
 * the way a browser sends one; @inline adds the header the page's own
 * form handling sends.
 */
static guint
post_form_full(Fixture *f, const gchar *path, const gchar *body, const gchar *accept,
	gboolean inline_errors, gchar **location, gchar **reply_body)
{
	g_autoptr(SoupSession) session = NULL;
	g_autoptr(SoupMessage) message = NULL;
	g_autoptr(GBytes) bytes = NULL;
	g_autofree gchar *url = NULL;
	Reply reply;

	memset(&reply, 0, sizeof(reply));
	session = soup_session_new_with_options("timeout", 15, NULL);
	url = g_strconcat(venture_web_server_get_base_url(f->server), path, NULL);
	message = soup_message_new("POST", url);
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	if (NULL != accept)
		soup_message_headers_replace(soup_message_get_request_headers(message), "Accept", accept);
	if (inline_errors)
		soup_message_headers_replace(soup_message_get_request_headers(message), "X-Venture-Inline", "1");
	bytes = g_bytes_new(body, strlen(body));
	soup_message_set_request_body_from_bytes(message,
		"application/x-www-form-urlencoded", bytes);
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT,
	                                 NULL, reply_done, &reply);

	while (!reply.done)
		g_main_context_iteration(NULL, TRUE);

	g_assert_no_error(reply.error);
	/* A refusal says why; keep it in the test log. */
	if (soup_message_get_status(message) >= 400 && NULL != reply.bytes)
		g_test_message("%s: %.*s", path, (gint)g_bytes_get_size(reply.bytes),
		               (const gchar *)g_bytes_get_data(reply.bytes, NULL));
	if (NULL != reply_body && NULL != reply.bytes)
		*reply_body = g_strndup(g_bytes_get_data(reply.bytes, NULL), g_bytes_get_size(reply.bytes));
	g_clear_pointer(&reply.bytes, g_bytes_unref);
	if (NULL != location)
		*location = g_strdup(soup_message_headers_get_one(
			soup_message_get_response_headers(message), "Location"));

	return soup_message_get_status(message);
}

static guint
post_form(Fixture *f, const gchar *path, const gchar *body, gchar **location)
{
	return post_form_full(f, path, body, NULL, FALSE, location, NULL);
}

/*
 * "Same invoice, on a schedule" is the invoice sheet in repeat mode: the
 * customer and lines typed there become a repeating schedule whose
 * template issues exactly those lines, with nothing of a draft invoice --
 * no number, no identity -- carried into every invoice it will make. If
 * this regresses, repeating an invoice means typing JSON again.
 */
static void
test_repeating_invoice(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) company = NULL;
	g_autoptr(VentureEntity) schedule = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) template = NULL;
	g_autofree gchar *location = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *kind = NULL;
	g_autofree gchar *text = NULL;
	JsonObject *object;
	JsonArray *lines;
	gint64 id;

	(void)data;

	company = g_object_new(VENTURE_TYPE_COMPANY, "name", "Bellhaven Books", NULL);
	save(f, company);

	body = g_strdup_printf("compose-form=1&repeat=1&company-id=%" G_GINT64_FORMAT
		"&line-0-description=Hosting&line-0-quantity=1&line-0-unit-price=40.00"
		"&line-3-description=Support&line-3-quantity=2&line-3-unit-price=15.00"
		"&repeat-frequency=monthly&repeat-start=2026-10-01",
		venture_entity_get_id(company));
	g_assert_cmpuint(post_form(f, "/invoices/compose", body, &location), ==, 302);
	g_assert_true(g_str_has_prefix(location, "/e/recurring_schedule/"));

	id = g_ascii_strtoll(location + strlen("/e/recurring_schedule/"), NULL, 10);
	schedule = venture_database_get(f->database, VENTURE_TYPE_RECURRING_SCHEDULE, id, &error);
	g_assert_no_error(error);
	g_assert_nonnull(schedule);

	/* A JSON field is stored as its text. */
	g_object_get(schedule, "template", &text, NULL);
	g_assert_nonnull(text);
	template = venture_json_parse(text, &error);
	g_assert_no_error(error);
	g_assert_nonnull(template);
	object = json_node_get_object(template);
	g_assert_cmpint(json_object_get_int_member(object, "company_id"), ==,
	                venture_entity_get_id(company));
	g_assert_false(json_object_has_member(object, "id"));
	g_assert_false(json_object_has_member(object, "number"));
	lines = json_object_get_array_member(object, "lines");
	g_assert_cmpuint(json_array_get_length(lines), ==, 2);
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(lines, 1),
		"description"), ==, "Support");
	g_assert_null(strstr(text, "\"uuid\""));
	(void)kind;
}

/*
 * A bill entered on the sheet is the bill and its lines, written together,
 * and "Save and approve" approves it through the payables service -- the
 * same step as the bill page's Approve. A draft stays a draft. If this
 * regresses, a bill is a header with no lines, entered one form at a time.
 */
static void
test_bill_sheet(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) supplier = NULL;
	g_autoptr(VentureEntity) bill = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) lines = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *location = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *status = NULL;
	gint64 id;

	(void)data;

	supplier = g_object_new(VENTURE_TYPE_COMPANY, "name", "Hosting Co", NULL);
	g_assert_true(venture_entity_set_field_from_string(supplier, "kind", "supplier", &error));
	g_assert_no_error(error);
	save(f, supplier);

	body = g_strdup_printf("company-id=%" G_GINT64_FORMAT "&number=HC-1&bill-date=2026-09-01"
		"&due-date=2026-10-01&line-0-description=Servers&line-0-quantity=1&line-0-unit-price=40.00"
		"&line-2-description=Backups&line-2-quantity=2&line-2-unit-price=5.00",
		venture_entity_get_id(supplier));
	g_assert_cmpuint(post_form(f, "/bills/compose", body, &location), ==, 302);
	g_assert_true(g_str_has_prefix(location, "/e/vendor_bill/"));
	id = g_ascii_strtoll(location + strlen("/e/vendor_bill/"), NULL, 10);

	bill = venture_database_get(f->database, VENTURE_TYPE_VENDOR_BILL, id, &error);
	g_assert_no_error(error);
	g_object_get(bill, "status", &status, NULL);
	g_assert_cmpstr(status, ==, "draft");

	query = venture_query_new(VENTURE_TYPE_VENDOR_BILL_LINE);
	venture_query_add_filter_int(query, "bill-id", VENTURE_FILTER_OP_EQ, id, NULL);
	lines = venture_database_find(f->database, query, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(lines->len, ==, 2);

	/* Save and approve: approved in the same request. */
	g_clear_pointer(&body, g_free);
	g_clear_pointer(&location, g_free);
	g_clear_pointer(&status, g_free);
	g_clear_object(&bill);
	body = g_strdup_printf("company-id=%" G_GINT64_FORMAT "&number=HC-2&bill-date=2026-09-01"
		"&line-0-description=Servers&line-0-quantity=1&line-0-unit-price=40.00&approve=1",
		venture_entity_get_id(supplier));
	g_assert_cmpuint(post_form(f, "/bills/compose", body, &location), ==, 302);
	id = g_ascii_strtoll(location + strlen("/e/vendor_bill/"), NULL, 10);
	bill = venture_database_get(f->database, VENTURE_TYPE_VENDOR_BILL, id, &error);
	g_assert_no_error(error);
	g_object_get(bill, "status", &status, NULL);
	g_assert_cmpstr(status, ==, "approved");
}

/* A tax rate, as the rate page makes one. */
static VentureEntity *
tax_rate(Fixture *f, const gchar *code, const gchar *name, gint64 numerator, gint64 denominator)
{
	VentureEntity *rate = g_object_new(VENTURE_TYPE_TAX_CODE, "code", code, "name", name,
		"rate-numerator", numerator, "rate-denominator", denominator, "active", TRUE, NULL);

	save(f, rate);
	return rate;
}

/*
 * The invoice sheet taxes a line by picking a rate -- "No tax" first and
 * chosen, then each rate with its percent -- and removes a line with a
 * bin you can see. A customer's exemption rides on the customer's option
 * so choosing them fills it in. If this regresses, tax is a number typed
 * on every line and the return cannot say what it was charged under.
 */
static void
test_invoice_sheet_tax(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) rate = NULL;
	g_autoptr(VentureEntity) charity = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *page = NULL;

	(void)data;

	rate = tax_rate(f, "NY", "New York sales tax", 8875, 100000);
	charity = g_object_new(VENTURE_TYPE_COMPANY, "name", "Food Bank", "tax-exempt", TRUE,
		"tax-exempt-reason", "Non-profit (501(c)(3))", "tax-exemption-number", "EX-77", NULL);
	save(f, charity);

	page = get(f, "/invoices/compose");
	g_assert_nonnull(strstr(page, "<option value=\"\" data-rate=\"0\">No tax</option>"));
	g_assert_nonnull(strstr(page, "New York sales tax \xc2\xb7 8.875%</option>"));
	g_assert_nonnull(strstr(page, "data-rate=\"8.875\""));
	g_assert_nonnull(strstr(page, "name=\"line-0-tax-code-id\""));
	g_assert_null(strstr(page, "name=\"line-0-tax-percent\""));
	g_assert_nonnull(strstr(page, "class=\"line-delete\""));
	g_assert_nonnull(strstr(page, "data-exempt=\"1\" data-exempt-kind=\"Non-profit (501(c)(3))\" "
		"data-exempt-number=\"EX-77\""));
	g_assert_nonnull(strstr(page, "name=\"tax-exempt\""));
	(void)error;
}

/*
 * A line taxed at a rate keeps the rate; an exemption ticked on the sheet
 * is frozen onto the invoice with its certificate and, by default,
 * remembered on the customer so the next invoice fills it in. If this
 * regresses, a non-profit is charged tax on the invoice after the one
 * somebody remembered to fix by hand.
 */
static void
test_invoice_sheet_exemption(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) rate = NULL;
	g_autoptr(VentureEntity) customer = NULL;
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureEntity) again = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) lines = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *body = NULL, *location = NULL, *reason = NULL;
	g_autofree gchar *company_reason = NULL, *company_number = NULL;
	gboolean exempt = FALSE, company_exempt = FALSE;
	gint64 id, code = 0;

	(void)data;

	rate = tax_rate(f, "NY", "New York sales tax", 8875, 100000);
	customer = g_object_new(VENTURE_TYPE_COMPANY, "name", "Harbour Library", NULL);
	save(f, customer);

	body = g_strdup_printf("compose-form=1&company-id=%" G_GINT64_FORMAT
		"&line-0-description=Shelving&line-0-quantity=1&line-0-unit-price=400.00"
		"&line-0-tax-code-id=%" G_GINT64_FORMAT
		"&tax-exempt=true&exempt-kind=Non-profit+(501(c)(3))&exempt-number=EX-12&exempt-remember=1",
		venture_entity_get_id(customer), venture_entity_get_id(rate));
	g_assert_cmpuint(post_form(f, "/invoices/compose", body, &location), ==, 302);
	g_assert_true(g_str_has_prefix(location, "/e/invoice/"));
	id = g_ascii_strtoll(location + strlen("/e/invoice/"), NULL, 10);

	invoice = venture_database_get(f->database, VENTURE_TYPE_INVOICE, id, &error);
	g_assert_no_error(error);
	g_object_get(invoice, "tax-exempt", &exempt, "tax-exempt-reason", &reason, NULL);
	g_assert_true(exempt);
	g_assert_cmpstr(reason, ==, "Non-profit (501(c)(3)), certificate EX-12");

	query = venture_query_new(VENTURE_TYPE_INVOICE_LINE);
	venture_query_add_filter_int(query, "invoice-id", VENTURE_FILTER_OP_EQ, id, NULL);
	lines = venture_database_find(f->database, query, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(lines->len, ==, 1);
	g_object_get(g_ptr_array_index(lines, 0), "tax-code-id", &code, NULL);
	g_assert_cmpint(code, ==, venture_entity_get_id(rate));

	again = venture_database_get(f->database, VENTURE_TYPE_COMPANY,
		venture_entity_get_id(customer), &error);
	g_assert_no_error(error);
	g_object_get(again, "tax-exempt", &company_exempt, "tax-exempt-reason", &company_reason,
		"tax-exemption-number", &company_number, NULL);
	g_assert_true(company_exempt);
	g_assert_cmpstr(company_reason, ==, "Non-profit (501(c)(3))");
	g_assert_cmpstr(company_number, ==, "EX-12");
}

/*
 * "Attention of" is somebody at the customer: an invoice to one company
 * for the attention of somebody at another is refused at the save, from
 * any writer, and the form offers only the customer's people. A record
 * whose contact has since moved company stays editable. If this
 * regresses, Tesla's buyer can be put on Amazon's invoice.
 */
static void
test_attention_of_same_customer(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) amazon = NULL, tesla = NULL, buyer = NULL, elon = NULL;
	g_autoptr(VentureEntity) invoice = NULL, quote = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *page = NULL, *parent = NULL;

	(void)data;

	amazon = g_object_new(VENTURE_TYPE_COMPANY, "name", "Amazon", NULL);
	tesla = g_object_new(VENTURE_TYPE_COMPANY, "name", "Tesla", NULL);
	save(f, amazon);
	save(f, tesla);
	buyer = g_object_new(VENTURE_TYPE_CONTACT, "name", "Andy Buyer",
		"company-id", venture_entity_get_id(amazon), NULL);
	elon = g_object_new(VENTURE_TYPE_CONTACT, "name", "Elon",
		"company-id", venture_entity_get_id(tesla), NULL);
	save(f, buyer);
	save(f, elon);

	g_assert_cmpstr(venture_entity_class_get_shared_parent(
		g_type_class_peek(VENTURE_TYPE_INVOICE), "contact-id"), ==, "company-id");

	invoice = g_object_new(VENTURE_TYPE_INVOICE, "number", "INV-9", "company-id",
		venture_entity_get_id(amazon), "contact-id", venture_entity_get_id(elon), NULL);
	venture_entity_set_organization_id(invoice, f->organization_id);
	g_assert_false(venture_database_save(f->database, invoice, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_nonnull(strstr(error->message, "Attention of"));
	g_clear_error(&error);

	quote = g_object_new(VENTURE_TYPE_QUOTE, "number", "Q-9", "company-id",
		venture_entity_get_id(amazon), "contact-id", venture_entity_get_id(elon), NULL);
	venture_entity_set_organization_id(quote, f->organization_id);
	g_assert_false(venture_database_save(f->database, quote, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);

	g_object_set(invoice, "contact-id", venture_entity_get_id(buyer), NULL);
	save(f, invoice);

	/* The buyer moves to Tesla; the invoice's notes can still be fixed. */
	g_object_set(buyer, "company-id", venture_entity_get_id(tesla), NULL);
	save(f, buyer);
	g_object_set(invoice, "notes", "Called about it", NULL);
	save(f, invoice);

	page = get(f, "/e/invoice/new");
	parent = g_strdup_printf("data-parent=\"%" G_GINT64_FORMAT "\">Elon</option>",
		venture_entity_get_id(tesla));
	g_assert_nonnull(strstr(page, "<select name=\"contact-id\" data-same-parent=\"company-id\">"));
	g_assert_nonnull(strstr(page, parent));
}

/*
 * A refusal reaches a person as words, never as JSON: a browser posting
 * a form with scripting off gets a page in the app saying what to fix,
 * the page's own form handling gets the JSON it shows above the form,
 * and a client that did not ask for HTML is answered exactly as before.
 * If this regresses, a typo in a form is a screen of braces.
 */
static void
test_errors_for_people(Fixture *f, gconstpointer data)
{
	g_autofree gchar *page = NULL, *json = NULL, *plain = NULL;
	const gchar *body = "compose-form=1&company-id=&line-0-description=";

	(void)data;

	g_assert_cmpuint(post_form_full(f, "/invoices/compose", body,
		"text/html,application/xhtml+xml", FALSE, NULL, &page), ==, 422);
	g_assert_nonnull(strstr(page, "<html"));
	g_assert_nonnull(strstr(page, "something needs fixing"));
	g_assert_nonnull(strstr(page, "Go back and fix it"));
	/* In the reader's words: the service's name is for the log. */
	g_assert_nonnull(strstr(page, "At least one line is required"));
	g_assert_null(strstr(page, "VentureDocumentService"));
	g_assert_null(strstr(page, "\"error\":"));

	g_assert_cmpuint(post_form_full(f, "/invoices/compose", body,
		"text/html", TRUE, NULL, &json), ==, 422);
	g_assert_nonnull(strstr(json, "\"message\""));

	g_assert_cmpuint(post_form_full(f, "/invoices/compose", body,
		NULL, FALSE, NULL, &plain), ==, 422);
	g_assert_nonnull(strstr(plain, "\"message\""));
}

/*
 * A tax rate is made from a name and a percent, and stored exactly: 8.875
 * is 8875 over 100000, not a float. If this regresses, a person is asked
 * for a numerator and a denominator to charge sales tax.
 */
static void
test_tax_rate_page(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) rate = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *page = NULL, *location = NULL, *code = NULL;
	gint64 numerator = 0, denominator = 0;

	(void)data;

	page = get(f, "/tax-rates/new");
	g_assert_nonnull(strstr(page, "name=\"rate\""));
	g_assert_null(strstr(page, "numerator"));

	g_assert_cmpuint(post_form(f, "/tax-rates/new",
		"name=New+York+City&rate=8.875&jurisdiction=US-NY", &location), ==, 302);
	query = venture_query_new(VENTURE_TYPE_TAX_CODE);
	rate = venture_database_find_one(f->database, query, &error);
	g_assert_no_error(error);
	g_assert_nonnull(rate);
	g_object_get(rate, "rate-numerator", &numerator, "rate-denominator", &denominator,
		"code", &code, NULL);
	g_assert_cmpint(numerator, ==, 8875);
	g_assert_cmpint(denominator, ==, 100000);
	g_assert_cmpstr(code, ==, "NEW-YORK-CITY");

	/* A rate that is not a number is refused, and nothing is saved. */
	g_assert_cmpuint(post_form(f, "/tax-rates/new", "name=Bad&rate=eight", NULL), ==, 422);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/record-view/roles", test_roles);
	g_test_add_func("/record-view/declared-technical", test_declared_technical);
	g_test_add_func("/record-view/type-labels", test_type_labels);
	g_test_add("/record-view/submission-page", Fixture, NULL, set_up,
	           test_submission_page, tear_down);
	g_test_add("/record-view/status-and-list", Fixture, NULL, set_up,
	           test_status_and_list, tear_down);
	g_test_add("/record-view/grouped-form", Fixture, NULL, set_up,
	           test_grouped_form, tear_down);
	g_test_add("/record-view/repeating-invoice", Fixture, NULL, set_up,
	           test_repeating_invoice, tear_down);
	g_test_add("/record-view/invoice-sheet-tax", Fixture, NULL, set_up,
	           test_invoice_sheet_tax, tear_down);
	g_test_add("/record-view/invoice-sheet-exemption", Fixture, NULL, set_up,
	           test_invoice_sheet_exemption, tear_down);
	g_test_add("/record-view/attention-of-same-customer", Fixture, NULL, set_up,
	           test_attention_of_same_customer, tear_down);
	g_test_add("/record-view/errors-for-people", Fixture, NULL, set_up,
	           test_errors_for_people, tear_down);
	g_test_add("/record-view/tax-rate-page", Fixture, NULL, set_up,
	           test_tax_rate_page, tear_down);
	g_test_add("/record-view/bill-sheet", Fixture, NULL, set_up,
	           test_bill_sheet, tear_down);

	return g_test_run();
}
