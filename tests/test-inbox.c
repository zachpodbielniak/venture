/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* One inbox (docs/inbox.org): a conversation per contact, read from the
 * channels' own records -- mail interactions from the sync, call
 * activities from log_call, text messages from the telephony service and
 * web chat messages -- with an owner, a status and replies that go back
 * out through each channel's one send path. The text line is a provider
 * defined in this file and registered the way a real one is; it answers
 * every send itself, so nothing leaves the machine. Mail is the fake IMAP
 * client and the outbox's log mailer. No number, address or person is
 * real. */
#include <venture.h>
#include <string.h>
#include <libsoup/soup.h>
#include "venture-test-util.h"
#include "venture-test-accounting.h"

/* --- A text provider that never touches the network ------------------- */

static gint fake_sends;

#define INBOX_TYPE_FAKE_LINE (inbox_fake_line_get_type())
G_DECLARE_FINAL_TYPE(InboxFakeLine, inbox_fake_line, INBOX, FAKE_LINE, GObject)
struct _InboxFakeLine { GObject parent_instance; };
static void inbox_fake_line_iface_init(VentureTelephonyConnectorInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE(InboxFakeLine, inbox_fake_line, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(VENTURE_TYPE_TELEPHONY_CONNECTOR, inbox_fake_line_iface_init))
static void inbox_fake_line_class_init(InboxFakeLineClass *klass) { (void)klass; }
static void inbox_fake_line_init(InboxFakeLine *self) { (void)self; }
static const gchar *line_id(VentureTelephonyConnector *self) { (void)self; return "inboxfake"; }
static const gchar *line_label(VentureTelephonyConnector *self) { (void)self; return "Inbox test line"; }
static VentureTelephonyCapabilities line_capabilities(VentureTelephonyConnector *self)
{
	(void)self;
	return VENTURE_TELEPHONY_CAPABILITY_SMS | VENTURE_TELEPHONY_CAPABILITY_CALL_EVENTS;
}
static JsonNode *line_schema(VentureTelephonyConnector *self)
{
	(void)self;
	return json_from_string("{\"type\":\"object\",\"properties\":{"
		"\"account\":{\"type\":\"string\",\"title\":\"Account\"},"
		"\"secret\":{\"type\":\"string\",\"title\":\"Key\",\"x-sensitive\":true}}}", NULL);
}
static gchar *line_identity(VentureTelephonyConnector *self, JsonObject *settings, GError **error)
{
	(void)self; (void)error;
	return g_strdup(json_object_has_member(settings, "account") ? json_object_get_string_member(settings, "account") : "test");
}
static gboolean line_send(VentureTelephonyConnector *self, VentureTelephonyLink *link,
	const VentureTelephonyOutbound *message, gchar **provider_id, GError **error)
{
	(void)self; (void)link; (void)message; (void)error;
	fake_sends++;
	*provider_id = g_strdup_printf("sent-%d", fake_sends);
	return TRUE;
}
/* {"events":[{"id":"t1","from":"+15550100123","body":"hi"}]} -- texts only. */
static GPtrArray *line_parse(VentureTelephonyConnector *self, VentureTelephonyLink *link,
	const VentureTelephonyWebhook *hook, GError **error)
{
	g_autofree gchar *text = g_strndup(g_bytes_get_data(hook->body, NULL), g_bytes_get_size(hook->body));
	g_autoptr(JsonNode) root = json_from_string(text, error);
	GPtrArray *events = g_ptr_array_new_with_free_func((GDestroyNotify)venture_telephony_event_free);
	JsonArray *list;
	guint i;
	(void)self; (void)link;
	if (root == NULL) { g_ptr_array_unref(events); return NULL; }
	list = json_object_get_array_member(json_node_get_object(root), "events");
	for (i = 0; i < json_array_get_length(list); i++)
	{
		JsonObject *item = json_array_get_object_element(list, i);
		VentureTelephonyEvent *event = venture_telephony_event_new(VENTURE_TELEPHONY_EVENT_MESSAGE);
		event->id = g_strdup(json_object_get_string_member(item, "id"));
		event->from = g_strdup(json_object_get_string_member(item, "from"));
		event->to = g_strdup("+15550100100");
		event->inbound = TRUE;
		event->body = g_strdup(json_object_get_string_member(item, "body"));
		g_ptr_array_add(events, event);
	}
	return events;
}
static void
inbox_fake_line_iface_init(VentureTelephonyConnectorInterface *iface)
{
	iface->get_id = line_id;
	iface->get_label = line_label;
	iface->get_capabilities = line_capabilities;
	iface->settings_schema = line_schema;
	iface->account_identity = line_identity;
	iface->send_message = line_send;
	iface->parse_webhook = line_parse;
}
VENTURE_TELEPHONY_PROVIDER(inbox_fake_line_get_type)

/* --- A channel defined in a test: one file, one registration ----------- */

static gboolean fax_enabled;

static gboolean
fax_list(VentureContext *context, VentureEntity *contact, GDateTime *since, guint limit, GPtrArray *items, GError **error)
{
	g_autoptr(GDateTime) when = NULL;
	(void)context; (void)since; (void)limit; (void)error;
	if (!fax_enabled)
		return TRUE;
	when = g_date_time_new_from_iso8601("2026-09-15T08:30:00Z", NULL);
	g_ptr_array_add(items, venture_inbox_item_new(venture_inbox_source_lookup("fax"), contact, when, TRUE));
	return TRUE;
}

static void
fax_render(VentureContext *context, const VentureInboxItem *item, GString *html)
{
	(void)context; (void)item;
	g_string_append(html, "<p>A fax arrived</p>");
}

static gchar *
fax_summary(VentureContext *context, const VentureInboxItem *item)
{
	(void)context; (void)item;
	return g_strdup("A fax arrived");
}

static const VentureInboxSource fax_source = {
	"fax", "Fax", NULL, fax_list, NULL, fax_render, fax_summary, NULL, NULL
};
VENTURE_INBOX_SOURCE(fax_source)

/* --- The fixture ------------------------------------------------------ */

static const gchar *mail_question =
	"From: Ada <Ada@Example.com>\r\nTo: ops@example.net\r\nSubject: Spring order\r\n"
	"Date: Tue, 15 Sep 2026 09:00:00 +0000\r\nMessage-ID: <q1@example.net>\r\n"
	"Content-Type: text/plain\r\n\r\nCan you deliver in March?\r\n";

static const gchar *mail_followup =
	"From: Ada <ada@example.com>\r\nTo: ops@example.net\r\nSubject: Re: Spring order\r\n"
	"Date: Tue, 15 Sep 2026 13:00:00 +0000\r\nMessage-ID: <q2@example.net>\r\n"
	"In-Reply-To: <q1@example.net>\r\nReferences: <q1@example.net>\r\n"
	"Content-Type: text/plain\r\n\r\nAnd the invoice address is unchanged.\r\n";

typedef struct {
	VentureDatabase *db;
	VentureConfig *config;
	VentureContext *context;
	VentureInboxService *inbox;
	VentureTelephonyService *phone;
	VentureFakeImapClient *imap;
	VentureMailSyncService *sync;
	VentureEntity *mailbox;
	VentureEntity *ada;
	VentureEntity *bob;
	VentureEntity *carol;
	VentureEntity *stranger;
	gchar *line_token;
	gchar *root;
	gint64 org;
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

static gint
enum_of(gpointer row, const gchar *field)
{
	gint value = 0;
	g_object_get(row, field, &value, NULL);
	return value;
}

static VentureActor
person(const gchar *name)
{
	VentureActor actor;
	actor.kind = VENTURE_ACTOR_KIND_USER;
	actor.name = name;
	actor.prompt = NULL;
	actor.request_id = NULL;
	actor.approved_by = NULL;
	return actor;
}

static VentureEntity *
member(Fixture *f, const gchar *username, gint64 org)
{
	VentureEntity *user = g_object_new(VENTURE_TYPE_USER, "username", username,
		"role", VENTURE_USER_ROLE_EDITOR, "active", TRUE, NULL);
	persist(f, user);
	if (org > 0)
	{
		g_autoptr(VentureEntity) membership = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP, "organization-id", org,
			"user-id", venture_entity_get_id(user), "role", VENTURE_ORGANIZATION_ROLE_EDITOR, "active", TRUE, NULL);
		persist(f, membership);
	}
	return user;
}

static void
setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) key = g_bytes_new_static("01234567890123456789012345678901", 32);
	g_autoptr(JsonObject) password = json_object_new();
	g_autoptr(JsonNode) settings = NULL;
	g_autoptr(VentureIntegrationConnection) binding = NULL;
	g_autoptr(VentureEntity) line = NULL;
	(void)data;
	fake_sends = 0;
	fax_enabled = FALSE;
	f->root = g_dir_make_tmp("venture-inbox-XXXXXX", &error);
	g_assert_no_error(error);
	f->config = venture_config_new();
	g_object_set(f->config, "state-dir", f->root, "connectors-allow-plaintext-loopback", TRUE,
		"server-base-url", "https://venture.example.com", "imap-allowed-endpoints", "imap.example.net:993",
		"mail-from-address", "ops@example.net", NULL);
	f->db = venture_test_accounting_database(&error);
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	g_assert_true(venture_integration_service_set_key(venture_integration_service_get(f->db), key, &error));
	f->org = venture_context_get_default_organization_id(f->context);
	f->inbox = venture_inbox_service_get(f->db);
	f->phone = venture_telephony_service_get(f->db);
	f->ada = g_object_new(VENTURE_TYPE_CONTACT, "organization-id", f->org, "name", "Ada Example",
		"email", "ada@example.com", "phone", "+1 555 010 0123", "timezone", "UTC", NULL);
	persist(f, f->ada);
	f->bob = member(f, "bob", f->org);
	f->carol = member(f, "carol", f->org);
	f->stranger = member(f, "stranger", 0);
	/* The mailbox the sync files Ada's mail from. */
	f->mailbox = g_object_new(VENTURE_TYPE_MAIL_ACCOUNT, "organization-id", f->org,
		"address", "ops@example.net", "imap-host", "imap.example.net", "imap-port", (gint64)993, "imap-tls", "tls",
		"username", "ops@example.net", "folders", "INBOX", "active", TRUE, NULL);
	persist(f, f->mailbox);
	json_object_set_string_member(password, "password", "app-password");
	binding = venture_connector_configure(f->db, f->config, f->mailbox, password, 0, 0, NULL, &error);
	g_assert_no_error(error);
	f->imap = venture_fake_imap_client_new();
	f->sync = venture_mail_sync_service_new(f->db, VENTURE_IMAP_CLIENT(f->imap));
	g_object_set(f->sync, "attachment-root", f->root, "config", f->config, NULL);
	/* The texting line. */
	settings = json_from_string("{\"name\":\"Texts\",\"account\":\"ops\",\"secret\":\"line-secret-0001\","
		"\"base_url\":\"http://127.0.0.1:9\",\"phone_number\":\"+1 555 010 0100\"}", &error);
	g_assert_no_error(error);
	line = venture_telephony_service_connect(f->phone, f->org, "inboxfake", json_node_get_object(settings), 0, NULL, &f->line_token, &error);
	g_assert_no_error(error);
	g_assert_nonnull(line);
}

static void
teardown(Fixture *f, gconstpointer data)
{
	(void)data;
	g_clear_object(&f->ada);
	g_clear_object(&f->bob);
	g_clear_object(&f->carol);
	g_clear_object(&f->stranger);
	g_clear_object(&f->mailbox);
	g_clear_object(&f->sync);
	g_clear_object(&f->imap);
	g_clear_pointer(&f->line_token, g_free);
	g_clear_object(&f->context);
	venture_test_accounting_database_cleanup(f->db);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
	venture_test_remove_tree(f->root);
	g_clear_pointer(&f->root, g_free);
}

/* --- Putting messages in, each through its channel's own path ----------- */

static guint32 next_uid = 1;

static void
mail_in(Fixture *f, const gchar *raw)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) fresh = reread(f, f->mailbox);
	venture_fake_imap_client_add_message(f->imap, "INBOX", next_uid++, raw);
	g_assert_cmpint(venture_mail_sync_service_sync(f->sync, fresh, NULL, &error), >=, 1);
	g_assert_no_error(error);
}

static void
text_in(Fixture *f, const gchar *id, const gchar *from, const gchar *body, const gchar *when)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *json = g_strdup_printf("{\"events\":[{\"id\":\"%s\",\"from\":\"%s\",\"body\":\"%s\"}]}", id, from, body);
	g_autoptr(GBytes) bytes = g_bytes_new(json, strlen(json));
	g_autoptr(GDateTime) now = at(when);
	g_autoptr(JsonNode) result = NULL;
	g_autofree gchar *reply = NULL;
	VentureTelephonyWebhook hook;
	hook.method = "POST";
	hook.remote_address = "127.0.0.1";
	hook.query = NULL;
	hook.headers = NULL;
	hook.content_type = "application/json";
	hook.body = bytes;
	result = venture_telephony_service_receive(f->phone, f->line_token, &hook, now, &reply, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
}

static VentureEntity *
call_in(Fixture *f, VentureEntity *contact, const gchar *outcome, const gchar *when, const gchar *transcript)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonObject) details = json_object_new();
	VentureEntity *logged;
	json_object_set_string_member(details, "subject", g_strcmp0(outcome, "missed") == 0 ? "Missed call" : "Voicemail");
	json_object_set_string_member(details, "owner", "Unassigned");
	json_object_set_string_member(details, "call_direction", "inbound");
	json_object_set_int_member(details, "call_duration", 0);
	json_object_set_string_member(details, "call_occurred_at", when);
	json_object_set_string_member(details, "call_outcome", outcome);
	json_object_set_boolean_member(details, "call_inbox_ready", TRUE);
	if (transcript != NULL)
	{
		json_object_set_string_member(details, "call_transcript", transcript);
		json_object_set_string_member(details, "call_recording_url", "https://pbx.example.com/recordings/7001");
	}
	logged = venture_activity_service_log_call(venture_database_get_activity_service(f->db), contact, details, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(logged);
	return logged;
}

static VentureEntity *
widget(Fixture *f, gint64 org)
{
	g_autoptr(GError) error = NULL;
	VentureActor actor = person("owner");
	VentureEntity *row = venture_inbox_chat_widget_create(f->inbox, org, "Website chat", &actor, &error);
	g_assert_no_error(error);
	g_assert_nonnull(row);
	return row;
}

static VentureEntity *
chat_in(Fixture *f, VentureEntity *chat, const gchar *name, const gchar *email, const gchar *phone,
	const gchar *body, const gchar *remote, const gchar *when, gchar **visitor_token)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) now = at(when);
	g_autofree gchar *token = text_of(chat, "token");
	VentureInboxChatIntake intake;
	VentureEntity *session;
	intake.name = name;
	intake.email = email;
	intake.phone = phone;
	intake.body = body;
	intake.trap = NULL;
	intake.remote = remote;
	session = venture_inbox_chat_start(f->inbox, token, &intake, now, visitor_token, &error);
	g_assert_no_error(error);
	g_assert_nonnull(session);
	return session;
}

static VentureEntity *
conversation(Fixture *f, VentureEntity *contact)
{
	g_autoptr(GError) error = NULL;
	VentureEntity *row = venture_inbox_service_conversation(f->inbox, contact, FALSE, &error);
	g_assert_no_error(error);
	g_assert_nonnull(row);
	return row;
}

/* Every channel at once, at known times: a missed call, an email, a text,
 * a voicemail and a chat message, out of order on purpose. */
static void
all_channels(Fixture *f)
{
	g_autoptr(VentureEntity) chat = widget(f, f->org);
	g_autoptr(VentureEntity) session = NULL, voicemail = NULL, missed = NULL;
	g_autofree gchar *token = NULL;
	session = chat_in(f, chat, "Ada", "ADA@example.com", NULL, "Is the shop open on Sunday?", "192.0.2.10",
		"2026-09-15T12:00:00Z", &token);
	voicemail = call_in(f, f->ada, "voicemail", "2026-09-15T11:00:00Z", "Hi, it is Ada, please call me back.");
	text_in(f, "t1", "+15550100123", "Running ten minutes late", "2026-09-15T10:00:00Z");
	mail_in(f, mail_question);
	missed = call_in(f, f->ada, "missed", "2026-09-15T08:00:00Z", NULL);
}

static gchar *
channels_of(GPtrArray *items)
{
	GString *text = g_string_new(NULL);
	guint i;
	for (i = 0; i < items->len; i++)
	{
		VentureInboxItem *item = g_ptr_array_index(items, i);
		g_string_append_printf(text, "%s%s", i > 0 ? "," : "", item->source->channel);
	}
	return g_string_free(text, FALSE);
}

static GPtrArray *
thread(Fixture *f, VentureEntity *contact)
{
	g_autoptr(GError) error = NULL;
	GPtrArray *items = venture_inbox_service_thread(f->inbox, contact, NULL, 200, &error);
	g_assert_no_error(error);
	g_assert_nonnull(items);
	return items;
}

/* --- Sources ---------------------------------------------------------- */

/* Each channel registers its own source; a channel defined in another
 * file (this one) joins with one line, and nothing lists them by hand.
 * If the registry were a hand-kept table the fax would never appear. */
static void
test_sources(Fixture *f, gconstpointer data)
{
	g_autoptr(GPtrArray) sources = venture_inbox_sources();
	g_autoptr(GPtrArray) items = NULL;
	g_autofree gchar *channels = NULL;
	static const gchar *const wanted[] = { "email", "text", "call", "chat", "fax" };
	gsize i;
	(void)data;
	for (i = 0; i < G_N_ELEMENTS(wanted); i++)
	{
		const VentureInboxSource *source = venture_inbox_source_lookup(wanted[i]);
		guint j;
		gboolean listed = FALSE;
		g_assert_nonnull(source);
		g_assert_cmpstr(source->channel, ==, wanted[i]);
		g_assert_nonnull(source->list);
		g_assert_nonnull(source->render);
		for (j = 0; j < sources->len; j++)
			listed |= g_ptr_array_index(sources, j) == source;
		g_assert_true(listed);
	}
	g_assert_null(venture_inbox_source_lookup("pigeon"));
	/* Replies: email, text and chat answer in their channel; a call is
	 * answered by calling back, so it has an action instead. */
	g_assert_nonnull(venture_inbox_source_lookup("email")->reply);
	g_assert_nonnull(venture_inbox_source_lookup("text")->reply);
	g_assert_nonnull(venture_inbox_source_lookup("chat")->reply);
	g_assert_null(venture_inbox_source_lookup("call")->reply);
	g_assert_nonnull(venture_inbox_source_lookup("call")->action);
	fax_enabled = TRUE;
	items = thread(f, f->ada);
	channels = channels_of(items);
	g_assert_cmpstr(channels, ==, "fax");
}

/* --- The thread --------------------------------------------------------- */

/* One thread holds every channel in time order, whatever order the
 * messages were recorded in. A thread sorted by when it was written would
 * put the 08:00 missed call last. */
static void
test_thread_order(Fixture *f, gconstpointer data)
{
	g_autoptr(GPtrArray) items = NULL;
	g_autoptr(GPtrArray) since = NULL;
	g_autoptr(GDateTime) cursor = at("2026-09-15T10:00:00Z");
	g_autoptr(GError) error = NULL;
	g_autofree gchar *channels = NULL, *later = NULL;
	guint i;
	(void)data;
	all_channels(f);
	items = thread(f, f->ada);
	channels = channels_of(items);
	g_assert_cmpstr(channels, ==, "call,email,text,call,chat");
	for (i = 0; i < items->len; i++)
	{
		VentureInboxItem *item = g_ptr_array_index(items, i);
		g_assert_true(item->inbound);
		if (i > 0)
			g_assert_cmpint(g_date_time_compare(((VentureInboxItem *)g_ptr_array_index(items, i - 1))->at, item->at), <=, 0);
	}
	/* The cursor: only what came after it. */
	since = venture_inbox_service_thread(f->inbox, f->ada, cursor, 200, &error);
	g_assert_no_error(error);
	later = channels_of(since);
	g_assert_cmpstr(later, ==, "call,chat");
}

/* The thread is references, not copies: each item is the channel's own
 * record, and the conversation row holds no message words. A second store
 * of bodies is how an edited or erased message lives on in the inbox. */
static void
test_no_copies(Fixture *f, gconstpointer data)
{
	g_autoptr(GPtrArray) items = NULL;
	g_autoptr(VentureEntity) row = NULL;
	g_autoptr(JsonNode) json = NULL;
	g_autoptr(GPtrArray) fields = NULL;
	g_autofree gchar *serialized = NULL;
	static const gchar *const words[] = { "Can you deliver in March", "Running ten minutes late",
		"please call me back", "open on Sunday" };
	gsize i;
	(void)data;
	all_channels(f);
	items = thread(f, f->ada);
	g_assert_cmpuint(items->len, ==, 5);
	g_assert_true(VENTURE_IS_ACTIVITY(((VentureInboxItem *)g_ptr_array_index(items, 0))->record));
	g_assert_true(VENTURE_IS_INTERACTION(((VentureInboxItem *)g_ptr_array_index(items, 1))->record));
	g_assert_true(VENTURE_IS_TEXT_MESSAGE(((VentureInboxItem *)g_ptr_array_index(items, 2))->record));
	g_assert_true(VENTURE_IS_INBOX_CHAT_MESSAGE(((VentureInboxItem *)g_ptr_array_index(items, 4))->record));
	row = conversation(f, f->ada);
	json = venture_serializable_to_json(VENTURE_SERIALIZABLE(row), FALSE);
	serialized = json_to_string(json, FALSE);
	for (i = 0; i < G_N_ELEMENTS(words); i++)
		g_assert_null(strstr(serialized, words[i]));
	fields = venture_entity_get_field_specs(row);
	for (i = 0; i < fields->len; i++)
		g_assert_cmpint(venture_field_spec_get_kind(g_ptr_array_index(fields, i)), !=, VENTURE_FIELD_KIND_TEXT);
}

/* A first message makes the conversation, open and unassigned. */
static void
test_defaults(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) row = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) none = NULL;
	g_autofree gchar *channels = NULL, *last = NULL;
	(void)data;
	none = venture_inbox_service_conversation(f->inbox, f->ada, FALSE, &error);
	g_assert_null(none);
	g_clear_error(&error);
	text_in(f, "t1", "+15550100123", "Hello there", "2026-09-15T10:00:00Z");
	row = conversation(f, f->ada);
	g_assert_cmpint(enum_of(row, "status"), ==, VENTURE_INBOX_STATUS_OPEN);
	g_assert_cmpint(number_of(row, "owner-id"), ==, 0);
	g_assert_cmpint(number_of(row, "contact-id"), ==, venture_entity_get_id(f->ada));
	channels = text_of(row, "channels");
	last = text_of(row, "last-inbound-channel");
	g_assert_nonnull(strstr(channels, ",text,"));
	g_assert_cmpstr(last, ==, "text");
	/* One per contact: a second message on another channel is the same row. */
	mail_in(f, mail_question);
	{
		g_autoptr(VentureEntity) again = conversation(f, f->ada);
		g_autofree gchar *both = text_of(again, "channels");
		g_assert_cmpint(venture_entity_get_id(again), ==, venture_entity_get_id(row));
		g_assert_nonnull(strstr(both, ",email,"));
	}
	/* The generic record API cannot write one: only the inbox service does. */
	g_object_set(row, "status", VENTURE_INBOX_STATUS_CLOSED, NULL);
	g_assert_false(venture_database_save(f->db, row, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
}

/* --- Assignment --------------------------------------------------------- */

static guint
audit_count(Fixture *f, VentureEntity *row, const gchar *needle)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_AUDIT_ENTRY);
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;
	guint i, n = 0;
	venture_query_add_filter_string(query, "target-type", VENTURE_FILTER_OP_EQ, "inbox_conversation", NULL);
	venture_query_add_filter_int(query, "target-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(row), NULL);
	rows = venture_database_find(f->db, query, &error);
	g_assert_no_error(error);
	for (i = 0; i < rows->len; i++)
	{
		g_autofree gchar *text = NULL;
		g_object_get(g_ptr_array_index(rows, i), "diff", &text, NULL);
		if (text != NULL && strstr(text, needle) != NULL)
			n++;
	}
	return n;
}

static guint
listed(Fixture *f, VentureInboxView view, gint64 user, gint status, const gchar *channel)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	VentureInboxFilter filter;
	filter.organization_id = f->org;
	filter.view = view;
	filter.user_id = user;
	filter.status = status;
	filter.channel = channel;
	filter.limit = 50;
	rows = venture_inbox_service_list(f->inbox, &filter, &error);
	g_assert_no_error(error);
	g_assert_nonnull(rows);
	return rows->len;
}

/* An owner is a person in the organization; changing it is audited with
 * who did it, and the mine and unassigned views follow it. */
static void
test_assignment(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) row = NULL, fresh = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor alice = person("alice");
	gint64 bob = venture_entity_get_id(f->bob), carol = venture_entity_get_id(f->carol);
	(void)data;
	text_in(f, "t1", "+15550100123", "Hello there", "2026-09-15T10:00:00Z");
	row = conversation(f, f->ada);
	g_assert_cmpuint(listed(f, VENTURE_INBOX_VIEW_UNASSIGNED, 0, -1, NULL), ==, 1);
	g_assert_cmpuint(listed(f, VENTURE_INBOX_VIEW_MINE, bob, -1, NULL), ==, 0);
	g_assert_true(venture_inbox_service_assign(f->inbox, row, bob, &alice, &error));
	g_assert_no_error(error);
	fresh = reread(f, row);
	g_assert_cmpint(number_of(fresh, "owner-id"), ==, bob);
	g_assert_cmpuint(listed(f, VENTURE_INBOX_VIEW_MINE, bob, -1, NULL), ==, 1);
	g_assert_cmpuint(listed(f, VENTURE_INBOX_VIEW_UNASSIGNED, 0, -1, NULL), ==, 0);
	g_assert_cmpuint(audit_count(f, row, "owner_id"), ==, 1);
	/* Reassign, then unassign: each one more audited change. */
	g_assert_true(venture_inbox_service_assign(f->inbox, fresh, carol, &alice, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(listed(f, VENTURE_INBOX_VIEW_MINE, bob, -1, NULL), ==, 0);
	g_assert_cmpuint(listed(f, VENTURE_INBOX_VIEW_MINE, carol, -1, NULL), ==, 1);
	g_clear_object(&fresh);
	fresh = reread(f, row);
	g_assert_true(venture_inbox_service_assign(f->inbox, fresh, 0, &alice, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(listed(f, VENTURE_INBOX_VIEW_UNASSIGNED, 0, -1, NULL), ==, 1);
	g_assert_cmpuint(audit_count(f, row, "owner_id"), ==, 3);
	/* Someone outside the organization cannot own it. */
	g_clear_object(&fresh);
	fresh = reread(f, row);
	g_assert_false(venture_inbox_service_assign(f->inbox, fresh, venture_entity_get_id(f->stranger), &alice, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	g_assert_false(venture_inbox_service_assign(f->inbox, fresh, 999999, &alice, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

/* --- Status ------------------------------------------------------------- */

/* Open, waiting and closed by hand; anything new from the customer, on
 * any channel, reopens a waiting or closed conversation. Our own reply
 * does not. */
static void
test_status(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) row = NULL, fresh = NULL, chat = NULL, session = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) now = at("2026-09-15T10:05:00Z");
	g_autofree gchar *token = NULL, *outcome = NULL;
	VentureActor alice = person("alice");
	(void)data;
	text_in(f, "t1", "+15550100123", "Hello there", "2026-09-15T10:00:00Z");
	row = conversation(f, f->ada);
	g_assert_cmpuint(listed(f, VENTURE_INBOX_VIEW_ALL, 0, VENTURE_INBOX_STATUS_OPEN, NULL), ==, 1);
	g_assert_true(venture_inbox_service_set_status(f->inbox, row, VENTURE_INBOX_STATUS_WAITING, now, &alice, &error));
	g_assert_no_error(error);
	fresh = reread(f, row);
	g_assert_cmpint(enum_of(fresh, "status"), ==, VENTURE_INBOX_STATUS_WAITING);
	g_assert_cmpuint(listed(f, VENTURE_INBOX_VIEW_ALL, 0, VENTURE_INBOX_STATUS_WAITING, NULL), ==, 1);
	g_assert_cmpuint(listed(f, VENTURE_INBOX_VIEW_ALL, 0, VENTURE_INBOX_STATUS_OPEN, NULL), ==, 0);
	g_assert_cmpuint(audit_count(f, row, "waiting"), >=, 1);
	/* A text back reopens it. */
	text_in(f, "t2", "+15550100123", "Any news?", "2026-09-15T11:00:00Z");
	g_clear_object(&fresh);
	fresh = reread(f, row);
	g_assert_cmpint(enum_of(fresh, "status"), ==, VENTURE_INBOX_STATUS_OPEN);
	/* Closed, then a chat reopens it. */
	g_assert_true(venture_inbox_service_set_status(f->inbox, fresh, VENTURE_INBOX_STATUS_CLOSED, now, &alice, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(listed(f, VENTURE_INBOX_VIEW_ALL, 0, VENTURE_INBOX_STATUS_CLOSED, NULL), ==, 1);
	chat = widget(f, f->org);
	session = chat_in(f, chat, "Ada", "ada@example.com", NULL, "One more thing", "192.0.2.10", "2026-09-15T12:00:00Z", &token);
	g_clear_object(&fresh);
	fresh = reread(f, row);
	g_assert_cmpint(enum_of(fresh, "status"), ==, VENTURE_INBOX_STATUS_OPEN);
	/* Our reply leaves a closed conversation closed. */
	g_assert_true(venture_inbox_service_set_status(f->inbox, fresh, VENTURE_INBOX_STATUS_CLOSED, now, &alice, &error));
	g_assert_no_error(error);
	g_clear_object(&fresh);
	fresh = reread(f, row);
	outcome = venture_inbox_service_reply(f->inbox, fresh, NULL, "Thanks, all noted.", now, &alice, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(outcome);
	g_clear_object(&fresh);
	fresh = reread(f, row);
	g_assert_cmpint(enum_of(fresh, "status"), ==, VENTURE_INBOX_STATUS_CLOSED);
	/* The channel filter. */
	g_assert_cmpuint(listed(f, VENTURE_INBOX_VIEW_ALL, 0, -1, "chat"), ==, 1);
	g_assert_cmpuint(listed(f, VENTURE_INBOX_VIEW_ALL, 0, -1, "email"), ==, 0);
}

/* --- Replies ------------------------------------------------------------ */

/* A reply goes out in the channel the customer last wrote in: text by the
 * telephony send path, email by the outbox as a threaded reply, chat into
 * their chat. A missed call is answered by calling back, not by a reply. */
static void
test_reply_channel(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) row = NULL, chat = NULL, session = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) now = at("2026-09-15T14:00:00Z");
	g_autoptr(GPtrArray) messages = NULL;
	g_autofree gchar *outcome = NULL, *used = NULL, *token = NULL;
	g_autoptr(VentureQuery) mail = venture_query_new(VENTURE_TYPE_MAIL_MESSAGE);
	g_autoptr(VentureEntity) queued = NULL;
	g_autofree gchar *in_reply_to = NULL;
	VentureActor alice = person("alice");
	VentureInboxItem *last = NULL;
	const VentureInboxSource *source;
	(void)data;
	g_assert_true(venture_telephony_service_record_consent(f->phone, f->ada, TRUE, "Said yes on the phone", NULL, now, NULL, &error));
	g_assert_no_error(error);
	/* Last in: a missed call. Nothing to reply to in that channel. */
	call_in(f, f->ada, "missed", "2026-09-15T08:00:00Z", NULL);
	row = conversation(f, f->ada);
	source = venture_inbox_service_reply_channel(f->inbox, f->ada, NULL, &last, &error);
	g_assert_no_error(error);
	g_assert_nonnull(source);
	g_assert_cmpstr(source->channel, ==, "call");
	g_clear_pointer(&last, venture_inbox_item_free);
	g_assert_null(venture_inbox_service_reply(f->inbox, row, NULL, "Sorry we missed you", now, &alice, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	/* ...but a text can be chosen instead. */
	outcome = venture_inbox_service_reply(f->inbox, row, "text", "Sorry we missed you", now, &alice, &used, &error);
	g_assert_no_error(error);
	g_assert_nonnull(outcome);
	g_assert_cmpstr(used, ==, "text");
	g_assert_cmpint(fake_sends, ==, 1);
	g_clear_pointer(&outcome, g_free);
	g_clear_pointer(&used, g_free);
	/* Last in: a text. */
	text_in(f, "t1", "+15550100123", "Can I come at four?", "2026-09-15T10:00:00Z");
	outcome = venture_inbox_service_reply(f->inbox, row, NULL, "Four is fine.", now, &alice, &used, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(used, ==, "text");
	g_assert_cmpint(fake_sends, ==, 2);
	g_clear_pointer(&outcome, g_free);
	g_clear_pointer(&used, g_free);
	/* Last in: an email. */
	mail_in(f, mail_question);
	mail_in(f, mail_followup);
	outcome = venture_inbox_service_reply(f->inbox, row, NULL, "Yes, March works.", now, &alice, &used, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(used, ==, "email");
	venture_query_set_organization(mail, f->org);
	venture_query_add_filter_string(mail, "related-type", VENTURE_FILTER_OP_EQ, "interaction", NULL);
	queued = venture_database_find_one(f->db, mail, &error);
	g_assert_no_error(error);
	g_assert_nonnull(queued);
	in_reply_to = text_of(queued, "in-reply-to");
	g_assert_cmpstr(in_reply_to, ==, "<q2@example.net>");
	g_assert_cmpint(fake_sends, ==, 2);
	g_clear_pointer(&outcome, g_free);
	g_clear_pointer(&used, g_free);
	/* Last in: a chat. */
	chat = widget(f, f->org);
	session = chat_in(f, chat, "Ada", "ada@example.com", NULL, "Are you there?", "192.0.2.10", "2026-09-15T13:30:00Z", &token);
	outcome = venture_inbox_service_reply(f->inbox, row, NULL, "Yes, how can I help?", now, &alice, &used, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(used, ==, "chat");
	messages = venture_inbox_chat_messages(f->inbox, session, 0, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(messages->len, ==, 2);
	{
		VentureEntity *answer = g_ptr_array_index(messages, 1);
		g_autofree gchar *body = text_of(answer, "body"), *author = text_of(answer, "author");
		gboolean inbound = TRUE;
		g_object_get(answer, "inbound", &inbound, NULL);
		g_assert_false(inbound);
		g_assert_cmpstr(body, ==, "Yes, how can I help?");
		g_assert_cmpstr(author, ==, "alice");
	}
	g_assert_cmpint(fake_sends, ==, 2);
	/* An empty reply is refused before any channel is asked. */
	g_assert_null(venture_inbox_service_reply(f->inbox, row, NULL, "   ", now, &alice, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

/* A text reply obeys text consent and quiet hours, because it goes
 * through the one send path: refused without consent, held at night. */
static void
test_reply_text_rules(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) row = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) day = at("2026-09-15T14:00:00Z");
	g_autoptr(GDateTime) night = at("2026-09-15T23:30:00Z");
	g_autoptr(GPtrArray) texts = NULL;
	g_autofree gchar *outcome = NULL;
	VentureActor alice = person("alice");
	(void)data;
	text_in(f, "t1", "+15550100123", "Can I come at four?", "2026-09-15T10:00:00Z");
	row = conversation(f, f->ada);
	g_assert_null(venture_inbox_service_reply(f->inbox, row, NULL, "Four is fine.", day, &alice, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);
	g_assert_cmpint(fake_sends, ==, 0);
	g_assert_true(venture_telephony_service_record_consent(f->phone, f->ada, TRUE, "Said yes on the phone", NULL, day, NULL, &error));
	g_assert_no_error(error);
	outcome = venture_inbox_service_reply(f->inbox, row, NULL, "Four is fine.", night, &alice, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(outcome);
	g_assert_nonnull(strstr(outcome, "quiet hours"));
	g_assert_cmpint(fake_sends, ==, 0);
	texts = venture_telephony_service_thread(f->phone, f->ada, 10, &error);
	g_assert_no_error(error);
	{
		g_autofree gchar *state = text_of(g_ptr_array_index(texts, texts->len - 1), "state");
		g_assert_cmpstr(state, ==, "held");
	}
}

/* --- Web chat ------------------------------------------------------------- */

static gboolean
chat_refused(Fixture *f, VentureEntity *chat, const gchar *name, const gchar *email, const gchar *phone,
	const gchar *body, GQuark domain, gint code)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) now = at("2026-09-15T12:00:00Z");
	g_autofree gchar *token = text_of(chat, "token"), *visitor = NULL;
	g_autoptr(VentureEntity) session = NULL;
	VentureInboxChatIntake intake;
	intake.name = name;
	intake.email = email;
	intake.phone = phone;
	intake.body = body;
	intake.trap = NULL;
	intake.remote = "198.51.100.7";
	session = venture_inbox_chat_start(f->inbox, token, &intake, now, &visitor, &error);
	return session == NULL && visitor == NULL && g_error_matches(error, domain, code);
}

/* The intake takes a name and an email or a phone, matched to a contact
 * the way lead capture matches, or a new contact; everything is bounded,
 * and a filled-in trap field is accepted and dropped. */
static void
test_chat_intake(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) chat = widget(f, f->org);
	g_autoptr(VentureEntity) matched = NULL, by_phone = NULL, created = NULL, contact = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) now = at("2026-09-15T12:00:00Z");
	g_autofree gchar *t1 = NULL, *t2 = NULL, *t3 = NULL, *long_name = g_strnfill(VENTURE_INBOX_CHAT_NAME_MAX + 1, 'a');
	g_autofree gchar *long_body = g_strnfill(VENTURE_INBOX_CHAT_BODY_MAX + 1, 'b'), *source = NULL, *token = NULL;
	g_autofree gchar *visitor = NULL;
	g_autoptr(VentureEntity) trapped = NULL;
	VentureInboxChatIntake intake;
	(void)data;
	matched = chat_in(f, chat, "Ada", "  ADA@Example.COM ", NULL, "Hello", "192.0.2.1", "2026-09-15T12:00:00Z", &t1);
	g_assert_cmpint(number_of(matched, "contact-id"), ==, venture_entity_get_id(f->ada));
	by_phone = chat_in(f, chat, "Ada", NULL, "(555) 010-0123", "Hello again", "192.0.2.2", "2026-09-15T12:01:00Z", &t2);
	g_assert_cmpint(number_of(by_phone, "contact-id"), ==, venture_entity_get_id(f->ada));
	created = chat_in(f, chat, "Grace Example", "grace@example.org", NULL, "First time here", "192.0.2.3", "2026-09-15T12:02:00Z", &t3);
	g_assert_cmpint(number_of(created, "contact-id"), !=, venture_entity_get_id(f->ada));
	contact = venture_database_get(f->db, VENTURE_TYPE_CONTACT, number_of(created, "contact-id"), &error);
	g_assert_no_error(error);
	source = text_of(contact, "source");
	g_assert_cmpstr(source, ==, "Web chat");
	g_assert_true(chat_refused(f, chat, "", "x@example.com", NULL, "Hi", VENTURE_ERROR, VENTURE_ERROR_VALIDATION));
	g_assert_true(chat_refused(f, chat, "Ada", NULL, NULL, "Hi", VENTURE_ERROR, VENTURE_ERROR_VALIDATION));
	g_assert_true(chat_refused(f, chat, "Ada", "not an address", NULL, "Hi", VENTURE_ERROR, VENTURE_ERROR_VALIDATION));
	g_assert_true(chat_refused(f, chat, "Ada", "x@example.com\r\nBcc: y@example.com", NULL, "Hi", VENTURE_ERROR, VENTURE_ERROR_VALIDATION));
	g_assert_true(chat_refused(f, chat, "Ada", NULL, "call me maybe", "Hi", VENTURE_ERROR, VENTURE_ERROR_VALIDATION));
	g_assert_true(chat_refused(f, chat, long_name, "x@example.com", NULL, "Hi", VENTURE_ERROR, VENTURE_ERROR_VALIDATION));
	g_assert_true(chat_refused(f, chat, "Ada", "x@example.com", NULL, long_body, VENTURE_ERROR, VENTURE_ERROR_VALIDATION));
	g_assert_true(chat_refused(f, chat, "Ada", "x@example.com", NULL, "", VENTURE_ERROR, VENTURE_ERROR_VALIDATION));
	g_assert_true(chat_refused(f, chat, "Ada", "x@example.com", NULL, "\xff\xfe", VENTURE_ERROR, VENTURE_ERROR_VALIDATION));
	/* The trap: success, nothing kept. */
	token = text_of(chat, "token");
	intake.name = "Bot";
	intake.email = "bot@example.com";
	intake.phone = NULL;
	intake.body = "Buy now";
	intake.trap = "https://spam.example.com";
	intake.remote = "192.0.2.99";
	trapped = venture_inbox_chat_start(f->inbox, token, &intake, now, &visitor, &error);
	g_assert_no_error(error);
	g_assert_null(trapped);
	g_assert_null(visitor);
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_CONTACT);
		g_autoptr(VentureEntity) bot = NULL;
		venture_query_add_filter_string(query, "email", VENTURE_FILTER_OP_EQ, "bot@example.com", NULL);
		bot = venture_database_find_one(f->db, query, NULL);
		g_assert_null(bot);
	}
	/* An unknown or switched-off chat is not found. */
	intake.trap = NULL;
	g_assert_null(venture_inbox_chat_start(f->inbox, "0000000000000000000000000000000000000000", &intake, now, &visitor, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_clear_error(&error);
	g_assert_true(venture_inbox_chat_widget_set_active(f->inbox, chat, FALSE, NULL, &error));
	g_assert_no_error(error);
	g_assert_null(venture_inbox_chat_start(f->inbox, token, &intake, now, &visitor, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
}

/* One address may start only so many chats, and one chat take only so
 * many messages, in a window; the window passes. */
static void
test_chat_rate_limit(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) chat = widget(f, f->org);
	g_autoptr(VentureEntity) session = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) now = at("2026-09-15T12:00:00Z");
	g_autoptr(GDateTime) later = g_date_time_add_seconds(now, VENTURE_INBOX_CHAT_WINDOW_SECONDS + 1);
	g_autofree gchar *token = text_of(chat, "token");
	VentureInboxChatIntake intake;
	gint i;
	(void)data;
	intake.name = "Ada";
	intake.email = "ada@example.com";
	intake.phone = NULL;
	intake.body = "Hello";
	intake.trap = NULL;
	intake.remote = "203.0.113.5";
	for (i = 0; i < VENTURE_INBOX_CHAT_STARTS_PER_WINDOW; i++)
	{
		g_autofree gchar *visitor = NULL;
		g_autoptr(VentureEntity) row = venture_inbox_chat_start(f->inbox, token, &intake, now, &visitor, &error);
		g_assert_no_error(error);
		g_assert_nonnull(row);
		if (session == NULL)
			session = g_object_ref(row);
	}
	{
		g_autofree gchar *visitor = NULL;
		g_autoptr(VentureEntity) row = venture_inbox_chat_start(f->inbox, token, &intake, now, &visitor, &error);
		g_assert_null(row);
		g_assert_error(error, VENTURE_INBOX_ERROR, VENTURE_INBOX_ERROR_RATE_LIMITED);
		g_clear_error(&error);
	}
	intake.remote = "203.0.113.6";
	{
		g_autofree gchar *visitor = NULL;
		g_autoptr(VentureEntity) row = venture_inbox_chat_start(f->inbox, token, &intake, now, &visitor, &error);
		g_assert_no_error(error);
		g_assert_nonnull(row);
	}
	intake.remote = "203.0.113.5";
	{
		g_autofree gchar *visitor = NULL;
		g_autoptr(VentureEntity) row = venture_inbox_chat_start(f->inbox, token, &intake, later, &visitor, &error);
		g_assert_no_error(error);
		g_assert_nonnull(row);
	}
	/* Messages into one chat: the first was the opening one. */
	for (i = 1; i < VENTURE_INBOX_CHAT_MESSAGES_PER_WINDOW; i++)
	{
		g_autoptr(VentureEntity) message = venture_inbox_chat_post(f->inbox, session, "More", "203.0.113.5", now, &error);
		g_assert_no_error(error);
		g_assert_nonnull(message);
	}
	g_assert_null(venture_inbox_chat_post(f->inbox, session, "Too many", "203.0.113.5", now, &error));
	g_assert_error(error, VENTURE_INBOX_ERROR, VENTURE_INBOX_ERROR_RATE_LIMITED);
}

/* The visitor's link is a long random token kept only as its hash: the
 * stored hash does not open the chat, a truncated token does not, and one
 * visitor's link never shows another's messages. */
static void
test_chat_link_secrecy(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) chat = widget(f, f->org);
	g_autoptr(VentureEntity) first = NULL, second = NULL, found = NULL;
	g_autoptr(GPtrArray) messages = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) json = NULL;
	g_autofree gchar *t1 = NULL, *t2 = NULL, *hash = NULL, *stored = NULL, *expected = NULL, *serialized = NULL, *cut = NULL;
	gsize i;
	(void)data;
	first = chat_in(f, chat, "Ada", "ada@example.com", NULL, "First visitor words", "192.0.2.1", "2026-09-15T12:00:00Z", &t1);
	second = chat_in(f, chat, "Ada", "ada@example.com", NULL, "Second visitor words", "192.0.2.2", "2026-09-15T12:01:00Z", &t2);
	g_assert_cmpuint(strlen(t1), ==, 64);
	for (i = 0; t1[i] != '\0'; i++)
		g_assert_true(g_ascii_isxdigit(t1[i]));
	g_assert_cmpstr(t1, !=, t2);
	stored = text_of(first, "token-hash");
	expected = g_compute_checksum_for_string(G_CHECKSUM_SHA256, t1, -1);
	g_assert_cmpstr(stored, ==, expected);
	g_assert_null(strstr(stored, t1));
	found = venture_inbox_chat_session_lookup(f->inbox, t1);
	g_assert_nonnull(found);
	g_assert_cmpint(venture_entity_get_id(found), ==, venture_entity_get_id(first));
	hash = g_strdup(stored);
	g_assert_null(venture_inbox_chat_session_lookup(f->inbox, hash));
	cut = g_strndup(t1, 63);
	g_assert_null(venture_inbox_chat_session_lookup(f->inbox, cut));
	g_assert_null(venture_inbox_chat_session_lookup(f->inbox, ""));
	messages = venture_inbox_chat_messages(f->inbox, first, 0, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(messages->len, ==, 1);
	{
		g_autofree gchar *body = text_of(g_ptr_array_index(messages, 0), "body");
		g_assert_cmpstr(body, ==, "First visitor words");
	}
	json = venture_serializable_to_json(VENTURE_SERIALIZABLE(first), FALSE);
	serialized = json_to_string(json, FALSE);
	g_assert_null(strstr(serialized, stored));
}

/* --- Organizations ---------------------------------------------------------- */

/* A member of one organization sees only its conversations, and a chat
 * on one organization's site matches only its own contacts. */
static void
test_isolation(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) other = NULL, chat = NULL, session = NULL, theirs = NULL, row = NULL, contact = NULL;
	g_autoptr(VentureAccessScope) scope = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *token = NULL;
	VentureAuthPrincipal principal;
	VentureInboxFilter filter;
	(void)data;
	other = g_object_new(VENTURE_TYPE_ORGANIZATION, "name", "Other organization", "slug", "other", "active", TRUE, NULL);
	persist(f, other);
	theirs = g_object_new(VENTURE_TYPE_CONTACT, "organization-id", venture_entity_get_id(other), "name", "Ada Elsewhere",
		"email", "ada@example.com", NULL);
	persist(f, theirs);
	chat = widget(f, venture_entity_get_id(other));
	session = chat_in(f, chat, "Ada", "ada@example.com", NULL, "Hello other shop", "192.0.2.1", "2026-09-15T12:00:00Z", &token);
	g_assert_cmpint(number_of(session, "contact-id"), ==, venture_entity_get_id(theirs));
	g_assert_cmpint(venture_entity_get_organization_id(session), ==, venture_entity_get_id(other));
	text_in(f, "t1", "+15550100123", "Hello home shop", "2026-09-15T10:00:00Z");
	row = conversation(f, theirs);
	principal.authenticated = TRUE;
	principal.user_id = venture_entity_get_id(f->bob);
	principal.token_id = 0;
	principal.role = VENTURE_USER_ROLE_EDITOR;
	principal.name = NULL;
	principal.provider_token = FALSE;
	scope = venture_access_policy_enter(venture_database_get_access_policy(f->db), &principal);
	filter.organization_id = 0;
	filter.view = VENTURE_INBOX_VIEW_ALL;
	filter.user_id = 0;
	filter.status = -1;
	filter.channel = NULL;
	filter.limit = 50;
	rows = venture_inbox_service_list(f->inbox, &filter, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(rows->len, ==, 1);
	g_assert_cmpint(number_of(g_ptr_array_index(rows, 0), "contact-id"), ==, venture_entity_get_id(f->ada));
	contact = venture_database_get(f->db, VENTURE_TYPE_INBOX_CONVERSATION, venture_entity_get_id(row), NULL);
	g_assert_null(contact);
	/* Bob cannot assign the other shop's conversation to himself. */
	g_assert_false(venture_inbox_service_assign(f->inbox, row, venture_entity_get_id(f->bob), NULL, &error));
	g_assert_nonnull(error);
}

/* --- Pages and routes --------------------------------------------------------- */

typedef struct { gboolean done; GBytes *body; GError *error; } WebCall;

static void
web_done(GObject *source, GAsyncResult *result, gpointer data)
{
	WebCall *call = data;
	call->body = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &call->error);
	call->done = TRUE;
}

static guint
web_request(VentureWebServer *server, const gchar *method, const gchar *path, const gchar *type,
	const gchar *body, gchar **out, SoupMessageHeaders **headers)
{
	g_autoptr(SoupSession) session = soup_session_new();
	g_autofree gchar *url = g_strdup_printf("http://127.0.0.1:%u%s", venture_web_server_get_port(server), path);
	g_autoptr(SoupMessage) message = soup_message_new(method, url);
	WebCall call = { FALSE, NULL, NULL };
	guint status;
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	if (body != NULL)
	{
		g_autoptr(GBytes) bytes = g_bytes_new(body, strlen(body));
		soup_message_set_request_body_from_bytes(message, type != NULL ? type : "application/x-www-form-urlencoded", bytes);
	}
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL, web_done, &call);
	while (!call.done)
		g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(call.error);
	if (out != NULL)
		*out = g_strndup(g_bytes_get_data(call.body, NULL), g_bytes_get_size(call.body));
	if (headers != NULL)
		*headers = soup_message_headers_ref(soup_message_get_response_headers(message));
	status = soup_message_get_status(message);
	g_bytes_unref(call.body);
	return status;
}

static VentureWebServer *
web_start(Fixture *f)
{
	g_autoptr(GError) error = NULL;
	VentureWebServer *server;
	g_object_set(f->config, "server-bind-address", "127.0.0.1", "server-port", (gint64)0, "security-require-auth", FALSE, NULL);
	server = venture_web_server_new(f->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(server, &error));
	g_assert_no_error(error);
	return server;
}

/* The list and the conversation page show every channel, the assignment,
 * the status and a reply in the right channel; the API says the same. */
static void
test_pages(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureWebServer) server = NULL;
	g_autoptr(VentureEntity) row = NULL, fresh = NULL;
	g_autoptr(JsonNode) json = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *page = NULL, *path = NULL, *api = NULL, *body = NULL, *form = NULL, *assign = NULL;
	JsonArray *items;
	(void)data;
	all_channels(f);
	row = conversation(f, f->ada);
	server = web_start(f);
	g_assert_cmpuint(web_request(server, "GET", "/conversations", NULL, NULL, &page, NULL), ==, 200);
	g_assert_nonnull(strstr(page, "Ada Example"));
	g_assert_nonnull(strstr(page, "Unassigned"));
	g_assert_nonnull(strstr(page, "name=\"viewport\""));
	g_clear_pointer(&page, g_free);
	g_assert_cmpuint(web_request(server, "GET", "/conversations?view=unassigned&status=open&channel=chat", NULL, NULL, &page, NULL), ==, 200);
	g_assert_nonnull(strstr(page, "Ada Example"));
	g_clear_pointer(&page, g_free);
	g_assert_cmpuint(web_request(server, "GET", "/conversations?status=closed", NULL, NULL, &page, NULL), ==, 200);
	g_assert_null(strstr(page, "Ada Example"));
	g_clear_pointer(&page, g_free);
	path = g_strdup_printf("/conversations/%" G_GINT64_FORMAT, venture_entity_get_id(row));
	g_assert_cmpuint(web_request(server, "GET", path, NULL, NULL, &page, NULL), ==, 200);
	g_assert_nonnull(strstr(page, "Spring order"));
	g_assert_nonnull(strstr(page, "Running ten minutes late"));
	g_assert_nonnull(strstr(page, "Missed call"));
	g_assert_nonnull(strstr(page, "please call me back"));
	g_assert_nonnull(strstr(page, "open on Sunday?"));
	g_assert_nonnull(strstr(page, "Reply by chat"));
	g_clear_pointer(&page, g_free);
	/* The contact's own page links to it. */
	g_free(path);
	path = g_strdup_printf("/contacts/%" G_GINT64_FORMAT "/conversation", venture_entity_get_id(f->ada));
	g_assert_cmpuint(web_request(server, "GET", path, NULL, NULL, NULL, NULL), ==, 302);
	/* Assign, status and reply through the page. */
	g_free(path);
	path = g_strdup_printf("/conversations/%" G_GINT64_FORMAT "/assign", venture_entity_get_id(row));
	assign = g_strdup_printf("owner_id=%" G_GINT64_FORMAT, venture_entity_get_id(f->bob));
	g_assert_cmpuint(web_request(server, "POST", path, NULL, assign, NULL, NULL), ==, 302);
	g_free(path);
	path = g_strdup_printf("/conversations/%" G_GINT64_FORMAT "/status", venture_entity_get_id(row));
	g_assert_cmpuint(web_request(server, "POST", path, NULL, "status=waiting", NULL, NULL), ==, 302);
	g_free(path);
	path = g_strdup_printf("/conversations/%" G_GINT64_FORMAT "/reply", venture_entity_get_id(row));
	g_assert_cmpuint(web_request(server, "POST", path, NULL, "body=We+are+open+ten+to+four.", NULL, NULL), ==, 302);
	fresh = reread(f, row);
	g_assert_cmpint(number_of(fresh, "owner-id"), ==, venture_entity_get_id(f->bob));
	g_assert_cmpint(enum_of(fresh, "status"), ==, VENTURE_INBOX_STATUS_WAITING);
	/* The API. */
	g_free(path);
	path = g_strdup_printf("/api/v1/conversations/%" G_GINT64_FORMAT, venture_entity_get_id(row));
	g_assert_cmpuint(web_request(server, "GET", path, NULL, NULL, &api, NULL), ==, 200);
	json = json_from_string(api, &error);
	g_assert_no_error(error);
	items = json_object_get_array_member(json_node_get_object(json), "items");
	g_assert_cmpuint(json_array_get_length(items), ==, 6);
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(items, 0), "channel"), ==, "call");
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(items, 1), "channel"), ==, "email");
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(items, 5), "channel"), ==, "chat");
	g_assert_cmpstr(json_object_get_string_member(json_object_get_object_member(json_node_get_object(json), "conversation"), "status"), ==, "waiting");
	g_clear_pointer(&api, g_free);
	g_assert_cmpuint(web_request(server, "GET", "/api/v1/conversations?view=mine&status=waiting", NULL, NULL, &api, NULL), ==, 200);
	g_clear_pointer(&api, g_free);
	g_free(path);
	path = g_strdup_printf("/api/v1/conversations/%" G_GINT64_FORMAT "/status", venture_entity_get_id(row));
	g_assert_cmpuint(web_request(server, "POST", path, "application/json", "{\"status\":\"closed\"}", NULL, NULL), ==, 200);
	g_assert_cmpuint(web_request(server, "POST", path, "application/json", "{\"status\":\"sleeping\"}", NULL, NULL), ==, 400);
	g_free(path);
	path = g_strdup_printf("/api/v1/conversations/%" G_GINT64_FORMAT "/reply", venture_entity_get_id(row));
	g_assert_cmpuint(web_request(server, "POST", path, "application/json", "{\"body\":\"See you Sunday.\",\"channel\":\"chat\"}", &body, NULL), ==, 201);
	g_assert_nonnull(strstr(body, "\"channel\":\"chat\""));
	g_free(path);
	path = g_strdup_printf("/api/v1/conversations/%" G_GINT64_FORMAT "/assign", venture_entity_get_id(row));
	form = g_strdup_printf("{\"owner_id\":%" G_GINT64_FORMAT "}", venture_entity_get_id(f->stranger));
	g_assert_cmpuint(web_request(server, "POST", path, "application/json", form, NULL, NULL), ==, 400);
	g_assert_cmpuint(web_request(server, "GET", "/api/v1/conversations/999999", NULL, NULL, NULL, NULL), ==, 404);
	venture_web_server_stop(server);
}

/* The visitor's side: a script-free form that starts a chat and sends the
 * visitor to their own link, which shows their messages and our replies
 * and nothing else, sets no cookie and leaks no referrer. */
static void
test_visitor_pages(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureWebServer) server = NULL;
	g_autoptr(VentureEntity) chat = widget(f, f->org);
	g_autoptr(VentureEntity) other = NULL;
	g_autoptr(SoupMessageHeaders) headers = NULL;
	g_autofree gchar *token = text_of(chat, "token"), *path = NULL, *page = NULL, *location = NULL, *other_token = NULL;
	g_autofree gchar *link = NULL, *poll = NULL, *big = NULL, *big_form = NULL;
	(void)data;
	other = chat_in(f, chat, "Grace", "grace@example.org", NULL, "Grace's private question", "192.0.2.50", "2026-09-15T12:00:00Z", &other_token);
	mail_in(f, mail_question);
	server = web_start(f);
	path = g_strdup_printf("/webchat/%s", token);
	g_assert_cmpuint(web_request(server, "GET", path, NULL, NULL, &page, &headers), ==, 200);
	g_assert_nonnull(strstr(page, "<form"));
	g_assert_null(strstr(page, "<script"));
	g_assert_null(soup_message_headers_get_one(headers, "Set-Cookie"));
	g_clear_pointer(&page, g_free);
	g_clear_pointer(&headers, soup_message_headers_unref);
	g_assert_cmpuint(web_request(server, "POST", path, NULL, "name=Ada&email=ada%40example.com&message=Do+you+deliver%3F", &page, &headers), ==, 303);
	location = g_strdup(soup_message_headers_get_one(headers, "Location"));
	g_assert_nonnull(location);
	g_assert_true(g_str_has_prefix(location, "/webchat/c/"));
	g_assert_null(soup_message_headers_get_one(headers, "Set-Cookie"));
	g_clear_pointer(&page, g_free);
	g_clear_pointer(&headers, soup_message_headers_unref);
	g_assert_cmpuint(web_request(server, "GET", location, NULL, NULL, &page, &headers), ==, 200);
	g_assert_nonnull(strstr(page, "Do you deliver?"));
	g_assert_null(strstr(page, "private question"));
	g_assert_null(strstr(page, "Can you deliver in March"));
	g_assert_cmpstr(soup_message_headers_get_one(headers, "Referrer-Policy"), ==, "no-referrer");
	g_assert_nonnull(strstr(soup_message_headers_get_one(headers, "Cache-Control"), "no-store"));
	g_assert_null(soup_message_headers_get_one(headers, "Set-Cookie"));
	g_clear_pointer(&page, g_free);
	g_clear_pointer(&headers, soup_message_headers_unref);
	/* The visitor writes again; the poll answer has both, as JSON. */
	g_assert_cmpuint(web_request(server, "POST", location, NULL, "message=Also+on+Sundays%3F", NULL, NULL), ==, 303);
	poll = g_strdup_printf("%s/messages", location);
	g_assert_cmpuint(web_request(server, "GET", poll, NULL, NULL, &page, NULL), ==, 200);
	g_assert_nonnull(strstr(page, "Also on Sundays?"));
	g_assert_null(strstr(page, "private question"));
	g_clear_pointer(&page, g_free);
	g_assert_cmpuint(web_request(server, "GET", "/webchat.js", NULL, NULL, &page, NULL), ==, 200);
	g_clear_pointer(&page, g_free);
	/* Wrong tokens are simply not found. */
	g_assert_cmpuint(web_request(server, "GET", "/webchat/c/00000000000000000000000000000000000000000000000000000000000000aa", NULL, NULL, NULL, NULL), ==, 404);
	g_assert_cmpuint(web_request(server, "GET", "/webchat/c/short", NULL, NULL, NULL, NULL), ==, 404);
	g_assert_cmpuint(web_request(server, "GET", "/webchat/nope", NULL, NULL, NULL, NULL), ==, 404);
	/* An oversized request is refused before it is read as a chat. */
	big = g_strnfill(VENTURE_INBOX_CHAT_MAX_REQUEST + 10, 'x');
	big_form = g_strdup_printf("name=Ada&email=ada%%40example.com&message=%s", big);
	g_assert_cmpuint(web_request(server, "POST", path, NULL, big_form, NULL, NULL), ==, 413);
	/* A bad field shows the form again with the reason. */
	g_assert_cmpuint(web_request(server, "POST", path, NULL, "name=&email=ada%40example.com&message=Hi", &page, NULL), ==, 400);
	g_assert_nonnull(strstr(page, "<form"));
	g_clear_pointer(&page, g_free);
	link = g_strdup_printf("/webchat/c/%s", other_token);
	g_assert_cmpuint(web_request(server, "GET", link, NULL, NULL, &page, NULL), ==, 200);
	g_assert_nonnull(strstr(page, "private question"));
	g_assert_null(strstr(page, "Do you deliver?"));
	venture_web_server_stop(server);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/inbox/sources", Fixture, NULL, setup, test_sources, teardown);
	g_test_add("/inbox/thread/order", Fixture, NULL, setup, test_thread_order, teardown);
	g_test_add("/inbox/thread/no-copies", Fixture, NULL, setup, test_no_copies, teardown);
	g_test_add("/inbox/conversation/defaults", Fixture, NULL, setup, test_defaults, teardown);
	g_test_add("/inbox/conversation/assignment", Fixture, NULL, setup, test_assignment, teardown);
	g_test_add("/inbox/conversation/status", Fixture, NULL, setup, test_status, teardown);
	g_test_add("/inbox/reply/channel", Fixture, NULL, setup, test_reply_channel, teardown);
	g_test_add("/inbox/reply/text-rules", Fixture, NULL, setup, test_reply_text_rules, teardown);
	g_test_add("/inbox/chat/intake", Fixture, NULL, setup, test_chat_intake, teardown);
	g_test_add("/inbox/chat/rate-limit", Fixture, NULL, setup, test_chat_rate_limit, teardown);
	g_test_add("/inbox/chat/link-secrecy", Fixture, NULL, setup, test_chat_link_secrecy, teardown);
	g_test_add("/inbox/isolation", Fixture, NULL, setup, test_isolation, teardown);
	g_test_add("/inbox/pages", Fixture, NULL, setup, test_pages, teardown);
	g_test_add("/inbox/visitor-pages", Fixture, NULL, setup, test_visitor_pages, teardown);
	return g_test_run();
}
