/*
 * venture-automation.h - Scheduled automations, driven by podomation
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A podomation engine runs inside the server process, so an automation sees
 * the same database, the same validation and the same audit trail as
 * everything else. Rules are written in podomation's DSL:
 *
 * |[
 * pod nightly = cron->new("0 2 * * *");
 * nightly->on_tick => venture->report("pnl", "yesterday")
 *     | log->write_info("Yesterday: {pipe->summary}");
 *
 * pod stock = timer->new(3600000);
 * stock->on_tick => venture->low_stock("")
 *     | log->write_info("{pipe->count} items need reordering");
 * ]|
 *
 * The `venture` module is shipped by this repository rather than by
 * podomation, which is what the upstream module system is for. It is both an
 * event source -- record changes become events -- and a handler, so an
 * automation can react to a sale being recorded and can query, report and
 * create in response.
 *
 * Automations run as the `automation` actor, so anything they change is
 * distinguishable in the audit log from a person's edit or an AI's.
 */

#ifndef VENTURE_AUTOMATION_H
#define VENTURE_AUTOMATION_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>

G_BEGIN_DECLS

#define VENTURE_TYPE_AUTOMATION (venture_automation_get_type())

G_DECLARE_FINAL_TYPE(VentureAutomation, venture_automation,
                     VENTURE, AUTOMATION, GObject)

/**
 * venture_automation_new:
 * @context: the wiring
 * @error: (out) (optional): return location for a #GError
 *
 * Creates the engine and loads the VENTURE pod modules.
 *
 * Returns %NULL with %VENTURE_ERROR_CONFIG when automation is disabled. Like
 * AI, that is a normal state rather than a failure.
 *
 * Returns: (transfer full) (nullable): the engine, or %NULL
 */
VentureAutomation *
venture_automation_new(
	VentureContext	 *context,
	GError		**error
);

/**
 * venture_automation_start:
 * @self: a #VentureAutomation
 * @error: (out) (optional): return location for a #GError
 *
 * Parses the configured DSL file and starts the engine on the current main
 * context, so automations share the server's main loop rather than running
 * on a thread of their own.
 *
 * A missing DSL file is not an error: an install with no automations yet
 * simply has none.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_automation_start(
	VentureAutomation	 *self,
	GError			**error
);

/**
 * venture_automation_reload:
 * @self: a #VentureAutomation
 * @error: (out) (optional): return location for a #GError
 *
 * Tears the engine down and rebuilds it from the configured pods file. A
 * reload is a rebuild rather than a re-parse: pods hold timers and state,
 * and the only path where what runs afterwards is exactly what the file
 * says is to start from nothing.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_automation_reload(
	VentureAutomation	 *self,
	GError			**error
);

/**
 * venture_automation_validate_dsl:
 * @dsl: pod DSL source text
 * @out_message: (out) (optional) (transfer full): the parse error, or %NULL
 *
 * Checks that @dsl parses, without touching any running engine. This is the
 * rules editor's diagnostics: the message comes from the same parser that
 * will load the file, so what validates here loads there.
 *
 * Returns: %TRUE if the source parses
 */
gboolean
venture_automation_validate_dsl(
	const gchar	 *dsl,
	gchar		**out_message
);

/**
 * venture_automation_describe_modules:
 * @self: a #VentureAutomation
 *
 * Returns: (transfer full): a JSON array naming every loaded pod module
 */
JsonNode *
venture_automation_describe_modules(VentureAutomation *self);

/**
 * venture_automation_stop:
 * @self: a #VentureAutomation
 *
 * Stops the engine, allowing in-flight automations the configured grace
 * period to finish.
 */
void
venture_automation_stop(VentureAutomation *self);

/**
 * venture_automation_is_running:
 * @self: a #VentureAutomation
 *
 * Returns: %TRUE if the engine is running
 */
gboolean
venture_automation_is_running(VentureAutomation *self);

/**
 * venture_automation_is_dispatching:
 * @self: (nullable): a #VentureAutomation
 *
 * Whether an event is being handed to the pods right now -- that is,
 * whether the caller is running inside an automation handler, where the
 * cascade guard swallows every event a write would raise. Work whose
 * whole point is the events it raises (an alert hit's webhook and
 * on_created) waits until this is %FALSE rather than being written where
 * nobody would hear it.
 *
 * Returns: %TRUE inside a handler; %FALSE otherwise, and for %NULL
 */
gboolean
venture_automation_is_dispatching(VentureAutomation *self);

/**
 * venture_automation_load_dsl:
 * @self: a #VentureAutomation
 * @dsl: the rules to parse
 * @error: (out) (optional): return location for a #GError
 *
 * Parses DSL text directly. Used by the test suite and by a future
 * edit-rules-in-the-UI flow.
 *
 * Returns: %TRUE if the rules parsed
 */
gboolean
venture_automation_load_dsl(
	VentureAutomation	 *self,
	const gchar		 *dsl,
	GError			**error
);

/**
 * venture_automation_get_pod_count:
 * @self: a #VentureAutomation
 *
 * Returns: how many pods are defined
 */
guint
venture_automation_get_pod_count(VentureAutomation *self);

/**
 * venture_automation_emit_entity_event:
 * @self: a #VentureAutomation
 * @event_name: "on_created", "on_updated" or "on_deleted"
 * @entity: the record concerned
 *
 * Fires a record-change event into the engine. Connected automatically to
 * the database's signals, which is how "when a sale is recorded, do X" works
 * without the write path knowing automations exist.
 */
void
venture_automation_emit_entity_event(
	VentureAutomation	*self,
	const gchar		*event_name,
	VentureEntity		*entity
);

/**
 * venture_automation_emit:
 * @context: the wiring
 * @event_name: an event registered with
 *   venture_automation_handler_registry_add_event()
 * @data: (nullable) (transfer floating): the event's data, an `a{sv}`; a
 *   pod's pipeline reads its members as `{event->member}`. %NULL is an
 *   empty dictionary
 * @error: (out) (optional): return location for a #GError
 *
 * Raises a custom event into every pod built on the `venture` module. This
 * is how a plugin lets a rule say "when my feed finishes, do X" without the
 * plugin knowing automations exist, the same bargain the record-change
 * events strike for the database.
 *
 * Refused with %VENTURE_ERROR_AUTOMATION: an event nobody registered (so a
 * typo is found the first time, whether or not automation is on), and the
 * record-change events, which only the database raises. An @data that is
 * not a dictionary is refused with %VENTURE_ERROR_INVALID_ARGUMENT.
 *
 * With automation off or not yet started it succeeds and nothing happens.
 * Raised from inside a handler it also does nothing: the cascade guard
 * that stops an automation's own writes from triggering automations holds
 * for events too. Main thread only, like every handler.
 *
 * Returns: %TRUE unless the event was refused
 */
gboolean
venture_automation_emit(
	VentureContext	 *context,
	const gchar	 *event_name,
	GVariant	 *data,
	GError		**error
);

/**
 * venture_automation_register_builtins:
 * @registry: a #VentureAutomationHandlerRegistry
 *
 * Registers the built-in handlers -- query, count, report, create and the
 * modules' sweeps -- and the three record-change events. The context does
 * this when it first hands its registry out, so that the built-ins always
 * come first and no plugin can take one of their names.
 */
void
venture_automation_register_builtins(VentureAutomationHandlerRegistry *registry);

/**
 * venture_automation_register_provides:
 * @registry: the context's #VenturePluginProvidesRegistry
 *
 * Registers the `automation_handler` provides kind, through which an exec
 * plugin's manifest names a handler its program answers. The context does
 * this when it first hands its provides registry out, before any plugin
 * loads.
 */
void
venture_automation_register_provides(VenturePluginProvidesRegistry *registry);

/**
 * venture_automation_invoke:
 * @self: a #VentureAutomation
 * @handler: the handler to run, e.g. "report" or "low_stock"
 * @arguments: (array zero-terminated=1) (nullable): the handler's positional
 *   arguments
 * @result: (out) (optional) (transfer full): return location for what the
 *   handler produced
 * @error: (out) (optional): return location for a #GError
 *
 * Runs one of the `venture` module's handlers immediately, as though an
 * event had triggered it. Any handler in the context's registry may be
 * named, a plugin's as well as a built-in; an unknown one is refused with
 * %VENTURE_ERROR_AUTOMATION, naming those that exist, and a plugin
 * handler's own error comes back as it was raised.
 *
 * This exists because the alternative way to find out what a rule will do is
 * to wait for its trigger and read the log afterwards. Writes go through the
 * same audited path an event-driven run uses, so what this does is what the
 * rule will do.
 *
 * Returns: %TRUE if the handler ran successfully
 */
gboolean
venture_automation_invoke(
	VentureAutomation	  *self,
	const gchar		  *handler,
	const gchar *const	  *arguments,
	GVariant		 **result,
	GError			 **error
);

/**
 * venture_automation_describe:
 * @self: a #VentureAutomation
 *
 * Returns: (transfer full): the engine's state and its pods, as JSON
 */
JsonNode *
venture_automation_describe(VentureAutomation *self);

G_END_DECLS

#endif /* VENTURE_AUTOMATION_H */
