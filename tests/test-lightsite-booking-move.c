/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * POST /api/v1/lightsite/bookings/:id/move: Lightsite's daemon moving a
 * confirmed booking because the business owner rescheduled it. The trusted
 * service token is the authority; :id is the booking's meeting and
 * organization_id the business it must belong to.
 *
 * What breaks if these regress: an anonymous caller moves a booking; a move
 * stretches or shrinks the booking to the page's current duration; the
 * customer is not told; a booking of another business is moved; a move lands
 * on a taken slot or revives a cancelled booking; a bad body is accepted.
 */
#include <venture.h>
#include <libsoup/soup.h>
#include <string.h>
#include "venture-test-accounting.h"
#include "venture-test-util.h"

#define BUSINESS_NAME "Example Grooming"
#define ORIGIN "https://book.example.test"

typedef struct {
	VentureDatabase *db;
	VentureConfig *config;
	VentureContext *context;
	VentureWebServer *server;
	VentureBookingService *booking;
	SoupSession *session;
	gchar *directory, *token;
	gint64 business;
	VentureEntity *page;
} Fixture;
typedef struct { gboolean done; GBytes *bytes; GError *error; } Reply;

static void received(GObject *source, GAsyncResult *result, gpointer data)
{
	Reply *reply = data;
	reply->bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &reply->error);
	reply->done = TRUE;
}

static JsonNode *post(Fixture *f, const gchar *path, const gchar *body, gboolean auth, guint expected)
{
	g_autofree gchar *url = g_strconcat(venture_web_server_get_base_url(f->server), path, NULL);
	g_autofree gchar *header = auth ? g_strconcat("Bearer ", f->token, NULL) : NULL;
	g_autoptr(SoupMessage) message = soup_message_new("POST", url);
	g_autoptr(GBytes) payload = g_bytes_new(body, strlen(body));
	g_autofree gchar *terminated = NULL;
	Reply reply = { FALSE, NULL, NULL };
	JsonNode *answer;
	const gchar *text;
	gsize length;
	soup_message_headers_replace(soup_message_get_request_headers(message), "Host", "example.test");
	if (header) soup_message_headers_replace(soup_message_get_request_headers(message), "Authorization", header);
	soup_message_set_request_body_from_bytes(message, "application/json", payload);
	soup_session_send_and_read_async(f->session, message, G_PRIORITY_DEFAULT, NULL, received, &reply);
	while (!reply.done) g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(reply.error);
	text = g_bytes_get_data(reply.bytes, &length);
	if (soup_message_get_status(message) != expected) g_test_message("Response for %s: %.*s", path, (gint)length, text);
	g_assert_cmpuint(soup_message_get_status(message), ==, expected);
	terminated = g_strndup(text, length);
	answer = length ? venture_json_parse(terminated, NULL) : NULL;
	g_bytes_unref(reply.bytes);
	return answer;
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
	g_object_set(token, "name", "Move fixture", "user-id", user, "role", role, NULL);
	secret = venture_api_token_generate(token); save(f, VENTURE_ENTITY(token));
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
	f->directory = g_dir_make_tmp("venture-lightsite-move-XXXXXX", &error); g_assert_no_error(error);
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
	f->token = mint(f, venture_entity_get_id(g_ptr_array_index(users, 0)), VENTURE_USER_ROLE_EDITOR);
	f->context = venture_context_new(f->config, f->db);
	f->server = venture_web_server_new(f->context, &error); g_assert_no_error(error);
	g_assert_true(venture_web_server_start(f->server, &error)); g_assert_no_error(error);
	f->session = soup_session_new();
	business = post(f, "/api/v1/lightsite/signups",
		"{\"idempotency_key\":\"move-owner\",\"email\":\"owner@example.test\",\"issuer\":\"https://id.example.test\",\"subject\":\"move-owner\",\"business_name\":\"" BUSINESS_NAME "\",\"first_name\":\"Alex\",\"last_name\":\"Example\"}",
		TRUE, 201);
	f->business = json_object_get_int_member(json_node_get_object(business), "organization_id");
	f->booking = venture_booking_service_new(f->db);
	/* Open around the clock, so the slots are always in the future. */
	f->page = g_object_new(VENTURE_TYPE_BOOKING_PAGE, "organization-id", f->business, "title", "Groom", "slug", "move-groom",
		"owner", "operator", "duration-minutes", (gint64)30, "buffer-minutes", (gint64)0, "timezone", "UTC",
		"availability", "{\"mon\":\"00:00-24:00\",\"tue\":\"00:00-24:00\",\"wed\":\"00:00-24:00\",\"thu\":\"00:00-24:00\",\"fri\":\"00:00-24:00\",\"sat\":\"00:00-24:00\",\"sun\":\"00:00-24:00\"}",
		"horizon-days", (gint64)7, "active", TRUE, NULL);
	save(f, f->page);
}

static void teardown(Fixture *f, gconstpointer unused)
{
	(void)unused;
	if (f->server) venture_web_server_stop(f->server);
	g_clear_object(&f->page); g_clear_object(&f->booking);
	g_clear_object(&f->server); g_clear_object(&f->context); g_clear_object(&f->session);
	if (f->db) venture_test_accounting_database_cleanup(f->db);
	g_clear_object(&f->db); g_clear_object(&f->config);
	if (f->directory) venture_test_remove_tree(f->directory);
	g_free(f->directory); g_free(f->token);
}

/* The start of the index-th free slot, from now. */
static gchar *slot(Fixture *f, guint index)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) slots = venture_booking_service_slots(f->booking, f->page, NULL, &error);
	JsonArray *array;
	g_assert_no_error(error); g_assert_nonnull(slots);
	array = json_node_get_array(slots);
	g_assert_cmpuint(json_array_get_length(array), >, index);
	return g_strdup(json_object_get_string_member(json_array_get_object_element(array, index), "start"));
}

static VentureEntity *book(Fixture *f, const gchar *start, const gchar *email)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) hold = venture_booking_service_reserve(f->booking, f->page, start, 60, NULL, &error);
	VentureEntity *activity;
	g_assert_no_error(error); g_assert_nonnull(hold);
	activity = venture_booking_service_confirm(f->booking, hold, "Customer", email, NULL, ORIGIN, NULL, NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(activity);
	return activity;
}

static gchar *move_path(gint64 id)
{
	return g_strdup_printf("/api/v1/lightsite/bookings/%" G_GINT64_FORMAT "/move", id);
}

static gchar *move_body(gint64 organization, const gchar *start)
{
	return g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT ",\"starts_at\":\"%s\"}", organization, start);
}

static gint64 letters(Fixture *f, const gchar *subject)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_MAIL_MESSAGE);
	gint64 count;
	venture_query_set_organization(query, f->business);
	venture_query_add_filter_string(query, "subject", VENTURE_FILTER_OP_EQ, subject, NULL);
	count = venture_database_count(f->db, query, &error);
	g_assert_no_error(error);
	return count;
}

static void test_unauthenticated(Fixture *f, gconstpointer unused)
{
	g_autofree gchar *start = slot(f, 4), *later = slot(f, 10);
	g_autoptr(VentureEntity) booked = book(f, start, "first@example.test");
	g_autofree gchar *path = move_path(venture_entity_get_id(booked)), *body = move_body(f->business, later);
	g_autoptr(JsonNode) answer = NULL;
	g_autoptr(VentureEntity) fresh = NULL;
	g_autoptr(GDateTime) starts = NULL, expected = venture_time_from_string(start, NULL);
	(void)unused;
	answer = post(f, path, body, FALSE, 401);
	fresh = venture_database_get(f->db, VENTURE_TYPE_ACTIVITY, venture_entity_get_id(booked), NULL);
	g_object_get(fresh, "starts-at", &starts, NULL);
	g_assert_true(g_date_time_equal(starts, expected));
}

/* The page's duration changes after the booking was made; the move keeps the
 * length the customer booked, and tells them. */
static void test_moves_and_keeps_length(Fixture *f, gconstpointer unused)
{
	g_autofree gchar *start = slot(f, 4), *target = NULL;
	g_autoptr(VentureEntity) booked = book(f, start, "first@example.test");
	g_autofree gchar *path = move_path(venture_entity_get_id(booked)), *body = NULL;
	g_autoptr(JsonNode) answer = NULL;
	g_autoptr(VentureEntity) fresh = NULL;
	g_autoptr(GDateTime) original = venture_time_from_string(start, NULL), wanted = NULL, moved = NULL, ends = NULL, stored = NULL, stored_end = NULL;
	JsonObject *object;
	guint i;
	(void)unused;
	g_object_set(f->page, "duration-minutes", (gint64)60, NULL);
	save(f, f->page);
	/* A free hour-long slot at least three hours after the booking. */
	for (i = 0; target == NULL; i++) {
		g_autofree gchar *candidate = slot(f, i);
		g_autoptr(GDateTime) when = venture_time_from_string(candidate, NULL);
		if (g_date_time_difference(when, original) >= 3 * G_TIME_SPAN_HOUR) target = g_steal_pointer(&candidate);
	}
	g_assert_cmpint(letters(f, "Your booking with " BUSINESS_NAME " moved"), ==, 0);
	body = move_body(f->business, target);
	answer = post(f, path, body, TRUE, 200);
	object = json_node_get_object(answer);
	moved = venture_time_from_string(json_object_get_string_member(object, "starts_at"), NULL);
	ends = venture_time_from_string(json_object_get_string_member(object, "ends_at"), NULL);
	g_assert_nonnull(moved); g_assert_nonnull(ends);
	wanted = venture_time_from_string(target, NULL);
	g_assert_true(g_date_time_equal(moved, wanted));
	g_assert_cmpint(g_date_time_difference(ends, moved), ==, 30 * G_TIME_SPAN_MINUTE);
	fresh = venture_database_get(f->db, VENTURE_TYPE_ACTIVITY, venture_entity_get_id(booked), NULL);
	g_object_get(fresh, "starts-at", &stored, "ends-at", &stored_end, NULL);
	g_assert_true(g_date_time_equal(stored, moved));
	g_assert_true(g_date_time_equal(stored_end, ends));
	g_assert_cmpint(letters(f, "Your booking with " BUSINESS_NAME " moved"), ==, 1);
}

static void test_wrong_organization(Fixture *f, gconstpointer unused)
{
	g_autofree gchar *start = slot(f, 4), *later = slot(f, 10);
	g_autoptr(VentureEntity) booked = book(f, start, "first@example.test");
	g_autoptr(VentureEntity) other = g_object_new(VENTURE_TYPE_ORGANIZATION, "name", "Other business", "active", TRUE, NULL);
	g_autofree gchar *path = move_path(venture_entity_get_id(booked)), *missing = move_path(venture_entity_get_id(booked) + 1000);
	g_autofree gchar *wrong = NULL, *right = move_body(f->business, later);
	g_autoptr(JsonNode) a = NULL, b = NULL;
	(void)unused;
	save(f, other);
	wrong = move_body(venture_entity_get_id(other), later);
	a = post(f, path, wrong, TRUE, 404);
	b = post(f, missing, right, TRUE, 404);
	g_assert_cmpint(letters(f, "Your booking with " BUSINESS_NAME " moved"), ==, 0);
}

static void test_conflict(Fixture *f, gconstpointer unused)
{
	g_autofree gchar *start = slot(f, 4), *taken = slot(f, 10), *free_slot = slot(f, 14);
	g_autoptr(VentureEntity) booked = book(f, start, "first@example.test");
	g_autoptr(VentureEntity) second = book(f, taken, "second@example.test");
	g_autofree gchar *path = move_path(venture_entity_get_id(booked));
	g_autofree gchar *onto = move_body(f->business, taken), *elsewhere = move_body(f->business, free_slot);
	g_autoptr(JsonNode) a = NULL, b = NULL;
	g_autoptr(VentureEntity) cancelled = NULL;
	g_autoptr(GHashTable) params = g_hash_table_new(g_str_hash, g_str_equal);
	g_autoptr(GError) error = NULL;
	(void)unused;
	a = post(f, path, onto, TRUE, 409);
	cancelled = venture_action_registry_perform(venture_database_get_action_registry(f->db), "activity",
		venture_entity_get_id(booked), "cancel_booking", params, NULL, VENTURE_USER_ROLE_EDITOR, &error);
	g_assert_no_error(error); g_assert_nonnull(cancelled);
	b = post(f, path, elsewhere, TRUE, 409);
	g_assert_cmpint(letters(f, "Your booking with " BUSINESS_NAME " moved"), ==, 0);
}

static void test_invalid(Fixture *f, gconstpointer unused)
{
	g_autofree gchar *start = slot(f, 4), *later = slot(f, 10);
	g_autoptr(VentureEntity) booked = book(f, start, "first@example.test");
	g_autofree gchar *path = move_path(venture_entity_get_id(booked));
	g_autofree gchar *bad_time = move_body(f->business, "next tuesday");
	g_autofree gchar *no_start = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT "}", f->business);
	g_autofree gchar *text_org = g_strdup_printf("{\"organization_id\":\"%" G_GINT64_FORMAT "\",\"starts_at\":\"%s\"}", f->business, later);
	g_autofree gchar *zero_org = move_body(0, later);
	g_autofree gchar *extra = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT ",\"starts_at\":\"%s\",\"ends_at\":\"%s\"}", f->business, later, later);
	g_autofree gchar *good = move_body(f->business, later);
	const gchar *bodies[] = { "not json", "[]", bad_time, no_start, text_org, zero_org, extra };
	guint i;
	(void)unused;
	for (i = 0; i < G_N_ELEMENTS(bodies); i++) {
		g_autoptr(JsonNode) answer = post(f, path, bodies[i], TRUE, 422);
	}
	{
		g_autoptr(JsonNode) answer = post(f, "/api/v1/lightsite/bookings/abc/move", good, TRUE, 422);
	}
	g_assert_cmpint(letters(f, "Your booking with " BUSINESS_NAME " moved"), ==, 0);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/lightsite-booking-move/unauthenticated", Fixture, NULL, setup, test_unauthenticated, teardown);
	g_test_add("/lightsite-booking-move/moves-and-keeps-length", Fixture, NULL, setup, test_moves_and_keeps_length, teardown);
	g_test_add("/lightsite-booking-move/wrong-organization", Fixture, NULL, setup, test_wrong_organization, teardown);
	g_test_add("/lightsite-booking-move/conflict", Fixture, NULL, setup, test_conflict, teardown);
	g_test_add("/lightsite-booking-move/invalid", Fixture, NULL, setup, test_invalid, teardown);
	return g_test_run();
}
