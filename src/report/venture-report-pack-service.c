/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include <string.h>

static gboolean schedule_fields(const gchar *schedule, gint fields[5], GError **error);

static gboolean
validate_pack(VentureDatabase *database, VentureEntity *record, VentureEntity *previous,
	gpointer data, GError **error)
{
	g_autofree gchar *schedule = NULL;
	gint fields[5];
	(void)database;
	(void)previous;
	(void)data;
	g_object_get(record, "schedule", &schedule, NULL);
	return schedule == NULL || *schedule == '\0' || schedule_fields(schedule, fields, error);
}

struct _VentureReportPackService
{
	GObject parent_instance;
	VentureDatabase *database;
};
G_DEFINE_FINAL_TYPE(VentureReportPackService, venture_report_pack_service, G_TYPE_OBJECT)

static void
get_property(GObject *object, guint id, GValue *value, GParamSpec *spec)
{
	if (id == 1)
		g_value_set_object(value, VENTURE_REPORT_PACK_SERVICE(object)->database);
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

static void
set_property(GObject *object, guint id, const GValue *value, GParamSpec *spec)
{
	VentureReportPackService *self = VENTURE_REPORT_PACK_SERVICE(object);
	if (id == 1)
	{
		self->database = g_value_get_object(value);
		if (self->database != NULL)
			g_object_add_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	}
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

static void
finalize(GObject *object)
{
	VentureReportPackService *self = VENTURE_REPORT_PACK_SERVICE(object);
	if (self->database != NULL)
		g_object_remove_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	G_OBJECT_CLASS(venture_report_pack_service_parent_class)->finalize(object);
}

static void
venture_report_pack_service_class_init(VentureReportPackServiceClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	object_class->get_property = get_property;
	object_class->set_property = set_property;
	object_class->finalize = finalize;
	g_object_class_install_property(object_class, 1,
		g_param_spec_object("database", "Database", "Owning database", VENTURE_TYPE_DATABASE,
			G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
}

static void
venture_report_pack_service_init(VentureReportPackService *self)
{
	(void)self;
}

VentureReportPackService *
venture_report_pack_service_get(VentureDatabase *database)
{
	VentureReportPackService *self;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	self = g_object_get_data(G_OBJECT(database), "venture-report-pack-service");
	if (self == NULL)
	{
		self = g_object_new(VENTURE_TYPE_REPORT_PACK_SERVICE, "database", database, NULL);
		g_object_set_data_full(G_OBJECT(database), "venture-report-pack-service", self, g_object_unref);
		venture_database_add_save_validator(database, VENTURE_TYPE_REPORT_PACK, validate_pack, NULL, NULL);
	}
	return self;
}

VentureEntity *
venture_report_pack_service_save(VentureReportPackService *self, gint64 organization_id, const gchar *name,
	const gchar *report_name, const gchar *period, JsonObject *options, const gchar *dimension,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureSavedReport) saved = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *json = NULL;
	g_return_val_if_fail(VENTURE_IS_REPORT_PACK_SERVICE(self), NULL);
	saved = venture_saved_report_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(saved), organization_id);
	if (options != NULL)
	{
		node = json_node_new(JSON_NODE_OBJECT);
		json_node_set_object(node, options);
		json = venture_json_to_string(node, FALSE);
	}
	g_object_set(saved, "name", name, "report-name", report_name, "period", period ? period : "",
		"options", json ? json : "{}", "dimension", dimension ? dimension : "", NULL);
	if (!venture_database_save(self->database, VENTURE_ENTITY(saved), actor, error))
		return NULL;
	return VENTURE_ENTITY(g_steal_pointer(&saved));
}

VentureReportResult *
venture_report_pack_service_run(VentureReportPackService *self, VentureContext *context,
	VentureSavedReport *saved, GError **error)
{
	g_autofree gchar *report_name = NULL;
	g_autofree gchar *period_name = NULL;
	g_autofree gchar *options_text = NULL;
	g_autofree gchar *dimension = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(VentureDateRange) period = NULL;
	g_autoptr(JsonObject) owned = NULL;
	VentureReport *report;
	JsonObject *options = NULL;
	gint64 org;
	g_return_val_if_fail(VENTURE_IS_REPORT_PACK_SERVICE(self), NULL);
	org = venture_entity_get_organization_id(VENTURE_ENTITY(saved));
	g_object_get(saved, "report-name", &report_name, "period", &period_name, "options", &options_text,
		"dimension", &dimension, NULL);
	report = venture_report_registry_lookup(venture_context_get_report_registry(context), report_name);
	if (report == NULL)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Unknown report %s", report_name);
		return NULL;
	}
	period = venture_context_parse_period(context, period_name && period_name[0] ? period_name : "this_month", error);
	if (period == NULL)
		return NULL;
	if (options_text != NULL && options_text[0] != '\0')
	{
		node = json_from_string(options_text, error);
		if (node == NULL)
			return NULL;
		if (JSON_NODE_HOLDS_OBJECT(node))
			options = json_node_get_object(node);
	}
	if (options == NULL)
	{
		owned = json_object_new();
		options = owned;
	}
	if (org > 0)
		json_object_set_int_member(options, "organization_id", org);
	if (dimension != NULL && dimension[0] != '\0')
		json_object_set_string_member(options, "dimension", dimension);
	return venture_report_generate(report, context, period, options, error);
}

VentureEntity *
venture_report_pack_service_schedule(VentureReportPackService *self, gint64 organization_id, const gchar *name,
	const gchar *schedule, gint64 saved_report_id, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureReportPack) pack = NULL;
	g_autofree gchar *ids = NULL;
	g_return_val_if_fail(VENTURE_IS_REPORT_PACK_SERVICE(self), NULL);
	pack = venture_report_pack_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(pack), organization_id);
	ids = g_strdup_printf("%" G_GINT64_FORMAT, saved_report_id);
	g_object_set(pack, "name", name, "schedule", schedule ? schedule : "", "saved-report-ids", ids, NULL);
	if (!venture_database_save(self->database, VENTURE_ENTITY(pack), actor, error))
		return NULL;
	return VENTURE_ENTITY(g_steal_pointer(&pack));
}

GPtrArray *
venture_report_pack_service_run_pack(VentureReportPackService *self, VentureContext *context,
	VentureReportPack *pack, GError **error)
{
	g_autofree gchar *ids = NULL;
	g_auto(GStrv) parts = NULL;
	GPtrArray *results;
	guint i;
	g_return_val_if_fail(VENTURE_IS_REPORT_PACK_SERVICE(self), NULL);
	g_object_get(pack, "saved-report-ids", &ids, NULL);
	parts = ids ? g_strsplit(ids, ",", -1) : NULL;
	results = g_ptr_array_new_with_free_func(g_object_unref);
	for (i = 0; parts && parts[i]; i++)
	{
		gint64 id = g_ascii_strtoll(g_strstrip(parts[i]), NULL, 10);
		g_autoptr(VentureEntity) saved = NULL;
		VentureReportResult *result;
		if (id <= 0)
			continue;
		saved = venture_database_get(self->database, VENTURE_TYPE_SAVED_REPORT, id, error);
		if (saved == NULL)
		{
			g_ptr_array_unref(results);
			return NULL;
		}
		result = venture_report_pack_service_run(self, context, VENTURE_SAVED_REPORT(saved), error);
		if (result == NULL)
		{
			g_ptr_array_unref(results);
			return NULL;
		}
		g_ptr_array_add(results, result);
	}
	return results;
}

/* Numeric five-field cron; reject unsupported syntax rather than silently
 * turning a monthly or weekly schedule into a daily one. */
static gboolean
schedule_fields(const gchar *schedule, gint fields[5], GError **error)
{
	static const gint minimum[] = { 0, 0, 1, 1, 0 };
	static const gint maximum[] = { 59, 23, 31, 12, 7 };
	g_auto(GStrv) parts = NULL;
	guint i;

	parts = g_strsplit(schedule && g_str_equal(schedule, "daily") ? "0 0 * * *" : schedule, " ", -1);
	if (g_strv_length(parts) != 5)
		goto invalid;
	for (i = 0; i < 5; i++)
	{
		gchar *end = NULL;
		gint64 value;
		if (g_str_equal(parts[i], "*"))
		{
			fields[i] = -1;
			continue;
		}
		value = g_ascii_strtoll(parts[i], &end, 10);
		if (end == parts[i] || *end != '\0' || value < minimum[i] || value > maximum[i])
			goto invalid;
		fields[i] = value;
	}
	return TRUE;
invalid:
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		"Schedule requires five numeric or * fields (minute hour day month weekday), or daily");
	return FALSE;
}

static gint
pack_is_due(const gchar *schedule, GDateTime *last, GDateTime *as_of, GError **error)
{
	gint fields[5];
	gint hour, minute, dom, dow;
	gboolean day_matches;
	g_autoptr(GDateTime) midnight = NULL;
	g_autoptr(GDateTime) candidate = NULL;
	if (schedule == NULL || schedule[0] == '\0')
		return FALSE;
	if (!schedule_fields(schedule, fields, error))
		return -1;
	if (last != NULL && g_date_time_compare(last, as_of) >= 0)
		return FALSE;
	if (fields[3] >= 0 && fields[3] != g_date_time_get_month(as_of))
		return FALSE;
	dom = g_date_time_get_day_of_month(as_of);
	dow = g_date_time_get_day_of_week(as_of) % 7;
	day_matches = fields[2] < 0 ? fields[4] < 0 || fields[4] % 7 == dow :
		fields[4] < 0 ? fields[2] == dom : fields[2] == dom || fields[4] % 7 == dow;
	if (!day_matches)
		return FALSE;
	midnight = g_date_time_new(g_date_time_get_timezone(as_of), g_date_time_get_year(as_of),
		g_date_time_get_month(as_of), dom, 0, 0, 0);
	/* A delayed sweep catches up once on its scheduled day. Old days are
	 * not replayed; the output describes the time of this execution. */
	for (hour = g_date_time_get_hour(as_of); hour >= 0; hour--)
	{
		if (fields[1] >= 0 && fields[1] != hour)
			continue;
		for (minute = 59; minute >= 0; minute--)
		{
			if (fields[0] >= 0 && fields[0] != minute)
				continue;
			g_clear_pointer(&candidate, g_date_time_unref);
			candidate = g_date_time_add_minutes(midnight, hour * 60 + minute);
			if (g_date_time_compare(candidate, as_of) <= 0)
				return last == NULL || g_date_time_compare(candidate, last) > 0;
		}
	}
	return FALSE;
}

gint
venture_report_pack_service_run_due(VentureReportPackService *self, VentureContext *context,
	gint64 organization_id, GDateTime *as_of, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GDateTime) now = NULL;
	gint ran = 0;
	guint i;
	g_return_val_if_fail(VENTURE_IS_REPORT_PACK_SERVICE(self), -1);
	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), -1);
	now = as_of != NULL ? g_date_time_to_utc(as_of) : venture_time_now();
	query = venture_query_new(VENTURE_TYPE_REPORT_PACK);
	if (organization_id > 0)
		venture_query_set_organization(query, organization_id);
	venture_query_set_limit(query, 0);
	rows = venture_database_find(self->database, query, error);
	if (rows == NULL)
		return -1;
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *pack = g_ptr_array_index(rows, i);
		g_autofree gchar *schedule = NULL;
		g_autoptr(GDateTime) last = NULL;
		g_autoptr(GPtrArray) results = NULL;
		gint due;
		g_object_get(pack, "schedule", &schedule, "last-run-at", &last, NULL);
		due = pack_is_due(schedule, last, now, error);
		if (due < 0)
			return -1;
		if (due == 0)
			continue;
		results = venture_report_pack_service_run_pack(self, context, VENTURE_REPORT_PACK(pack), error);
		if (results == NULL)
			return -1;
		{
			g_autoptr(JsonNode) output = json_node_new(JSON_NODE_ARRAY);
			g_autofree gchar *json = NULL;
			JsonArray *array = json_array_new();
			guint j;
			json_node_take_array(output, array);
			for (j = 0; j < results->len; j++)
				json_array_add_element(array, venture_report_result_to_json(g_ptr_array_index(results, j)));
			json = venture_json_to_string(output, FALSE);
			g_object_set(pack, "last-run-at", now, "last-output", json, NULL);
		}
		if (!venture_database_save(self->database, pack, actor, error))
			return -1;
		ran++;
	}
	return ran;
}
