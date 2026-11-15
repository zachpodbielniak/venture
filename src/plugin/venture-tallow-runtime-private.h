/*
 * venture-tallow-runtime-private.h - .tlw plugins on the sandboxed suet VM
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A .tlw plugin is Tallow source run by suet, Tallow's bytecode VM, inside
 * the server: no compiler at run time, and a sandbox around every plugin --
 * no files, network, processes or environment unless the operator grants
 * them (plugins.tallow_capabilities), an instruction budget per call and a
 * heap cap. Each plugin gets a VM of its own.
 *
 * Plugins and user formulas (src/formula) run on the VM; VENTURE's own
 * Tallow -- the kernels in src/kernels -- is compiled C, never
 * interpreted. A plugin or a formula reaches it through the
 * `venture_kernels` host module, whose functions are bound to that compiled
 * code (tools/venture-tallow-bindings.sh), never to an interpreted copy.
 * docs/tallow-plugins.org is the guide.
 */

#ifndef VENTURE_TALLOW_RUNTIME_PRIVATE_H
#define VENTURE_TALLOW_RUNTIME_PRIVATE_H

#include <setjmp.h>
#include <glib.h>
#include <tallow-suet.h>
#include <tallow-rt.h>

#include "venture.h"

G_BEGIN_DECLS

/**
 * VentureTallowHostEntry:
 * @symbol: the `extern fn` symbol a host module declares
 * @func: the C function bound to it
 *
 * One host function, as the generated kernel table lists them.
 */
typedef struct
{
	const gchar		*symbol;
	TallowSuetHostFunc	 func;
} VentureTallowHostEntry;

/**
 * VentureTallowGuard:
 * @jump: where a violated contract returns to
 * @violation: (nullable): the violated clause, or %NULL after a clean call
 * @old: the contract handler that was installed before the guarded call
 * @old_data: its data
 *
 * Turns a violated `requires` in a compiled kernel, called on behalf of a
 * plugin, into an error for that plugin instead of an abort of the server
 * (tlw_contract_set_handler()). A `requires` is checked before the kernel's
 * body runs, so leaving through it frees nothing the kernel allocated.
 */
typedef struct
{
	sigjmp_buf		 jump;
	gchar			*violation;
	TlwContractHandler	 old;
	gpointer		 old_data;
} VentureTallowGuard;

/**
 * venture_tallow_guard_violated: (skip)
 * @kind: the clause kind
 * @where: the clause's position
 * @text: the clause
 * @user_data: the #VentureTallowGuard of the call
 *
 * The contract handler VENTURE_TALLOW_GUARDED() installs; it records the
 * clause and jumps back to the guarded call.
 */
void
venture_tallow_guard_violated(
	const gchar	*kind,
	const gchar	*where,
	const gchar	*text,
	gpointer	 user_data
);

/**
 * VENTURE_TALLOW_GUARDED:
 * @guard: a #VentureTallowGuard local to the calling function
 * @call: the statement to run under the guard
 *
 * Runs @call with this thread's contract handler pointing at @guard, then
 * restores the previous one. Afterwards `@guard.violation` is %NULL when
 * the call ran to its end; otherwise it names the clause and the caller
 * owns it (venture_tallow_kernel_violation() frees it). The jump buffer
 * lives in the caller's frame, which is why this is a macro.
 */
#define VENTURE_TALLOW_GUARDED(guard, call) \
	do { \
		(guard).violation = NULL; \
		(guard).old = tlw_contract_get_handler(&(guard).old_data); \
		tlw_contract_set_handler(venture_tallow_guard_violated, &(guard)); \
		if (0 == sigsetjmp((guard).jump, 0)) { \
			call; \
		} \
		tlw_contract_set_handler((guard).old, (guard).old_data); \
	} while (0)

/**
 * venture_tallow_kernel_arity: (skip)
 * @name: the kernel's plugin-facing name
 * @expected: how many arguments it takes
 * @got: how many it was given
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: %FALSE, with @error saying so
 */
gboolean
venture_tallow_kernel_arity(
	const gchar	 *name,
	guint		  expected,
	guint		  got,
	GError		**error
);

/**
 * venture_tallow_kernel_violation: (skip)
 * @guard: the guard of a call whose contract was violated
 * @name: the kernel's plugin-facing name
 * @error: (out) (optional): return location for a #GError
 *
 * Frees the guard's violation text into @error.
 *
 * Returns: %FALSE
 */
gboolean
venture_tallow_kernel_violation(
	VentureTallowGuard	 *guard,
	const gchar		 *name,
	GError			**error
);

/**
 * venture_tallow_kernel_functions: (skip)
 *
 * The compiled kernels a plugin may call, generated from the `/\* @export *\/`
 * kernels by tools/venture-tallow-bindings.sh.
 *
 * Returns: (transfer none): a table ending in a %NULL symbol
 */
const VentureTallowHostEntry *
venture_tallow_kernel_functions(void);

/**
 * venture_tallow_kernel_module_source: (skip)
 *
 * Returns: (transfer none): the source of the `venture_kernels` host module
 */
const gchar *
venture_tallow_kernel_module_source(void);

/**
 * venture_tallow_host_module_source: (skip)
 *
 * Returns: (transfer none): the source of the `venture` host module
 *   (src/plugin/venture-tallow-host.tlw)
 */
const gchar *
venture_tallow_host_module_source(void);

/**
 * venture_tallow_std_dir:
 * @config: (nullable): the configuration, for `plugins.tallow_std_dir`
 *
 * Where the VM finds the Tallow standard library: the configured directory,
 * else `VENTURE_TALLOW_STD_DIR` from the environment, else the copy
 * installed with VENTURE, else the source tree's (a build-tree run).
 *
 * Returns: (transfer full): a directory
 */
gchar *
venture_tallow_std_dir(VentureConfig *config);

/**
 * venture_tallow_vm_new:
 * @config: (nullable): the configuration (limits, grants, the std dir)
 * @plugin: (nullable): the plugin's name, which selects its grants
 * @error: (out) (optional): return location for a #GError
 *
 * A sandboxed VM as a plugin gets it: the limits and grants from @config,
 * no GIR, and the `venture` and `venture_kernels` host modules with every
 * function they declare bound -- except the registration functions, which
 * the plugin runtime binds per plugin. Used by the runtime and the tests.
 *
 * Returns: (transfer full) (nullable): the VM, or %NULL with @error set
 */
TallowSuet *
venture_tallow_vm_new(
	VentureConfig	 *config,
	const gchar	 *plugin,
	GError		**error
);

/**
 * venture_plugin_runtime_tallow_new:
 *
 * The runtime for `.tlw` plugins: one sandboxed suet VM per plugin. A
 * plugin defines `plugin_register()` (called once, where it registers
 * automation handlers and fee models through the `venture` host module)
 * and optionally `plugin_info() -> own str`. Off when
 * `plugins.allow_tallow` is.
 *
 * Returns: (transfer full): a new runtime
 */
VenturePluginRuntime *
venture_plugin_runtime_tallow_new(void);

G_END_DECLS

#endif /* VENTURE_TALLOW_RUNTIME_PRIVATE_H */
