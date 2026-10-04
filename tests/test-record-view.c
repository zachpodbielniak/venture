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

/*
 * An invoice's workflow state repeats its status for plugins; it is
 * machinery, not a fact a person reads twice. If this regresses, every
 * invoice page shows "Workflow state: paid" under a "Paid" badge.
 */
static void
test_workflow_state_is_machinery(void)
{
	g_autoptr(VentureEntity) invoice = g_object_new(VENTURE_TYPE_INVOICE, NULL);
	g_autoptr(GPtrArray) specs = venture_entity_get_field_specs(invoice);
	guint i, found = 0;

	for (i = 0; i < specs->len; i++)
	{
		VentureFieldSpec *spec = g_ptr_array_index(specs, i);

		if (g_strcmp0(venture_field_spec_get_name(spec), "workflow-state") != 0)
			continue;
		found++;
		g_assert_cmpint(venture_field_spec_get_role(spec), ==, VENTURE_FIELD_ROLE_TECHNICAL);
	}
	g_assert_cmpuint(found, ==, 1);
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
	port = 0;
	g_object_set(f->config, "state-dir", f->state_dir,
	             "server-bind-address", "127.0.0.1",
	             "server-port", (gint64)port,
	             "security-require-auth", FALSE, NULL);
	f->server = venture_web_server_new(f->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(f->server, &error));
	g_assert_no_error(error);
	port = venture_web_server_get_port(f->server);
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
	g_autofree gchar *list = NULL;
	g_autofree gchar *source = NULL;

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

	/* Permission is a string-backed status; the source snapshot embeds
	 * the site's UUID, not a human source name. Neither should expose
	 * internal spellings on the default page or generated list. */
	g_assert_nonnull(strstr(seen, "<dd>Not requested</dd>"));
	g_assert_null(strstr(seen, "not_requested"));
	g_object_get(submission, "first-source", &source, NULL);
	g_assert_true(g_str_has_prefix(source, "form:"));
	g_assert_null(strstr(seen, source));
	g_assert_null(strstr(seen, "First-touch source"));
	g_assert_null(strstr(seen, "Last-touch source"));
	g_assert_nonnull(strstr(page, source));
	list = get(f, "/e/attribution_submission");
	g_assert_null(strstr(list, source));
	g_assert_null(strstr(list, ">not_requested<"));

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
	                       "external-id", "ext-9f2c",
	                       "source", "https://scout:hunter2@crm.example/lead", NULL);
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

	/* A pasted link keeps its password off the list, as off the record
	 * page. If this regresses, a credential somebody pasted into a plain
	 * text field is on every list page that shows the column. */
	g_assert_nonnull(strstr(body, "crm.example"));
	g_assert_null(strstr(list, "hunter2"));

	/* An invoice is titled "Invoice INV-77" from its number; the number
	 * is not a second column saying the same thing. */
	{
		g_autoptr(VentureEntity) invoice = NULL;
		g_autofree gchar *invoices = NULL;

		invoice = g_object_new(VENTURE_TYPE_INVOICE, "number", "INV-77",
			"company-id", venture_entity_get_id(company), NULL);
		save(f, invoice);
		invoices = get(f, "/e/invoice");
		g_assert_nonnull(strstr(invoices, "Invoice INV-77"));
		g_assert_null(strstr(invoices, ">INV-77<"));
	}

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

	/* With no organization picked, the sheet is the one the post files
	 * the bill under -- the default -- in that organization's currency.
	 * If this regresses, the page totals in dollars a bill saved in euros. */
	{
		g_autoptr(VentureEntity) organization = NULL;
		g_autofree gchar *page = NULL;

		organization = venture_database_get(f->database, VENTURE_TYPE_ORGANIZATION,
			f->organization_id, &error);
		g_assert_no_error(error);
		g_object_set(organization, "default-currency", "EUR", NULL);
		g_assert_true(venture_database_save(f->database, organization, NULL, &error));
		g_assert_no_error(error);
		page = get(f, "/bills/compose");
		g_assert_nonnull(strstr(page, "<dt>Total, EUR</dt>"));
	}
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
 * The quote sheet taxes a line the way the invoice sheet does: a rate
 * picked from "No tax" and the tax rates, posted as the line's tax code,
 * and the quote's tax worked out exactly from it. If this regresses, a
 * quote is taxed at a whole percent that cannot say 8.875% and disagrees
 * with the invoice it becomes.
 */
static void
test_quote_sheet_tax(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) rate = NULL;
	g_autoptr(VentureEntity) customer = NULL;
	g_autoptr(VentureEntity) quote = NULL;
	g_autoptr(VentureMoney) tax = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *page = NULL, *body = NULL, *location = NULL;
	gint64 id;

	(void)data;

	rate = tax_rate(f, "NY", "New York sales tax", 8875, 100000);
	customer = g_object_new(VENTURE_TYPE_COMPANY, "name", "Harbour Library", NULL);
	save(f, customer);

	page = get(f, "/quotes/compose");
	g_assert_nonnull(strstr(page, "<option value=\"\" data-rate=\"0\">No tax</option>"));
	g_assert_nonnull(strstr(page, "New York sales tax \xc2\xb7 8.875%</option>"));
	g_assert_nonnull(strstr(page, "name=\"line-0-tax-code-id\""));
	g_assert_null(strstr(page, "name=\"line-0-tax-percent\""));

	body = g_strdup_printf("compose-form=1&company-id=%" G_GINT64_FORMAT
		"&line-0-description=Shelving&line-0-quantity=1&line-0-unit-price=400.00"
		"&line-0-tax-code-id=%" G_GINT64_FORMAT,
		venture_entity_get_id(customer), venture_entity_get_id(rate));
	g_assert_cmpuint(post_form(f, "/quotes/compose", body, &location), ==, 302);
	g_assert_true(g_str_has_prefix(location, "/e/quote/"));
	id = g_ascii_strtoll(location + strlen("/e/quote/"), NULL, 10);
	quote = venture_database_get(f->database, VENTURE_TYPE_QUOTE, id, &error);
	g_assert_no_error(error);
	g_object_get(quote, "tax", &tax, NULL);
	g_assert_nonnull(tax);
	/* 8.875% of 400.00 is 35.50 exactly. */
	g_assert_cmpint(venture_money_get_amount(tax), ==, 3550);
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

/* A refused invoice or schedule must not silently change the customer's tax
 * treatment. Remembering the exemption and saving the document are one write. */
static void
test_invoice_exemption_rollback(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) customer = NULL;
	g_autoptr(GError) error = NULL;
	guint i;
	const gchar *suffixes[] = { "", "&repeat=1&repeat-frequency=invalid&repeat-start=2026-01-01" };

	(void)data;
	customer = g_object_new(VENTURE_TYPE_COMPANY, "name", "Still taxable", NULL);
	save(f, customer);
	for (i = 0; i < G_N_ELEMENTS(suffixes); i++)
	{
		g_autoptr(VentureEntity) again = NULL;
		g_autofree gchar *body = g_strdup_printf("compose-form=1&company-id=%" G_GINT64_FORMAT
			"&tax-exempt=true&exempt-kind=Government&exempt-number=EX-FAIL&exempt-remember=1%s",
			venture_entity_get_id(customer), suffixes[i]);
		gboolean exempt = TRUE;

		g_assert_cmpuint(post_form(f, "/invoices/compose", body, NULL), ==, 422);
		again = venture_database_get(f->database, VENTURE_TYPE_COMPANY,
			venture_entity_get_id(customer), &error);
		g_assert_no_error(error);
		g_object_get(again, "tax-exempt", &exempt, NULL);
		g_assert_false(exempt);
		g_assert_cmpint(venture_entity_get_version(again), ==, venture_entity_get_version(customer));
	}
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

	/* Every "New invoice" goes to the composer -- the customer's related
	 * panel and the command palette as well as the list. The generated
	 * form makes an invoice with no lines, which is the thing
	 * venture_entity_class_set_create_path() exists to prevent. */
	{
		g_autofree gchar *company_page = NULL, *company_path = NULL, *palette = NULL;

		company_path = g_strdup_printf("/e/company/%" G_GINT64_FORMAT, venture_entity_get_id(amazon));
		company_page = get(f, company_path);
		g_assert_nonnull(strstr(company_page, "href=\"/invoices/compose\">New invoice"));
		g_assert_null(strstr(company_page, "/e/invoice/new"));
		palette = get(f, "/api/v1/palette?q=invoice");
		g_assert_nonnull(strstr(palette, "/invoices/compose"));
		g_assert_null(strstr(palette, "/e/invoice/new"));
	}
}

/*
 * A deal's contact, a ticket's "Raised by" and a sales order's contact are
 * somebody at that record's company, by the same flag as an invoice's
 * "Attention of": refused at the save from any writer, narrowed in the
 * form. An internal ticket with no company still takes anybody. If this
 * regresses, Tesla's founder is the contact on Amazon's deal and a
 * support reply is read by the wrong customer.
 */
static void
test_attention_of_deals_and_tickets(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) amazon = NULL, tesla = NULL, buyer = NULL, elon = NULL;
	g_autoptr(VentureEntity) deal = NULL, ticket = NULL, order = NULL, internal = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *deal_page = NULL, *ticket_page = NULL;
	GType types[3];
	guint i;

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

	types[0] = VENTURE_TYPE_DEAL;
	types[1] = VENTURE_TYPE_TICKET;
	types[2] = VENTURE_TYPE_SALES_ORDER;
	for (i = 0; i < G_N_ELEMENTS(types); i++)
		g_assert_cmpstr(venture_entity_class_get_shared_parent(
			g_type_class_peek(types[i]), "contact-id"), ==, "company-id");

	deal = g_object_new(VENTURE_TYPE_DEAL, "name", "Warehouse robots",
		"company-id", venture_entity_get_id(amazon),
		"contact-id", venture_entity_get_id(elon), NULL);
	venture_entity_set_organization_id(deal, f->organization_id);
	g_assert_false(venture_database_save(f->database, deal, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_nonnull(strstr(error->message, "Contact"));
	g_clear_error(&error);

	ticket = g_object_new(VENTURE_TYPE_TICKET, "title", "Robot stuck",
		"company-id", venture_entity_get_id(amazon),
		"contact-id", venture_entity_get_id(elon), NULL);
	venture_entity_set_organization_id(ticket, f->organization_id);
	g_assert_false(venture_database_save(f->database, ticket, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_nonnull(strstr(error->message, "Raised by"));
	g_clear_error(&error);

	order = g_object_new(VENTURE_TYPE_SALES_ORDER, "number", "SO-9",
		"status", "draft", "currency", "USD",
		"company-id", venture_entity_get_id(amazon),
		"contact-id", venture_entity_get_id(elon), NULL);
	venture_entity_set_organization_id(order, f->organization_id);
	g_assert_false(venture_database_save(f->database, order, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);

	/* Somebody at the account is accepted, on every one of them. */
	g_object_set(deal, "contact-id", venture_entity_get_id(buyer), NULL);
	save(f, deal);
	g_object_set(ticket, "contact-id", venture_entity_get_id(buyer), NULL);
	save(f, ticket);
	g_object_set(order, "contact-id", venture_entity_get_id(buyer), NULL);
	save(f, order);

	/* Somebody filed under no company is not at another one: an import
	 * matches people before anybody says where they work. */
	{
		g_autoptr(VentureEntity) loner = NULL, imported = NULL;

		loner = g_object_new(VENTURE_TYPE_CONTACT, "name", "Bob", NULL);
		save(f, loner);
		imported = g_object_new(VENTURE_TYPE_DEAL, "name", "Imported",
			"company-id", venture_entity_get_id(amazon),
			"contact-id", venture_entity_get_id(loner), NULL);
		save(f, imported);
	}

	/* Your own work names no company, so it has no parent to share. */
	internal = g_object_new(VENTURE_TYPE_TICKET, "title", "Asked on a call",
		"contact-id", venture_entity_get_id(elon), NULL);
	save(f, internal);

	deal_page = get(f, "/e/deal/new");
	ticket_page = get(f, "/e/ticket/new");
	g_assert_nonnull(strstr(deal_page,
		"<select name=\"contact-id\" data-same-parent=\"company-id\">"));
	g_assert_nonnull(strstr(ticket_page,
		"<select name=\"contact-id\" data-same-parent=\"company-id\">"));
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
	/* Announced, not just shown: the whole page is the refusal. */
	g_assert_nonnull(strstr(page, "<div class=\"empty error-page\" role=\"alert\">"));
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

/* A GET whose status is part of the answer. */
static guint
get_status(Fixture *f, const gchar *path, gchar **out_body)
{
	g_autoptr(SoupSession) session = NULL;
	g_autoptr(SoupMessage) message = NULL;
	g_autofree gchar *url = NULL;
	Reply reply;

	memset(&reply, 0, sizeof(reply));
	session = soup_session_new_with_options("timeout", 15, NULL);
	url = g_strconcat(venture_web_server_get_base_url(f->server), path, NULL);
	message = soup_message_new("GET", url);
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT,
	                                 NULL, reply_done, &reply);
	while (!reply.done)
		g_main_context_iteration(NULL, TRUE);

	g_assert_no_error(reply.error);
	*out_body = g_strndup(g_bytes_get_data(reply.bytes, NULL),
	                      g_bytes_get_size(reply.bytes));
	g_bytes_unref(reply.bytes);

	return soup_message_get_status(message);
}

/*
 * A list refuses a filter on a field that does not exist and says which
 * one -- a name taken straight from the query string. If this regresses,
 * a link to /e/company?<script>... runs its script in the operator's
 * session: a reflected XSS one click away from anybody who can send mail.
 */
static void
test_list_refusal_is_escaped(Fixture *f, gconstpointer data)
{
	g_autofree gchar *page = NULL;

	(void)data;

	g_assert_cmpuint(get_status(f, "/e/company?%3Cimg%20src%3Dx%3E=1", &page), ==, 400);
	g_assert_nonnull(strstr(page, "&lt;img"));
	g_assert_null(strstr(page, "<img src=x>"));
}

/*
 * Every button a person can reach says what it does: its own words, or an
 * aria-label when all it shows is an arrow, a bin or a cross. Checked on
 * the markup, button by button, because an icon button without a name is
 * read out as "button" and nothing else.
 */
static void
assert_buttons_named(const gchar *page, const gchar *path)
{
	const gchar *cursor = page;

	while (NULL != (cursor = strstr(cursor, "<button")))
	{
		const gchar *open_end = strchr(cursor, '>');
		const gchar *close = strstr(cursor, "</button>");
		g_autofree gchar *tag = NULL;
		gboolean words = FALSE;
		gboolean in_tag = FALSE;
		const gchar *p;

		g_assert_nonnull(open_end);
		g_assert_nonnull(close);
		tag = g_strndup(cursor, open_end - cursor);

		for (p = open_end + 1; p < close; p++)
		{
			if ('<' == *p)
				in_tag = TRUE;
			else if ('>' == *p)
				in_tag = FALSE;
			else if (!in_tag && g_ascii_isalnum(*p))
				words = TRUE;
		}

		if (!words && NULL == strstr(tag, "aria-label=\""))
			g_error("%s: a button with no name: %s>", path, tag);

		cursor = close;
	}
}

/*
 * The frame of every page works for a keyboard and a screen reader: a
 * skip link first, the sidebar a named landmark with a labelled search,
 * <main> a target the skip link can land in, the phone menu a button that
 * says what it controls, and a polite live region for toasts present from
 * the start. And no page offers a button without a name. If this
 * regresses, a keyboard user tabs through forty links on every page and a
 * screen reader announces "button" for the bin on an invoice line.
 */
static void
test_accessible_shell(Fixture *f, gconstpointer data)
{
	static const gchar *const paths[] = {
		"/tickets", "/invoices/compose", "/quotes/compose", "/bills/compose",
		"/e/ticket/new", "/e/contact", "/overview", "/market/browse", "/market/deals",
		"/market/venues", "/market/watchlists", "/market/alerts", NULL
	};
	g_autofree gchar *page = NULL;
	guint i;

	(void)data;

	page = get(f, "/e/contact");
	g_assert_true(g_str_has_prefix(strstr(page, "<body>"),
		"<body><a class=\"skip-link visually-hidden\" href=\"#main\">Skip to content</a>"));
	g_assert_nonnull(strstr(page, "<nav class=\"sidebar\" aria-label=\"Main\">"));
	g_assert_nonnull(strstr(page, "aria-label=\"Search everything\""));
	g_assert_nonnull(strstr(page, "<main class=\"main\" id=\"main\" tabindex=\"-1\">"));
	g_assert_nonnull(strstr(page, "<div class=\"toasts\" role=\"status\" aria-live=\"polite\"></div>"));
	g_assert_nonnull(strstr(page, "aria-expanded=\"true\" aria-controls=\"site-menu\""));
	g_assert_nonnull(strstr(page, "<div class=\"nav\" id=\"site-menu\">"));

	for (i = 0; NULL != paths[i]; i++)
	{
		g_autofree gchar *other = get(f, paths[i]);

		assert_buttons_named(other, paths[i]);
	}
}

/*
 * The ticket board reads without opening a ticket: each card says whose
 * it is, who has it (or that nobody does), how long it has waited and how
 * urgent it is in words; the columns are headings with human names; the
 * type filter speaks words, not enum nicks; and every card can be moved
 * from the keyboard with a named control. If this regresses, the board
 * is a wall of titles with "in_progress" over a column and no way to tell
 * a customer's fire from an internal chore.
 */
static void
test_ticket_board(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) shop = NULL, buyer = NULL, fire = NULL, chore = NULL;
	g_autofree gchar *page = NULL, *move = NULL;
	gint value = 0;

	(void)data;

	shop = g_object_new(VENTURE_TYPE_COMPANY, "name", "Bellhaven Books", NULL);
	save(f, shop);
	buyer = g_object_new(VENTURE_TYPE_CONTACT, "name", "Ruth Ellery",
		"company-id", venture_entity_get_id(shop), NULL);
	save(f, buyer);

	fire = g_object_new(VENTURE_TYPE_TICKET, "title", "Checkout is down",
		"company-id", venture_entity_get_id(shop),
		"contact-id", venture_entity_get_id(buyer), NULL);
	g_assert_true(venture_enum_from_nick(VENTURE_TYPE_TICKET_KIND, "external", &value));
	g_object_set(fire, "kind", value, NULL);
	g_assert_true(venture_enum_from_nick(VENTURE_TYPE_PRIORITY, "urgent", &value));
	g_object_set(fire, "priority", value, NULL);
	g_assert_true(venture_enum_from_nick(VENTURE_TYPE_TICKET_STATUS, "triage", &value));
	g_object_set(fire, "status", value, NULL);
	save(f, fire);

	chore = g_object_new(VENTURE_TYPE_TICKET, "title", "Rotate the keys",
		"assignee", "dave", NULL);
	g_assert_true(venture_enum_from_nick(VENTURE_TYPE_TICKET_STATUS, "in_progress", &value));
	g_object_set(chore, "status", value, NULL);
	save(f, chore);

	page = get(f, "/tickets");

	/* The head says how the queue stands. */
	g_assert_nonnull(strstr(page, "2 open &middot; 1 waiting to triage"));

	/* Controls: one labelled toolbar, the type filter in words. */
	g_assert_nonnull(strstr(page, "<nav class=\"board-toolbar\" aria-label=\"Ticket filters\">"));
	g_assert_nonnull(strstr(page, "<summary class=\"btn\">Type: Any type</summary>"));
	g_assert_nonnull(strstr(page, ">Subtask</a>"));
	g_assert_null(strstr(page, ">subtask</a>"));

	/* Columns are headings, named as a person names them. */
	g_assert_nonnull(strstr(page, "role=\"region\" aria-label=\"Ticket board\""));
	g_assert_nonnull(strstr(page, "<h2 id=\"column-in_progress\">In progress</h2>"));
	g_assert_null(strstr(page, ">in_progress<"));

	/* The customer's card: whose, unassigned, urgent in words, its age. */
	g_assert_nonnull(strstr(page, "<span class=\"ticket-customer\">Bellhaven Books</span>"));
	g_assert_nonnull(strstr(page, "<span class=\"ticket-assignee muted\">Unassigned</span>"));
	g_assert_nonnull(strstr(page, "<span class=\"ticket-priority\">Urgent</span>"));
	g_assert_nonnull(strstr(page, "Opened "));

	/* The chore: internal, and who has it. */
	g_assert_nonnull(strstr(page, "<span class=\"ticket-customer muted\">Internal</span>"));
	g_assert_nonnull(strstr(page, "<span class=\"ticket-assignee\">dave</span>"));

	/* Moving a card is a named control a keyboard reaches. */
	move = g_strdup_printf("aria-label=\"Move ticket #%" G_GINT64_FORMAT " to\"",
		venture_entity_get_id(fire));
	g_assert_nonnull(strstr(page, move));
	g_assert_nonnull(strstr(page, "<option value=\"in_progress\">In progress</option>"));
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

/* A save check that says no without saying why: the bug being guarded. */
static gboolean
refuse_silently(VentureDatabase *database, VentureEntity *entity, VentureEntity *previous,
	gpointer user_data, GError **error)
{
	(void)database;
	(void)entity;
	(void)previous;
	(void)user_data;
	(void)error;
	return FALSE;
}

/*
 * A refused save always says why. An invoice line posted through the form
 * with no invoice was refused by a check that found no invoice and set no
 * error, and the person saw a 500 reading "Unknown error". It is a 422
 * naming the field now. And a save that fails without a reason from any
 * other check becomes an internal error that says so -- with a warning in
 * the log -- rather than the same "Unknown error".
 */
static void
test_save_refusal_says_why(Fixture *f, gconstpointer data)
{
	g_autofree gchar *reply = NULL;
	g_autoptr(VentureEntity) contact = NULL;
	g_autoptr(GError) error = NULL;
	guint status;

	(void)data;
	status = post_form_full(f, "/e/invoice_line", "description=Hours&quantity=1&unit-price=12.50",
		"application/json", FALSE, NULL, &reply);
	g_assert_cmpuint(status, ==, 422);
	g_assert_nonnull(reply);
	g_assert_null(strstr(reply, "Unknown error"));
	g_assert_nonnull(strstr(reply, "invoice"));

	venture_database_add_save_validator(f->database, VENTURE_TYPE_CONTACT, refuse_silently, NULL, NULL);
	contact = g_object_new(VENTURE_TYPE_CONTACT, "organization-id", f->organization_id,
		"name", "Silent", NULL);
	g_test_expect_message("Venture", G_LOG_LEVEL_WARNING, "*failed without saying why*");
	g_assert_false(venture_database_save(f->database, contact, NULL, &error));
	g_test_assert_expected_messages();
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_FAILED);
	g_assert_nonnull(strstr(error->message, "contact"));
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/record-view/roles", test_roles);
	g_test_add_func("/record-view/declared-technical", test_declared_technical);
	g_test_add_func("/record-view/type-labels", test_type_labels);
	g_test_add_func("/record-view/workflow-state-is-machinery", test_workflow_state_is_machinery);
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
	g_test_add("/record-view/quote-sheet-tax", Fixture, NULL, set_up,
	           test_quote_sheet_tax, tear_down);
	g_test_add("/record-view/invoice-sheet-exemption", Fixture, NULL, set_up,
	           test_invoice_sheet_exemption, tear_down);
	g_test_add("/record-view/invoice-exemption-rollback", Fixture, NULL, set_up,
	           test_invoice_exemption_rollback, tear_down);
	g_test_add("/record-view/attention-of-same-customer", Fixture, NULL, set_up,
	           test_attention_of_same_customer, tear_down);
	g_test_add("/record-view/attention-of-deals-and-tickets", Fixture, NULL, set_up,
	           test_attention_of_deals_and_tickets, tear_down);
	g_test_add("/record-view/list-refusal-is-escaped", Fixture, NULL, set_up,
	           test_list_refusal_is_escaped, tear_down);
	g_test_add("/record-view/errors-for-people", Fixture, NULL, set_up,
	           test_errors_for_people, tear_down);
	g_test_add("/record-view/save-refusal-says-why", Fixture, NULL, set_up,
	           test_save_refusal_says_why, tear_down);
	g_test_add("/record-view/accessible-shell", Fixture, NULL, set_up,
	           test_accessible_shell, tear_down);
	g_test_add("/record-view/ticket-board", Fixture, NULL, set_up,
	           test_ticket_board, tear_down);
	g_test_add("/record-view/tax-rate-page", Fixture, NULL, set_up,
	           test_tax_rate_page, tear_down);
	g_test_add("/record-view/bill-sheet", Fixture, NULL, set_up,
	           test_bill_sheet, tear_down);

	return g_test_run();
}
