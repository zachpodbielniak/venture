/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* A hosted workspace has no sequence worker. The same sweep that sends the
 * outbox must run due steps, and a reply must keep the next step from going. */
#include <venture.h>
#include <string.h>
#include "venture-test-util.h"

typedef struct {
	VentureConfig *config;
	VentureDatabase *db;
	VentureContext *context;
	VentureLogMailer *mailer;
	VentureEntity *organization;
	GDateTime *now;
} Fixture;

static void
save(VentureDatabase *db, VentureEntity *row)
{
	g_autoptr(GError) error = NULL;
	gboolean saved = venture_database_save(db, row, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(saved);
}

static void
setup(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	(void)unused;
	f->config = venture_config_new();
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	/* Sequence mail carries an unsubscribe link, so it needs a public address. */
	g_object_set(venture_sequence_service_get(f->db), "base-url", "https://example.test", NULL);
	f->mailer = venture_log_mailer_new();
	venture_context_set_mailer(f->context, VENTURE_MAILER(f->mailer));
	f->now = g_date_time_new_now_utc();
	f->organization = g_object_new(VENTURE_TYPE_ORGANIZATION, "name", "Example Plumbing", "active", TRUE, NULL);
	save(f->db, f->organization);
}

static void
teardown(Fixture *f, gconstpointer unused)
{
	(void)unused;
	g_clear_object(&f->organization);
	g_clear_pointer(&f->now, g_date_time_unref);
	g_clear_object(&f->mailer);
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
}

static void
enroll(Fixture *f, guint steps)
{
	gint64 org = venture_entity_get_id(f->organization);
	g_autoptr(VentureEntity) contact = g_object_new(VENTURE_TYPE_CONTACT, "organization-id", org,
		"name", "Example Reader", "email", "reader@example.test", NULL);
	g_autoptr(VentureEntity) sequence = g_object_new(VENTURE_TYPE_SEQUENCE, "organization-id", org,
		"name", "Follow up until they reply", "active", TRUE, "timezone", "UTC",
		"send-window-start", (gint64)0, "send-window-end", (gint64)24,
		"weekdays", "1,2,3,4,5,6,7", "exit-on-reply", TRUE, NULL);
	g_autoptr(VentureEntity) enrollment = NULL;
	guint i;
	save(f->db, contact);
	save(f->db, sequence);
	for (i = 1; i <= steps; i++) {
		g_autoptr(VentureEntity) step = g_object_new(VENTURE_TYPE_SEQUENCE_STEP,
			"organization-id", org, "sequence-id", venture_entity_get_id(sequence),
			"position", (gint64)i, "active", TRUE, "delay-days", (gint64)0, "delay-hours", (gint64)0,
			"subject", i == 1 ? "Checking in" : "One more note",
			"body", "Reply to this message and these follow-ups stop.", NULL);
		save(f->db, step);
	}
	enrollment = g_object_new(VENTURE_TYPE_SEQUENCE_ENROLLMENT, "organization-id", org,
		"sequence-id", venture_entity_get_id(sequence), "contact-id", venture_entity_get_id(contact),
		"enrolled-at", f->now, "enrollment-reason", "Follow up until they reply", NULL);
	save(f->db, enrollment);
}

static void
sweep(Fixture *f)
{
	g_autoptr(GError) error = NULL;
	g_assert_cmpint(venture_mail_delivery_sweep(f->context, &error), >=, 0);
	g_assert_no_error(error);
}

/* The first message goes out because the sweep ran the sequence. Nobody
 * called sequences/run. */
static void
test_sends_due_step(Fixture *f, gconstpointer unused)
{
	(void)unused;
	enroll(f, 1);
	sweep(f);
	g_assert_cmpuint(venture_log_mailer_get_messages(f->mailer)->len, ==, 1);
}

/* A reply exits the enrollment, so the next sweep does not send step two. */
static void
test_reply_stops_the_next_step(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) interaction = NULL;
	gint64 org = venture_entity_get_id(f->organization);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_CONTACT);
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) contact = NULL;
	(void)unused;
	enroll(f, 2);
	sweep(f);
	g_assert_cmpuint(venture_log_mailer_get_messages(f->mailer)->len, ==, 1);
	venture_query_set_organization(query, org);
	contact = venture_database_find_one(f->db, query, &error);
	g_assert_no_error(error);
	g_assert_nonnull(contact);
	interaction = g_object_new(VENTURE_TYPE_INTERACTION, "organization-id", org,
		"contact-id", venture_entity_get_id(contact), "subject", "Please stop", "outbound", FALSE, NULL);
	save(f->db, interaction);
	sweep(f);
	g_assert_cmpuint(venture_log_mailer_get_messages(f->mailer)->len, ==, 1);
}

/* The sweep's work list. A business with nothing due is not on it, so an
 * idle business costs the timer nothing. */
static gboolean
listed(Fixture *f, gint64 organization)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GArray) due = venture_mail_delivery_due_organizations(f->context, f->now, &error);
	guint i;
	g_assert_no_error(error);
	g_assert_nonnull(due);
	for (i = 0; i < due->len; i++)
		if (g_array_index(due, gint64, i) == organization) return TRUE;
	return FALSE;
}

static void
queue_mail(Fixture *f, VentureEntity *organization, const gchar *key)
{
	g_autoptr(VentureMailMessage) message = venture_mail_message_new(), queued = NULL;
	g_autoptr(GError) error = NULL;
	venture_entity_set_organization_id(VENTURE_ENTITY(message), venture_entity_get_id(organization));
	g_object_set(message, "to", "customer@example.test", "subject", "We got your request",
		"text-body", "Thanks, we will call you back today.", "idempotency-key", key, NULL);
	queued = venture_mail_outbox_enqueue(venture_database_get_mail_outbox(f->db), message, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(queued);
}

static void
test_idle_business_not_listed(Fixture *f, gconstpointer unused)
{
	(void)unused;
	g_assert_false(listed(f, venture_entity_get_id(f->organization)));
}

static void
test_due_step_listed(Fixture *f, gconstpointer unused)
{
	(void)unused;
	enroll(f, 1);
	g_assert_true(listed(f, venture_entity_get_id(f->organization)));
}

static void
test_queued_mail_listed(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) other = g_object_new(VENTURE_TYPE_ORGANIZATION, "name", "Quiet Bakery", "active", TRUE, NULL);
	(void)unused;
	save(f->db, other);
	queue_mail(f, f->organization, "mail-delivery:listed");
	g_assert_true(listed(f, venture_entity_get_id(f->organization)));
	g_assert_false(listed(f, venture_entity_get_id(other)));
}

/* The sweep visits active businesses only, as it did before the list. */
static void
test_inactive_business_not_listed(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) closed = g_object_new(VENTURE_TYPE_ORGANIZATION, "name", "Closed Bakery", "active", FALSE, NULL);
	(void)unused;
	save(f->db, closed);
	queue_mail(f, closed, "mail-delivery:closed");
	g_assert_false(listed(f, venture_entity_get_id(closed)));
}

/* A sent message leaves the list: the next sweep skips the business. */
static void
test_sent_mail_leaves_list(Fixture *f, gconstpointer unused)
{
	(void)unused;
	enroll(f, 1);
	sweep(f);
	g_assert_cmpuint(venture_log_mailer_get_messages(f->mailer)->len, ==, 1);
	g_assert_false(listed(f, venture_entity_get_id(f->organization)));
}

/* With per-business sending, mail from a business that has not set it up
 * stays queued. It is not work the sweep can do, so it is not listed. */
static void
test_unsendable_mail_not_listed(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureOrganizationMailer) mailer = venture_organization_mailer_new(f->db, f->config);
	g_autoptr(GBytes) key = g_bytes_new_static("01234567890123456789012345678901", 32);
	g_autoptr(JsonObject) values = json_object_new();
	g_autoptr(VentureIntegrationConnection) connection = NULL;
	g_autoptr(GError) error = NULL;
	gint64 org = venture_entity_get_id(f->organization);
	(void)unused;
	venture_context_set_mailer(f->context, VENTURE_MAILER(mailer));
	queue_mail(f, f->organization, "mail-delivery:unsendable");
	g_assert_false(listed(f, org));
	/* Once the business sets up sending, its queued mail is work again. */
	g_assert_true(venture_integration_service_set_key(venture_integration_service_get(f->db), key, &error));
	g_assert_no_error(error);
	g_object_set(f->config, "mail-allowed-endpoints", "smtp.example.invalid:587", NULL);
	json_object_set_string_member(values, "host", "smtp.example.invalid");
	json_object_set_string_member(values, "from", "office@example.invalid");
	json_object_set_string_member(values, "username", "organization");
	json_object_set_string_member(values, "password", "secret-A");
	connection = venture_organization_mailer_configure(mailer, org, values, 0, 0, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(connection);
	g_assert_true(listed(f, org));
}

/* The work list as a sweep at @when would see it. */
static gboolean
listed_at(Fixture *f, GDateTime *when, gint64 organization)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GArray) due = venture_mail_delivery_due_organizations(f->context, when, &error);
	guint i;
	g_assert_no_error(error);
	g_assert_nonnull(due);
	for (i = 0; i < due->len; i++)
		if (g_array_index(due, gint64, i) == organization) return TRUE;
	return FALSE;
}

/* A feed trigger is work only when the feed has an event of its kind past
 * its position. An active trigger alone is not: most businesses keep one.
 * A sent quote is read once it is old enough, so it is not work before. */
static void
test_trigger_listed_on_new_event(Fixture *f, gconstpointer unused)
{
	gint64 org = venture_entity_get_id(f->organization);
	g_autoptr(VentureEntity) sequence = g_object_new(VENTURE_TYPE_SEQUENCE, "organization-id", org,
		"name", "Any questions about the quote?", "active", TRUE, "timezone", "UTC",
		"send-window-start", (gint64)0, "send-window-end", (gint64)24, "weekdays", "1,2,3,4,5,6,7", NULL);
	g_autoptr(VentureEntity) trigger = NULL, lead = NULL, company = NULL, person = NULL, quote = NULL, line = NULL, send = NULL;
	g_autoptr(GDateTime) later = g_date_time_add_days(f->now, 8);
	g_autoptr(GError) error = NULL;
	gint64 id;
	(void)unused;
	save(f->db, sequence);
	trigger = g_object_new(VENTURE_TYPE_SEQUENCE_TRIGGER, "organization-id", org, "name", "Quote waiting",
		"sequence-id", venture_entity_get_id(sequence), "kind", 3, "days", (gint64)7, "active", TRUE, NULL);
	save(f->db, trigger);
	g_assert_false(listed_at(f, later, org));
	/* An event of another kind does not wake a quote trigger. */
	lead = g_object_new(VENTURE_TYPE_LEAD, "organization-id", org, "name", "Example inquiry", NULL);
	save(f->db, lead);
	g_assert_false(listed_at(f, later, org));
	company = g_object_new(VENTURE_TYPE_COMPANY, "organization-id", org, "name", "Example Customer", NULL);
	save(f->db, company);
	person = g_object_new(VENTURE_TYPE_CONTACT, "organization-id", org, "name", "Example Buyer",
		"email", "buyer@example.test", "company-id", venture_entity_get_id(company), NULL);
	save(f->db, person);
	quote = g_object_new(VENTURE_TYPE_QUOTE, "organization-id", org, "number", "Q-1", "currency", "USD",
		"company-id", venture_entity_get_id(company), "contact-id", venture_entity_get_id(person), NULL);
	save(f->db, quote);
	line = g_object_new(VENTURE_TYPE_QUOTE_LINE, "organization-id", org, "quote-id", venture_entity_get_id(quote),
		"description", "Example service", "quantity", (gint64)1, NULL);
	g_assert_true(venture_entity_set_field_from_string(line, "unit-price", "100.00 USD", NULL));
	save(f->db, line);
	/* The line moved the quote's version on: send the stored one. */
	id = venture_entity_get_id(quote);
	g_clear_object(&quote);
	quote = venture_database_get(f->db, VENTURE_TYPE_QUOTE, id, &error);
	g_assert_no_error(error);
	g_assert_nonnull(quote);
	send = g_object_new(VENTURE_TYPE_QUOTE_ACTION, "organization-id", org, "quote-id", venture_entity_get_id(quote),
		"expected-version", venture_entity_get_version(quote), "action", "send", "accepted-by", "Example Buyer", NULL);
	save(f->db, send);
	/* Sent today: not old enough to read, so not work yet. */
	g_assert_false(listed(f, org));
	g_assert_true(listed_at(f, later, org));
	/* Once the trigger has read the event, the business is idle again. */
	g_assert_cmpint(venture_sequence_service_run_triggers(venture_sequence_service_get(f->db), org, later, NULL, &error), >=, 0);
	g_assert_no_error(error);
	g_assert_false(listed_at(f, later, org));
}

/* The timer runs on the main loop. With nothing due, a tick is one
 * statement however many businesses there are; before, it was about ten
 * per active business, and 429 businesses held the loop for 2.4 s. */
static void
test_idle_tick_is_one_statement(Fixture *f, gconstpointer unused)
{
	guint64 before;
	guint i;
	(void)unused;
	for (i = 0; i < 40; i++) {
		g_autofree gchar *name = g_strdup_printf("Idle business %u", i);
		g_autoptr(VentureEntity) business = g_object_new(VENTURE_TYPE_ORGANIZATION, "name", name, "active", TRUE, NULL);
		save(f->db, business);
	}
	before = venture_database_get_statement_count(f->db);
	sweep(f);
	g_assert_cmpuint(venture_database_get_statement_count(f->db) - before, ==, 1);
}

/* A transient failure waits out its backoff. Listing the business on every
 * tick would visit it for nothing, which is the stall this list avoids. */
static void
test_retry_waits_until_due(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) send_error = g_error_new_literal(VENTURE_ERROR, VENTURE_ERROR_MAIL_TRANSIENT, "Connection refused");
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) later = g_date_time_add_hours(f->now, 2);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_MAIL_MESSAGE);
	g_autoptr(VentureEntity) row = NULL;
	g_autofree gchar *state = NULL;
	gint64 org = venture_entity_get_id(f->organization);
	(void)unused;
	queue_mail(f, f->organization, "mail-delivery:retry");
	venture_log_mailer_set_error(f->mailer, send_error);
	sweep(f);
	/* The log mailer records the attempt even when it refuses it. */
	venture_query_set_organization(query, org);
	row = venture_database_find_one(f->db, query, &error);
	g_assert_no_error(error);
	g_assert_nonnull(row);
	g_object_get(row, "state", &state, NULL);
	g_assert_cmpstr(state, ==, "failed");
	g_assert_false(listed(f, org));
	g_assert_true(listed_at(f, later, org));
	venture_log_mailer_set_error(f->mailer, NULL);
}

/* A stored whole second sorts after a fraction of that second unless the
 * comparison writes the fraction back. The enrollment and quote comparisons
 * use the same padding. */
static void
test_whole_second_is_due(Fixture *f, gconstpointer unused)
{
	g_autoptr(GDateTime) second = g_date_time_new_utc(2026, 1, 1, 0, 0, 0);
	g_autoptr(GDateTime) inside = g_date_time_add(second, 500000);
	g_autoptr(GDateTime) before = g_date_time_add_seconds(second, -1);
	g_autoptr(GError) error = NULL;
	gint64 org = venture_entity_get_id(f->organization);
	(void)unused;
	queue_mail(f, f->organization, "mail-delivery:whole-second");
	g_assert_true(venture_database_execute(f->db,
		"UPDATE \"mail_messages\" SET \"next_attempt_at\" = '2026-01-01T00:00:00Z'", NULL, &error));
	g_assert_no_error(error);
	g_assert_false(listed_at(f, before, org));
	g_assert_true(listed_at(f, inside, org));
	g_assert_true(listed_at(f, second, org));
}

/* The work list finds mail to send through an index on its state, not by
 * reading every message the business ever sent. */
static void
test_mail_state_indexed(Fixture *f, gconstpointer unused)
{
	g_auto(GStrv) statements = venture_schema_get_create_index_sql(VENTURE_TYPE_MAIL_MESSAGE, ORM_DIALECT_SQLITE);
	gboolean found = FALSE;
	guint i;
	(void)f; (void)unused;
	for (i = 0; statements[i]; i++)
		if (strstr(statements[i], "idx_mail_messages_state ")) found = TRUE;
	g_assert_true(found);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/mail-delivery/sends-due-step", Fixture, NULL, setup, test_sends_due_step, teardown);
	g_test_add("/mail-delivery/reply-stops-the-next-step", Fixture, NULL, setup, test_reply_stops_the_next_step, teardown);
	g_test_add("/mail-delivery/idle-business-not-listed", Fixture, NULL, setup, test_idle_business_not_listed, teardown);
	g_test_add("/mail-delivery/due-step-listed", Fixture, NULL, setup, test_due_step_listed, teardown);
	g_test_add("/mail-delivery/queued-mail-listed", Fixture, NULL, setup, test_queued_mail_listed, teardown);
	g_test_add("/mail-delivery/inactive-business-not-listed", Fixture, NULL, setup, test_inactive_business_not_listed, teardown);
	g_test_add("/mail-delivery/sent-mail-leaves-list", Fixture, NULL, setup, test_sent_mail_leaves_list, teardown);
	g_test_add("/mail-delivery/unsendable-mail-not-listed", Fixture, NULL, setup, test_unsendable_mail_not_listed, teardown);
	g_test_add("/mail-delivery/trigger-listed-on-new-event", Fixture, NULL, setup, test_trigger_listed_on_new_event, teardown);
	g_test_add("/mail-delivery/idle-tick-is-one-statement", Fixture, NULL, setup, test_idle_tick_is_one_statement, teardown);
	g_test_add("/mail-delivery/retry-waits-until-due", Fixture, NULL, setup, test_retry_waits_until_due, teardown);
	g_test_add("/mail-delivery/whole-second-is-due", Fixture, NULL, setup, test_whole_second_is_due, teardown);
	g_test_add("/mail-delivery/mail-state-indexed", Fixture, NULL, setup, test_mail_state_indexed, teardown);
	return g_test_run();
}
