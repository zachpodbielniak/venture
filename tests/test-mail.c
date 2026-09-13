/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>

/* A mail record must participate in every generated surface. */
static void
test_mail_registered(void)
{
	VentureEntityRegistry *registry;
	registry = venture_entity_registry_get_default();
	g_assert_cmpuint(venture_entity_registry_lookup(registry, "mail_message"), !=, G_TYPE_INVALID);
	g_assert_cmpuint(venture_entity_registry_lookup(registry, "mail_template"), !=, G_TYPE_INVALID);
}

typedef struct {
	VentureDatabase *db;
	VentureLogMailer *mailer;
	VentureMailOutbox *outbox;
	gint64 org;
} Fixture;
static void setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ORGANIZATION);
		g_autoptr(VentureEntity) org = venture_database_find_one(f->db, query, &error);
		g_assert_no_error(error);
		f->org = venture_entity_get_id(org);
	}
	f->mailer = venture_log_mailer_new();
	f->outbox = venture_mail_outbox_new(f->db, VENTURE_MAILER(f->mailer));
}
static void teardown(Fixture *f, gconstpointer data)
{
	g_clear_object(&f->outbox);
	g_clear_object(&f->mailer);
	g_clear_object(&f->db);
}
static VentureMailMessage *enqueue(Fixture *f, const gchar *key)
{
	g_autoptr(VentureMailMessage) message = venture_mail_message_new();
	g_autoptr(GError) error = NULL;
	VentureMailMessage *row;
	g_object_set(message, "organization-id", f->org, "to", "reader@example.test",
		"subject", "Receipt", "text-body", "Hello", "idempotency-key", key, NULL);
	row = venture_mail_outbox_enqueue(f->outbox, message, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(row);
	return row;
}
static void assert_state(Fixture *f, gint64 id, const gchar *expected)
{
	g_autoptr(VentureEntity) row = venture_database_get(f->db, VENTURE_TYPE_MAIL_MESSAGE, id, NULL);
	g_autofree gchar *state = NULL;
	g_object_get(row, "state", &state, NULL);
	g_assert_cmpstr(state, ==, expected);
}
static void test_uncertain(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureMailMessage) row = enqueue(f, "receipt-1");
	g_autoptr(VentureMailMessage) duplicate = enqueue(f, "receipt-1");
	g_autoptr(GError) error = g_error_new_literal(VENTURE_ERROR, VENTURE_ERROR_MAIL_UNCERTAIN, "Reply lost");
	gint64 id = venture_entity_get_id(VENTURE_ENTITY(row));
	g_assert_cmpint(id, ==, venture_entity_get_id(VENTURE_ENTITY(duplicate)));
	venture_log_mailer_set_error(f->mailer, error);
	g_assert_cmpint(venture_mail_outbox_deliver_due(f->outbox, f->org, 10, NULL, NULL, NULL), ==, 1);
	assert_state(f, id, "uncertain");
	g_assert_cmpint(venture_mail_outbox_deliver_due(f->outbox, f->org, 10, NULL, NULL, NULL), ==, 0);
	g_assert_cmpuint(venture_log_mailer_get_messages(f->mailer)->len, ==, 1);
	venture_log_mailer_set_error(f->mailer, NULL);
	g_assert_true(venture_mail_outbox_retry(f->outbox, f->org, id, NULL, NULL));
	g_assert_cmpint(venture_mail_outbox_deliver_due(f->outbox, f->org, 10, NULL, NULL, NULL), ==, 1);
	assert_state(f, id, "sent");
	{
		const GPtrArray *messages = venture_log_mailer_get_messages(f->mailer);
		g_autofree gchar *first = NULL;
		g_autofree gchar *second = NULL;
		g_object_get(g_ptr_array_index((GPtrArray *)messages, 0), "message-id", &first, NULL);
		g_object_get(g_ptr_array_index((GPtrArray *)messages, 1), "message-id", &second, NULL);
		g_assert_nonnull(first);
		g_assert_cmpstr(first, ==, second);
	}
}
static void test_backoff(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureMailMessage) row = enqueue(f, "receipt-2");
	g_autoptr(GError) error = g_error_new_literal(VENTURE_ERROR, VENTURE_ERROR_MAIL_TRANSIENT, "Connection refused");
	g_autoptr(GDateTime) now = g_date_time_new_now_utc();
	g_autoptr(GDateTime) later = g_date_time_add_seconds(now, 120);
	gint64 id = venture_entity_get_id(VENTURE_ENTITY(row));
	g_object_set(f->outbox, "max-attempts", 2u, NULL);
	venture_log_mailer_set_error(f->mailer, error);
	g_assert_cmpint(venture_mail_outbox_deliver_due(f->outbox, f->org, 10, now, NULL, NULL), ==, 1);
	assert_state(f, id, "failed");
	g_assert_cmpint(venture_mail_outbox_deliver_due(f->outbox, f->org, 10, now, NULL, NULL), ==, 0);
	g_assert_cmpint(venture_mail_outbox_deliver_due(f->outbox, f->org, 10, later, NULL, NULL), ==, 1);
	assert_state(f, id, "dead");
	g_assert_cmpint(venture_mail_outbox_deliver_due(f->outbox, f->org, 10, later, NULL, NULL), ==, 0);
}
static void test_lease(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureMailMessage) row = enqueue(f, "receipt-3");
	g_autoptr(GDateTime) now = g_date_time_new_now_utc();
	g_autoptr(GDateTime) later = g_date_time_add_seconds(now, 601);
	g_autoptr(VentureMailMessage) claimed = venture_mail_outbox_claim(f->outbox, f->org, venture_entity_get_id(VENTURE_ENTITY(row)), now, NULL);
	g_assert_nonnull(claimed);
	g_assert_cmpint(venture_mail_outbox_deliver_due(f->outbox, f->org, 10, later, NULL, NULL), ==, 0);
	assert_state(f, venture_entity_get_id(VENTURE_ENTITY(row)), "uncertain");
	g_assert_cmpuint(venture_log_mailer_get_messages(f->mailer)->len, ==, 0);
}
static void test_refuse_generic(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureMailMessage) row = enqueue(f, "receipt-4");
	g_autoptr(GError) error = NULL;
	g_object_set(row, "state", "sent", NULL);
	g_assert_false(venture_database_save(f->db, VENTURE_ENTITY(row), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	assert_state(f, venture_entity_get_id(VENTURE_ENTITY(row)), "queued");
}
static void test_template(void)
{
	g_autoptr(VentureMailTemplate) t = venture_mail_template_new();
	g_autoptr(VentureCompany) company = venture_company_new();
	g_autoptr(VentureMailMessage) message = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *html = NULL;
	g_object_set(company, "name", "A & B", NULL);
	g_object_set(t, "subject", "Hello {name}", "text-body", "Dear {name}", "html-body", "<p>{name}</p>", NULL);
	message = venture_mail_template_render(t, VENTURE_ENTITY(company), &error);
	g_assert_no_error(error);
	g_assert_nonnull(message);
	g_object_get(message, "html-body", &html, NULL);
	g_assert_cmpstr(html, ==, "<p>A &amp; B</p>");
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/mail/registered", test_mail_registered);
	g_test_add("/mail/uncertain-idempotency-retry", Fixture, NULL, setup, test_uncertain, teardown);
	g_test_add("/mail/backoff-dead", Fixture, NULL, setup, test_backoff, teardown);
	g_test_add("/mail/expired-lease", Fixture, NULL, setup, test_lease, teardown);
	g_test_add("/mail/refuse-generic", Fixture, NULL, setup, test_refuse_generic, teardown);
	g_test_add_func("/mail/template", test_template);
	return g_test_run();
}
