/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* A Lightsite business's leaving state lives on its Venture organization:
 * leave, close, return within six calendar months, or purge. Only the
 * trusted service reads or moves it, and a repeat changes nothing. */
#include <venture.h>
#include <libsoup/soup.h>
#include <string.h>
#include "venture-test-accounting.h"
#include "venture-test-util.h"

typedef struct {
	VentureDatabase *db;
	VentureConfig *config;
	VentureContext *context;
	VentureWebServer *server;
	SoupSession *session;
	gchar *directory, *token, *service;
	gint64 business, admin, owner;
} Fixture;
typedef struct { gboolean done; GBytes *bytes; GError *error; } Reply;

static void received(GObject *source, GAsyncResult *result, gpointer data)
{
	Reply *reply = data;
	reply->bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &reply->error);
	reply->done = TRUE;
}

/* Any method, no body; @token NULL sends no credential at all. */
static JsonNode *request(Fixture *f, const gchar *method, const gchar *path, const gchar *token, guint expected)
{
	g_autofree gchar *url = g_strconcat(venture_web_server_get_base_url(f->server), path, NULL);
	g_autoptr(SoupMessage) message = soup_message_new(method, url);
	Reply reply = { FALSE, NULL, NULL };
	g_autofree gchar *terminated = NULL;
	const gchar *text;
	gsize length;
	soup_message_headers_replace(soup_message_get_request_headers(message), "Host", "example.test");
	if (token) {
		g_autofree gchar *auth = g_strconcat("Bearer ", token, NULL);
		soup_message_headers_replace(soup_message_get_request_headers(message), "Authorization", auth);
	}
	soup_session_send_and_read_async(f->session, message, G_PRIORITY_DEFAULT, NULL, received, &reply);
	while (!reply.done) g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(reply.error);
	text = g_bytes_get_data(reply.bytes, &length);
	if (soup_message_get_status(message) != expected) g_test_message("Response for %s %s: %.*s", method, path, (gint)length, text);
	g_assert_cmpuint(soup_message_get_status(message), ==, expected);
	terminated = g_strndup(text, length);
	g_bytes_unref(reply.bytes);
	return venture_json_parse(terminated, NULL);
}

static JsonNode *read_state(Fixture *f, guint expected)
{
	g_autofree gchar *path = g_strdup_printf("/api/v1/lightsite/departure/%" G_GINT64_FORMAT, f->business);
	return request(f, "GET", path, f->service, expected);
}

static JsonNode *act(Fixture *f, const gchar *action, guint expected)
{
	g_autofree gchar *path = g_strdup_printf("/api/v1/lightsite/departure/%" G_GINT64_FORMAT "/%s", f->business, action);
	return request(f, "POST", path, f->service, expected);
}

static const gchar *member(JsonNode *node, const gchar *name)
{
	JsonObject *object = json_node_get_object(node);
	g_assert_true(json_object_has_member(object, name));
	return json_object_get_string_member(object, name);
}

static void assert_shape(Fixture *f, JsonNode *node, const gchar *state)
{
	JsonObject *object;
	g_assert_nonnull(node);
	g_assert_true(JSON_NODE_HOLDS_OBJECT(node));
	object = json_node_get_object(node);
	g_assert_cmpuint(json_object_get_size(object), ==, 5);
	g_assert_cmpint(json_object_get_int_member(object, "organization_id"), ==, f->business);
	g_assert_cmpstr(json_object_get_string_member(object, "state"), ==, state);
	g_assert_true(json_object_has_member(object, "left_at"));
	g_assert_true(json_object_has_member(object, "returned_at"));
	g_assert_true(json_object_has_member(object, "purged_at"));
}

static void assert_time(JsonNode *node, const gchar *name)
{
	const gchar *text = member(node, name);
	g_autoptr(GDateTime) when = NULL;
	g_assert_nonnull(text);
	when = g_date_time_new_from_iso8601(text, NULL);
	g_assert_nonnull(when);
}

static void save(Fixture *f, VentureEntity *row)
{
	g_autoptr(GError) error = NULL;
	gboolean ok = venture_database_save(f->db, row, NULL, &error);
	g_assert_no_error(error); g_assert_true(ok);
}

static gchar *mint(Fixture *f, gint64 user, VentureUserRole role)
{
	g_autoptr(VentureApiToken) token = venture_api_token_new();
	gchar *secret;
	g_object_set(token, "name", "Example token", "user-id", user, "role", role, NULL);
	secret = venture_api_token_generate(token);
	save(f, VENTURE_ENTITY(token));
	return secret;
}

static void setup(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GInetAddress) address = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
	g_autofree gchar *bind = g_inet_address_to_string(address);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_USER);
	g_autoptr(GPtrArray) users = NULL;
	g_autoptr(JsonNode) business = NULL;
	VentureTenantService *tenant;
	(void)unused;
	f->db = venture_test_accounting_database(&error); g_assert_no_error(error);
	f->config = venture_config_new();
	f->directory = g_dir_make_tmp("venture-lightsite-departure-XXXXXX", &error); g_assert_no_error(error);
	g_object_set(f->config, "hosted-enabled", TRUE, "hosted-workspace-id", "8f062b79-1d2b-4d7f-99e5-bd3bf588e05a",
		"hosted-origin", "https://example.test", "security-password-iterations", (gint64)2000,
		"state-dir", f->directory, "server-bind-address", bind, "server-port", (gint64)0, NULL);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error)); g_assert_no_error(error);
	tenant = venture_tenant_service_get(f->db);
	g_assert_true(venture_tenant_service_configure(tenant, f->config, &error)); g_assert_no_error(error);
	g_assert_true(venture_tenant_service_initialize(tenant, &error)); g_assert_no_error(error);
	g_assert_true(venture_tenant_service_bootstrap_admin(tenant, f->config, "operator", "test-password", FALSE, "Fixture", &error));
	g_assert_no_error(error);
	users = venture_database_find(f->db, query, &error); g_assert_no_error(error);
	f->admin = venture_entity_get_id(g_ptr_array_index(users, 0));
	f->service = mint(f, f->admin, VENTURE_USER_ROLE_EDITOR);
	f->context = venture_context_new(f->config, f->db);
	f->server = venture_web_server_new(f->context, &error); g_assert_no_error(error);
	g_assert_true(venture_web_server_start(f->server, &error)); g_assert_no_error(error);
	f->session = soup_session_new();
	{
		g_autofree gchar *url = g_strconcat(venture_web_server_get_base_url(f->server), "/api/v1/lightsite/signups", NULL);
		g_autofree gchar *auth = g_strconcat("Bearer ", f->service, NULL);
		static const gchar body[] = "{\"idempotency_key\":\"departure-example\",\"email\":\"owner@example.test\",\"issuer\":\"https://id.example.test\",\"subject\":\"example-owner\",\"business_name\":\"Example business\",\"first_name\":\"Alex\",\"last_name\":\"Example\"}";
		g_autoptr(SoupMessage) message = soup_message_new("POST", url);
		g_autoptr(GBytes) payload = g_bytes_new_static(body, strlen(body));
		Reply reply = { FALSE, NULL, NULL };
		const gchar *text;
		gsize length;
		g_autofree gchar *terminated = NULL;
		soup_message_headers_replace(soup_message_get_request_headers(message), "Host", "example.test");
		soup_message_headers_replace(soup_message_get_request_headers(message), "Authorization", auth);
		soup_message_set_request_body_from_bytes(message, "application/json", payload);
		soup_session_send_and_read_async(f->session, message, G_PRIORITY_DEFAULT, NULL, received, &reply);
		while (!reply.done) g_main_context_iteration(NULL, TRUE);
		g_assert_no_error(reply.error);
		g_assert_cmpuint(soup_message_get_status(message), ==, 201);
		text = g_bytes_get_data(reply.bytes, &length);
		terminated = g_strndup(text, length);
		g_bytes_unref(reply.bytes);
		business = venture_json_parse(terminated, NULL);
	}
	f->business = json_object_get_int_member(json_node_get_object(business), "organization_id");
	f->owner = json_object_get_int_member(json_node_get_object(business), "user_id");
	g_assert_cmpint(f->business, >, 0);
}

static void teardown(Fixture *f, gconstpointer unused)
{
	(void)unused;
	venture_web_server_stop(f->server);
	g_clear_object(&f->server); g_clear_object(&f->context); g_clear_object(&f->session);
	venture_test_accounting_database_cleanup(f->db); g_clear_object(&f->db); g_clear_object(&f->config);
	venture_test_remove_tree(f->directory); g_free(f->directory); g_free(f->token); g_free(f->service);
}

/* Moves left_at back, as if the business closed that long ago. */
static void backdate(Fixture *f, gint months, gint days)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) organization = venture_database_get(f->db, VENTURE_TYPE_ORGANIZATION, f->business, &error);
	g_autoptr(GDateTime) now = venture_time_now(), earlier = g_date_time_add_months(now, -months), when = g_date_time_add_days(earlier, days);
	g_assert_no_error(error); g_assert_nonnull(organization);
	g_object_set(organization, "left-at", when, NULL);
	save(f, organization);
}

/* 1. A new organization is active with every time null. */
static void test_new_active(Fixture *f, gconstpointer unused)
{
	g_autoptr(JsonNode) state = read_state(f, 200);
	g_autoptr(JsonNode) missing = request(f, "GET", "/api/v1/lightsite/departure/987654321", f->service, 404);
	g_autoptr(JsonNode) missing_act = request(f, "POST", "/api/v1/lightsite/departure/987654321/leave", f->service, 404);
	JsonObject *object;
	(void)unused;
	assert_shape(f, state, "active");
	object = json_node_get_object(state);
	g_assert_true(json_object_get_null_member(object, "left_at"));
	g_assert_true(json_object_get_null_member(object, "returned_at"));
	g_assert_true(json_object_get_null_member(object, "purged_at"));
}

/* 2. Each action makes its change and answers the new state. */
static void test_actions(Fixture *f, gconstpointer unused)
{
	g_autoptr(JsonNode) left = NULL, closed = NULL, back = NULL, read = NULL, again = NULL, reclosed = NULL, purged = NULL;
	g_autofree gchar *first_left = NULL;
	(void)unused;
	left = act(f, "leave", 200);
	assert_shape(f, left, "closing");
	g_assert_true(json_object_get_null_member(json_node_get_object(left), "left_at"));
	closed = act(f, "close", 200);
	assert_shape(f, closed, "closed");
	assert_time(closed, "left_at");
	g_assert_true(json_object_get_null_member(json_node_get_object(closed), "returned_at"));
	first_left = g_strdup(member(closed, "left_at"));
	back = act(f, "return", 200);
	assert_shape(f, back, "active");
	assert_time(back, "returned_at");
	g_assert_cmpstr(member(back, "left_at"), ==, first_left);
	read = read_state(f, 200);
	g_assert_true(json_node_equal(read, back));
	again = act(f, "leave", 200);
	assert_shape(f, again, "closing");
	reclosed = act(f, "close", 200);
	assert_shape(f, reclosed, "closed");
	assert_time(reclosed, "left_at");
	purged = act(f, "purge", 200);
	assert_shape(f, purged, "purged");
	assert_time(purged, "purged_at");
	g_assert_cmpstr(member(purged, "left_at"), ==, member(reclosed, "left_at"));
	g_assert_cmpstr(member(purged, "returned_at"), ==, member(back, "returned_at"));
}

/* 3. A repeat answers 200 and moves nothing. */
static void test_repeats(Fixture *f, gconstpointer unused)
{
	g_autoptr(JsonNode) a = NULL, b = NULL, c = NULL, d = NULL, e = NULL, g = NULL, back = NULL, again = NULL;
	(void)unused;
	a = act(f, "leave", 200);
	b = act(f, "leave", 200);
	g_assert_true(json_node_equal(a, b));
	c = act(f, "close", 200);
	g_usleep(1100 * 1000);
	d = act(f, "close", 200);
	g_assert_true(json_node_equal(c, d));
	g_assert_cmpstr(member(c, "left_at"), ==, member(d, "left_at"));
	/* A return repeated after a crash: 200, and returned_at stays. */
	back = act(f, "return", 200);
	g_usleep(1100 * 1000);
	again = act(f, "return", 200);
	g_assert_true(json_node_equal(back, again));
	g_assert_cmpstr(member(back, "returned_at"), ==, member(again, "returned_at"));
	g_assert_cmpstr(member(back, "left_at"), ==, member(again, "left_at"));
	json_node_unref(act(f, "leave", 200));
	json_node_unref(act(f, "close", 200));
	e = act(f, "purge", 200);
	g_usleep(1100 * 1000);
	g = act(f, "purge", 200);
	g_assert_true(json_node_equal(e, g));
	g_assert_cmpstr(member(e, "purged_at"), ==, member(g, "purged_at"));
}

/* 4. The wrong state is 409; a return too late or after a purge is 410. */
static void test_wrong_state(Fixture *f, gconstpointer unused)
{
	g_autoptr(JsonNode) before = NULL, after = NULL;
	(void)unused;
	before = read_state(f, 200);
	json_node_unref(act(f, "close", 409));
	/* Active and never left: nothing to come back from. */
	json_node_unref(act(f, "return", 409));
	json_node_unref(act(f, "purge", 409));
	json_node_unref(request(f, "POST", "/api/v1/lightsite/departure/1/stay", f->service, 404));
	after = read_state(f, 200);
	g_assert_true(json_node_equal(before, after));
	json_node_unref(act(f, "leave", 200));
	json_node_unref(act(f, "return", 409));
	json_node_unref(act(f, "purge", 409));
	json_node_unref(act(f, "close", 200));
	json_node_unref(act(f, "leave", 409));
	/* Six calendar months is the last day back; one day more is gone. */
	backdate(f, 6, 1);
	json_node_unref(act(f, "return", 200));
	json_node_unref(act(f, "leave", 200));
	json_node_unref(act(f, "close", 200));
	backdate(f, 6, -1);
	g_clear_pointer(&before, json_node_unref);
	before = read_state(f, 200);
	json_node_unref(act(f, "return", 410));
	g_clear_pointer(&after, json_node_unref);
	after = read_state(f, 200);
	g_assert_true(json_node_equal(before, after));
	json_node_unref(act(f, "purge", 200));
	json_node_unref(act(f, "leave", 409));
	json_node_unref(act(f, "close", 409));
	json_node_unref(act(f, "return", 410));
	g_clear_pointer(&after, json_node_unref);
	after = read_state(f, 200);
	assert_shape(f, after, "purged");
}

/* 5. No token is 401; the business owner's own token or a read-only
 * administrator token is 403; nothing changes either way. */
static void test_authority(Fixture *f, gconstpointer unused)
{
	g_autofree gchar *read = g_strdup_printf("/api/v1/lightsite/departure/%" G_GINT64_FORMAT, f->business);
	g_autofree gchar *leave = g_strdup_printf("/api/v1/lightsite/departure/%" G_GINT64_FORMAT "/leave", f->business);
	g_autofree gchar *owner = mint(f, f->owner, VENTURE_USER_ROLE_EDITOR);
	g_autofree gchar *viewer = mint(f, f->admin, VENTURE_USER_ROLE_VIEWER);
	g_autoptr(JsonNode) state = NULL;
	(void)unused;
	json_node_unref(request(f, "GET", read, NULL, 401));
	json_node_unref(request(f, "POST", leave, NULL, 401));
	json_node_unref(request(f, "GET", read, owner, 403));
	json_node_unref(request(f, "POST", leave, owner, 403));
	json_node_unref(request(f, "GET", read, viewer, 403));
	json_node_unref(request(f, "POST", leave, viewer, 403));
	state = read_state(f, 200);
	assert_shape(f, state, "active");
}

static gint64 scalar(VentureDatabase *db, const gchar *sql)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(OrmResult) result = venture_database_query_raw(db, sql, NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(result);
	g_assert_true(orm_result_next(result));
	return orm_row_get_integer(orm_result_get_row(result), 0);
}

/* 6. The migration adds the four columns, and an organization from before
 * it reads active. A new one is stored active too. */
static void test_migration(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDatabase) db = venture_test_accounting_database(&error);
	g_autoptr(VentureEntity) existing = g_object_new(VENTURE_TYPE_ORGANIZATION, "name", "Example existing", "active", TRUE, NULL);
	g_autoptr(VentureEntity) fresh = NULL;
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error)); g_assert_no_error(error);
	g_assert_true(venture_database_save(db, existing, NULL, &error)); g_assert_no_error(error);
	g_assert_cmpint(scalar(db, "SELECT COUNT(*) FROM schema_migrations WHERE version = 1078"), ==, 1);
	/* The table as it was before: no departure columns, history without 1078. */
	g_assert_true(venture_database_execute(db,
		"ALTER TABLE organizations DROP COLUMN departure_state;"
		"ALTER TABLE organizations DROP COLUMN left_at;"
		"ALTER TABLE organizations DROP COLUMN returned_at;"
		"ALTER TABLE organizations DROP COLUMN purged_at;"
		"DELETE FROM schema_migrations WHERE version >= 1078", NULL, &error));
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error)); g_assert_no_error(error);
	g_assert_cmpint(scalar(db, "SELECT COUNT(*) FROM schema_migrations WHERE version = 1078"), ==, 1);
	g_assert_cmpint(scalar(db, "SELECT COUNT(*) FROM organizations WHERE departure_state IS NULL OR departure_state <> 'active'"), ==, 0);
	g_assert_cmpint(scalar(db, "SELECT COUNT(*) FROM organizations WHERE left_at IS NULL AND returned_at IS NULL AND purged_at IS NULL"), ==,
		scalar(db, "SELECT COUNT(*) FROM organizations"));
	fresh = g_object_new(VENTURE_TYPE_ORGANIZATION, "name", "Example fresh", "active", TRUE, NULL);
	g_assert_true(venture_database_save(db, fresh, NULL, &error)); g_assert_no_error(error);
	{
		g_autofree gchar *sql = g_strdup_printf("SELECT COUNT(*) FROM organizations WHERE id = %" G_GINT64_FORMAT " AND departure_state = 'active'",
			venture_entity_get_id(fresh));
		g_assert_cmpint(scalar(db, sql), ==, 1);
	}
	venture_test_accounting_database_cleanup(db);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/lightsite-departure/new-active", Fixture, NULL, setup, test_new_active, teardown);
	g_test_add("/lightsite-departure/actions", Fixture, NULL, setup, test_actions, teardown);
	g_test_add("/lightsite-departure/repeats", Fixture, NULL, setup, test_repeats, teardown);
	g_test_add("/lightsite-departure/wrong-state", Fixture, NULL, setup, test_wrong_state, teardown);
	g_test_add("/lightsite-departure/authority", Fixture, NULL, setup, test_authority, teardown);
	g_test_add_func("/lightsite-departure/migration", test_migration);
	return g_test_run();
}
