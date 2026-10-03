/*
 * venture-automation-registry.h - What the `venture` pod module answers to
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A rule calls `venture->report(...)` or `venture->dunning_sweep(...)`, and
 * a pod binds `watcher->on_created`. Which handler names and which event
 * names the `venture` module answers to used to be two fixed lists and an
 * if-chain in the module itself, so a plugin could add a record type, a
 * report and a page but never a step an automation could call.
 *
 * The registry holds both lists. It lives on the #VentureContext rather
 * than on the engine, because plugins load before the engine exists -- and
 * a reload rebuilds the engine, which must not forget what a plugin added.
 * The built-in handlers and the three record-change events are registered
 * first, when the context first hands the registry out; a name is never
 * registered twice, so a plugin cannot replace a built-in.
 */

#ifndef VENTURE_AUTOMATION_REGISTRY_H
#define VENTURE_AUTOMATION_REGISTRY_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>

G_BEGIN_DECLS

/**
 * VENTURE_AUTOMATION_NAME_MAX:
 *
 * The longest handler or event name accepted, in bytes.
 */
#define VENTURE_AUTOMATION_NAME_MAX (64)

/**
 * VentureAutomationHandlerFunc:
 * @context: the wiring
 * @name: the handler's name, as the rule called it
 * @params: (nullable): the rule's positional arguments -- a tuple of
 *   strings, a single string, or %NULL when there are none; read them with
 *   venture_automation_argument()
 * @result: (out) (optional) (transfer full): where to put what the handler
 *   produced, built with venture_automation_result_new()
 * @user_data: the data given at registration
 * @error: (out) (optional): return location for a #GError
 *
 * Runs one handler call. Handlers run synchronously on the main thread,
 * inside whatever dispatched them -- a pod, or a direct invocation -- and
 * may read and write the database. A handler that writes should write as
 * the automation actor (%VENTURE_ACTOR_KIND_AUTOMATION).
 *
 * A handler that fails sets @error. One that returns %FALSE without an
 * error is taken to have logged its own reason; its caller then reports
 * %VENTURE_ERROR_AUTOMATION and a pod does not log it a second time.
 *
 * Returns: %TRUE on success
 */
typedef gboolean (*VentureAutomationHandlerFunc) (
	VentureContext	 *context,
	const gchar	 *name,
	GVariant	 *params,
	GVariant	**result,
	gpointer	  user_data,
	GError		**error
);

#define VENTURE_TYPE_AUTOMATION_HANDLER_REGISTRY \
	(venture_automation_handler_registry_get_type())

G_DECLARE_FINAL_TYPE(VentureAutomationHandlerRegistry,
                     venture_automation_handler_registry,
                     VENTURE, AUTOMATION_HANDLER_REGISTRY, GObject)

/**
 * venture_automation_handler_registry_new:
 *
 * Creates an empty registry. The context's own registry, from
 * venture_context_get_automation_handlers(), already holds the built-ins;
 * this is for tests and for code that wants a registry of its own.
 *
 * Returns: (transfer full): a new, empty registry
 */
VentureAutomationHandlerRegistry *
venture_automation_handler_registry_new(void);

/**
 * venture_automation_handler_registry_add:
 * @self: a #VentureAutomationHandlerRegistry
 * @name: the handler's name: a lower-case letter, then lower-case letters,
 *   digits and `_`, at most %VENTURE_AUTOMATION_NAME_MAX bytes
 * @description: (nullable): one line for the rules editor's reference
 * @func: (scope notified) (closure user_data) (destroy destroy): the handler
 * @user_data: (nullable): data for @func
 * @destroy: (nullable): frees @user_data when the registry is finalized
 * @error: (out) (optional): return location for a #GError
 *
 * Registers a handler a rule can call as `venture-><name>(...)`. A name
 * already registered -- a built-in's or another plugin's -- is refused with
 * %VENTURE_ERROR_ALREADY_EXISTS: two handlers behind one name would make
 * what a rule does depend on which plugin loaded first. On failure
 * @destroy is not called; @user_data stays the caller's.
 *
 * Returns: %TRUE if the handler was registered
 */
gboolean
venture_automation_handler_registry_add(
	VentureAutomationHandlerRegistry	 *self,
	const gchar				 *name,
	const gchar				 *description,
	VentureAutomationHandlerFunc		  func,
	gpointer				  user_data,
	GDestroyNotify				  destroy,
	GError					**error
);

/**
 * venture_automation_handler_registry_has:
 * @self: a #VentureAutomationHandlerRegistry
 * @name: (nullable): a handler name
 *
 * Returns: %TRUE if a handler of that name is registered
 */
gboolean
venture_automation_handler_registry_has(
	VentureAutomationHandlerRegistry	*self,
	const gchar				*name
);

/**
 * venture_automation_handler_registry_get_names:
 * @self: a #VentureAutomationHandlerRegistry
 *
 * The handler names in the order they were registered, built-ins first.
 * The array stays valid for the registry's lifetime, but a later
 * registration is not in it: ask again after one.
 *
 * Returns: (transfer none) (array zero-terminated=1): the names
 */
const gchar *const *
venture_automation_handler_registry_get_names(
	VentureAutomationHandlerRegistry *self
);

/**
 * venture_automation_handler_registry_get_description:
 * @self: a #VentureAutomationHandlerRegistry
 * @name: a handler name
 *
 * Returns: (transfer none) (nullable): its description, or %NULL when it
 *   has none or is not registered
 */
const gchar *
venture_automation_handler_registry_get_description(
	VentureAutomationHandlerRegistry	*self,
	const gchar				*name
);

/**
 * venture_automation_handler_registry_call:
 * @self: a #VentureAutomationHandlerRegistry
 * @context: the wiring handed to the handler
 * @name: the handler to run
 * @params: (nullable): its positional arguments
 * @result: (out) (optional) (transfer full): what it produced
 * @error: (out) (optional): return location for a #GError
 *
 * Runs a handler by name. An unknown name is refused with
 * %VENTURE_ERROR_AUTOMATION naming every handler that exists. A handler
 * that fails without saying why is reported as %VENTURE_ERROR_AUTOMATION
 * ("see the log"), its own warning being the reason.
 *
 * Returns: %TRUE if the handler ran and succeeded
 */
gboolean
venture_automation_handler_registry_call(
	VentureAutomationHandlerRegistry	 *self,
	VentureContext				 *context,
	const gchar				 *name,
	GVariant				 *params,
	GVariant				**result,
	GError					**error
);

/**
 * venture_automation_handler_registry_add_event:
 * @self: a #VentureAutomationHandlerRegistry
 * @name: the event's name, spelled like a handler name
 * @description: (nullable): one line for the rules editor's reference
 * @error: (out) (optional): return location for a #GError
 *
 * Registers an event a pod built on the `venture` module can bind --
 * `watcher-><name> => ...` -- and venture_automation_emit() can raise. An
 * event already registered is refused with %VENTURE_ERROR_ALREADY_EXISTS.
 *
 * Returns: %TRUE if the event was registered
 */
gboolean
venture_automation_handler_registry_add_event(
	VentureAutomationHandlerRegistry	 *self,
	const gchar				 *name,
	const gchar				 *description,
	GError					**error
);

/**
 * venture_automation_handler_registry_has_event:
 * @self: a #VentureAutomationHandlerRegistry
 * @name: (nullable): an event name
 *
 * Returns: %TRUE if an event of that name is registered
 */
gboolean
venture_automation_handler_registry_has_event(
	VentureAutomationHandlerRegistry	*self,
	const gchar				*name
);

/**
 * venture_automation_handler_registry_get_events:
 * @self: a #VentureAutomationHandlerRegistry
 *
 * The event names in registration order, the record-change events first.
 * Valid for the registry's lifetime; a later registration is not in it.
 *
 * Returns: (transfer none) (array zero-terminated=1): the names
 */
const gchar *const *
venture_automation_handler_registry_get_events(
	VentureAutomationHandlerRegistry *self
);

/**
 * venture_automation_name_is_valid:
 * @name: (nullable): a candidate handler or event name
 *
 * Returns: %TRUE if @name is a lower-case letter followed by lower-case
 *   letters, digits and `_`, at most %VENTURE_AUTOMATION_NAME_MAX bytes
 */
gboolean
venture_automation_name_is_valid(const gchar *name);

/**
 * venture_automation_argument:
 * @params: (nullable): a handler's positional arguments
 * @index: which one, from 0
 *
 * Reads the @index-th positional argument of a rule's call as a string.
 * The DSL passes arguments as a tuple of strings, or a single string when
 * there is one.
 *
 * Returns: (transfer none) (nullable): the argument, owned by @params, or
 *   %NULL when there is no such argument or it is not a string
 */
const gchar *
venture_automation_argument(
	GVariant	*params,
	gsize		 index
);

/**
 * venture_automation_result_new:
 * @count: the figure a rule branches on
 * @summary: (nullable): one line a rule can put in a message
 * @detail: (nullable): the longer text, such as a rendered report
 *
 * Builds the dictionary every handler answers with, so a pipe stage
 * downstream reaches `{pipe->count}`, `{pipe->summary}` and
 * `{pipe->detail}` whichever handler produced it.
 *
 * Returns: (transfer floating): an `a{sv}` with `count` (int64), `summary`
 *   and `detail` (strings, empty when %NULL)
 */
GVariant *
venture_automation_result_new(
	gint64		 count,
	const gchar	*summary,
	const gchar	*detail
);

G_END_DECLS

#endif /* VENTURE_AUTOMATION_REGISTRY_H */
