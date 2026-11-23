/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * POST /api/v1/lightsite/bookings/:id/cancel: Lightsite's daemon cancelling a
 * confirmed booking because the business owner cancelled it from the office.
 * The trusted service token is the authority; :id is the booking's meeting
 * and organization_id the business it must belong to.
 *
 * What breaks if these regress: an anonymous caller cancels a booking; a
 * booking of another business is cancelled; a cancelled booking is cancelled
 * again; Venture mails the customer too, so they get two letters (the caller
 * sends the letter); the caller cannot read back whom to write to; the
 * generic cancel action answers 500 for a booking that does not exist.
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
	g_object_set(token, "name", "Cancel fixture", "user-id", user, "role", role, NULL);
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
	f->directory = g_dir_make_tmp("venture-lightsite-cancel-XXXXXX", &error); g_assert_no_error(error);
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
		"{\"idempotency_key\":\"cancel-owner\",\"email\":\"owner@example.test\",\"issuer\":\"https://id.example.test\",\"subject\":\"cancel-owner\",\"business_name\":\"" BUSINESS_NAME "\",\"first_name\":\"Alex\",\"last_name\":\"Example\"}",
		TRUE, 201);
	f->business = json_object_get_int_member(json_node_get_object(business), "organization_id");
	f->booking = venture_booking_service_new(f->db);
	/* Open around the clock, so the slots are always in the future. */
	f->page = g_object_new(VENTURE_TYPE_BOOKING_PAGE, "organization-id", f->business, "title", "Groom", "slug", "cancel-groom",
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

static gchar *cancel_path(gint64 id)
{
	return g_strdup_printf("/api/v1/lightsite/bookings/%" G_GINT64_FORMAT "/cancel", id);
}

static gchar *cancel_body(gint64 organization)
{
	return g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT "}", organization);
}

/* Mail queued for the business; @subject NULL counts every letter. */
static gint64 letters(Fixture *f, const gchar *subject)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_MAIL_MESSAGE);
	gint64 count;
	venture_query_set_organization(query, f->business);
	if (subject) venture_query_add_filter_string(query, "subject", VENTURE_FILTER_OP_EQ, subject, NULL);
	count = venture_database_count(f->db, query, &error);
	g_assert_no_error(error);
	return count;
}

/* The reservation and meeting states of @booked, as stored. */
static void states(Fixture *f, VentureEntity *booked, gchar **state, gint *status)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_BOOKING_RESERVATION);
	g_autoptr(VentureEntity) reservation = NULL, activity = NULL;
	venture_query_set_organization(query, f->business);
	venture_query_add_filter_int(query, "activity-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(booked), NULL);
	reservation = venture_database_find_one(f->db, query, &error);
	g_assert_no_error(error); g_assert_nonnull(reservation);
	activity = venture_database_get(f->db, VENTURE_TYPE_ACTIVITY, venture_entity_get_id(booked), &error);
	g_assert_no_error(error); g_assert_nonnull(activity);
	g_object_get(reservation, "state", state, NULL);
	g_object_get(activity, "status", status, NULL);
}

static void assert_confirmed(Fixture *f, VentureEntity *booked)
{
	g_autofree gchar *state = NULL;
	gint status = 0;
	states(f, booked, &state, &status);
	g_assert_cmpstr(state, ==, "confirmed");
	g_assert_cmpint(status, ==, VENTURE_ACTIVITY_STATUS_PLANNED);
}

/* Done when 1 and 5: cancelled, answered with the booking, no mail queued. */
static void test_cancels(Fixture *f, gconstpointer unused)
{
	g_autofree gchar *start = slot(f, 4), *state = NULL;
	g_autoptr(VentureEntity) booked = book(f, start, "first@example.test");
	g_autofree gchar *path = cancel_path(venture_entity_get_id(booked)), *body = cancel_body(f->business);
	g_autoptr(JsonNode) answer = NULL;
	g_autoptr(GDateTime) wanted = venture_time_from_string(start, NULL), starts = NULL;
	JsonObject *booking, *customer;
	gint status = 0;
	gint64 queued;
	(void)unused;
	queued = letters(f, NULL);
	g_assert_cmpint(queued, ==, 1);
	answer = post(f, path, body, TRUE, 200);
	g_assert_nonnull(answer);
	booking = json_object_get_object_member(json_node_get_object(answer), "booking");
	g_assert_nonnull(booking);
	g_assert_cmpstr(json_object_get_string_member(booking, "state"), ==, "cancelled");
	g_assert_cmpint(json_object_get_int_member(booking, "reservation_id"), >, 0);
	starts = venture_time_from_string(json_object_get_string_member(booking, "starts_at"), NULL);
	g_assert_nonnull(starts);
	g_assert_true(g_date_time_equal(starts, wanted));
	g_assert_cmpstr(json_object_get_string_member(booking, "time_zone"), ==, "UTC");
	g_assert_cmpstr(json_object_get_string_member(booking, "service"), ==, "Groom");
	customer = json_object_get_object_member(booking, "customer");
	g_assert_nonnull(customer);
	g_assert_cmpstr(json_object_get_string_member(customer, "email"), ==, "first@example.test");
	states(f, booked, &state, &status);
	g_assert_cmpstr(state, ==, "cancelled");
	g_assert_cmpint(status, ==, VENTURE_ACTIVITY_STATUS_CANCELLED);
	g_assert_cmpint(letters(f, NULL), ==, queued);
}

/* Done when 2: another business's booking, or none, is 404 and unchanged. */
static void test_wrong_organization(Fixture *f, gconstpointer unused)
{
	g_autofree gchar *start = slot(f, 4);
	g_autoptr(VentureEntity) booked = book(f, start, "first@example.test");
	g_autoptr(VentureEntity) other = g_object_new(VENTURE_TYPE_ORGANIZATION, "name", "Other business", "active", TRUE, NULL);
	g_autofree gchar *path = cancel_path(venture_entity_get_id(booked)), *missing = cancel_path(venture_entity_get_id(booked) + 1000);
	g_autofree gchar *wrong = NULL, *right = cancel_body(f->business);
	g_autoptr(JsonNode) a = NULL, b = NULL;
	(void)unused;
	save(f, other);
	wrong = cancel_body(venture_entity_get_id(other));
	a = post(f, path, wrong, TRUE, 404);
	b = post(f, missing, right, TRUE, 404);
	assert_confirmed(f, booked);
	g_assert_cmpint(letters(f, NULL), ==, 1);
}

/* Done when 3: a booking that is not confirmed any more is 409. */
static void test_not_confirmed(Fixture *f, gconstpointer unused)
{
	g_autofree gchar *start = slot(f, 4);
	g_autoptr(VentureEntity) booked = book(f, start, "first@example.test");
	g_autofree gchar *path = cancel_path(venture_entity_get_id(booked)), *body = cancel_body(f->business);
	g_autoptr(JsonNode) a = NULL, b = NULL;
	(void)unused;
	a = post(f, path, body, TRUE, 200);
	b = post(f, path, body, TRUE, 409);
	g_assert_cmpint(letters(f, NULL), ==, 1);
}

/* Done when 4: no token is 401. */
static void test_unauthenticated(Fixture *f, gconstpointer unused)
{
	g_autofree gchar *start = slot(f, 4);
	g_autoptr(VentureEntity) booked = book(f, start, "first@example.test");
	g_autofree gchar *path = cancel_path(venture_entity_get_id(booked)), *body = cancel_body(f->business);
	g_autoptr(JsonNode) answer = NULL;
	(void)unused;
	answer = post(f, path, body, FALSE, 401);
	assert_confirmed(f, booked);
}

/* Done when 4: a bad body or id is 422, and nothing changes. */
static void test_invalid(Fixture *f, gconstpointer unused)
{
	g_autofree gchar *start = slot(f, 4);
	g_autoptr(VentureEntity) booked = book(f, start, "first@example.test");
	g_autofree gchar *path = cancel_path(venture_entity_get_id(booked));
	g_autofree gchar *text_org = g_strdup_printf("{\"organization_id\":\"%" G_GINT64_FORMAT "\"}", f->business);
	g_autofree gchar *zero_org = cancel_body(0);
	g_autofree gchar *extra = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT ",\"starts_at\":\"%s\"}", f->business, start);
	g_autofree gchar *good = cancel_body(f->business);
	const gchar *bodies[] = { "not json", "[]", "{}", text_org, zero_org, extra };
	guint i;
	(void)unused;
	for (i = 0; i < G_N_ELEMENTS(bodies); i++) {
		g_autoptr(JsonNode) answer = post(f, path, bodies[i], TRUE, 422);
	}
	{
		g_autoptr(JsonNode) answer = post(f, "/api/v1/lightsite/bookings/abc/cancel", good, TRUE, 422);
	}
	assert_confirmed(f, booked);
	g_assert_cmpint(letters(f, NULL), ==, 1);
}

/* Done when 6: the generic cancel action on an unknown id is 404, not 500. */
static void test_generic_action_unknown(Fixture *f, gconstpointer unused)
{
	g_autofree gchar *start = slot(f, 4);
	g_autoptr(VentureEntity) booked = book(f, start, "first@example.test");
	g_autofree gchar *path = g_strdup_printf("/api/v1/activity/%" G_GINT64_FORMAT "/actions/cancel_booking",
		venture_entity_get_id(booked) + 1000);
	g_autoptr(JsonNode) answer = NULL;
	(void)unused;
	answer = post(f, path, "{}", TRUE, 404);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/lightsite-booking-cancel/cancels", Fixture, NULL, setup, test_cancels, teardown);
	g_test_add("/lightsite-booking-cancel/wrong-organization", Fixture, NULL, setup, test_wrong_organization, teardown);
	g_test_add("/lightsite-booking-cancel/not-confirmed", Fixture, NULL, setup, test_not_confirmed, teardown);
	g_test_add("/lightsite-booking-cancel/unauthenticated", Fixture, NULL, setup, test_unauthenticated, teardown);
	g_test_add("/lightsite-booking-cancel/invalid", Fixture, NULL, setup, test_invalid, teardown);
	g_test_add("/lightsite-booking-cancel/generic-action-unknown", Fixture, NULL, setup, test_generic_action_unknown, teardown);
	return g_test_run();
}
