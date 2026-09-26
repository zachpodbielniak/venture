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

	return g_test_run();
}
