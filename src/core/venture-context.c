/*
 * venture-context.c - The wiring every subsystem is handed
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

struct _VentureContext
{
	GObject parent_instance;

	VentureConfig		*config;
	VentureDatabase		*database;
	VentureEntityRegistry	*entities;
	VentureReportRegistry	*reports;
	VentureVentureTypeRegistry *venture_types;
	VentureConfirmationStore *confirmations;
	VentureAiService	*ai;
	VentureAutomation	*automation;
	VenturePluginManager	*plugins;
	VentureWorkService	*work;
	VentureKbService	*kb;

	GTimeZone		*timezone;
	gint64			 default_organization_id;
};

G_DEFINE_FINAL_TYPE(VentureContext, venture_context, G_TYPE_OBJECT)

static void
venture_context_finalize(GObject *object)
{
	VentureContext *self;

	self = VENTURE_CONTEXT(object);

	g_clear_object(&self->config);
	g_clear_object(&self->database);
	g_clear_object(&self->reports);
	g_clear_object(&self->venture_types);
	g_clear_object(&self->confirmations);
	g_clear_object(&self->ai);
	g_clear_object(&self->automation);
	g_clear_object(&self->plugins);
	g_clear_object(&self->work);
	g_clear_object(&self->kb);
	g_clear_pointer(&self->timezone, g_time_zone_unref);

	/* The entity registry is the process-wide default and is not owned. */

	G_OBJECT_CLASS(venture_context_parent_class)->finalize(object);
}

static void
venture_context_class_init(VentureContextClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = venture_context_finalize;
}

static void
venture_context_init(VentureContext *self)
{
}

VentureContext *
venture_context_new(
	VentureConfig	*config,
	VentureDatabase	*database
){
	VentureContext *self;

	g_return_val_if_fail(VENTURE_IS_CONFIG(config), NULL);
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);

	self = g_object_new(VENTURE_TYPE_CONTEXT, NULL);
	self->config = g_object_ref(config);
	self->database = g_object_ref(database);
	self->entities = venture_entity_registry_get_default();
	self->reports = venture_report_registry_new();
	self->venture_types = venture_venture_type_registry_new();
	self->timezone = venture_config_get_timezone(config);

	/*
	 * The confirmation queue exists whether or not AI does. It began as
	 * the assistant's, but a change proposed by an outside agent holding
	 * an API token has to land somewhere a person can answer it, and one
	 * queue answered from one page is the whole point. An install with
	 * `ai.enabled: false` still stages REST writes.
	 */
	{
		gint64 ttl;
		gint64 limit;

		g_object_get(config,
		             "ai-confirmation-ttl", &ttl,
		             "ai-confirmation-limit", &limit,
		             NULL);

		self->confirmations = venture_confirmation_store_new(database, ttl,
		                                                     limit);
	}

	venture_report_registry_register_builtins(self->reports);

	return self;
}

VentureConfirmationStore *
venture_context_get_confirmations(VentureContext *self)
{
	g_return_val_if_fail(VENTURE_IS_CONTEXT(self), NULL);

	return self->confirmations;
}

VentureConfig *
venture_context_get_config(VentureContext *self)
{
	g_return_val_if_fail(VENTURE_IS_CONTEXT(self), NULL);

	return self->config;
}

VentureDatabase *
venture_context_get_database(VentureContext *self)
{
	g_return_val_if_fail(VENTURE_IS_CONTEXT(self), NULL);

	return self->database;
}

VentureEntityRegistry *
venture_context_get_entity_registry(VentureContext *self)
{
	g_return_val_if_fail(VENTURE_IS_CONTEXT(self), NULL);

	return self->entities;
}

VentureReportRegistry *
venture_context_get_report_registry(VentureContext *self)
{
	g_return_val_if_fail(VENTURE_IS_CONTEXT(self), NULL);

	return self->reports;
}

GTimeZone *
venture_context_get_timezone(VentureContext *self)
{
	g_return_val_if_fail(VENTURE_IS_CONTEXT(self), NULL);

	return self->timezone;
}

gint
venture_context_get_fiscal_year_start_month(VentureContext *self)
{
	gint64 month;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(self), 1);

	g_object_get(self->config, "locale-fiscal-year-start-month", &month, NULL);

	return (gint)month;
}

VentureDateRange *
venture_context_parse_period(
	VentureContext	 *self,
	const gchar	 *text,
	GError		**error
){
	g_return_val_if_fail(VENTURE_IS_CONTEXT(self), NULL);

	/* Defaulting to the current month rather than to all time matters:
	 * an unqualified "how are sales doing" should answer about now, and
	 * an all-time default would quietly scan the whole table. */
	if (venture_string_is_empty(text))
		text = "this_month";

	return venture_date_range_parse(text, self->timezone,
		venture_context_get_fiscal_year_start_month(self), error);
}

gint64
venture_context_get_default_organization_id(VentureContext *self)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) organization = NULL;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(self), 0);

	if (0 != self->default_organization_id)
		return self->default_organization_id;

	/* Prefer the entity explicitly flagged as default. */
	query = venture_query_new(VENTURE_TYPE_ORGANIZATION);

	if (venture_query_add_filter_string(query, "is-default",
	                                    VENTURE_FILTER_OP_EQ, "true", NULL))
	{
		organization = venture_database_find_one(self->database, query, NULL);
	}

	if (NULL == organization)
	{
		g_autoptr(VentureQuery) any = NULL;

		/* Otherwise the first one, which on a fresh install is the
		 * organisation the migration seeded. */
		any = venture_query_new(VENTURE_TYPE_ORGANIZATION);
		organization = venture_database_find_one(self->database, any, NULL);
	}

	if (NULL == organization)
		return 0;

	self->default_organization_id = venture_entity_get_id(organization);

	return self->default_organization_id;
}

VentureVentureTypeRegistry *
venture_context_get_venture_types(VentureContext *self)
{
	g_return_val_if_fail(VENTURE_IS_CONTEXT(self), NULL);

	return self->venture_types;
}

void
venture_context_set_ai_service(
	VentureContext		*self,
	VentureAiService	*service
){
	g_return_if_fail(VENTURE_IS_CONTEXT(self));

	g_set_object(&self->ai, service);
}

void
venture_context_set_kb_service(
	VentureContext		*self,
	VentureKbService	*service
){
	g_return_if_fail(VENTURE_IS_CONTEXT(self));

	g_set_object(&self->kb, service);
}

VentureKbService *
venture_context_get_kb_service(VentureContext *self)
{
	g_return_val_if_fail(VENTURE_IS_CONTEXT(self), NULL);

	return self->kb;
}

VentureAiService *
venture_context_get_ai_service(VentureContext *self)
{
	g_return_val_if_fail(VENTURE_IS_CONTEXT(self), NULL);

	return self->ai;
}

void
venture_context_set_automation(
	VentureContext		*self,
	VentureAutomation	*automation
){
	g_return_if_fail(VENTURE_IS_CONTEXT(self));

	g_set_object(&self->automation, automation);
}

VentureAutomation *
venture_context_get_automation(VentureContext *self)
{
	g_return_val_if_fail(VENTURE_IS_CONTEXT(self), NULL);

	return self->automation;
}

void
venture_context_set_work_service(
	VentureContext		*self,
	VentureWorkService	*service
){
	g_return_if_fail(VENTURE_IS_CONTEXT(self));

	g_set_object(&self->work, service);
}

VentureWorkService *
venture_context_get_work_service(VentureContext *self)
{
	g_return_val_if_fail(VENTURE_IS_CONTEXT(self), NULL);

	return self->work;
}

void
venture_context_set_plugin_manager(
	VentureContext		*self,
	VenturePluginManager	*manager
){
	g_return_if_fail(VENTURE_IS_CONTEXT(self));

	g_set_object(&self->plugins, manager);
}

VenturePluginManager *
venture_context_get_plugin_manager(VentureContext *self)
{
	g_return_val_if_fail(VENTURE_IS_CONTEXT(self), NULL);

	return self->plugins;
}
