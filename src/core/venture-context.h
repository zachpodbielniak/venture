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
 * venture_context_get_plugin_provides:
 * @self: a #VentureContext
 *
 * The kinds a plugin manifest's `provides` list may name. A subsystem that
 * can take something from a plugin -- a data-source provider, say --
 * registers its kind here, and the plugin manager hands it each entry that
 * names it.
 *
 * Returns: (transfer none): the registry, created on first use
 */
VenturePluginProvidesRegistry *
venture_context_get_plugin_provides(VentureContext *self);

/**
 * venture_context_get_automation_handlers:
 * @self: a #VentureContext
 *
 * The handlers a rule may call on the `venture` pod module and the events
 * a pod may bind to it. Created on first use with the built-ins already
 * in it. It lives here rather than on the engine because plugins load
 * before the engine is built, and a reload rebuilds the engine without
 * losing what a plugin registered.
 *
 * Returns: (transfer none): the registry
 */
VentureAutomationHandlerRegistry *
venture_context_get_automation_handlers(VentureContext *self);

/**
 * VentureWebExtensionFunc:
 * @server: the web server being built
 * @user_data: the data given to venture_context_add_web_extension()
 * @error: (out) (optional): return location for a #GError
 *
 * Adds a plugin's pages to a web server: routes through
 * venture_web_server_add_classified_route(), each carrying its own
 * authentication check, and sidebar rows through
 * venture_web_server_add_nav_link(). Called once per server, after every
 * built-in route is registered.
 *
 * Returns: %TRUE on success. A failure is logged and the server starts
 *   without the rest of this extension; routes it added before failing
 *   stay.
 */
typedef gboolean (*VentureWebExtensionFunc) (
	VentureWebServer	 *server,
	gpointer		  user_data,
	GError			**error
);

/**
 * venture_context_add_web_extension:
 * @self: a #VentureContext
 * @func: (scope notified) (closure user_data) (destroy destroy): the
 *   extension
 * @user_data: (nullable): data for @func
 * @destroy: (nullable): frees @user_data with the context
 *
 * Registers pages for every web server built over this context. Plugins
 * load before the server exists, so a plugin cannot add a route directly;
 * it leaves this instead, and venture_web_server_new() runs each extension,
 * in the order added, after its own routes -- which also means a built-in
 * route always wins a path both match.
 */
void
venture_context_add_web_extension(
	VentureContext		*self,
	VentureWebExtensionFunc	 func,
	gpointer		 user_data,
	GDestroyNotify		 destroy
);

/**
 * venture_context_count_web_extensions:
 * @self: a #VentureContext
 *
 * Returns: how many web extensions are registered
 */
guint
venture_context_count_web_extensions(VentureContext *self);

/**
 * venture_context_remove_web_extension:
 * @self: a #VentureContext
 * @index: the position, counting from 0 in the order added
 *
 * Drops one extension, freeing its data. Extensions have no names and are
 * kept in the order added, so the plugin manager takes back a failed
 * plugin's by position: those added since its load began. Nothing else
 * should need to.
 */
void
venture_context_remove_web_extension(
	VentureContext	*self,
	guint		 index
);

/**
 * venture_context_run_web_extensions:
 * @self: a #VentureContext
 * @server: the server being built
 *
 * Runs every registered web extension against @server. Called by
 * venture_web_server_new(); nothing else should need to.
 *
 * Returns: how many extensions succeeded
 */
guint
venture_context_run_web_extensions(
	VentureContext		*self,
	VentureWebServer	*server
);

/**
 * venture_context_get_stripe_service:
 * @self: context
 * Returns: (transfer none) (nullable): explicitly injected provider; NULL in production or when switched off
 */
VentureStripeService *venture_context_get_stripe_service(VentureContext *self);
/**
 * venture_context_set_stripe_service:
 * @self: context
 * @service: (nullable): configured provider
 *
 * Attaches an explicit test provider. Production resolves an organization binding per operation.
 */
void venture_context_set_stripe_service(VentureContext *self, VentureStripeService *service);
/**
 * venture_context_start_stripe:
 * @self: context
 * @error: (out) (optional): module start error
 * Returns: TRUE; organization credentials are resolved lazily, never from deployment defaults
 */
gboolean venture_context_start_stripe(VentureContext *self, GError **error);
/**
 * venture_context_start_feeds:
 * @self: a #VentureContext
 * @error: (out) (optional): return location for a #GError
 *
 * Starts market data feeds when the module is on and a source runs on its
 * own schedule; does nothing otherwise, and nothing in a build without
 * SQLite, which has no series store.
 *
 * Returns: %TRUE
 */
gboolean venture_context_start_feeds(VentureContext *self, GError **error);

/**
 * venture_context_get_bankfeed_service:
 * @self: the service or registry instance
 *
 * Returns: (transfer none) (nullable): borrowed result
 */
VentureBankFeedService *venture_context_get_bankfeed_service(VentureContext *self);
/**
 * venture_context_set_bankfeed_service:
 * @self: the service or registry instance
 * @service: (nullable) (transfer none): replacement service; the context retains a reference
 *
 * Replaces the context-owned service reference.
 */
void venture_context_set_bankfeed_service(VentureContext *self, VentureBankFeedService *service);
/**
 * venture_context_start_bankfeed:
 * @self: the service or registry instance
 * @error: (out) (optional): return location for an error
 *
 * Returns: TRUE on success, FALSE on failure
 */
gboolean venture_context_start_bankfeed(VentureContext *self, GError **error);
/**
 * venture_context_get_commerce_service:
 * @self: the service or registry instance
 *
 * Returns: (transfer none) (nullable): borrowed result
 */
VentureCommerceService *venture_context_get_commerce_service(VentureContext *self);
/**
 * venture_context_set_commerce_service:
 * @self: the service or registry instance
 * @service: (nullable) (transfer none): replacement service; the context retains a reference
 *
 * Replaces the context-owned service reference.
 */
void venture_context_set_commerce_service(VentureContext *self, VentureCommerceService *service);
/**
 * venture_context_start_commerce:
 * @self: the service or registry instance
 * @error: (out) (optional): return location for an error
 *
 * Returns: TRUE on success, FALSE on failure
 */
gboolean venture_context_start_commerce(VentureContext *self, GError **error);

/**
 * venture_context_get_mailer:
 * @self: context
 * Returns: (transfer none) (nullable): configured transport, or NULL with mail off
 */
VentureMailer *venture_context_get_mailer(VentureContext *self);
/**
 * venture_context_get_mail_outbox:
 * @self: context
 * Returns: (transfer none) (nullable): durable outbox, or NULL with mail off
 */
VentureMailOutbox *venture_context_get_mail_outbox(VentureContext *self);

/**
 * venture_context_set_mailer:
 * @self: context
 * @mailer: replacement SMTP implementation; retained by the registry
 */
void venture_context_set_mailer(VentureContext *self, VentureMailer *mailer);
/**
 * venture_context_get_mailer_registry:
 * @self: context
 * Returns: (transfer none): implementation registry, with the active "smtp" slot
 */
VentureMailerRegistry *venture_context_get_mailer_registry(VentureContext *self);
G_END_DECLS

#endif /* VENTURE_CONTEXT_H */
