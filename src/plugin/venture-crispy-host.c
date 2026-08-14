/*
 * venture-crispy-host.c - Compile-on-demand C, via crispy
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

#include <crispy.h>

struct _VentureCrispyHost
{
	GObject parent_instance;

	CrispyCompiler		*compiler;
	CrispyCacheProvider	*cache;
	gchar			*default_flags;

	/* source path -> GModule. Modules are never closed: a compiled
	 * object may have registered a GType, and unloading the code behind
	 * a live type is a crash waiting for the next instantiation. */
	GHashTable		*modules;
};

G_DEFINE_FINAL_TYPE(VentureCrispyHost, venture_crispy_host, G_TYPE_OBJECT)

static void
venture_crispy_host_finalize(GObject *object)
{
	VentureCrispyHost *self;

	self = VENTURE_CRISPY_HOST(object);

	g_clear_object(&self->compiler);
	g_clear_object(&self->cache);
	g_clear_pointer(&self->default_flags, g_free);
	g_clear_pointer(&self->modules, g_hash_table_unref);

	G_OBJECT_CLASS(venture_crispy_host_parent_class)->finalize(object);
}

static void
venture_crispy_host_class_init(VentureCrispyHostClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = venture_crispy_host_finalize;
}

static void
venture_crispy_host_init(VentureCrispyHost *self)
{
	/* The values are GModules that are deliberately never closed, so the
	 * table owns only its keys. */
	self->modules = g_hash_table_new_full(g_str_hash, g_str_equal,
	                                      g_free, NULL);
}

/*
 * Builds the compiler flags a VENTURE crispy source needs: the header
 * search path, plus the cflags of every library the public API exposes in
 * its signatures.
 *
 * The include path prefers the development tree when running from a build
 * directory, so a plugin can be edited and reloaded without installing
 * anything.
 */
static gchar *
venture_crispy_host_build_flags(void)
{
	g_autoptr(GString) flags = NULL;
	g_autofree gchar *pkg_flags = NULL;

	flags = g_string_new(NULL);

	/*
	 * Two include paths per prefix, not one. `<venture/venture.h>` resolves
	 * against the outer directory, but the headers it pulls in refer to
	 * each other as `"boxed/venture-money.h"` -- rooted at the venture/
	 * directory rather than relative to the including file. Without the
	 * inner path, every crispy plugin fails on the first nested include.
	 */
	if (g_file_test(VENTURE_DEV_INCLUDE_DIR, G_FILE_TEST_IS_DIR))
	{
		g_string_append_printf(flags, "-I%s -I%s/venture ",
		                       VENTURE_DEV_INCLUDE_DIR,
		                       VENTURE_DEV_INCLUDE_DIR);
	}

	/*
	 * The installed location, which is where install-headers actually
	 * puts them -- not a path derived from the data directory. Those two
	 * diverge the moment anyone builds with a prefix, and the symptom is
	 * a plugin that fails on a missing header naming a directory that
	 * does exist, just not the one being searched.
	 */
	g_string_append_printf(flags, "-I%s -I%s/venture ",
	                       VENTURE_INCLUDE_DIR, VENTURE_INCLUDE_DIR);

	/*
	 * The same feature defines and dependency include paths a native
	 * plugin is built with, baked in by config.mk. VENTURE_SERVER_BUILD
	 * is the one that matters most: without it the umbrella header hides
	 * the reporting engine and the database, which is nearly everything a
	 * plugin is written to reach.
	 *
	 * The include paths in here point into the source tree, so they are
	 * live in a development checkout and simply absent on an installed
	 * system, where the next block covers the same headers.
	 */
	g_string_append(flags, VENTURE_CRISPY_PLUGIN_CFLAGS);
	g_string_append_c(flags, ' ');

	/*
	 * The vendored dependencies' installed headers. VENTURE's public
	 * headers include <orm.h> and <htmx-glib.h>, so without these a
	 * plugin fails on a header from a library it never mentioned.
	 *
	 * Each dependency keeps its own directory because several of them
	 * have a src/core/ and a src/types/, and one flat include path would
	 * resolve the wrong one.
	 */
	if (g_file_test(VENTURE_DEPS_INCLUDE_DIR, G_FILE_TEST_IS_DIR))
	{
		static const gchar *const dependencies[] = {
			"yaml-glib", "htmx-glib", "ai-glib", "crispy", "podomation",
			"orm-glib", NULL
		};
		gsize i;

		for (i = 0; NULL != dependencies[i]; i++)
		{
			g_string_append_printf(flags, "-I%s/%s ",
			                       VENTURE_DEPS_INCLUDE_DIR, dependencies[i]);
		}
	}

	/* Ask pkg-config rather than hardcoding paths: the same source has to
	 * compile on a machine where glib lives somewhere unexpected. */
	{
		g_autofree gchar *standard_output = NULL;
		g_autoptr(GError) local_error = NULL;
		gint exit_status = 0;

		if (g_spawn_command_line_sync(
			"pkg-config --cflags glib-2.0 gobject-2.0 gio-2.0 "
			"gmodule-2.0 json-glib-1.0 libsoup-3.0",
			&standard_output, NULL, &exit_status, &local_error))
		{
			if ((0 == exit_status) && (NULL != standard_output))
			{
				pkg_flags = g_strstrip(g_steal_pointer(&standard_output));
				g_string_append(flags, pkg_flags);
			}
		}
		else
		{
			g_debug("venture_crispy_host: pkg-config unavailable: %s",
			        local_error->message);
		}
	}

	/* A plugin resolves venture_* against the executable rather than
	 * linking a copy, so nothing is added to the link line here. */
	return g_string_free(g_steal_pointer(&flags), FALSE);
}

VentureCrispyHost *
venture_crispy_host_new(
	const gchar	 *cache_dir,
	GError		**error
){
	g_autoptr(VentureCrispyHost) self = NULL;
	g_autoptr(GError) local_error = NULL;
	CrispyGccCompiler *compiler;

	self = g_object_new(VENTURE_TYPE_CRISPY_HOST, NULL);

	compiler = crispy_gcc_compiler_new(&local_error);

	if (NULL == compiler)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
		            "No usable C compiler for crispy sources: %s",
		            (NULL != local_error) ? local_error->message
		                                  : "gcc not found");
		return NULL;
	}

	self->compiler = CRISPY_COMPILER(compiler);

	self->cache = (NULL != cache_dir)
		? CRISPY_CACHE_PROVIDER(crispy_file_cache_new_with_dir(cache_dir))
		: CRISPY_CACHE_PROVIDER(crispy_file_cache_new());

	self->default_flags = venture_crispy_host_build_flags();

	return g_steal_pointer(&self);
}

GModule *
venture_crispy_host_load(
	VentureCrispyHost	 *self,
	const gchar		 *source_path,
	const gchar		 *extra_flags,
	GError			**error
){
	g_autofree gchar *source = NULL;
	g_autofree gchar *hash = NULL;
	g_autofree gchar *object_path = NULL;
	g_autofree gchar *flags = NULL;
	g_autoptr(GError) local_error = NULL;
	GModule *module;
	gsize source_len = 0;

	g_return_val_if_fail(VENTURE_IS_CRISPY_HOST(self), NULL);
	g_return_val_if_fail(NULL != source_path, NULL);

	module = g_hash_table_lookup(self->modules, source_path);

	if (NULL != module)
		return module;

	if (!g_file_get_contents(source_path, &source, &source_len, &local_error))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
		            "Cannot read %s: %s", source_path, local_error->message);
		return NULL;
	}

	flags = (NULL != extra_flags)
		? g_strdup_printf("%s %s", self->default_flags, extra_flags)
		: g_strdup(self->default_flags);

	/* The cache key covers the source text, the flags and the compiler
	 * version, so an edit, a flag change or a toolchain upgrade all
	 * invalidate it and nothing else does. */
	hash = crispy_cache_provider_compute_hash(self->cache, source,
	                                          (gssize)source_len, flags,
	                                          crispy_compiler_get_version(self->compiler));
	object_path = crispy_cache_provider_get_path(self->cache, hash);

	if (!crispy_cache_provider_has_valid(self->cache, hash, source_path))
	{
		g_debug("venture_crispy_host: compiling %s", source_path);

		if (!crispy_compiler_compile_shared(self->compiler, source_path,
		                                    object_path, flags,
		                                    &local_error))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
			            "Cannot compile %s: %s", source_path,
			            local_error->message);
			return NULL;
		}
	}

	/* G_MODULE_BIND_LAZY keeps a symbol the source does not actually call
	 * from failing the load; BIND_LOCAL is deliberately not used, because
	 * the object must see the executable's exported symbols. */
	module = g_module_open(object_path, G_MODULE_BIND_LAZY);

	if (NULL == module)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
		            "Cannot load the compiled form of %s: %s",
		            source_path, g_module_error());
		return NULL;
	}

	/* Never unloaded: the object may have registered a GType, and pulling
	 * the code out from under a live type crashes on next use. */
	g_module_make_resident(module);
	g_hash_table_insert(self->modules, g_strdup(source_path), module);

	return module;
}

gboolean
venture_crispy_host_lookup(
	VentureCrispyHost	 *self,
	const gchar		 *source_path,
	const gchar		 *symbol_name,
	gpointer		 *out_symbol,
	const gchar		 *extra_flags,
	GError			**error
){
	GModule *module;

	g_return_val_if_fail(VENTURE_IS_CRISPY_HOST(self), FALSE);
	g_return_val_if_fail(NULL != symbol_name, FALSE);
	g_return_val_if_fail(NULL != out_symbol, FALSE);

	module = venture_crispy_host_load(self, source_path, extra_flags, error);

	if (NULL == module)
		return FALSE;

	if (!g_module_symbol(module, symbol_name, out_symbol))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
		            "%s defines no %s()", source_path, symbol_name);
		return FALSE;
	}

	if (NULL == *out_symbol)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
		            "%s declares %s() but it resolved to nothing",
		            source_path, symbol_name);
		return FALSE;
	}

	return TRUE;
}

const gchar *
venture_crispy_host_get_default_flags(VentureCrispyHost *self)
{
	g_return_val_if_fail(VENTURE_IS_CRISPY_HOST(self), NULL);

	return self->default_flags;
}
