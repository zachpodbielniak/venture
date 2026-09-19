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
	VentureEntity *contact;
	VentureEntity *sequence;
	VentureEntity *step;
	VentureEntity *later;
} Fixture;

static void
save(Fixture *f, VentureEntity *row)
{
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_database_save(f->db, row, NULL, &error));
	g_assert_no_error(error);
}

static void
setup(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	(void)unused;
	f->config = venture_config_new();
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->contact = g_object_new(VENTURE_TYPE_CONTACT, "organization-id", (gint64)1,
		"name", "Ada", "email", "ada@example.com", NULL);
	save(f, f->contact);
	f->sequence = g_object_new(VENTURE_TYPE_SEQUENCE, "organization-id", (gint64)1,
		"name", "Welcome", "active", TRUE, "timezone", "UTC",
		"send-window-start", (gint64)0, "send-window-end", (gint64)24,
		"weekdays", "1,2,3,4,5,6,7", "exit-on-reply", TRUE, "tracking", TRUE, NULL);
	save(f, f->sequence);
	f->step = g_object_new(VENTURE_TYPE_SEQUENCE_STEP, "organization-id", (gint64)1,
		"sequence-id", venture_entity_get_id(f->sequence), "position", (gint64)10, "active", TRUE,
		"subject", "Hello {contact.name}",
		"body", "See https://example.com/a and http://example.org/b?x=1.\nBye", NULL);
	save(f, f->step);
	f->later = g_object_new(VENTURE_TYPE_SEQUENCE_STEP, "organization-id", (gint64)1,
		"sequence-id", venture_entity_get_id(f->sequence), "position", (gint64)20, "active", TRUE,
		"delay-days", (gint64)3, "subject", "Still there?", "body", "No links here", NULL);
	save(f, f->later);
}

static void
teardown(Fixture *f, gconstpointer unused)
{
	(void)unused;
	g_clear_object(&f->later);
	g_clear_object(&f->step);
	g_clear_object(&f->sequence);
	g_clear_object(&f->contact);
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
}

static void
consent(Fixture *f, gboolean allowed)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) organization = venture_database_get(f->db, VENTURE_TYPE_ORGANIZATION,
		venture_context_get_default_organization_id(f->context), &error);
	g_assert_no_error(error);
	g_assert_nonnull(organization);
	g_object_set(organization, "sequence-tracking", allowed, NULL);
	save(f, organization);
}

static VentureEntity *
enroll(Fixture *f)
{
	g_autoptr(GDateTime) at = g_date_time_new_from_iso8601("2026-09-14T10:00:00Z", NULL);
	VentureEntity *row = g_object_new(VENTURE_TYPE_SEQUENCE_ENROLLMENT, "organization-id", (gint64)1,
		"sequence-id", venture_entity_get_id(f->sequence),
		"contact-id", venture_entity_get_id(f->contact), "enrolled-at", at, NULL);
	save(f, row);
	return row;
}

static void
run(Fixture *f, const gchar *iso, gint expected)
{
	g_autoptr(GDateTime) as_of = g_date_time_new_from_iso8601(iso, NULL);
	g_autoptr(GError) error = NULL;
	gint count = venture_sequence_service_run_due(venture_sequence_service_get(f->db), 1, as_of, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(count, ==, expected);
}

static GPtrArray *
rows(Fixture *f, GType type)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	g_autoptr(GError) error = NULL;
	GPtrArray *found;
	venture_query_set_organization(query, 1);
	venture_query_set_limit(query, 0);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	found = venture_database_find(f->db, query, &error);
	g_assert_no_error(error);
	return found;
}

/* Enroll, run the first step, and return the tracked delivery. */
static VentureEntity *
tracked_delivery(Fixture *f)
{
	g_autoptr(VentureEntity) enrollment = NULL;
	g_autoptr(GPtrArray) deliveries = NULL;
	consent(f, TRUE);
	enrollment = enroll(f);
	run(f, "2026-09-14T11:00:00Z", 1);
	deliveries = rows(f, VENTURE_TYPE_SEQUENCE_DELIVERY);
	g_assert_cmpuint(deliveries->len, ==, 1);
	return g_object_ref(g_ptr_array_index(deliveries, 0));
}

static gchar *
token_of(VentureEntity *delivery)
{
	gchar *token = NULL;
	g_object_get(delivery, "tracking-token", &token, NULL);
	return token;
}

/* The records and switches the contract names exist, and consent is off by default. */
static void
test_records(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureSequence) sequence = venture_sequence_new();
	g_autoptr(VentureSequenceDelivery) delivery = venture_sequence_delivery_new();
	g_autoptr(VentureOrganization) organization = venture_organization_new();
	g_autoptr(VentureEntity) seeded = NULL;
	g_autoptr(GError) error = NULL;
	gboolean allowed = TRUE;
	(void)unused;
	g_assert_cmpuint(venture_entity_registry_lookup(venture_entity_registry_get_default(), "sequence_link"), !=, G_TYPE_INVALID);
	g_assert_cmpuint(venture_entity_registry_lookup(venture_entity_registry_get_default(), "sequence_tracking_event"), !=, G_TYPE_INVALID);
	g_assert_nonnull(g_object_class_find_property(G_OBJECT_GET_CLASS(sequence), "tracking"));
	g_assert_nonnull(g_object_class_find_property(G_OBJECT_GET_CLASS(delivery), "tracking-token"));
	g_assert_nonnull(g_object_class_find_property(G_OBJECT_GET_CLASS(organization), "sequence-tracking"));
	seeded = venture_database_get(f->db, VENTURE_TYPE_ORGANIZATION, venture_context_get_default_organization_id(f->context), &error);
	g_assert_no_error(error);
	g_object_get(seeded, "sequence-tracking", &allowed, NULL);
	g_assert_false(allowed);
}

/* With both switches on, links are wrapped, the pixel appended and the
 * originals retained; the token never leaves through generic output. */
static void
test_wrapped(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) delivery = tracked_delivery(f);
	g_autoptr(GPtrArray) links = rows(f, VENTURE_TYPE_SEQUENCE_LINK);
	g_autoptr(JsonNode) json = venture_serializable_to_json(VENTURE_SERIALIZABLE(delivery), FALSE);
	g_autofree gchar *token = token_of(delivery);
	g_autofree gchar *body = NULL;
	g_autofree gchar *first = NULL;
	g_autofree gchar *second = NULL;
	g_autofree gchar *pixel = NULL;
	g_autofree gchar *text = venture_json_to_string(json, FALSE);
	g_autofree gchar *url = NULL;
	gint64 position;
	(void)unused;
	g_assert_nonnull(token);
	g_assert_cmpuint(strlen(token), ==, 64);
	g_object_get(delivery, "body", &body, NULL);
	first = g_strdup_printf("/t/c/%s/1", token);
	second = g_strdup_printf("/t/c/%s/2", token);
	pixel = g_strdup_printf("<img src=\"/t/o/%s.gif\"", token);
	g_assert_nonnull(strstr(body, first));
	g_assert_nonnull(strstr(body, second));
	g_assert_nonnull(strstr(body, pixel));
	g_assert_null(strstr(body, "https://example.com/a"));
	g_assert_nonnull(strstr(body, "Bye"));
	g_assert_cmpuint(links->len, ==, 2);
	g_object_get(g_ptr_array_index(links, 1), "url", &url, "position", &position, NULL);
	g_assert_cmpstr(url, ==, "http://example.org/b?x=1");
	g_assert_cmpint(position, ==, 2);
	g_assert_null(strstr(text, "tracking_token"));
}

/* Tracking off by default at the organization: nothing is rewritten. */
static void
test_privacy_default(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) enrollment = enroll(f);
	g_autoptr(GPtrArray) deliveries = NULL;
	g_autoptr(GPtrArray) links = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *token = NULL;
	(void)unused;
	run(f, "2026-09-14T11:00:00Z", 1);
	deliveries = rows(f, VENTURE_TYPE_SEQUENCE_DELIVERY);
	links = rows(f, VENTURE_TYPE_SEQUENCE_LINK);
	g_assert_cmpuint(deliveries->len, ==, 1);
	g_object_get(g_ptr_array_index(deliveries, 0), "body", &body, "tracking-token", &token, NULL);
	g_assert_null(token);
	g_assert_nonnull(strstr(body, "https://example.com/a"));
	g_assert_null(strstr(body, "/t/o/"));
	g_assert_cmpuint(links->len, ==, 0);
}

/* The organization allows it but this sequence does not ask for it. */
static void
test_sequence_off(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) enrollment = NULL;
	g_autoptr(GPtrArray) deliveries = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *token = NULL;
	(void)unused;
	consent(f, TRUE);
	g_object_set(f->sequence, "tracking", FALSE, NULL);
	save(f, f->sequence);
	enrollment = enroll(f);
	run(f, "2026-09-14T11:00:00Z", 1);
	deliveries = rows(f, VENTURE_TYPE_SEQUENCE_DELIVERY);
	g_object_get(g_ptr_array_index(deliveries, 0), "body", &body, "tracking-token", &token, NULL);
	g_assert_null(token);
	g_assert_nonnull(strstr(body, "https://example.com/a"));
	g_assert_null(strstr(body, "/t/c/"));
}

static guint
interactions(Fixture *f)
{
	g_autoptr(GPtrArray) found = rows(f, VENTURE_TYPE_INTERACTION);
	return found->len;
}

/* Opens count once per delivery per day, land on the timeline as outbound
 * touches, and never exit the enrollment as a reply. */
static void
test_open(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) delivery = tracked_delivery(f);
	g_autofree gchar *token = token_of(delivery);
	g_autoptr(GDateTime) morning = g_date_time_new_from_iso8601("2026-09-14T12:00:00Z", NULL);
	g_autoptr(GDateTime) evening = g_date_time_new_from_iso8601("2026-09-14T20:00:00Z", NULL);
	g_autoptr(GDateTime) tomorrow = g_date_time_new_from_iso8601("2026-09-15T08:00:00Z", NULL);
	g_autoptr(GPtrArray) events = NULL;
	g_autoptr(GPtrArray) enrollments = NULL;
	g_autoptr(GPtrArray) touches = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *subject = NULL;
	gboolean outbound;
	gint kind, status;
	(void)unused;
	g_assert_true(venture_sequence_service_record_open(venture_sequence_service_get(f->db), token, morning, NULL, &error));
	g_assert_no_error(error);
	g_assert_true(venture_sequence_service_record_open(venture_sequence_service_get(f->db), token, evening, NULL, &error));
	g_assert_no_error(error);
	events = rows(f, VENTURE_TYPE_SEQUENCE_TRACKING_EVENT);
	g_assert_cmpuint(events->len, ==, 1);
	g_object_get(g_ptr_array_index(events, 0), "kind", &kind, NULL);
	g_assert_cmpint(kind, ==, 0);
	g_assert_cmpuint(interactions(f), ==, 1);
	touches = rows(f, VENTURE_TYPE_INTERACTION);
	g_object_get(g_ptr_array_index(touches, 0), "subject", &subject, "outbound", &outbound, "kind", &kind, NULL);
	g_assert_cmpstr(subject, ==, "Opened: Hello Ada");
	g_assert_true(outbound);
	g_assert_cmpint(kind, ==, VENTURE_INTERACTION_KIND_OUTREACH);
	enrollments = rows(f, VENTURE_TYPE_SEQUENCE_ENROLLMENT);
	g_object_get(g_ptr_array_index(enrollments, 0), "status", &status, NULL);
	g_assert_cmpint(status, ==, 0);
	g_assert_true(venture_sequence_service_record_open(venture_sequence_service_get(f->db), token, tomorrow, NULL, &error));
	g_clear_pointer(&events, g_ptr_array_unref);
	events = rows(f, VENTURE_TYPE_SEQUENCE_TRACKING_EVENT);
	g_assert_cmpuint(events->len, ==, 2);
	g_assert_false(venture_sequence_service_record_open(venture_sequence_service_get(f->db),
		"0000000000000000000000000000000000000000000000000000000000000000", tomorrow, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_clear_error(&error);
	g_assert_false(venture_sequence_service_record_open(venture_sequence_service_get(f->db), "short", tomorrow, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_clear_pointer(&events, g_ptr_array_unref);
	events = rows(f, VENTURE_TYPE_SEQUENCE_TRACKING_EVENT);
	g_assert_cmpuint(events->len, ==, 2);
	g_assert_cmpuint(interactions(f), ==, 2);
}

/* Every click is recorded and resolves to the original destination. */
static void
test_click(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) delivery = tracked_delivery(f);
	g_autofree gchar *token = token_of(delivery);
	g_autoptr(GDateTime) now = g_date_time_new_from_iso8601("2026-09-14T12:00:00Z", NULL);
	g_autoptr(GPtrArray) events = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *url = NULL;
	g_autofree gchar *again = NULL;
	g_autofree gchar *missing = NULL;
	gint64 position;
	gint kind;
	(void)unused;
	url = venture_sequence_service_record_click(venture_sequence_service_get(f->db), token, 2, now, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(url, ==, "http://example.org/b?x=1");
	again = venture_sequence_service_record_click(venture_sequence_service_get(f->db), token, 2, now, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(again, ==, "http://example.org/b?x=1");
	events = rows(f, VENTURE_TYPE_SEQUENCE_TRACKING_EVENT);
	g_assert_cmpuint(events->len, ==, 2);
	g_object_get(g_ptr_array_index(events, 0), "kind", &kind, "link-position", &position, NULL);
	g_assert_cmpint(kind, ==, 1);
	g_assert_cmpint(position, ==, 2);
	g_assert_cmpuint(interactions(f), ==, 2);
	missing = venture_sequence_service_record_click(venture_sequence_service_get(f->db), token, 9, now, NULL, &error);
	g_assert_null(missing);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_clear_error(&error);
	missing = venture_sequence_service_record_click(venture_sequence_service_get(f->db),
		"0000000000000000000000000000000000000000000000000000000000000000", 1, now, NULL, &error);
	g_assert_null(missing);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_clear_pointer(&events, g_ptr_array_unref);
	events = rows(f, VENTURE_TYPE_SEQUENCE_TRACKING_EVENT);
	g_assert_cmpuint(events->len, ==, 2);
}

/* Links and events are evidence: generic writes and removals are refused. */
static void
test_evidence(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) delivery = tracked_delivery(f);
	g_autoptr(GPtrArray) links = rows(f, VENTURE_TYPE_SEQUENCE_LINK);
	g_autoptr(VentureEntity) forged = g_object_new(VENTURE_TYPE_SEQUENCE_TRACKING_EVENT, "organization-id", (gint64)1,
		"delivery-id", venture_entity_get_id(delivery), "kind", 0, NULL);
	g_autoptr(VentureEntity) link = g_object_new(VENTURE_TYPE_SEQUENCE_LINK, "organization-id", (gint64)1,
		"delivery-id", venture_entity_get_id(delivery), "position", (gint64)3, "url", "https://evil.example/", NULL);
	g_autoptr(GError) error = NULL;
	(void)unused;
	g_assert_false(venture_database_save(f->db, forged, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_nonnull(strstr(error->message, "VentureSequenceService"));
	g_clear_error(&error);
	g_assert_false(venture_database_save(f->db, link, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	g_object_set(g_ptr_array_index(links, 0), "url", "https://evil.example/", NULL);
	g_assert_false(venture_database_save(f->db, g_ptr_array_index(links, 0), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	g_assert_false(venture_database_delete(f->db, g_ptr_array_index(links, 0), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

/* Per-step counts and rates, with a reply attributed to the step it answered. */
static void
test_report(Fixture *f, gconstpointer unused)
{
	VentureReportRegistry *registry = venture_context_get_report_registry(f->context);
	g_autoptr(VentureEntity) delivery = tracked_delivery(f);
	g_autofree gchar *token = token_of(delivery);
	g_autoptr(GDateTime) now = g_date_time_new_from_iso8601("2026-09-14T12:00:00Z", NULL);
	g_autoptr(VentureEntity) reply = NULL;
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(JsonNode) json = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *url = NULL;
	JsonArray *table;
	JsonObject *row;
	(void)unused;
	g_assert_nonnull(venture_report_registry_lookup(registry, "sequence_engagement"));
	g_object_set(delivery, "state", 1, "external-message-id", "provider-1", NULL);
	save(f, delivery);
	g_assert_true(venture_sequence_service_record_open(venture_sequence_service_get(f->db), token, now, NULL, &error));
	url = venture_sequence_service_record_click(venture_sequence_service_get(f->db), token, 1, now, NULL, &error);
	g_assert_nonnull(url);
	reply = g_object_new(VENTURE_TYPE_INTERACTION, "organization-id", (gint64)1,
		"contact-id", venture_entity_get_id(f->contact), "subject", "Interested", "outbound", FALSE, NULL);
	save(f, reply);
	result = venture_report_generate(venture_report_registry_lookup(registry, "sequence_engagement"), f->context, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	json = venture_report_result_to_json(result);
	table = json_object_get_array_member(json_node_get_object(json), "rows");
	g_assert_cmpuint(json_array_get_length(table), ==, 2);
	row = json_array_get_object_element(table, 0);
	g_assert_cmpfloat(json_object_get_double_member(row, "step"), ==, 10);
	g_assert_cmpfloat(json_object_get_double_member(row, "sent"), ==, 1);
	g_assert_cmpfloat(json_object_get_double_member(row, "opened"), ==, 1);
	g_assert_cmpfloat(json_object_get_double_member(row, "clicked"), ==, 1);
	g_assert_cmpfloat(json_object_get_double_member(row, "replies"), ==, 1);
	g_assert_cmpfloat(json_object_get_double_member(row, "open_rate"), ==, 100);
	g_assert_cmpfloat(json_object_get_double_member(row, "click_rate"), ==, 100);
	g_assert_cmpfloat(json_object_get_double_member(row, "reply_rate"), ==, 100);
	row = json_array_get_object_element(table, 1);
	g_assert_cmpfloat(json_object_get_double_member(row, "step"), ==, 20);
	g_assert_cmpfloat(json_object_get_double_member(row, "sent"), ==, 0);
	g_assert_cmpfloat(json_object_get_double_member(row, "open_rate"), ==, 0);
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
http(SoupSession *session, const gchar *base, const gchar *path, gchar **text, gchar **location, gchar **type)
{
	g_autofree gchar *url = g_strconcat(base, path, NULL);
	g_autoptr(SoupMessage) message = soup_message_new("GET", url);
	Reply reply = { FALSE, NULL, NULL };
	guint status;
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL, reply_done, &reply);
	while (!reply.done)
		g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(reply.error);
	*text = g_strndup(g_bytes_get_data(reply.body, NULL), g_bytes_get_size(reply.body));
	if (NULL != location)
		*location = g_strdup(soup_message_headers_get_one(soup_message_get_response_headers(message), "Location"));
	if (NULL != type)
		*type = g_strdup(soup_message_headers_get_one(soup_message_get_response_headers(message), "Content-Type"));
	status = soup_message_get_status(message);
	g_bytes_unref(reply.body);
	return status;
}

/* The pixel and wrapped links are real HTTP endpoints that need no session. */
static void
test_surfaces(Fixture *f, gconstpointer unused)
{
	g_autoptr(GSocketListener) probe = g_socket_listener_new();
	g_autoptr(VentureWebServer) server = NULL;
	g_autoptr(SoupSession) session = soup_session_new();
	g_autoptr(VentureEntity) delivery = NULL;
	g_autoptr(GPtrArray) events = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *state = g_dir_make_tmp("venture-tracking-web-XXXXXX", NULL);
	g_autofree gchar *base = NULL;
	g_autofree gchar *token = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *open_path = NULL;
	g_autofree gchar *click_path = NULL;
	g_autofree gchar *bad_click = NULL;
	g_autofree gchar *page = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *location = NULL;
	g_autofree gchar *type = NULL;
	g_autofree gchar *absolute = NULL;
	guint port = g_socket_listener_add_any_inet_port(probe, NULL, &error);
	(void)unused;
	g_assert_no_error(error);
	g_clear_object(&probe);
	base = g_strdup_printf("http://127.0.0.1:%u", port);
	g_object_set(f->config, "state-dir", state, "server-bind-address", "127.0.0.1",
		"server-port", (gint64)port, "security-require-auth", FALSE, "server-base-url", base, NULL);
	/* The context reads the base URL once; hand it to the live service too. */
	g_object_set(venture_sequence_service_get(f->db), "base-url", base, NULL);
	server = venture_web_server_new(f->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(server, &error));
	g_assert_no_error(error);
	delivery = tracked_delivery(f);
	token = token_of(delivery);
	g_object_get(delivery, "body", &body, NULL);
	absolute = g_strdup_printf("%s/t/o/%s.gif", base, token);
	g_assert_nonnull(strstr(body, absolute));
	open_path = g_strdup_printf("/t/o/%s.gif", token);
	click_path = g_strdup_printf("/t/c/%s/1", token);
	bad_click = g_strdup_printf("/t/c/%s/9", token);
	g_assert_cmpuint(http(session, base, open_path, &text, NULL, &type), ==, 200);
	g_assert_cmpstr(type, ==, "image/gif");
	g_assert_true(g_str_has_prefix(text, "GIF89a"));
	g_clear_pointer(&text, g_free);
	g_assert_cmpuint(http(session, base, open_path, &text, NULL, NULL), ==, 200);
	g_clear_pointer(&text, g_free);
	events = rows(f, VENTURE_TYPE_SEQUENCE_TRACKING_EVENT);
	g_assert_cmpuint(events->len, ==, 1);
	g_assert_cmpuint(http(session, base, click_path, &text, &location, NULL), ==, 302);
	g_assert_cmpstr(location, ==, "https://example.com/a");
	g_clear_pointer(&text, g_free);
	g_assert_cmpuint(http(session, base, bad_click, &text, NULL, NULL), ==, 404);
	g_clear_pointer(&text, g_free);
	g_assert_cmpuint(http(session, base, "/t/o/0000000000000000000000000000000000000000000000000000000000000000.gif", &text, NULL, NULL), ==, 404);
	g_clear_pointer(&text, g_free);
	g_assert_cmpuint(http(session, base, "/t/c/0000000000000000000000000000000000000000000000000000000000000000/1", &text, NULL, NULL), ==, 404);
	g_clear_pointer(&text, g_free);
	g_assert_cmpuint(http(session, base, "/t/o/nope", &text, NULL, NULL), ==, 404);
	g_clear_pointer(&text, g_free);
	g_clear_pointer(&events, g_ptr_array_unref);
	events = rows(f, VENTURE_TYPE_SEQUENCE_TRACKING_EVENT);
	g_assert_cmpuint(events->len, ==, 2);
	page = g_strdup_printf("/e/contact/%" G_GINT64_FORMAT, venture_entity_get_id(f->contact));
	g_assert_cmpuint(http(session, base, page, &text, NULL, NULL), ==, 200);
	g_assert_nonnull(strstr(text, "Opened: Hello Ada"));
	g_assert_nonnull(strstr(text, "Clicked link 1"));
	g_clear_pointer(&text, g_free);
	g_assert_cmpuint(http(session, base, "/api/v1/reports/sequence_engagement?period=all_time", &text, NULL, NULL), ==, 200);
	g_assert_nonnull(strstr(text, "open_rate"));
	g_clear_pointer(&text, g_free);
	venture_config_set_module_enabled(f->config, "sequences", FALSE);
	g_assert_cmpuint(http(session, base, open_path, &text, NULL, NULL), ==, 404);
	g_clear_pointer(&text, g_free);
	g_assert_cmpuint(http(session, base, click_path, &text, NULL, NULL), ==, 404);
	venture_config_set_module_enabled(f->config, "sequences", TRUE);
	venture_web_server_stop(server);
	g_clear_object(&server);
	venture_test_remove_tree(state);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/sequence-tracking/records", Fixture, NULL, setup, test_records, teardown);
	g_test_add("/sequence-tracking/wrapped", Fixture, NULL, setup, test_wrapped, teardown);
	g_test_add("/sequence-tracking/privacy_default", Fixture, NULL, setup, test_privacy_default, teardown);
	g_test_add("/sequence-tracking/sequence_off", Fixture, NULL, setup, test_sequence_off, teardown);
	g_test_add("/sequence-tracking/open", Fixture, NULL, setup, test_open, teardown);
	g_test_add("/sequence-tracking/click", Fixture, NULL, setup, test_click, teardown);
	g_test_add("/sequence-tracking/evidence", Fixture, NULL, setup, test_evidence, teardown);
	g_test_add("/sequence-tracking/report", Fixture, NULL, setup, test_report, teardown);
	g_test_add("/sequence-tracking/surfaces", Fixture, NULL, setup, test_surfaces, teardown);
	return g_test_run();
}
