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

/* An upgrade must attach retained deals without inventing earlier history. */
static void
test_upgrade(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDatabase) db = venture_database_new("sqlite://:memory:", &error);
	g_autoptr(VentureEntity) deal = NULL;
	gint64 pipeline, stage;
	gint legacy;

	g_assert_true(venture_database_execute(db,
		"CREATE TABLE deals (id INTEGER PRIMARY KEY, name TEXT, stage TEXT, organization_id BIGINT, version BIGINT, uuid TEXT);"
		"INSERT INTO deals VALUES (42, 'Historical proposal', 'proposal', 1, 1, 'legacy-deal-42')", NULL, &error));
	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	deal = venture_database_get(db, VENTURE_TYPE_DEAL, 42, &error);
	g_assert_no_error(error);
	g_assert_nonnull(deal);
	g_object_get(deal, "pipeline-id", &pipeline, "stage-id", &stage, "stage", &legacy, NULL);
	g_assert_cmpint(pipeline, >, 0);
	g_assert_cmpint(stage, >, 0);
	g_assert_cmpint(legacy, ==, VENTURE_DEAL_STAGE_PROPOSAL);
	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
}

static void
test_reports_registered(void)
{
	static const gchar *names[] = { "stage_duration", "funnel", "forecast", "loss_reasons", "overdue_deals" };
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureDatabase) db = venture_database_new("sqlite://:memory:", NULL);
	g_autoptr(VentureContext) context = venture_context_new(config, db);
	guint i;
	for (i = 0; i < G_N_ELEMENTS(names); i++)
		g_assert_nonnull(venture_report_registry_lookup(venture_context_get_report_registry(context), names[i]));
}

static void
test_history_retained(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDatabase) db = venture_database_new("sqlite://:memory:", &error);
	g_autoptr(VentureDeal) deal = venture_deal_new();
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_DEAL_STAGE_ENTRY);
	g_autoptr(VentureEntity) entry = NULL;
	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error));
	g_object_set(deal, "name", "Retained history", "organization-id", (gint64)1, NULL);
	g_assert_true(venture_database_save(db, VENTURE_ENTITY(deal), NULL, &error));
	entry = venture_database_find_one(db, query, &error);
	g_assert_nonnull(entry);
	g_assert_false(venture_database_delete(db, entry, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	g_assert_false(venture_database_purge(db, entry, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

/* Pin numerical definitions, including aging without a background sweep. */
static void
test_report_values(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDatabase) db = venture_database_new("sqlite://:memory:", &error);
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureContext) context = venture_context_new(config, db);
	g_autoptr(VentureDateRange) period = venture_context_parse_period(context, "all_time", &error);
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(GDateTime) now = g_date_time_new_now_utc();
	guint i;
	gint64 open = 0, committed = 0;
	VentureReportRegistry *registry = venture_context_get_report_registry(context);

	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error));
	for (i = 0; i < 2; i++)
	{
		g_autoptr(VentureDeal) deal = venture_deal_new();
		g_autoptr(VentureEntity) stage = NULL;
		g_autoptr(GDateTime) entered = g_date_time_add_days(now, i == 0 ? -2 : -4);
		g_autofree gchar *stamp = g_date_time_format_iso8601(entered);
		g_autofree gchar *sql = NULL;
		gint64 stage_id;
		g_object_set(deal, "name", "Forecast sample", "organization-id", (gint64)1,
			"owner", i == 0 ? "ben" : "sue", "probability", (gint64)(i == 0 ? 40 : 95), NULL);
		g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(deal), "value", "100 USD", &error));
		g_assert_true(venture_database_save(db, VENTURE_ENTITY(deal), NULL, &error));
		g_object_get(deal, "stage-id", &stage_id, NULL);
		stage = venture_database_get(db, VENTURE_TYPE_PIPELINE_STAGE, stage_id, &error);
		g_object_set(stage, "rotting-days", (gint64)1, NULL);
		g_assert_true(venture_database_save(db, stage, NULL, &error));
		sql = g_strdup_printf("UPDATE deal_stage_entries SET entered_at='%s' WHERE deal_id=%" G_GINT64_FORMAT,
			stamp, venture_entity_get_id(VENTURE_ENTITY(deal)));
		g_assert_true(venture_database_execute(db, sql, NULL, &error));
	}
	result = venture_report_generate(venture_report_registry_lookup(registry, "forecast"), context, period, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 2);
	for (i = 0; i < 2; i++)
	{
		const GValue *cell = venture_report_result_get_cell(result, i, "open_weighted");
		g_assert_nonnull(cell);
		open += venture_money_get_amount(g_value_get_boxed(cell));
		cell = venture_report_result_get_cell(result, i, "committed");
		if (NULL != cell)
			committed += venture_money_get_amount(g_value_get_boxed(cell));
	}
	g_assert_cmpint(open, ==, 13500);
	g_assert_cmpint(committed, ==, 10000);
	g_clear_object(&result);
	result = venture_report_generate(venture_report_registry_lookup(registry, "stage_duration"), context, period, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 1);
	g_assert_cmpfloat_with_epsilon(g_value_get_double(venture_report_result_get_cell(result, 0, "average_days")), 3.0, 0.001);
	g_assert_cmpfloat_with_epsilon(g_value_get_double(venture_report_result_get_cell(result, 0, "median_days")), 3.0, 0.001);
	g_clear_object(&result);
	result = venture_report_generate(venture_report_registry_lookup(registry, "funnel"), context, period, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpfloat(g_value_get_double(venture_report_result_get_cell(result, 0, "entered")), ==, 2);
	g_assert_cmpfloat(g_value_get_double(venture_report_result_get_cell(result, 0, "converted")), ==, 0);
	g_clear_object(&result);
	result = venture_report_generate(venture_report_registry_lookup(registry, "overdue_deals"), context, period, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpfloat(venture_metric_get_number(g_ptr_array_index(venture_report_result_get_metrics(result), 0)), ==, 2);
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
	g_test_add_func("/pipelines/upgrade", test_upgrade);
	g_test_add_func("/pipelines/reports", test_reports_registered);
	g_test_add_func("/pipelines/history-retained", test_history_retained);
	g_test_add_func("/pipelines/report-values", test_report_values);
	return g_test_run();
}
