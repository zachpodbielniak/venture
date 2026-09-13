/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>

/* Every surface must discover the same five durable record types. */
static void
test_records(void)
{
	const gchar *names[] = { "sequence", "sequence_step", "sequence_enrollment",
		"sequence_delivery", "suppression" };
	guint i;
	VentureEntityRegistry *registry = venture_entity_registry_get_default();

	venture_entity_registry_register_builtins(registry);
	for (i = 0; i < G_N_ELEMENTS(names); i++)
		g_assert_cmpuint(venture_entity_registry_lookup(registry, names[i]), !=, G_TYPE_INVALID);
}

typedef struct
{
	VentureDatabase *db;
	VentureContext *context;
	VentureConfig *config;
	VentureEntity *sequence;
	VentureEntity *contact;
	VentureEntity *step;
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
		"name", "Ada", "email", "Ada@Example.com", NULL);
	save(f, f->contact);
	f->sequence = g_object_new(VENTURE_TYPE_SEQUENCE, "organization-id", (gint64)1,
		"name", "Welcome", "active", TRUE, "timezone", "America/New_York",
		"send-window-start", (gint64)9, "send-window-end", (gint64)17,
		"weekdays", "1,2,3,4,5", "exit-on-reply", TRUE,
		"exit-on-unsubscribe", TRUE, "exit-on-deal-won", TRUE, NULL);
	save(f, f->sequence);
	f->step = g_object_new(VENTURE_TYPE_SEQUENCE_STEP, "organization-id", (gint64)1,
		"sequence-id", venture_entity_get_id(f->sequence), "position", (gint64)10,
		"active", TRUE, "subject", "Hello {contact.name}", "body", "For {contact.email}", NULL);
	save(f, f->step);
}

static void
teardown(Fixture *f, gconstpointer unused)
{
	(void)unused;
	g_clear_object(&f->step);
	g_clear_object(&f->contact);
	g_clear_object(&f->sequence);
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
}

static VentureEntity *
enrollment(Fixture *f)
{
	g_autoptr(GDateTime) at = g_date_time_new_from_iso8601("2026-09-11T22:00:00Z", NULL);
	return g_object_new(VENTURE_TYPE_SEQUENCE_ENROLLMENT, "organization-id", (gint64)1,
		"sequence-id", venture_entity_get_id(f->sequence),
		"contact-id", venture_entity_get_id(f->contact), "enrolled-at", at,
		"enrollment-reason", "Requested information", NULL);
}

/* Friday after closing must become Monday 09:00 in the chosen timezone. */
static void
test_enroll_window(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) row = enrollment(f);
	g_autoptr(GDateTime) next = NULL;
	g_autofree gchar *text = NULL;
	gint64 position;
	(void)unused;
	save(f, row);
	g_object_get(row, "next-run-at", &next, "current-step", &position, NULL);
	g_assert_nonnull(next);
	text = g_date_time_format_iso8601(next);
	g_assert_cmpstr(text, ==, "2026-09-14T13:00:00Z");
	g_assert_cmpint(position, ==, 10);
}

static void
test_duplicate(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) first = enrollment(f);
	g_autoptr(VentureEntity) second = enrollment(f);
	g_autoptr(GError) error = NULL;
	(void)unused;
	save(f, first);
	g_assert_false(venture_database_save(f->db, second, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_ALREADY_EXISTS);
}

static void
test_suppression(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) first = enrollment(f);
	g_autoptr(VentureEntity) suppression = NULL;
	g_autoptr(VentureEntity) stored = NULL;
	g_autoptr(VentureEntity) second = NULL;
	g_autofree gchar *email = NULL;
	g_autofree gchar *reason = NULL;
	g_autoptr(GError) error = NULL;
	gint status;
	(void)unused;
	save(f, first);
	suppression = g_object_new(VENTURE_TYPE_SUPPRESSION, "organization-id", (gint64)1,
		"email", " ADA@EXAMPLE.COM ", NULL);
	save(f, suppression);
	g_object_get(suppression, "email", &email, NULL);
	g_assert_cmpstr(email, ==, "ada@example.com");
	stored = venture_database_get(f->db, VENTURE_TYPE_SEQUENCE_ENROLLMENT,
		venture_entity_get_id(first), &error);
	g_assert_no_error(error);
	g_object_get(stored, "status", &status, "exit-reason", &reason, NULL);
	g_assert_cmpint(status, ==, 3);
	g_assert_nonnull(reason);
	second = enrollment(f);
	g_assert_false(venture_database_save(f->db, second, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

static void
test_reply(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) row = enrollment(f);
	g_autoptr(VentureEntity) interaction = NULL;
	g_autoptr(VentureEntity) stored = NULL;
	gint status;
	(void)unused;
	save(f, row);
	interaction = g_object_new(VENTURE_TYPE_INTERACTION, "organization-id", (gint64)1,
		"contact-id", venture_entity_get_id(f->contact), "subject", "Reply", "outbound", FALSE, NULL);
	save(f, interaction);
	stored = venture_database_get(f->db, VENTURE_TYPE_SEQUENCE_ENROLLMENT,
		venture_entity_get_id(row), NULL);
	g_object_get(stored, "status", &status, NULL);
	g_assert_cmpint(status, ==, 3);
}

static void
test_won(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) row = enrollment(f);
	g_autoptr(VentureEntity) deal = g_object_new(VENTURE_TYPE_DEAL,
		"organization-id", (gint64)1, "name", "Purchase",
		"contact-id", venture_entity_get_id(f->contact), NULL);
	g_autoptr(VentureEntity) stored = NULL;
	gint status;
	(void)unused;
	save(f, deal);
	g_object_set(row, "deal-id", venture_entity_get_id(deal), NULL);
	save(f, row);
	g_assert_true(venture_entity_set_field_from_string(deal, "stage", "won", NULL));
	save(f, deal);
	stored = venture_database_get(f->db, VENTURE_TYPE_SEQUENCE_ENROLLMENT,
		venture_entity_get_id(row), NULL);
	g_object_get(stored, "status", &status, NULL);
	g_assert_cmpint(status, ==, 3);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/sequences/records", test_records);
	g_test_add("/sequences/enroll_window", Fixture, NULL, setup, test_enroll_window, teardown);
	g_test_add("/sequences/duplicate", Fixture, NULL, setup, test_duplicate, teardown);
	g_test_add("/sequences/suppression", Fixture, NULL, setup, test_suppression, teardown);
	g_test_add("/sequences/reply", Fixture, NULL, setup, test_reply, teardown);
	g_test_add("/sequences/won", Fixture, NULL, setup, test_won, teardown);
	return g_test_run();
}
