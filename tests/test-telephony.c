/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Phone calls and text messages (docs/telephony.org). The providers are a
 * fake 3CX phone system and a fake VoIP.ms, both one SoupServer on
 * loopback in its own thread (the code under test blocks the main loop on
 * each request), answering from the recorded fixtures in
 * tests/fixtures/telephony. A third provider, defined in this file with
 * the same one-line registration a real one uses, has a signed webhook and
 * delivery receipts. Nothing leaves the machine and no number is real. */
#include <venture.h>
#include <string.h>
#include <libsoup/soup.h>
#include "venture-test-util.h"
#include "venture-test-accounting.h"

#define FIXTURE(name) VENTURE_TEST_FIXTURES "/telephony/" name
#define PBX_SECRET "pbx-secret-0001"
#define VOIP_SECRET "voip-secret-0001"
#define FAKE_SECRET "fake-secret-0001"
#define HOOK_SECRET "hook-secret-0001"

/* --- Everything logged, so no test can miss a secret in a log line ------ */

static GMutex log_lock;
static GString *logged;

static GLogWriterOutput
capture_log(GLogLevelFlags level, const GLogField *fields, gsize n_fields, gpointer data)
{
	gsize i;
	g_mutex_lock(&log_lock);
	for (i = 0; i < n_fields; i++)
		if (g_strcmp0(fields[i].key, "MESSAGE") == 0 && fields[i].length < 0)
		{
			g_string_append(logged, fields[i].value);
			g_string_append_c(logged, '\n');
		}
	g_mutex_unlock(&log_lock);
	return g_log_writer_default(level, fields, n_fields, data);
}

static void
assert_not_logged(const gchar *secret)
{
	g_mutex_lock(&log_lock);
	g_assert_null(strstr(logged->str, secret));
	g_mutex_unlock(&log_lock);
}

/* --- The fake providers ---------------------------------------------- */

typedef struct {
	GThread *thread;
	GMainContext *context;
	GMainLoop *loop;
	SoupServer *server;
	GMutex lock;
	GCond ready;
	gboolean started;
	gchar *origin;
	/* Read and written under the lock. */
	GPtrArray *hits;	/* "METHOD /path" in arrival order */
	GHashTable *bodies;	/* path -> last body */
	GHashTable *queries;	/* path -> last query string */
	GHashTable *auth;	/* path -> last Authorization */
	gchar *token_body;
	guint token_status;
	gchar *makecall_body;
	guint makecall_status;
	gchar *voip_body;
} Fake;

static gchar *
fixture_text(const gchar *path)
{
	g_autoptr(GError) error = NULL;
	gchar *text = NULL;
	g_assert_true(g_file_get_contents(path, &text, NULL, &error));
	g_assert_no_error(error);
	g_strstrip(text);
	return text;
}

static void
answer(SoupServerMessage *message, guint status, const gchar *type, const gchar *body)
{
	soup_server_message_set_status(message, status, NULL);
	soup_server_message_set_response(message, type, SOUP_MEMORY_COPY, body, strlen(body));
}

static void
fake_handle(SoupServer *server, SoupServerMessage *message, const gchar *path, GHashTable *query, gpointer data)
{
	Fake *f = data;
	g_autoptr(GBytes) body = soup_message_body_flatten(soup_server_message_get_request_body(message));
	g_autofree gchar *text = g_strndup(g_bytes_get_data(body, NULL), g_bytes_get_size(body));
	const gchar *method = soup_server_message_get_method(message);
	const gchar *authorization = soup_message_headers_get_one(soup_server_message_get_request_headers(message), "Authorization");
	const gchar *raw_query = g_uri_get_query(soup_server_message_get_uri(message));
	(void)server; (void)query;
	g_mutex_lock(&f->lock);
	g_ptr_array_add(f->hits, g_strdup_printf("%s %s", method, path));
	g_hash_table_replace(f->bodies, g_strdup(path), g_strdup(text));
	g_hash_table_replace(f->queries, g_strdup(path), g_strdup(raw_query != NULL ? raw_query : ""));
	g_hash_table_replace(f->auth, g_strdup(path), g_strdup(authorization != NULL ? authorization : ""));
	if (g_strcmp0(path, "/connect/token") == 0)
		answer(message, f->token_status, "application/json", f->token_body);
	else if (g_str_has_prefix(path, "/callcontrol/"))
		answer(message, f->makecall_status, "application/json", f->makecall_body);
	else if (g_strcmp0(path, "/api/v1/rest.php") == 0)
		answer(message, 200, "application/json", f->voip_body);
	else if (g_strcmp0(path, "/fake/send") == 0)
	{
		if (g_strcmp0(authorization, "Bearer " FAKE_SECRET) != 0)
			answer(message, 401, "application/json", "{\"error\":\"unauthorized\"}");
		else
			answer(message, 200, "application/json", "{\"id\":\"m-1\"}");
	}
	else if (g_strcmp0(path, "/media/7002.jpg") == 0)
		answer(message, 200, "image/jpeg", "\xff\xd8\xff\xe0 not really a jpeg");
	else
		answer(message, 404, "text/plain", "no such path");
	g_mutex_unlock(&f->lock);
}

static gpointer
fake_serve(gpointer data)
{
	Fake *f = data;
	g_autoptr(GError) error = NULL;
	GSList *uris;
	g_main_context_push_thread_default(f->context);
	f->server = soup_server_new(NULL, NULL);
	soup_server_add_handler(f->server, NULL, fake_handle, f, NULL);
	g_assert_true(soup_server_listen_local(f->server, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, &error));
	uris = soup_server_get_uris(f->server);
	g_mutex_lock(&f->lock);
	f->origin = g_strdup_printf("http://127.0.0.1:%d", g_uri_get_port(uris->data));
	f->started = TRUE;
	g_cond_signal(&f->ready);
	g_mutex_unlock(&f->lock);
	g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
	g_main_loop_run(f->loop);
	soup_server_disconnect(f->server);
	g_clear_object(&f->server);
	g_main_context_pop_thread_default(f->context);
	return NULL;
}

static void
fake_start(Fake *f)
{
	memset(f, 0, sizeof *f);
	g_mutex_init(&f->lock);
	g_cond_init(&f->ready);
	f->hits = g_ptr_array_new_with_free_func(g_free);
	f->bodies = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	f->queries = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	f->auth = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	f->token_body = fixture_text(FIXTURE("3cx-token.json"));
	f->token_status = 200;
	f->makecall_body = fixture_text(FIXTURE("3cx-makecall.json"));
	f->makecall_status = 200;
	f->voip_body = fixture_text(FIXTURE("voipms-sendsms.json"));
	f->context = g_main_context_new();
	f->loop = g_main_loop_new(f->context, FALSE);
	f->thread = g_thread_new("fake-telephony", fake_serve, f);
	g_mutex_lock(&f->lock);
	while (!f->started) g_cond_wait(&f->ready, &f->lock);
	g_mutex_unlock(&f->lock);
}

static gboolean
fake_quit(gpointer data)
{
	g_main_loop_quit(data);
	return G_SOURCE_REMOVE;
}

static void
fake_stop(Fake *f)
{
	GSource *source = g_idle_source_new();
	g_source_set_callback(source, fake_quit, f->loop, NULL);
	g_source_attach(source, f->context);
	g_source_unref(source);
	g_thread_join(f->thread);
	g_main_loop_unref(f->loop);
	g_main_context_unref(f->context);
	g_ptr_array_unref(f->hits);
	g_hash_table_unref(f->bodies);
	g_hash_table_unref(f->queries);
	g_hash_table_unref(f->auth);
	g_free(f->token_body); g_free(f->makecall_body); g_free(f->voip_body); g_free(f->origin);
	g_mutex_clear(&f->lock);
	g_cond_clear(&f->ready);
}

static guint
fake_count(Fake *f, const gchar *hit)
{
	guint i, n = 0;
	g_mutex_lock(&f->lock);
	for (i = 0; i < f->hits->len; i++)
		if (g_strcmp0(g_ptr_array_index(f->hits, i), hit) == 0) n++;
	g_mutex_unlock(&f->lock);
	return n;
}

static gchar *
fake_seen(Fake *f, GHashTable *table, const gchar *path)
{
	gchar *value;
	g_mutex_lock(&f->lock);
	value = g_strdup(g_hash_table_lookup(table, path));
	g_mutex_unlock(&f->lock);
	return value;
}

static void
fake_set_voip(Fake *f, const gchar *fixture)
{
	g_autofree gchar *text = fixture_text(fixture);
	g_mutex_lock(&f->lock);
	g_free(f->voip_body);
	f->voip_body = g_steal_pointer(&text);
	g_mutex_unlock(&f->lock);
}

/* --- A provider defined in a test, registered like any other ---------- */

#define FAKE_TYPE_SIGNED (fake_signed_get_type())
G_DECLARE_FINAL_TYPE(FakeSigned, fake_signed, FAKE, SIGNED, GObject)
struct _FakeSigned { GObject parent_instance; };
static void fake_signed_iface_init(VentureTelephonyConnectorInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE(FakeSigned, fake_signed, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(VENTURE_TYPE_TELEPHONY_CONNECTOR, fake_signed_iface_init))
static void fake_signed_class_init(FakeSignedClass *klass) { (void)klass; }
static void fake_signed_init(FakeSigned *self) { (void)self; }
static const gchar *fake_id(VentureTelephonyConnector *self) { (void)self; return "fakesig"; }
static const gchar *fake_label(VentureTelephonyConnector *self) { (void)self; return "Signed test carrier"; }
static VentureTelephonyCapabilities fake_capabilities(VentureTelephonyConnector *self)
{
	(void)self;
	return VENTURE_TELEPHONY_CAPABILITY_SMS | VENTURE_TELEPHONY_CAPABILITY_DELIVERY_STATUS |
		VENTURE_TELEPHONY_CAPABILITY_SIGNED_WEBHOOK;
}
static JsonNode *fake_schema(VentureTelephonyConnector *self)
{
	(void)self;
	return json_from_string("{\"type\":\"object\",\"properties\":{"
		"\"account\":{\"type\":\"string\",\"title\":\"Account\"},"
		"\"secret\":{\"type\":\"string\",\"title\":\"API key\",\"x-sensitive\":true},"
		"\"webhook_secret\":{\"type\":\"string\",\"title\":\"Signing secret\",\"x-sensitive\":true}}}", NULL);
}
static gchar *fake_identity(VentureTelephonyConnector *self, JsonObject *settings, GError **error)
{
	(void)self;
	if (!json_object_has_member(settings, "account"))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Account is required");
		return NULL;
	}
	return g_strdup(json_object_get_string_member(settings, "account"));
}
static gboolean fake_send(VentureTelephonyConnector *self, VentureTelephonyLink *link,
	const VentureTelephonyOutbound *message, gchar **provider_id, GError **error)
{
	g_autofree gchar *authorization = g_strdup_printf("Bearer %s", venture_telephony_link_get_setting(link, "secret"));
	g_autofree gchar *json = g_strdup_printf("{\"to\":\"%s\",\"text\":\"%s\"}", message->to, message->body);
	g_autoptr(GBytes) body = g_bytes_new(json, strlen(json));
	g_autoptr(GBytes) response = NULL;
	g_autoptr(JsonNode) parsed = NULL;
	guint status = 0;
	(void)self;
	if (!venture_telephony_link_request(link, "POST", "/fake/send", authorization, "application/json", body, &status, &response, error) ||
		!venture_telephony_http_status_error(status, "Signed test carrier", error))
		return FALSE;
	parsed = json_from_string(g_bytes_get_data(response, NULL), NULL);
	*provider_id = g_strdup(json_object_get_string_member(json_node_get_object(parsed), "id"));
	return TRUE;
}
static gboolean fake_verify(VentureTelephonyConnector *self, VentureTelephonyLink *link,
	const VentureTelephonyWebhook *hook, GError **error)
{
	(void)self;
	if (venture_telephony_signature_matches(venture_telephony_link_get_setting(link, "webhook_secret"), hook->body,
		hook->headers != NULL ? g_hash_table_lookup(hook->headers, "x-fake-signature") : NULL))
		return TRUE;
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED, "The signature does not match");
	return FALSE;
}
static GPtrArray *fake_parse(VentureTelephonyConnector *self, VentureTelephonyLink *link,
	const VentureTelephonyWebhook *hook, GError **error)
{
	g_autoptr(JsonNode) root = json_from_string(g_bytes_get_data(hook->body, NULL), error);
	GPtrArray *events = g_ptr_array_new_with_free_func((GDestroyNotify)venture_telephony_event_free);
	JsonArray *list;
	guint i;
	(void)self; (void)link;
	if (root == NULL) { g_ptr_array_unref(events); return NULL; }
	list = json_object_get_array_member(json_node_get_object(root), "events");
	for (i = 0; i < json_array_get_length(list); i++)
	{
		JsonObject *item = json_array_get_object_element(list, i);
		VentureTelephonyEvent *event = venture_telephony_event_new(VENTURE_TELEPHONY_EVENT_STATUS);
		event->id = g_strdup(json_object_get_string_member(item, "id"));
		event->status = g_strdup(json_object_get_string_member(item, "status"));
		g_ptr_array_add(events, event);
	}
	return events;
}
static void
fake_signed_iface_init(VentureTelephonyConnectorInterface *iface)
{
	iface->get_id = fake_id;
	iface->get_label = fake_label;
	iface->get_capabilities = fake_capabilities;
	iface->settings_schema = fake_schema;
	iface->account_identity = fake_identity;
	iface->send_message = fake_send;
	iface->verify_webhook = fake_verify;
	iface->parse_webhook = fake_parse;
}
VENTURE_TELEPHONY_PROVIDER(fake_signed_get_type)

/* --- The fixture ------------------------------------------------------ */

typedef struct {
	VentureDatabase *db;
	VentureConfig *config;
	VentureContext *context;
	VentureTelephonyService *service;
	Fake fake;
	VentureEntity *ada;
	VentureEntity *alice;
	gchar *token;
	gchar *root;
} Fixture;

static void
persist(Fixture *f, gpointer row)
{
	g_autoptr(GError) error = NULL;
	gboolean saved = venture_database_save(f->db, VENTURE_ENTITY(row), NULL, &error);
	g_assert_no_error(error);
	g_assert_true(saved);
}

static VentureEntity *
reread(Fixture *f, VentureEntity *row)
{
	g_autoptr(GError) error = NULL;
	VentureEntity *fresh = venture_database_get(f->db, G_OBJECT_TYPE(row), venture_entity_get_id(row), &error);
	g_assert_no_error(error);
	g_assert_nonnull(fresh);
	return fresh;
}

static GDateTime *
at(const gchar *text)
{
	GDateTime *value = g_date_time_new_from_iso8601(text, NULL);
	g_assert_nonnull(value);
	return value;
}

static gchar *
text_of(gpointer row, const gchar *field)
{
	gchar *value = NULL;
	g_object_get(row, field, &value, NULL);
	return value;
}

static gint64
number_of(gpointer row, const gchar *field)
{
	gint64 value = 0;
	g_object_get(row, field, &value, NULL);
	return value;
}

static VentureEntity *
person(Fixture *f, const gchar *name, const gchar *phone, const gchar *zone)
{
	VentureEntity *row = g_object_new(VENTURE_TYPE_CONTACT, "organization-id", (gint64)1, "name", name,
		"phone", phone, "timezone", zone, NULL);
	g_autofree gchar *email = g_strdup_printf("%s@example.com", name);
	g_strdelimit(email, " ", '.');
	g_object_set(row, "email", email, NULL);
	persist(f, row);
	return row;
}

static void
setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) key = g_bytes_new_static("01234567890123456789012345678901", 32);
	g_autoptr(VentureEntity) extension = NULL;
	(void)data;
	fake_start(&f->fake);
	f->root = g_dir_make_tmp("venture-telephony-XXXXXX", &error);
	g_assert_no_error(error);
	f->config = venture_config_new();
	g_object_set(f->config, "connectors-allow-plaintext-loopback", TRUE,
		"server-base-url", "https://venture.example.com", "state-dir", f->root, NULL);
	f->db = venture_test_accounting_database(&error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	g_assert_true(venture_integration_service_set_key(venture_integration_service_get(f->db), key, &error));
	f->context = venture_context_new(f->config, f->db);
	f->service = venture_telephony_service_get(f->db);
	f->ada = person(f, "Ada", "(555) 010-0123", "UTC");
	f->alice = g_object_new(VENTURE_TYPE_USER, "username", "alice",
		"role", VENTURE_USER_ROLE_EDITOR, "active", TRUE, NULL);
	persist(f, f->alice);
	extension = g_object_new(VENTURE_TYPE_TELEPHONY_EXTENSION, "organization-id", (gint64)1,
		"user-id", venture_entity_get_id(f->alice), "extension", "101", NULL);
	persist(f, extension);
}

static void
teardown(Fixture *f, gconstpointer data)
{
	(void)data;
	g_clear_object(&f->ada);
	g_clear_object(&f->alice);
	g_clear_pointer(&f->token, g_free);
	g_clear_object(&f->context);
	venture_test_accounting_database_cleanup(f->db);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
	fake_stop(&f->fake);
	venture_test_remove_tree(f->root);
	g_clear_pointer(&f->root, g_free);
}

static VentureEntity *
connect_line(Fixture *f, const gchar *provider, const gchar *json)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) settings = json_from_string(json, &error);
	VentureEntity *line;
	g_assert_no_error(error);
	json_object_set_string_member(json_node_get_object(settings), "base_url", f->fake.origin);
	g_clear_pointer(&f->token, g_free);
	line = venture_telephony_service_connect(f->service, 1, provider, json_node_get_object(settings), 0, NULL, &f->token, &error);
	g_assert_no_error(error);
	g_assert_nonnull(line);
	g_assert_nonnull(f->token);
	g_assert_cmpuint(strlen(f->token), >=, 40);
	return line;
}

static VentureEntity *
connect_3cx(Fixture *f)
{
	return connect_line(f, "3cx", "{\"name\":\"Office phones\",\"client_id\":\"venture-pbx\",\"client_secret\":\"" PBX_SECRET "\"}");
}

static VentureEntity *
connect_voipms(Fixture *f)
{
	return connect_line(f, "voipms", "{\"name\":\"Texts\",\"api_username\":\"ops@example.com\",\"api_password\":\"" VOIP_SECRET "\","
		"\"phone_number\":\"+1 555 010 0100\",\"webhook_sources\":\"127.0.0.1\",\"media_hosts\":\"127.0.0.1\"}");
}

static void
agree(Fixture *f, VentureEntity *contact)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) when = at("2026-09-01T12:00:00Z");
	g_assert_true(venture_telephony_service_record_consent(f->service, contact, TRUE,
		"Said yes to texts on the phone on 1 September", NULL, when, NULL, &error));
	g_assert_no_error(error);
}

/* One webhook request: a query string (VoIP.ms calls back with one) or a
 * JSON body, from 127.0.0.1 unless a test says otherwise. */
static JsonNode *
deliver_hook(Fixture *f, const gchar *token, const gchar *query, const gchar *json,
	GHashTable *headers, const gchar *remote, const gchar *when, GError **error)
{
	g_autoptr(GHashTable) decoded = query != NULL ? soup_form_decode(query) : NULL;
	g_autoptr(GBytes) body = g_bytes_new(json != NULL ? json : "", json != NULL ? strlen(json) : 0);
	g_autoptr(GDateTime) now = at(when != NULL ? when : "2026-09-14T15:30:00Z");
	g_autofree gchar *reply = NULL;
	VentureTelephonyWebhook hook;
	hook.method = json != NULL ? "POST" : "GET";
	hook.remote_address = remote != NULL ? remote : "127.0.0.1";
	hook.query = decoded;
	hook.headers = headers;
	hook.content_type = json != NULL ? "application/json" : NULL;
	hook.body = body;
	return venture_telephony_service_receive(f->service, token, &hook, now, &reply, error);
}

static JsonNode *
hook_fixture(Fixture *f, const gchar *path)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = fixture_text(path);
	JsonNode *result;
	if (g_str_has_suffix(path, ".query"))
	{
		g_autofree gchar *encoded = g_uri_escape_string(f->fake.origin, NULL, FALSE);
		g_auto(GStrv) parts = g_strsplit(text, "{MEDIA_ORIGIN}", -1);
		g_autofree gchar *query = g_strjoinv(encoded, parts);
		result = deliver_hook(f, f->token, query, NULL, NULL, NULL, NULL, &error);
	}
	else
		result = deliver_hook(f, f->token, NULL, text, NULL, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	return result;
}

static gint64
counted(JsonNode *result, const gchar *what)
{
	return json_object_get_int_member(json_node_get_object(result), what);
}

static GPtrArray *
rows_of(Fixture *f, GType type, const gchar *field, gint64 value)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	g_autoptr(GError) error = NULL;
	GPtrArray *rows;
	venture_query_set_organization(query, 1);
	venture_query_set_limit(query, 0);
	if (field != NULL)
		venture_query_add_filter_int(query, field, VENTURE_FILTER_OP_EQ, value, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	rows = venture_database_find(f->db, query, &error);
	g_assert_no_error(error);
	return rows;
}

/* ------------------------------------------------------------------ */
/* The registry: a provider is one file that registers itself. If this
 * breaks, a provider is compiled in and nothing can find it. */
static void
test_registry(void)
{
	g_auto(GStrv) ids = venture_telephony_provider_ids();
	g_autoptr(VentureTelephonyConnector) pbx = venture_telephony_provider_new("3cx");
	g_autoptr(VentureTelephonyConnector) carrier = venture_telephony_provider_new("voipms");
	g_autoptr(VentureTelephonyConnector) signed_carrier = venture_telephony_provider_new("fakesig");
	g_autoptr(JsonNode) schema = NULL;
	VentureTelephonyCapabilities can;
	g_assert_true(g_strv_contains((const gchar *const *)ids, "3cx"));
	g_assert_true(g_strv_contains((const gchar *const *)ids, "voipms"));
	g_assert_true(g_strv_contains((const gchar *const *)ids, "fakesig"));
	g_assert_null(venture_telephony_provider_new("no-such-carrier"));
	g_assert_nonnull(pbx);
	can = venture_telephony_connector_get_capabilities(pbx);
	g_assert_true(can & VENTURE_TELEPHONY_CAPABILITY_CLICK_TO_CALL);
	g_assert_true(can & VENTURE_TELEPHONY_CAPABILITY_CALL_EVENTS);
	g_assert_false(can & VENTURE_TELEPHONY_CAPABILITY_SMS);
	g_assert_false(can & VENTURE_TELEPHONY_CAPABILITY_SIGNED_WEBHOOK);
	g_assert_nonnull(carrier);
	can = venture_telephony_connector_get_capabilities(carrier);
	g_assert_true(can & VENTURE_TELEPHONY_CAPABILITY_SMS);
	g_assert_true(can & VENTURE_TELEPHONY_CAPABILITY_MMS);
	g_assert_false(can & VENTURE_TELEPHONY_CAPABILITY_CLICK_TO_CALL);
	g_assert_false(can & VENTURE_TELEPHONY_CAPABILITY_SIGNED_WEBHOOK);
	g_assert_cmpstr(venture_telephony_connector_default_base_url(carrier), ==, "https://voip.ms");
	g_assert_nonnull(signed_carrier);
	g_assert_true(venture_telephony_connector_get_capabilities(signed_carrier) & VENTURE_TELEPHONY_CAPABILITY_DELIVERY_STATUS);
	/* The credential fields are marked so the settings page never fills them. */
	schema = venture_telephony_connector_settings_schema(pbx);
	g_assert_nonnull(schema);
	g_assert_true(json_object_get_boolean_member(json_object_get_object_member(json_object_get_object_member(
		json_node_get_object(schema), "properties"), "client_secret"), "x-sensitive"));
}

/* E.164 with a default country: the same person written three ways is one
 * number, which is what matching an inbound call depends on. */
static void
test_normalise(void)
{
	static const struct { const gchar *raw, *country, *expected; } cases[] = {
		{ "(555) 010-0123", NULL, "+15550100123" },
		{ "555.010.0123", "US", "+15550100123" },
		{ "1-555-010-0123", "US", "+15550100123" },
		{ "+1 555 010 0123", "GB", "+15550100123" },
		{ "011 44 20 7946 0123", "US", "+442079460123" },
		{ "020 7946 0123", "GB", "+442079460123" },
		{ "+44 (0)20 7946 0123", NULL, "+442079460123" },
		{ "00 49 30 1234567", "DE", "+49301234567" },
		{ "030 1234567", "DE", "+49301234567" },
		{ "0412 345 678", "AU", "+61412345678" },
		{ "tel:+15550100123", NULL, "+15550100123" },
		{ "", NULL, NULL },
		{ "call me", NULL, NULL },
		{ "123", NULL, NULL },
		{ "+1234567890123456", NULL, NULL },
	};
	gsize i;
	for (i = 0; i < G_N_ELEMENTS(cases); i++)
	{
		g_autofree gchar *got = venture_phone_normalize(cases[i].raw, cases[i].country);
		g_assert_cmpstr(got, ==, cases[i].expected);
	}
	g_assert_cmpstr(venture_phone_country_code("gb"), ==, "44");
	g_assert_null(venture_phone_country_code("XX"));
}

/* 3CX click-to-call: a token by client credentials, then makecall on the
 * user's own extension with the contact as destination. The phone system
 * rings that extension first and dials out when it answers; were the
 * destination used as the extension, the customer's phone would ring
 * before anybody was on the line. */
static void
test_click_to_call(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) line = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *call = NULL, *token_body = NULL, *makecall_body = NULL, *auth = NULL;
	g_autoptr(GHashTable) form = NULL;
	g_autoptr(JsonNode) sent = NULL;
	g_autoptr(VentureEntity) deal = NULL, ticket = NULL;
	(void)data;
	line = connect_3cx(f);
	call = venture_telephony_service_click_to_call(f->service, f->ada, venture_entity_get_id(f->alice), NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(call, ==, "4242");
	g_mutex_lock(&f->fake.lock);
	g_assert_cmpuint(f->fake.hits->len, ==, 2);
	g_assert_cmpstr(g_ptr_array_index(f->fake.hits, 0), ==, "POST /connect/token");
	g_assert_cmpstr(g_ptr_array_index(f->fake.hits, 1), ==, "POST /callcontrol/101/makecall");
	g_mutex_unlock(&f->fake.lock);
	token_body = fake_seen(&f->fake, f->fake.bodies, "/connect/token");
	form = soup_form_decode(token_body);
	g_assert_cmpstr(g_hash_table_lookup(form, "grant_type"), ==, "client_credentials");
	g_assert_cmpstr(g_hash_table_lookup(form, "client_id"), ==, "venture-pbx");
	g_assert_cmpstr(g_hash_table_lookup(form, "client_secret"), ==, PBX_SECRET);
	auth = fake_seen(&f->fake, f->fake.auth, "/callcontrol/101/makecall");
	g_assert_cmpstr(auth, ==, "Bearer fixture-access-token-0001");
	makecall_body = fake_seen(&f->fake, f->fake.bodies, "/callcontrol/101/makecall");
	sent = json_from_string(makecall_body, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(sent), "destination"), ==, "+15550100123");

	/* From a deal or a ticket the number is its contact's. */
	deal = g_object_new(VENTURE_TYPE_DEAL, "organization-id", (gint64)1, "name", "Kitchen refit",
		"contact-id", venture_entity_get_id(f->ada), NULL);
	persist(f, deal);
	g_clear_pointer(&call, g_free);
	call = venture_telephony_service_click_to_call(f->service, deal, venture_entity_get_id(f->alice), NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(call, ==, "4242");
	ticket = g_object_new(VENTURE_TYPE_TICKET, "organization-id", (gint64)1, "title", "Leaking tap",
		"contact-id", venture_entity_get_id(f->ada), NULL);
	persist(f, ticket);
	g_clear_pointer(&call, g_free);
	call = venture_telephony_service_click_to_call(f->service, ticket, venture_entity_get_id(f->alice), NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(fake_count(&f->fake, "POST /callcontrol/101/makecall"), ==, 3);
}

/* Plain-words refusals, each before anything is dialled. */
static void
test_click_to_call_refusals(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) line = NULL, bob = NULL, nobody = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *call = NULL;
	(void)data;
	call = venture_telephony_service_click_to_call(f->service, f->ada, venture_entity_get_id(f->alice), NULL, NULL, &error);
	g_assert_null(call);
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "No phone system"));
	g_clear_error(&error);
	line = connect_3cx(f);
	bob = g_object_new(VENTURE_TYPE_USER, "username", "bob", "active", TRUE, NULL);
	persist(f, bob);
	call = venture_telephony_service_click_to_call(f->service, f->ada, venture_entity_get_id(bob), NULL, NULL, &error);
	g_assert_null(call);
	g_assert_nonnull(strstr(error->message, "extension"));
	g_clear_error(&error);
	nobody = g_object_new(VENTURE_TYPE_CONTACT, "organization-id", (gint64)1, "name", "No Phone", NULL);
	persist(f, nobody);
	call = venture_telephony_service_click_to_call(f->service, nobody, venture_entity_get_id(f->alice), NULL, NULL, &error);
	g_assert_null(call);
	g_assert_nonnull(strstr(error->message, "phone number"));
	g_clear_error(&error);
	g_assert_cmpuint(f->fake.hits->len, ==, 0);
}

/* Error mapping and no retries. A refused credential says so without
 * repeating the secret the PBX echoed; a 503 is tried exactly once (a
 * retried makecall could ring the customer twice); a busy device is named. */
static void
test_3cx_errors(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) line = connect_3cx(f);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *call = NULL;
	(void)data;
	g_mutex_lock(&f->fake.lock);
	g_free(f->fake.token_body);
	f->fake.token_body = g_strdup("{\"error\":\"invalid_client\",\"error_description\":\"bad " PBX_SECRET "\"}");
	f->fake.token_status = 401;
	g_mutex_unlock(&f->fake.lock);
	call = venture_telephony_service_click_to_call(f->service, f->ada, venture_entity_get_id(f->alice), NULL, NULL, &error);
	g_assert_null(call);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_assert_null(strstr(error->message, PBX_SECRET));
	g_clear_error(&error);
	g_assert_cmpuint(fake_count(&f->fake, "POST /connect/token"), ==, 1);
	g_assert_cmpuint(fake_count(&f->fake, "POST /callcontrol/101/makecall"), ==, 0);

	g_mutex_lock(&f->fake.lock);
	g_free(f->fake.token_body);
	f->fake.token_body = fixture_text(FIXTURE("3cx-token.json"));
	f->fake.token_status = 200;
	f->fake.makecall_status = 503;
	g_mutex_unlock(&f->fake.lock);
	call = venture_telephony_service_click_to_call(f->service, f->ada, venture_entity_get_id(f->alice), NULL, NULL, &error);
	g_assert_null(call);
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_assert_cmpuint(fake_count(&f->fake, "POST /callcontrol/101/makecall"), ==, 1);

	g_mutex_lock(&f->fake.lock);
	g_free(f->fake.makecall_body);
	f->fake.makecall_body = fixture_text(FIXTURE("3cx-makecall-busy.json"));
	f->fake.makecall_status = 200;
	g_mutex_unlock(&f->fake.lock);
	call = venture_telephony_service_click_to_call(f->service, f->ada, venture_entity_get_id(f->alice), NULL, NULL, &error);
	g_assert_null(call);
	g_assert_nonnull(strstr(error->message, "no free device"));
	assert_not_logged(PBX_SECRET);
}

static VentureEntity *
only_call(Fixture *f, VentureEntity *contact)
{
	g_autoptr(GPtrArray) calls = rows_of(f, VENTURE_TYPE_ACTIVITY, "contact-id", venture_entity_get_id(contact));
	g_assert_cmpuint(calls->len, ==, 1);
	return g_object_ref(g_ptr_array_index(calls, 0));
}

/* Every call the phone system reports is logged once on the contact,
 * through log_call with the provider's call id as replay identity: the
 * same journal posted twice is one call, one history entry. */
static void
test_call_logging(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) line = connect_3cx(f);
	g_autoptr(JsonNode) first = NULL, again = NULL, recorded = NULL;
	g_autoptr(VentureEntity) call = NULL, organization = NULL;
	g_autoptr(GPtrArray) history = NULL, calls = NULL;
	g_autofree gchar *source = NULL, *external = NULL, *recording = NULL, *owner = NULL, *journal = NULL, *changed = NULL;
	g_autoptr(GError) error = NULL;
	gint direction = -1, outcome = -1;
	(void)data;
	first = hook_fixture(f, FIXTURE("3cx-journal-inbound.json"));
	g_assert_cmpint(counted(first, "calls"), ==, 1);
	call = only_call(f, f->ada);
	g_object_get(call, "call-external-source", &source, "call-external-id", &external, "call-direction", &direction,
		"call-outcome", &outcome, "call-recording-url", &recording, "owner", &owner, NULL);
	g_assert_true(g_str_has_prefix(source, "3cx-v1"));
	g_assert_nonnull(strstr(external, "3cx-call-9001"));
	g_assert_cmpint(direction, ==, VENTURE_CALL_DIRECTION_INBOUND);
	g_assert_cmpint(outcome, ==, VENTURE_CALL_OUTCOME_REACHED);
	g_assert_cmpint(number_of(call, "call-duration"), ==, 125);
	g_assert_cmpstr(owner, ==, "alice");
	/* Recording links are off until the organization turns them on. */
	g_assert_true(recording == NULL || *recording == '\0');
	history = rows_of(f, VENTURE_TYPE_INTERACTION, "contact-id", venture_entity_get_id(f->ada));
	g_assert_cmpuint(history->len, ==, 1);

	again = hook_fixture(f, FIXTURE("3cx-journal-inbound.json"));
	g_assert_cmpint(counted(again, "calls"), ==, 0);
	g_assert_cmpint(counted(again, "duplicates"), ==, 1);
	g_clear_pointer(&history, g_ptr_array_unref);
	history = rows_of(f, VENTURE_TYPE_INTERACTION, "contact-id", venture_entity_get_id(f->ada));
	g_assert_cmpuint(history->len, ==, 1);

	/* With recording links on, the link is kept. */
	organization = venture_database_get(f->db, VENTURE_TYPE_ORGANIZATION, 1, &error);
	g_object_set(organization, "telephony-recording-links", TRUE, NULL);
	persist(f, organization);
	journal = fixture_text(FIXTURE("3cx-journal-inbound.json"));
	{
		g_auto(GStrv) parts = g_strsplit(journal, "3cx-call-9001", -1);
		changed = g_strjoinv("3cx-call-9101", parts);
	}
	recorded = deliver_hook(f, f->token, NULL, changed, NULL, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(counted(recorded, "calls"), ==, 1);
	calls = rows_of(f, VENTURE_TYPE_ACTIVITY, "contact-id", venture_entity_get_id(f->ada));
	g_assert_cmpuint(calls->len, ==, 2);
	g_clear_pointer(&recording, g_free);
	g_object_get(g_ptr_array_index(calls, 1), "call-recording-url", &recording, NULL);
	g_assert_cmpstr(recording, ==, "https://pbx.example.com/recordings/9001");
}

/* Inbound numbers are matched after normalising both sides; a missed call
 * is a call-log row with outcome missed, marked for the inbox; a caller
 * nobody knows becomes a contact so the missed call has somewhere to be. */
static void
test_missed_calls(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) line = connect_3cx(f);
	g_autoptr(JsonNode) known = NULL, unknown = NULL, outbound = NULL;
	g_autoptr(VentureEntity) call = NULL, stranger = NULL;
	g_autoptr(GPtrArray) calls = NULL;
	g_autoptr(GError) error = NULL;
	gboolean inbox = FALSE;
	gint outcome = -1, direction = -1;
	(void)data;
	stranger = venture_telephony_service_match_contact(f->service, 1, "1 (555) 010-0123", &error);
	g_assert_no_error(error);
	g_assert_nonnull(stranger);
	g_assert_cmpint(venture_entity_get_id(stranger), ==, venture_entity_get_id(f->ada));
	g_clear_object(&stranger);

	known = hook_fixture(f, FIXTURE("3cx-journal-missed-known.json"));
	g_assert_cmpint(counted(known, "calls"), ==, 1);
	call = only_call(f, f->ada);
	g_object_get(call, "call-outcome", &outcome, "call-inbox-ready", &inbox, "call-direction", &direction, NULL);
	g_assert_cmpint(outcome, ==, VENTURE_CALL_OUTCOME_MISSED);
	g_assert_cmpint(direction, ==, VENTURE_CALL_DIRECTION_INBOUND);
	g_assert_true(inbox);

	unknown = hook_fixture(f, FIXTURE("3cx-journal-missed-unknown.json"));
	g_assert_cmpint(counted(unknown, "calls"), ==, 1);
	stranger = venture_telephony_service_match_contact(f->service, 1, "+15550100177", &error);
	g_assert_nonnull(stranger);
	g_clear_object(&call);
	call = only_call(f, stranger);
	g_object_get(call, "call-outcome", &outcome, "call-inbox-ready", &inbox, NULL);
	g_assert_cmpint(outcome, ==, VENTURE_CALL_OUTCOME_MISSED);
	g_assert_true(inbox);

	/* An outbound call nobody answered is no answer, not an inbox item. */
	outbound = hook_fixture(f, FIXTURE("3cx-journal-outbound.json"));
	g_assert_cmpint(counted(outbound, "calls"), ==, 1);
	calls = rows_of(f, VENTURE_TYPE_ACTIVITY, "contact-id", venture_entity_get_id(f->ada));
	g_assert_cmpuint(calls->len, ==, 2);
	g_object_get(g_ptr_array_index(calls, 1), "call-outcome", &outcome, "call-inbox-ready", &inbox, "call-direction", &direction, NULL);
	g_assert_cmpint(outcome, ==, VENTURE_CALL_OUTCOME_NO_ANSWER);
	g_assert_cmpint(direction, ==, VENTURE_CALL_DIRECTION_OUTBOUND);
	g_assert_false(inbox);
}

/* VoIP.ms sendSMS: a form POST with the API user and password in the body
 * (never the URL), the line's number and the recipient as ten digits. */
static void
test_voipms_send(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) line = connect_voipms(f);
	g_autoptr(VentureEntity) sent = NULL, failed = NULL, picture = NULL, image = NULL;
	g_autoptr(GDateTime) now = at("2026-09-14T15:00:00Z");
	g_autoptr(GError) error = NULL;
	g_autoptr(GHashTable) form = NULL;
	g_autofree gchar *body = NULL, *query = NULL, *state = NULL, *provider = NULL, *why = NULL, *auth = NULL;
	static const guchar png[] = {
		0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
		0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4,
		0x89, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0xf8, 0xcf, 0xc0, 0xf0,
		0x1f, 0x00, 0x05, 0x00, 0x01, 0xff, 0x89, 0x99, 0x3d, 0x1d, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45,
		0x4e, 0x44, 0xae, 0x42, 0x60, 0x82
	};
	(void)data;
	agree(f, f->ada);
	sent = venture_telephony_service_send_text(f->service, f->ada, "Your estimate is ready", 0, now, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(sent);
	g_object_get(sent, "state", &state, "provider-message-id", &provider, NULL);
	g_assert_cmpstr(state, ==, "sent");
	g_assert_cmpstr(provider, ==, "23434");
	g_assert_cmpuint(fake_count(&f->fake, "POST /api/v1/rest.php"), ==, 1);
	body = fake_seen(&f->fake, f->fake.bodies, "/api/v1/rest.php");
	form = soup_form_decode(body);
	g_assert_cmpstr(g_hash_table_lookup(form, "api_username"), ==, "ops@example.com");
	g_assert_cmpstr(g_hash_table_lookup(form, "api_password"), ==, VOIP_SECRET);
	g_assert_cmpstr(g_hash_table_lookup(form, "method"), ==, "sendSMS");
	g_assert_cmpstr(g_hash_table_lookup(form, "did"), ==, "5550100100");
	g_assert_cmpstr(g_hash_table_lookup(form, "dst"), ==, "5550100123");
	g_assert_cmpstr(g_hash_table_lookup(form, "message"), ==, "Your estimate is ready");
	query = fake_seen(&f->fake, f->fake.queries, "/api/v1/rest.php");
	g_assert_null(strstr(query, VOIP_SECRET));
	auth = fake_seen(&f->fake, f->fake.auth, "/api/v1/rest.php");
	g_assert_cmpstr(auth, ==, "");

	/* A picture goes as MMS, by its hosted address. */
	{
		g_autoptr(GBytes) bytes = g_bytes_new_static(png, sizeof png);
		g_autoptr(GHashTable) mms = NULL;
		g_autofree gchar *mms_body = NULL;
		image = venture_hosted_image_store(f->context, 1, "estimate.png", bytes, "Estimate", NULL, &error);
		g_assert_no_error(error);
		fake_set_voip(&f->fake, FIXTURE("voipms-sendmms.json"));
		picture = venture_telephony_service_send_text(f->service, f->ada, "Here it is", venture_entity_get_id(image), now, NULL, &error);
		g_assert_no_error(error);
		mms_body = fake_seen(&f->fake, f->fake.bodies, "/api/v1/rest.php");
		mms = soup_form_decode(mms_body);
		g_assert_cmpstr(g_hash_table_lookup(mms, "method"), ==, "sendMMS");
		g_assert_true(g_str_has_prefix(g_hash_table_lookup(mms, "media1"), "https://venture.example.com/"));
		g_clear_pointer(&provider, g_free);
		g_object_get(picture, "provider-message-id", &provider, NULL);
		g_assert_cmpstr(provider, ==, "88001");
	}

	/* A refusal is the message's failure, in words, and is not retried. */
	fake_set_voip(&f->fake, FIXTURE("voipms-invalid-dst.json"));
	failed = venture_telephony_service_send_text(f->service, f->ada, "Are you there?", 0, now, NULL, &error);
	g_assert_no_error(error);
	g_clear_pointer(&state, g_free);
	g_object_get(failed, "state", &state, "error", &why, NULL);
	g_assert_cmpstr(state, ==, "failed");
	g_assert_nonnull(strstr(why, "not a valid"));
	g_assert_cmpuint(fake_count(&f->fake, "POST /api/v1/rest.php"), ==, 3);
	assert_not_logged(VOIP_SECRET);
}

/* Inbound texts by the VoIP.ms URL callback: matched to the contact,
 * marked for the inbox, kept once however often the callback repeats, and
 * refused for an unknown address token or from a sender not allowed. */
static void
test_voipms_receive(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) line = connect_voipms(f);
	g_autoptr(JsonNode) first = NULL, again = NULL, refused = NULL, picture = NULL;
	g_autoptr(GPtrArray) thread = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *query = fixture_text(FIXTURE("voipms-callback-sms.query"));
	VentureEntity *message;
	gboolean inbox = FALSE;
	gint direction = -1;
	(void)data;
	first = hook_fixture(f, FIXTURE("voipms-callback-sms.query"));
	g_assert_cmpint(counted(first, "messages"), ==, 1);
	again = hook_fixture(f, FIXTURE("voipms-callback-sms.query"));
	g_assert_cmpint(counted(again, "messages"), ==, 0);
	g_assert_cmpint(counted(again, "duplicates"), ==, 1);
	thread = venture_telephony_service_thread(f->service, f->ada, 50, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(thread->len, ==, 1);
	message = g_ptr_array_index(thread, 0);
	g_object_get(message, "inbox-ready", &inbox, "direction", &direction, NULL);
	g_assert_true(inbox);
	g_assert_cmpint(direction, ==, VENTURE_TEXT_DIRECTION_INBOUND);
	{
		g_autofree gchar *body = text_of(message, "body"), *state = text_of(message, "state"), *from = text_of(message, "from-number");
		g_assert_cmpstr(body, ==, "Hello there, is Tuesday fine?");
		g_assert_cmpstr(state, ==, "received");
		g_assert_cmpstr(from, ==, "+15550100123");
	}

	refused = deliver_hook(f, "not-the-token-not-the-token-not-the-token-xx", query, NULL, NULL, NULL, NULL, &error);
	g_assert_null(refused);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_clear_error(&error);
	refused = deliver_hook(f, f->token, query, NULL, NULL, "192.0.2.10", NULL, &error);
	g_assert_null(refused);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);

	/* A picture is fetched from an allowed host and kept as a private
	 * document, not a public hosted image. */
	picture = hook_fixture(f, FIXTURE("voipms-callback-mms.query"));
	g_assert_cmpint(counted(picture, "messages"), ==, 1);
	g_assert_cmpuint(fake_count(&f->fake, "GET /media/7002.jpg"), ==, 1);
	g_clear_pointer(&thread, g_ptr_array_unref);
	thread = venture_telephony_service_thread(f->service, f->ada, 50, &error);
	g_assert_cmpuint(thread->len, ==, 2);
	{
		g_autofree gchar *documents = text_of(g_ptr_array_index(thread, 1), "media-documents");
		g_autoptr(VentureEntity) document = NULL;
		g_autofree gchar *kind = NULL;
		g_assert_nonnull(documents);
		document = venture_database_get(f->db, VENTURE_TYPE_DOCUMENT, g_ascii_strtoll(documents, NULL, 10), &error);
		g_assert_nonnull(document);
		g_object_get(document, "kind", &kind, NULL);
		g_assert_cmpstr(kind, ==, "text-media");
	}
	g_assert_cmpuint(rows_of(f, VENTURE_TYPE_HOSTED_IMAGE, NULL, 0)->len, ==, 0);
}

/* A provider that signs: a forged status is refused, a genuine one marks
 * the text delivered, and the same receipt twice changes nothing. */
static void
test_signed_status(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) line = connect_line(f, "fakesig", "{\"name\":\"Signed\",\"account\":\"fake-1\","
		"\"secret\":\"" FAKE_SECRET "\",\"webhook_secret\":\"" HOOK_SECRET "\",\"phone_number\":\"+15550100199\"}");
	g_autoptr(GDateTime) now = at("2026-09-14T15:00:00Z");
	g_autoptr(VentureEntity) sent = NULL, fresh = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GHashTable) headers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	g_autoptr(JsonNode) forged = NULL, genuine = NULL, again = NULL;
	g_autofree gchar *state = NULL, *signature = NULL;
	const gchar *receipt = "{\"events\":[{\"type\":\"status\",\"id\":\"m-1\",\"status\":\"delivered\"}]}";
	g_autoptr(GDateTime) delivered = NULL;
	(void)data;
	agree(f, f->ada);
	sent = venture_telephony_service_send_text(f->service, f->ada, "See you Tuesday", 0, now, NULL, &error);
	g_assert_no_error(error);
	g_hash_table_replace(headers, g_strdup("x-fake-signature"), g_strdup("00"));
	forged = deliver_hook(f, f->token, NULL, receipt, headers, NULL, NULL, &error);
	g_assert_null(forged);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);
	signature = g_compute_hmac_for_string(G_CHECKSUM_SHA256, (const guchar *)HOOK_SECRET, strlen(HOOK_SECRET), receipt, -1);
	g_hash_table_replace(headers, g_strdup("x-fake-signature"), g_strdup(signature));
	genuine = deliver_hook(f, f->token, NULL, receipt, headers, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(counted(genuine, "statuses"), ==, 1);
	fresh = reread(f, sent);
	g_object_get(fresh, "state", &state, "delivered-at", &delivered, NULL);
	g_assert_cmpstr(state, ==, "delivered");
	g_assert_nonnull(delivered);
	again = deliver_hook(f, f->token, NULL, receipt, headers, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(counted(again, "statuses"), ==, 0);
	g_assert_cmpint(counted(again, "duplicates"), ==, 1);
}

static guint
replies_with(Fixture *f, const gchar *words)
{
	g_autoptr(GPtrArray) texts = rows_of(f, VENTURE_TYPE_TEXT_MESSAGE, "contact-id", venture_entity_get_id(f->ada));
	guint i, n = 0;
	for (i = 0; i < texts->len; i++)
	{
		g_autofree gchar *body = text_of(g_ptr_array_index(texts, i), "body");
		gboolean automatic = FALSE;
		g_object_get(g_ptr_array_index(texts, i), "auto-reply", &automatic, NULL);
		if (automatic && strstr(body, words)) n++;
	}
	return n;
}

static guint
text_consent_rows(Fixture *f)
{
	g_autoptr(GPtrArray) rows = rows_of(f, VENTURE_TYPE_MARKETING_CONSENT, NULL, 0);
	guint i, n = 0;
	for (i = 0; i < rows->len; i++)
	{
		g_autofree gchar *purpose = text_of(g_ptr_array_index(rows, i), "purpose");
		g_autofree gchar *phone = text_of(g_ptr_array_index(rows, i), "phone");
		if (g_strcmp0(purpose, "text") == 0 && g_strcmp0(phone, "+15550100123") == 0) n++;
	}
	return n;
}

/* STOP and its synonyms withdraw text consent as retained evidence and are
 * answered once; HELP is answered; START or UNSTOP agrees again. A send
 * after STOP is refused. */
static void
test_keywords(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) line = connect_voipms(f);
	g_autoptr(JsonNode) stop = NULL, again = NULL, synonym = NULL, help = NULL, start = NULL;
	g_autoptr(VentureEntity) refused = NULL, allowed = NULL;
	g_autoptr(GDateTime) now = at("2026-09-14T16:00:00Z");
	g_autoptr(GError) error = NULL;
	(void)data;
	agree(f, f->ada);
	g_assert_cmpuint(text_consent_rows(f), ==, 1);
	stop = hook_fixture(f, FIXTURE("voipms-callback-stop.query"));
	g_assert_cmpint(counted(stop, "messages"), ==, 1);
	g_assert_false(venture_telephony_service_has_consent(f->service, f->ada, NULL));
	g_assert_cmpuint(text_consent_rows(f), ==, 2);
	g_assert_cmpuint(replies_with(f, "STOP"), ==, 0);
	g_assert_cmpuint(replies_with(f, "unsubscribed"), ==, 1);
	g_assert_cmpuint(fake_count(&f->fake, "POST /api/v1/rest.php"), ==, 1);
	again = hook_fixture(f, FIXTURE("voipms-callback-stop.query"));
	g_assert_cmpint(counted(again, "duplicates"), ==, 1);
	synonym = hook_fixture(f, FIXTURE("voipms-callback-unsubscribe.query"));
	g_assert_cmpint(counted(synonym, "messages"), ==, 1);
	/* Already out: no second confirmation and no second withdrawal. */
	g_assert_cmpuint(replies_with(f, "unsubscribed"), ==, 1);
	g_assert_cmpuint(fake_count(&f->fake, "POST /api/v1/rest.php"), ==, 1);

	refused = venture_telephony_service_send_text(f->service, f->ada, "One more offer", 0, now, NULL, &error);
	g_assert_null(refused);
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "agreed"));
	g_clear_error(&error);
	g_assert_cmpuint(fake_count(&f->fake, "POST /api/v1/rest.php"), ==, 1);

	help = hook_fixture(f, FIXTURE("voipms-callback-help.query"));
	g_assert_cmpint(counted(help, "messages"), ==, 1);
	g_assert_cmpuint(replies_with(f, "STOP"), >=, 1);
	g_assert_cmpuint(fake_count(&f->fake, "POST /api/v1/rest.php"), ==, 2);

	start = hook_fixture(f, FIXTURE("voipms-callback-start.query"));
	g_assert_cmpint(counted(start, "messages"), ==, 1);
	g_assert_true(venture_telephony_service_has_consent(f->service, f->ada, NULL));
	g_assert_cmpuint(fake_count(&f->fake, "POST /api/v1/rest.php"), ==, 3);
	allowed = venture_telephony_service_send_text(f->service, f->ada, "Welcome back", 0, now, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(allowed);
	g_assert_cmpuint(fake_count(&f->fake, "POST /api/v1/rest.php"), ==, 4);
}

/* Without text consent nothing is queued and nothing reaches a provider. */
static void
test_consent_refusal(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) line = connect_voipms(f);
	g_autoptr(GDateTime) now = at("2026-09-14T15:00:00Z");
	g_autoptr(VentureEntity) refused = NULL, queued = NULL;
	g_autoptr(GError) error = NULL;
	(void)data;
	refused = venture_telephony_service_send_text(f->service, f->ada, "Hello", 0, now, NULL, &error);
	g_assert_null(refused);
	g_assert_nonnull(strstr(error->message, "agreed"));
	g_clear_error(&error);
	queued = venture_telephony_service_queue_text(f->service, f->ada, "Hello", NULL, "manual", now, NULL, &error);
	g_assert_null(queued);
	g_assert_nonnull(error);
	g_assert_cmpuint(rows_of(f, VENTURE_TYPE_TEXT_MESSAGE, NULL, 0)->len, ==, 0);
	g_assert_cmpuint(f->fake.hits->len, ==, 0);
}

static gchar *
iso(GDateTime *value)
{
	g_autoptr(GDateTime) utc = value != NULL ? g_date_time_to_utc(value) : NULL;
	return utc != NULL ? g_date_time_format(utc, "%Y-%m-%dT%H:%M:%SZ") : NULL;
}

/* Quiet hours, 21:00 to 08:00 in the recipient's own time zone (the
 * contact's, else the organization's), hold a text until they end; the
 * send path checks again, so a held text cannot go early. */
static void
test_quiet_hours(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) line = connect_voipms(f);
	g_autoptr(VentureEntity) west = person(f, "West Coast", "+1 555 010 0131", "America/Los_Angeles");
	g_autoptr(VentureEntity) berlin = person(f, "Berlin", "+1 555 010 0132", "Europe/Berlin");
	g_autoptr(VentureEntity) tokyo = person(f, "Tokyo", "+1 555 010 0133", "Asia/Tokyo");
	g_autoptr(VentureEntity) zoneless = person(f, "Zoneless", "+1 555 010 0134", NULL);
	g_autoptr(VentureEntity) organization = NULL, held = NULL, fresh = NULL;
	g_autoptr(GDateTime) now = at("2026-09-14T05:00:00Z");
	g_autoptr(GDateTime) early = at("2026-09-14T14:00:00Z");
	g_autoptr(GDateTime) open = at("2026-09-14T15:00:00Z");
	g_autoptr(GError) error = NULL;
	g_autofree gchar *state = NULL;
	(void)data;
	organization = venture_database_get(f->db, VENTURE_TYPE_ORGANIZATION, 1, &error);
	g_object_set(organization, "timezone", "America/New_York", NULL);
	persist(f, organization);
	{
		g_autoptr(GDateTime) a = venture_telephony_service_quiet_until(f->service, west, now, NULL);
		g_autoptr(GDateTime) b = venture_telephony_service_quiet_until(f->service, berlin, now, NULL);
		g_autoptr(GDateTime) c = venture_telephony_service_quiet_until(f->service, tokyo, now, NULL);
		g_autoptr(GDateTime) d = venture_telephony_service_quiet_until(f->service, zoneless, now, NULL);
		g_autofree gchar *sa = iso(a), *sb = iso(b), *sd = iso(d);
		g_assert_cmpstr(sa, ==, "2026-09-14T15:00:00Z");	/* 08:00 PDT */
		g_assert_cmpstr(sb, ==, "2026-09-14T06:00:00Z");	/* 08:00 CEST */
		g_assert_null(c);					/* 14:00 JST */
		g_assert_cmpstr(sd, ==, "2026-09-14T12:00:00Z");	/* 08:00 EDT, the organization's zone */
	}
	agree(f, west);
	held = venture_telephony_service_send_text(f->service, west, "Morning reminder", 0, now, NULL, &error);
	g_assert_no_error(error);
	g_object_get(held, "state", &state, NULL);
	g_assert_cmpstr(state, ==, "held");
	{
		g_autoptr(GDateTime) until = NULL;
		g_autofree gchar *text = NULL;
		g_object_get(held, "hold-until", &until, NULL);
		text = iso(until);
		g_assert_cmpstr(text, ==, "2026-09-14T15:00:00Z");
	}
	g_assert_cmpint(venture_telephony_service_deliver_due(f->service, 1, early, 10, NULL, &error), ==, 0);
	g_assert_no_error(error);
	g_assert_cmpuint(f->fake.hits->len, ==, 0);
	g_assert_cmpint(venture_telephony_service_deliver_due(f->service, 1, open, 10, NULL, &error), ==, 1);
	fresh = reread(f, held);
	g_clear_pointer(&state, g_free);
	g_object_get(fresh, "state", &state, NULL);
	g_assert_cmpstr(state, ==, "sent");

	/* An organization may turn quiet hours off. */
	g_object_set(organization, "telephony-quiet-hours", "off", NULL);
	g_clear_object(&organization);
	organization = venture_database_get(f->db, VENTURE_TYPE_ORGANIZATION, 1, &error);
	g_object_set(organization, "telephony-quiet-hours", "off", NULL);
	persist(f, organization);
	{
		g_autoptr(GDateTime) none = venture_telephony_service_quiet_until(f->service, west, now, NULL);
		g_assert_null(none);
	}
}

/* A text step in a journey goes through lane 4's channel hook: it queues a
 * real text through the one send path, and a contact without text consent
 * is skipped with the reason, the journey carrying on. */
static void
test_sequence_text_step(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) line = connect_voipms(f);
	g_autoptr(VentureEntity) silent = person(f, "Silent", "+1 555 010 0141", "UTC");
	VentureSequenceService *sequences = venture_sequence_service_get(f->db);
	g_autoptr(VentureEntity) journey = NULL, step = NULL, first = NULL, second = NULL;
	g_autoptr(GDateTime) when = at("2026-09-14T15:00:00Z");
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) deliveries = NULL, texts = NULL;
	guint i;
	gint sent_state = -1, skipped_state = -1;
	g_autofree gchar *external = NULL, *reason = NULL;
	(void)data;
	agree(f, f->ada);
	journey = g_object_new(VENTURE_TYPE_SEQUENCE, "organization-id", (gint64)1, "name", "Appointment reminder",
		"active", TRUE, "timezone", "UTC", "send-window-start", (gint64)0, "send-window-end", (gint64)24,
		"weekdays", "1,2,3,4,5,6,7", "exit-on-reply", FALSE, NULL);
	persist(f, journey);
	step = g_object_new(VENTURE_TYPE_SEQUENCE_STEP, "organization-id", (gint64)1, "sequence-id", venture_entity_get_id(journey),
		"position", (gint64)1, "channel", 5, "active", TRUE, "subject", "Reminder", "body", "Hi {contact.name}, see you Tuesday", NULL);
	persist(f, step);
	first = g_object_new(VENTURE_TYPE_SEQUENCE_ENROLLMENT, "organization-id", (gint64)1, "sequence-id", venture_entity_get_id(journey),
		"contact-id", venture_entity_get_id(f->ada), "enrolled-at", when, "enrollment-reason", "Test", NULL);
	g_assert_true(venture_sequence_service_enroll(sequences, first, NULL, &error));
	g_assert_no_error(error);
	second = g_object_new(VENTURE_TYPE_SEQUENCE_ENROLLMENT, "organization-id", (gint64)1, "sequence-id", venture_entity_get_id(journey),
		"contact-id", venture_entity_get_id(silent), "enrolled-at", when, "enrollment-reason", "Test", NULL);
	g_assert_true(venture_sequence_service_enroll(sequences, second, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpint(venture_sequence_service_run_due(sequences, 1, when, NULL, &error), >=, 2);
	g_assert_no_error(error);
	/* The step queued; the send path sends. */
	g_assert_cmpuint(f->fake.hits->len, ==, 0);
	g_assert_cmpint(venture_telephony_service_deliver_due(f->service, 1, when, 10, NULL, &error), ==, 1);
	texts = rows_of(f, VENTURE_TYPE_TEXT_MESSAGE, "contact-id", venture_entity_get_id(f->ada));
	g_assert_cmpuint(texts->len, ==, 1);
	{
		g_autofree gchar *body = text_of(g_ptr_array_index(texts, 0), "body"), *state = text_of(g_ptr_array_index(texts, 0), "state");
		g_autofree gchar *origin = text_of(g_ptr_array_index(texts, 0), "origin");
		g_assert_cmpstr(body, ==, "Hi Ada, see you Tuesday");
		g_assert_cmpstr(state, ==, "sent");
		g_assert_true(g_str_has_prefix(origin, "sequence:"));
	}
	deliveries = rows_of(f, VENTURE_TYPE_SEQUENCE_DELIVERY, "step-id", venture_entity_get_id(step));
	g_assert_cmpuint(deliveries->len, ==, 2);
	for (i = 0; i < deliveries->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(deliveries, i);
		gint state;
		g_object_get(row, "state", &state, NULL);
		if (number_of(row, "enrollment-id") == venture_entity_get_id(first))
		{
			sent_state = state;
			external = text_of(row, "external-message-id");
		}
		else
		{
			skipped_state = state;
			reason = text_of(row, "error");
		}
	}
	g_assert_cmpint(sent_state, ==, 1);
	g_assert_cmpstr(external, ==, venture_entity_get_uuid(g_ptr_array_index(texts, 0)));
	g_assert_cmpint(skipped_state, ==, 3);
	g_assert_nonnull(strstr(reason, "agreed"));
}

/* A text campaign is an ordinary marketing send with channel text: the
 * preview leaves out whoever has no text consent, the run queues one
 * recipient at a time, and quiet hours hold what the run queued at night. */
static void
test_text_campaign(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) line = connect_voipms(f);
	g_autoptr(VentureEntity) silent = person(f, "Silent", "+1 555 010 0141", "UTC");
	VentureMarketingService *marketing = venture_marketing_service_get(f->db);
	g_autoptr(VentureEntity) list = NULL, send = NULL, a = NULL, b = NULL;
	g_autoptr(VentureMarketingSend) previewed = NULL;
	/* After the approval, which is stamped by the real clock. */
	g_autoptr(GDateTime) night = at("2030-01-15T03:00:00Z");
	g_autoptr(GDateTime) morning = at("2030-01-15T09:00:00Z");
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) texts = NULL;
	g_autofree gchar *state = NULL, *origin = NULL, *body = NULL;
	(void)data;
	agree(f, f->ada);
	list = g_object_new(VENTURE_TYPE_MARKETING_LIST, "organization-id", (gint64)1, "name", "Texters", NULL);
	persist(f, list);
	a = g_object_new(VENTURE_TYPE_MARKETING_MEMBER, "organization-id", (gint64)1, "list-id", venture_entity_get_id(list),
		"contact-id", venture_entity_get_id(f->ada), NULL);
	persist(f, a);
	b = g_object_new(VENTURE_TYPE_MARKETING_MEMBER, "organization-id", (gint64)1, "list-id", venture_entity_get_id(list),
		"contact-id", venture_entity_get_id(silent), NULL);
	persist(f, b);
	send = g_object_new(VENTURE_TYPE_MARKETING_SEND, "organization-id", (gint64)1, "name", "Autumn hours",
		"list-id", venture_entity_get_id(list), "subject", "Autumn hours", "text-body", "Hi {name}, we open at 9 from Monday.",
		"interval-seconds", (gint64)1, "channel", 1, NULL);
	persist(f, send);
	previewed = venture_marketing_service_preview(marketing, 1, venture_entity_get_id(send), "https://venture.example.com", NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(previewed);
	g_assert_cmpint(number_of(previewed, "eligible-count"), ==, 1);
	g_assert_cmpint(number_of(previewed, "excluded-count"), ==, 1);
	g_assert_true(venture_marketing_service_transition(marketing, 1, venture_entity_get_id(send), "approve", NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpint(venture_marketing_service_run(marketing, 1, venture_entity_get_id(send), 100, night, NULL, &error), ==, 1);
	g_assert_no_error(error);
	texts = rows_of(f, VENTURE_TYPE_TEXT_MESSAGE, NULL, 0);
	g_assert_cmpuint(texts->len, ==, 1);
	g_object_get(g_ptr_array_index(texts, 0), "state", &state, "origin", &origin, "body", &body, NULL);
	g_assert_cmpstr(state, ==, "held");
	g_assert_true(g_str_has_prefix(origin, "campaign:"));
	g_assert_nonnull(strstr(body, "Hi Ada, we open at 9 from Monday."));
	g_assert_nonnull(strstr(body, "STOP"));
	g_assert_cmpint(venture_telephony_service_deliver_due(f->service, 1, night, 10, NULL, &error), ==, 0);
	g_assert_cmpint(venture_telephony_service_deliver_due(f->service, 1, morning, 10, NULL, &error), ==, 1);
	g_assert_cmpuint(fake_count(&f->fake, "POST /api/v1/rest.php"), ==, 1);
	/* Nobody is texted twice: the next run finds no one left. */
	g_assert_cmpint(venture_marketing_service_run(marketing, 1, venture_entity_get_id(send), 100, morning, NULL, &error), ==, 0);
}

/* No secret in any record, error or log line: the credentials are only in
 * the vault's ciphertext, and the webhook token only as a hash. */
static void
test_no_secrets(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) pbx = connect_3cx(f);
	g_autofree gchar *pbx_token = g_strdup(f->token);
	g_autoptr(VentureEntity) carrier = connect_voipms(f);
	g_autoptr(GPtrArray) lines = rows_of(f, VENTURE_TYPE_TELEPHONY_ACCOUNT, NULL, 0);
	g_autoptr(GPtrArray) bindings = rows_of(f, VENTURE_TYPE_INTEGRATION_CONNECTION, NULL, 0);
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) now = at("2026-09-14T15:00:00Z");
	g_autoptr(VentureEntity) failed = NULL;
	g_autofree gchar *why = NULL;
	guint i;
	(void)data;
	g_assert_cmpuint(lines->len, ==, 2);
	g_assert_cmpuint(bindings->len, ==, 2);
	for (i = 0; i < lines->len + bindings->len; i++)
	{
		VentureEntity *row = i < lines->len ? g_ptr_array_index(lines, i) : g_ptr_array_index(bindings, i - lines->len);
		g_autoptr(JsonNode) json = venture_serializable_to_json(VENTURE_SERIALIZABLE(row), FALSE);
		g_autofree gchar *text = venture_json_to_string(json, FALSE);
		g_assert_null(strstr(text, PBX_SECRET));
		g_assert_null(strstr(text, VOIP_SECRET));
		g_assert_null(strstr(text, pbx_token));
		g_assert_null(strstr(text, f->token));
	}
	agree(f, f->ada);
	fake_set_voip(&f->fake, FIXTURE("voipms-invalid-credentials.json"));
	failed = venture_telephony_service_send_text(f->service, f->ada, "Hello", 0, now, NULL, &error);
	g_assert_no_error(error);
	g_object_get(failed, "error", &why, NULL);
	g_assert_nonnull(strstr(why, "password"));
	g_assert_null(strstr(why, VOIP_SECRET));
	assert_not_logged(PBX_SECRET);
	assert_not_logged(VOIP_SECRET);
	assert_not_logged(f->token);
	assert_not_logged(pbx_token);
	/* Only the service writes a line: the generic path cannot make one. */
	{
		g_autoptr(VentureEntity) forged = g_object_new(VENTURE_TYPE_TELEPHONY_ACCOUNT, "organization-id", (gint64)1,
			"name", "Forged", "provider", "voipms", "active", TRUE, NULL);
		g_assert_false(venture_database_save(f->db, forged, NULL, &error));
		g_assert_nonnull(error);
	}
}

int
main(int argc, char **argv)
{
	logged = g_string_new(NULL);
	g_log_set_writer_func(capture_log, NULL, NULL);
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/telephony/registry", test_registry);
	g_test_add_func("/telephony/normalise", test_normalise);
	g_test_add("/telephony/3cx/click-to-call", Fixture, NULL, setup, test_click_to_call, teardown);
	g_test_add("/telephony/3cx/refusals", Fixture, NULL, setup, test_click_to_call_refusals, teardown);
	g_test_add("/telephony/3cx/errors", Fixture, NULL, setup, test_3cx_errors, teardown);
	g_test_add("/telephony/calls/logging", Fixture, NULL, setup, test_call_logging, teardown);
	g_test_add("/telephony/calls/missed", Fixture, NULL, setup, test_missed_calls, teardown);
	g_test_add("/telephony/voipms/send", Fixture, NULL, setup, test_voipms_send, teardown);
	g_test_add("/telephony/voipms/receive", Fixture, NULL, setup, test_voipms_receive, teardown);
	g_test_add("/telephony/webhook/signed-status", Fixture, NULL, setup, test_signed_status, teardown);
	g_test_add("/telephony/texts/keywords", Fixture, NULL, setup, test_keywords, teardown);
	g_test_add("/telephony/texts/consent-refusal", Fixture, NULL, setup, test_consent_refusal, teardown);
	g_test_add("/telephony/texts/quiet-hours", Fixture, NULL, setup, test_quiet_hours, teardown);
	g_test_add("/telephony/sequences/text-step", Fixture, NULL, setup, test_sequence_text_step, teardown);
	g_test_add("/telephony/marketing/text-campaign", Fixture, NULL, setup, test_text_campaign, teardown);
	g_test_add("/telephony/no-secrets", Fixture, NULL, setup, test_no_secrets, teardown);
	return g_test_run();
}
