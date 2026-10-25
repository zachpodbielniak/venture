/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * What an owner's browser downloads per click.
 *
 * The stylesheet and the two scripts are the whole interface, over four
 * hundred kilobytes together. Pasted into every page, each click carried
 * them again and the browser parsed them again: half a megabyte per quote
 * list, and most of the second an owner waited for it on a phone. Linked
 * under a name that changes with their content, they are fetched once and
 * then come from the browser's cache until the next release changes them.
 */
#include <venture.h>
#include <libsoup/soup.h>
#include <string.h>
#include "venture-test-accounting.h"
#include "venture-test-util.h"

#define WORKSPACE "5d0c3c8e-6f8e-4d59-9a59-1f3c7c1b2a10"
#define ORIGIN "https://example.test:8443"
#define AUTHORITY "example.test:8443"
#define PASSWORD "owner-test-password"

/* A page is the markup for its records and the shell around them. */
#define PAGE_BUDGET (96 * 1024)

typedef struct {
	VentureDatabase *db;
	VentureConfig *config;
	VentureContext *context;
	VentureWebServer *server;
	VentureTenantService *service;
	SoupSession *session;
	gchar *directory;
	gchar *cookie;
} Fixture;

typedef struct {
	guint status;
	gchar *body;
	gsize length;
	gchar *type;
	gchar *cache;
} Answer;

typedef struct { gboolean done; GBytes *body; GError *error; } Reply;

static void
answer_clear(Answer *answer)
{
	g_free(answer->body);
	g_free(answer->type);
	g_free(answer->cache);
	memset(answer, 0, sizeof(*answer));
}

static void
received(GObject *source, GAsyncResult *result, gpointer data)
{
	Reply *reply = data;
	reply->body = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &reply->error);
	reply->done = TRUE;
}

/* One exchange, signed in when @signed_in; the session cookie a sign-in
 * sets is kept in @out_cookie. */
static void
exchange(Fixture *f, const gchar *method, const gchar *path, const gchar *payload, gboolean signed_in,
	Answer *answer, gchar **out_cookie)
{
	g_autofree gchar *url = g_strconcat(venture_web_server_get_base_url(f->server), path, NULL);
	g_autoptr(SoupMessage) message = soup_message_new(method, url);
	SoupMessageHeaders *headers = soup_message_get_request_headers(message);
	Reply reply = { FALSE, NULL, NULL };
	gconstpointer data;
	gsize size = 0;
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	soup_message_headers_replace(headers, "Host", AUTHORITY);
	if (signed_in && f->cookie) {
		soup_message_headers_replace(headers, "Cookie", f->cookie);
		soup_message_headers_replace(headers, "Origin", ORIGIN);
	}
	if (payload) {
		g_autoptr(GBytes) body = g_bytes_new(payload, strlen(payload));
		soup_message_set_request_body_from_bytes(message, "application/x-www-form-urlencoded", body);
	}
	soup_session_send_and_read_async(f->session, message, G_PRIORITY_DEFAULT, NULL, received, &reply);
	while (!reply.done) g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(reply.error);
	if (out_cookie) {
		GSList *list = soup_cookies_from_response(message), *iter;
		*out_cookie = NULL;
		for (iter = list; iter; iter = iter->next)
			if (!strcmp(soup_cookie_get_name(iter->data), "venture_session") && *soup_cookie_get_value(iter->data)) {
				g_free(*out_cookie);
				*out_cookie = soup_cookie_to_cookie_header(iter->data);
			}
		soup_cookies_free(list);
	}
	data = g_bytes_get_data(reply.body, &size);
	answer->status = soup_message_get_status(message);
	answer->body = g_strndup(data, size);
	answer->length = size;
	answer->type = g_strdup(soup_message_headers_get_one(soup_message_get_response_headers(message), "Content-Type"));
	answer->cache = g_strdup(soup_message_headers_get_one(soup_message_get_response_headers(message), "Cache-Control"));
	g_bytes_unref(reply.body);
}

static void
setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GInetAddress) loopback_address = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
	g_autofree gchar *loopback = g_inet_address_to_string(loopback_address);
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) home = g_object_new(VENTURE_TYPE_ORGANIZATION, "name", "Provider", "active", TRUE,
		"is-default", TRUE, NULL);
	g_autoptr(VentureTenantMaintenance) maintenance = NULL;
	g_autoptr(VentureEntity) organization = NULL;
	g_autoptr(VentureEntity) user = NULL;
	g_autoptr(VentureEntity) tenant_member = NULL;
	g_autoptr(VentureEntity) org_member = NULL;
	g_autofree gchar *form = g_strdup_printf("username=owner&password=%s", PASSWORD);
	Answer answer = { 0 };
	gint64 id;
	(void)data;
	f->db = venture_test_accounting_database(&error); g_assert_no_error(error);
	f->config = venture_config_new();
	f->directory = g_dir_make_tmp("venture-page-assets-XXXXXX", &error); g_assert_no_error(error);
	f->session = soup_session_new_with_options("timeout", 30, NULL);
	g_object_set(f->config, "hosted-enabled", TRUE, "hosted-workspace-id", WORKSPACE, "hosted-origin", ORIGIN,
		"security-password-iterations", (gint64)2000, "state-dir", f->directory,
		"server-bind-address", loopback, "server-port", (gint64)0, NULL);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error)); g_assert_no_error(error);
	f->service = venture_tenant_service_get(f->db);
	g_assert_true(venture_tenant_service_configure(f->service, f->config, &error)); g_assert_no_error(error);
	g_assert_true(venture_tenant_service_initialize(f->service, &error)); g_assert_no_error(error);
	g_assert_true(venture_database_save(f->db, home, NULL, &error)); g_assert_no_error(error);
	g_assert_true(venture_tenant_service_bootstrap_admin(f->service, f->config, "provisioner", "private-test-password",
		FALSE, "Page assets fixture", &error)); g_assert_no_error(error);

	/* An owner's business, the way a hosted sign-up leaves it. */
	maintenance = venture_tenant_service_enter_maintenance(f->service, "Provision a business", &error);
	g_assert_no_error(error); g_assert_nonnull(maintenance);
	organization = g_object_new(VENTURE_TYPE_ORGANIZATION, "name", "Example Window Cleaning", "slug", "owner",
		"active", TRUE, NULL);
	g_assert_true(venture_database_save(f->db, organization, NULL, &error)); g_assert_no_error(error);
	id = venture_entity_get_id(organization);
	user = g_object_new(VENTURE_TYPE_USER, "username", "owner", "display-name", "Example Owner", "organization-id", id,
		"role", VENTURE_USER_ROLE_EDITOR, "active", TRUE, NULL);
	g_assert_true(venture_user_set_password(VENTURE_USER(user), PASSWORD, 2000, &error));
	g_assert_true(venture_database_save(f->db, user, NULL, &error)); g_assert_no_error(error);
	tenant_member = g_object_new(VENTURE_TYPE_TENANT_MEMBERSHIP, "name", "owner",
		"user-id", venture_entity_get_id(user), "role", VENTURE_TENANT_ROLE_MEMBER, "active", TRUE, NULL);
	g_assert_true(venture_database_save(f->db, tenant_member, NULL, &error)); g_assert_no_error(error);
	org_member = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP, "organization-id", id,
		"user-id", venture_entity_get_id(user), "role", VENTURE_ORGANIZATION_ROLE_OWNER, "active", TRUE, NULL);
	g_assert_true(venture_database_save(f->db, org_member, NULL, &error)); g_assert_no_error(error);
	g_assert_true(venture_tenant_maintenance_finish(maintenance, &error)); g_assert_no_error(error);

	f->context = venture_context_new(f->config, f->db);
	f->server = venture_web_server_new(f->context, &error); g_assert_no_error(error);
	g_assert_true(venture_web_server_start(f->server, &error)); g_assert_no_error(error);
	exchange(f, "POST", "/login", form, FALSE, &answer, &f->cookie);
	g_assert_cmpuint(answer.status, ==, 302);
	g_assert_nonnull(f->cookie);
	answer_clear(&answer);
}

static void
teardown(Fixture *f, gconstpointer data)
{
	(void)data;
	venture_web_server_stop(f->server);
	g_clear_object(&f->server);
	g_clear_object(&f->context);
	g_clear_object(&f->session);
	venture_test_accounting_database_cleanup(f->db);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
	venture_test_remove_tree(f->directory);
	g_free(f->directory);
	g_free(f->cookie);
}

static gchar *
asset(const gchar *name)
{
	g_autofree gchar *path = g_build_filename("data", "static", name, NULL);
	g_autoptr(GError) error = NULL;
	gchar *contents = NULL;
	g_assert_true(g_file_get_contents(path, &contents, NULL, &error));
	g_assert_no_error(error);
	return contents;
}

/* The address @page gives for @name: the value of the attribute that
 * precedes it. NULL when the page does not link it. */
static gchar *
linked(const gchar *page, const gchar *attribute, const gchar *name)
{
	g_autofree gchar *needle = g_strdup_printf("%s=\"/pub/ui/", attribute);
	const gchar *at = page;
	while ((at = strstr(at, needle)) != NULL) {
		const gchar *start = at + strlen(attribute) + 2;
		const gchar *end = strchr(start, '"');
		g_autofree gchar *href = NULL;
		g_assert_nonnull(end);
		href = g_strndup(start, end - start);
		if (g_str_has_suffix(href, name))
			return g_steal_pointer(&href);
		at = end;
	}
	return NULL;
}

/* A piece of @contents long enough to be its own, from its middle. */
static gchar *
sample(const gchar *contents)
{
	gsize length = strlen(contents);
	g_assert_cmpuint(length, >, 4096);
	return g_strndup(contents + length / 2, 160);
}

static void
check_page(Fixture *f, const gchar *path, gboolean signed_in)
{
	static const gchar *const scripts[] = { "venture-hx.js", "venture.js" };
	g_autofree gchar *css = asset("venture-industrial.css");
	g_autofree gchar *css_piece = sample(css);
	g_autofree gchar *stylesheet = NULL;
	Answer page = { 0 };
	guint i;
	exchange(f, "GET", path, NULL, signed_in, &page, NULL);
	g_test_message("%s: %" G_GSIZE_FORMAT " bytes", path, page.length);
	g_assert_cmpuint(page.status, ==, 200);
	g_assert_cmpuint(page.length, <, PAGE_BUDGET);
	g_assert_null(strstr(page.body, css_piece));
	stylesheet = linked(page.body, "href", "/venture-industrial.css");
	g_assert_nonnull(stylesheet);
	/* The sign-in page is styled but runs no script. */
	if (!signed_in) {
		answer_clear(&page);
		return;
	}
	for (i = 0; i < G_N_ELEMENTS(scripts); i++) {
		g_autofree gchar *script = asset(scripts[i]);
		g_autofree gchar *piece = sample(script);
		g_autofree gchar *name = g_strconcat("/", scripts[i], NULL);
		g_autofree gchar *src = linked(page.body, "src", name);
		g_assert_null(strstr(page.body, piece));
		g_assert_nonnull(src);
	}
	/* The stylesheet before the scripts, and hx before venture.js, which
	 * uses it. */
	g_assert_true(strstr(page.body, "/venture-industrial.css") < strstr(page.body, "/venture-hx.js"));
	g_assert_true(strstr(page.body, "/venture-hx.js") < strstr(page.body, "/venture.js"));
	answer_clear(&page);
}

/* The owner's quote, invoice, money calendar and receivables screens, and
 * the sign-in page before them, link the interface instead of carrying it. */
static void
test_pages_link(Fixture *f, gconstpointer data)
{
	static const gchar *const pages[] = {
		"/e/quote", "/e/invoice", "/money/calendar?view=month&period=2026-11", "/reports/receivables", "/",
	};
	guint i;
	(void)data;
	for (i = 0; i < G_N_ELEMENTS(pages); i++)
		check_page(f, pages[i], TRUE);
	check_page(f, "/login", FALSE);
}

/* What a page links answers anyone, byte for byte the file, typed, and
 * cacheable for good: its address changes when its content does. */
static void
test_assets_served(Fixture *f, gconstpointer data)
{
	static const struct { const gchar *name; const gchar *attribute; const gchar *type; } assets[] = {
		{ "venture-industrial.css", "href", "text/css" },
		{ "venture-hx.js", "src", "application/javascript" },
		{ "venture.js", "src", "application/javascript" },
	};
	Answer page = { 0 };
	guint i;
	(void)data;
	exchange(f, "GET", "/e/quote", NULL, TRUE, &page, NULL);
	g_assert_cmpuint(page.status, ==, 200);
	for (i = 0; i < G_N_ELEMENTS(assets); i++) {
		g_autofree gchar *name = g_strconcat("/", assets[i].name, NULL);
		g_autofree gchar *href = linked(page.body, assets[i].attribute, name);
		g_autofree gchar *contents = asset(assets[i].name);
		Answer answer = { 0 };
		g_assert_nonnull(href);
		exchange(f, "GET", href, NULL, FALSE, &answer, NULL);
		g_assert_cmpuint(answer.status, ==, 200);
		g_assert_cmpstr(answer.body, ==, contents);
		g_assert_true(g_str_has_prefix(answer.type, assets[i].type));
		g_assert_nonnull(answer.cache);
		g_assert_nonnull(strstr(answer.cache, "immutable"));
		answer_clear(&answer);
	}
	answer_clear(&page);
}

/* The classic look links its own stylesheet. */
static void
test_classic_look(Fixture *f, gconstpointer data)
{
	g_autofree gchar *contents = asset("venture-classic.css");
	g_autofree gchar *cookie = g_strconcat(f->cookie, "; venture_look=classic", NULL);
	g_autofree gchar *href = NULL;
	Answer page = { 0 };
	Answer answer = { 0 };
	(void)data;
	g_free(f->cookie);
	f->cookie = g_steal_pointer(&cookie);
	exchange(f, "GET", "/e/quote", NULL, TRUE, &page, NULL);
	g_assert_cmpuint(page.status, ==, 200);
	href = linked(page.body, "href", "/venture-classic.css");
	g_assert_nonnull(href);
	g_assert_null(linked(page.body, "href", "/venture-industrial.css"));
	exchange(f, "GET", href, NULL, FALSE, &answer, NULL);
	g_assert_cmpuint(answer.status, ==, 200);
	g_assert_cmpstr(answer.body, ==, contents);
	answer_clear(&answer);
	answer_clear(&page);
}

/* A page left open across a release still gets the interface, but not for
 * keeps; a name that is not one of them is not found. */
static void
test_old_and_unknown(Fixture *f, gconstpointer data)
{
	g_autofree gchar *contents = asset("venture.js");
	Answer answer = { 0 };
	(void)data;
	exchange(f, "GET", "/pub/ui/0000000000000000/venture.js", NULL, FALSE, &answer, NULL);
	g_assert_cmpuint(answer.status, ==, 200);
	g_assert_cmpstr(answer.body, ==, contents);
	g_assert_nonnull(answer.cache);
	g_assert_null(strstr(answer.cache, "immutable"));
	answer_clear(&answer);
	exchange(f, "GET", "/pub/ui/0000000000000000/forms.js", NULL, FALSE, &answer, NULL);
	g_assert_cmpuint(answer.status, ==, 404);
	answer_clear(&answer);
	exchange(f, "GET", "/pub/ui/0000000000000000/../../etc/passwd", NULL, FALSE, &answer, NULL);
	g_assert_cmpuint(answer.status, !=, 200);
	answer_clear(&answer);
}

/* The raw answer for @path with @encoding asked for, undecoded. */
static GBytes *
fetch_raw(Fixture *f, const gchar *path, const gchar *encoding, gchar **out_encoding)
{
	g_autofree gchar *url = g_strconcat(venture_web_server_get_base_url(f->server), path, NULL);
	g_autoptr(SoupSession) session = soup_session_new_with_options("timeout", 30, NULL);
	g_autoptr(SoupMessage) message = soup_message_new("GET", url);
	SoupMessageHeaders *headers = soup_message_get_request_headers(message);
	Reply reply = { FALSE, NULL, NULL };
	soup_session_remove_feature_by_type(session, SOUP_TYPE_CONTENT_DECODER);
	soup_message_headers_replace(headers, "Host", AUTHORITY);
	if (encoding)
		soup_message_headers_replace(headers, "Accept-Encoding", encoding);
	else
		soup_message_headers_remove(headers, "Accept-Encoding");
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL, received, &reply);
	while (!reply.done) g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(reply.error);
	g_assert_cmpuint(soup_message_get_status(message), ==, 200);
	*out_encoding = g_strdup(soup_message_headers_get_one(soup_message_get_response_headers(message), "Content-Encoding"));
	return reply.body;
}

/* A first visit downloads the interface once; gzipped it is a fraction of
 * the bytes, for a browser that says it reads gzip, and plain otherwise. */
static void
test_assets_gzipped(Fixture *f, gconstpointer data)
{
	static const gchar *const names[] = { "venture-industrial.css", "venture-hx.js", "venture.js" };
	Answer page = { 0 };
	guint i;
	(void)data;
	exchange(f, "GET", "/e/quote", NULL, TRUE, &page, NULL);
	g_assert_cmpuint(page.status, ==, 200);
	for (i = 0; i < G_N_ELEMENTS(names); i++) {
		g_autofree gchar *name = g_strconcat("/", names[i], NULL);
		g_autofree gchar *href = linked(page.body, i == 0 ? "href" : "src", name);
		g_autofree gchar *contents = asset(names[i]);
		g_autofree gchar *encoding = NULL;
		g_autofree gchar *plain_encoding = NULL;
		g_autoptr(GBytes) zipped = NULL;
		g_autoptr(GBytes) plain = NULL;
		g_autoptr(GConverter) decompressor = G_CONVERTER(g_zlib_decompressor_new(G_ZLIB_COMPRESSOR_FORMAT_GZIP));
		g_autoptr(GInputStream) raw = NULL;
		g_autoptr(GInputStream) stream = NULL;
		g_autoptr(GOutputStream) sink = g_memory_output_stream_new_resizable();
		g_autoptr(GError) error = NULL;
		g_autoptr(GBytes) unzipped = NULL;
		g_assert_nonnull(href);
		zipped = fetch_raw(f, href, "gzip, deflate, br", &encoding);
		g_assert_cmpstr(encoding, ==, "gzip");
		g_test_message("%s: %" G_GSIZE_FORMAT " bytes gzipped of %" G_GSIZE_FORMAT,
			names[i], g_bytes_get_size(zipped), strlen(contents));
		g_assert_cmpuint(g_bytes_get_size(zipped) * 3, <, strlen(contents));
		raw = g_memory_input_stream_new_from_bytes(zipped);
		stream = g_converter_input_stream_new(raw, decompressor);
		g_assert_cmpint(g_output_stream_splice(sink, stream, G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET, NULL, &error), >=, 0);
		g_assert_no_error(error);
		unzipped = g_memory_output_stream_steal_as_bytes(G_MEMORY_OUTPUT_STREAM(sink));
		g_assert_cmpuint(g_bytes_get_size(unzipped), ==, strlen(contents));
		g_assert_true(memcmp(g_bytes_get_data(unzipped, NULL), contents, strlen(contents)) == 0);
		plain = fetch_raw(f, href, NULL, &plain_encoding);
		g_assert_null(plain_encoding);
		g_assert_cmpuint(g_bytes_get_size(plain), ==, strlen(contents));
	}
	answer_clear(&page);
}

/* Quality zero means the browser refused gzip, including the written-out
 * q=0.0 that used to match the "q=0." acceptance check. */
static void
test_assets_gzip_quality(Fixture *f, gconstpointer data)
{
	Answer page = { 0 };
	g_autofree gchar *href = NULL;
	static const struct { const gchar *encoding; gboolean gzip; } cases[] = {
		{ "gzip", TRUE },
		{ "gzip;q=0.1", TRUE },
		{ "gzip;q=0.9", TRUE },
		{ "gzip;q=0", FALSE },
		{ "gzip;q=0.0", FALSE },
		{ "gzip;q=0.05", FALSE },
		{ "gzip;q=00", FALSE },
		{ "identity, gzip;q=0.0", FALSE }
	};
	guint i;
	(void)data;
	exchange(f, "GET", "/e/quote", NULL, TRUE, &page, NULL);
	g_assert_cmpuint(page.status, ==, 200);
	href = linked(page.body, "src", "/venture.js");
	g_assert_nonnull(href);
	for (i = 0; i < G_N_ELEMENTS(cases); i++)
	{
		g_autofree gchar *encoding = NULL;
		g_autoptr(GBytes) body = fetch_raw(f, href, cases[i].encoding, &encoding);
		g_assert_nonnull(body);
		if (cases[i].gzip)
			g_assert_cmpstr(encoding, ==, "gzip");
		else
			g_assert_null(encoding);
	}
	answer_clear(&page);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/page-assets/pages-link", Fixture, NULL, setup, test_pages_link, teardown);
	g_test_add("/page-assets/served", Fixture, NULL, setup, test_assets_served, teardown);
	g_test_add("/page-assets/classic-look", Fixture, NULL, setup, test_classic_look, teardown);
	g_test_add("/page-assets/gzipped", Fixture, NULL, setup, test_assets_gzipped, teardown);
	g_test_add("/page-assets/gzip-quality", Fixture, NULL, setup, test_assets_gzip_quality, teardown);
	g_test_add("/page-assets/old-and-unknown", Fixture, NULL, setup, test_old_and_unknown, teardown);
	return g_test_run();
}
