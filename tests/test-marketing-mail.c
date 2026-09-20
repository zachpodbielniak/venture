/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include "venture-test-accounting.h"

static gboolean paused(VentureMailOutbox *outbox, VentureMailMessage *message,
	GDateTime *now, gpointer data)
{
	return *(gboolean *)data;
}

/* A pause must preserve the queued identity and retry budget until resumed. */
static void test_defer(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDatabase) db = venture_test_accounting_database(&error);
	g_autoptr(VentureLogMailer) mailer = venture_log_mailer_new();
	g_autoptr(VentureMailOutbox) outbox = NULL;
	g_autoptr(VentureMailMessage) input = venture_mail_message_new(), row = NULL;
	g_autoptr(VentureEntity) stored = NULL;
	g_autofree gchar *state = NULL;
	gint64 attempts;
	gboolean hold = TRUE;
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	outbox = venture_mail_outbox_new(db, VENTURE_MAILER(mailer));
	g_assert_cmpuint(g_signal_lookup("before-claim", VENTURE_TYPE_MAIL_OUTBOX), !=, 0);
	g_signal_connect(outbox, "before-claim", G_CALLBACK(paused), &hold);
	g_object_set(input, "organization-id", (gint64)1, "to", "reader@example.test",
		"subject", "Paused campaign", "text-body", "Snapshot", "idempotency-key", "pause-test", NULL);
	row = venture_mail_outbox_enqueue(outbox, input, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_mail_outbox_deliver_due(outbox, 1, 10, NULL, NULL, &error), ==, 0);
	g_assert_no_error(error);
	stored = venture_database_get(db, VENTURE_TYPE_MAIL_MESSAGE, venture_entity_get_id(VENTURE_ENTITY(row)), &error);
	g_assert_no_error(error);
	g_object_get(stored, "state", &state, "attempts", &attempts, NULL);
	g_assert_cmpstr(state, ==, "queued");
	g_assert_cmpint(attempts, ==, 0);
	g_assert_cmpuint(venture_log_mailer_get_messages(mailer)->len, ==, 0);
	hold = FALSE;
	g_assert_cmpint(venture_mail_outbox_deliver_due(outbox, 1, 10, NULL, NULL, &error), ==, 1);
	g_assert_no_error(error);
	g_assert_cmpuint(venture_log_mailer_get_messages(mailer)->len, ==, 1);
	venture_test_accounting_database_cleanup(db);
}

/* Withdrawal must remove queued work without erasing ambiguous SMTP evidence. */
static void test_cancel(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDatabase) db = venture_test_accounting_database(&error);
	g_autoptr(VentureLogMailer) mailer = venture_log_mailer_new();
	g_autoptr(VentureMailOutbox) outbox = NULL;
	g_autoptr(VentureMailMessage) input = venture_mail_message_new(), row = NULL, claimed = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	gint64 id;
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	outbox = venture_mail_outbox_new(db, VENTURE_MAILER(mailer));
	g_object_set(input, "organization-id", (gint64)1, "to", "reader@example.test",
		"subject", "Campaign", "text-body", "Snapshot", "idempotency-key", "cancel-test", NULL);
	row = venture_mail_outbox_enqueue(outbox, input, NULL, &error);
	g_assert_no_error(error);
	id = venture_entity_get_id(VENTURE_ENTITY(row));
	g_assert_false(venture_mail_outbox_cancel(outbox, 2, id, "Wrong organization", NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_clear_error(&error);
	g_assert_true(venture_mail_outbox_cancel(outbox, 1, id, "Withdrawn", NULL, &error));
	g_assert_no_error(error);
	g_assert_true(venture_mail_outbox_cancel(outbox, 1, id, "Withdrawn again", NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpint(venture_mail_outbox_deliver_due(outbox, 1, 10, now, NULL, &error), ==, 0);
	g_assert_no_error(error);
	g_clear_object(&row);
	g_object_set(input, "idempotency-key", "cancel-sending", NULL);
	row = venture_mail_outbox_enqueue(outbox, input, NULL, &error);
	g_assert_no_error(error);
	id = venture_entity_get_id(VENTURE_ENTITY(row));
	claimed = venture_mail_outbox_claim(outbox, 1, id, now, &error);
	g_assert_no_error(error);
	g_assert_nonnull(claimed);
	g_assert_false(venture_mail_outbox_cancel(outbox, 1, id, "Cannot know", NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	{
		g_autoptr(GDateTime) expired = g_date_time_add_seconds(now, 601);
		g_assert_cmpint(venture_mail_outbox_deliver_due(outbox, 1, 10, expired, NULL, &error), ==, 0);
		g_assert_no_error(error);
	}
	g_assert_false(venture_mail_outbox_cancel(outbox, 1, id, "Still uncertain", NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	venture_test_accounting_database_cleanup(db);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/marketing-mail/defer", test_defer);
	g_test_add_func("/marketing-mail/cancel", test_cancel);
	return g_test_run();
}
