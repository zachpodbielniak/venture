/*
 * venture-crispy-host.h - Compile-on-demand C, via crispy
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Two features share one mechanism: the compiled C configuration and crispy
 * plugins. Both take a .c file, compile it to a shared object with
 * content-hash caching so the compile happens once, load it, and look up a
 * known symbol.
 *
 * The loaded object resolves venture_*, glib and every other symbol against
 * the running executable, which is linked --export-dynamic. That means a
 * crispy plugin has the full API available with no link line to get right,
 * and there is exactly one instance of every GType in the process.
 *
 * This is only present in the server build; libventure-core.a has no
 * compiler dependency.
 */

#ifndef VENTURE_CRISPY_HOST_H
#define VENTURE_CRISPY_HOST_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>
#include <gmodule.h>

G_BEGIN_DECLS

#define VENTURE_TYPE_CRISPY_HOST (venture_crispy_host_get_type())

G_DECLARE_FINAL_TYPE(VentureCrispyHost, venture_crispy_host,
                     VENTURE, CRISPY_HOST, GObject)

/**
 * venture_crispy_host_new:
 * @cache_dir: (nullable): where to keep compiled objects; %NULL uses
 *   crispy's own default cache location
 * @error: (out) (optional): return location for a #GError
 *
 * Creates a host, failing if no usable C compiler can be found.
 *
 * Returns: (transfer full) (nullable): a new host, or %NULL on error
 */
VentureCrispyHost *
venture_crispy_host_new(
	const gchar	 *cache_dir,
	GError		**error
);

/**
 * venture_crispy_host_load:
 * @self: a #VentureCrispyHost
 * @source_path: the C source file to compile
 * @extra_flags: (nullable): additional compiler flags
 * @error: (out) (optional): return location for a #GError
 *
 * Compiles @source_path to a shared object if the cache does not already
 * hold one for its exact contents, then opens it.
 *
 * The returned module is kept open for the lifetime of the host: unloading
 * it would pull the ground out from under any GType it registered.
 *
 * Returns: (transfer none) (nullable): the loaded module, or %NULL on error
 */
GModule *
venture_crispy_host_load(
	VentureCrispyHost	 *self,
	const gchar		 *source_path,
	const gchar		 *extra_flags,
	GError			**error
);

/**
 * venture_crispy_host_lookup:
 * @self: a #VentureCrispyHost
 * @source_path: the C source file
 * @symbol_name: the symbol to find
 * @out_symbol: (out): return location for the symbol address
 * @extra_flags: (nullable): additional compiler flags
 * @error: (out) (optional): return location for a #GError
 *
 * Compiles and loads @source_path, then resolves @symbol_name in it.
 *
 * Returns: %TRUE if the symbol was found
 */
gboolean
venture_crispy_host_lookup(
	VentureCrispyHost	 *self,
	const gchar		 *source_path,
	const gchar		 *symbol_name,
	gpointer		 *out_symbol,
	const gchar		 *extra_flags,
	GError			**error
);

/**
 * venture_crispy_host_get_default_flags:
 * @self: a #VentureCrispyHost
 *
 * Retrieves the compiler flags a VENTURE crispy source is built with: the
 * include path for the in-tree or installed headers plus the flags for
 * every library the API exposes.
 *
 * Returns: (transfer none): the flags
 */
const gchar *
venture_crispy_host_get_default_flags(VentureCrispyHost *self);

G_END_DECLS

#endif /* VENTURE_CRISPY_HOST_H */
