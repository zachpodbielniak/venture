/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <string.h>

/* Planned work must be discoverable by every generated surface. */
static void
test_records(void)
{
	VentureEntityRegistry *registry = venture_entity_registry_get_default();
	g_autoptr(VentureEntity) activity = NULL;
	g_autoptr(VentureEntity) activity_type = NULL;
	g_autoptr(GError) error = NULL;
	GType type = venture_entity_registry_lookup(registry, "activity");
	const gchar *fields[] = { "organization-id", "kind", "subject", "body",
		"owner", "due-at", "starts-at", "ends-at", "priority", "status",
		"completed-at", "outcome", "related-type", "related-id", "contact-id",
		"company-id", "deal-id", "remind-at", "recurrence" };
	guint i;

	g_assert_cmpuint(type, !=, G_TYPE_INVALID);
	activity = g_object_new(type, NULL);
	for (i = 0; i < G_N_ELEMENTS(fields); i++)
		g_assert_nonnull(g_object_class_find_property(G_OBJECT_GET_CLASS(activity), fields[i]));
	g_assert_true(venture_entity_set_field_from_string(activity, "kind", "meeting", &error));
	g_assert_true(venture_entity_set_field_from_string(activity, "status", "planned", &error));
	g_assert_true(venture_entity_set_field_from_string(activity, "recurrence", "monthly", &error));
	g_assert_no_error(error);
	type = venture_entity_registry_lookup(registry, "activity_type");
	g_assert_cmpuint(type, !=, G_TYPE_INVALID);
	activity_type = g_object_new(type, NULL);
	g_assert_nonnull(g_object_class_find_property(G_OBJECT_GET_CLASS(activity_type), "default-duration"));
	g_assert_nonnull(g_object_class_find_property(G_OBJECT_GET_CLASS(activity_type), "active"));
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/activities/records", test_records);
	return g_test_run();
}
