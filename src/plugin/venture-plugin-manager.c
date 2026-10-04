/*
 * venture-plugin-manager.c - Loading plugins and declarative types
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

#include <string.h>
#include <yaml-glib.h>

struct _VenturePluginRecord
{
	gchar			*name;
	gchar			*path;
	gchar			*runtime;
	gchar			*description;
	VenturePluginKind	 kind;
	GStrv			 provides;	/* the kinds, in manifest order */
};

/*
 * A load that failed, kept so that a required plugin's refusal can say why
 * rather than only that it is missing.
 */
typedef struct
{
	gchar	*name;
	gchar	*path;
	gchar	*message;
} VenturePluginFailure;

struct _VenturePluginManager
{
	GObject parent_instance;

	VentureContext			*context;
	VentureVentureTypeRegistry	*venture_types;

	GPtrArray			*runtimes;	/* VenturePluginRuntime */
	GPtrArray			*loaded;	/* VenturePluginRecord */
	GHashTable			*seen;		/* path -> NULL */
	GPtrArray			*deferred;	/* gchar *path, in order met */
	GPtrArray			*failures;	/* VenturePluginFailure */
	GHashTable			*exec;		/* name -> VentureExecSpec */

	guint				 load_depth;
	GPtrArray			*frames;	/* ManagerFrame, one per load in progress */
	gboolean			 retry_pending;
	gboolean			 retrying;
};

G_DEFINE_FINAL_TYPE(VenturePluginManager, venture_plugin_manager, G_TYPE_OBJECT)

static void manager_frame_free(gpointer data);

typedef enum
{
	MANAGER_LOADED,
	MANAGER_FAILED,
	MANAGER_DEFERRED
} ManagerOutcome;

/* ==========================================================================
 * Records
 * ========================================================================== */

static VenturePluginRecord *
venture_plugin_record_new(
	const gchar	*name,
	const gchar	*path,
	const gchar	*runtime
){
	VenturePluginRecord *record;

	record = g_new0(VenturePluginRecord, 1);
	record->name = g_strdup(name);
	record->path = g_strdup(path);
	record->runtime = g_strdup(runtime);
	record->kind = VENTURE_PLUGIN_KIND_OTHER;

	return record;
}

static void
venture_plugin_record_free(gpointer data)
{
	VenturePluginRecord *record;

	record = data;

	g_free(record->name);
	g_free(record->path);
	g_free(record->runtime);
	g_free(record->description);
	g_strfreev(record->provides);
	g_free(record);
}

const gchar *
venture_plugin_record_get_name(VenturePluginRecord *self)
{
	g_return_val_if_fail(NULL != self, NULL);

	return self->name;
}

const gchar *
venture_plugin_record_get_path(VenturePluginRecord *self)
{
	g_return_val_if_fail(NULL != self, NULL);

	return self->path;
}

const gchar *
venture_plugin_record_get_runtime(VenturePluginRecord *self)
{
	g_return_val_if_fail(NULL != self, NULL);

	return self->runtime;
}

VenturePluginKind
venture_plugin_record_get_kind(VenturePluginRecord *self)
{
	g_return_val_if_fail(NULL != self, VENTURE_PLUGIN_KIND_OTHER);

	return self->kind;
}

void
venture_plugin_record_set_kind(
	VenturePluginRecord	*self,
	VenturePluginKind	 kind
){
	g_return_if_fail(NULL != self);

	self->kind = kind;
}

const gchar *
venture_plugin_record_get_description(VenturePluginRecord *self)
{
	g_return_val_if_fail(NULL != self, NULL);

	return self->description;
}

void
venture_plugin_record_set_description(
	VenturePluginRecord	*self,
	const gchar		*description
){
	g_return_if_fail(NULL != self);

	g_free(self->description);
	self->description = g_strdup(description);
}

static void
venture_plugin_failure_free(gpointer data)
{
	VenturePluginFailure *failure;

	failure = data;

	g_free(failure->name);
	g_free(failure->path);
	g_free(failure->message);
	g_free(failure);
}

/* ==========================================================================
 * The object
 * ========================================================================== */

static void
venture_plugin_manager_finalize(GObject *object)
{
	VenturePluginManager *self;

	self = VENTURE_PLUGIN_MANAGER(object);

	g_clear_object(&self->context);
	g_clear_object(&self->venture_types);
	g_clear_pointer(&self->runtimes, g_ptr_array_unref);
	g_clear_pointer(&self->loaded, g_ptr_array_unref);
	g_clear_pointer(&self->seen, g_hash_table_unref);
	g_clear_pointer(&self->deferred, g_ptr_array_unref);
	g_clear_pointer(&self->failures, g_ptr_array_unref);
	g_clear_pointer(&self->exec, g_hash_table_unref);
	g_clear_pointer(&self->frames, g_ptr_array_unref);

	G_OBJECT_CLASS(venture_plugin_manager_parent_class)->finalize(object);
}

enum
{
	SIGNAL_CONFIG_CHANGED,
	N_SIGNALS
};

static guint venture_plugin_manager_signals[N_SIGNALS];

static void
venture_plugin_manager_class_init(VenturePluginManagerClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = venture_plugin_manager_finalize;

	/**
	 * VenturePluginManager::config-changed:
	 * @self: the manager
	 * @plugin_name: whose configuration changed
	 *
	 * Raised after a plugin's configuration is stored. A plugin that
	 * connects here and re-reads its configuration follows the settings
	 * page live, without a restart -- which is the entire point of
	 * configuration being a record rather than a file read once.
	 */
	venture_plugin_manager_signals[SIGNAL_CONFIG_CHANGED] =
		g_signal_new("config-changed",
		             VENTURE_TYPE_PLUGIN_MANAGER,
		             G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		             G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
venture_plugin_manager_init(VenturePluginManager *self)
{
	self->runtimes = g_ptr_array_new_with_free_func(g_object_unref);
	self->loaded = g_ptr_array_new_with_free_func(venture_plugin_record_free);
	self->seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	self->deferred = g_ptr_array_new_with_free_func(g_free);
	self->failures = g_ptr_array_new_with_free_func(
		venture_plugin_failure_free);
	self->exec = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                   (GDestroyNotify)venture_exec_spec_unref);
	self->frames = g_ptr_array_new_with_free_func(manager_frame_free);
}

VenturePluginManager *
venture_plugin_manager_new(VentureContext *context)
{
	VenturePluginManager *self;
	VenturePluginRuntime *builtins[4];
	guint i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	self = g_object_new(VENTURE_TYPE_PLUGIN_MANAGER, NULL);
	self->context = g_object_ref(context);

	/*
	 * Types are loaded into the context's registry, not a private one.
	 * Everything that consumes them -- the API, the web UI, validation --
	 * reads the context, so a second registry here would load the files
	 * and then have nobody look at them.
	 */
	self->venture_types = g_object_ref(
		venture_context_get_venture_types(context));

	/* The built-ins, in the order a runtime listing shows them. They
	 * claim distinct extensions, so none of these can be refused. */
	builtins[0] = venture_plugin_runtime_native_new();
	builtins[1] = venture_plugin_runtime_crispy_new();
	builtins[2] = venture_plugin_runtime_declarative_new();
	builtins[3] = venture_plugin_runtime_exec_new();

	for (i = 0; i < G_N_ELEMENTS(builtins); i++)
	{
		venture_plugin_manager_add_runtime(self, builtins[i], NULL);
		g_object_unref(builtins[i]);
	}

	return self;
}

VentureVentureTypeRegistry *
venture_plugin_manager_get_venture_types(VenturePluginManager *self)
{
	g_return_val_if_fail(VENTURE_IS_PLUGIN_MANAGER(self), NULL);

	return self->venture_types;
}

VentureContext *
venture_plugin_manager_get_context(VenturePluginManager *self)
{
	g_return_val_if_fail(VENTURE_IS_PLUGIN_MANAGER(self), NULL);

	return self->context;
}

guint
venture_plugin_manager_get_count(VenturePluginManager *self)
{
	g_return_val_if_fail(VENTURE_IS_PLUGIN_MANAGER(self), 0);

	return self->loaded->len;
}

/* ==========================================================================
 * Runtimes
 * ========================================================================== */

VenturePluginRuntime *
venture_plugin_manager_lookup_runtime(
	VenturePluginManager	*self,
	const gchar		*name
){
	guint i;

	g_return_val_if_fail(VENTURE_IS_PLUGIN_MANAGER(self), NULL);

	if (NULL == name)
		return NULL;

	for (i = 0; i < self->runtimes->len; i++)
	{
		VenturePluginRuntime *runtime;

		runtime = g_ptr_array_index(self->runtimes, i);

		if (0 == g_strcmp0(venture_plugin_runtime_get_name(runtime), name))
			return runtime;
	}

	return NULL;
}

/*
 * The runtime whose extension the path ends with, longest extension first,
 * or NULL. Extensions are unique across runtimes, so there is no tie.
 */
static VenturePluginRuntime *
manager_runtime_for_path(
	VenturePluginManager	*self,
	const gchar		*path
){
	VenturePluginRuntime *best;
	gsize best_length;
	gsize path_length;
	guint i;

	best = NULL;
	best_length = 0;
	path_length = strlen(path);

	for (i = 0; i < self->runtimes->len; i++)
	{
		VenturePluginRuntime *runtime;
		const gchar *const *extensions;
		gsize j;

		runtime = g_ptr_array_index(self->runtimes, i);
		extensions = venture_plugin_runtime_get_extensions(runtime);

		for (j = 0; NULL != extensions[j]; j++)
		{
			gsize length;

			length = strlen(extensions[j]);

			/* A file called exactly ".so" has no name to load. */
			if ((length < path_length) && (length > best_length) &&
			    g_str_has_suffix(path, extensions[j]))
			{
				best = runtime;
				best_length = length;
			}
		}
	}

	return best;
}

static ManagerOutcome manager_load(VenturePluginManager *self,
                                   const gchar *path, GError **error);

/*
 * Gives every deferred file another chance, now that a runtime has been
 * added. Runs only between loads, never inside one: a plugin that registers
 * a runtime does so from inside its own registration, and loading other
 * plugins from there would interleave two registrations.
 *
 * Deferred files are retried in the order they were met, which is sorted
 * order within each directory, so the outcome does not depend on hash
 * order or timing. A load that adds yet another runtime asks for one more
 * round rather than recursing.
 */
static void
manager_retry_deferred(VenturePluginManager *self)
{
	if (self->retrying || (0 != self->load_depth))
		return;

	self->retrying = TRUE;

	while (self->retry_pending)
	{
		g_autoptr(GPtrArray) pending = NULL;
		guint i;

		self->retry_pending = FALSE;
		pending = g_steal_pointer(&self->deferred);
		self->deferred = g_ptr_array_new_with_free_func(g_free);

		for (i = 0; i < pending->len; i++)
		{
			g_autoptr(GError) load_error = NULL;
			const gchar *path;

			path = g_ptr_array_index(pending, i);

			/* A deferred file that still has no runtime puts
			 * itself back on the list. */
			if (MANAGER_FAILED == manager_load(self, path, &load_error))
				g_warning("Skipping plugin: %s", load_error->message);
		}
	}

	self->retrying = FALSE;
}

gboolean
venture_plugin_manager_add_runtime(
	VenturePluginManager	 *self,
	VenturePluginRuntime	 *runtime,
	GError			**error
){
	const gchar *const *extensions;
	const gchar *name;
	gsize i;

	g_return_val_if_fail(VENTURE_IS_PLUGIN_MANAGER(self), FALSE);
	g_return_val_if_fail(VENTURE_IS_PLUGIN_RUNTIME(runtime), FALSE);

	name = venture_plugin_runtime_get_name(runtime);

	if ((NULL == name) || ('\0' == name[0]))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "A plugin runtime needs a name");
		return FALSE;
	}

	if (NULL != venture_plugin_manager_lookup_runtime(self, name))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_ALREADY_EXISTS,
		            "A plugin runtime called \"%s\" is already registered",
		            name);
		return FALSE;
	}

	/*
	 * An extension belongs to one runtime. Letting a second claim ".so"
	 * would make which one loads a library depend on registration order,
	 * which is plugin load order, which is file names.
	 */
	extensions = venture_plugin_runtime_get_extensions(runtime);

	for (i = 0; NULL != extensions[i]; i++)
	{
		guint j;

		if (('.' != extensions[i][0]) || ('\0' == extensions[i][1]) ||
		    (0 == g_strcmp0(extensions[i], VENTURE_PLUGIN_MANIFEST_SUFFIX)))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "The runtime \"%s\" cannot claim \"%s\": an extension "
			            "starts with '.', and %s is reserved for manifests",
			            name, extensions[i], VENTURE_PLUGIN_MANIFEST_SUFFIX);
			return FALSE;
		}

		for (j = 0; j < self->runtimes->len; j++)
		{
			VenturePluginRuntime *other;

			other = g_ptr_array_index(self->runtimes, j);

			if (g_strv_contains(venture_plugin_runtime_get_extensions(other),
			                    extensions[i]))
			{
				g_set_error(error, VENTURE_ERROR,
				            VENTURE_ERROR_ALREADY_EXISTS,
				            "The runtime \"%s\" cannot claim \"%s\": \"%s\" "
				            "already loads it", name, extensions[i],
				            venture_plugin_runtime_get_name(other));
				return FALSE;
			}
		}
	}

	g_ptr_array_add(self->runtimes, g_object_ref(runtime));

	if (self->deferred->len > 0)
	{
		self->retry_pending = TRUE;
		manager_retry_deferred(self);
	}

	return TRUE;
}

gchar **
venture_plugin_manager_list_runtimes(VenturePluginManager *self)
{
	GPtrArray *names;
	guint i;

	g_return_val_if_fail(VENTURE_IS_PLUGIN_MANAGER(self), NULL);

	names = g_ptr_array_new();

	for (i = 0; i < self->runtimes->len; i++)
	{
		g_ptr_array_add(names, g_strdup(venture_plugin_runtime_get_name(
			g_ptr_array_index(self->runtimes, i))));
	}

	g_ptr_array_add(names, NULL);

	return (gchar **)g_ptr_array_free(names, FALSE);
}

gchar **
venture_plugin_manager_list_deferred(VenturePluginManager *self)
{
	GPtrArray *paths;
	guint i;

	g_return_val_if_fail(VENTURE_IS_PLUGIN_MANAGER(self), NULL);

	paths = g_ptr_array_new();

	for (i = 0; i < self->deferred->len; i++)
		g_ptr_array_add(paths, g_strdup(g_ptr_array_index(self->deferred, i)));

	g_ptr_array_add(paths, NULL);

	return (gchar **)g_ptr_array_free(paths, FALSE);
}

/* ==========================================================================
 * Taking back a failed load
 * ==========================================================================
 *
 * A plugin's register function (native or crispy) and its manifest's
 * provides write into registries the manager does not own. When the load
 * then fails -- the function returns FALSE after registering a fee model,
 * a provides entry is refused after the program was registered -- the
 * plugin is reported as not loaded, and nothing it registered may stay
 * behind for a scan, a rule or a source to find.
 *
 * Each load opens a frame: the names every rollback-able registry holds
 * before the load. A failed load removes every name that appeared since;
 * a successful one inside another (a plugin registering a runtime makes
 * held plugins load at once, inside it) hands its names to the frame
 * around it, so an outer failure does not take back an inner success.
 *
 * Rolled back: data source providers, fee models, export formats,
 * arbitrage strategies, automation handlers, reports, posting rules,
 * provides kinds, plugin runtimes and web extensions. Not: record types
 * and modules (a GType cannot be unregistered, and the module registry is
 * written bottom-up for the process), actions, save validators, ledger
 * source types, feeds hooks, automation events, venture types and the
 * registries that hang off a module's service (bank feeds, commerce
 * connectors, reconciliation matchers, tax filing adapters). A plugin
 * that registers any of those should do so last, after everything that
 * can fail. docs/plugins.org "When a load fails" says the same.
 */

typedef gchar **(*ManagerNamesFunc) (VenturePluginManager *self);
typedef void (*ManagerRemoveFunc) (VenturePluginManager *self, const gchar *name);

typedef struct
{
	ManagerNamesFunc	names;
	ManagerRemoveFunc	remove;
} ManagerRegistry;

static gchar **
manager_names_from(GPtrArray *names)
{
	g_ptr_array_add(names, NULL);

	return (gchar **)g_ptr_array_free(names, FALSE);
}

static gchar **
manager_provider_names(VenturePluginManager *self)
{
	g_autoptr(GPtrArray) providers = NULL;
	GPtrArray *names;
	guint i;

	providers = venture_data_source_provider_registry_list(
		venture_context_get_data_source_providers(self->context));
	names = g_ptr_array_new();

	for (i = 0; i < providers->len; i++)
		g_ptr_array_add(names, g_strdup(venture_data_source_provider_get_name(
			g_ptr_array_index(providers, i))));

	return manager_names_from(names);
}

static void
manager_provider_remove(VenturePluginManager *self, const gchar *name)
{
	venture_data_source_provider_registry_remove(
		venture_context_get_data_source_providers(self->context), name);
}

static gchar **
manager_fee_model_names(VenturePluginManager *self)
{
	return venture_fee_model_registry_dup_names(venture_context_get_fee_models(self->context));
}

static void
manager_fee_model_remove(VenturePluginManager *self, const gchar *name)
{
	venture_fee_model_registry_remove(venture_context_get_fee_models(self->context), name);
}

static gchar **
manager_export_names(VenturePluginManager *self)
{
	return venture_export_format_registry_dup_names(
		venture_context_get_export_formats(self->context));
}

static void
manager_export_remove(VenturePluginManager *self, const gchar *name)
{
	venture_export_format_registry_remove(venture_context_get_export_formats(self->context), name);
}

static gchar **
manager_strategy_names(VenturePluginManager *self)
{
	return venture_arbitrage_strategy_registry_dup_names(
		venture_context_get_arbitrage_strategies(self->context));
}

static void
manager_strategy_remove(VenturePluginManager *self, const gchar *name)
{
	venture_arbitrage_strategy_registry_remove(
		venture_context_get_arbitrage_strategies(self->context), name);
}

static gchar **
manager_handler_names(VenturePluginManager *self)
{
	return g_strdupv((gchar **)venture_automation_handler_registry_get_names(
		venture_context_get_automation_handlers(self->context)));
}

static void
manager_handler_remove(VenturePluginManager *self, const gchar *name)
{
	venture_automation_handler_registry_remove(
		venture_context_get_automation_handlers(self->context), name);
}

static gchar **
manager_report_names(VenturePluginManager *self)
{
	g_autoptr(GPtrArray) reports = NULL;
	GPtrArray *names;
	guint i;

	reports = venture_report_registry_list(venture_context_get_report_registry(self->context));
	names = g_ptr_array_new();

	for (i = 0; i < reports->len; i++)
		g_ptr_array_add(names, g_strdup(venture_report_get_name(g_ptr_array_index(reports, i))));

	return manager_names_from(names);
}

static void
manager_report_remove(VenturePluginManager *self, const gchar *name)
{
	venture_report_registry_remove(venture_context_get_report_registry(self->context), name);
}

static VenturePostingRuleRegistry *
manager_posting_rules(VenturePluginManager *self)
{
	return venture_posting_service_get_rules(venture_database_get_posting_service(
		venture_context_get_database(self->context)));
}

static gchar **
manager_rule_names(VenturePluginManager *self)
{
	g_autoptr(GPtrArray) rules = NULL;
	GPtrArray *names;
	guint i;

	rules = venture_posting_rule_registry_list(manager_posting_rules(self));
	names = g_ptr_array_new();

	for (i = 0; i < rules->len; i++)
		g_ptr_array_add(names, g_strdup(venture_posting_rule_get_name(g_ptr_array_index(rules, i))));

	return manager_names_from(names);
}

static void
manager_rule_remove(VenturePluginManager *self, const gchar *name)
{
	venture_posting_rule_registry_remove(manager_posting_rules(self), name);
}

static gchar **
manager_kind_names(VenturePluginManager *self)
{
	return venture_plugin_provides_registry_list(venture_context_get_plugin_provides(self->context));
}

static void
manager_kind_remove(VenturePluginManager *self, const gchar *name)
{
	venture_plugin_provides_registry_remove(venture_context_get_plugin_provides(self->context), name);
}

static void
manager_runtime_remove(VenturePluginManager *self, const gchar *name)
{
	guint i;

	for (i = 0; i < self->runtimes->len; i++)
	{
		if (0 == g_strcmp0(venture_plugin_runtime_get_name(g_ptr_array_index(self->runtimes, i)),
		                   name))
		{
			g_ptr_array_remove_index(self->runtimes, i);
			return;
		}
	}
}

static const ManagerRegistry manager_registries[] = {
	{ manager_provider_names, manager_provider_remove },
	{ manager_fee_model_names, manager_fee_model_remove },
	{ manager_export_names, manager_export_remove },
	{ manager_strategy_names, manager_strategy_remove },
	{ manager_handler_names, manager_handler_remove },
	{ manager_report_names, manager_report_remove },
	{ manager_rule_names, manager_rule_remove },
	{ manager_kind_names, manager_kind_remove },
	{ venture_plugin_manager_list_runtimes, manager_runtime_remove }
};

typedef struct
{
	GHashTable	*before[G_N_ELEMENTS(manager_registries)];	/* name set */
	guint		 web_extensions;	/* how many there were */
	GArray		*kept;			/* guint start, end pairs: a nested success's */
} ManagerFrame;

static void
manager_frame_free(gpointer data)
{
	ManagerFrame *frame = data;
	guint i;

	for (i = 0; i < G_N_ELEMENTS(manager_registries); i++)
		g_clear_pointer(&frame->before[i], g_hash_table_unref);

	g_clear_pointer(&frame->kept, g_array_unref);
	g_free(frame);
}

/* Opens a frame over every registry as it is now. */
static void
manager_frame_begin(VenturePluginManager *self)
{
	ManagerFrame *frame;
	guint i;

	frame = g_new0(ManagerFrame, 1);

	for (i = 0; i < G_N_ELEMENTS(manager_registries); i++)
	{
		g_auto(GStrv) names = manager_registries[i].names(self);
		guint j;

		frame->before[i] = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

		for (j = 0; (NULL != names) && (NULL != names[j]); j++)
			g_hash_table_add(frame->before[i], g_strdup(names[j]));
	}

	frame->web_extensions = venture_context_count_web_extensions(self->context);
	frame->kept = g_array_new(FALSE, FALSE, sizeof(guint));
	g_ptr_array_add(self->frames, frame);
}

/* Closes the innermost frame. A failed load takes back every name that
 * appeared since it opened; a successful one hands those names to the
 * frame around it, which must not take them back. */
static void
manager_frame_end(
	VenturePluginManager	*self,
	gboolean		 loaded
){
	ManagerFrame *frame;
	ManagerFrame *outer;
	guint i;

	g_return_if_fail(self->frames->len > 0);

	frame = g_ptr_array_index(self->frames, self->frames->len - 1);
	outer = (self->frames->len > 1) ? g_ptr_array_index(self->frames, self->frames->len - 2) : NULL;

	for (i = 0; i < G_N_ELEMENTS(manager_registries); i++)
	{
		g_auto(GStrv) names = manager_registries[i].names(self);
		guint j;

		for (j = 0; (NULL != names) && (NULL != names[j]); j++)
		{
			if (g_hash_table_contains(frame->before[i], names[j]))
				continue;

			if (!loaded)
			{
				g_debug("venture_plugin_manager: taking back %s, registered by a "
				        "plugin that did not load", names[j]);
				manager_registries[i].remove(self, names[j]);
			}
			else if (NULL != outer)
				g_hash_table_add(outer->before[i], g_strdup(names[j]));
		}
	}

	/*
	 * Web extensions have only their position. Everything added since the
	 * frame opened goes, newest first so the positions below stay put,
	 * except a nested success's run of them. A frame only ever appends,
	 * and a nested failure only removes past its own start, so a kept run
	 * recorded earlier still names the same extensions.
	 */
	if (!loaded)
	{
		guint index = venture_context_count_web_extensions(self->context);

		while (index-- > frame->web_extensions)
		{
			gboolean keep = FALSE;
			guint k;

			for (k = 0; !keep && (k + 1 < frame->kept->len); k += 2)
				keep = (index >= g_array_index(frame->kept, guint, k)) &&
				       (index < g_array_index(frame->kept, guint, k + 1));

			if (!keep)
				venture_context_remove_web_extension(self->context, index);
		}
	}
	else if (NULL != outer)
	{
		guint end = venture_context_count_web_extensions(self->context);

		g_array_append_val(outer->kept, frame->web_extensions);
		g_array_append_val(outer->kept, end);
	}

	g_ptr_array_set_size(self->frames, self->frames->len - 1);
}

/* ==========================================================================
 * Loading
 * ========================================================================== */

static void
manager_note_failure(
	VenturePluginManager	*self,
	const gchar		*name,
	const gchar		*path,
	const gchar		*message
){
	VenturePluginFailure *failure;

	failure = g_new0(VenturePluginFailure, 1);
	failure->name = g_strdup(name);
	failure->path = g_strdup(path);
	failure->message = g_strdup(message);

	g_ptr_array_add(self->failures, failure);
}

static void
manager_defer(
	VenturePluginManager	*self,
	const gchar		*path
){
	guint i;

	for (i = 0; i < self->deferred->len; i++)
	{
		if (0 == g_strcmp0(g_ptr_array_index(self->deferred, i), path))
			return;
	}

	g_ptr_array_add(self->deferred, g_strdup(path));
}

/*
 * Hands one plugin to its runtime and, if it loads, to the kinds it
 * provides. The record is kept only when both succeed.
 */
static gboolean
manager_dispatch(
	VenturePluginManager	 *self,
	VenturePluginRuntime	 *runtime,
	const gchar		 *load_path,
	VenturePluginManifest	 *manifest,
	const gchar		 *name,
	const gchar		 *record_path,
	GError			**error
){
	VenturePluginRecord *record;
	g_autoptr(GError) local_error = NULL;
	gboolean ok;

	record = venture_plugin_record_new(name, record_path,
	                                   venture_plugin_runtime_get_name(runtime));

	self->load_depth++;
	manager_frame_begin(self);

	ok = venture_plugin_runtime_load(runtime, self, self->context, load_path,
	                                 manifest, record, &local_error);

	if (ok && (NULL != manifest))
	{
		ok = venture_plugin_provides_registry_dispatch(
			venture_context_get_plugin_provides(self->context), self,
			manifest, &local_error);

		/* An exec plugin registered its program before its provides
		 * were judged; a refused plugin must not leave one behind for
		 * a later lookup to run. */
		if (!ok)
			g_hash_table_remove(self->exec, name);
	}

	/* Everything a refused load registered goes, wherever it was in
	 * its register function or its manifest when it was refused. */
	manager_frame_end(self, ok);
	self->load_depth--;

	if (!ok)
	{
		if (NULL == local_error)
			local_error = g_error_new(VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
			                          "%s did not load, and its runtime gave "
			                          "no reason", record_path);

		manager_note_failure(self, name, record_path, local_error->message);
		g_propagate_error(error, g_steal_pointer(&local_error));
		venture_plugin_record_free(record);
		manager_retry_deferred(self);
		return FALSE;
	}

	if ((NULL == record->description) && (NULL != manifest))
		venture_plugin_record_set_description(record,
			venture_plugin_manifest_get_description(manifest));

	if ((NULL != manifest) &&
	    (NULL != venture_plugin_manifest_get_provides(manifest)))
	{
		JsonArray *provides;
		GPtrArray *kinds;
		guint i;

		provides = venture_plugin_manifest_get_provides(manifest);
		kinds = g_ptr_array_new();

		for (i = 0; i < json_array_get_length(provides); i++)
		{
			g_ptr_array_add(kinds, g_strdup(json_object_get_string_member(
				json_array_get_object_element(provides, i), "kind")));
		}

		g_ptr_array_add(kinds, NULL);
		record->provides = (GStrv)g_ptr_array_free(kinds, FALSE);
	}

	g_ptr_array_add(self->loaded, record);
	g_message("Loaded plugin %s", record_path);

	manager_retry_deferred(self);

	return TRUE;
}

static const gchar *const manager_manifest_keys[] = {
	"name", "runtime", "entry", "description", "protocol", "provides", NULL
};

static ManagerOutcome
manager_load_manifest(
	VenturePluginManager	 *self,
	const gchar		 *path,
	GError			**error
){
	g_autoptr(VenturePluginManifest) manifest = NULL;
	g_autoptr(GError) local_error = NULL;
	VenturePluginRuntime *runtime;
	const gchar *runtime_name;
	const gchar *name;
	guint i;

	manifest = venture_plugin_manifest_new_from_file(path, &local_error);

	if (NULL == manifest)
	{
		g_autofree gchar *basename = NULL;

		basename = g_path_get_basename(path);
		g_hash_table_add(self->seen, g_strdup(path));
		manager_note_failure(self, basename, path, local_error->message);
		g_propagate_error(error, g_steal_pointer(&local_error));
		return MANAGER_FAILED;
	}

	name = venture_plugin_manifest_get_name(manifest);
	runtime_name = venture_plugin_manifest_get_runtime(manifest);

	runtime = (NULL != runtime_name)
		? venture_plugin_manager_lookup_runtime(self, runtime_name)
		: manager_runtime_for_path(self,
		                           venture_plugin_manifest_get_entry(manifest));

	/* A runtime a plugin has not registered yet: held, and loaded the
	 * moment one is. Not seen, so the retry is not mistaken for a
	 * duplicate. */
	if (NULL == runtime)
	{
		manager_defer(self, path);

		if (NULL != runtime_name)
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
			            "%s asks for the runtime \"%s\", which is not "
			            "registered; it is held and loads if one is", path,
			            runtime_name);
		else
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
			            "%s names no runtime, and none loads its entry; it "
			            "is held and loads if one is", path);

		return MANAGER_DEFERRED;
	}

	runtime_name = venture_plugin_runtime_get_name(runtime);
	g_hash_table_add(self->seen, g_strdup(path));

	/*
	 * Unknown top-level keys are refused. The runtime's own section is
	 * the one allowed addition, so a later runtime brings its settings
	 * without this list knowing it.
	 */
	{
		JsonObjectIter iter;
		JsonNode *member;
		const gchar *key;

		json_object_iter_init(&iter, venture_plugin_manifest_get_object(manifest));

		while (json_object_iter_next(&iter, &key, &member))
		{
			if (g_strv_contains(manager_manifest_keys, key) ||
			    (0 == g_strcmp0(key, runtime_name)))
				continue;

			local_error = g_error_new(VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
			                          "%s: \"%s\" is not a manifest key", path,
			                          key);
			manager_note_failure(self, name, path, local_error->message);
			g_propagate_error(error, g_steal_pointer(&local_error));
			return MANAGER_FAILED;
		}
	}

	/* One name, one plugin: the name keys its configuration record and
	 * its program, and two plugins sharing one would share both. */
	for (i = 0; i < self->loaded->len; i++)
	{
		const VenturePluginRecord *record;

		record = g_ptr_array_index(self->loaded, i);

		if (0 == g_strcmp0(record->name, name))
		{
			local_error = g_error_new(VENTURE_ERROR, VENTURE_ERROR_ALREADY_EXISTS,
			                          "%s: a plugin named \"%s\" is already "
			                          "loaded from %s", path, name,
			                          record->path);
			manager_note_failure(self, name, path, local_error->message);
			g_propagate_error(error, g_steal_pointer(&local_error));
			return MANAGER_FAILED;
		}
	}

	return manager_dispatch(self, runtime,
	                        venture_plugin_manifest_get_entry(manifest),
	                        manifest, name, path, error)
		? MANAGER_LOADED : MANAGER_FAILED;
}

static ManagerOutcome
manager_load(
	VenturePluginManager	 *self,
	const gchar		 *path,
	GError			**error
){
	VenturePluginRuntime *runtime;
	g_autofree gchar *basename = NULL;

	/* Loading the same file twice would register its types twice, which
	 * the entity registry would then refuse; skipping is the right
	 * answer when two configured directories overlap. */
	if (g_hash_table_contains(self->seen, path))
		return MANAGER_LOADED;

	/* Before any extension: a manifest is also a .yaml, and the
	 * declarative runtime would take it for a venture type. */
	if (g_str_has_suffix(path, VENTURE_PLUGIN_MANIFEST_SUFFIX))
		return manager_load_manifest(self, path, error);

	runtime = manager_runtime_for_path(self, path);

	if (NULL == runtime)
	{
		manager_defer(self, path);
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
		            "%s is not a plugin any runtime loads: expected .so, .c, "
		            ".yaml, .yml or a %s manifest; it is held and loads if a "
		            "runtime for it is registered", path,
		            VENTURE_PLUGIN_MANIFEST_SUFFIX);
		return MANAGER_DEFERRED;
	}

	g_hash_table_add(self->seen, g_strdup(path));
	basename = g_path_get_basename(path);

	return manager_dispatch(self, runtime, path, NULL, basename, path, error)
		? MANAGER_LOADED : MANAGER_FAILED;
}

gboolean
venture_plugin_manager_load_file(
	VenturePluginManager	 *self,
	const gchar		 *path,
	GError			**error
){
	g_return_val_if_fail(VENTURE_IS_PLUGIN_MANAGER(self), FALSE);
	g_return_val_if_fail(NULL != path, FALSE);

	return MANAGER_LOADED == manager_load(self, path, error);
}

/*
 * Loads what one scanned path holds, and reports a failure the way every
 * directory does: one broken plugin must not stop the server, because the
 * operator still needs their data, and a warning they can act on beats a
 * refusal to start.
 */
static guint
manager_scan_one(
	VenturePluginManager	*self,
	const gchar		*path
){
	g_autoptr(GError) load_error = NULL;

	switch (manager_load(self, path, &load_error))
	{
	case MANAGER_LOADED:
		return 1;

	case MANAGER_FAILED:
		g_warning("Skipping plugin: %s", load_error->message);
		return 0;

	case MANAGER_DEFERRED:
	default:
		/* A README beside the plugins is deferred too, harmlessly:
		 * no runtime will ever claim it, and it says nothing. */
		g_debug("venture_plugin_manager: %s", load_error->message);
		return 0;
	}
}

/*
 * The names in a directory, sorted, so load order is deterministic and a
 * plugin that depends on another loading first can rely on naming to
 * arrange it.
 */
static GPtrArray *
manager_sorted_entries(const gchar *path)
{
	g_autoptr(GDir) directory = NULL;
	g_autoptr(GError) local_error = NULL;
	GPtrArray *entries;
	const gchar *entry;

	directory = g_dir_open(path, 0, &local_error);

	if (NULL == directory)
	{
		/* A configured directory that does not exist is normal on a
		 * fresh install, not a failure. */
		g_debug("venture_plugin_manager: %s", local_error->message);
		return NULL;
	}

	entries = g_ptr_array_new_with_free_func(g_free);

	while (NULL != (entry = g_dir_read_name(directory)))
		g_ptr_array_add(entries, g_strdup(entry));

	g_ptr_array_sort_values(entries, (GCompareFunc)g_strcmp0);

	return entries;
}

guint
venture_plugin_manager_load_directory(
	VenturePluginManager	 *self,
	const gchar		 *path,
	GError			**error
){
	g_autoptr(GPtrArray) entries = NULL;
	guint loaded;
	guint i;

	g_return_val_if_fail(VENTURE_IS_PLUGIN_MANAGER(self), 0);
	g_return_val_if_fail(NULL != path, 0);

	(void)error;

	entries = manager_sorted_entries(path);

	if (NULL == entries)
		return 0;

	loaded = 0;

	for (i = 0; i < entries->len; i++)
	{
		g_autofree gchar *full_path = NULL;
		const gchar *name;

		name = g_ptr_array_index(entries, i);
		full_path = g_build_filename(path, name, NULL);

		/*
		 * A subdirectory is a plugin's own directory -- a program and
		 * its manifest, a library and its data -- and only its
		 * manifests are read. Exactly one level: nothing below it is
		 * scanned, and nothing in it but a manifest is loaded, so a
		 * plugin's helper scripts are never mistaken for plugins.
		 */
		if (g_file_test(full_path, G_FILE_TEST_IS_DIR))
		{
			g_autoptr(GPtrArray) inner = NULL;
			guint j;

			inner = manager_sorted_entries(full_path);

			for (j = 0; (NULL != inner) && (j < inner->len); j++)
			{
				g_autofree gchar *manifest_path = NULL;
				const gchar *inner_name;

				inner_name = g_ptr_array_index(inner, j);

				if (!g_str_has_suffix(inner_name,
				                      VENTURE_PLUGIN_MANIFEST_SUFFIX))
					continue;

				manifest_path = g_build_filename(full_path, inner_name,
				                                 NULL);

				if (g_file_test(manifest_path, G_FILE_TEST_IS_REGULAR))
					loaded += manager_scan_one(self, manifest_path);
			}

			continue;
		}

		/* A name with no extension -- a Makefile, a LICENSE -- is not
		 * something any runtime could ever claim. */
		if ((NULL == strchr(name, '.')) ||
		    !g_file_test(full_path, G_FILE_TEST_IS_REGULAR))
			continue;

		loaded += manager_scan_one(self, full_path);
	}

	return loaded;
}

gboolean
venture_plugin_manager_load_configured(
	VenturePluginManager	 *self,
	GError			**error
){
	VentureConfig *config;
	g_auto(GStrv) paths = NULL;
	g_auto(GStrv) type_paths = NULL;
	g_auto(GStrv) required = NULL;
	gboolean enabled;
	gsize i;

	g_return_val_if_fail(VENTURE_IS_PLUGIN_MANAGER(self), FALSE);

	config = venture_context_get_config(self->context);
	g_object_get(config,
	             "plugins-paths", &paths,
	             "plugins-venture-type-paths", &type_paths,
	             "plugins-required", &required,
	             NULL);

	/* The module registry folds plugins.enabled in. */
	enabled = venture_context_module_enabled(self->context, "plugins");

	if (!enabled)
		return TRUE;

	/* The install prefix first, then anything configured, so a
	 * locally-configured plugin can override a packaged one by name. */
	venture_plugin_manager_load_directory(self, VENTURE_PLUGINDIR, NULL);

	/* The build tree, so a freshly-built plugin is picked up without
	 * installing it. */
	{
		const gchar *development_path;

		development_path = g_getenv("VENTURE_PLUGIN_PATH");

		if (NULL != development_path)
		{
			g_auto(GStrv) parts = NULL;

			parts = g_strsplit(development_path, ":", -1);

			for (i = 0; NULL != parts[i]; i++)
				venture_plugin_manager_load_directory(self, parts[i], NULL);
		}
	}

	for (i = 0; (NULL != paths) && (NULL != paths[i]); i++)
		venture_plugin_manager_load_directory(self, paths[i], NULL);

	/* The declarative venture types shipped with the install, then any
	 * configured directory. */
	{
		g_autofree gchar *builtin_types = NULL;

		builtin_types = g_build_filename(VENTURE_DATADIR, "venture-types",
		                                 NULL);
		venture_venture_type_registry_load_directory(self->venture_types,
		                                             builtin_types, NULL);
	}

	{
		const gchar *development_types;

		development_types = g_getenv("VENTURE_VENTURE_TYPE_PATH");

		if (NULL != development_types)
		{
			g_auto(GStrv) parts = NULL;

			/* Colon-separated, like every other *_PATH. */
			parts = g_strsplit(development_types, ":", -1);

			for (i = 0; NULL != parts[i]; i++)
			{
				venture_venture_type_registry_load_directory(
					self->venture_types, parts[i], NULL);
			}
		}
	}

	for (i = 0; (NULL != type_paths) && (NULL != type_paths[i]); i++)
	{
		venture_venture_type_registry_load_directory(self->venture_types,
		                                             type_paths[i], NULL);
	}

	/*
	 * A required plugin that did not load IS fatal. That is the whole
	 * point of the list: an install that depends on a plugin should not
	 * start half-configured and quietly behave differently. The refusal
	 * says why when the plugin was found and failed, because "did not
	 * load" alone sends the operator looking for a file that is there.
	 */
	for (i = 0; (NULL != required) && (NULL != required[i]); i++)
	{
		const gchar *reason;
		gboolean found;
		guint j;

		found = FALSE;
		reason = NULL;

		for (j = 0; j < self->loaded->len; j++)
		{
			const VenturePluginRecord *record;
			g_autofree gchar *basename = NULL;

			record = g_ptr_array_index(self->loaded, j);
			basename = g_path_get_basename(record->path);

			/* By name, by file name or by path: a manifest's file
			 * name is not its plugin's name, and an operator may
			 * write either. */
			if ((0 == g_strcmp0(record->name, required[i])) ||
			    (0 == g_strcmp0(record->path, required[i])) ||
			    (0 == g_strcmp0(basename, required[i])))
			{
				found = TRUE;
				break;
			}
		}

		if (found)
			continue;

		for (j = 0; j < self->failures->len; j++)
		{
			const VenturePluginFailure *failure;
			g_autofree gchar *basename = NULL;

			failure = g_ptr_array_index(self->failures, j);
			basename = g_path_get_basename(failure->path);

			if ((0 == g_strcmp0(failure->name, required[i])) ||
			    (0 == g_strcmp0(failure->path, required[i])) ||
			    (0 == g_strcmp0(basename, required[i])))
				reason = failure->message;
		}

		for (j = 0; (NULL == reason) && (j < self->deferred->len); j++)
		{
			const gchar *deferred;
			g_autofree gchar *basename = NULL;

			deferred = g_ptr_array_index(self->deferred, j);
			basename = g_path_get_basename(deferred);

			if ((0 == g_strcmp0(deferred, required[i])) ||
			    (0 == g_strcmp0(basename, required[i])))
				reason = "no runtime loads it";
		}

		if (NULL != reason)
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
			            "The required plugin \"%s\" did not load: %s. Fix "
			            "it, or remove it from plugins.required.",
			            required[i], reason);
		else
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
			            "The required plugin \"%s\" did not load. Fix it, "
			            "or remove it from plugins.required.", required[i]);

		return FALSE;
	}

	return TRUE;
}

JsonNode *
venture_plugin_manager_list(VenturePluginManager *self)
{
	g_autoptr(JsonBuilder) builder = NULL;
	guint i;

	g_return_val_if_fail(VENTURE_IS_PLUGIN_MANAGER(self), NULL);

	builder = json_builder_new();
	json_builder_begin_array(builder);

	for (i = 0; i < self->loaded->len; i++)
	{
		const VenturePluginRecord *record;

		record = g_ptr_array_index(self->loaded, i);

		json_builder_begin_object(builder);

		json_builder_set_member_name(builder, "name");
		json_builder_add_string_value(builder, record->name);

		json_builder_set_member_name(builder, "kind");
		json_builder_add_string_value(builder,
			venture_enum_to_nick(VENTURE_TYPE_PLUGIN_KIND,
			                     (gint)record->kind));

		json_builder_set_member_name(builder, "runtime");
		json_builder_add_string_value(builder, record->runtime);

		json_builder_set_member_name(builder, "path");
		json_builder_add_string_value(builder, record->path);

		if (NULL != record->description)
		{
			json_builder_set_member_name(builder, "description");
			json_builder_add_string_value(builder, record->description);
		}

		if (NULL != record->provides)
		{
			gsize j;

			json_builder_set_member_name(builder, "provides");
			json_builder_begin_array(builder);

			for (j = 0; NULL != record->provides[j]; j++)
				json_builder_add_string_value(builder, record->provides[j]);

			json_builder_end_array(builder);
		}

		json_builder_end_object(builder);
	}

	json_builder_end_array(builder);

	return json_builder_get_root(builder);
}

/* ==========================================================================
 * Exec plugins
 * ========================================================================== */

gboolean
venture_plugin_manager_add_exec(
	VenturePluginManager	 *self,
	VentureExecSpec		 *spec,
	GError			**error
){
	const gchar *name;

	g_return_val_if_fail(VENTURE_IS_PLUGIN_MANAGER(self), FALSE);
	g_return_val_if_fail(NULL != spec, FALSE);

	name = venture_exec_spec_get_name(spec);

	if (g_hash_table_contains(self->exec, name))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_ALREADY_EXISTS,
		            "An exec plugin named \"%s\" is already registered", name);
		return FALSE;
	}

	g_hash_table_insert(self->exec, g_strdup(name),
	                    venture_exec_spec_ref(spec));

	return TRUE;
}

VentureExecSpec *
venture_plugin_manager_lookup_exec(
	VenturePluginManager	*self,
	const gchar		*name
){
	g_return_val_if_fail(VENTURE_IS_PLUGIN_MANAGER(self), NULL);

	if (NULL == name)
		return NULL;

	return g_hash_table_lookup(self->exec, name);
}

JsonObject *
venture_plugin_manager_build_exec_request(
	VenturePluginManager	*self,
	const gchar		*name,
	const gchar		*command,
	JsonObject		*params
){
	g_autoptr(JsonNode) settings = NULL;
	JsonObject *request;

	g_return_val_if_fail(VENTURE_IS_PLUGIN_MANAGER(self), NULL);
	g_return_val_if_fail(NULL != name, NULL);

	request = json_object_new();
	json_object_set_string_member(request, "plugin", name);
	json_object_set_string_member(request, "command",
	                              (NULL != command) ? command : "run");

	/*
	 * The settings the /plugins page stores for it, read now so that an
	 * edit there reaches the very next run. They travel on standard input
	 * with everything else; an argv carrying them would show them to every
	 * user on the machine.
	 */
	settings = venture_plugin_manager_get_config(self, name);

	if ((NULL != settings) && JSON_NODE_HOLDS_OBJECT(settings))
		json_object_set_member(request, "settings",
		                       g_steal_pointer(&settings));
	else
		json_object_set_object_member(request, "settings", json_object_new());

	if (NULL != params)
		json_object_set_object_member(request, "params",
		                              json_object_ref(params));
	else
		json_object_set_object_member(request, "params", json_object_new());

	return request;
}

VentureExecResult *
venture_plugin_manager_run_exec(
	VenturePluginManager	 *self,
	const gchar		 *name,
	const gchar		 *command,
	JsonObject		 *params,
	JsonObject		 *secrets,
	GCancellable		 *cancellable,
	GError			**error
){
	g_autoptr(JsonObject) request = NULL;
	VentureExecSpec *spec;
	gboolean allow_exec;

	g_return_val_if_fail(VENTURE_IS_PLUGIN_MANAGER(self), NULL);
	g_return_val_if_fail(NULL != name, NULL);

	/* Asked again at every run, not only at load: switching it off on a
	 * running install must stop the programs now, not at the next
	 * restart. */
	g_object_get(venture_context_get_config(self->context),
	             "plugins-allow-exec", &allow_exec, NULL);

	if (!allow_exec)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
		            "The exec plugin %s cannot run: plugins.allow_exec is off",
		            name);
		return NULL;
	}

	spec = venture_plugin_manager_lookup_exec(self, name);

	if (NULL == spec)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "No exec plugin named \"%s\" is loaded", name);
		return NULL;
	}

	request = venture_plugin_manager_build_exec_request(self, name, command,
	                                                    params);

	return venture_exec_run(spec, request, secrets, cancellable, error);
}


/* ==========================================================================
 * Plugin configuration
 * ========================================================================== */

/*
 * Finds the configuration record for @plugin_name, if one exists.
 *
 * Returns: (transfer full) (nullable): the record, or %NULL
 */
static VentureEntity *
venture_plugin_manager_find_config(
	VenturePluginManager	*self,
	const gchar		*plugin_name
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) records = NULL;

	query = venture_query_new(VENTURE_TYPE_PLUGIN_CONFIG);

	if (!venture_query_add_filter_string(query, "name",
	                                     VENTURE_FILTER_OP_EQ, plugin_name,
	                                     NULL))
		return NULL;

	venture_query_set_limit(query, 1);

	records = venture_database_find(
		venture_context_get_database(self->context), query, NULL);

	if ((NULL == records) || (0 == records->len))
		return NULL;

	return g_object_ref(g_ptr_array_index(records, 0));
}

gchar *
venture_plugin_manager_get_config_text(
	VenturePluginManager	*self,
	const gchar		*plugin_name
){
	g_autoptr(VentureEntity) record = NULL;
	gchar *text;

	g_return_val_if_fail(VENTURE_IS_PLUGIN_MANAGER(self), NULL);
	g_return_val_if_fail(NULL != plugin_name, NULL);

	record = venture_plugin_manager_find_config(self, plugin_name);

	if (NULL == record)
		return NULL;

	g_object_get(record, "config", &text, NULL);

	return text;
}

JsonNode *
venture_plugin_manager_get_config(
	VenturePluginManager	*self,
	const gchar		*plugin_name
){
	g_autofree gchar *text = NULL;
	g_autoptr(YamlParser) parser = NULL;
	YamlNode *root;

	g_return_val_if_fail(VENTURE_IS_PLUGIN_MANAGER(self), NULL);

	text = venture_plugin_manager_get_config_text(self, plugin_name);

	if (venture_string_is_empty(text))
		return NULL;

	parser = yaml_parser_new();

	if (!yaml_parser_load_from_data(parser, text, -1, NULL))
		return NULL;

	root = yaml_parser_get_root(parser);

	return (NULL != root) ? yaml_node_to_json_node(root) : NULL;
}

gboolean
venture_plugin_manager_set_config(
	VenturePluginManager	 *self,
	const gchar		 *plugin_name,
	const gchar		 *yaml,
	const VentureActor	 *actor,
	GError			**error
){
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(GError) local_error = NULL;

	g_return_val_if_fail(VENTURE_IS_PLUGIN_MANAGER(self), FALSE);
	g_return_val_if_fail(NULL != plugin_name, FALSE);

	if (NULL == yaml)
		yaml = "";

	/*
	 * Refuse YAML that does not parse before anything is stored. The
	 * plugin re-reads on the change signal, and handing a running plugin
	 * settings it cannot parse turns a typo on the settings page into a
	 * runtime failure somewhere far from the typo.
	 */
	if ('\0' != *yaml)
	{
		g_autoptr(YamlParser) parser = NULL;

		parser = yaml_parser_new();

		if (!yaml_parser_load_from_data(parser, yaml, -1, &local_error))
		{
			g_set_error(error, VENTURE_ERROR,
			            VENTURE_ERROR_INVALID_ARGUMENT,
			            "That is not valid YAML: %s",
			            (NULL != local_error)
			                ? local_error->message : "unparseable");
			return FALSE;
		}
	}

	record = venture_plugin_manager_find_config(self, plugin_name);

	if (NULL == record)
	{
		record = VENTURE_ENTITY(venture_plugin_config_new());
		g_object_set(record, "name", plugin_name, NULL);
		venture_entity_set_organization_id(record,
			venture_context_get_default_organization_id(self->context));
	}

	g_object_set(record, "config", yaml, NULL);

	if (!venture_database_save(venture_context_get_database(self->context),
	                           record, actor, error))
		return FALSE;

	g_signal_emit(self,
	              venture_plugin_manager_signals[SIGNAL_CONFIG_CHANGED], 0,
	              plugin_name);

	return TRUE;
}

gchar *
venture_plugin_config_get_string(
	JsonNode	*config,
	const gchar	*key,
	const gchar	*fallback
){
	JsonObject *object;
	JsonNode *member;

	g_return_val_if_fail(NULL != key, NULL);

	if ((NULL == config) || !JSON_NODE_HOLDS_OBJECT(config))
		return g_strdup(fallback);

	object = json_node_get_object(config);

	if (!json_object_has_member(object, key))
		return g_strdup(fallback);

	member = json_object_get_member(object, key);

	if (!JSON_NODE_HOLDS_VALUE(member))
		return g_strdup(fallback);

	return g_strdup(json_node_get_string(member));
}

gdouble
venture_plugin_config_get_double(
	JsonNode	*config,
	const gchar	*key,
	gdouble		 fallback
){
	JsonObject *object;
	JsonNode *member;

	g_return_val_if_fail(NULL != key, fallback);

	if ((NULL == config) || !JSON_NODE_HOLDS_OBJECT(config))
		return fallback;

	object = json_node_get_object(config);

	if (!json_object_has_member(object, key))
		return fallback;

	member = json_object_get_member(object, key);

	if (!JSON_NODE_HOLDS_VALUE(member))
		return fallback;

	return json_node_get_double(member);
}
