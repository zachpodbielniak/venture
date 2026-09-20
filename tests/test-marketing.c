/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <string.h>
#include "venture-test-accounting.h"
#include "venture-test-util.h"
#include "db/venture-migrations.h"
#include <libsoup/soup.h>

/* Without registry ownership the audience is invisible through every generic surface. */
static void test_records(void)
{
	const gchar *names[] = { "marketing_list", "marketing_member", "marketing_consent",
		"marketing_send", "marketing_recipient", "marketing_event" };
	guint i;
	for (i = 0; i < G_N_ELEMENTS(names); i++)
		g_assert_cmpuint(venture_entity_registry_lookup(venture_entity_registry_get_default(), names[i]), !=, G_TYPE_INVALID);
}

/* Imports and generic CRUD must not manufacture permission or delivery evidence. */
static void test_evidence_guard(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDatabase) db = venture_test_accounting_database(&error);
	g_autoptr(VentureEntity) consent = g_object_new(VENTURE_TYPE_MARKETING_CONSENT,
		"organization-id", (gint64)1, "email", "reader@example.test",
		"purpose", "marketing", "source", "Forged import", "evidence-key", "forged", NULL);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	g_assert_false(venture_database_save(db, consent, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	venture_test_accounting_database_cleanup(db);
}

typedef struct {
	gchar *directory;
	gchar *uri;
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
	VentureEntity *row = g_object_new(VENTURE_TYPE_CONTACT, "organization-id", (gint64)1,
		"name", name, "email", email, NULL);
	persist(f, row); return row;
}
static void member(Fixture *f, VentureEntity *row)
{
	g_autoptr(VentureEntity) entry = g_object_new(VENTURE_TYPE_MARKETING_MEMBER,
		"organization-id", (gint64)1, "list-id", venture_entity_get_id(f->list),
		"contact-id", venture_entity_get_id(row), NULL);
	persist(f, entry);
}
static VentureMarketingConsent *permission_for(Fixture *f, VentureEntity *row, const gchar *key)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	VentureMarketingConsent *result = venture_marketing_service_consent(f->service, 1, row,
		"Signed preference form", "I request this organization's marketing email", key, now, NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(result); return result;
}
static const gchar restart_mode[] = "restart";
static void setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	f->directory = NULL; f->uri = NULL;
	if (data == restart_mode && !g_getenv("VENTURE_TEST_ACCOUNTING_POSTGRES_URI")) {
		f->directory = g_dir_make_tmp("venture-marketing-restart-XXXXXX", &error); g_assert_no_error(error);
		f->uri = g_strdup_printf("sqlite://%s/marketing.db", f->directory);
		f->db = venture_database_new(f->uri, &error);
	} else f->db = venture_test_accounting_database(&error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error)); g_assert_no_error(error);
	f->service = venture_marketing_service_get(f->db);
	f->mailer = venture_log_mailer_new(); f->outbox = venture_mail_outbox_new(f->db, VENTURE_MAILER(f->mailer));
	f->contact = contact(f, "Alice", " Alice@Example.test ");
	f->list = g_object_new(VENTURE_TYPE_MARKETING_LIST, "organization-id", (gint64)1, "name", "Readers", NULL); persist(f, f->list);
	member(f, f->contact);
	f->send = g_object_new(VENTURE_TYPE_MARKETING_SEND, "organization-id", (gint64)1,
		"name", "Autumn", "list-id", venture_entity_get_id(f->list), "subject", "Hello {name}",
		"text-body", "Read https://example.test/news", "html-body", "<p>Hello {name}</p><a href=\"https://example.test/news\">News</a>",
		"interval-seconds", (gint64)1, NULL); persist(f, f->send);
}
static void teardown(Fixture *f, gconstpointer data)
{
	g_clear_object(&f->contact); g_clear_object(&f->list); g_clear_object(&f->send);
	g_clear_object(&f->outbox); g_clear_object(&f->mailer);
	venture_test_accounting_database_cleanup(f->db); g_clear_object(&f->db);
	if (f->directory) venture_test_remove_tree(f->directory);
	g_free(f->directory); g_free(f->uri);
}
static GPtrArray *recipients(Fixture *f)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_MARKETING_RECIPIENT);
	g_autoptr(GError) error = NULL;
	GPtrArray *rows;
	venture_query_set_organization(query, 1); venture_query_set_limit(query, 0);
	venture_query_add_filter_int(query, "send-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(f->send), NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	rows = venture_database_find(f->db, query, &error); g_assert_no_error(error); return rows;
}
static void preview(Fixture *f)
{
	g_autoptr(GError) error = NULL;
	VentureMarketingSend *row = venture_marketing_service_preview(f->service, 1,
		venture_entity_get_id(f->send), "https://venture.example.test", NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(row);
	g_clear_object(&f->send); f->send = VENTURE_ENTITY(row);
}
static void approve(Fixture *f)
{
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_marketing_service_transition(f->service, 1, venture_entity_get_id(f->send), "approve", NULL, &error));
	g_assert_no_error(error);
}
static gint run(Fixture *f)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	gint result = venture_marketing_service_run(f->service, 1, venture_entity_get_id(f->send), 100, now, NULL, &error);
	g_assert_no_error(error); return result;
}
/* An approval binds both the address set and rendered personalizations. */
static void test_snapshot(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) duplicate = contact(f, "Duplicate", "alice@example.test"), outsider = contact(f, "No permission", "none@example.test");
	g_autoptr(VentureMarketingConsent) consent = permission_for(f, f->contact, "alice"), duplicate_consent = permission_for(f, duplicate, "duplicate");
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *subject_text = NULL, *email = NULL;
	gint64 eligible, excluded;
	member(f, duplicate); member(f, outsider); preview(f);
	g_object_get(f->send, "eligible-count", &eligible, "excluded-count", &excluded, NULL);
	g_assert_cmpint(eligible, ==, 1); g_assert_cmpint(excluded, ==, 2);
	g_test_message("Frozen preview: eligible=%" G_GINT64_FORMAT ", exclusions=%" G_GINT64_FORMAT, eligible, excluded);
	rows = recipients(f); g_assert_cmpuint(rows->len, ==, 3);
	g_object_get(g_ptr_array_index(rows, 0), "subject", &subject_text, "email", &email, NULL);
	g_assert_cmpstr(subject_text, ==, "Hello Alice"); g_assert_cmpstr(email, ==, "alice@example.test");
	g_object_set(f->send, "subject", "Changed after approval", NULL);
	g_assert_false(venture_database_save(f->db, f->send, NULL, &error)); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION); g_clear_error(&error);
	g_object_set(f->contact, "name", "Later name", NULL); persist(f, f->contact);
	approve(f); g_assert_cmpint(run(f), ==, 1); g_assert_cmpint(run(f), ==, 0);
	g_assert_cmpint(venture_mail_outbox_deliver_due(f->outbox, 1, 100, NULL, NULL, &error), ==, 1); g_assert_no_error(error);
	g_clear_pointer(&subject_text, g_free);
	g_object_get(g_ptr_array_index((GPtrArray *)venture_log_mailer_get_messages(f->mailer), 0), "subject", &subject_text, NULL);
	g_assert_cmpstr(subject_text, ==, "Hello Alice");
	g_test_message("Delivered frozen personalization after source rename: %s", subject_text);
}
/* Unsubscribe must survive a removed CRM subject and stop already queued mail. */
static void test_unsubscribe(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureMarketingConsent) consent = permission_for(f, f->contact, "withdraw-permission");
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureMailMessage) transactional = venture_mail_message_new(), queued = NULL;
	g_autofree gchar *url = NULL;
	const gchar *token;
	preview(f); approve(f); g_assert_cmpint(run(f), ==, 1);
	rows = recipients(f); g_object_get(g_ptr_array_index(rows, 0), "unsubscribe-url", &url, NULL);
	token = strrchr(url, '/') + 1;
	if (data) { g_assert_true(venture_database_delete(f->db, f->contact, NULL, &error)); g_assert_no_error(error); }
	g_assert_true(venture_marketing_service_unsubscribe(f->service, token, &error)); g_assert_no_error(error);
	g_assert_true(venture_marketing_service_unsubscribe(f->service, token, &error)); g_assert_no_error(error);
	g_assert_cmpint(venture_mail_outbox_deliver_due(f->outbox, 1, 100, NULL, NULL, &error), ==, 0); g_assert_no_error(error);
	g_assert_cmpuint(venture_log_mailer_get_messages(f->mailer)->len, ==, 0);
	g_object_set(transactional, "organization-id", (gint64)1, "to", "alice@example.test", "subject", "Necessary receipt",
		"text-body", "Transactional receipt", "idempotency-key", "transactional", NULL);
	queued = venture_mail_outbox_enqueue(f->outbox, transactional, NULL, &error); g_assert_no_error(error); g_assert_nonnull(queued);
	g_assert_cmpint(venture_mail_outbox_deliver_due(f->outbox, 1, 100, NULL, NULL, &error), ==, 1); g_assert_no_error(error);
}
/* A consent for the old address is never transferred by an ordinary edit. */
static void test_address_change(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureMarketingConsent) consent = permission_for(f, f->contact, "old-address");
	g_autoptr(GError) error = NULL;
	preview(f); approve(f); g_assert_cmpint(run(f), ==, 1);
	g_object_set(f->contact, "email", "new@example.test", NULL); persist(f, f->contact);
	g_assert_cmpint(venture_mail_outbox_deliver_due(f->outbox, 1, 100, NULL, NULL, &error), ==, 1); g_assert_no_error(error);
	g_assert_cmpuint(venture_log_mailer_get_messages(f->mailer)->len, ==, 0);
}

static GPtrArray *all_rows(Fixture *f, GType type)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	g_autoptr(GError) error = NULL;
	GPtrArray *rows;
	venture_query_set_organization(query, 1); venture_query_set_limit(query, 0);
	rows = venture_database_find(f->db, query, &error); g_assert_no_error(error); return rows;
}
static void enable_tracking(Fixture *f)
{
	g_autoptr(VentureEntity) org = venture_database_get(f->db, VENTURE_TYPE_ORGANIZATION, 1, NULL);
	g_object_set(org, "marketing-tracking", TRUE, NULL); persist(f, org);
	g_object_set(f->send, "tracking", TRUE, NULL); persist(f, f->send);
}
/* Two clocks representing one instant must not manufacture two daily opens. */
static void test_observations(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureMarketingConsent) consent = permission_for(f, f->contact, "tracking-permission");
	g_autoptr(GPtrArray) rows = NULL, events = NULL;
	g_autoptr(GDateTime) now = NULL, local = NULL;
	g_autoptr(GTimeZone) zone = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *html = NULL, *token = NULL, *destination = NULL;
	const gchar *pixel;
	enable_tracking(f); preview(f); approve(f); g_assert_cmpint(run(f), ==, 1);
	g_assert_cmpint(venture_mail_outbox_deliver_due(f->outbox, 1, 1, NULL, NULL, &error), ==, 1); g_assert_no_error(error);
	rows = recipients(f); g_object_get(g_ptr_array_index(rows, 0), "private-html-body", &html, NULL);
	pixel = strstr(html, "/marketing/t/o/"); g_assert_nonnull(pixel);
	token = g_strndup(pixel + strlen("/marketing/t/o/"), 64);
	now = venture_time_now(); zone = g_time_zone_new_offset(g_date_time_get_hour(now) < 12 ? -12 * 3600 : 14 * 3600);
	local = g_date_time_to_timezone(now, zone);
	g_assert_true(venture_marketing_service_record_open(f->service, token, local, &error)); g_assert_no_error(error);
	g_assert_true(venture_marketing_service_record_open(f->service, token, now, &error)); g_assert_no_error(error);
	events = all_rows(f, VENTURE_TYPE_MARKETING_EVENT); g_assert_cmpuint(events->len, ==, 1);
	g_test_message("Two open clocks representing one instant retained %u daily observation", events->len);
	destination = venture_marketing_service_record_click(f->service, token, 1, now, &error); g_assert_no_error(error);
	g_assert_cmpstr(destination, ==, "https://example.test/news");
	g_test_message("Retained click destination: %s", destination);
	g_clear_pointer(&destination, g_free);
	destination = venture_marketing_service_record_click(f->service, token, 1, now, &error); g_assert_no_error(error);
	g_clear_pointer(&events, g_ptr_array_unref); events = all_rows(f, VENTURE_TYPE_MARKETING_EVENT); g_assert_cmpuint(events->len, ==, 2);
	g_assert_false(venture_marketing_service_record_open(f->service,
		"0000000000000000000000000000000000000000000000000000000000000000", now, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
}
/* Only explicit hard evidence suppresses: retryable mail errors are not bounces. */
static void test_feedback(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureMarketingConsent) consent = permission_for(f, f->contact, "feedback-permission");
	g_autoptr(GPtrArray) rows = NULL, suppressions = NULL, events = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) now = NULL;
	gint64 recipient_id;
	preview(f); approve(f); g_assert_cmpint(run(f), ==, 1);
	g_assert_cmpint(venture_mail_outbox_deliver_due(f->outbox, 1, 1, NULL, NULL, &error), ==, 1); g_assert_no_error(error);
	rows = recipients(f); recipient_id = venture_entity_get_id(g_ptr_array_index(rows, 0)); now = venture_time_now();
	g_assert_true(venture_marketing_service_feedback(f->service, 1, recipient_id, "temporary_bounce", "Relay DSN 4.2.2 reviewed", "temp", now, NULL, &error)); g_assert_no_error(error);
	suppressions = all_rows(f, VENTURE_TYPE_SUPPRESSION); g_assert_cmpuint(suppressions->len, ==, 0);
	g_assert_true(venture_marketing_service_feedback(f->service, 1, recipient_id, "hard_bounce", "Relay DSN 5.1.1 reviewed", "hard", now, NULL, &error)); g_assert_no_error(error);
	g_assert_true(venture_marketing_service_feedback(f->service, 1, recipient_id, "hard_bounce", "Relay DSN 5.1.1 reviewed", "hard", now, NULL, &error)); g_assert_no_error(error);
	g_clear_pointer(&suppressions, g_ptr_array_unref); suppressions = all_rows(f, VENTURE_TYPE_SUPPRESSION); g_assert_cmpuint(suppressions->len, ==, 1);
	events = all_rows(f, VENTURE_TYPE_MARKETING_EVENT); g_assert_cmpuint(events->len, ==, 2);
	g_test_message("Temporary feedback retained without suppression; hard-bounce evidence retained once: events=%u, suppressions=%u", events->len, suppressions->len);
	g_assert_false(venture_marketing_service_feedback(f->service, 1, recipient_id, "complaint", "Different evidence", "hard", now, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION); g_clear_error(&error);
	g_assert_null(venture_marketing_service_consent(f->service, 1, f->contact, "Import", "New record", "new-permission", now, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}
/* Pausing keeps queued work, while cancellation cannot undo accepted SMTP. */
static void test_pause(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureMarketingConsent) consent = permission_for(f, f->contact, "pause-permission");
	g_autoptr(GError) error = NULL;
	preview(f); approve(f); g_assert_cmpint(run(f), ==, 1);
	g_assert_true(venture_marketing_service_transition(f->service, 1, venture_entity_get_id(f->send), "pause", NULL, &error)); g_assert_no_error(error);
	g_assert_cmpint(venture_mail_outbox_deliver_due(f->outbox, 1, 10, NULL, NULL, &error), ==, 0); g_assert_no_error(error);
	g_assert_cmpuint(venture_log_mailer_get_messages(f->mailer)->len, ==, 0);
	g_assert_true(venture_marketing_service_transition(f->service, 1, venture_entity_get_id(f->send), "resume", NULL, &error)); g_assert_no_error(error);
	g_assert_cmpint(venture_mail_outbox_deliver_due(f->outbox, 1, 10, NULL, NULL, &error), ==, 1); g_assert_no_error(error);
	g_assert_cmpuint(venture_log_mailer_get_messages(f->mailer)->len, ==, 1);
	g_assert_true(venture_marketing_service_transition(f->service, 1, venture_entity_get_id(f->send), "cancel", NULL, &error)); g_assert_no_error(error);
	g_assert_cmpint(run(f), ==, 0);
}

/* Bearer preference authority is independent of an unrelated logged-in user. */
static void test_capability_actor(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureMarketingConsent) consent = permission_for(f, f->contact, "capability-permission");
	g_autoptr(VentureEntity) user = g_object_new(VENTURE_TYPE_USER, "username", "unrelated-viewer", "active", TRUE, "role", VENTURE_USER_ROLE_VIEWER, NULL);
	g_autoptr(VentureAccessScope) scope = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *url = NULL;
	VentureAuthPrincipal principal;
	const VentureAuthPrincipal *active;
	preview(f); approve(f); g_assert_cmpint(run(f), ==, 1);
	rows = recipients(f); g_object_get(g_ptr_array_index(rows, 0), "unsubscribe-url", &url, NULL);
	persist(f, user);
	principal.user_id = venture_entity_get_id(user); principal.token_id = 0;
	principal.role = VENTURE_USER_ROLE_VIEWER; principal.name = (gchar *)"unrelated-viewer"; principal.authenticated = TRUE;
	scope = venture_access_policy_enter(venture_database_get_access_policy(f->db), &principal);
	active = venture_access_policy_get_actor(venture_database_get_access_policy(f->db));
	g_assert_true(venture_marketing_service_unsubscribe(f->service, strrchr(url, '/') + 1, &error));
	g_assert_no_error(error);
	g_assert_true(venture_access_policy_get_actor(venture_database_get_access_policy(f->db)) == active);
}
typedef struct { Fixture *fixture; gchar *token; gboolean invoked; } WithdrawalHook;
static void withdraw_on_bookkeeping(VentureDatabase *database, VentureEntity *row, gboolean created, gpointer data)
{
	WithdrawalHook *hook = data;
	g_autoptr(GDateTime) next = NULL;
	g_autoptr(GError) error = NULL;
	if (!VENTURE_IS_MARKETING_SEND(row) || hook->invoked) return;
	g_object_get(row, "next-delivery-at", &next, NULL);
	if (!next) return;
	hook->invoked = TRUE;
	g_assert_true(venture_marketing_service_unsubscribe(hook->fixture->service, hook->token, &error));
	g_assert_no_error(error);
}
/* Synchronous automation during bookkeeping must not outrun the final veto. */
static void test_last_recheck(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureMarketingConsent) consent = permission_for(f, f->contact, "recheck-permission");
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *url = NULL;
	WithdrawalHook hook;
	gulong handler;
	preview(f); approve(f); g_assert_cmpint(run(f), ==, 1);
	rows = recipients(f); g_object_get(g_ptr_array_index(rows, 0), "unsubscribe-url", &url, NULL);
	hook.fixture = f; hook.token = strrchr(url, '/') + 1; hook.invoked = FALSE;
	handler = g_signal_connect(f->db, "entity-saved", G_CALLBACK(withdraw_on_bookkeeping), &hook);
	g_assert_cmpint(venture_mail_outbox_deliver_due(f->outbox, 1, 1, NULL, NULL, &error), ==, 1); g_assert_no_error(error);
	g_signal_handler_disconnect(f->db, handler);
	g_assert_true(hook.invoked);
	g_assert_cmpuint(venture_log_mailer_get_messages(f->mailer)->len, ==, 0);
}

/* Segments use the existing typed query parser but never its scope controls. */
static void test_segment(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureMarketingConsent) consent = permission_for(f, f->contact, "segment-permission");
	g_autoptr(VentureEntity) other = contact(f, "Other", "other@example.test");
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	gint64 eligible, excluded;
	g_object_set(f->list, "mode", 1, "filters", "organization_id=2", NULL);
	g_assert_false(venture_database_save(f->db, f->list, NULL, &error)); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION); g_clear_error(&error);
	g_object_set(f->list, "filters", "limit=1", NULL);
	g_assert_false(venture_database_save(f->db, f->list, NULL, &error)); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION); g_clear_error(&error);
	g_object_set(f->list, "filters", "name=Alice", NULL); persist(f, f->list);
	preview(f); g_object_get(f->send, "eligible-count", &eligible, "excluded-count", &excluded, NULL);
	g_assert_cmpint(eligible, ==, 1); g_assert_cmpint(excluded, ==, 0);
	g_object_set(f->list, "filters", "name=Other", NULL); persist(f, f->list);
	approve(f); g_assert_cmpint(run(f), ==, 1);
	rows = recipients(f); g_assert_cmpuint(rows->len, ==, 1);
}
/* Every concrete reference and API organization must agree, even internally. */
static void test_scope(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) org = g_object_new(VENTURE_TYPE_ORGANIZATION, "name", "Other organization", NULL);
	g_autoptr(VentureEntity) outsider = NULL, entry = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GError) error = NULL;
	gint64 other;
	persist(f, org); other = venture_entity_get_id(org);
	outsider = g_object_new(VENTURE_TYPE_CONTACT, "organization-id", other, "name", "Outside", "email", "outside@example.test", NULL); persist(f, outsider);
	entry = g_object_new(VENTURE_TYPE_MARKETING_MEMBER, "organization-id", (gint64)1,
		"list-id", venture_entity_get_id(f->list), "contact-id", venture_entity_get_id(outsider), NULL);
	g_assert_false(venture_database_save(f->db, entry, NULL, &error)); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND); g_clear_error(&error);
	g_assert_null(venture_marketing_service_consent(f->service, 1, outsider, "Source", "Evidence", "cross-org", now, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND); g_clear_error(&error);
	g_assert_null(venture_marketing_service_preview(f->service, other, venture_entity_get_id(f->send), "https://venture.example.test", NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
}
/* The concrete company and lead mappings are their own primary addresses. */
static void test_source_kinds(Fixture *f, gconstpointer data)
{
	GType type = data ? VENTURE_TYPE_LEAD : VENTURE_TYPE_COMPANY;
	const gchar *field = data ? "lead-id" : "company-id";
	g_autoptr(VentureEntity) source = g_object_new(type, "organization-id", (gint64)1,
		"name", "Primary address", "email", "primary@example.test", NULL);
	g_autoptr(VentureEntity) entry = NULL;
	g_autoptr(VentureMarketingConsent) consent = NULL;
	gint64 eligible, excluded;
	persist(f, source); consent = permission_for(f, source, "primary-permission");
	entry = g_object_new(VENTURE_TYPE_MARKETING_MEMBER, "organization-id", (gint64)1,
		"list-id", venture_entity_get_id(f->list), field, venture_entity_get_id(source), NULL); persist(f, entry);
	preview(f); g_object_get(f->send, "eligible-count", &eligible, "excluded-count", &excluded, NULL);
	g_assert_cmpint(eligible, ==, 1); g_assert_cmpint(excluded, ==, 1);
}
/* Actor checks happen in service APIs as well as generated route handlers. */
static void test_authority(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) user = g_object_new(VENTURE_TYPE_USER, "username", "campaign-editor", "active", TRUE, "role", VENTURE_USER_ROLE_EDITOR, NULL);
	g_autoptr(VentureEntity) membership = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP, "organization-id", (gint64)1, "active", TRUE, "role", VENTURE_ORGANIZATION_ROLE_EDITOR, NULL);
	g_autoptr(VentureAccessScope) scope = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(VentureMarketingConsent) consent = NULL;
	g_autoptr(GError) error = NULL;
	VentureAuthPrincipal principal;
	persist(f, user); g_object_set(membership, "user-id", venture_entity_get_id(user), NULL); persist(f, membership);
	principal.user_id = venture_entity_get_id(user); principal.token_id = 0; principal.role = VENTURE_USER_ROLE_EDITOR;
	principal.name = (gchar *)"campaign-editor"; principal.authenticated = TRUE;
	scope = venture_access_policy_enter(venture_database_get_access_policy(f->db), &principal);
	consent = venture_marketing_service_consent(f->service, 1, f->contact, "Verified", "Permission", "authorized", now, NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(consent);
	g_clear_object(&scope); g_object_set(membership, "role", VENTURE_ORGANIZATION_ROLE_VIEWER, NULL); persist(f, membership);
	scope = venture_access_policy_enter(venture_database_get_access_policy(f->db), &principal);
	g_assert_null(venture_marketing_service_preview(f->service, 1, venture_entity_get_id(f->send), "https://venture.example.test", NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
}
/* Consent replay identities remain unique underneath application validators. */
static void test_history(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureMarketingConsent) consent = permission_for(f, f->contact, "history-permission");
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) at = NULL;
	g_autoptr(VentureMarketingConsent) replay = NULL;
	g_object_get(consent, "recorded-at", &at, NULL);
	replay = venture_marketing_service_consent(f->service, 1, f->contact, "Signed preference form",
		"I request this organization's marketing email", "history-permission", at, NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(replay);
	g_assert_cmpint(venture_entity_get_id(VENTURE_ENTITY(consent)), ==, venture_entity_get_id(VENTURE_ENTITY(replay)));
	g_assert_false(venture_database_delete(f->db, VENTURE_ENTITY(consent), NULL, &error)); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION); g_clear_error(&error);
	g_assert_false(venture_database_restore(f->db, VENTURE_ENTITY(consent), NULL, &error)); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION); g_clear_error(&error);
	g_assert_false(venture_database_purge(f->db, VENTURE_ENTITY(consent), NULL, &error)); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION); g_clear_error(&error);
	g_assert_false(venture_database_execute(f->db,
		"INSERT INTO marketing_consents (uuid, organization_id, evidence_key, email) VALUES ('raw-duplicate', 1, 'history-permission', 'reader@example.test')", NULL, &error));
	g_assert_nonnull(error); g_assert_nonnull(strstr(error->message, "evidence_key"));
}

/* A lost process cannot silently resubmit an unknown SMTP outcome. */
static void test_restart(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureMarketingConsent) consent = permission_for(f, f->contact, "restart-permission");
	g_autoptr(GPtrArray) rows = NULL, mails = NULL;
	g_autoptr(VentureMailMessage) claimed = NULL;
	g_autoptr(GDateTime) now = venture_time_now(), later = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *schema = NULL, *sql = NULL, *state = NULL;
	gint64 mail_id;
	preview(f); approve(f); g_assert_cmpint(run(f), ==, 1);
	rows = recipients(f); g_object_get(g_ptr_array_index(rows, 0), "mail-id", &mail_id, NULL);
	g_clear_pointer(&now, g_date_time_unref); now = venture_time_now();
	claimed = venture_mail_outbox_claim(f->outbox, 1, mail_id, now, &error); g_assert_no_error(error); g_assert_nonnull(claimed);
	schema = g_strdup(g_object_get_data(G_OBJECT(f->db), "accounting-test-schema"));
	g_clear_object(&f->outbox); g_clear_object(&f->db);
	f->db = venture_database_new(schema ? g_getenv("VENTURE_TEST_ACCOUNTING_POSTGRES_URI") : f->uri, &error); g_assert_no_error(error);
	if (schema) {
		sql = g_strdup_printf("SET search_path TO %s", schema);
		g_assert_true(venture_database_execute(f->db, sql, NULL, &error)); g_assert_no_error(error);
		g_object_set_data_full(G_OBJECT(f->db), "accounting-test-schema", g_strdup(schema), g_free);
	}
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error)); g_assert_no_error(error);
	f->service = venture_marketing_service_get(f->db); f->outbox = venture_mail_outbox_new(f->db, VENTURE_MAILER(f->mailer));
	later = g_date_time_add_seconds(now, VENTURE_MAIL_LEASE_SECONDS + 1);
	g_assert_cmpint(venture_mail_outbox_deliver_due(f->outbox, 1, 10, later, NULL, &error), ==, 0); g_assert_no_error(error);
	g_assert_cmpint(run(f), ==, 0); g_assert_cmpuint(venture_log_mailer_get_messages(f->mailer)->len, ==, 0);
	mails = all_rows(f, VENTURE_TYPE_MAIL_MESSAGE); g_assert_cmpuint(mails->len, ==, 1);
	g_object_get(g_ptr_array_index(mails, 0), "state", &state, NULL); g_assert_cmpstr(state, ==, "uncertain");
	g_test_message("After reconnect and expired lease: state=%s, transport submissions=0", state);
	g_assert_true(venture_mail_outbox_retry(f->outbox, 1, mail_id, NULL, &error)); g_assert_no_error(error);
	g_assert_cmpint(venture_mail_outbox_deliver_due(f->outbox, 1, 10, later, NULL, &error), ==, 1); g_assert_no_error(error);
	g_assert_cmpuint(venture_log_mailer_get_messages(f->mailer)->len, ==, 1);
	g_clear_pointer(&mails, g_ptr_array_unref); mails = all_rows(f, VENTURE_TYPE_MAIL_MESSAGE); g_assert_cmpuint(mails->len, ==, 1);
	g_assert_cmpint(venture_entity_get_id(g_ptr_array_index(mails, 0)), ==, mail_id);
	g_test_message("Explicit retry retained mail identity %" G_GINT64_FORMAT " and submitted once", mail_id);
}
/* Capabilities must never escape generic JSON even though SMTP needs them. */
static void test_sensitive(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureMarketingConsent) consent = permission_for(f, f->contact, "private-permission");
	g_autoptr(GPtrArray) rows = NULL, mails = NULL;
	g_autoptr(JsonNode) json = NULL;
	g_autofree gchar *url = NULL, *serialized = NULL;
	enable_tracking(f); preview(f); approve(f); g_assert_cmpint(run(f), ==, 1);
	rows = recipients(f); g_object_get(g_ptr_array_index(rows, 0), "unsubscribe-url", &url, NULL);
	json = venture_serializable_to_json(VENTURE_SERIALIZABLE(g_ptr_array_index(rows, 0)), FALSE);
	serialized = venture_json_to_string(json, FALSE);
	g_assert_null(strstr(serialized, strrchr(url, '/') + 1)); g_assert_null(strstr(serialized, "unsubscribe_hash"));
	g_assert_null(strstr(serialized, "private_html_body")); g_assert_null(strstr(serialized, "tracking_hash"));
	g_clear_pointer(&json, json_node_unref); g_clear_pointer(&serialized, g_free);
	mails = all_rows(f, VENTURE_TYPE_MAIL_MESSAGE);
	json = venture_serializable_to_json(VENTURE_SERIALIZABLE(g_ptr_array_index(mails, 0)), FALSE);
	serialized = venture_json_to_string(json, FALSE);
	g_assert_null(strstr(serialized, strrchr(url, '/') + 1)); g_assert_null(strstr(serialized, "private_unsubscribe_url"));
}

/* Switching the module off defers pending mail but cannot revoke unsubscribe. */
static void test_module(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureContext) context = venture_context_new(config, f->db);
	g_autoptr(VentureMarketingConsent) consent = permission_for(f, f->contact, "module-permission");
	g_autoptr(GPtrArray) rows = NULL;
	g_autofree gchar *url = NULL;
	g_autoptr(GError) error = NULL;
	preview(f); approve(f); g_assert_cmpint(run(f), ==, 1);
	rows = recipients(f); g_object_get(g_ptr_array_index(rows, 0), "unsubscribe-url", &url, NULL);
	venture_config_set_module_enabled(config, "marketing", FALSE);
	if (data) venture_config_set_module_enabled(config, data, FALSE);
	g_assert_cmpuint(venture_entity_registry_lookup(venture_entity_registry_get_default(), "marketing_send"), ==, G_TYPE_INVALID);
	if (g_strcmp0(data, "mail")) { g_assert_cmpint(venture_mail_outbox_deliver_due(f->outbox, 1, 1, NULL, NULL, &error), ==, 0); g_assert_no_error(error); }
	g_assert_true(venture_marketing_service_unsubscribe(f->service, strrchr(url, '/') + 1, &error)); g_assert_no_error(error);
	if (data) venture_config_set_module_enabled(config, data, TRUE);
	venture_config_set_module_enabled(config, "marketing", TRUE);
	g_assert_cmpint(venture_mail_outbox_deliver_due(f->outbox, 1, 1, NULL, NULL, &error), ==, 0); g_assert_no_error(error);
	g_assert_cmpuint(venture_log_mailer_get_messages(f->mailer)->len, ==, 0);
}
/* Engagement does not turn SMTP acceptance into a delivery or read guarantee. */
static void test_report(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureContext) context = venture_context_new(config, f->db);
	g_autoptr(VentureMarketingConsent) consent = permission_for(f, f->contact, "report-permission");
	g_autoptr(VentureReportResult) report = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(JsonObject) options = json_object_new();
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GDateTime) now = NULL;
	JsonArray *results;
	JsonObject *row;
	VentureReport *definition = venture_report_registry_lookup(venture_context_get_report_registry(context), "marketing_performance");
	g_assert_nonnull(definition);
	preview(f); approve(f); g_assert_cmpint(run(f), ==, 1);
	g_assert_cmpint(venture_mail_outbox_deliver_due(f->outbox, 1, 1, NULL, NULL, &error), ==, 1); g_assert_no_error(error);
	rows = recipients(f); now = venture_time_now();
	g_assert_true(venture_marketing_service_feedback(f->service, 1, venture_entity_get_id(g_ptr_array_index(rows, 0)),
		"hard_bounce", "Reviewed relay DSN", "report-bounce", now, NULL, &error)); g_assert_no_error(error);
	report = venture_report_generate(definition, context, NULL, NULL, &error); g_assert_no_error(error); g_assert_nonnull(report);
	node = venture_report_result_to_json(report); results = json_object_get_array_member(json_node_get_object(node), "rows");
	g_assert_cmpuint(json_array_get_length(results), ==, 1); row = json_array_get_object_element(results, 0);
	g_assert_cmpint(json_object_get_int_member(row, "eligible"), ==, 1);
	g_assert_cmpint(json_object_get_int_member(row, "accepted"), ==, 1);
	g_assert_cmpint(json_object_get_int_member(row, "hard_bounced"), ==, 1);
	g_assert_cmpint(json_object_get_int_member(row, "uniquely_opened"), ==, 0);
	g_assert_cmpint(json_object_get_int_member(row, "pending"), ==, 0);
	g_test_message("Report: accepted=1, hard_bounced=1, uniquely_opened=0, pending=0");
	{
		g_autoptr(VentureEntity) send = venture_database_get(f->db, VENTURE_TYPE_MARKETING_SEND, venture_entity_get_id(f->send), &error);
		g_autoptr(GDateTime) approved = NULL, before = NULL, after = NULL;
		g_autoptr(VentureDateRange) period = NULL;
		g_autoptr(VentureReportResult) cohort = NULL;
		g_autoptr(JsonNode) cohort_json = NULL;
		g_assert_no_error(error); g_object_get(send, "approved-at", &approved, NULL);
		before = g_date_time_add_seconds(approved, -1); after = g_date_time_add_seconds(approved, 1);
		period = venture_date_range_new(before, approved);
		cohort = venture_report_generate(definition, context, period, NULL, &error); g_assert_no_error(error);
		cohort_json = venture_report_result_to_json(cohort);
		g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(cohort_json), "rows")), ==, 0);
		g_clear_object(&cohort); g_clear_pointer(&cohort_json, json_node_unref); g_clear_pointer(&period, venture_date_range_free);
		period = venture_date_range_new(approved, after);
		cohort = venture_report_generate(definition, context, period, NULL, &error); g_assert_no_error(error);
		cohort_json = venture_report_result_to_json(cohort);
		g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(cohort_json), "rows")), ==, 1);
		g_test_message("Approval period: exclusive end omits send; inclusive start includes send");
	}
	g_clear_object(&report); g_clear_pointer(&node, json_node_unref);
	json_object_set_int_member(options, "organization_id", 999);
	report = venture_report_generate(definition, context, NULL, options, &error); g_assert_no_error(error); g_assert_nonnull(report);
	node = venture_report_result_to_json(report);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(node), "rows")), ==, 0);
}
typedef struct { gboolean done; GBytes *body; GError *error; } MarketingReply;
static void marketing_reply(GObject *source, GAsyncResult *result, gpointer data)
{
	MarketingReply *reply = data;
	reply->body = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &reply->error); reply->done = TRUE;
}
static guint marketing_http(SoupSession *session, const gchar *base, const gchar *method, const gchar *path, const gchar *payload, gchar **text)
{
	g_autofree gchar *url = g_strconcat(base, path, NULL);
	g_autoptr(SoupMessage) message = soup_message_new(method, url);
	MarketingReply reply = { FALSE, NULL, NULL };
	guint status;
	if (payload) {
		g_autoptr(GBytes) bytes = g_bytes_new(payload, strlen(payload));
		soup_message_set_request_body_from_bytes(message, "application/x-www-form-urlencoded", bytes);
	}
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL, marketing_reply, &reply);
	while (!reply.done) g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(reply.error); status = soup_message_get_status(message);
	*text = g_strndup(g_bytes_get_data(reply.body, NULL), g_bytes_get_size(reply.body)); g_bytes_unref(reply.body);
	g_test_message("HTTP %s %s => %u", method, g_str_has_prefix(path, "/marketing/") ? "[private marketing capability]" : path, status);
	return status;
}
typedef struct { gboolean done; gchar *out; gchar *err; GError *error; } MarketingCli;
static void marketing_cli_done(GObject *source, GAsyncResult *result, gpointer data)
{
	MarketingCli *reply = data;
	g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &reply->out, &reply->err, &reply->error); reply->done = TRUE;
}
static gboolean marketing_cli_timeout(gpointer data)
{
	g_subprocess_force_exit(G_SUBPROCESS(data)); return G_SOURCE_CONTINUE;
}
static gchar *marketing_cli(const gchar *base, const gchar *secret, const gchar *command, const gchar *type, const gchar *id, const gchar *action)
{
	g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
	g_autoptr(GSubprocess) process = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *args[] = { "build/debug/venturectl", "--server", base, "--format", "json", command, type, id, action, NULL };
	MarketingCli reply = { FALSE, NULL, NULL, NULL };
	guint timeout;
	g_subprocess_launcher_setenv(launcher, "VENTURE_TOKEN", secret, TRUE);
	process = g_subprocess_launcher_spawnv(launcher, args, &error); g_assert_no_error(error);
	timeout = g_timeout_add_seconds(30, marketing_cli_timeout, process);
	g_subprocess_communicate_utf8_async(process, NULL, NULL, marketing_cli_done, &reply);
	while (!reply.done) g_main_context_iteration(NULL, TRUE);
	g_source_remove(timeout); g_assert_no_error(reply.error);
	if (!g_subprocess_get_successful(process)) g_test_message("CLI error: %s", reply.err);
	g_assert_true(g_subprocess_get_successful(process)); g_free(reply.err);
	return reply.out;
}

/* Link scanners must not unsubscribe on GET; the one-click POST is cookie-free. */
static void test_http(Fixture *f, gconstpointer data)
{
	g_autoptr(GSocketListener) probe = g_socket_listener_new();
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(VentureWebServer) server = NULL;
	g_autoptr(SoupSession) session = soup_session_new();
	g_autoptr(VentureMarketingConsent) consent = permission_for(f, f->contact, "http-permission");
	g_autoptr(VentureEntity) user = g_object_new(VENTURE_TYPE_USER, "username", "marketing-operator", "active", TRUE, "role", VENTURE_USER_ROLE_OWNER, NULL);
	g_autoptr(VentureApiToken) token = venture_api_token_new();
	g_autofree gchar *secret = NULL;
	g_autoptr(GPtrArray) rows = NULL, suppressions = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *directory = g_dir_make_tmp("venture-marketing-web-XXXXXX", NULL), *base = NULL, *url = NULL, *text = NULL, *path = NULL;
	guint port = g_socket_listener_add_any_inet_port(probe, NULL, &error); g_assert_no_error(error); g_clear_object(&probe);
	base = g_strdup_printf("http://127.0.0.1:%u", port);
	g_object_set(config, "state-dir", directory, "server-bind-address", "127.0.0.1", "server-port", (gint64)port,
		"security-require-auth", TRUE, "server-base-url", base, NULL);
	context = venture_context_new(config, f->db); venture_context_set_mailer(context, VENTURE_MAILER(f->mailer));
	server = venture_web_server_new(context, &error); g_assert_no_error(error);
	g_assert_true(venture_web_server_start(server, &error)); g_assert_no_error(error);
	persist(f, user);
	g_object_set(token, "name", "Marketing demonstration", "user-id", venture_entity_get_id(user), "role", VENTURE_USER_ROLE_OWNER, NULL);
	secret = venture_api_token_generate(token); persist(f, VENTURE_ENTITY(token));
	{
		g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, venture_entity_get_id(f->send));
		text = marketing_cli(base, secret, "describe", "marketing_send", NULL, NULL);
		g_assert_nonnull(strstr(text, "interval_seconds")); g_clear_pointer(&text, g_free);
		text = marketing_cli(base, secret, "act", "marketing_send", id, "preview");
		g_assert_nonnull(strstr(text, "preview")); g_test_message("Built CLI preview: %s", g_strchomp(text)); g_clear_pointer(&text, g_free);
		text = marketing_cli(base, secret, "act", "marketing_send", id, "approve");
		g_assert_nonnull(strstr(text, "approved")); g_test_message("Built CLI approve: %s", g_strchomp(text)); g_clear_pointer(&text, g_free);
		text = marketing_cli(base, secret, "act", "marketing_send", id, "pause");
		g_assert_nonnull(strstr(text, "paused")); g_clear_pointer(&text, g_free);
		text = marketing_cli(base, secret, "act", "marketing_send", id, "resume");
		g_assert_nonnull(strstr(text, "approved")); g_clear_pointer(&text, g_free);
		text = marketing_cli(base, secret, "act", "marketing_send", id, "run");
		g_test_message("Built CLI bounded run: %s", g_strchomp(text)); g_clear_pointer(&text, g_free);
		g_assert_cmpuint(venture_log_mailer_get_messages(f->mailer)->len, ==, 1);
		text = marketing_cli(base, secret, "act", "marketing_send", id, "run"); g_clear_pointer(&text, g_free);
		g_assert_cmpuint(venture_log_mailer_get_messages(f->mailer)->len, ==, 1);
	}
	g_object_set(config, "security-require-auth", TRUE, NULL);
	rows = recipients(f);
	g_object_get(g_ptr_array_index(rows, 0), "unsubscribe-url", &url, NULL); path = g_strdup_printf("/marketing/u/%s", strrchr(url, '/') + 1);
	g_assert_cmpuint(marketing_http(session, base, "GET", path, NULL, &text), ==, 200);
	g_assert_nonnull(strstr(text, "Stop marketing email")); g_assert_null(strstr(text, "alice@example.test")); g_clear_pointer(&text, g_free);
	suppressions = all_rows(f, VENTURE_TYPE_SUPPRESSION); g_assert_cmpuint(suppressions->len, ==, 0); g_clear_pointer(&suppressions, g_ptr_array_unref);
	g_assert_cmpuint(marketing_http(session, base, "POST", path, "List-Unsubscribe=One-Click", &text), ==, 200); g_clear_pointer(&text, g_free);
	g_assert_cmpuint(marketing_http(session, base, "POST", path, "List-Unsubscribe=One-Click", &text), ==, 200); g_clear_pointer(&text, g_free);
	suppressions = all_rows(f, VENTURE_TYPE_SUPPRESSION); g_assert_cmpuint(suppressions->len, ==, 1);
	g_assert_cmpuint(marketing_http(session, base, "POST", "/marketing/u/0000000000000000000000000000000000000000000000000000000000000000", "List-Unsubscribe=One-Click", &text), ==, 404); g_clear_pointer(&text, g_free);
	g_assert_cmpuint(marketing_http(session, base, "GET", "/marketing/t/o/nope", NULL, &text), ==, 404); g_clear_pointer(&text, g_free);
	g_assert_cmpuint(marketing_http(session, base, "POST", "/api/v1/marketing_send/1/actions/approve", "", &text), ==, 401);
	venture_web_server_stop(server); g_clear_object(&server); g_clear_object(&context);
	while (g_main_context_iteration(NULL, FALSE)) { }
	venture_test_remove_tree(directory);
}

/* Actual submissions remain spaced even when enqueue and delivery clocks differ. */
static void test_throttle(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) second = contact(f, "Bob", "bob@example.test");
	g_autoptr(VentureMarketingConsent) alice = permission_for(f, f->contact, "throttle-alice"), bob = permission_for(f, second, "throttle-bob");
	g_autoptr(GDateTime) now = NULL, later = NULL, eligible = NULL, due = NULL;
	g_autoptr(GPtrArray) messages = NULL;
	g_autoptr(GError) error = NULL;
	gint64 send_id = venture_entity_get_id(f->send);
	member(f, second); g_object_set(f->send, "interval-seconds", (gint64)60, NULL); persist(f, f->send);
	preview(f); approve(f); now = venture_time_now(); later = g_date_time_add_seconds(now, 120);
	eligible = g_date_time_add_seconds(now, 121); due = g_date_time_add_seconds(now, 180);
	g_assert_cmpint(venture_marketing_service_run(f->service, 1, send_id, 0, now, NULL, &error), ==, -1); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION); g_clear_error(&error);
	g_assert_cmpint(venture_marketing_service_run(f->service, 1, send_id, 1, now, NULL, &error), ==, 1); g_assert_no_error(error);
	g_assert_cmpint(venture_marketing_service_run(f->service, 1, send_id, 100, later, NULL, &error), ==, 0); g_assert_no_error(error);
	g_assert_cmpint(venture_mail_outbox_deliver_due(f->outbox, 1, 100, later, NULL, &error), ==, 1); g_assert_no_error(error);
	g_assert_cmpint(venture_marketing_service_run(f->service, 1, send_id, 100, eligible, NULL, &error), ==, 1); g_assert_no_error(error);
	g_assert_cmpint(venture_mail_outbox_deliver_due(f->outbox, 1, 100, eligible, NULL, &error), ==, 0); g_assert_no_error(error);
	g_assert_cmpuint(venture_log_mailer_get_messages(f->mailer)->len, ==, 1);
	g_assert_cmpint(venture_mail_outbox_deliver_due(f->outbox, 1, 100, due, NULL, &error), ==, 1); g_assert_no_error(error);
	g_assert_cmpuint(venture_log_mailer_get_messages(f->mailer)->len, ==, 2);
	g_test_message("Delayed first submission at +120s; second deferred at +121s and accepted at +180s; transport count=2");
	messages = all_rows(f, VENTURE_TYPE_MAIL_MESSAGE); g_assert_cmpuint(messages->len, ==, 2);
}
/* Marketing withdrawal uses the same suppression decision as sequences. */
static void test_sequence_exit(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) sequence = g_object_new(VENTURE_TYPE_SEQUENCE, "organization-id", (gint64)1,
		"name", "Followup", "active", TRUE, "timezone", "UTC", "send-window-start", (gint64)0,
		"send-window-end", (gint64)24, "weekdays", "1,2,3,4,5,6,7", "exit-on-unsubscribe", TRUE, NULL);
	g_autoptr(VentureEntity) step = NULL, enrollment = NULL, stored = NULL;
	g_autoptr(VentureMarketingConsent) consent = permission_for(f, f->contact, "sequence-permission");
	g_autoptr(GPtrArray) rows = NULL;
	g_autofree gchar *url = NULL;
	g_autoptr(GError) error = NULL;
	gint status;
	persist(f, sequence);
	step = g_object_new(VENTURE_TYPE_SEQUENCE_STEP, "organization-id", (gint64)1,
		"sequence-id", venture_entity_get_id(sequence), "position", (gint64)10, "active", TRUE,
		"subject", "Follow up", "body", "Outreach", NULL); persist(f, step);
	enrollment = g_object_new(VENTURE_TYPE_SEQUENCE_ENROLLMENT, "organization-id", (gint64)1,
		"sequence-id", venture_entity_get_id(sequence), "contact-id", venture_entity_get_id(f->contact), "enrollment-reason", "Requested followup", NULL); persist(f, enrollment);
	preview(f); rows = recipients(f); g_object_get(g_ptr_array_index(rows, 0), "unsubscribe-url", &url, NULL);
	g_assert_true(venture_marketing_service_unsubscribe(f->service, strrchr(url, '/') + 1, &error)); g_assert_no_error(error);
	stored = venture_database_get(f->db, VENTURE_TYPE_SEQUENCE_ENROLLMENT, venture_entity_get_id(enrollment), &error); g_assert_no_error(error);
	g_object_get(stored, "status", &status, NULL); g_assert_cmpint(status, ==, 3);
	g_test_message("Sequence enrollment after marketing unsubscribe: status=%d (exited)", status);
}
/* Upgrade never treats a CRM address or an existing suppression as consent. */
static void test_upgrade(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureContext) context = venture_context_new(config, f->db);
	g_autoptr(VentureEntity) suppression = g_object_new(VENTURE_TYPE_SUPPRESSION,
		"organization-id", (gint64)1, "email", "prior@example.test", NULL), old = NULL;
	g_autoptr(GPtrArray) consents = NULL, retained = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *name = NULL;
	persist(f, suppression);
	venture_config_set_module_enabled(config, "marketing", FALSE);
	g_assert_true(venture_database_execute(f->db,
		"DELETE FROM schema_migrations WHERE version >= 420;"
		"DROP TABLE marketing_members; DROP TABLE marketing_consents; DROP TABLE marketing_recipients;"
		"DROP TABLE marketing_events; DROP TABLE marketing_sends; DROP TABLE marketing_lists", NULL, &error)); g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error)); g_assert_no_error(error);
	venture_config_set_module_enabled(config, "marketing", TRUE);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error)); g_assert_no_error(error);
	old = venture_database_get(f->db, VENTURE_TYPE_CONTACT, venture_entity_get_id(f->contact), &error); g_assert_no_error(error);
	g_object_get(old, "name", &name, NULL); g_assert_cmpstr(name, ==, "Alice");
	consents = all_rows(f, VENTURE_TYPE_MARKETING_CONSENT); g_assert_cmpuint(consents->len, ==, 0);
	retained = all_rows(f, VENTURE_TYPE_SUPPRESSION); g_assert_cmpuint(retained->len, ==, 1);
	g_assert_cmpint(venture_entity_get_id(g_ptr_array_index(retained, 0)), ==, venture_entity_get_id(suppression));
	g_test_message("Upgrade retained CRM source %s and %u suppression; inferred consent count=%u", name, retained->len, consents->len);
}

/* A missing identity index fails the whole migration transaction, retaining history. */
static void test_migration_guard(Fixture *f, gconstpointer data)
{
	g_autofree gchar *sql = NULL, *path = g_strdup_printf("migrations/%s/000420_marketing.sql",
		g_getenv("VENTURE_TEST_ACCOUNTING_POSTGRES_URI") ? "postgresql" : "sqlite");
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureMarketingConsent) consent = permission_for(f, f->contact, "migration-guard");
	g_autoptr(GPtrArray) retained = NULL;
	g_assert_true(g_file_get_contents(path, &sql, NULL, &error)); g_assert_no_error(error);
	g_assert_true(venture_database_begin(f->db, &error)); g_assert_no_error(error);
	g_assert_true(venture_database_execute(f->db, "DROP INDEX uq_marketing_consents_organization_evidence_key", NULL, &error)); g_assert_no_error(error);
	g_assert_false(venture_migrations_execute_sql(venture_database_get_connection(f->db), sql, &error)); g_assert_nonnull(error); g_clear_error(&error);
	venture_database_rollback(f->db);
	retained = all_rows(f, VENTURE_TYPE_MARKETING_CONSENT); g_assert_cmpuint(retained->len, ==, 1);
	g_assert_cmpint(venture_entity_get_id(g_ptr_array_index(retained, 0)), ==, venture_entity_get_id(VENTURE_ENTITY(consent)));
	g_assert_true(venture_migrations_execute_sql(venture_database_get_connection(f->db), sql, &error)); g_assert_no_error(error);
}

/* External evidence identities cannot occupy the service's withdrawal namespace. */
static void test_reserved_identity(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureMarketingConsent) original = permission_for(f, f->contact, "original-permission");
	g_autofree gchar *key = g_strconcat("withdraw:", venture_entity_get_uuid(VENTURE_ENTITY(original)), NULL);
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GError) error = NULL;
	g_assert_null(venture_marketing_service_consent(f->service, 1, f->contact, "Form", "Permission", key, now, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

/* Generic import, rewrite and removal cannot resurrect a withdrawn address. */
static void test_withdrawn_grant(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureMarketingConsent) consent = permission_for(f, f->contact, "retained-permission");
	g_autoptr(GPtrArray) rows = NULL, suppressions = NULL;
	g_autofree gchar *url = NULL;
	g_autoptr(GError) error = NULL;
	gint64 eligible, excluded;
	preview(f); rows = recipients(f); g_object_get(g_ptr_array_index(rows, 0), "unsubscribe-url", &url, NULL);
	g_assert_true(venture_marketing_service_unsubscribe(f->service, strrchr(url, '/') + 1, &error)); g_assert_no_error(error);
	suppressions = all_rows(f, VENTURE_TYPE_SUPPRESSION); g_assert_cmpuint(suppressions->len, ==, 1);
	g_assert_false(venture_database_purge(f->db, g_ptr_array_index(suppressions, 0), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION); g_clear_error(&error);
	g_object_set(g_ptr_array_index(suppressions, 0), "email", "replacement@example.test", NULL);
	g_assert_false(venture_database_save(f->db, g_ptr_array_index(suppressions, 0), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION); g_clear_error(&error);
	g_clear_object(&f->send);
	f->send = g_object_new(VENTURE_TYPE_MARKETING_SEND, "organization-id", (gint64)1,
		"list-id", venture_entity_get_id(f->list), "name", "Later campaign", "subject", "Hello", "text-body", "Still withdrawn", NULL); persist(f, f->send);
	preview(f); g_object_get(f->send, "eligible-count", &eligible, "excluded-count", &excluded, NULL);
	g_assert_cmpint(eligible, ==, 0); g_assert_cmpint(excluded, ==, 1);
}

/* Dedupe's raw reference rewrite must not transfer a grant or approved recipient. */
static void test_merge_history(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) survivor = contact(f, "Survivor", "alice@example.test"), merged = NULL, stored = NULL;
	g_autoptr(VentureEntity) interaction = g_object_new(VENTURE_TYPE_INTERACTION, "organization-id", (gint64)1,
		"contact-id", venture_entity_get_id(f->contact), "subject", "Ordinary CRM history", NULL);
	g_autoptr(VentureMarketingConsent) consent = permission_for(f, f->contact, "before-merge");
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;
	gint64 source_id, target_id, actual;
	persist(f, interaction); preview(f); approve(f); g_assert_cmpint(run(f), ==, 1);
	source_id = venture_entity_get_id(f->contact); target_id = venture_entity_get_id(survivor);
	merged = venture_dedupe_service_merge_records(venture_dedupe_service_get(f->db), "contact", target_id, source_id, NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(merged);
	stored = venture_database_get(f->db, VENTURE_TYPE_INTERACTION, venture_entity_get_id(interaction), &error); g_assert_no_error(error);
	g_object_get(stored, "contact-id", &actual, NULL); g_assert_cmpint(actual, ==, target_id); g_clear_object(&stored);
	stored = venture_database_get(f->db, VENTURE_TYPE_MARKETING_CONSENT, venture_entity_get_id(VENTURE_ENTITY(consent)), &error); g_assert_no_error(error);
	g_object_get(stored, "contact-id", &actual, NULL); g_assert_cmpint(actual, ==, source_id);
	{
		g_autoptr(JsonNode) exported = venture_serializable_to_json(VENTURE_SERIALIZABLE(stored), FALSE);
		g_assert_cmpint(json_object_get_int_member(json_node_get_object(exported), "contact_id"), ==, source_id);
	}
	rows = recipients(f); g_object_get(g_ptr_array_index(rows, 0), "contact-id", &actual, NULL); g_assert_cmpint(actual, ==, source_id);
	g_clear_pointer(&rows, g_ptr_array_unref); rows = all_rows(f, VENTURE_TYPE_MARKETING_MEMBER);
	g_object_get(g_ptr_array_index(rows, 0), "contact-id", &actual, NULL); g_assert_cmpint(actual, ==, source_id);
	g_assert_cmpint(venture_mail_outbox_deliver_due(f->outbox, 1, 10, NULL, NULL, &error), ==, 1); g_assert_no_error(error);
	g_assert_cmpuint(venture_log_mailer_get_messages(f->mailer)->len, ==, 0);
	g_test_message("Merge moved ordinary interaction to survivor; consent, membership and approved recipient kept original source; queued submission cancelled");
}

/* Plugins and native field tables must express the same retained-reference contract. */
static void test_retained_metadata(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) definition = venture_json_parse("{\"type\":\"reference\",\"references\":\"contact\",\"retain_reference\":true}", &error);
	g_autoptr(VentureFieldSpec) field = venture_field_spec_new_from_json("contact-id", definition, &error), copy = NULL;
	g_autoptr(VentureEntity) consent = g_object_new(VENTURE_TYPE_MARKETING_CONSENT, NULL);
	g_autoptr(GPtrArray) fields = venture_entity_get_field_specs(consent);
	guint i;
	gboolean found = FALSE;
	g_assert_no_error(error); g_assert_nonnull(field);
	copy = venture_field_spec_copy(field);
	g_assert_true((venture_field_spec_get_flags(copy) & VENTURE_COLUMN_FLAG_RETAIN_REFERENCE) != 0);
	/* Retaining acquisition history must not mark its subject as a host path
	 * or a private-owner declaration when feature flags are combined. */
	g_assert_cmpuint(venture_field_spec_get_flags(copy) &
		(VENTURE_COLUMN_FLAG_HOST_RESOURCE | VENTURE_COLUMN_FLAG_OPTIONAL_PERSONAL_OWNER), ==, 0);
	for (i = 0; i < fields->len; i++) {
		VentureFieldSpec *candidate = g_ptr_array_index(fields, i);
		if (!g_strcmp0(venture_field_spec_get_name(candidate), "contact-id")) {
			found = TRUE; g_assert_true((venture_field_spec_get_flags(candidate) & VENTURE_COLUMN_FLAG_RETAIN_REFERENCE) != 0);
		}
	}
	g_assert_true(found);
}

/* A later mutable veto handler cannot invalidate an earlier permission decision. */
static gboolean withdraw_after_policy(VentureMailOutbox *outbox, VentureMailMessage *message, GError **error, gpointer data)
{
	WithdrawalHook *hook = data;
	hook->invoked = TRUE;
	g_assert_true(venture_marketing_service_unsubscribe(hook->fixture->service, hook->token, error));
	g_assert_no_error(*error);
	return FALSE;
}
static void test_transport_recheck(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureMarketingConsent) consent = permission_for(f, f->contact, "transport-permission");
	g_autoptr(GPtrArray) rows = NULL, messages = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *url = NULL, *state = NULL;
	WithdrawalHook hook;
	gulong handler;
	gint64 attempts;
	preview(f); approve(f); g_assert_cmpint(run(f), ==, 1);
	rows = recipients(f); g_object_get(g_ptr_array_index(rows, 0), "unsubscribe-url", &url, NULL);
	hook.fixture = f; hook.token = strrchr(url, '/') + 1; hook.invoked = FALSE;
	handler = g_signal_connect(f->outbox, "before-send", G_CALLBACK(withdraw_after_policy), &hook);
	g_assert_cmpint(venture_mail_outbox_deliver_due(f->outbox, 1, 1, NULL, NULL, &error), ==, 1); g_assert_no_error(error);
	g_assert_true(hook.invoked); g_assert_cmpuint(venture_log_mailer_get_messages(f->mailer)->len, ==, 0);
	messages = all_rows(f, VENTURE_TYPE_MAIL_MESSAGE); g_object_get(g_ptr_array_index(messages, 0), "state", &state, "attempts", &attempts, NULL);
	g_assert_cmpstr(state, ==, "cancelled"); g_assert_cmpint(attempts, ==, 0);
	g_signal_handler_disconnect(f->outbox, handler);
	g_test_message("Later before-send withdrawal: transport submissions=0, retained message cancelled, attempts=0");
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/marketing/records", test_records);
	g_test_add_func("/marketing/evidence-guard", test_evidence_guard);
	g_test_add("/marketing/snapshot", Fixture, NULL, setup, test_snapshot, teardown);
	g_test_add("/marketing/unsubscribe", Fixture, NULL, setup, test_unsubscribe, teardown);
	g_test_add("/marketing/unsubscribe-deleted-subject", Fixture, GINT_TO_POINTER(1), setup, test_unsubscribe, teardown);
	g_test_add("/marketing/address-change", Fixture, NULL, setup, test_address_change, teardown);
	g_test_add("/marketing/observations", Fixture, NULL, setup, test_observations, teardown);
	g_test_add("/marketing/feedback", Fixture, NULL, setup, test_feedback, teardown);
	g_test_add("/marketing/pause", Fixture, NULL, setup, test_pause, teardown);
	g_test_add("/marketing/capability-actor", Fixture, NULL, setup, test_capability_actor, teardown);
	g_test_add("/marketing/last-recheck", Fixture, NULL, setup, test_last_recheck, teardown);
	g_test_add("/marketing/segment", Fixture, NULL, setup, test_segment, teardown);
	g_test_add("/marketing/scope", Fixture, NULL, setup, test_scope, teardown);
	g_test_add("/marketing/company-address", Fixture, NULL, setup, test_source_kinds, teardown);
	g_test_add("/marketing/lead-address", Fixture, GINT_TO_POINTER(1), setup, test_source_kinds, teardown);
	g_test_add("/marketing/authority", Fixture, NULL, setup, test_authority, teardown);
	g_test_add("/marketing/history", Fixture, NULL, setup, test_history, teardown);
	g_test_add("/marketing/restart", Fixture, restart_mode, setup, test_restart, teardown);
	g_test_add("/marketing/sensitive", Fixture, NULL, setup, test_sensitive, teardown);
	g_test_add("/marketing/module", Fixture, NULL, setup, test_module, teardown);
	g_test_add("/marketing/report", Fixture, NULL, setup, test_report, teardown);
	g_test_add("/marketing/http", Fixture, NULL, setup, test_http, teardown);
	g_test_add("/marketing/throttle", Fixture, NULL, setup, test_throttle, teardown);
	g_test_add("/marketing/sequence-exit", Fixture, NULL, setup, test_sequence_exit, teardown);
	g_test_add("/marketing/upgrade", Fixture, NULL, setup, test_upgrade, teardown);
	g_test_add("/marketing/migration-guard", Fixture, NULL, setup, test_migration_guard, teardown);
	g_test_add("/marketing/reserved-identity", Fixture, NULL, setup, test_reserved_identity, teardown);
	g_test_add("/marketing/mail-disabled", Fixture, "mail", setup, test_module, teardown);
	g_test_add("/marketing/sequences-disabled", Fixture, "sequences", setup, test_module, teardown);
	g_test_add("/marketing/withdrawn-grant", Fixture, NULL, setup, test_withdrawn_grant, teardown);
	g_test_add("/marketing/merge-history", Fixture, NULL, setup, test_merge_history, teardown);
	g_test_add_func("/marketing/retained-metadata", test_retained_metadata);
	g_test_add("/marketing/transport-recheck", Fixture, NULL, setup, test_transport_recheck, teardown);
	return g_test_run();
}
