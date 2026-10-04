/*
 * venture-automation-registry.c - What the `venture` pod module answers to
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

#include <string.h>

typedef struct
{
	gchar				*name;
	gchar				*description;
	VentureAutomationHandlerFunc	 func;
	gpointer			 user_data;
	GDestroyNotify			 destroy;
} HandlerEntry;

typedef struct
{
	gchar	*name;
	gchar	*description;
} EventEntry;

static void
handler_entry_free(gpointer data)
{
	HandlerEntry *entry;

	entry = data;

	if (NULL != entry->destroy)
		entry->destroy(entry->user_data);

	g_free(entry->name);
	g_free(entry->description);
	g_free(entry);
}

static void
event_entry_free(gpointer data)
{
	EventEntry *entry;

	entry = data;

	g_free(entry->name);
	g_free(entry->description);
	g_free(entry);
}

struct _VentureAutomationHandlerRegistry
{
	GObject parent_instance;

	/* In registration order: the order is what the rules editor lists
	 * and what an "available handlers" refusal names, built-ins first. */
	GPtrArray	*handlers;	/* HandlerEntry */
	GPtrArray	*events;	/* EventEntry */
	GHashTable	*by_name;	/* name -> HandlerEntry, borrowed */

	/*
	 * Every name list ever handed out, kept until finalize.
	 *
	 * podomation asks the module for its supported handlers and events
	 * and may hold the answer; freeing the previous array when a plugin
	 * registered another name would leave it reading freed memory.
	 * Registrations happen a handful of times per process, so keeping
	 * each superseded array costs nothing worth saving.
	 */
	GPtrArray	*name_lists;	/* GStrv */
	gchar		**handler_names;
	gchar		**event_names;
};

G_DEFINE_FINAL_TYPE(VentureAutomationHandlerRegistry,
                    venture_automation_handler_registry, G_TYPE_OBJECT)

static void
venture_automation_handler_registry_finalize(GObject *object)
{
	VentureAutomationHandlerRegistry *self;

	self = VENTURE_AUTOMATION_HANDLER_REGISTRY(object);

	g_clear_pointer(&self->by_name, g_hash_table_unref);
	g_clear_pointer(&self->handlers, g_ptr_array_unref);
	g_clear_pointer(&self->events, g_ptr_array_unref);
	g_clear_pointer(&self->name_lists, g_ptr_array_unref);

	G_OBJECT_CLASS(venture_automation_handler_registry_parent_class)
		->finalize(object);
}

static void
venture_automation_handler_registry_class_init(
	VentureAutomationHandlerRegistryClass *klass
){
	G_OBJECT_CLASS(klass)->finalize =
		venture_automation_handler_registry_finalize;
}

static void
venture_automation_handler_registry_init(
	VentureAutomationHandlerRegistry *self
){
	self->handlers = g_ptr_array_new_with_free_func(handler_entry_free);
	self->events = g_ptr_array_new_with_free_func(event_entry_free);
	self->by_name = g_hash_table_new(g_str_hash, g_str_equal);
	self->name_lists = g_ptr_array_new_with_free_func(
		(GDestroyNotify)g_strfreev);
	self->handler_names = NULL;
	self->event_names = NULL;
}

VentureAutomationHandlerRegistry *
venture_automation_handler_registry_new(void)
{
	return g_object_new(VENTURE_TYPE_AUTOMATION_HANDLER_REGISTRY, NULL);
}

gboolean
venture_automation_name_is_valid(const gchar *name)
{
	gsize i;

	if ((NULL == name) || !g_ascii_islower(name[0]))
		return FALSE;

	for (i = 1; '\0' != name[i]; i++)
	{
		if (i >= VENTURE_AUTOMATION_NAME_MAX)
			return FALSE;

		if (!g_ascii_islower(name[i]) && !g_ascii_isdigit(name[i]) &&
		    ('_' != name[i]))
			return FALSE;
	}

	return TRUE;
}

/*
 * Rebuilds one of the cached name arrays. The old array is kept (see
 * name_lists above), so a pointer handed out earlier stays readable.
 */
static gchar **
registry_snapshot(
	VentureAutomationHandlerRegistry	*self,
	GPtrArray				*entries,
	gboolean				 handlers
){
	gchar **names;
	guint i;

	names = g_new0(gchar *, entries->len + 1);

	for (i = 0; i < entries->len; i++)
	{
		names[i] = g_strdup(handlers
			? ((HandlerEntry *)g_ptr_array_index(entries, i))->name
			: ((EventEntry *)g_ptr_array_index(entries, i))->name);
	}

	g_ptr_array_add(self->name_lists, names);

	return names;
}

gboolean
venture_automation_handler_registry_add(
	VentureAutomationHandlerRegistry	 *self,
	const gchar				 *name,
	const gchar				 *description,
	VentureAutomationHandlerFunc		  func,
	gpointer				  user_data,
	GDestroyNotify				  destroy,
	GError					**error
){
	HandlerEntry *entry;

	g_return_val_if_fail(VENTURE_IS_AUTOMATION_HANDLER_REGISTRY(self), FALSE);
	g_return_val_if_fail(NULL != func, FALSE);

	if (!venture_automation_name_is_valid(name))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "\"%s\" is not a usable automation handler name: it "
		            "must be a lower-case letter followed by lower-case "
		            "letters, digits and underscores, at most %d bytes",
		            (NULL != name) ? name : "", VENTURE_AUTOMATION_NAME_MAX);
		return FALSE;
	}

	if (g_hash_table_contains(self->by_name, name))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_ALREADY_EXISTS,
		            "An automation handler called \"%s\" is already "
		            "registered", name);
		return FALSE;
	}

	entry = g_new0(HandlerEntry, 1);
	entry->name = g_strdup(name);
	entry->description = g_strdup(description);
	entry->func = func;
	entry->user_data = user_data;
	entry->destroy = destroy;

	g_ptr_array_add(self->handlers, entry);
	g_hash_table_insert(self->by_name, entry->name, entry);

	/* The next get_names() builds a fresh array. */
	self->handler_names = NULL;

	return TRUE;
}

gboolean
venture_automation_handler_registry_remove(
	VentureAutomationHandlerRegistry	*self,
	const gchar				*name
){
	HandlerEntry *entry;

	g_return_val_if_fail(VENTURE_IS_AUTOMATION_HANDLER_REGISTRY(self), FALSE);

	entry = (NULL != name) ? g_hash_table_lookup(self->by_name, name) : NULL;

	if (NULL == entry)
		return FALSE;

	g_hash_table_remove(self->by_name, name);
	g_ptr_array_remove(self->handlers, entry);

	/* A name list handed out earlier stays readable (name_lists); the
	 * next get_names() leaves this one out. */
	self->handler_names = NULL;

	return TRUE;
}

gboolean
venture_automation_handler_registry_has(
	VentureAutomationHandlerRegistry	*self,
	const gchar				*name
){
	g_return_val_if_fail(VENTURE_IS_AUTOMATION_HANDLER_REGISTRY(self), FALSE);

	return (NULL != name) && g_hash_table_contains(self->by_name, name);
}

const gchar *const *
venture_automation_handler_registry_get_names(
	VentureAutomationHandlerRegistry *self
){
	g_return_val_if_fail(VENTURE_IS_AUTOMATION_HANDLER_REGISTRY(self), NULL);

	if (NULL == self->handler_names)
		self->handler_names = registry_snapshot(self, self->handlers, TRUE);

	return (const gchar *const *)self->handler_names;
}

const gchar *
venture_automation_handler_registry_get_description(
	VentureAutomationHandlerRegistry	*self,
	const gchar				*name
){
	HandlerEntry *entry;

	g_return_val_if_fail(VENTURE_IS_AUTOMATION_HANDLER_REGISTRY(self), NULL);

	if (NULL == name)
		return NULL;

	entry = g_hash_table_lookup(self->by_name, name);

	return (NULL != entry) ? entry->description : NULL;
}

gboolean
venture_automation_handler_registry_call(
	VentureAutomationHandlerRegistry	 *self,
	VentureContext				 *context,
	const gchar				 *name,
	GVariant				 *params,
	GVariant				**result,
	GError					**error
){
	g_autoptr(GError) local_error = NULL;
	HandlerEntry *entry;

	g_return_val_if_fail(VENTURE_IS_AUTOMATION_HANDLER_REGISTRY(self), FALSE);
	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), FALSE);

	entry = (NULL != name) ? g_hash_table_lookup(self->by_name, name) : NULL;

	if (NULL == entry)
	{
		g_autofree gchar *known = NULL;

		known = g_strjoinv(", ", (gchar **)
			venture_automation_handler_registry_get_names(self));

		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_AUTOMATION,
		            "There is no automation handler called \"%s\". "
		            "Available handlers: %s",
		            (NULL != name) ? name : "", known);
		return FALSE;
	}

	if (entry->func(context, entry->name, params, result, entry->user_data,
	                &local_error))
		return TRUE;

	if (NULL == local_error)
	{
		/* The handler logged its own reason, as every built-in does. */
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_AUTOMATION,
		            "The \"%s\" handler failed; see the log for why",
		            entry->name);
		return FALSE;
	}

	g_propagate_error(error, g_steal_pointer(&local_error));

	return FALSE;
}

gboolean
venture_automation_handler_registry_add_event(
	VentureAutomationHandlerRegistry	 *self,
	const gchar				 *name,
	const gchar				 *description,
	GError					**error
){
	EventEntry *entry;

	g_return_val_if_fail(VENTURE_IS_AUTOMATION_HANDLER_REGISTRY(self), FALSE);

	if (!venture_automation_name_is_valid(name))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "\"%s\" is not a usable automation event name: it "
		            "must be a lower-case letter followed by lower-case "
		            "letters, digits and underscores, at most %d bytes",
		            (NULL != name) ? name : "", VENTURE_AUTOMATION_NAME_MAX);
		return FALSE;
	}

	if (venture_automation_handler_registry_has_event(self, name))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_ALREADY_EXISTS,
		            "An automation event called \"%s\" is already "
		            "registered", name);
		return FALSE;
	}

	entry = g_new0(EventEntry, 1);
	entry->name = g_strdup(name);
	entry->description = g_strdup(description);

	g_ptr_array_add(self->events, entry);
	self->event_names = NULL;

	return TRUE;
}

gboolean
venture_automation_handler_registry_has_event(
	VentureAutomationHandlerRegistry	*self,
	const gchar				*name
){
	guint i;

	g_return_val_if_fail(VENTURE_IS_AUTOMATION_HANDLER_REGISTRY(self), FALSE);

	if (NULL == name)
		return FALSE;

	/* A handful of events; a walk is cheaper than a second table. */
	for (i = 0; i < self->events->len; i++)
	{
		EventEntry *entry;

		entry = g_ptr_array_index(self->events, i);

		if (0 == strcmp(entry->name, name))
			return TRUE;
	}

	return FALSE;
}

const gchar *const *
venture_automation_handler_registry_get_events(
	VentureAutomationHandlerRegistry *self
){
	g_return_val_if_fail(VENTURE_IS_AUTOMATION_HANDLER_REGISTRY(self), NULL);

	if (NULL == self->event_names)
		self->event_names = registry_snapshot(self, self->events, FALSE);

	return (const gchar *const *)self->event_names;
}

const gchar *
venture_automation_argument(
	GVariant	*params,
	gsize		 index
){
	g_autoptr(GVariant) child = NULL;

	if (NULL == params)
		return NULL;

	if (g_variant_is_of_type(params, G_VARIANT_TYPE_STRING))
		return (0 == index) ? g_variant_get_string(params, NULL) : NULL;

	if (!g_variant_is_of_type(params, G_VARIANT_TYPE_TUPLE))
		return NULL;

	if (index >= g_variant_n_children(params))
		return NULL;

	child = g_variant_get_child_value(params, index);

	if (!g_variant_is_of_type(child, G_VARIANT_TYPE_STRING))
		return NULL;

	/* The tuple owns the string for as long as @params lives, which
	 * outlasts the handler call. */
	return g_variant_get_string(child, NULL);
}

GVariant *
venture_automation_result_new(
	gint64		 count,
	const gchar	*summary,
	const gchar	*detail
){
	GVariantBuilder builder;

	g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&builder, "{sv}", "count",
	                      g_variant_new_int64(count));
	g_variant_builder_add(&builder, "{sv}", "summary",
	                      g_variant_new_string((NULL != summary) ? summary : ""));
	g_variant_builder_add(&builder, "{sv}", "detail",
	                      g_variant_new_string((NULL != detail) ? detail : ""));

	return g_variant_builder_end(&builder);
}
