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
 * venture_context_get_modules:
 * @self: a #VentureContext
 *
 * Retrieves the module registry, resolved against this context's
 * configuration. Every page, service and tool that belongs to a module
 * asks it before doing anything.
 *
 * Returns: (transfer none): the module registry
 */
VentureModuleRegistry *
venture_context_get_modules(VentureContext *self);

/**
 * venture_context_module_enabled:
 * @self: a #VentureContext
 * @module_name: a module name
 *
 * Shorthand for venture_module_registry_is_enabled() on the context's
 * registry. An unknown module is disabled.
 *
 * Returns: %TRUE if the module is on
 */
gboolean
venture_context_module_enabled(
	VentureContext	*self,
	const gchar	*module_name
);

/**
 * venture_context_register_module:
 * @self: a #VentureContext
 * @info: the module's description, which must outlive the context
 * @error: (out) (optional): return location for a #GError
 *
 * The one call a plugin makes to become a module: registers every record
 * type the description lists, adds the module -- resolving its switch and
 * its requirements against the running configuration -- and applies the
 * result, so a plugin's types are hidden when its module is off exactly as
 * a built-in module's are. A description that requires a module which is
 * off is an error, and the plugin should treat it as "not now" rather than
 * "broken".
 *
 * Returns: %TRUE on success
 */
gboolean
venture_context_register_module(
	VentureContext			 *self,
	const VentureModuleInfo		 *info,
	GError				**error
);

/**
 * venture_context_apply_modules:
 * @self: a #VentureContext
 *
 * Re-applies the resolved module states to the entity and report
 * registries. Called at construction, and again by the plugin manager once
 * a plugin has registered a module of its own, so the plugin's types are
 * masked the same way the built-in ones are.
 */
void
venture_context_apply_modules(VentureContext *self);

/**
 * venture_context_get_confirmations:
 * @self: a #VentureContext
 *
 * Retrieves the queue of changes proposed but not yet made.
 *
 * Always present, unlike the AI service: the queue is where a staged REST
 * write waits as well as an assistant's, and an install with AI switched off
 * still stages and still needs somewhere to put them.
 *
 * Returns: (transfer none): the confirmation store
 */
VentureConfirmationStore *
venture_context_get_confirmations(VentureContext *self);

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
 * venture_context_get_ai_harness:
 * @self: a #VentureContext
 *
 * The assistant's harness: the slash commands, the completion behind the
 * composer's menus, and the `@record` references. Built on first use.
 *
 * Returns: (transfer none): the harness
 */
VentureAiHarness *
venture_context_get_ai_harness(VentureContext *self);

/**
 * venture_context_set_kb_service:
 * @self: a #VentureContext
 * @service: (nullable): the knowledge-base service
 *
 * Holds the knowledge-base service so every surface shares one.
 *
 * Building one opens an embedding client, so a per-request instance would
 * mean a connection per request and, worse, a different one answering the
 * web UI than the AI -- which matters because the model a base is indexed
 * with is checked against the embedder's.
 */
void
venture_context_set_kb_service(
	VentureContext		*self,
	VentureKbService	*service
);

/**
 * venture_context_get_kb_service:
 * @self: a #VentureContext
 *
 * Returns: (transfer none) (nullable): the knowledge-base service, or %NULL
 *   when knowledge bases are disabled or the embedder could not be built
 */
VentureKbService *
venture_context_get_kb_service(VentureContext *self);

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

/**
 * venture_context_get_stripe_service:
 * @self: context
 * Returns: (transfer none) (nullable): the active provider; NULL when switched off
 */
VentureStripeService *venture_context_get_stripe_service(VentureContext *self);
/**
 * venture_context_set_stripe_service:
 * @self: context
 * @service: (nullable): configured provider
 *
 * Attaches the provider after configuration and schema startup.
 */
void venture_context_set_stripe_service(VentureContext *self, VentureStripeService *service);
/**
 * venture_context_start_stripe:
 * @self: context
 * @error: (out) (optional): module start error
 * Returns: TRUE when disabled or successfully started from deployment environment
 */
gboolean venture_context_start_stripe(VentureContext *self, GError **error);
VentureBankFeedService *venture_context_get_bankfeed_service(VentureContext *self);
void venture_context_set_bankfeed_service(VentureContext *self, VentureBankFeedService *service);
gboolean venture_context_start_bankfeed(VentureContext *self, GError **error);

/** venture_context_get_mailer:
 * @self: context
 * Returns: (transfer none) (nullable): configured transport, or NULL with mail off
 */
VentureMailer *venture_context_get_mailer(VentureContext *self);
/** venture_context_get_mail_outbox:
 * @self: context
 * Returns: (transfer none) (nullable): durable outbox, or NULL with mail off
 */
VentureMailOutbox *venture_context_get_mail_outbox(VentureContext *self);

/** venture_context_set_mailer:
 * @self: context
 * @mailer: replacement SMTP implementation; retained by the registry
 */
void venture_context_set_mailer(VentureContext *self, VentureMailer *mailer);
/** venture_context_get_mailer_registry:
 * @self: context
 * Returns: (transfer none): implementation registry, with the active "smtp" slot
 */
VentureMailerRegistry *venture_context_get_mailer_registry(VentureContext *self);
G_END_DECLS

#endif /* VENTURE_CONTEXT_H */
