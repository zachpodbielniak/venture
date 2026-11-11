/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include "venture-test-util.h"

/* The program stores a refund window, and a reward can be revoked. */

typedef struct {
	VentureConfig *config;
	VentureDatabase *db;
	VentureContext *context;
	gint64 org;
} Fixture;

static void
setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	(void)data;
	f->config = venture_config_new();
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
}

static void
teardown(Fixture *f, gconstpointer data)
{
	(void)data;
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
}

static void
test_window_field(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	VentureEntity *program = g_object_new(VENTURE_TYPE_REFERRAL_PROGRAM, "organization-id", f->org,
		"name", "Friends", "active", TRUE, "reward-kind", VENTURE_REFERRAL_REWARD_NONE,
		"refund-window-days", (gint64)30, NULL);
	g_autoptr(VentureEntity) fresh = NULL;
	GEnumClass *klass;
	gint64 days = -1;
	(void)data;
	g_assert_true(venture_database_save(f->db, program, NULL, &error));
	g_assert_no_error(error);
	fresh = venture_database_get(f->db, VENTURE_TYPE_REFERRAL_PROGRAM, venture_entity_get_id(program), &error);
	g_assert_no_error(error);
	g_object_get(fresh, "refund-window-days", &days, NULL);
	g_assert_cmpint(days, ==, 30);
	g_object_unref(program);
	program = g_object_new(VENTURE_TYPE_REFERRAL_PROGRAM, "organization-id", f->org,
		"name", "Closed", "active", TRUE, "reward-kind", VENTURE_REFERRAL_REWARD_NONE,
		"refund-window-days", (gint64)-1, NULL);
	g_assert_false(venture_database_save(f->db, program, NULL, &error));
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "negative"));
	g_object_unref(program);
	klass = g_type_class_ref(venture_referral_reward_status_get_type());
	g_assert_nonnull(g_enum_get_value_by_nick(klass, "revoked"));
	g_type_class_unref(klass);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	venture_entity_registry_register_builtins(venture_entity_registry_get_default());
	g_test_add("/referral-records/window", Fixture, NULL, setup, test_window_field, teardown);
	return g_test_run();
}
