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
 * event had triggered it.
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
