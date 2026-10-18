/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Deliverability: the signed provider feed and DSNs feeding the marketing
 * evidence rules, campaign replies, the domain check, the warm-up and the
 * per-send report. Nothing here touches the network: DNS goes through a
 * fake resolver and mail through the log mailer. */
#include <venture.h>
#include <string.h>
#include "venture-test-accounting.h"
#include "venture-test-util.h"
#include <libsoup/soup.h>

#define FIXTURE(name) VENTURE_TEST_FIXTURES "/deliverability/" name
#define SECRET "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

/* ------------------------------------------------------------------ */
/* A fake resolver: names to TXT records, and every name it was asked. */

#define TEST_TYPE_FAKE_RESOLVER (test_fake_resolver_get_type())
G_DECLARE_FINAL_TYPE(TestFakeResolver, test_fake_resolver, TEST, FAKE_RESOLVER, GObject)
struct _TestFakeResolver {
	GObject parent_instance;
	GHashTable *records;
	GPtrArray *asked;
};
static void test_fake_resolver_iface_init(VentureDnsResolverInterface *iface);
G_DEFINE_TYPE_WITH_CODE(TestFakeResolver, test_fake_resolver, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(VENTURE_TYPE_DNS_RESOLVER, test_fake_resolver_iface_init))
static gchar **fake_lookup(VentureDnsResolver *resolver, const gchar *name, GCancellable *cancellable, GError **error)
{
	TestFakeResolver *self = TEST_FAKE_RESOLVER(resolver);
	const gchar *const *found = g_hash_table_lookup(self->records, name);
	(void)cancellable;
	g_ptr_array_add(self->asked, g_strdup(name));
	if (found == NULL) {
		g_set_error(error, G_RESOLVER_ERROR, G_RESOLVER_ERROR_NOT_FOUND, "No TXT records at %s", name);
		return NULL;
	}
	return g_strdupv((gchar **)found);
}
static void test_fake_resolver_iface_init(VentureDnsResolverInterface *iface)
{
	iface->lookup_txt = fake_lookup;
}
static void test_fake_resolver_finalize(GObject *object)
{
	TestFakeResolver *self = TEST_FAKE_RESOLVER(object);
	g_hash_table_unref(self->records);
	g_ptr_array_unref(self->asked);
	G_OBJECT_CLASS(test_fake_resolver_parent_class)->finalize(object);
}
static void test_fake_resolver_class_init(TestFakeResolverClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = test_fake_resolver_finalize;
}
static void test_fake_resolver_init(TestFakeResolver *self)
{
	self->records = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_strfreev);
	self->asked = g_ptr_array_new_with_free_func(g_free);
}
static void fake_add(TestFakeResolver *self, const gchar *name, const gchar *record, const gchar *second)
{
	const gchar *values[] = { record, second, NULL };
	g_hash_table_replace(self->records, g_strdup(name), g_strdupv((gchar **)values));
}

/* ------------------------------------------------------------------ */
/* The campaign fixture, the same shape as tests/test-marketing.c's. */

typedef struct {
	VentureDatabase *db;
	VentureLogMailer *mailer;
	VentureMailOutbox *outbox;
	VentureMarketingService *service;
	VentureEntity *contact;
	VentureEntity *list;
	VentureEntity *send;
} Fixture;

static void persist(Fixture *f, VentureEntity *row)
{
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_database_save(f->db, row, NULL, &error));
	g_assert_no_error(error);
}
static VentureEntity *contact(Fixture *f, const gchar *name, const gchar *email)
{
	VentureEntity *row = g_object_new(VENTURE_TYPE_CONTACT, "organization-id", (gint64)1, "name", name, "email", email, NULL);
	persist(f, row);
	return row;
}
static void member(Fixture *f, VentureEntity *row)
{
	g_autoptr(VentureEntity) entry = g_object_new(VENTURE_TYPE_MARKETING_MEMBER, "organization-id", (gint64)1,
		"list-id", venture_entity_get_id(f->list), "contact-id", venture_entity_get_id(row), NULL);
	persist(f, entry);
}
static void permit(Fixture *f, VentureEntity *row, const gchar *key)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(VentureMarketingConsent) consent = venture_marketing_service_consent(f->service, 1, row,
		"Signed preference form", "I request this organization's marketing email", key, now, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(consent);
}
static void setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) key = NULL;
	guint8 bytes[32];
	guint i;
	(void)data;
	f->db = venture_test_accounting_database(&error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	for (i = 0; i < sizeof bytes; i++) bytes[i] = (guint8)(i * 7 + 3);
	key = g_bytes_new(bytes, sizeof bytes);
	g_assert_true(venture_integration_service_set_key(venture_integration_service_get(f->db), key, &error));
	g_assert_no_error(error);
	f->service = venture_marketing_service_get(f->db);
	f->mailer = venture_log_mailer_new();
	f->outbox = venture_mail_outbox_new(f->db, VENTURE_MAILER(f->mailer));
	f->contact = contact(f, "Alice", "alice@example.net");
	f->list = g_object_new(VENTURE_TYPE_MARKETING_LIST, "organization-id", (gint64)1, "name", "Readers", NULL);
	persist(f, f->list);
	member(f, f->contact);
	f->send = g_object_new(VENTURE_TYPE_MARKETING_SEND, "organization-id", (gint64)1,
		"name", "Autumn", "list-id", venture_entity_get_id(f->list), "subject", "Hello {name}",
		"text-body", "Read https://example.com/news",
		"html-body", "<p>Hello {name}</p><a href=\"https://example.com/news?utm=x\">News</a> <a href=\"https://example.com/shop\">Shop</a>",
		"interval-seconds", (gint64)1, NULL);
	persist(f, f->send);
}
static void teardown(Fixture *f, gconstpointer data)
{
	(void)data;
	g_clear_object(&f->contact); g_clear_object(&f->list); g_clear_object(&f->send);
	g_clear_object(&f->outbox); g_clear_object(&f->mailer);
	venture_test_accounting_database_cleanup(f->db);
	g_clear_object(&f->db);
}
static GPtrArray *rows_of(Fixture *f, GType type)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	g_autoptr(GError) error = NULL;
	GPtrArray *rows;
	venture_query_set_organization(query, 1);
	venture_query_set_limit(query, 0);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	rows = venture_database_find(f->db, query, &error);
	g_assert_no_error(error);
	return rows;
}
static void preview_and_approve(Fixture *f)
{
	g_autoptr(GError) error = NULL;
	VentureMarketingSend *row = venture_marketing_service_preview(f->service, 1,
		venture_entity_get_id(f->send), "https://venture.example.com", NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(row);
	g_clear_object(&f->send);
	f->send = VENTURE_ENTITY(row);
	g_assert_true(venture_marketing_service_transition(f->service, 1, venture_entity_get_id(f->send), "approve", NULL, &error));
	g_assert_no_error(error);
}
/* Queue and deliver the next recipient; returns its recipient row. */
static VentureEntity *send_next(Fixture *f)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GPtrArray) rows = NULL;
	guint i;
	g_assert_cmpint(venture_marketing_service_run(f->service, 1, venture_entity_get_id(f->send), 100, now, NULL, &error), ==, 1);
	g_assert_no_error(error);
	g_assert_cmpint(venture_mail_outbox_deliver_due(f->outbox, 1, 1, now, NULL, &error), ==, 1);
	g_assert_no_error(error);
	rows = rows_of(f, VENTURE_TYPE_MARKETING_RECIPIENT);
	for (i = rows->len; i > 0; i--) {
		VentureEntity *row = g_ptr_array_index(rows, i - 1);
		gint64 mail = 0;
		g_object_get(row, "mail-id", &mail, NULL);
		if (mail) return g_object_ref(row);
	}
	g_assert_not_reached();
	return NULL;
}
static gchar *mail_message_id(Fixture *f, VentureEntity *recipient)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) mail = NULL;
	gint64 mail_id = 0;
	gchar *id = NULL;
	g_object_get(recipient, "mail-id", &mail_id, NULL);
	mail = venture_database_get(f->db, VENTURE_TYPE_MAIL_MESSAGE, mail_id, &error);
	g_assert_no_error(error);
	g_object_get(mail, "message-id", &id, NULL);
	g_assert_nonnull(id);
	return id;
}
static VentureEntity *reread(Fixture *f, VentureEntity *row)
{
	g_autoptr(GError) error = NULL;
	VentureEntity *fresh = venture_database_get(f->db, G_OBJECT_TYPE(row), venture_entity_get_id(row), &error);
	g_assert_no_error(error);
	return fresh;
}
static gboolean has_date(VentureEntity *row, const gchar *field)
{
	g_autoptr(GDateTime) at = NULL;
	g_object_get(row, field, &at, NULL);
	return at != NULL;
}
static guint suppressions(Fixture *f)
{
	g_autoptr(GPtrArray) rows = rows_of(f, VENTURE_TYPE_SUPPRESSION);
	return rows->len;
}
/* A fixture file with its placeholders filled in. */
static GBytes *fixture(const gchar *name, const gchar *message_id, const gchar *recipient)
{
	g_autofree gchar *text = NULL, *path = g_strdup(name);
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autofree gchar *occurred = g_date_time_format_iso8601(now);
	g_autoptr(GString) body = NULL;
	g_assert_true(g_file_get_contents(path, &text, NULL, &error));
	g_assert_no_error(error);
	body = g_string_new(text);
	g_string_replace(body, "{{MESSAGE_ID}}", message_id ? message_id : "", 0);
	g_string_replace(body, "{{RECIPIENT}}", recipient ? recipient : "", 0);
	g_string_replace(body, "{{OCCURRED_AT}}", occurred, 0);
	return g_string_free_to_bytes(g_steal_pointer(&body));
}
static gchar *sign(const gchar *secret, const gchar *timestamp, GBytes *body)
{
	g_autoptr(GHmac) hmac = g_hmac_new(G_CHECKSUM_SHA256, (const guchar *)secret, strlen(secret));
	gsize length = 0;
	const guint8 *bytes = g_bytes_get_data(body, &length);
	g_hmac_update(hmac, (const guchar *)timestamp, strlen(timestamp));
	g_hmac_update(hmac, (const guchar *)"\n", 1);
	g_hmac_update(hmac, bytes, length);
	return g_strdup(g_hmac_get_string(hmac));
}
static void configure_feed(Fixture *f)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *secret = venture_deliverability_set_feed_secret(f->db, 1, SECRET, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(secret, ==, SECRET);
}
/* Delivers @body to the feed, signed at @age seconds old with @secret. */
static JsonNode *deliver(Fixture *f, GBytes *body, const gchar *secret, gint64 age, GError **error)
{
	g_autoptr(GDateTime) now = venture_time_now();
	g_autofree gchar *timestamp = g_strdup_printf("%" G_GINT64_FORMAT, g_date_time_to_unix(now) - age);
	g_autofree gchar *signature = sign(secret, timestamp, body);
	return venture_deliverability_receive(f->db, 1, "generic", timestamp, signature, body, now, error);
}

/* ------------------------------------------------------------------ */
/* The signature is the feed's only guard: a wrong secret, a changed body,
 * an old timestamp or a missing header must all be refused alike, and a
 * comparison that stops at the first wrong byte leaks the right one. */
static void test_signature(void)
{
	g_autoptr(GBytes) body = g_bytes_new_static("{\"events\":[]}", 13);
	g_autoptr(GBytes) changed = g_bytes_new_static("{\"events\":[ ]}", 14);
	g_autoptr(GDateTime) now = venture_time_now();
	g_autofree gchar *timestamp = g_strdup_printf("%" G_GINT64_FORMAT, g_date_time_to_unix(now));
	g_autofree gchar *stale = g_strdup_printf("%" G_GINT64_FORMAT, g_date_time_to_unix(now) - VENTURE_DELIVERABILITY_TOLERANCE_SECONDS - 1);
	g_autofree gchar *future = g_strdup_printf("%" G_GINT64_FORMAT, g_date_time_to_unix(now) + VENTURE_DELIVERABILITY_TOLERANCE_SECONDS + 1);
	g_autofree gchar *good = sign(SECRET, timestamp, body), *prefixed = g_strconcat("sha256=", good, NULL);
	g_autofree gchar *old = sign(SECRET, stale, body), *ahead = sign(SECRET, future, body);
	g_autofree gchar *wrong = sign("another-secret-another-secret-another-secret", timestamp, body);
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_deliverability_verify_signature(SECRET, timestamp, good, body, now, &error));
	g_assert_no_error(error);
	g_assert_true(venture_deliverability_verify_signature(SECRET, timestamp, prefixed, body, now, &error));
	g_assert_no_error(error);
	g_assert_false(venture_deliverability_verify_signature(SECRET, timestamp, wrong, body, now, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED); g_clear_error(&error);
	g_assert_false(venture_deliverability_verify_signature(SECRET, timestamp, good, changed, now, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED); g_clear_error(&error);
	g_assert_false(venture_deliverability_verify_signature(SECRET, stale, old, body, now, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED); g_clear_error(&error);
	g_assert_false(venture_deliverability_verify_signature(SECRET, future, ahead, body, now, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED); g_clear_error(&error);
	g_assert_false(venture_deliverability_verify_signature(SECRET, NULL, good, body, now, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED); g_clear_error(&error);
	g_assert_false(venture_deliverability_verify_signature(SECRET, timestamp, NULL, body, now, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED); g_clear_error(&error);
	g_assert_false(venture_deliverability_verify_signature(SECRET, "12x45", good, body, now, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED); g_clear_error(&error);
}

/* An organization that never set a secret is refused, not trusted: an
 * unsigned feed would let anybody suppress any address. */
static void test_feed_unconfigured(Fixture *f, gconstpointer data)
{
	g_autoptr(GBytes) body = g_bytes_new_static("{\"events\":[]}", 13);
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) result = NULL;
	(void)data;
	result = deliver(f, body, SECRET, 0, &error);
	g_assert_null(result);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
}

/* A replayed delivery -- the same signed bytes, sent again later -- is
 * refused by its timestamp; a replayed event inside a fresh delivery is
 * recorded once by its id. */
static void test_feed_replay(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) recipient = NULL;
	g_autofree gchar *message_id = NULL;
	g_autoptr(GBytes) body = NULL;
	g_autoptr(JsonNode) first = NULL, second = NULL, stale = NULL;
	g_autoptr(GPtrArray) events = NULL;
	g_autoptr(GError) error = NULL;
	(void)data;
	configure_feed(f);
	permit(f, f->contact, "replay-permission");
	preview_and_approve(f);
	recipient = send_next(f);
	message_id = mail_message_id(f, recipient);
	body = fixture(FIXTURE("generic-soft-bounce.json"), message_id, "alice@example.net");
	first = deliver(f, body, SECRET, 0, &error);
	g_assert_no_error(error);
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(first), "accepted"), ==, 1);
	second = deliver(f, body, SECRET, 1, &error);
	g_assert_no_error(error);
	g_assert_nonnull(second);
	events = rows_of(f, VENTURE_TYPE_MARKETING_EVENT);
	g_assert_cmpuint(events->len, ==, 1);
	g_test_expect_message("Venture", G_LOG_LEVEL_WARNING, "*refused*");
	stale = deliver(f, body, SECRET, VENTURE_DELIVERABILITY_TOLERANCE_SECONDS + 60, &error);
	g_test_assert_expected_messages();
	g_assert_null(stale);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_assert_cmpuint(suppressions(f), ==, 0);
}

/* The recorded provider payloads reach the same evidence and suppression
 * the operator's Record delivery feedback action writes: a hard bounce and
 * a complaint suppress, a soft bounce does not, delivered is counted, and
 * mail this organization never sent is ignored rather than refused. */
static void test_feed_fixtures(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) second = contact(f, "Bob", "bob@example.net");
	g_autoptr(VentureEntity) alice = NULL, bob = NULL, fresh = NULL;
	g_autofree gchar *alice_id = NULL, *bob_id = NULL;
	g_autoptr(GBytes) delivered = NULL, hard = NULL, complaint = NULL;
	g_autoptr(JsonNode) result = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	(void)data;
	member(f, second);
	configure_feed(f);
	permit(f, f->contact, "fixture-alice");
	permit(f, second, "fixture-bob");
	preview_and_approve(f);
	alice = send_next(f);
	g_usleep(1100 * 1000);
	bob = send_next(f);
	alice_id = mail_message_id(f, alice);
	bob_id = mail_message_id(f, bob);

	delivered = fixture(FIXTURE("generic-delivered.json"), alice_id, "alice@example.net");
	result = deliver(f, delivered, SECRET, 0, &error);
	g_assert_no_error(error);
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(result), "accepted"), ==, 1);
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(result), "ignored"), ==, 1);
	fresh = reread(f, alice);
	g_assert_true(has_date(fresh, "delivered-at"));
	g_clear_object(&fresh);
	g_assert_cmpuint(suppressions(f), ==, 0);
	g_clear_pointer(&result, json_node_unref);

	hard = fixture(FIXTURE("generic-hard-bounce.json"), alice_id, "alice@example.net");
	result = deliver(f, hard, SECRET, 0, &error);
	g_assert_no_error(error);
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(result), "accepted"), ==, 1);
	fresh = reread(f, alice);
	g_assert_true(has_date(fresh, "bounced-at"));
	g_clear_object(&fresh);
	g_assert_cmpuint(suppressions(f), ==, 1);
	g_clear_pointer(&result, json_node_unref);

	complaint = fixture(FIXTURE("generic-complaint.json"), bob_id, "bob@example.net");
	result = deliver(f, complaint, SECRET, 0, &error);
	g_assert_no_error(error);
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(result), "accepted"), ==, 1);
	fresh = reread(f, bob);
	g_assert_true(has_date(fresh, "complained-at"));
	g_assert_false(has_date(fresh, "bounced-at"));
	g_assert_cmpuint(suppressions(f), ==, 2);
	rows = rows_of(f, VENTURE_TYPE_MARKETING_EVENT);
	g_assert_cmpuint(rows->len, ==, 3);
	g_test_message("delivered, hard bounce and complaint fixtures: %u events, %u suppressions", rows->len, suppressions(f));
}

/* An event naming our Message-ID but another address is not evidence
 * about this recipient: a forwarded copy bouncing elsewhere must not
 * suppress the person we wrote to. */
static void test_feed_mismatch(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) recipient = NULL;
	g_autofree gchar *message_id = NULL;
	g_autoptr(GBytes) body = NULL;
	g_autoptr(JsonNode) result = NULL;
	g_autoptr(GError) error = NULL;
	(void)data;
	configure_feed(f);
	permit(f, f->contact, "mismatch-permission");
	preview_and_approve(f);
	recipient = send_next(f);
	message_id = mail_message_id(f, recipient);
	body = fixture(FIXTURE("generic-hard-bounce.json"), message_id, "forwarded@example.org");
	result = deliver(f, body, SECRET, 0, &error);
	g_assert_no_error(error);
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(result), "accepted"), ==, 0);
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(result), "ignored"), ==, 1);
	g_assert_cmpuint(suppressions(f), ==, 0);
}

/* The DSN parser reads the machine part, never the prose: hard is failed
 * with 5.x.x, soft is delayed or 4.x.x, and the Message-ID comes from the
 * report, not the bounce's own. */
static void test_dsn_parse(void)
{
	g_autoptr(GBytes) hard = fixture(FIXTURE("dsn-hard.eml"), "abc-1@venture.invalid", "carol@example.net");
	g_autoptr(GBytes) soft = fixture(FIXTURE("dsn-soft.eml"), "abc-2@venture.invalid", "dave@example.net");
	g_autoptr(GBytes) arf = fixture(FIXTURE("arf-complaint.eml"), "abc-3@venture.invalid", "erin@example.net");
	g_autoptr(GBytes) plain = fixture(FIXTURE("not-a-report.eml"), "abc-4@venture.invalid", NULL);
	g_autoptr(GPtrArray) events = NULL;
	g_autoptr(GError) error = NULL;
	VentureDeliverabilityEvent *event;

	events = venture_deliverability_parse_dsn(hard, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(events->len, ==, 1);
	event = g_ptr_array_index(events, 0);
	g_assert_cmpint(event->kind, ==, VENTURE_DELIVERABILITY_HARD_BOUNCE);
	g_assert_cmpstr(event->recipient, ==, "carol@example.net");
	g_assert_cmpstr(event->status, ==, "5.1.1");
	g_assert_cmpstr(event->message_id, ==, "abc-1@venture.invalid");
	g_assert_nonnull(event->detail);
	g_assert_nonnull(strstr(event->detail, "User unknown"));
	g_clear_pointer(&events, g_ptr_array_unref);

	events = venture_deliverability_parse_dsn(soft, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(events->len, ==, 1);
	event = g_ptr_array_index(events, 0);
	g_assert_cmpint(event->kind, ==, VENTURE_DELIVERABILITY_SOFT_BOUNCE);
	g_assert_cmpstr(event->recipient, ==, "dave@example.net");
	g_assert_cmpstr(event->status, ==, "4.2.2");
	g_assert_cmpstr(event->message_id, ==, "abc-2@venture.invalid");
	g_clear_pointer(&events, g_ptr_array_unref);

	events = venture_deliverability_parse_dsn(arf, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(events->len, ==, 1);
	event = g_ptr_array_index(events, 0);
	g_assert_cmpint(event->kind, ==, VENTURE_DELIVERABILITY_COMPLAINT);
	g_assert_cmpstr(event->recipient, ==, "erin@example.net");
	g_assert_cmpstr(event->message_id, ==, "abc-3@venture.invalid");
	g_clear_pointer(&events, g_ptr_array_unref);

	events = venture_deliverability_parse_dsn(plain, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(events->len, ==, 0);
}

/* A DSN in the synced mailbox about our own campaign message suppresses
 * exactly that recipient; one about mail we never sent changes nothing. */
static void test_dsn_match(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) recipient = NULL, fresh = NULL;
	g_autofree gchar *message_id = NULL;
	g_autoptr(GBytes) hard = NULL, stranger = NULL, soft = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GError) error = NULL;
	(void)data;
	permit(f, f->contact, "dsn-permission");
	preview_and_approve(f);
	recipient = send_next(f);
	message_id = mail_message_id(f, recipient);

	stranger = fixture(FIXTURE("dsn-hard.eml"), "someone-else@example.org", "alice@example.net");
	g_assert_cmpint(venture_deliverability_apply_dsn(f->db, 1, stranger, now, &error), ==, 0);
	g_assert_no_error(error);
	g_assert_cmpuint(suppressions(f), ==, 0);

	soft = fixture(FIXTURE("dsn-soft.eml"), message_id, "alice@example.net");
	g_assert_cmpint(venture_deliverability_apply_dsn(f->db, 1, soft, now, &error), ==, 1);
	g_assert_no_error(error);
	g_assert_cmpuint(suppressions(f), ==, 0);

	hard = fixture(FIXTURE("dsn-hard.eml"), message_id, "alice@example.net");
	g_assert_cmpint(venture_deliverability_apply_dsn(f->db, 1, hard, now, &error), ==, 1);
	g_assert_no_error(error);
	g_assert_cmpint(venture_deliverability_apply_dsn(f->db, 1, hard, now, &error), ==, 1);
	g_assert_no_error(error);
	fresh = reread(f, recipient);
	g_assert_true(has_date(fresh, "bounced-at"));
	g_assert_cmpuint(suppressions(f), ==, 1);
	{
		g_autoptr(GPtrArray) events = rows_of(f, VENTURE_TYPE_MARKETING_EVENT);
		g_assert_cmpuint(events->len, ==, 2);
	}
}

/* A reply is what sequences call one: an inbound interaction for the
 * contact. It counts for the most recent campaign message sent to them,
 * once, and an outbound one or one from before the send never counts. */
static void test_replies(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) recipient = NULL, fresh = NULL, before = NULL, outbound = NULL, reply = NULL, again = NULL;
	g_autoptr(GDateTime) now = NULL, earlier = NULL;
	g_autoptr(VentureMarketingSendReport) report = NULL;
	g_autoptr(GError) error = NULL;
	(void)data;
	permit(f, f->contact, "reply-permission");
	preview_and_approve(f);
	earlier = g_date_time_add_days(venture_time_now(), -2);
	before = g_object_new(VENTURE_TYPE_INTERACTION, "organization-id", (gint64)1, "contact-id", venture_entity_get_id(f->contact),
		"kind", VENTURE_INTERACTION_KIND_EMAIL, "subject", "Earlier", "occurred-at", earlier, "outbound", FALSE, NULL);
	recipient = send_next(f);
	persist(f, before);
	fresh = reread(f, recipient);
	g_assert_false(has_date(fresh, "replied-at"));
	g_clear_object(&fresh);
	now = venture_time_now();
	outbound = g_object_new(VENTURE_TYPE_INTERACTION, "organization-id", (gint64)1, "contact-id", venture_entity_get_id(f->contact),
		"kind", VENTURE_INTERACTION_KIND_EMAIL, "subject", "Our follow-up", "occurred-at", now, "outbound", TRUE, NULL);
	persist(f, outbound);
	fresh = reread(f, recipient);
	g_assert_false(has_date(fresh, "replied-at"));
	g_clear_object(&fresh);
	reply = g_object_new(VENTURE_TYPE_INTERACTION, "organization-id", (gint64)1, "contact-id", venture_entity_get_id(f->contact),
		"kind", VENTURE_INTERACTION_KIND_EMAIL, "subject", "Re: Hello Alice", "occurred-at", now, "outbound", FALSE, NULL);
	persist(f, reply);
	again = g_object_new(VENTURE_TYPE_INTERACTION, "organization-id", (gint64)1, "contact-id", venture_entity_get_id(f->contact),
		"kind", VENTURE_INTERACTION_KIND_EMAIL, "subject", "Re: Re: Hello Alice", "occurred-at", now, "outbound", FALSE, NULL);
	persist(f, again);
	fresh = reread(f, recipient);
	g_assert_true(has_date(fresh, "replied-at"));
	report = venture_marketing_send_report(f->db, 1, venture_entity_get_id(f->send), &error);
	g_assert_no_error(error);
	g_assert_cmpint(report->replied, ==, 1);
}

/* ------------------------------------------------------------------ */
/* Domain checks, through the fake resolver only. */

static const gchar *check_status(JsonNode *node, const gchar *id)
{
	JsonArray *checks = json_object_get_array_member(json_node_get_object(node), "checks");
	guint i;
	for (i = 0; i < json_array_get_length(checks); i++) {
		JsonObject *check = json_array_get_object_element(checks, i);
		if (g_strcmp0(json_object_get_string_member(check, "id"), id) == 0)
			return json_object_get_string_member(check, "status");
	}
	return NULL;
}
static const gchar *check_fix(JsonNode *node, const gchar *id)
{
	JsonArray *checks = json_object_get_array_member(json_node_get_object(node), "checks");
	guint i;
	for (i = 0; i < json_array_get_length(checks); i++) {
		JsonObject *check = json_array_get_object_element(checks, i);
		if (g_strcmp0(json_object_get_string_member(check, "id"), id) == 0)
			return json_object_get_string_member_with_default(check, "fix", "");
	}
	return NULL;
}

/* A domain set up properly passes all three, and the DKIM lookup goes to
 * the configured selector, not a guessed one. */
static void test_dns_pass(void)
{
	g_autoptr(TestFakeResolver) resolver = g_object_new(TEST_TYPE_FAKE_RESOLVER, NULL);
	g_autoptr(JsonNode) result = NULL;
	g_autoptr(GError) error = NULL;
	guint i;
	gboolean asked_selector = FALSE;
	fake_add(resolver, "example.com", "v=spf1 include:_spf.example.net -all", "google-site-verification=abc");
	fake_add(resolver, "mail2026._domainkey.example.com", "v=DKIM1; k=rsa; p=MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEA", NULL);
	fake_add(resolver, "_dmarc.example.com", "v=DMARC1; p=reject; rua=mailto:dmarc@example.com", NULL);
	result = venture_deliverability_check_domain(VENTURE_DNS_RESOLVER(resolver), "Example.COM", "mail2026", &error);
	g_assert_no_error(error);
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(result), "domain"), ==, "example.com");
	g_assert_cmpstr(check_status(result, "spf"), ==, "pass");
	g_assert_cmpstr(check_status(result, "dkim"), ==, "pass");
	g_assert_cmpstr(check_status(result, "dmarc"), ==, "pass");
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(result), "overall"), ==, "pass");
	for (i = 0; i < resolver->asked->len; i++)
		if (g_strcmp0(g_ptr_array_index(resolver->asked, i), "mail2026._domainkey.example.com") == 0) asked_selector = TRUE;
	g_assert_true(asked_selector);
}

/* Nothing published: each check says it is missing and what to add. */
static void test_dns_missing(void)
{
	g_autoptr(TestFakeResolver) resolver = g_object_new(TEST_TYPE_FAKE_RESOLVER, NULL);
	g_autoptr(JsonNode) result = NULL;
	g_autoptr(GError) error = NULL;
	result = venture_deliverability_check_domain(VENTURE_DNS_RESOLVER(resolver), "example.net", "default", &error);
	g_assert_no_error(error);
	g_assert_cmpstr(check_status(result, "spf"), ==, "missing");
	g_assert_cmpstr(check_status(result, "dkim"), ==, "missing");
	g_assert_cmpstr(check_status(result, "dmarc"), ==, "missing");
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(result), "overall"), ==, "missing");
	g_assert_cmpstr(check_fix(result, "spf"), !=, "");
	g_assert_cmpstr(check_fix(result, "dmarc"), !=, "");
	g_assert_null(venture_deliverability_check_domain(VENTURE_DNS_RESOLVER(resolver), "not a domain", "default", &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

/* A DMARC policy of none only watches, an SPF of +all lets anybody send,
 * and two SPF records are an error at the receiver: weak, never pass. */
static void test_dns_weak(void)
{
	g_autoptr(TestFakeResolver) resolver = g_object_new(TEST_TYPE_FAKE_RESOLVER, NULL);
	g_autoptr(JsonNode) result = NULL, doubled = NULL;
	g_autoptr(GError) error = NULL;
	fake_add(resolver, "example.org", "v=spf1 +all", NULL);
	fake_add(resolver, "default._domainkey.example.org", "v=DKIM1; p=", NULL);
	fake_add(resolver, "_dmarc.example.org", "v=DMARC1; p=none", NULL);
	result = venture_deliverability_check_domain(VENTURE_DNS_RESOLVER(resolver), "example.org", "default", &error);
	g_assert_no_error(error);
	g_assert_cmpstr(check_status(result, "spf"), ==, "weak");
	g_assert_cmpstr(check_status(result, "dkim"), ==, "missing");
	g_assert_cmpstr(check_status(result, "dmarc"), ==, "weak");
	g_assert_nonnull(strstr(check_fix(result, "dmarc"), "quarantine"));
	fake_add(resolver, "example.com", "v=spf1 -all", "v=spf1 include:_spf.example.net ~all");
	doubled = venture_deliverability_check_domain(VENTURE_DNS_RESOLVER(resolver), "example.com", NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(check_status(doubled, "spf"), ==, "weak");
}

/* ------------------------------------------------------------------ */
/* Warm-up. */

static void test_ramp_schedule(void)
{
	g_autoptr(GError) error = NULL;
	g_assert_cmpint(venture_deliverability_ramp_limit(NULL, 1, &error), ==, 50);
	g_assert_cmpint(venture_deliverability_ramp_limit("", 2, &error), ==, 50);
	g_assert_cmpint(venture_deliverability_ramp_limit(NULL, 3, &error), ==, 100);
	g_assert_cmpint(venture_deliverability_ramp_limit(NULL, 29, &error), ==, 5000);
	g_assert_cmpint(venture_deliverability_ramp_limit(NULL, 30, &error), ==, 0);
	g_assert_cmpint(venture_deliverability_ramp_limit(NULL, 400, &error), ==, 0);
	g_assert_cmpint(venture_deliverability_ramp_limit("1:5,4:20", 3, &error), ==, 5);
	g_assert_cmpint(venture_deliverability_ramp_limit("1:5,4:20", 9, &error), ==, 20);
	g_assert_cmpint(venture_deliverability_ramp_limit("off", 1, &error), ==, 0);
	g_assert_no_error(error);
	g_assert_cmpint(venture_deliverability_ramp_limit("4:20,1:5", 1, &error), ==, -1);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	g_assert_cmpint(venture_deliverability_ramp_limit("1:lots", 1, &error), ==, -1);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

/* A new domain's daily limit holds the bounded delivery until tomorrow
 * (UTC) instead of skipping anyone: the next recipient waits, then goes. */
static void test_ramp_enforced(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) second = contact(f, "Bob", "bob@example.net");
	g_autoptr(VentureEntity) third = contact(f, "Carol", "carol@example.net");
	g_autoptr(VentureEntity) domain = NULL, send = NULL;
	g_autoptr(GDateTime) now = venture_time_now(), later = NULL, tomorrow = NULL, next = NULL, day_three = NULL;
	g_autoptr(GError) error = NULL;
	gint64 send_id, day = 0, limit = 0, used = 0;
	/* The run's clock must not be earlier than the approval. */
	(void)data;
	member(f, second);
	member(f, third);
	permit(f, f->contact, "ramp-alice");
	permit(f, second, "ramp-bob");
	permit(f, third, "ramp-carol");
	domain = g_object_new(VENTURE_TYPE_MARKETING_SENDING_DOMAIN, "organization-id", (gint64)1,
		"domain", "example.com", "dkim-selector", "mail", "warmup-started-on", now, "ramp-schedule", "1:1,3:0", NULL);
	persist(f, domain);
	preview_and_approve(f);
	send_id = venture_entity_get_id(f->send);
	g_clear_pointer(&now, g_date_time_unref);
	now = venture_time_now();
	g_assert_cmpint(venture_marketing_service_run(f->service, 1, send_id, 100, now, NULL, &error), ==, 1);
	g_assert_no_error(error);
	g_assert_cmpint(venture_mail_outbox_deliver_due(f->outbox, 1, 1, now, NULL, &error), ==, 1);
	g_assert_no_error(error);
	g_assert_true(venture_deliverability_ramp_status(f->db, 1, now, &day, &limit, &used, &error));
	g_assert_no_error(error);
	g_assert_cmpint(day, ==, 1);
	g_assert_cmpint(limit, ==, 1);
	g_assert_cmpint(used, ==, 1);
	later = g_date_time_add_seconds(now, 5);
	g_assert_cmpint(venture_marketing_service_run(f->service, 1, send_id, 100, later, NULL, &error), ==, 0);
	g_assert_no_error(error);
	send = venture_database_get(f->db, VENTURE_TYPE_MARKETING_SEND, send_id, &error);
	g_object_get(send, "next-enqueue-at", &next, NULL);
	g_assert_nonnull(next);
	g_assert_cmpint(g_date_time_get_hour(next), ==, 0);
	g_assert_cmpint(g_date_time_difference(next, now), >, 0);
	tomorrow = g_date_time_add_seconds(next, 30);
	g_assert_cmpint(venture_marketing_service_run(f->service, 1, send_id, 100, tomorrow, NULL, &error), ==, 1);
	g_assert_no_error(error);
	day_three = g_date_time_add_days(tomorrow, 1);
	g_assert_true(venture_deliverability_ramp_status(f->db, 1, day_three, &day, &limit, &used, &error));
	g_assert_cmpint(day, ==, 3);
	g_assert_cmpint(limit, ==, 0);
	g_test_message("Day one sent 1 and held the rest until %s", g_date_time_format_iso8601(next));
}

/* Without a sending domain record the default schedule applies from the
 * first accepted campaign message; "off" turns the warm-up off. */
static void test_ramp_default(Fixture *f, gconstpointer data)
{
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(VentureEntity) domain = NULL;
	g_autoptr(GError) error = NULL;
	gint64 day = 0, limit = 0, used = 0;
	(void)data;
	g_assert_true(venture_deliverability_ramp_status(f->db, 1, now, &day, &limit, &used, &error));
	g_assert_no_error(error);
	g_assert_cmpint(day, ==, 1);
	g_assert_cmpint(limit, ==, 50);
	g_assert_cmpint(used, ==, 0);
	domain = g_object_new(VENTURE_TYPE_MARKETING_SENDING_DOMAIN, "organization-id", (gint64)1,
		"domain", "example.com", "ramp-schedule", "off", NULL);
	persist(f, domain);
	g_assert_true(venture_deliverability_ramp_status(f->db, 1, now, &day, &limit, &used, &error));
	g_assert_cmpint(limit, ==, 0);
	g_object_set(domain, "ramp-schedule", "3:9,1:4", NULL);
	g_assert_false(venture_database_save(f->db, domain, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

/* ------------------------------------------------------------------ */
/* The report. */

/* The heuristic itself, at its edges. */
static void test_machine_reason(void)
{
	g_autoptr(GDateTime) sent = g_date_time_new_utc(2026, 10, 18, 12, 0, 0);
	g_autoptr(GDateTime) soon = g_date_time_add_seconds(sent, 3), late = g_date_time_add_seconds(sent, 3600);
	g_assert_nonnull(venture_marketing_machine_reason(sent, soon, NULL, 10));
	g_assert_null(venture_marketing_machine_reason(sent, soon, NULL, 0));
	g_assert_null(venture_marketing_machine_reason(sent, late, "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7)", 10));
	g_assert_null(venture_marketing_machine_reason(sent, late, "Mozilla/5.0 (Windows NT 5.1; rv:11.0) Gecko Firefox/11.0 (via ggpht.com GoogleImageProxy)", 10));
	g_assert_nonnull(venture_marketing_machine_reason(sent, late, "YahooMailProxy; https://help.yahoo.com/kb/yahoo-mail-proxy-SLN28749.html", 10));
	g_assert_nonnull(venture_marketing_machine_reason(sent, late, "Barracuda Sentinel (EE)", 10));
	g_assert_nonnull(venture_marketing_machine_reason(sent, late, "python-requests/2.31", 10));
	g_assert_nonnull(venture_marketing_machine_reason(NULL, late, "Mozilla/5.0 (compatible; bingbot/2.0)", 10));
}

static gchar *tracking_token(VentureEntity *recipient)
{
	g_autofree gchar *html = NULL;
	const gchar *pixel;
	g_object_get(recipient, "private-html-body", &html, NULL);
	pixel = strstr(html, "/marketing/t/o/");
	g_assert_nonnull(pixel);
	return g_strndup(pixel + strlen("/marketing/t/o/"), 64);
}
static gchar *unsubscribe_token(VentureEntity *recipient)
{
	g_autofree gchar *url = NULL;
	g_object_get(recipient, "unsubscribe-url", &url, NULL);
	return g_strdup(strrchr(url, '/') + 1);
}

/* Each number in the send's report comes from its own evidence, machine
 * opens are counted apart and never as opens, and the click map counts
 * people per link with machine clicks beside them. */
static void test_report(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) bob_contact = contact(f, "Bob", "bob@example.net");
	g_autoptr(VentureEntity) carol_contact = contact(f, "Carol", "carol@example.net");
	g_autoptr(VentureEntity) alice = NULL, bob = NULL, carol = NULL, reply = NULL;
	g_autofree gchar *alice_token = NULL, *bob_token = NULL, *bob_unsubscribe = NULL, *alice_id = NULL, *bob_id = NULL, *carol_id = NULL;
	g_autofree gchar *url = NULL;
	g_autoptr(VentureMarketingSendReport) report = NULL;
	g_autoptr(GBytes) delivered = NULL, complaint = NULL, hard = NULL;
	g_autoptr(JsonNode) result = NULL, json = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autoptr(GError) error = NULL;
	VentureMarketingLinkCount *first, *second;
	(void)data;
	{
		g_autoptr(VentureEntity) org = venture_database_get(f->db, VENTURE_TYPE_ORGANIZATION, 1, NULL);
		g_object_set(org, "marketing-tracking", TRUE, NULL);
		persist(f, org);
		g_object_set(f->send, "tracking", TRUE, NULL);
		persist(f, f->send);
	}
	member(f, bob_contact);
	member(f, carol_contact);
	permit(f, f->contact, "report-alice");
	permit(f, bob_contact, "report-bob");
	permit(f, carol_contact, "report-carol");
	configure_feed(f);
	preview_and_approve(f);
	alice = send_next(f);
	g_usleep(1100 * 1000);
	bob = send_next(f);
	g_usleep(1100 * 1000);
	carol = send_next(f);
	alice_id = mail_message_id(f, alice);
	bob_id = mail_message_id(f, bob);
	carol_id = mail_message_id(f, carol);
	alice_token = tracking_token(alice);
	bob_token = tracking_token(bob);
	bob_unsubscribe = unsubscribe_token(bob);

	/* Alice: an image proxy fetching on arrival, then her own open and
	 * clicks, then a reply. The timing rule is on: the first open lands
	 * within seconds of acceptance. */
	now = venture_time_now();
	g_assert_true(venture_marketing_service_record_open(f->service, alice_token, NULL, now, &error));
	g_assert_no_error(error);
	venture_marketing_service_set_machine_seconds(f->service, 0);
	g_assert_true(venture_marketing_service_record_open(f->service, alice_token, "Mozilla/5.0 (X11; Linux x86_64) Firefox/131.0", now, &error));
	g_assert_no_error(error);
	url = venture_marketing_service_record_click(f->service, alice_token, 1, "Mozilla/5.0 (X11; Linux x86_64) Firefox/131.0", now, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(url, ==, "https://example.com/news?utm=x");
	g_clear_pointer(&url, g_free);
	url = venture_marketing_service_record_click(f->service, alice_token, 2, "Mozilla/5.0 (X11; Linux x86_64) Firefox/131.0", now, &error);
	g_assert_no_error(error);
	g_clear_pointer(&url, g_free);
	reply = g_object_new(VENTURE_TYPE_INTERACTION, "organization-id", (gint64)1, "contact-id", venture_entity_get_id(f->contact),
		"kind", VENTURE_INTERACTION_KIND_EMAIL, "subject", "Re: Hello Alice", "occurred-at", now, "outbound", FALSE, NULL);
	persist(f, reply);

	/* Bob: only a link scanner opened and clicked; then he unsubscribed
	 * and complained. */
	g_assert_true(venture_marketing_service_record_open(f->service, bob_token, "Barracuda Sentinel (EE)", now, &error));
	g_assert_no_error(error);
	url = venture_marketing_service_record_click(f->service, bob_token, 1, "Barracuda Sentinel (EE)", now, &error);
	g_assert_no_error(error);
	g_clear_pointer(&url, g_free);
	g_assert_true(venture_marketing_service_unsubscribe(f->service, bob_unsubscribe, &error));
	g_assert_no_error(error);
	complaint = fixture(FIXTURE("generic-complaint.json"), bob_id, "bob@example.net");
	result = deliver(f, complaint, SECRET, 0, &error);
	g_assert_no_error(error);
	g_clear_pointer(&result, json_node_unref);

	/* Alice delivered; Carol hard bounced. */
	delivered = fixture(FIXTURE("generic-delivered.json"), alice_id, "alice@example.net");
	result = deliver(f, delivered, SECRET, 0, &error);
	g_assert_no_error(error);
	g_clear_pointer(&result, json_node_unref);
	hard = fixture(FIXTURE("generic-hard-bounce.json"), carol_id, "carol@example.net");
	result = deliver(f, hard, SECRET, 0, &error);
	g_assert_no_error(error);

	report = venture_marketing_send_report(f->db, 1, venture_entity_get_id(f->send), &error);
	g_assert_no_error(error);
	g_assert_nonnull(report);
	g_assert_cmpint(report->eligible, ==, 3);
	g_assert_cmpint(report->accepted, ==, 3);
	g_assert_cmpint(report->delivered, ==, 1);
	g_assert_cmpint(report->opened, ==, 1);
	g_assert_cmpint(report->machine_opened, ==, 1);
	g_assert_cmpint(report->clicked, ==, 1);
	g_assert_cmpint(report->replied, ==, 1);
	g_assert_cmpint(report->unsubscribed, ==, 1);
	g_assert_cmpint(report->bounced, ==, 1);
	g_assert_cmpint(report->complained, ==, 1);
	g_assert_cmpuint(report->links->len, ==, 2);
	first = g_ptr_array_index(report->links, 0);
	second = g_ptr_array_index(report->links, 1);
	g_assert_cmpint(first->position, ==, 1);
	g_assert_cmpstr(first->destination, ==, "https://example.com/news");
	g_assert_cmpint(first->clicks, ==, 1);
	g_assert_cmpint(first->machine_clicks, ==, 1);
	g_assert_cmpint(second->clicks, ==, 1);
	g_assert_cmpint(second->machine_clicks, ==, 0);
	json = venture_marketing_send_report_to_json(report);
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(json), "machine_opened"), ==, 1);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(json), "links")), ==, 2);
	{
		g_autoptr(VentureConfig) config = venture_config_new();
		g_autoptr(VentureContext) context = venture_context_new(config, f->db);
		VentureReport *definition = venture_report_registry_lookup(venture_context_get_report_registry(context), "marketing_performance");
		g_autoptr(VentureReportResult) table = venture_report_generate(definition, context, NULL, NULL, &error);
		g_autoptr(JsonNode) node = NULL;
		JsonObject *row;
		g_assert_no_error(error);
		node = venture_report_result_to_json(table);
		row = json_array_get_object_element(json_object_get_array_member(json_node_get_object(node), "rows"), 0);
		g_assert_cmpint(json_object_get_int_member(row, "uniquely_opened"), ==, 1);
		g_assert_cmpint(json_object_get_int_member(row, "machine_opened"), ==, 1);
		g_assert_cmpint(json_object_get_int_member(row, "replied"), ==, 1);
		g_assert_cmpint(json_object_get_int_member(row, "complained"), ==, 1);
		g_assert_cmpint(json_object_get_int_member(row, "delivered"), ==, 1);
	}
	g_test_message("opened=%" G_GINT64_FORMAT " machine=%" G_GINT64_FORMAT " clicked=%" G_GINT64_FORMAT
		" replied=%" G_GINT64_FORMAT, report->opened, report->machine_opened, report->clicked, report->replied);
}

/* ------------------------------------------------------------------ */
/* The routes: the feed answers only signed deliveries; the domain page,
 * its API and the send report need a session or token, and the domain
 * check needs an owner or administrator. */

typedef struct { gboolean done; GBytes *body; GError *error; } Reply;
static void http_done(GObject *source, GAsyncResult *result, gpointer data)
{
	Reply *reply = data;
	reply->body = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &reply->error);
	reply->done = TRUE;
}
static guint http(SoupSession *session, const gchar *base, const gchar *method, const gchar *path,
	const gchar *bearer, GBytes *body, const gchar *timestamp, const gchar *signature, gchar **text)
{
	g_autofree gchar *url = g_strconcat(base, path, NULL);
	g_autoptr(SoupMessage) message = soup_message_new(method, url);
	SoupMessageHeaders *headers = soup_message_get_request_headers(message);
	Reply reply = { FALSE, NULL, NULL };
	guint status;
	if (bearer) {
		g_autofree gchar *value = g_strconcat("Bearer ", bearer, NULL);
		soup_message_headers_replace(headers, "Authorization", value);
	}
	if (timestamp) soup_message_headers_replace(headers, "X-Venture-Timestamp", timestamp);
	if (signature) soup_message_headers_replace(headers, "X-Venture-Signature", signature);
	if (body) soup_message_set_request_body_from_bytes(message, "application/json", body);
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL, http_done, &reply);
	while (!reply.done) g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(reply.error);
	status = soup_message_get_status(message);
	if (text) *text = g_strndup(g_bytes_get_data(reply.body, NULL), g_bytes_get_size(reply.body));
	g_bytes_unref(reply.body);
	g_test_message("HTTP %s %s => %u", method, path, status);
	return status;
}
static gchar *token_for(Fixture *f, const gchar *username, VentureUserRole role)
{
	g_autoptr(VentureEntity) user = g_object_new(VENTURE_TYPE_USER, "username", username, "active", TRUE, "role", role, NULL);
	g_autoptr(VentureApiToken) token = venture_api_token_new();
	gchar *secret;
	persist(f, user);
	g_object_set(token, "name", username, "user-id", venture_entity_get_id(user), "role", role, NULL);
	secret = venture_api_token_generate(token);
	persist(f, VENTURE_ENTITY(token));
	return secret;
}
static void test_http(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(VentureWebServer) server = NULL;
	g_autoptr(SoupSession) session = soup_session_new();
	g_autoptr(TestFakeResolver) resolver = g_object_new(TEST_TYPE_FAKE_RESOLVER, NULL);
	g_autoptr(VentureEntity) recipient = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autofree gchar *directory = g_dir_make_tmp("venture-deliverability-web-XXXXXX", NULL), *base = NULL;
	g_autofree gchar *owner = NULL, *viewer = NULL, *text = NULL, *timestamp = NULL, *signature = NULL, *message_id = NULL;
	g_autofree gchar *report_path = NULL, *page_path = NULL;
	(void)data;
	g_object_set(config, "state-dir", directory, "server-bind-address", "127.0.0.1", "server-port", (gint64)0,
		"security-require-auth", TRUE, NULL);
	context = venture_context_new(config, f->db);
	venture_context_set_mailer(context, VENTURE_MAILER(f->mailer));
	server = venture_web_server_new(context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(server, &error));
	g_assert_no_error(error);
	base = g_strdup(venture_web_server_get_base_url(server));
	fake_add(resolver, "example.com", "v=spf1 -all", NULL);
	fake_add(resolver, "_dmarc.example.com", "v=DMARC1; p=none", NULL);
	venture_deliverability_set_resolver(f->db, VENTURE_DNS_RESOLVER(resolver));
	owner = token_for(f, "deliverability-owner", VENTURE_USER_ROLE_OWNER);
	viewer = token_for(f, "deliverability-viewer", VENTURE_USER_ROLE_VIEWER);
	configure_feed(f);
	permit(f, f->contact, "http-permission");
	preview_and_approve(f);
	recipient = send_next(f);
	message_id = mail_message_id(f, recipient);

	/* The feed. */
	body = fixture(FIXTURE("generic-hard-bounce.json"), message_id, "alice@example.net");
	now = venture_time_now();
	timestamp = g_strdup_printf("%" G_GINT64_FORMAT, g_date_time_to_unix(now));
	g_test_expect_message("Venture", G_LOG_LEVEL_WARNING, "*refused*");
	g_assert_cmpuint(http(session, base, "POST", "/hooks/deliverability/1/generic", NULL, body, timestamp, "00", &text), ==, 401);
	g_test_assert_expected_messages();
	g_clear_pointer(&text, g_free);
	g_assert_cmpuint(suppressions(f), ==, 0);
	g_test_expect_message("Venture", G_LOG_LEVEL_WARNING, "*refused*");
	g_assert_cmpuint(http(session, base, "POST", "/hooks/deliverability/1/generic", NULL, body, NULL, NULL, &text), ==, 401);
	g_test_assert_expected_messages();
	g_clear_pointer(&text, g_free);
	g_assert_cmpuint(http(session, base, "POST", "/hooks/deliverability/1/no-such-provider", NULL, body, timestamp, "00", &text), ==, 404);
	g_clear_pointer(&text, g_free);
	signature = sign(SECRET, timestamp, body);
	g_assert_cmpuint(http(session, base, "POST", "/hooks/deliverability/1/generic", NULL, body, timestamp, signature, &text), ==, 200);
	g_test_message("feed answered: %s", text);
	g_assert_true(strstr(text, "\"accepted\":1") || strstr(text, "\"accepted\" : 1"));
	g_clear_pointer(&text, g_free);
	g_assert_cmpuint(suppressions(f), ==, 1);

	/* The domain check. */
	g_assert_cmpuint(http(session, base, "GET", "/marketing/deliverability", NULL, NULL, NULL, NULL, NULL), ==, 302);
	g_assert_cmpuint(http(session, base, "GET", "/api/v1/marketing/domain-health?organization_id=1&domain=example.com", NULL, NULL, NULL, NULL, NULL), ==, 401);
	g_assert_cmpuint(http(session, base, "GET", "/api/v1/marketing/domain-health?organization_id=1&domain=example.com", viewer, NULL, NULL, NULL, NULL), ==, 403);
	g_assert_cmpuint(http(session, base, "GET", "/api/v1/marketing/domain-health?organization_id=1&domain=example.com&selector=mail", owner, NULL, NULL, NULL, &text), ==, 200);
	g_assert_nonnull(strstr(text, "\"dmarc\""));
	g_assert_nonnull(strstr(text, "\"weak\""));
	g_clear_pointer(&text, g_free);
	{
		g_autoptr(GBytes) ask = g_bytes_new_static("{\"organization_id\":1}", 21);
		g_assert_cmpuint(http(session, base, "POST", "/api/v1/marketing/feedback-secret", NULL, ask, NULL, NULL, NULL), ==, 401);
	}

	/* The send report. */
	report_path = g_strdup_printf("/api/v1/marketing/sends/%" G_GINT64_FORMAT "/report", venture_entity_get_id(f->send));
	page_path = g_strdup_printf("/marketing/sends/%" G_GINT64_FORMAT "/report", venture_entity_get_id(f->send));
	g_assert_cmpuint(http(session, base, "GET", report_path, NULL, NULL, NULL, NULL, NULL), ==, 401);
	g_assert_cmpuint(http(session, base, "GET", page_path, NULL, NULL, NULL, NULL, NULL), ==, 302);
	g_assert_cmpuint(http(session, base, "GET", report_path, owner, NULL, NULL, NULL, &text), ==, 200);
	g_assert_true(strstr(text, "\"bounced\":1") || strstr(text, "\"bounced\" : 1"));
	g_clear_pointer(&text, g_free);

	venture_deliverability_set_resolver(f->db, NULL);
	venture_web_server_stop(server);
	g_clear_object(&server);
	g_clear_object(&context);
	while (g_main_context_iteration(NULL, FALSE)) { }
	venture_test_remove_tree(directory);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/deliverability/signature", test_signature);
	g_test_add("/deliverability/feed-unconfigured", Fixture, NULL, setup, test_feed_unconfigured, teardown);
	g_test_add("/deliverability/feed-replay", Fixture, NULL, setup, test_feed_replay, teardown);
	g_test_add("/deliverability/feed-fixtures", Fixture, NULL, setup, test_feed_fixtures, teardown);
	g_test_add("/deliverability/feed-mismatch", Fixture, NULL, setup, test_feed_mismatch, teardown);
	g_test_add_func("/deliverability/dsn-parse", test_dsn_parse);
	g_test_add("/deliverability/dsn-match", Fixture, NULL, setup, test_dsn_match, teardown);
	g_test_add("/deliverability/replies", Fixture, NULL, setup, test_replies, teardown);
	g_test_add_func("/deliverability/dns-pass", test_dns_pass);
	g_test_add_func("/deliverability/dns-missing", test_dns_missing);
	g_test_add_func("/deliverability/dns-weak", test_dns_weak);
	g_test_add_func("/deliverability/ramp-schedule", test_ramp_schedule);
	g_test_add("/deliverability/ramp-enforced", Fixture, NULL, setup, test_ramp_enforced, teardown);
	g_test_add("/deliverability/ramp-default", Fixture, NULL, setup, test_ramp_default, teardown);
	g_test_add_func("/deliverability/machine-reason", test_machine_reason);
	g_test_add("/deliverability/report", Fixture, NULL, setup, test_report, teardown);
	g_test_add("/deliverability/http", Fixture, NULL, setup, test_http, teardown);
	return g_test_run();
}
