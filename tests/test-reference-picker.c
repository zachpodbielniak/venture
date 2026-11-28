/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * test-reference-picker.c - Reference pickers past a <select> (VENTURE #51)
 *
 * Copyright (C) 2026 Zach Podbielniak
 *
 * A reference picker lists every candidate as a <select> while there are
 * at most VENTURE_WEB_REFERENCE_PICKER_MAX of them, and past that is a
 * lookup: a hidden id, a text box, GET /ui/records/search behind it, and
 * a name or a #id resolved on submit with scripting off
 * (docs/interface.org, "Pickers"). What this pins, and what breaks if it
 * regresses:
 *
 *   - at the bound the form's <select> is what it always was, every
 *     candidate in it; one more and there is no <select>, only the lookup,
 *     holding the chosen record as "#12 Acme Anvils";
 *   - the lookup route answers prefix matches first, at most the cap, says
 *     when there are more, takes a #id, narrows to an organization, and
 *     never offers a record the caller may not read, nor a type whose
 *     module is off;
 *   - a record form resolves the box by name, by #id and by "#id label",
 *     the box winning over the hidden id; an ambiguous or unknown name, or
 *     a record the caller may not read, is a field error naming the box's
 *     field, and nothing is saved;
 *   - a list's Filters panel does the same with ?company_id__lookup=,
 *     answering 400 for a name it cannot resolve, while ?company_id=<id>
 *     filters exactly as it did;
 *   - a record action's form resolves its reference parameters the same
 *     way before the action runs.
 *
 * Hosted workspaces are test-reference-lookup-hosted.c; fifty thousand
 * rows are test-lql-lists.c's /lql-lists/scale/reference-pickers.
 */

#include <venture.h>
#include <libsoup/soup.h>
#include <string.h>
#include "web/venture-web-picker-private.h"
#include "venture-test-util.h"

#define TEST_PASSWORD "picker-long-password"

typedef struct
{
	gchar			*directory;
	VentureConfig		*config;
	VentureDatabase		*db;
	VentureContext		*context;
	VentureWebServer	*server;
	SoupSession		*session;
	gint64			 org;
	gint64			 other_org;
	gint64			 acme;
	gint64			 bolt;
	gint64			 twin_a;
	gint64			 twin_b;
	gint64			 hidden;
	gchar			*ada;
	gchar			*ana;
} Fixture;

/* Read workers or not: a list page may be answered on one. */
typedef struct
{
	gboolean	workers;
} Variant;

static const Variant plain = { FALSE };
static const Variant with_workers = { TRUE };

/* --- The fixture ------------------------------------------------------------ */

static void
persist(
	Fixture		*f,
	VentureEntity	*entity
){
	g_autoptr(GError) error = NULL;

	if (!venture_database_save(f->db, entity, NULL, &error))
		g_error("save %s: %s", G_OBJECT_TYPE_NAME(entity), error->message);
}

/* A user with a password, a member of @org in @org_role. */
static gint64
fixture_user(
	Fixture		*f,
	const gchar	*username,
	VentureUserRole	 role,
	gint64		 org,
	gint		 org_role
){
	g_autoptr(VentureUser) user = NULL;
	g_autoptr(VentureEntity) member = NULL;
	gint64 id;

	user = venture_user_new();
	g_object_set(user, "username", username, "role", role, "active", TRUE, NULL);
	g_assert_true(venture_user_set_password(user, TEST_PASSWORD, 100000, NULL));
	persist(f, VENTURE_ENTITY(user));
	id = venture_entity_get_id(VENTURE_ENTITY(user));

	member = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP, "organization-id", org,
		"user-id", id, "active", TRUE, "role", org_role, NULL);
	persist(f, member);

	return id;
}

/* A company called @name in @org; its id. */
static gint64
fixture_company(
	Fixture		*f,
	gint64		 org,
	const gchar	*name
){
	g_autoptr(VentureEntity) company = NULL;

	company = g_object_new(VENTURE_TYPE_COMPANY, "organization-id", org, "name", name, NULL);
	persist(f, company);

	return venture_entity_get_id(company);
}

/* @rows companies "Bulk 00001" on in @org, in one statement. */
static void
fixture_bulk(
	Fixture	*f,
	gint64	 org,
	guint	 first,
	guint	 rows
){
	g_autofree gchar *sql = NULL;
	g_autoptr(GError) error = NULL;

	sql = g_strdup_printf(
		"WITH RECURSIVE n(x) AS (SELECT %u UNION ALL SELECT x + 1 FROM n WHERE x < %u) "
		"INSERT INTO companies (uuid, organization_id, created_at, updated_at, version, "
		"name, kind, customer, supplier, active) "
		"SELECT printf('bulk-%%08d', x), %" G_GINT64_FORMAT ", "
		"'2020-01-01T00:00:00.000000Z', '2020-01-01T00:00:00.000000Z', 1, "
		"printf('Bulk %%05d', x), 'customer', 1, 0, 1 FROM n",
		first, first + rows - 1, org);

	g_assert_true(venture_database_execute(f->db, sql, NULL, &error));
	g_assert_no_error(error);
}

typedef struct
{
	gboolean	 done;
	GBytes		*body;
	GError		*error;
} Outcome;

static void
request_done(
	GObject		*source,
	GAsyncResult	*result,
	gpointer	 data
){
	Outcome *outcome = data;

	outcome->body = soup_session_send_and_read_finish(SOUP_SESSION(source), result,
	                                                  &outcome->error);
	outcome->done = TRUE;
}

/*
 * One request as @cookie, driving the main context by hand; @form is a
 * urlencoded POST body. Redirects are not followed.
 */
static guint
fixture_request(
	Fixture		 *f,
	const gchar	 *method,
	const gchar	 *path,
	const gchar	 *cookie,
	const gchar	 *form,
	gchar		**out_body,
	gchar		**out_set_cookie
){
	g_autoptr(SoupMessage) message = NULL;
	g_autofree gchar *url = NULL;
	Outcome outcome = { FALSE, NULL, NULL };
	SoupMessageHeaders *headers;

	url = g_strconcat(venture_web_server_get_base_url(f->server), path, NULL);
	message = soup_message_new(method, url);
	g_assert_nonnull(message);
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	headers = soup_message_get_request_headers(message);

	if (NULL != cookie)
		soup_message_headers_append(headers, "Cookie", cookie);

	if (NULL != form)
	{
		g_autoptr(GBytes) bytes = g_bytes_new(form, strlen(form));

		soup_message_set_request_body_from_bytes(message,
			"application/x-www-form-urlencoded", bytes);
	}

	soup_session_send_and_read_async(f->session, message, G_PRIORITY_DEFAULT, NULL,
	                                 request_done, &outcome);

	while (!outcome.done)
		g_main_context_iteration(NULL, TRUE);

	if (NULL != outcome.error)
		g_error("%s %s: %s", method, path, outcome.error->message);

	if (NULL != out_body)
		*out_body = g_strndup(g_bytes_get_data(outcome.body, NULL),
		                      g_bytes_get_size(outcome.body));

	if (NULL != out_set_cookie)
		*out_set_cookie = g_strdup(soup_message_headers_get_one(
			soup_message_get_response_headers(message), "Set-Cookie"));

	g_bytes_unref(outcome.body);

	return soup_message_get_status(message);
}

/* @username's browser session: the cookie to send. */
static gchar *
fixture_login(
	Fixture		*f,
	const gchar	*username
){
	g_autofree gchar *set_cookie = NULL;
	g_autofree gchar *login = NULL;
	gchar *semicolon;

	login = g_strdup_printf("username=%s&password=%s", username, TEST_PASSWORD);
	g_assert_cmpuint(fixture_request(f, "POST", "/login", NULL, login, NULL, &set_cookie),
	                 ==, SOUP_STATUS_FOUND);
	g_assert_nonnull(set_cookie);
	semicolon = strchr(set_cookie, ';');

	if (NULL != semicolon)
		*semicolon = '\0';

	return g_steal_pointer(&set_cookie);
}

static void
fixture_setup(
	Fixture		*f,
	gconstpointer	 data
){
	const Variant *variant = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) other = NULL;

	f->directory = g_dir_make_tmp("venture-reference-picker-XXXXXX", NULL);
	f->config = venture_config_new();
	g_object_set(f->config, "state-dir", f->directory, "server-bind-address", "127.0.0.1",
	             "server-port", (gint64)0, "security-require-auth", TRUE,
	             "security-session-secret-env", "VENTURE_TEST_SESSION_SECRET", NULL);

	/* Expense claims are off: a type the lookup must not search. */
	venture_config_set_module_enabled(f->config, "claims", FALSE);

	if (variant->workers)
	{
		g_autofree gchar *uri = g_strdup_printf("sqlite://%s/venture.db", f->directory);

		g_object_set(f->config, "server-read-workers", (gint64)2, NULL);
		f->db = venture_database_new(uri, NULL);
	}
	else
		f->db = venture_database_new("sqlite://:memory:", NULL);

	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), NULL));
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);

	other = g_object_new(VENTURE_TYPE_ORGANIZATION, "name", "Other Business", "active", TRUE, NULL);
	persist(f, other);
	f->other_org = venture_entity_get_id(other);

	/* ada owns both businesses; ana edits the first and is no member of
	 * the other, whose company she must never be offered. */
	fixture_user(f, "ada", VENTURE_USER_ROLE_ADMIN, f->org, VENTURE_ORGANIZATION_ROLE_OWNER);
	{
		g_autoptr(VentureEntity) member = NULL;
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_USER);
		g_autoptr(VentureEntity) ada = NULL;

		venture_query_add_filter_string(query, "username", VENTURE_FILTER_OP_EQ, "ada", NULL);
		ada = venture_database_find_one(f->db, query, NULL);
		member = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP, "organization-id", f->other_org,
			"user-id", venture_entity_get_id(ada), "active", TRUE,
			"role", VENTURE_ORGANIZATION_ROLE_OWNER, NULL);
		persist(f, member);
	}
	fixture_user(f, "ana", VENTURE_USER_ROLE_EDITOR, f->org, VENTURE_ORGANIZATION_ROLE_EDITOR);

	f->acme = fixture_company(f, f->org, "Acme Anvils");
	f->bolt = fixture_company(f, f->org, "Bolt Brothers");
	f->twin_a = fixture_company(f, f->org, "Twin Peaks");
	f->twin_b = fixture_company(f, f->org, "Twin Peaks");
	f->hidden = fixture_company(f, f->other_org, "Hidden Holdings");

	f->server = venture_web_server_new(f->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(f->server, &error));
	g_assert_no_error(error);
	f->session = soup_session_new();
	f->ada = fixture_login(f, "ada");
	f->ana = fixture_login(f, "ana");
}

static void
fixture_teardown(
	Fixture		*f,
	gconstpointer	 data
){
	(void)data;

	if (NULL != f->server)
		venture_web_server_stop(f->server);

	g_clear_object(&f->session);
	g_clear_object(&f->server);
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
	g_clear_pointer(&f->ada, g_free);
	g_clear_pointer(&f->ana, g_free);
	venture_test_remove_tree(f->directory);
	g_clear_pointer(&f->directory, g_free);
}

/* --- Helpers ------------------------------------------------------------------ */

/* GET @path as @cookie, expecting @expect; the body. */
static gchar *
page(
	Fixture		*f,
	const gchar	*cookie,
	const gchar	*path,
	guint		 expect
){
	gchar *body = NULL;
	guint status;

	status = fixture_request(f, "GET", path, cookie, NULL, &body, NULL);

	if (status != expect)
		g_error("GET %s: %u, wanted %u\n%s", path, status, expect, body);

	return body;
}

/* POST @form to @path as @cookie; the status, and the body in @out_body. */
static guint
post(
	Fixture		 *f,
	const gchar	 *cookie,
	const gchar	 *path,
	const gchar	 *form,
	gchar		**out_body
){
	return fixture_request(f, "POST", path, cookie, form, out_body, NULL);
}

/* How often @needle occurs in @haystack, up to @end (NULL: its end). */
static guint
occurrences(
	const gchar	*haystack,
	const gchar	*end,
	const gchar	*needle
){
	const gchar *at;
	guint count = 0;

	for (at = strstr(haystack, needle); (NULL != at) && ((NULL == end) || (at < end));
	     at = strstr(at + 1, needle))
		count++;

	return count;
}

/* The <select name="@name"> in @html to its end, or NULL. */
static gchar *
select_named(
	const gchar	*html,
	const gchar	*name
){
	g_autofree gchar *needle = g_strdup_printf("<select name=\"%s\"", name);
	const gchar *start = strstr(html, needle);
	const gchar *end;

	if (NULL == start)
		return NULL;

	end = strstr(start, "</select>");
	g_assert_nonnull(end);

	return g_strndup(start, end - start);
}

/*
 * The lookup control for @name in @html, to its end, or NULL. Only a
 * control: a list's query bar carries the Filters panel's parameters
 * along as hidden inputs, the box's among them.
 */
static gchar *
lookup_named(
	const gchar	*html,
	const gchar	*name
){
	g_autofree gchar *needle = g_strdup_printf("name=\"%s" VENTURE_WEB_REFERENCE_LOOKUP_SUFFIX "\"",
	                                           name);
	const gchar *start;

	for (start = strstr(html, "<span class=\"record-pick reference-lookup\""); NULL != start;
	     start = strstr(start + 1, "<span class=\"record-pick reference-lookup\""))
	{
		const gchar *end = strstr(start, "</span>");
		g_autofree gchar *control = NULL;

		g_assert_nonnull(end);
		control = g_strndup(start, end - start);

		if (NULL != strstr(control, needle))
			return g_steal_pointer(&control);
	}

	return NULL;
}

/* GET /ui/records/search?@query as @cookie; the parsed answer. */
static JsonObject *
search(
	Fixture		*f,
	const gchar	*cookie,
	const gchar	*query,
	guint		 expect
){
	g_autofree gchar *path = g_strconcat("/ui/records/search?", query, NULL);
	g_autofree gchar *body = page(f, cookie, path, expect);
	g_autoptr(JsonNode) node = venture_json_parse(body, NULL);

	g_assert_nonnull(node);

	return json_object_ref(json_node_get_object(node));
}

/* The labels of a search answer's items, joined by "|". */
static gchar *
labels(JsonObject *answer)
{
	JsonArray *items = json_object_get_array_member(answer, "items");
	g_autoptr(GString) joined = g_string_new(NULL);
	guint i;

	for (i = 0; i < json_array_get_length(items); i++)
	{
		if (0 != i)
			g_string_append_c(joined, '|');
		g_string_append(joined, json_object_get_string_member(
			json_array_get_object_element(items, i), "label"));
	}

	return g_string_free(g_steal_pointer(&joined), FALSE);
}

/* The newest contact's company. */
static gint64
newest_contact_company(Fixture *f)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_CONTACT);
	g_autoptr(VentureEntity) contact = NULL;
	gint64 company = 0;

	venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, NULL);
	contact = venture_database_find_one(f->db, query, NULL);
	g_assert_nonnull(contact);
	g_object_get(contact, "company-id", &company, NULL);

	return company;
}

/* How many contacts there are. */
static gint64
contacts(Fixture *f)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_CONTACT);

	return venture_database_count(f->db, query, NULL);
}

/* --- Below and past the bound ----------------------------------------------- */

/*
 * At the bound the form's company picker is the <select> it always was,
 * with every candidate in it; one more company and it is the lookup, with
 * no option at all. Breaks if the bound moves, if the select changes
 * shape below it, or if anything past it still lists the table.
 */
static void
test_bound(
	Fixture		*f,
	gconstpointer	 data
){
	g_autofree gchar *html = NULL;
	g_autofree gchar *picker = NULL;
	g_autofree gchar *lookup = NULL;
	g_autofree gchar *acme = NULL;
	guint seeded;

	(void)data;

	/* Five companies ada can read: today's select, unchanged. */
	html = page(f, f->ada, "/e/contact/new", 200);
	picker = select_named(html, "company-id");
	g_assert_nonnull(picker);
	g_assert_nonnull(strstr(picker, "data-picker-search"));
	g_assert_nonnull(strstr(picker, "<option value=\"\">"));
	acme = g_strdup_printf("<option value=\"%" G_GINT64_FORMAT "\">Acme Anvils</option>", f->acme);
	g_assert_nonnull(strstr(picker, acme));
	g_assert_nonnull(strstr(picker, ">Hidden Holdings</option>"));
	g_assert_null(lookup_named(html, "company-id"));
	g_assert_cmpuint(occurrences(picker, NULL, "<option value=\""), ==, 5 + 1);

	/* ana reads one business: four, and never the other's. */
	g_clear_pointer(&html, g_free);
	g_clear_pointer(&picker, g_free);
	html = page(f, f->ana, "/e/contact/new", 200);
	picker = select_named(html, "company-id");
	g_assert_nonnull(picker);
	g_assert_null(strstr(picker, "Hidden Holdings"));
	g_assert_cmpuint(occurrences(picker, NULL, "<option value=\""), ==, 4 + 1);

	/* Exactly the bound: still every one of them, as a select. */
	seeded = VENTURE_WEB_REFERENCE_PICKER_MAX - 5;
	fixture_bulk(f, f->org, 1, seeded);
	g_clear_pointer(&html, g_free);
	g_clear_pointer(&picker, g_free);
	html = page(f, f->ada, "/e/contact/new", 200);
	picker = select_named(html, "company-id");
	g_assert_nonnull(picker);
	g_assert_cmpuint(occurrences(picker, NULL, "<option value=\""), ==,
	                 VENTURE_WEB_REFERENCE_PICKER_MAX + 1);

	/* One past it: the lookup, empty, and no option for any company. */
	fixture_bulk(f, f->org, seeded + 1, 1);
	g_clear_pointer(&html, g_free);
	g_clear_pointer(&picker, g_free);
	html = page(f, f->ada, "/e/contact/new", 200);
	g_assert_null(select_named(html, "company-id"));
	lookup = lookup_named(html, "company-id");
	g_assert_nonnull(lookup);
	g_assert_nonnull(strstr(lookup, "data-record-pick data-type=\"company\""));
	g_assert_nonnull(strstr(lookup, "<input type=\"hidden\" name=\"company-id\" value=\"\">"));
	g_assert_nonnull(strstr(lookup, "data-record-lookup"));
	g_assert_null(strstr(html, ">Bulk 00001</option>"));
	g_assert_null(strstr(html, ">Acme Anvils</option>"));

	/* The bound counts what the caller reads: ana is no member of the
	 * other business, so she reads exactly the bound, and keeps her
	 * select -- without the company she may not see. */
	g_clear_pointer(&html, g_free);
	g_clear_pointer(&picker, g_free);
	html = page(f, f->ana, "/e/contact/new", 200);
	picker = select_named(html, "company-id");
	g_assert_nonnull(picker);
	g_assert_cmpuint(occurrences(picker, NULL, "<option value=\""), ==,
	                 VENTURE_WEB_REFERENCE_PICKER_MAX + 1);
	g_assert_null(strstr(picker, "Hidden Holdings"));
	g_assert_null(lookup_named(html, "company-id"));
}

/*
 * An edit form past the bound shows the record's company by name in the
 * box, its id in the hidden input; and the Filters panel of a list with a
 * company filter is the lookup too, carrying the active organization.
 */
static void
test_bound_current_value(
	Fixture		*f,
	gconstpointer	 data
){
	g_autoptr(VentureEntity) contact = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *html = NULL;
	g_autofree gchar *lookup = NULL;
	g_autofree gchar *hidden = NULL;
	g_autofree gchar *shown = NULL;
	g_autofree gchar *organization = NULL;

	(void)data;

	fixture_bulk(f, f->org, 1, VENTURE_WEB_REFERENCE_PICKER_MAX);
	contact = g_object_new(VENTURE_TYPE_CONTACT, "organization-id", f->org, "name", "Pat Smith",
	                       "company-id", f->acme, NULL);
	persist(f, contact);

	path = g_strdup_printf("/e/contact/%" G_GINT64_FORMAT "/edit", venture_entity_get_id(contact));
	html = page(f, f->ada, path, 200);
	lookup = lookup_named(html, "company-id");
	g_assert_nonnull(lookup);
	hidden = g_strdup_printf("name=\"company-id\" value=\"%" G_GINT64_FORMAT "\"", f->acme);
	shown = g_strdup_printf("value=\"#%" G_GINT64_FORMAT " Acme Anvils\"", f->acme);
	g_assert_nonnull(strstr(lookup, hidden));
	g_assert_nonnull(strstr(lookup, shown));

	/* The contact list's Filters panel: the lookup, in this business. */
	g_clear_pointer(&html, g_free);
	g_clear_pointer(&lookup, g_free);
	html = page(f, f->ada, "/e/contact", 200);
	g_assert_nonnull(strstr(html, "<summary>Filters</summary>"));
	lookup = lookup_named(html, "company-id");
	g_assert_nonnull(lookup);
	organization = g_strdup_printf("data-organization=\"%" G_GINT64_FORMAT "\"", f->org);
	g_assert_nonnull(strstr(lookup, organization));
	g_assert_null(strstr(html, ">Bulk 00001</option>"));
}

/* --- The lookup route ------------------------------------------------------- */

/*
 * GET /ui/records/search: names that start with the text first, in name
 * order, at most the cap, "more" when there were others; a #id; one
 * organization when asked; nothing ana may not read; and no type whose
 * module is off.
 */
static void
test_lookup_route(
	Fixture		*f,
	gconstpointer	 data
){
	g_autoptr(JsonObject) answer = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *query = NULL;
	JsonArray *items;

	(void)data;

	fixture_bulk(f, f->org, 1, 300);

	/* A prefix shared by three hundred: the first twelve, and more. */
	answer = search(f, f->ada, "type=company&q=bulk", 200);
	items = json_object_get_array_member(answer, "items");
	g_assert_cmpuint(json_array_get_length(items), ==, VENTURE_WEB_RECORD_SEARCH_MAX);
	g_assert_true(json_object_get_boolean_member(answer, "more"));
	text = labels(answer);
	g_assert_true(g_str_has_prefix(text, "Bulk 00001|Bulk 00002|Bulk 00003|"));

	/* Prefix before contains: "Peak" is inside two names, at the start
	 * of none, and both are found by the search all the same. */
	g_clear_pointer(&answer, json_object_unref);
	g_clear_pointer(&text, g_free);
	answer = search(f, f->ada, "type=company&q=Peak", 200);
	text = labels(answer);
	g_assert_cmpstr(text, ==, "Twin Peaks|Twin Peaks");
	g_assert_false(json_object_get_boolean_member(answer, "more"));

	g_clear_pointer(&answer, json_object_unref);
	g_clear_pointer(&text, g_free);
	answer = search(f, f->ada, "type=company&q=Acme", 200);
	text = labels(answer);
	g_assert_cmpstr(text, ==, "Acme Anvils");

	/* #id, and the label the box shows after it. */
	g_clear_pointer(&answer, json_object_unref);
	g_clear_pointer(&text, g_free);
	query = g_strdup_printf("type=company&q=%%23%" G_GINT64_FORMAT "+Bolt+Brothers", f->bolt);
	answer = search(f, f->ada, query, 200);
	text = labels(answer);
	g_assert_cmpstr(text, ==, "Bolt Brothers");

	/* One organization: the other business's company only there. */
	g_clear_pointer(&answer, json_object_unref);
	g_clear_pointer(&text, g_free);
	g_clear_pointer(&query, g_free);
	query = g_strdup_printf("type=company&q=Hidden&organization_id=%" G_GINT64_FORMAT, f->other_org);
	answer = search(f, f->ada, query, 200);
	text = labels(answer);
	g_assert_cmpstr(text, ==, "Hidden Holdings");
	g_clear_pointer(&answer, json_object_unref);
	g_clear_pointer(&text, g_free);
	g_clear_pointer(&query, g_free);
	query = g_strdup_printf("type=company&q=Hidden&organization_id=%" G_GINT64_FORMAT, f->org);
	answer = search(f, f->ada, query, 200);
	text = labels(answer);
	g_assert_cmpstr(text, ==, "");

	/* ana is no member of the other business: not by name, not by id,
	 * not by naming its organization. */
	g_clear_pointer(&answer, json_object_unref);
	g_clear_pointer(&text, g_free);
	answer = search(f, f->ana, "type=company&q=Hidden", 200);
	text = labels(answer);
	g_assert_cmpstr(text, ==, "");
	g_clear_pointer(&answer, json_object_unref);
	g_clear_pointer(&text, g_free);
	g_clear_pointer(&query, g_free);
	query = g_strdup_printf("type=company&q=%%23%" G_GINT64_FORMAT, f->hidden);
	answer = search(f, f->ana, query, 200);
	text = labels(answer);
	g_assert_cmpstr(text, ==, "");
	g_clear_pointer(&answer, json_object_unref);
	g_clear_pointer(&text, g_free);
	g_clear_pointer(&query, g_free);
	query = g_strdup_printf("type=company&q=Hidden&organization_id=%" G_GINT64_FORMAT, f->other_org);
	answer = search(f, f->ana, query, 200);
	text = labels(answer);
	g_assert_cmpstr(text, ==, "");

	/* A type whose module is off is no type at all. */
	g_clear_pointer(&answer, json_object_unref);
	answer = search(f, f->ada, "type=expense_claim&q=a", 404);
}

/* --- With scripting off: a record form ------------------------------------- */

/*
 * POST /e/contact with the box filled in, as a browser with scripting off
 * sends it: by name, by #id, by the "#id label" the box shows; the box
 * wins over the hidden id; blank clears. Ambiguous, unknown and unreadable
 * names are field errors and save nothing.
 */
static void
test_no_js_form(
	Fixture		*f,
	gconstpointer	 data
){
	g_autofree gchar *form = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *twins = NULL;
	gint64 before;

	(void)data;

	g_assert_cmpuint(post(f, f->ada, "/e/contact",
		"name=By+Name&company-id=&company-id__lookup=acme+anvils", NULL), ==, SOUP_STATUS_FOUND);
	g_assert_cmpint(newest_contact_company(f), ==, f->acme);

	form = g_strdup_printf("name=By+Id&company-id=&company-id__lookup=%%23%" G_GINT64_FORMAT, f->bolt);
	g_assert_cmpuint(post(f, f->ada, "/e/contact", form, NULL), ==, SOUP_STATUS_FOUND);
	g_assert_cmpint(newest_contact_company(f), ==, f->bolt);

	/* Left as drawn: "#id label" is that id, however many share the name. */
	g_clear_pointer(&form, g_free);
	form = g_strdup_printf("name=As+Drawn&company-id=%" G_GINT64_FORMAT
	                       "&company-id__lookup=%%23%" G_GINT64_FORMAT "+Twin+Peaks",
	                       f->twin_b, f->twin_b);
	g_assert_cmpuint(post(f, f->ada, "/e/contact", form, NULL), ==, SOUP_STATUS_FOUND);
	g_assert_cmpint(newest_contact_company(f), ==, f->twin_b);

	/* Retyped: the box decides, not the stale hidden id. */
	g_clear_pointer(&form, g_free);
	form = g_strdup_printf("name=Retyped&company-id=%" G_GINT64_FORMAT
	                       "&company-id__lookup=Bolt+Brothers", f->acme);
	g_assert_cmpuint(post(f, f->ada, "/e/contact", form, NULL), ==, SOUP_STATUS_FOUND);
	g_assert_cmpint(newest_contact_company(f), ==, f->bolt);

	/* Part of a name that only one company has. */
	g_assert_cmpuint(post(f, f->ada, "/e/contact",
		"name=Partly&company-id__lookup=Brother", NULL), ==, SOUP_STATUS_FOUND);
	g_assert_cmpint(newest_contact_company(f), ==, f->bolt);

	/* Cleared. */
	g_clear_pointer(&form, g_free);
	form = g_strdup_printf("name=Cleared&company-id=%" G_GINT64_FORMAT "&company-id__lookup=+", f->acme);
	g_assert_cmpuint(post(f, f->ada, "/e/contact", form, NULL), ==, SOUP_STATUS_FOUND);
	g_assert_cmpint(newest_contact_company(f), ==, 0);

	/* Two companies are called that: refused, both named by id. */
	before = contacts(f);
	g_assert_cmpuint(post(f, f->ada, "/e/contact",
		"name=Ambiguous&company-id__lookup=Twin+Peaks", &body), ==, 422);
	g_assert_nonnull(strstr(body, "company-id"));
	g_assert_nonnull(strstr(body, "matches more than one company"));
	twins = g_strdup_printf("#%" G_GINT64_FORMAT " Twin Peaks", f->twin_a);
	g_assert_nonnull(strstr(body, twins));
	g_clear_pointer(&twins, g_free);
	twins = g_strdup_printf("#%" G_GINT64_FORMAT " Twin Peaks", f->twin_b);
	g_assert_nonnull(strstr(body, twins));
	g_assert_cmpint(contacts(f), ==, before);

	/* Nobody is called that. */
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(post(f, f->ada, "/e/contact",
		"name=Unknown&company-id__lookup=Nowhere+Ltd", &body), ==, 422);
	g_assert_nonnull(strstr(body, "no company matches"));
	g_assert_nonnull(strstr(body, "Nowhere Ltd"));
	g_assert_cmpint(contacts(f), ==, before);

	/* An id nobody has. */
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(post(f, f->ada, "/e/contact",
		"name=NoId&company-id__lookup=%23999999", &body), ==, 422);
	g_assert_nonnull(strstr(body, "there is no company #999999"));
	g_assert_cmpint(contacts(f), ==, before);

	/* ana may not read the other business's company: by name or by id,
	 * it is no company at all, exactly as one that does not exist. */
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(post(f, f->ana, "/e/contact",
		"name=Sneaky&company-id__lookup=Hidden+Holdings", &body), ==, 422);
	g_assert_nonnull(strstr(body, "no company matches"));
	g_clear_pointer(&body, g_free);
	g_clear_pointer(&form, g_free);
	form = g_strdup_printf("name=Sneaky&company-id__lookup=%%23%" G_GINT64_FORMAT, f->hidden);
	g_assert_cmpuint(post(f, f->ana, "/e/contact", form, &body), ==, 422);
	g_assert_nonnull(strstr(body, "there is no company #"));
	g_assert_cmpint(contacts(f), ==, before);
}

/* --- With scripting off: a list's Filters panel ---------------------------- */

/*
 * GET /e/contact?list_filters=1&company-id__lookup=NAME filters by the
 * company it names and shows it in the box; ?company_id=<id> filters as
 * it did; a name the panel cannot resolve is a 400; a blank box is "any".
 */
static void
test_no_js_list(
	Fixture		*f,
	gconstpointer	 data
){
	g_autofree gchar *html = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *shown = NULL;
	g_autofree gchar *lookup = NULL;
	guint i;

	(void)data;

	for (i = 0; i < 2; i++)
	{
		g_autoptr(VentureEntity) contact = NULL;

		contact = g_object_new(VENTURE_TYPE_CONTACT, "organization-id", f->org,
			"name", (0 == i) ? "Anvil Person" : "Bolt Person",
			"company-id", (0 == i) ? f->acme : f->bolt, NULL);
		persist(f, contact);
	}

	/* Below the bound, a link by id: unchanged. */
	path = g_strdup_printf("/e/contact?company_id=%" G_GINT64_FORMAT, f->acme);
	html = page(f, f->ada, path, 200);
	g_assert_nonnull(strstr(html, ">Anvil Person</a></td>"));
	g_assert_null(strstr(html, ">Bolt Person</a></td>"));

	/* Past it, the panel's box as a browser without scripting sends it. */
	fixture_bulk(f, f->org, 1, VENTURE_WEB_REFERENCE_PICKER_MAX);
	g_clear_pointer(&html, g_free);
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/e/contact?list_filters=1&company-id=%" G_GINT64_FORMAT
	                       "&company-id__lookup=bolt+brothers", f->acme);
	html = page(f, f->ada, path, 200);
	g_assert_nonnull(strstr(html, ">Bolt Person</a></td>"));
	g_assert_null(strstr(html, ">Anvil Person</a></td>"));
	lookup = lookup_named(html, "company-id");
	g_assert_nonnull(lookup);
	shown = g_strdup_printf("value=\"#%" G_GINT64_FORMAT " Bolt Brothers\"", f->bolt);
	g_assert_nonnull(strstr(lookup, shown));

	/* The id link still filters past the bound too. */
	g_clear_pointer(&html, g_free);
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/e/contact?company_id=%" G_GINT64_FORMAT, f->acme);
	html = page(f, f->ada, path, 200);
	g_assert_nonnull(strstr(html, ">Anvil Person</a></td>"));
	g_assert_null(strstr(html, ">Bolt Person</a></td>"));

	/* A blank box is "any", whatever the hidden id said. */
	g_clear_pointer(&html, g_free);
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/e/contact?list_filters=1&company-id=%" G_GINT64_FORMAT
	                       "&company-id__lookup=", f->acme);
	html = page(f, f->ada, path, 200);
	g_assert_nonnull(strstr(html, ">Anvil Person</a></td>"));
	g_assert_nonnull(strstr(html, ">Bolt Person</a></td>"));

	/* Ambiguous and unknown: 400, saying which. */
	g_clear_pointer(&html, g_free);
	html = page(f, f->ada, "/e/contact?list_filters=1&company-id__lookup=Twin+Peaks", 400);
	g_assert_nonnull(strstr(html, "matches more than one company"));
	g_clear_pointer(&html, g_free);
	html = page(f, f->ada, "/e/contact?list_filters=1&company-id__lookup=Nowhere", 400);
	g_assert_nonnull(strstr(html, "no company matches"));

	/* Only a reference has a lookup. */
	g_clear_pointer(&html, g_free);
	html = page(f, f->ada, "/e/contact?name__lookup=x", 400);
}

/* --- With scripting off: a record action's form ----------------------------- */

/*
 * A record action's form posts its reference parameters the same way, to
 * /api/v1/TYPE/ID/actions/NAME: the box is resolved before the action is
 * looked at, and refused by name, rather than reaching the action as a
 * parameter it never declared. Adopting a legacy commerce invoice takes an
 * invoice reference.
 */
static void
test_no_js_action(
	Fixture		*f,
	gconstpointer	 data
){
	g_autofree gchar *body = NULL;

	(void)data;

	g_assert_cmpuint(post(f, f->ada, "/api/v1/integration_connection/1/actions/adopt_commerce_invoice",
		"invoice_id__lookup=Nowhere&reason=Adopt", &body), ==, 422);
	g_assert_nonnull(strstr(body, "invoice_id: no invoice matches"));

	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(post(f, f->ada, "/api/v1/integration_connection/1/actions/adopt_commerce_invoice",
		"invoice_id=&invoice_id__lookup=%23999999&reason=Adopt", &body), ==, 422);
	g_assert_nonnull(strstr(body, "invoice_id: there is no invoice #999999"));
}

int
main(
	int	  argc,
	char	**argv
){
	g_test_init(&argc, &argv, NULL);
	g_setenv("VENTURE_TEST_SESSION_SECRET", "reference-picker-test-secret", TRUE);

#define ADD(path, func) \
	g_test_add(path, Fixture, &plain, fixture_setup, func, fixture_teardown); \
	g_test_add(path "/read-workers", Fixture, &with_workers, fixture_setup, func, fixture_teardown)

	ADD("/reference-picker/bound/select-then-lookup", test_bound);
	ADD("/reference-picker/bound/current-value", test_bound_current_value);
	ADD("/reference-picker/lookup/route", test_lookup_route);
	ADD("/reference-picker/no-js/record-form", test_no_js_form);
	ADD("/reference-picker/no-js/list-filters", test_no_js_list);
	g_test_add("/reference-picker/no-js/action-form", Fixture, &plain, fixture_setup,
	           test_no_js_action, fixture_teardown);

#undef ADD

	return g_test_run();
}
