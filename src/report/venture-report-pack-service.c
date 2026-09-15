/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include <string.h>

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
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(VentureDateRange) period = NULL;
	VentureReport *report;
	JsonObject *options = NULL;
	g_return_val_if_fail(VENTURE_IS_REPORT_PACK_SERVICE(self), NULL);
	g_object_get(saved, "report-name", &report_name, "period", &period_name, "options", &options_text, NULL);
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
