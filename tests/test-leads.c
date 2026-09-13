/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <string.h>

static void
test_records(void)
{
	static const gchar *const names[] = { "lead", "lead_form", "lead_assignment_rule" };
	g_autoptr(VentureModuleRegistry) modules = venture_module_registry_new();
	guint i;

	venture_module_registry_register_builtins(modules);
	for (i = 0; i < G_N_ELEMENTS(names); i++)
	{
		GType type = venture_entity_registry_lookup(venture_entity_registry_get_default(), names[i]);
		VentureModule *module;
		g_assert_cmpuint(type, !=, G_TYPE_INVALID);
		module = venture_module_registry_get_module_for_type(modules, names[i]);
		g_assert_nonnull(module);
		g_assert_cmpstr(venture_module_get_name(module), ==, "leads");
	}
}

typedef struct {
	VentureDatabase *db;
	VentureConfig *config;
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

static VentureEntity *
record(Fixture *f, const gchar *type, const gchar *name)
{
	GType t = venture_entity_registry_lookup(venture_entity_registry_get_default(), type);
	VentureEntity *e;
	g_assert_cmpuint(t, !=, G_TYPE_INVALID);
	e = g_object_new(t, "name", name, "organization-id", f->org, NULL);
	return e;
}

static void
save(Fixture *f, VentureEntity *e)
{
	g_autoptr(GError) error = NULL;
	gboolean ok = venture_database_save(f->db, e, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(ok);
}

static void
test_normalize(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) lead = record(f, "lead", "Inquiry");
	g_autofree gchar *email = NULL;
	g_autofree gchar *phone = NULL;
	g_autofree gchar *website = NULL;
	(void)data;
	g_object_set(lead, "email", " Alice+form@EXAMPLE.COM ", "phone", "+1 (555) 123-4567",
		"website", "https://www.Example.COM/path", NULL);
	save(f, lead);
	g_object_get(lead, "email", &email, "phone", &phone, "website", &website, NULL);
	g_assert_cmpstr(email, ==, "alice@example.com");
	g_assert_cmpstr(phone, ==, "15551234567");
	g_assert_cmpstr(website, ==, "example.com");
}

static void
test_assignment(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) rule = record(f, "lead_assignment_rule", "Rota");
	g_autoptr(VentureEntity) a = record(f, "lead", "A");
	g_autoptr(VentureEntity) b = record(f, "lead", "B");
	g_autofree gchar *owner = NULL;
	(void)data;
	g_object_set(rule, "active", TRUE, "assignees", "alice, bob", NULL);
	save(f, rule);
	save(f, a);
	save(f, b);
	g_object_get(a, "owner", &owner, NULL);
	g_assert_cmpstr(owner, ==, "alice");
	g_clear_pointer(&owner, g_free);
	g_object_get(b, "owner", &owner, NULL);
	g_assert_cmpstr(owner, ==, "bob");
}

static void
test_generic_conversion_refused(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) lead = record(f, "lead", "Inquiry");
	g_autoptr(GError) error = NULL;
	(void)data;
	g_object_set(lead, "status", VENTURE_LEAD_CONVERTED, NULL);
	g_assert_false(venture_database_save(f->db, lead, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_nonnull(strstr(error->message, "VentureLeadService"));
}

static void
test_duplicate_refused(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) a = record(f, "lead", "First");
	g_autoptr(VentureEntity) b = record(f, "lead", "Again");
	g_autoptr(GError) error = NULL;
	(void)data;
	g_object_set(a, "email", "alice@example.com", NULL);
	g_object_set(b, "email", "ALICE+web@example.com", NULL);
	save(f, a);
	g_assert_false(venture_database_save(f->db, b, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_ALREADY_EXISTS);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	venture_entity_registry_register_builtins(venture_entity_registry_get_default());
	g_test_add_func("/leads/records", test_records);
	g_test_add("/leads/normalize", Fixture, NULL, setup, test_normalize, teardown);
	g_test_add("/leads/assignment", Fixture, NULL, setup, test_assignment, teardown);
	g_test_add("/leads/generic-conversion-refused", Fixture, NULL, setup, test_generic_conversion_refused, teardown);
	g_test_add("/leads/duplicate-refused", Fixture, NULL, setup, test_duplicate_refused, teardown);
	return g_test_run();
}
