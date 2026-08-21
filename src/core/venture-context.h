/*
 * venture-context.h - The wiring every subsystem is handed
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A #VentureContext holds the configuration, the database, the registries
 * and the services. It is constructed once at startup and passed to
 * everything: reports, AI tools, automation handlers, web routes, plugins.
 *
 * It exists so that none of those reach for a global. That is not
 * fastidiousness -- it is what lets the test suite build a context over an
 * in-memory database with AI disabled and exercise the same code the server
 * runs, and what lets a plugin be handed exactly the capabilities it is
 * allowed to have.
 *
 * The subsystems are attached after construction rather than built in the
 * constructor, because several of them need the context themselves.
 */

#ifndef VENTURE_CONTEXT_H
#define VENTURE_CONTEXT_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>

G_BEGIN_DECLS

#define VENTURE_TYPE_CONTEXT (venture_context_get_type())

G_DECLARE_FINAL_TYPE(VentureContext, venture_context, VENTURE, CONTEXT, GObject)

/**
 * venture_context_new:
 * @config: the configuration
 * @database: the open database
 *
 * Creates a context. The entity and report registries are attached
 * automatically; the AI, automation and plugin services are attached later
 * by the application if they are enabled.
 *
 * Returns: (transfer full): a new #VentureContext
 */
VentureContext *
venture_context_new(
	VentureConfig	*config,
	VentureDatabase	*database
);

/**
 * venture_context_get_config:
 * @self: a #VentureContext
 *
 * Returns: (transfer none): the configuration
 */
VentureConfig *
venture_context_get_config(VentureContext *self);

/**
 * venture_context_get_database:
 * @self: a #VentureContext
 *
 * Returns: (transfer none): the database
 */
VentureDatabase *
venture_context_get_database(VentureContext *self);

/**
 * venture_context_get_entity_registry:
 * @self: a #VentureContext
 *
 * Returns: (transfer none): the record type registry
 */
VentureEntityRegistry *
venture_context_get_entity_registry(VentureContext *self);

/**
 * venture_context_get_report_registry:
 * @self: a #VentureContext
 *
 * Returns: (transfer none): the report registry
 */
VentureReportRegistry *
venture_context_get_report_registry(VentureContext *self);

/**
 * venture_context_get_timezone:
 * @self: a #VentureContext
 *
 * Returns: (transfer none): the configured timezone, used for every period
 *   boundary and date rendering
 */
GTimeZone *
venture_context_get_timezone(VentureContext *self);

/**
 * venture_context_get_fiscal_year_start_month:
 * @self: a #VentureContext
 *
 * Returns: the month the fiscal year begins, 1-12
 */
gint
venture_context_get_fiscal_year_start_month(VentureContext *self);

/**
 * venture_context_parse_period:
 * @self: a #VentureContext
 * @text: (nullable): a period expression; %NULL means the current month
 * @error: (out) (optional): return location for a #GError
 *
 * Parses a period in the configured timezone and fiscal year, so every
 * caller -- the CLI, the API, the AI, a report -- resolves "this quarter" to
 * the same instants.
 *
 * Returns: (transfer full) (nullable): the period, or %NULL on error
 */
VentureDateRange *
venture_context_parse_period(
	VentureContext	 *self,
	const gchar	 *text,
	GError		**error
);

/**
 * venture_context_get_default_organization_id:
 * @self: a #VentureContext
 *
 * Retrieves the organisation used when a request does not name one. This is
 * the entity flagged as default, or the first one if none is.
 *
 * Returns: the organisation identifier, or 0 if there are none
 */
gint64
venture_context_get_default_organization_id(VentureContext *self);

/**
 * venture_context_get_venture_types:
 * @self: a #VentureContext
 *
 * Retrieves the declaratively-defined venture types. Empty until the plugin
 * manager has loaded them, which is why this returns the registry rather
 * than the types: a caller holds it once and sees whatever is loaded.
 *
 * Returns: (transfer none): the venture type registry
 */
VentureVentureTypeRegistry *
venture_context_get_venture_types(VentureContext *self);

/**
 * venture_context_set_ai_service:
 * @self: a #VentureContext
 * @service: (nullable): the AI service
 *
 * Attaches the AI service. Left unset when AI is disabled or unconfigured,
 * in which case the chat dock and the AI tools are simply absent rather than
 * failing at use.
 */
void
venture_context_set_ai_service(
	VentureContext		*self,
	VentureAiService	*service
);

/**
 * venture_context_get_ai_service:
 * @self: a #VentureContext
 *
 * Returns: (transfer none) (nullable): the AI service, or %NULL
 */
VentureAiService *
venture_context_get_ai_service(VentureContext *self);

/**
 * venture_context_set_automation:
 * @self: a #VentureContext
 * @automation: (nullable): the automation engine
 */
void
venture_context_set_automation(
	VentureContext		*self,
	VentureAutomation	*automation
);

/**
 * venture_context_get_automation:
 * @self: a #VentureContext
 *
 * Returns: (transfer none) (nullable): the automation engine, or %NULL
 */
VentureAutomation *
venture_context_get_automation(VentureContext *self);

/**
 * venture_context_set_work_service:
 * @self: a #VentureContext
 * @service: (nullable): the coding-run service
 *
 * Attaches the service that runs coding agents. Left unset when runs are
 * turned off, which is the default -- in which case the run routes report
 * that rather than failing at use.
 */
void
venture_context_set_work_service(
	VentureContext		*self,
	VentureWorkService	*service
);

/**
 * venture_context_get_work_service:
 * @self: a #VentureContext
 *
 * Returns: (transfer none) (nullable): the service, or %NULL
 */
VentureWorkService *
venture_context_get_work_service(VentureContext *self);

/**
 * venture_context_set_plugin_manager:
 * @self: a #VentureContext
 * @manager: (nullable): the plugin host
 */
void
venture_context_set_plugin_manager(
	VentureContext		*self,
	VenturePluginManager	*manager
);

/**
 * venture_context_get_plugin_manager:
 * @self: a #VentureContext
 *
 * Returns: (transfer none) (nullable): the plugin host, or %NULL
 */
VenturePluginManager *
venture_context_get_plugin_manager(VentureContext *self);

G_END_DECLS

#endif /* VENTURE_CONTEXT_H */
