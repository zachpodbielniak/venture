/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <string.h>

/* Configurable stages must participate in every generated record surface. */
static void
test_records(void)
{
	static const gchar *names[] = { "pipeline", "pipeline_stage", "deal_stage_entry", "loss_reason" };
	VentureEntityRegistry *registry = venture_entity_registry_get_default();
	guint i;

	for (i = 0; i < G_N_ELEMENTS(names); i++)
		g_assert_cmpuint(venture_entity_registry_lookup(registry, names[i]), !=, G_TYPE_INVALID);
}

/* Appending fields must retain the legacy enum and expose next actions. */
static void
test_deal_fields(void)
{
	static const gchar *names[] = { "stage", "pipeline-id", "stage-id", "next-step", "next-action-at", "loss-reason-id", "lost-note", "owner" };
	g_autoptr(VentureDeal) deal = venture_deal_new();
	guint i;

	for (i = 0; i < G_N_ELEMENTS(names); i++)
		g_assert_nonnull(g_object_class_find_property(G_OBJECT_GET_CLASS(deal), names[i]));
}

static void
test_first_use(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDatabase) db = venture_database_new("sqlite://:memory:", &error);
	g_autoptr(VentureDeal) deal = venture_deal_new();
	gint64 pipeline, stage;

	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error));
	g_object_set(deal, "name", "First opportunity", "organization-id", (gint64)1, NULL);
	g_assert_true(venture_database_save(db, VENTURE_ENTITY(deal), NULL, &error));
	g_assert_no_error(error);
	g_object_get(deal, "pipeline-id", &pipeline, "stage-id", &stage, NULL);
	g_assert_cmpint(pipeline, >, 0);
	g_assert_cmpint(stage, >, 0);
}

/* A generic writer must not bypass history by editing the legacy enum. */
static void
test_generic_refused(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDatabase) db = venture_database_new("sqlite://:memory:", &error);
	g_autoptr(VentureDeal) deal = venture_deal_new();

	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error));
	g_object_set(deal, "name", "Protected opportunity", "organization-id", (gint64)1, NULL);
	g_assert_true(venture_database_save(db, VENTURE_ENTITY(deal), NULL, &error));
	g_object_set(deal, "stage", VENTURE_DEAL_STAGE_WON, NULL);
	g_assert_false(venture_database_save(db, VENTURE_ENTITY(deal), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_nonnull(strstr(error->message, "VentureDealService"));
}

static void
test_move(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDatabase) db = venture_database_new("sqlite://:memory:", &error);
	g_autoptr(VentureDeal) deal = venture_deal_new();
	g_autoptr(VentureDeal) moved = NULL;
	g_autoptr(VenturePipelineStage) target = venture_pipeline_stage_new();
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_DEAL_STAGE_ENTRY);
	gint64 pipeline, stage_id;

	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error));
	g_object_set(deal, "name", "A sale", "organization-id", (gint64)1, NULL);
	g_assert_true(venture_database_save(db, VENTURE_ENTITY(deal), NULL, &error));
	g_object_get(deal, "pipeline-id", &pipeline, NULL);
	g_object_set(target, "organization-id", (gint64)1, "pipeline-id", pipeline,
		"name", "Discovery", "position", (gint64)7, "probability", (gint64)65,
		"required-fields", "next_step,owner", NULL);
	g_assert_true(venture_database_save(db, VENTURE_ENTITY(target), NULL, &error));
	stage_id = venture_entity_get_id(VENTURE_ENTITY(target));
	moved = venture_deal_service_move_stage(venture_database_get_deal_service(db), deal, stage_id, "Discovery", NULL, &error);
	g_assert_null(moved);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	g_object_set(deal, "next-step", "Demo", "owner", "ben", NULL);
	g_assert_true(venture_database_save(db, VENTURE_ENTITY(deal), NULL, &error));
	moved = venture_deal_service_move_stage(venture_database_get_deal_service(db), deal, stage_id, "Discovery", NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(moved);
	g_object_get(moved, "stage-id", &pipeline, NULL);
	g_assert_cmpint(pipeline, ==, stage_id);
	g_object_get(moved, "probability", &pipeline, NULL);
	g_assert_cmpint(pipeline, ==, 65);
	venture_query_set_organization(query, 1);
	g_assert_cmpint(venture_database_count(db, query, &error), ==, 2);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/pipelines/records", test_records);
	g_test_add_func("/pipelines/deal-fields", test_deal_fields);
	g_test_add_func("/pipelines/first-use", test_first_use);
	g_test_add_func("/pipelines/generic-refused", test_generic_refused);
	g_test_add_func("/pipelines/move", test_move);
	return g_test_run();
}
