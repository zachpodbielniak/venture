/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* A hosted workspace has no sequence worker. The same sweep that sends the
 * outbox must run due steps, and a reply must keep the next step from going. */
#include <venture.h>
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
	g_assert_true(venture_database_save(db, row, NULL, &error));
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

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/mail-delivery/sends-due-step", Fixture, NULL, setup, test_sends_due_step, teardown);
	g_test_add("/mail-delivery/reply-stops-the-next-step", Fixture, NULL, setup, test_reply_stops_the_next_step, teardown);
	return g_test_run();
}
